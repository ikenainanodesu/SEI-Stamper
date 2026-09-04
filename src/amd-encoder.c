#include "amd-encoder.h"
#include <util/dstr.h>
#include <util/platform.h>

#ifdef ENABLE_AMD

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Logging macros */
#define encoder_log(level, enc, format, ...)                                   \
  blog(level, "[AMD Encoder: '%s'] " format,                                   \
       obs_encoder_get_name(enc->encoder), ##__VA_ARGS__)

#include "sei-handler.h"
#if 0
/* NTP SEI build helper (mirrors nvenc-encoder.c) */
static bool amd_build_ntp_sei_payload(int64_t pts, ntp_timestamp_t *ntp_time,
                                      uint8_t **payload, size_t *size) {
  /* UUID: the same one the other encoders use */
  const uint8_t uuid[16] = {0xa5, 0xb3, 0xc2, 0xd1, 0xe4, 0xf5, 0x67, 0x89,
                            0xab, 0xcd, 0xef, 0x01, 0x23, 0x45, 0x67, 0x89};

  *size = 16 + 8; // UUID + 64bit NTP
  *payload = bmalloc(*size);
  if (!*payload)
    return false;

  memcpy(*payload, uuid, 16);

  /* Big Endian NTP Timestamp */
  uint32_t ntp_sec = ntp_time->seconds;
  uint32_t ntp_frac = ntp_time->fraction;

  uint8_t *data = *payload + 16;
  data[0] = (ntp_sec >> 24) & 0xFF;
  data[1] = (ntp_sec >> 16) & 0xFF;
  data[2] = (ntp_sec >> 8) & 0xFF;
  data[3] = (ntp_sec) & 0xFF;
  data[4] = (ntp_frac >> 24) & 0xFF;
  data[5] = (ntp_frac >> 16) & 0xFF;
  data[6] = (ntp_frac >> 8) & 0xFF;
  data[7] = (ntp_frac) & 0xFF;

  return true;
}

static bool amd_build_sei_nal_unit(uint8_t *payload, size_t payload_size,
                                   int payload_type, uint8_t **nal_unit,
                                   size_t *nal_size) {
  /* Standard H.264 SEI NAL construction */
  size_t size_bytes = 1;
  if (payload_size >= 255)
    size_bytes += (payload_size / 255);

  size_t total_size = 4 + 1 + 1 + size_bytes + payload_size + 1;
  *nal_unit = bmalloc(total_size);
  if (!*nal_unit)
    return false;

  uint8_t *p = *nal_unit;
  // Start Code
  *p++ = 0x00;
  *p++ = 0x00;
  *p++ = 0x00;
  *p++ = 0x01;
  // NAL Header (SEI=6)
  *p++ = 0x06;

  // Payload Type (User Data Unregistered = 5)
  *p++ = 0x05;

  // Payload Size
  size_t s = payload_size;
  while (s >= 255) {
    *p++ = 0xFF;
    s -= 255;
  }
  *p++ = (uint8_t)s;

  // Payload
  memcpy(p, payload, payload_size);
  p += payload_size;

  // Trailing bits
  *p++ = 0x80;

  *nal_size = (p - *nal_unit);
  return true;
}
#endif

/*
 * h264_amf/hevc_amf hand out Annex-B extradata; the AVCC branch is a
 * defensive fallback, gated to H.264 because HEVC's HVCC also begins with
 * configurationVersion 0x01 and would be mis-parsed as AVCC garbage.
 */
static uint8_t *amd_extradata_to_annexb(const uint8_t *extradata,
                                        size_t extradata_size, int codec_type,
                                        size_t *out_size) {
  *out_size = 0;
  if (!extradata || extradata_size < 4)
    return NULL;

  /* Annex-B already? Either 0x00 0x00 0x00 0x01 or 0x00 0x00 0x01. */
  bool is_annexb =
      (extradata[0] == 0 && extradata[1] == 0 &&
       ((extradata[2] == 0 && extradata[3] == 1) || extradata[2] == 1));
  if (is_annexb) {
    uint8_t *out = bmalloc(extradata_size);
    memcpy(out, extradata, extradata_size);
    *out_size = extradata_size;
    return out;
  }

  /* AVCDecoderConfigurationRecord: configurationVersion must be 1. */
  if (codec_type != 0 || extradata[0] != 0x01 || extradata_size < 7)
    return NULL;

  /* Layout:
   *   [0]      configurationVersion (=1)
   *   [1..3]   profile/compat/level
   *   [4]      6 reserved bits | 2 lengthSizeMinusOne bits
   *   [5]      3 reserved bits | 5 numOfSequenceParameterSets bits
   *   [6..]    array of {2-byte length BE, SPS NAL bytes}
   *            then 1 byte numOfPictureParameterSets
   *            then array of {2-byte length BE, PPS NAL bytes}
   */
  size_t pos = 5;
  int num_sps = extradata[pos++] & 0x1F;
  size_t total = 0;

  /* Pass 1: validate ranges and tally the output size. */
  size_t scan = pos;
  for (int i = 0; i < num_sps; i++) {
    if (scan + 2 > extradata_size)
      return NULL;
    uint16_t len = ((uint16_t)extradata[scan] << 8) | extradata[scan + 1];
    scan += 2;
    if (scan + len > extradata_size)
      return NULL;
    total += 4 + len; /* 4-byte start code + NAL */
    scan += len;
  }
  if (scan + 1 > extradata_size)
    return NULL;
  int num_pps = extradata[scan++];
  for (int i = 0; i < num_pps; i++) {
    if (scan + 2 > extradata_size)
      return NULL;
    uint16_t len = ((uint16_t)extradata[scan] << 8) | extradata[scan + 1];
    scan += 2;
    if (scan + len > extradata_size)
      return NULL;
    total += 4 + len;
    scan += len;
  }
  if (total == 0)
    return NULL;

  /* Pass 2: emit Annex-B. */
  uint8_t *out = bmalloc(total);
  uint8_t *wp = out;
  scan = pos;
  for (int i = 0; i < num_sps; i++) {
    uint16_t len = ((uint16_t)extradata[scan] << 8) | extradata[scan + 1];
    scan += 2;
    *wp++ = 0;
    *wp++ = 0;
    *wp++ = 0;
    *wp++ = 1;
    memcpy(wp, extradata + scan, len);
    wp += len;
    scan += len;
  }
  scan++; /* skip num_pps */
  for (int i = 0; i < num_pps; i++) {
    uint16_t len = ((uint16_t)extradata[scan] << 8) | extradata[scan + 1];
    scan += 2;
    *wp++ = 0;
    *wp++ = 0;
    *wp++ = 0;
    *wp++ = 1;
    memcpy(wp, extradata + scan, len);
    wp += len;
    scan += len;
  }

  *out_size = total;
  return out;
}

/* H.264/H.265 NAL type definitions */
#define H264_NAL_SPS 7
#define H264_NAL_PPS 8
#define H265_NAL_VPS 32
#define H265_NAL_SPS 33
#define H265_NAL_PPS 34

/* Find a NAL unit start code */
static const uint8_t *find_nal_start_code_amd(const uint8_t *data, size_t size,
                                              size_t *start_code_size) {
  if (size < 3)
    return NULL;

  for (size_t i = 0; i < size - 2; i++) {
    if (data[i] == 0 && data[i + 1] == 0) {
      if (data[i + 2] == 1) {
        *start_code_size = 3;
        return data + i;
      } else if (i < size - 3 && data[i + 2] == 0 && data[i + 3] == 1) {
        *start_code_size = 4;
        return data + i;
      }
    }
  }
  return NULL;
}

/* Find the end of the leading AUD/parameter-set run and report whether an
 * actual SPS is in it — an AUD alone must not count as "params in-band". */
static size_t find_parameter_sets_end_amd(const uint8_t *data, size_t size,
                                          int codec_type, bool *sps_seen) {
  const uint8_t *current = data;
  size_t remaining = size;
  size_t last_param_end = 0;

  *sps_seen = false;

  while (remaining > 0) {
    size_t sc_size = 0;
    const uint8_t *nal_start =
        find_nal_start_code_amd(current, remaining, &sc_size);

    if (!nal_start)
      break;

    const uint8_t *nal_data = nal_start + sc_size;
    size_t nal_remaining = remaining - (nal_data - current);

    if (nal_remaining < 1)
      break;

    uint8_t nal_type;
    bool is_param_set = false;

    if (codec_type == 0) { // H.264
      nal_type = nal_data[0] & 0x1F;
      is_param_set = (nal_type == H264_NAL_SPS || nal_type == H264_NAL_PPS ||
                      nal_type == 9);
      if (nal_type == H264_NAL_SPS)
        *sps_seen = true;
    } else if (codec_type == 1) { // H.265
      nal_type = (nal_data[0] >> 1) & 0x3F;
      is_param_set = (nal_type == H265_NAL_VPS || nal_type == H265_NAL_SPS ||
                      nal_type == H265_NAL_PPS || nal_type == 35);
      if (nal_type == H265_NAL_SPS)
        *sps_seen = true;
    } else {
      return 0;
    }

    size_t next_sc_size = 0;
    const uint8_t *next_nal =
        find_nal_start_code_amd(nal_data, nal_remaining, &next_sc_size);

    if (is_param_set) {
      if (next_nal) {
        last_param_end = next_nal - data;
      } else {
        last_param_end = size;
      }
    } else {
      return last_param_end;
    }

    if (!next_nal)
      break;

    current = next_nal;
    remaining = size - (current - data);
  }

  return last_param_end;
}

/* Destroy the encoder */
void amd_encoder_destroy(amd_encoder_t *enc) {
  if (!enc)
    return;

  encoder_log(LOG_INFO, enc, "Destroying AMD encoder");

  if (enc->codec_context) {
    avcodec_free_context(&enc->codec_context);
  }
  if (enc->frame) {
    av_frame_free(&enc->frame);
  }
  if (enc->packet) {
    av_packet_free(&enc->packet);
  }

  if (enc->extra_data)
    bfree(enc->extra_data);
  if (enc->inline_params)
    bfree(enc->inline_params);
  if (enc->profile)
    bfree(enc->profile);
  if (enc->preset)
    bfree(enc->preset);
  if (enc->packet_buffer)
    bfree(enc->packet_buffer);

  ntp_client_destroy(&enc->ntp_client);
  bfree(enc);
}

/*
 * Coerce the UI's H.264 profile names to a codec-valid value: hevc_amf and
 * av1_amf reject "high" with EINVAL, so H.265 never opens.
 * NULL means "leave the profile option unset".
 */
static const char *amd_resolve_profile(int codec_type, const char *in) {
  if (codec_type == 0) { /* h264_amf: baseline/main/high/constrained_* */
    if (!in || !*in)
      return "high";
    return in;
  }
  if (codec_type == 1) /* hevc_amf: main only */
    return "main";
  if (codec_type == 2) /* av1_amf: main only */
    return "main";
  return NULL;
}

/* Create - internal (public for the unified encoder) */
void *amd_encoder_create_internal(obs_data_t *settings,
                                  obs_encoder_t *encoder) {
  amd_encoder_t *enc = bzalloc(sizeof(amd_encoder_t));
  enc->encoder = encoder;

  video_t *video = obs_encoder_video(encoder);
  const struct video_output_info *voi = video_output_get_info(video);

  enc->width = voi->width;
  enc->height = voi->height;
  enc->fps_num = voi->fps_num;
  enc->fps_den = voi->fps_den;
  enc->bitrate = (int)obs_data_get_int(settings, "bitrate");
  enc->keyint = (int)obs_data_get_int(settings, "keyint_sec") * enc->fps_num /
                enc->fps_den;
  enc->bframes = (int)obs_data_get_int(settings, "bframes");
  enc->preset = bstrdup(obs_data_get_string(settings, "preset"));
  enc->profile = bstrdup(obs_data_get_string(settings, "profile"));

  /* Codec Type */
  enc->codec_type = (int)obs_data_get_int(settings, "codec_type");
  if (enc->codec_type < 0 || enc->codec_type > 2)
    enc->codec_type = 0; // Default to H.264

  /* Pick the encoder name from codec_type */
  switch (enc->codec_type) {
  case 0: // H.264
    snprintf(enc->codec_name, sizeof(enc->codec_name), "h264_amf");
    break;
  case 1: // H.265
    snprintf(enc->codec_name, sizeof(enc->codec_name), "hevc_amf");
    break;
  case 2: // AV1
    snprintf(enc->codec_name, sizeof(enc->codec_name), "av1_amf");
    break;
  default:
    snprintf(enc->codec_name, sizeof(enc->codec_name), "h264_amf");
    break;
  }

  /* NTP initialisation */
  const char *ntp_server = obs_data_get_string(settings, "ntp_server");
  uint16_t ntp_port = (uint16_t)obs_data_get_int(settings, "ntp_port");
  if (ntp_port == 0) ntp_port = 123;
  ntp_client_init(&enc->ntp_client, ntp_server, ntp_port);
  enc->ntp_enabled = true;
  enc->ntp_sync_interval_ms =
      (uint32_t)obs_data_get_int(settings, "ntp_sync_interval_ms");
  if (enc->ntp_sync_interval_ms == 0)
    enc->ntp_sync_interval_ms = 60000; // default 60 s
  ntp_client_start_background_sync(&enc->ntp_client, enc->ntp_sync_interval_ms);

  encoder_log(LOG_INFO, enc, "Creating AMD AMF encoder: %s", enc->codec_name);

  /* Find the FFmpeg AMF encoder */
  enc->codec = avcodec_find_encoder_by_name(enc->codec_name);
  if (!enc->codec) {
    encoder_log(LOG_ERROR, enc, "AMD AMF encoder not found (%s)",
                enc->codec_name);
    encoder_log(LOG_ERROR, enc,
                "Make sure FFmpeg is built with AMF support and AMD GPU "
                "drivers are installed");
    amd_encoder_destroy(enc);
    return NULL;
  }

  enc->codec_context = avcodec_alloc_context3(enc->codec);
  if (!enc->codec_context) {
    encoder_log(LOG_ERROR, enc, "Failed to allocate codec context");
    amd_encoder_destroy(enc);
    return NULL;
  }

  /* Configure encoding parameters */
  enc->codec_context->width = enc->width;
  enc->codec_context->height = enc->height;
  enc->codec_context->time_base = (AVRational){voi->fps_den, voi->fps_num};
  enc->codec_context->framerate = (AVRational){voi->fps_num, voi->fps_den};
  enc->codec_context->pix_fmt = AV_PIX_FMT_NV12;
  enc->codec_context->bit_rate = enc->bitrate * 1000;
  enc->codec_context->gop_size = enc->keyint;
  enc->codec_context->max_b_frames = enc->bframes;
  /* No-op for amfenc (it never reads this flag: extradata is always
   * populated, and in-band header emission follows AMF driver defaults) —
   * kept so extradata survives if avcodec ever gates it on GLOBAL_HEADER
   * the way nvenc does. */
  if (enc->codec_type == 0 || enc->codec_type == 1)
    enc->codec_context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

  /* AMD AMF-specific options */
  AVDictionary *opts = NULL;

  /* AMF's quality option expects speed/balanced/quality, but the unified UI
   * sends fast; map it to speed. */
  const char *amf_quality = enc->preset;
  if (enc->preset && strcmp(enc->preset, "fast") == 0)
    amf_quality = "speed";
  if (amf_quality && strlen(amf_quality) > 0) {
    av_dict_set(&opts, "quality", amf_quality, 0);
    encoder_log(LOG_INFO, enc, "Using quality preset: %s (requested %s)",
                amf_quality, enc->preset);
  }

  /* Profile */
  const char *amf_profile = amd_resolve_profile(enc->codec_type, enc->profile);
  if (amf_profile) {
    av_dict_set(&opts, "profile", amf_profile, 0);
    encoder_log(LOG_INFO, enc, "Using AMF profile: %s (requested: %s)",
                amf_profile,
                (enc->profile && *enc->profile) ? enc->profile : "(default)");
  }

  /* Rate control - CBR */
  av_dict_set(&opts, "rc", "cbr", 0);

  /* Open the encoder */
  char errbuf[128];
  int ret = avcodec_open2(enc->codec_context, enc->codec, &opts);
  if (ret < 0) {
    av_strerror(ret, errbuf, sizeof(errbuf));
    encoder_log(LOG_ERROR, enc, "Failed to open AMD AMF encoder: %s (%d)",
                errbuf, ret);
    if (opts)
      av_dict_free(&opts);
    amd_encoder_destroy(enc);
    return NULL;
  }
  if (opts)
    av_dict_free(&opts);

  /* Allocate the frame and packet */
  enc->frame = av_frame_alloc();
  enc->packet = av_packet_alloc();

  /* Extract extra data */
  if (enc->codec_context->extradata_size > 0) {
    enc->extra_data_size = enc->codec_context->extradata_size;
    enc->extra_data = bmalloc(enc->extra_data_size);
    memcpy(enc->extra_data, enc->codec_context->extradata,
           enc->extra_data_size);
    encoder_log(LOG_INFO, enc, "Extra data size: %zu bytes",
                enc->extra_data_size);

    if (enc->codec_type == 0 || enc->codec_type == 1) {
      enc->inline_params = amd_extradata_to_annexb(
          enc->extra_data, enc->extra_data_size, enc->codec_type,
          &enc->inline_params_size);
      if (enc->inline_params && enc->inline_params_size > 0) {
        encoder_log(LOG_INFO, enc,
                    "Inline parameter set payload built: %zu bytes (Annex-B)",
                    enc->inline_params_size);
      } else {
        encoder_log(LOG_WARNING, enc,
                    "Could not build inline parameter sets from extradata; "
                    "mid-stream MPEG-TS/SRT joiners may fail to decode");
      }
    }
  } else if (enc->codec_type == 0 || enc->codec_type == 1) {
    encoder_log(LOG_WARNING, enc,
                "Extradata is empty after open - recording / RTMP will likely "
                "fail (no codec sequence header in container)");
  }

  encoder_log(LOG_INFO, enc,
              "AMD AMF encoder created successfully (%dx%d @ %d kbps)",
              enc->width, enc->height, enc->bitrate);

  return enc;
}

/* Encode - internal (public for the unified encoder) */
bool amd_encoder_encode_internal(void *data, struct encoder_frame *frame,
                                 struct encoder_packet *packet,
                                 bool *received_packet) {
  amd_encoder_t *enc = data;
  char errbuf[128];

  if (!frame || !packet || !received_packet)
    return false;

  /* Clean up the previous frame */
  av_frame_unref(enc->frame);

  /* Set the frame parameters */
  enc->frame->format = enc->codec_context->pix_fmt;
  enc->frame->width = enc->codec_context->width;
  enc->frame->height = enc->codec_context->height;
  enc->frame->pts = frame->pts;

  /* Copy the NV12 data */
  if (enc->codec_context->pix_fmt == AV_PIX_FMT_NV12) {
    enc->frame->linesize[0] = frame->linesize[0];
    enc->frame->linesize[1] = frame->linesize[1];
    enc->frame->data[0] = frame->data[0];
    enc->frame->data[1] = frame->data[1];
  } else {
    encoder_log(LOG_ERROR, enc, "Unsupported pixel format: %d",
                enc->codec_context->pix_fmt);
    return false;
  }

  /* Send the frame */
  int ret = avcodec_send_frame(enc->codec_context, enc->frame);
  av_frame_unref(enc->frame);

  if (ret < 0) {
    av_strerror(ret, errbuf, sizeof(errbuf));
    encoder_log(LOG_ERROR, enc, "Error sending frame: %s (%d)", errbuf, ret);
    return false;
  }

  /* Receive the packet */
  ret = avcodec_receive_packet(enc->codec_context, enc->packet);
  if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
    *received_packet = false;
    return true;
  } else if (ret < 0) {
    av_strerror(ret, errbuf, sizeof(errbuf));
    encoder_log(LOG_ERROR, enc, "Error receiving packet: %s (%d)", errbuf, ret);
    return false;
  }

  *received_packet = true;

  /* SEI insertion (keyframes only — IVS player is hot-loop sensitive to
   * per-frame SEI) */
  bool keyframe = (enc->packet->flags & AV_PKT_FLAG_KEY) != 0;
  uint8_t *sei_nal = NULL;
  size_t sei_nal_size = 0;

  if (keyframe) {
    /* NTP is refreshed on a background thread; just read the latest here. */
    if (ntp_client_get_time(&enc->ntp_client, &enc->current_ntp_time)) {
      uint8_t *payload = NULL;
      size_t payload_size = 0;
      if (build_ntp_sei_payload(frame->pts, &enc->current_ntp_time, &payload,
                                &payload_size)) {
        sei_nal_type_t nal_type = (enc->codec_type == 1) ? SEI_NAL_H265_PREFIX : SEI_NAL_H264;
        build_sei_nal_unit(payload, payload_size, nal_type, &sei_nal, &sei_nal_size);
        bfree(payload);

        encoder_log(LOG_DEBUG, enc,
                    "[AMD] SEI stamped: PTS=%lld NTP_sec=%u (0x%08X) NTP_frac=%u keyframe=%d",
                    frame->pts,
                    enc->current_ntp_time.seconds, enc->current_ntp_time.seconds,
                    enc->current_ntp_time.fraction, keyframe);
      }
    } else {
      encoder_log(LOG_WARNING, enc,
                  "[AMD] Keyframe at PTS=%lld but NTP not synced, "
                  "skipping SEI insertion", frame->pts);
    }
  }

  /* Assemble the packet: [leading AUD/params] -> [injected params] -> SEI ->
   * slice data */
  size_t param_sets_end = 0;
  bool sps_inband = false;
  if (keyframe)
    param_sets_end = find_parameter_sets_end_amd(
        enc->packet->data, enc->packet->size, enc->codec_type, &sps_inband);

  /* Not gated on the SEI: a keyframe with no NTP to stamp still has to be
   * decodable by a mid-stream MPEG-TS/SRT joiner. */
  size_t params_prefix = 0;
  if (keyframe && !sps_inband)
    params_prefix = enc->inline_params_size;

  size_t total_size = params_prefix + sei_nal_size + enc->packet->size;
  if (enc->packet_buffer_size < total_size) {
    bfree(enc->packet_buffer);
    enc->packet_buffer = bmalloc(total_size);
    enc->packet_buffer_size = total_size;
  }

  /* Any leading AUD/param run stays first — the AUD must open the AU. */
  size_t offset = 0;
  size_t body_start = param_sets_end;
  if (param_sets_end > 0) {
    memcpy(enc->packet_buffer, enc->packet->data, param_sets_end);
    offset = param_sets_end;
  }
  if (params_prefix > 0) {
    memcpy(enc->packet_buffer + offset, enc->inline_params, params_prefix);
    offset += params_prefix;
  } else if (keyframe && !sps_inband &&
             (enc->codec_type == 0 || enc->codec_type == 1)) {
    encoder_log(LOG_DEBUG, enc,
                "Keyframe carries no parameter sets (extradata unusable)");
  }

  if (sei_nal) {
    memcpy(enc->packet_buffer + offset, sei_nal, sei_nal_size);
    offset += sei_nal_size;
    bfree(sei_nal);
  }

  memcpy(enc->packet_buffer + offset, enc->packet->data + body_start,
         enc->packet->size - body_start);

  packet->data = enc->packet_buffer;
  packet->size = total_size;
  packet->type = OBS_ENCODER_VIDEO;
  packet->pts = enc->packet->pts;
  packet->dts = enc->packet->dts;
  packet->keyframe = keyframe;

  av_packet_unref(enc->packet);
  return true;
}

