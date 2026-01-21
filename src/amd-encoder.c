#include "amd-encoder.h"
#include <util/dstr.h>
#include <util/platform.h>

#ifdef ENABLE_AMD

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 日志宏 */
#define encoder_log(level, enc, format, ...)                                   \
  blog(level, "[AMD Encoder: '%s'] " format,                                   \
       obs_encoder_get_name(enc->encoder), ##__VA_ARGS__)

/* NTP SEI 构建函数 (复用自 nvenc-encoder.c) */
static bool amd_build_ntp_sei_payload(int64_t pts, ntp_timestamp_t *ntp_time,
                                      uint8_t **payload, size_t *size) {
  /* UUID: 与其他编码器使用相同的 UUID */
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

/* 销毁编码器 */
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

  /* 根据 codec_type 设置编码器名称 */
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

  /* NTP 初始化 */
  const char *ntp_server = obs_data_get_string(settings, "ntp_server");
  ntp_client_init(&enc->ntp_client, ntp_server, 123);
  enc->ntp_enabled = true;
  enc->ntp_sync_interval_ms =
      (uint32_t)obs_data_get_int(settings, "ntp_sync_interval");
  if (enc->ntp_sync_interval_ms == 0)
    enc->ntp_sync_interval_ms = 60000; // 默认 60 秒

  encoder_log(LOG_INFO, enc, "Creating AMD AMF encoder: %s", enc->codec_name);

  /* 查找 FFmpeg AMF 编码器 */
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

  /* 配置编码参数 */
  enc->codec_context->width = enc->width;
  enc->codec_context->height = enc->height;
  enc->codec_context->time_base = (AVRational){voi->fps_den, voi->fps_num};
  enc->codec_context->framerate = (AVRational){voi->fps_num, voi->fps_den};
  enc->codec_context->pix_fmt = AV_PIX_FMT_NV12;
  enc->codec_context->bit_rate = enc->bitrate * 1000;
  enc->codec_context->gop_size = enc->keyint;
  enc->codec_context->max_b_frames = enc->bframes;
  enc->codec_context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

  /* AMD AMF 特定选项 */
  AVDictionary *opts = NULL;

  /* Quality preset */
  if (enc->preset && strlen(enc->preset) > 0) {
    av_dict_set(&opts, "quality", enc->preset, 0);
    encoder_log(LOG_INFO, enc, "Using quality preset: %s", enc->preset);
  }

  /* Profile */
  if (enc->profile && strlen(enc->profile) > 0) {
    av_dict_set(&opts, "profile", enc->profile, 0);
  }

  /* Rate control - CBR */
  av_dict_set(&opts, "rc", "cbr", 0);

  /* 打开编码器 */
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
  }

  encoder_log(LOG_INFO, enc,
              "AMD AMF encoder created successfully (%dx%d @ %d kbps)",
              enc->width, enc->height, enc->bitrate);

  return enc;
}

/* 编码函数 - Internal (public for unified encoder) */
bool amd_encoder_encode_internal(void *data, struct encoder_frame *frame,
                                 struct encoder_packet *packet,
                                 bool *received_packet) {
  amd_encoder_t *enc = data;
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
    if (amd_build_ntp_sei_payload(frame->pts, &enc->current_ntp_time, &payload,
                                  &payload_size)) {
      amd_build_sei_nal_unit(payload, payload_size, 6, &sei_nal, &sei_nal_size);
      bfree(payload);

      encoder_log(LOG_DEBUG, enc,
                  "[AMD] Inserted SEI: PTS=%lld NTP=%u.%u Size=%zu", frame->pts,
                  enc->current_ntp_time.seconds, enc->current_ntp_time.fraction,
                  sei_nal_size);
    }
  }

  /* 组装 Packet */
  size_t total_size = enc->packet->size + sei_nal_size;
  if (enc->packet_buffer_size < total_size) {
    bfree(enc->packet_buffer);
    enc->packet_buffer = bmalloc(total_size);
    enc->packet_buffer_size = total_size;
  }

  size_t offset = 0;
  if (sei_nal) {
    memcpy(enc->packet_buffer, sei_nal, sei_nal_size);
    offset += sei_nal_size;
    bfree(sei_nal);
  }
  memcpy(enc->packet_buffer + offset, enc->packet->data, enc->packet->size);

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
static void amd_get_defaults(obs_data_t *settings) {
  obs_data_set_default_int(settings, "bitrate", 2500);
  obs_data_set_default_int(settings, "keyint_sec", 2);
  obs_data_set_default_int(settings, "bframes", 2);
  obs_data_set_default_string(settings, "preset", "balanced");
  obs_data_set_default_string(settings, "profile", "high");
  obs_data_set_default_string(settings, "ntp_server", "time.windows.com");
  obs_data_set_default_int(settings, "ntp_sync_interval", 60000); // 60 秒
}

/* 属性 */
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
  obs_properties_add_int(props, "ntp_sync_interval", "NTP Sync Interval (ms)",
                         1000, 600000, 1000); // 1秒 到 10分钟

  return props;
}

/* 获取编码器名称 */
static const char *amd_get_name(void *type_data) {
  return "SEI Stamper (AMD AMF)";
}

/* 获取视频信息 - Internal (public for unified encoder) */
void amd_encoder_get_video_info_internal(void *data,
                                         struct video_scale_info *info) {
  info->format = VIDEO_FORMAT_NV12;
}

/* 获取 Extra Data - Internal (public for unified encoder) */
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

/* 编码器 Info 结构体 */
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
