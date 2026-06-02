#include "nvenc-encoder.h"
#include <util/dstr.h>
#include <util/platform.h>

#ifdef ENABLE_NVENC

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 日志宏 */
#define encoder_log(level, enc, format, ...)                                   \
  blog(level, "[NVENC Encoder: '%s'] " format,                                 \
       obs_encoder_get_name(enc->encoder), ##__VA_ARGS__)

#include "sei-handler.h"
#if 0
/* NTP SEI 构建函数 (复用自 qsv-encoder.c) */
static bool nvenc_build_ntp_sei_payload(int64_t pts, ntp_timestamp_t *ntp_time,
                                        uint8_t **payload, size_t *size) {
  /* UUID: 与 QSV 编码器使用相同的 UUID */
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

static bool nvenc_build_sei_nal_unit(uint8_t *payload, size_t payload_size,
                                     int payload_type, uint8_t **nal_unit,
                                     size_t *nal_size) {
  /* 标准 H.264 SEI NAL 构建 */
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
 * Convert FFmpeg encoder extradata to a raw Annex-B byte stream so it can be
 * prepended inline at each keyframe. FFmpeg's h264_nvenc / hevc_nvenc both
 * emit extradata in Annex-B form (start codes) under AV_CODEC_FLAG_GLOBAL_HEADER,
 * which is the path this plugin uses. As a defensive fallback the H.264
 * AVCDecoderConfigurationRecord (AVCC) layout is also parsed; HEVC's HVCC is
 * not (it would return NULL → caller warning, harmless for the Annex-B case).
 * Returns a bmalloc'd buffer, or NULL on failure. *out_size is set to 0 on
 * failure or when the input format is unrecognised.
 */
static uint8_t *nvenc_extradata_to_annexb(const uint8_t *extradata,
                                          size_t extradata_size,
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
  if (extradata[0] != 0x01 || extradata_size < 7)
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

  /* Pass 1: validate ranges and tally output size. */
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
    *wp++ = 0; *wp++ = 0; *wp++ = 0; *wp++ = 1;
    memcpy(wp, extradata + scan, len);
    wp += len;
    scan += len;
  }
  scan++; /* skip num_pps */
  for (int i = 0; i < num_pps; i++) {
    uint16_t len = ((uint16_t)extradata[scan] << 8) | extradata[scan + 1];
    scan += 2;
    *wp++ = 0; *wp++ = 0; *wp++ = 0; *wp++ = 1;
    memcpy(wp, extradata + scan, len);
    wp += len;
    scan += len;
  }

  *out_size = total;
  return out;
}

/*
 * Translate the unified-encoder UI preset ("fast"/"balanced"/"quality") to a
 * modern NVENC SDK 10+ P1-P7 preset accepted by FFmpeg's h264_nvenc/hevc_nvenc/
 * av1_nvenc. The legacy presets (default/slow/medium/fast/hp/hq/bd/ll/llhq/llhp/
 * lossless/losslesshp) were deprecated by NVIDIA starting with the R550 driver,
 * so passing them through to FFmpeg fails with EINVAL on modern systems.
 * Already-valid P1-P7 strings pass through unchanged.
 */
static const char *nvenc_translate_preset(const char *in) {
  if (!in || !*in)
    return "p4";
  if (strcmp(in, "fast") == 0)
    return "p2";
  if (strcmp(in, "balanced") == 0)
    return "p4";
  if (strcmp(in, "quality") == 0)
    return "p7";
  if (in[0] == 'p' && in[1] >= '1' && in[1] <= '7' && in[2] == '\0')
    return in;
  return "p4";
}

/*
 * Resolve the codec-appropriate profile string for FFmpeg's *_nvenc encoders.
 * The unified-encoder UI exposes H.264 profile names (baseline/main/high) for
 * all codecs, but hevc_nvenc accepts only main/main10/rext and av1_nvenc only
 * main. Passing "high" to hevc_nvenc returns EINVAL from avcodec_open2 — so
 * we coerce unrecognised values to a sensible per-codec default.
 *
 * codec_type: 0 = H.264, 1 = H.265, 2 = AV1.
 * Returns NULL to mean "do not set the profile option".
 */
static const char *nvenc_resolve_profile(int codec_type, const char *in) {
  if (codec_type == 0) { /* h264_nvenc: baseline/main/high/high444p */
    if (!in || !*in)
      return "high";
    return in;
  }
  if (codec_type == 1) { /* hevc_nvenc: main/main10/rext */
    if (in && (!strcmp(in, "main") || !strcmp(in, "main10") ||
               !strcmp(in, "rext")))
      return in;
    return "main";
  }
  if (codec_type == 2) /* av1_nvenc: main */
    return "main";
  return NULL;
}

/* H.264/H.265 NAL类型定义 */
#define H264_NAL_SPS 7
#define H264_NAL_PPS 8
#define H265_NAL_VPS 32
#define H265_NAL_SPS 33
#define H265_NAL_PPS 34

/* 查找NAL单元起始码 */
static const uint8_t *find_nal_start_code_nvenc(const uint8_t *data,
                                                size_t size,
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

/* 查找参数集结束位置(SPS/PPS/VPS之后) */
static size_t find_parameter_sets_end_nvenc(const uint8_t *data, size_t size,
                                            int codec_type) {
  const uint8_t *current = data;
  size_t remaining = size;
  size_t last_param_end = 0;

  while (remaining > 0) {
    size_t sc_size = 0;
    const uint8_t *nal_start =
        find_nal_start_code_nvenc(current, remaining, &sc_size);

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
      // SPS=7, PPS=8, AUD=9
      is_param_set = (nal_type == H264_NAL_SPS || nal_type == H264_NAL_PPS ||
                      nal_type == 9);
    } else if (codec_type == 1) { // H.265
      nal_type = (nal_data[0] >> 1) & 0x3F;
      // VPS=32, SPS=33, PPS=34, AUD=35
      is_param_set = (nal_type == H265_NAL_VPS || nal_type == H265_NAL_SPS ||
                      nal_type == H265_NAL_PPS || nal_type == 35);
    } else {
      // AV1不使用NAL结构
      return 0;
    }

    // 查找下一个起始码
    size_t next_sc_size = 0;
    const uint8_t *next_nal =
        find_nal_start_code_nvenc(nal_data, nal_remaining, &next_sc_size);

    if (is_param_set) {
      // 这是参数集，记录结束位置
      if (next_nal) {
        last_param_end = next_nal - data;
      } else {
        last_param_end = size;
      }
    } else {
      // 遇到非参数集NAL，返回上一个参数集的结束位置
      return last_param_end;
    }

    if (!next_nal)
      break;

    current = next_nal;
    remaining = size - (current - data);
  }

  return last_param_end;
}

/* 销毁编码器 */
void nvenc_encoder_destroy(nvenc_encoder_t *enc) {
  if (!enc)
    return;

  encoder_log(LOG_INFO, enc, "Destroying NVENC encoder");

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

/* 创建编码器 - Internal (public for unified encoder) */
void *nvenc_encoder_create_internal(obs_data_t *settings,
                                    obs_encoder_t *encoder) {
  nvenc_encoder_t *enc = bzalloc(sizeof(nvenc_encoder_t));
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

  /* 根据 codec_type 设置编码器名称 */
  switch (enc->codec_type) {
  case 0: // H.264
    snprintf(enc->codec_name, sizeof(enc->codec_name), "h264_nvenc");
    break;
  case 1: // H.265
    snprintf(enc->codec_name, sizeof(enc->codec_name), "hevc_nvenc");
    break;
  case 2: // AV1
    snprintf(enc->codec_name, sizeof(enc->codec_name), "av1_nvenc");
    break;
  default:
    snprintf(enc->codec_name, sizeof(enc->codec_name), "h264_nvenc");
    break;
  }

  /* NTP 初始化 */
  const char *ntp_server = obs_data_get_string(settings, "ntp_server");
  ntp_client_init(&enc->ntp_client, ntp_server, 123);
  enc->ntp_enabled = true;
  enc->ntp_sync_interval_ms =
      (uint32_t)obs_data_get_int(settings, "ntp_sync_interval");
  if (enc->ntp_sync_interval_ms == 0)
    enc->ntp_sync_interval_ms = 60000; // 默认 60 秒

  encoder_log(LOG_INFO, enc, "Creating NVENC encoder:  %s", enc->codec_name);

  /* 查找 FFmpeg NVENC 编码器 */
  enc->codec = avcodec_find_encoder_by_name(enc->codec_name);
  if (!enc->codec) {
    encoder_log(LOG_ERROR, enc, "NVENC encoder not found (%s)",
                enc->codec_name);
    encoder_log(LOG_ERROR, enc,
                "Make sure FFmpeg is built with NVENC support and NVIDIA GPU "
                "drivers are installed");
    nvenc_encoder_destroy(enc);
    return NULL;
  }

  enc->codec_context = avcodec_alloc_context3(enc->codec);
  if (!enc->codec_context) {
    encoder_log(LOG_ERROR, enc, "Failed to allocate codec context");
    nvenc_encoder_destroy(enc);
    return NULL;
  }

  /* 配置编码参数 */
  enc->codec_context->width = enc->width;
  enc->codec_context->height = enc->height;
  enc->codec_context->time_base = (AVRational){voi->fps_den, voi->fps_num};
  enc->codec_context->framerate = (AVRational){voi->fps_num, voi->fps_den};
  enc->codec_context->pix_fmt = AV_PIX_FMT_NV12;
  enc->codec_context->bit_rate = enc->bitrate * 1000;
  enc->codec_context->gop_size = enc->keyint;
  enc->codec_context->max_b_frames = enc->bframes;

  /* Enable GLOBAL_HEADER for H.264 and H.265 so FFmpeg populates extradata at
   * avcodec_open2() with a codec sequence header. Without it:
   *   - H.264: OBS's RTMP/FLV muxer has no AVCDecoderConfigurationRecord
   *     → ingest servers drop the stream within ~100ms.
   *   - H.265: OBS's file/MPEG-TS muxer has no HEVCDecoderConfigurationRecord
   *     → recording hangs at stop and SLS/SRT distribution fails (frames only
   *     reach P2P listeners that happened to catch an in-band header).
   * We re-inject SPS/PPS (and VPS for HEVC) inline at each keyframe below,
   * preserving the v1.2.2 in-band-parameter-set behaviour for MPEG-TS/SRT
   * receivers that join mid-stream. AV1 is left untouched. */
  if (enc->codec_type == 0 || enc->codec_type == 1) {
    enc->codec_context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
  }

  /* NVENC 特定选项 */
  AVDictionary *opts = NULL;

  /* Preset (translate to NVENC SDK 10+ P1-P7 — legacy presets are deprecated
   * by the R550 driver and cause EINVAL on modern systems) */
  const char *nvenc_preset = nvenc_translate_preset(enc->preset);
  av_dict_set(&opts, "preset", nvenc_preset, 0);
  encoder_log(LOG_INFO, enc, "Using NVENC preset: %s (requested: %s)",
              nvenc_preset, (enc->preset && *enc->preset) ? enc->preset : "(default)");

  /* Profile (coerce to codec-valid value — the UI exposes H.264 names for all
   * codecs, but hevc_nvenc/av1_nvenc reject "high" with EINVAL). */
  const char *nvenc_profile = nvenc_resolve_profile(enc->codec_type, enc->profile);
  if (nvenc_profile) {
    av_dict_set(&opts, "profile", nvenc_profile, 0);
    encoder_log(LOG_INFO, enc, "Using NVENC profile: %s (requested: %s)",
                nvenc_profile,
                (enc->profile && *enc->profile) ? enc->profile : "(default)");
  }

  /* Rate control - CBR */
  av_dict_set(&opts, "rc", "cbr", 0);

  /* 打开编码器 */
  char errbuf[128];
  int ret = avcodec_open2(enc->codec_context, enc->codec, &opts);
  if (ret < 0) {
    av_strerror(ret, errbuf, sizeof(errbuf));
    encoder_log(LOG_ERROR, enc, "Failed to open NVENC encoder: %s (%d)", errbuf,
                ret);
    if (opts)
      av_dict_free(&opts);
    nvenc_encoder_destroy(enc);
    return NULL;
  }
  if (opts)
    av_dict_free(&opts);

  /* 分配 Frame 和 Packet */
  enc->frame = av_frame_alloc();
  enc->packet = av_packet_alloc();

  /* 提取 Extra Data */
  if (enc->codec_context->extradata_size > 0) {
    enc->extra_data_size = enc->codec_context->extradata_size;
    enc->extra_data = bmalloc(enc->extra_data_size);
    memcpy(enc->extra_data, enc->codec_context->extradata,
           enc->extra_data_size);
    encoder_log(LOG_INFO, enc, "Extra data size: %zu bytes",
                enc->extra_data_size);

    /* For H.264 / H.265 with GLOBAL_HEADER on, build a reusable Annex-B
     * parameter-set payload (SPS/PPS, plus VPS for HEVC) so we can keep
     * parameter sets inline at every keyframe — required by MPEG-TS / SRT
     * receivers (and SLS servers) that join mid-stream. */
    if (enc->codec_type == 0 || enc->codec_type == 1) {
      enc->inline_params = nvenc_extradata_to_annexb(
          enc->extra_data, enc->extra_data_size, &enc->inline_params_size);
      if (enc->inline_params && enc->inline_params_size > 0) {
        encoder_log(LOG_INFO, enc,
                    "Inline parameter set payload built: %zu bytes (Annex-B)",
                    enc->inline_params_size);
      } else {
        encoder_log(LOG_WARNING, enc,
                    "Could not build inline parameter sets from extradata; "
                    "mid-stream MPEG-TS/SRT joiners may need to wait for the "
                    "next out-of-band header");
      }
    }
  } else if (enc->codec_type == 0 || enc->codec_type == 1) {
    encoder_log(LOG_WARNING, enc,
                "Extradata is empty after open — recording / RTMP will likely "
                "fail (no codec sequence header in container)");
  }

  encoder_log(LOG_INFO, enc,
              "NVENC encoder created successfully (%dx%d @ %d kbps)",
              enc->width, enc->height, enc->bitrate);

  return enc;
}

/* 编码函数 - Internal (public for unified encoder) */
bool nvenc_encoder_encode_internal(void *data, struct encoder_frame *frame,
                                   struct encoder_packet *packet,
                                   bool *received_packet) {
  nvenc_encoder_t *enc = data;
  char errbuf[128];

  if (!frame || !packet || !received_packet)
    return false;

  /* 清理上一帧 */
  av_frame_unref(enc->frame);

  /* 设置 Frame 参数 */
  enc->frame->format = enc->codec_context->pix_fmt;
  enc->frame->width = enc->codec_context->width;
  enc->frame->height = enc->codec_context->height;
  enc->frame->pts = frame->pts;

  /* 复制 NV12 数据 */
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

  /* 发送 Frame */
  int ret = avcodec_send_frame(enc->codec_context, enc->frame);
  av_frame_unref(enc->frame);

  if (ret < 0) {
    av_strerror(ret, errbuf, sizeof(errbuf));
    encoder_log(LOG_ERROR, enc, "Error sending frame: %s (%d)", errbuf, ret);
    return false;
  }

  /* 接收 Packet */
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

  /* NTP 时间更新 */
  uint64_t now = os_gettime_ns();
  uint64_t sync_interval_ns = (uint64_t)enc->ntp_sync_interval_ms * 1000000ULL;
  if (enc->last_ntp_sync_time == 0 ||
      (now - enc->last_ntp_sync_time) > sync_interval_ns) {
    /* Always update last_sync_time to avoid retry storm on failure */
    enc->last_ntp_sync_time = now;
    ntp_client_sync(&enc->ntp_client);
  }
  ntp_client_get_time(&enc->ntp_client, &enc->current_ntp_time);

  /* SEI 插入 (关键帧) */
  bool keyframe = (enc->packet->flags & AV_PKT_FLAG_KEY) != 0;
  uint8_t *sei_nal = NULL;
  size_t sei_nal_size = 0;

  if (keyframe) {
    uint8_t *payload = NULL;
    size_t payload_size = 0;
    if (build_ntp_sei_payload(frame->pts, &enc->current_ntp_time,
                                    &payload, &payload_size)) {
      sei_nal_type_t nal_type = (enc->codec_type == 1) ? SEI_NAL_H265_PREFIX : SEI_NAL_H264;
      build_sei_nal_unit(payload, payload_size, nal_type, &sei_nal,
                               &sei_nal_size);
      bfree(payload);

      encoder_log(LOG_DEBUG, enc,
                  "[NVENC] Inserted SEI: PTS=%lld NTP=%u.%u Size=%zu",
                  frame->pts, enc->current_ntp_time.seconds,
                  enc->current_ntp_time.fraction, sei_nal_size);
    }
  }

  /* 组装Packet with correct SEI insertion position */
  size_t total_size = enc->packet->size + sei_nal_size;
  if (enc->packet_buffer_size < total_size) {
    bfree(enc->packet_buffer);
    enc->packet_buffer = bmalloc(total_size);
    enc->packet_buffer_size = total_size;
  }

  if (sei_nal && keyframe) {
    /* 查找参数集结束位置 */
    size_t param_sets_end = find_parameter_sets_end_nvenc(
        enc->packet->data, enc->packet->size, enc->codec_type);

    if (param_sets_end > 0 && param_sets_end < enc->packet->size) {
      /* FFmpeg emitted SPS/PPS inline (e.g. H.265 path, or H.264 without
       * GLOBAL_HEADER). Order: existing parameter sets → SEI → IDR slice. */
      memcpy(enc->packet_buffer, enc->packet->data, param_sets_end);
      size_t offset = param_sets_end;
      memcpy(enc->packet_buffer + offset, sei_nal, sei_nal_size);
      offset += sei_nal_size;
      size_t remaining = enc->packet->size - param_sets_end;
      memcpy(enc->packet_buffer + offset, enc->packet->data + param_sets_end,
             remaining);
      encoder_log(LOG_DEBUG, enc,
                  "SEI inserted after parameter sets (offset: %zu)",
                  param_sets_end);
    } else if (enc->inline_params && enc->inline_params_size > 0) {
      /* GLOBAL_HEADER path (H.264): FFmpeg dropped SPS/PPS from the packet;
       * re-inject them inline so MPEG-TS/SRT receivers can still decode.
       * Order: SPS/PPS (Annex-B from extradata) → SEI → IDR slice. */
      total_size =
          enc->inline_params_size + sei_nal_size + enc->packet->size;
      if (enc->packet_buffer_size < total_size) {
        bfree(enc->packet_buffer);
        enc->packet_buffer = bmalloc(total_size);
        enc->packet_buffer_size = total_size;
      }
      memcpy(enc->packet_buffer, enc->inline_params, enc->inline_params_size);
      size_t offset = enc->inline_params_size;
      memcpy(enc->packet_buffer + offset, sei_nal, sei_nal_size);
      offset += sei_nal_size;
      memcpy(enc->packet_buffer + offset, enc->packet->data, enc->packet->size);
      encoder_log(LOG_DEBUG, enc,
                  "Inline SPS/PPS+SEI prepended to keyframe (%zu + %zu bytes)",
                  enc->inline_params_size, sei_nal_size);
    } else {
      /* Last-resort fallback: just put SEI before the IDR slice. */
      encoder_log(LOG_WARNING, enc,
                  "Could not find parameter sets end, inserting SEI at "
                  "beginning (may cause decoding issues)");
      memcpy(enc->packet_buffer, sei_nal, sei_nal_size);
      memcpy(enc->packet_buffer + sei_nal_size, enc->packet->data,
             enc->packet->size);
    }

    bfree(sei_nal);
  } else {
    /* 非关键帧或无SEI */
    memcpy(enc->packet_buffer, enc->packet->data, enc->packet->size);
  }

  packet->data = enc->packet_buffer;
  packet->size = total_size;
  packet->type = OBS_ENCODER_VIDEO;
  packet->pts = enc->packet->pts;
  packet->dts = enc->packet->dts;
  packet->keyframe = keyframe;

  av_packet_unref(enc->packet);
  return true;
}

/* 默认设置 */
static void nvenc_get_defaults(obs_data_t *settings) {
  obs_data_set_default_int(settings, "bitrate", 2500);
  obs_data_set_default_int(settings, "keyint_sec", 2);
  obs_data_set_default_int(settings, "bframes", 2);
  obs_data_set_default_string(settings, "preset", "p4");
  obs_data_set_default_string(settings, "profile", "high");
  obs_data_set_default_string(settings, "ntp_server", "time.windows.com");
  obs_data_set_default_int(settings, "ntp_sync_interval", 60000); // 60 秒
}

/* 属性 */
static obs_properties_t *nvenc_properties(void *unused) {
  obs_properties_t *props = obs_properties_create();

  obs_properties_add_int(props, "bitrate", "Bitrate (kbps)", 50, 50000, 50);
  obs_properties_add_int(props, "keyint_sec", "Keyframe Interval (s)", 1, 10,
                         1);
  obs_properties_add_int(props, "bframes", "B-Frames", 0, 4, 1);

  obs_property_t *list = obs_properties_add_list(
      props, "preset", "Preset", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
  obs_property_list_add_string(list, "P1 (Fastest)", "p1");
  obs_property_list_add_string(list, "P2", "p2");
  obs_property_list_add_string(list, "P3", "p3");
  obs_property_list_add_string(list, "P4 (Default)", "p4");
  obs_property_list_add_string(list, "P5", "p5");
  obs_property_list_add_string(list, "P6", "p6");
  obs_property_list_add_string(list, "P7 (Slowest)", "p7");

  obs_properties_add_text(props, "profile", "Profile", OBS_TEXT_DEFAULT);
  obs_properties_add_text(props, "ntp_server", "NTP Server", OBS_TEXT_DEFAULT);
  obs_properties_add_int(props, "ntp_sync_interval", "NTP Sync Interval (ms)",
                         1000, 600000, 1000); // 1秒 到 10分钟

  return props;
}

/* 获取编码器名称 */
static const char *nvenc_get_name(void *type_data) {
  return "SEI Stamper (NVIDIA NVENC)";
}

/* 获取视频信息 - Internal (public for unified encoder) */
void nvenc_encoder_get_video_info_internal(void *data,
                                           struct video_scale_info *info) {
  info->format = VIDEO_FORMAT_NV12;
}

/* 获取 Extra Data - Internal (public for unified encoder) */
bool nvenc_encoder_get_extra_data_internal(void *data, uint8_t **extra_data,
                                           size_t *size) {
  nvenc_encoder_t *enc = (nvenc_encoder_t *)data;
  if (!enc || !enc->extra_data)
    return false;
  *extra_data = enc->extra_data;
  *size = enc->extra_data_size;
  return true;
}

/* Static wrappers for obs_encoder_info */
static void *nvenc_create(obs_data_t *settings, obs_encoder_t *encoder) {
  return nvenc_encoder_create_internal(settings, encoder);
}

static bool nvenc_encode(void *data, struct encoder_frame *frame,
                         struct encoder_packet *packet, bool *received_packet) {
  return nvenc_encoder_encode_internal(data, frame, packet, received_packet);
}

static void nvenc_get_video_info(void *data, struct video_scale_info *info) {
  nvenc_encoder_get_video_info_internal(data, info);
}

static bool nvenc_get_extra_data(void *data, uint8_t **extra_data,
                                 size_t *size) {
  return nvenc_encoder_get_extra_data_internal(data, extra_data, size);
}

/* 编码器 Info 结构体 */
struct obs_encoder_info nvenc_encoder_info = {
    .id = "h264_nvenc_native",
    .type = OBS_ENCODER_VIDEO,
    .codec = "h264",
    .get_name = nvenc_get_name,
    .create = nvenc_create,
    .destroy = (void (*)(void *))nvenc_encoder_destroy,
    .encode = nvenc_encode,
    .get_defaults = nvenc_get_defaults,
    .get_properties = nvenc_properties,
    .get_video_info = nvenc_get_video_info,
    .get_extra_data = nvenc_get_extra_data,
};

#else

/* Dummy implementation if NVENC not enabled */
#include <obs-module.h>

struct obs_encoder_info nvenc_encoder_info = {0};

#endif