/* Defaults */
static void amd_get_defaults(obs_data_t *settings) {
  obs_data_set_default_int(settings, "bitrate", 2500);
  obs_data_set_default_int(settings, "keyint_sec", 2);
  obs_data_set_default_int(settings, "bframes", 2);
  obs_data_set_default_string(settings, "preset", "balanced");
  obs_data_set_default_string(settings, "profile", "high");
  obs_data_set_default_string(settings, "ntp_server", "time.windows.com");
  obs_data_set_default_int(settings, "ntp_sync_interval_ms", 60000); // 60 s
}

/* Properties */
static obs_properties_t *amd_properties(void *unused) {
  obs_properties_t *props = obs_properties_create();

  obs_properties_add_int(props, "bitrate", "Bitrate (kbps)", 50, 50000, 50);
  obs_properties_add_int(props, "keyint_sec", "Keyframe Interval (s)", 1, 10,
                         1);
  obs_properties_add_int(props, "bframes", "B-Frames", 0, 4, 1);

  obs_property_t *list =
      obs_properties_add_list(props, "preset", "Quality Preset",
                              OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
  obs_property_list_add_string(list, "Speed", "speed");
  obs_property_list_add_string(list, "Balanced (Default)", "balanced");
  obs_property_list_add_string(list, "Quality", "quality");

  obs_properties_add_text(props, "profile", "Profile", OBS_TEXT_DEFAULT);
  obs_properties_add_text(props, "ntp_server", "NTP Server", OBS_TEXT_DEFAULT);
  obs_properties_add_int(props, "ntp_sync_interval_ms", "NTP Sync Interval (ms)",
                         1000, 600000, 1000); // 1 s to 10 min

  return props;
}

/* Encoder name */
static const char *amd_get_name(void *type_data) {
  return "SEI Stamper (AMD AMF)";
}

/* Video info - internal (public for the unified encoder) */
void amd_encoder_get_video_info_internal(void *data,
                                         struct video_scale_info *info) {
  info->format = VIDEO_FORMAT_NV12;
}

/* Extra data - internal (public for the unified encoder) */
bool amd_encoder_get_extra_data_internal(void *data, uint8_t **extra_data,
                                         size_t *size) {
  amd_encoder_t *enc = (amd_encoder_t *)data;
  if (!enc || !enc->extra_data)
    return false;
  *extra_data = enc->extra_data;
  *size = enc->extra_data_size;
  return true;
}

/* Static wrappers for obs_encoder_info */
static void *amd_create(obs_data_t *settings, obs_encoder_t *encoder) {
  return amd_encoder_create_internal(settings, encoder);
}

static bool amd_encode(void *data, struct encoder_frame *frame,
                       struct encoder_packet *packet, bool *received_packet) {
  return amd_encoder_encode_internal(data, frame, packet, received_packet);
}

static void amd_get_video_info(void *data, struct video_scale_info *info) {
  amd_encoder_get_video_info_internal(data, info);
}

static bool amd_get_extra_data(void *data, uint8_t **extra_data, size_t *size) {
  return amd_encoder_get_extra_data_internal(data, extra_data, size);
}

/* Encoder info structs */
struct obs_encoder_info amd_encoder_info = {
    .id = "h264_amf_native",
    .type = OBS_ENCODER_VIDEO,
    .codec = "h264",
    .get_name = amd_get_name,
    .create = amd_create,
    .destroy = (void (*)(void *))amd_encoder_destroy,
    .encode = amd_encode,
    .get_defaults = amd_get_defaults,
    .get_properties = amd_properties,
    .get_video_info = amd_get_video_info,
    .get_extra_data = amd_get_extra_data,
};

#else

/* Dummy implementation if AMD not enabled */
#include <obs-module.h>

struct obs_encoder_info amd_encoder_info = {0};

#endif
