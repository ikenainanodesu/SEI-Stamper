/******************************************************************************
    SEI Stamper Encoder - FFmpeg Implementation
    Copyright (C) 2026

    Wraps FFmpeg encoders (x264, NVENC, AMF, QSV) and inserts NTP timestamps.
******************************************************************************/

#include "sei-stamper-encoder.h"
#include <obs-avc.h>
#include <util/bmem.h>
#include <util/dstr.h>
#include <util/platform.h>

/* 日志宏 */
#define encoder_log(level, enc, format, ...)                                   \
  blog(level, "[SEI Stamper: '%s'] " format,                                   \
       obs_encoder_get_name(enc->context), ##__VA_ARGS__)

/* 编码器名称 */
static const char *h264_encoder_getname(void *unused) {
  UNUSED_PARAMETER(unused);
  return "SEI Stamper (H.264)";
}

static const char *h265_encoder_getname(void *unused) {
  UNUSED_PARAMETER(unused);
  return "SEI Stamper (H.265/HEVC)";
}

static const char *av1_encoder_getname(void *unused) {
  UNUSED_PARAMETER(unused);
  return "SEI Stamper (AV1)";
}

/* 销毁编码器 */
static void sei_stamper_encoder_destroy(void *data) {
  struct sei_stamper_encoder *enc = data;
  if (!enc)
    return;

  encoder_log(LOG_INFO, enc, "Destroying encoder");

  if (enc->codec_context) {
    avcodec_free_context(&enc->codec_context);
  }
  if (enc->frame) {
    av_frame_free(&enc->frame);
  }
  if (enc->packet) {
    av_packet_free(&enc->packet);
  }

  if (enc->ntp_enabled) {
    ntp_client_destroy(&enc->ntp_client);
  }

  bfree(enc->merged_sei_buffer);
  bfree(enc->packet_buffer);
  bfree(enc->preset);
  bfree(enc->profile);
  bfree(enc->rate_control);
  bfree(enc);
}

/* 创建编码器 */
static void *sei_stamper_encoder_create(obs_data_t *settings,
                                        obs_encoder_t *encoder,
                                        enum sei_stamper_codec_type type) {
  struct sei_stamper_encoder *enc = bzalloc(sizeof(struct sei_stamper_encoder));
  enc->context = encoder;
  enc->codec_type = type;

  /* 获取用户设置 */
  const char *codec_name = obs_data_get_string(settings, "codec_name");
  enc->bitrate = (int)obs_data_get_int(settings, "bitrate");
  enc->keyint_sec = (int)obs_data_get_int(settings, "keyint_sec");
  enc->bframes = (int)obs_data_get_int(settings, "bframes");
  enc->preset = bstrdup(obs_data_get_string(settings, "preset"));
  enc->profile = bstrdup(obs_data_get_string(settings, "profile"));
  enc->rate_control = bstrdup(obs_data_get_string(settings, "rate_control"));

  if (!codec_name || strlen(codec_name) == 0) {
    if (type == SEI_STAMPER_CODEC_H264)
      codec_name = "libx264";
    else if (type == SEI_STAMPER_CODEC_H265)
      codec_name = "hevc_nvenc"; // default fallback
    else
      codec_name = "libaom-av1";
  }

  encoder_log(LOG_INFO, enc, "Creating encoder: %s", codec_name);

  /* DIAGNOSTIC: List all available H.264 encoders if first run (or debug) */
  const AVCodec *c = NULL;
  void *i = NULL;
  encoder_log(LOG_DEBUG, enc, "Available H.264 Encoders in FFmpeg:");
  while ((c = av_codec_iterate(&i))) {
    if (av_codec_is_encoder(c) && c->id == AV_CODEC_ID_H264) {
      encoder_log(LOG_DEBUG, enc, "  - %s (%s)", c->name, c->long_name);
    }
  }

  /* 查找FFmpeg编码器 */
  const AVCodec *codec = avcodec_find_encoder_by_name(codec_name);
  if (!codec) {
    encoder_log(LOG_ERROR, enc, "Encoder not found: %s", codec_name);
    sei_stamper_encoder_destroy(enc);
    return NULL;
  }
  enc->codec = codec;

  enc->codec_context = avcodec_alloc_context3(enc->codec);
  if (!enc->codec_context) {
    encoder_log(LOG_ERROR, enc, "Failed to allocate codec context");
    sei_stamper_encoder_destroy(enc);
    return NULL;
  }

  /* 获取OBS视频参数 */
  video_t *video = obs_encoder_video(encoder);
  const struct video_output_info *voi = video_output_get_info(video);

  enc->codec_context->width = (int)obs_encoder_get_width(encoder);
  enc->codec_context->height = (int)obs_encoder_get_height(encoder);
  enc->codec_context->time_base = (AVRational){voi->fps_den, voi->fps_num};
  enc->codec_context->framerate = (AVRational){voi->fps_num, voi->fps_den};
  enc->codec_context->pix_fmt = AV_PIX_FMT_NV12; /* Default to NV12 for now */

  /* 检查像素格式支持 */
  if (enc->codec->pix_fmts) {
    const enum AVPixelFormat *p = enc->codec->pix_fmts;
    bool nv12_supported = false;

    encoder_log(LOG_INFO, enc, "Checking supported pixel formats:");
    while (*p != AV_PIX_FMT_NONE) {
      encoder_log(LOG_INFO, enc, "  - %d", *p);
      if (*p == AV_PIX_FMT_NV12)
        nv12_supported = true;
      p++;
    }

    if (nv12_supported) {
      enc->codec_context->pix_fmt = AV_PIX_FMT_NV12;
    } else {
      enc->codec_context->pix_fmt =
          enc->codec->pix_fmts[0]; /* Use first supported */
      encoder_log(LOG_WARNING, enc, "NV12 not supported, using format %d",
                  enc->codec_context->pix_fmt);
    }
  } else {
    /* Some encoders don't list fmts, assume NV12 or YUV420P */
    enc->codec_context->pix_fmt = AV_PIX_FMT_NV12;
    encoder_log(LOG_WARNING, enc, "Encoder didn't list formats, assuming NV12");
  }

  /* 设置编码参数 */
  enc->codec_context->bit_rate = enc->bitrate * 1000;
  enc->codec_context->gop_size = enc->keyint_sec * voi->fps_num / voi->fps_den;

  /* MF handles B-frames internally or strictly. Avoid setting it manually to
   * avoid EINVAL. */
  if (enc->bframes > 0 && !strstr(codec_name, "mf")) {
    enc->codec_context->max_b_frames = enc->bframes;
    enc->codec_context->has_b_frames = enc->bframes;
  }

  /* Flags */
  enc->codec_context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

  /* 打开编码器 opts */
  AVDictionary *opts = NULL;

  /* Preset Mapping */
  const char *pkt_preset = enc->preset;

  // Clean up preset for HW encoders
  if (strstr(codec_name, "nvenc") || strstr(codec_name, "amf") ||
      strstr(codec_name, "qsv") || strstr(codec_name, "mf")) {
    // Map x264 presets to generic ffmpeg/HW ones if possible, or just ignore
    // invalid ones
    if (_strnicmp(pkt_preset, "veryfast", 8) == 0)
      pkt_preset = "fast"; // NVENC supports 'fast'
    else if (_strnicmp(pkt_preset, "ultrafast", 9) == 0)
      pkt_preset = "fast";
    else if (_strnicmp(pkt_preset, "superfast", 9) == 0)
      pkt_preset = "fast";
    else if (_strnicmp(pkt_preset, "slow", 4) == 0)
      pkt_preset = "slow"; // NVENC supports 'slow'

    // AMF might need 'speed', 'quality', 'balanced'
    if (strstr(codec_name, "amf")) {
      if (strstr(pkt_preset, "fast"))
        pkt_preset = "speed";
      else if (strstr(pkt_preset, "slow"))
        pkt_preset = "quality";
      else
        pkt_preset = "balanced";
    }

    /* MF (Media Foundation) is strict. */
    if (strstr(codec_name, "mf")) {
      pkt_preset = NULL;
    }
  }

  if (pkt_preset && strlen(pkt_preset) > 0) {
    av_dict_set(&opts, "preset", pkt_preset, 0);
    encoder_log(LOG_INFO, enc, "Using preset: %s (mapped from %s)", pkt_preset,
                enc->preset);
  } else if (enc->preset && strlen(enc->preset) > 0 &&
             !strstr(codec_name, "mf")) {
    av_dict_set(&opts, "preset", enc->preset, 0);
  }

  if (enc->profile && strlen(enc->profile) > 0 && !strstr(codec_name, "mf"))
    av_dict_set(&opts, "profile", enc->profile, 0);

  /* CBR handling (simplified) */
  if (enc->rate_control && _strnicmp(enc->rate_control, "CBR", 3) == 0) {
    enc->codec_context->rc_min_rate = enc->codec_context->bit_rate;
    enc->codec_context->rc_max_rate = enc->codec_context->bit_rate;
    enc->codec_context->rc_buffer_size =
        enc->codec_context->bit_rate; /* 1s buffer */
  }

  /* 打印错误信息 (Helper) */
  char errbuf[128];
  int ret;

  if ((ret = avcodec_open2(enc->codec_context, enc->codec, &opts)) < 0) {
    av_strerror(ret, errbuf, sizeof(errbuf));
    encoder_log(LOG_ERROR, enc, "Failed to open FFmpeg encoder: %s (%d)",
                errbuf, ret);
    if (opts)
      av_dict_free(&opts);
    sei_stamper_encoder_destroy(enc);
    return NULL;
  }
  if (opts)
    av_dict_free(&opts);

  /* 分配Frame和Packet */
  enc->frame = av_frame_alloc();
  enc->packet = av_packet_alloc();

  /* 初始化NTP */
  const char *ntp_server = obs_data_get_string(settings, "ntp_server");
  int ntp_port = (int)obs_data_get_int(settings, "ntp_port");
  enc->ntp_enabled = obs_data_get_bool(settings, "ntp_enabled");

  if (enc->ntp_enabled) {
    if (ntp_client_init(&enc->ntp_client, ntp_server, (uint16_t)ntp_port)) {
      ntp_client_sync(&enc->ntp_client);
      encoder_log(LOG_INFO, enc, "NTP Initialized: %s:%d", ntp_server,
                  ntp_port);
    } else {
      enc->ntp_enabled = false;
    }
  }

  encoder_log(LOG_INFO, enc, "Encoder created successfully (%dx%d @ %d kbps)",
              enc->codec_context->width, enc->codec_context->height,
              enc->bitrate);

  return enc;
}

static void *h264_encoder_create(obs_data_t *settings, obs_encoder_t *encoder) {
  return sei_stamper_encoder_create(settings, encoder, SEI_STAMPER_CODEC_H264);
}

static void *h265_encoder_create(obs_data_t *settings, obs_encoder_t *encoder) {
  return sei_stamper_encoder_create(settings, encoder, SEI_STAMPER_CODEC_H265);
}

static void *av1_encoder_create(obs_data_t *settings, obs_encoder_t *encoder) {
  return sei_stamper_encoder_create(settings, encoder, SEI_STAMPER_CODEC_AV1);
}

/* 编码函数 */
static bool sei_stamper_encoder_encode(void *data, struct encoder_frame *frame,
                                       struct encoder_packet *packet,
                                       bool *received_packet) {
  struct sei_stamper_encoder *enc = data;
  char errbuf[128];

  if (!frame || !packet || !received_packet)
    return false;

  /* 清理上一帧的状态（如果有）- 重要：防止引用泄露，也防止脏数据 */
  av_frame_unref(enc->frame);

  /* 手动关联OBS的数据到FFmpeg Frame
     注意：我们不拥有数据，所以不要创建AVBufferRef (buf[i]保持NULL)
     这样av_frame_unref不会释放frame->data指向的内存 */

  enc->frame->format = enc->codec_context->pix_fmt;
  enc->frame->width = enc->codec_context->width;
  enc->frame->height = enc->codec_context->height;
  enc->frame->pts = frame->pts;

  /* 只有NV12支持目前 */
  if (enc->codec_context->pix_fmt == AV_PIX_FMT_NV12) {
    enc->frame->linesize[0] = frame->linesize[0];
    enc->frame->linesize[1] = frame->linesize[1];
    enc->frame->data[0] = frame->data[0];
    enc->frame->data[1] = frame->data[1];
  } else {
    /* Try generic copy if linesizes match, but safest is assume NV12 or copy
     * plane by plane */
    encoder_log(LOG_ERROR, enc, "Unsupported pixel format in encode: %d",
                enc->codec_context->pix_fmt);
    return false;
  }

  /* 发送Frame给编码器 */
  int ret = avcodec_send_frame(enc->codec_context, enc->frame);

  /* 发送后立即解除引用，不仅是为了清理，也是为了断开与OBS数据的关联
     FFmpeg如果内部需要保留数据（异步编码），它在send_frame时已经做了深拷贝（因为buf为NULL）
  */
  av_frame_unref(enc->frame);

  if (ret < 0) {
    av_strerror(ret, errbuf, sizeof(errbuf));
    encoder_log(LOG_ERROR, enc, "Error sending frame to encoder: %s (%d)",
                errbuf, ret);
    return false;
  }

  /* 接收Packets */
  ret = avcodec_receive_packet(enc->codec_context, enc->packet);
  if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
    *received_packet = false;
    return true;
  } else if (ret < 0) {
    av_strerror(ret, errbuf, sizeof(errbuf));
    encoder_log(LOG_ERROR, enc, "Error receiving packet from encoder: %s (%d)",
                errbuf, ret);
    return false;
  }

  *received_packet = true;

  /* 更新NTP时间 */
  if (enc->ntp_enabled) {
    uint64_t now = os_gettime_ns();
    if (enc->last_ntp_sync_time == 0 ||
        (now - enc->last_ntp_sync_time) > 60000000000ULL) { // 1 min sync
      if (ntp_client_sync(&enc->ntp_client))
        enc->last_ntp_sync_time = now;
    }
    ntp_client_get_time(&enc->ntp_client, &enc->current_ntp_time);
  }

  /* 处理SEI插入 (仅对关键帧插入时间戳) */
  bool has_sei = false;
  uint8_t *sei_nal = NULL;
  size_t sei_nal_size = 0;

  if (enc->ntp_enabled && (enc->packet->flags & AV_PKT_FLAG_KEY)) {
    uint8_t *payload = NULL;
    size_t payload_size = 0;
    if (build_ntp_sei_payload(frame->pts, &enc->current_ntp_time, &payload,
                              &payload_size)) {
      /* 根据编码器类型选择SEI NAL类型 */
      sei_nal_type_t nal_type = SEI_NAL_H264;
      if (enc->codec_type == SEI_STAMPER_CODEC_H265) {
        nal_type = SEI_NAL_H265_PREFIX;
      }
      /* AV1使用不同的SEI机制，暂时跳过 */
      if (enc->codec_type != SEI_STAMPER_CODEC_AV1) {
        if (build_sei_nal_unit(payload, payload_size, nal_type, &sei_nal,
                               &sei_nal_size)) {
          has_sei = true;
        }
      }
      bfree(payload);
    }
  }

  /* 组装最终Packet数据 */
  size_t total_size = enc->packet->size + (has_sei ? sei_nal_size : 0);

  if (enc->packet_buffer_size < total_size) {
    bfree(enc->packet_buffer);
    enc->packet_buffer = bmalloc(total_size);
    enc->packet_buffer_size = total_size;
  }

  size_t offset = 0;

  if (has_sei) {
    memcpy(enc->packet_buffer + offset, sei_nal, sei_nal_size);
    offset += sei_nal_size;
    bfree(sei_nal);
  }

  memcpy(enc->packet_buffer + offset, enc->packet->data, enc->packet->size);

  packet->data = enc->packet_buffer;
  packet->size = total_size;
  packet->type = OBS_ENCODER_VIDEO;
  packet->pts = enc->packet->pts;
  packet->dts = enc->packet->dts;
  packet->keyframe = (enc->packet->flags & AV_PKT_FLAG_KEY) != 0;

  av_packet_unref(enc->packet);
  return true;
}

static bool sei_stamper_encoder_extra_data(void *data, uint8_t **extra_data,
                                           size_t *size) {
  struct sei_stamper_encoder *enc = data;
  if (!enc->codec_context->extradata_size) {
    *extra_data = NULL;
    *size = 0;
    return false;
  }

  *extra_data = enc->codec_context->extradata;
  *size = enc->codec_context->extradata_size;
  return true;
}

static void sei_stamper_encoder_defaults(obs_data_t *settings) {
  obs_data_set_default_string(settings, "codec_name", "libx264");
  obs_data_set_default_int(settings, "bitrate", 2500);
  obs_data_set_default_int(settings, "keyint_sec", 2);
  obs_data_set_default_int(settings, "bframes", 2);
  obs_data_set_default_string(settings, "preset", "veryfast");
  obs_data_set_default_string(settings, "profile", "high");
  obs_data_set_default_string(settings, "rate_control", "CBR");

  obs_data_set_default_bool(settings, "ntp_enabled", true);
  obs_data_set_default_string(settings, "ntp_server", "time.windows.com");
  obs_data_set_default_int(settings, "ntp_port", 123);
}

static obs_properties_t *sei_stamper_encoder_properties(void *unused) {
  UNUSED_PARAMETER(unused);
  obs_properties_t *props = obs_properties_create();

  /* 核心设置 */
  obs_property_t *list =
      obs_properties_add_list(props, "codec_name", "Encoder",
                              OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
  obs_property_list_add_string(list, "Software (x264)", "libx264");
  obs_property_list_add_string(list, "NVIDIA NVENC H.264", "h264_nvenc");
  obs_property_list_add_string(list, "AMD AMF H.264", "h264_amf");
  obs_property_list_add_string(list, "Intel QuickSync H.264", "h264_qsv");
  obs_property_list_add_string(list, "Windows Media Foundation H.264",
                               "h264_mf");

  list = obs_properties_add_list(props, "rate_control", "Rate Control",
                                 OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
  obs_property_list_add_string(list, "CBR", "CBR");
  obs_property_list_add_string(list, "VBR", "VBR");

  obs_properties_add_int(props, "bitrate", "Bitrate (kbps)", 500, 50000, 100);
  obs_properties_add_int(props, "keyint_sec", "Keyframe Interval (s)", 0, 10,
                         1);
  obs_properties_add_int(props, "bframes", "B-Frames", 0, 4, 1);

  list = obs_properties_add_list(props, "preset", "Usage/Preset",
                                 OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
  obs_property_list_add_string(list, "Veryfast (Default)", "veryfast");
  obs_property_list_add_string(list, "Ultrafast", "ultrafast");
  obs_property_list_add_string(list, "Fast", "fast");
  obs_property_list_add_string(list, "Medium", "medium");
  obs_property_list_add_string(list, "Slow", "slow");

  obs_properties_add_text(props, "profile", "Profile (e.g. high, main)",
                          OBS_TEXT_DEFAULT);

  /* NTP设置 */
  obs_properties_add_bool(props, "ntp_enabled", "Enable NTP Sync");
  obs_properties_add_text(props, "ntp_server", "NTP Server", OBS_TEXT_DEFAULT);
  obs_properties_add_int(props, "ntp_port", "NTP Port", 1, 65535, 1);

  return props;
}

/* H.264 编码器回调 */
static void sei_stamper_encoder_update(void *data, obs_data_t *settings) {
  /* 更新编码器设置（如果需要运行时更新）*/
  UNUSED_PARAMETER(data);
  UNUSED_PARAMETER(settings);
}

struct obs_encoder_info sei_stamper_h264_encoder_info = {
    .id = "sei_stamper_h264",
    .type = OBS_ENCODER_VIDEO,
    .codec = "h264",
    .get_name = h264_encoder_getname,
    .create = h264_encoder_create,
    .destroy = sei_stamper_encoder_destroy,
    .encode = sei_stamper_encoder_encode,
    .update = sei_stamper_encoder_update,
    .get_defaults = sei_stamper_encoder_defaults,
    .get_properties = sei_stamper_encoder_properties,
    .get_extra_data = sei_stamper_encoder_extra_data,
    .caps = OBS_ENCODER_CAP_DEPRECATED | OBS_ENCODER_CAP_PASS_TEXTURE,
};

struct obs_encoder_info sei_stamper_h265_encoder_info = {
    .id = "sei_stamper_h265",
    .type = OBS_ENCODER_VIDEO,
    .codec = "hevc",
    .get_name = h265_encoder_getname,
    .create = h265_encoder_create,
    .destroy = sei_stamper_encoder_destroy,
    .encode = sei_stamper_encoder_encode,
    .update = sei_stamper_encoder_update,
    .get_defaults = sei_stamper_encoder_defaults,
    .get_properties = sei_stamper_encoder_properties,
    .get_extra_data = sei_stamper_encoder_extra_data,
    .caps = OBS_ENCODER_CAP_DEPRECATED | OBS_ENCODER_CAP_PASS_TEXTURE,
};

struct obs_encoder_info sei_stamper_av1_encoder_info = {
    .id = "sei_stamper_av1",
    .type = OBS_ENCODER_VIDEO,
    .codec = "av1",
    .get_name = av1_encoder_getname,
    .create = av1_encoder_create,
    .destroy = sei_stamper_encoder_destroy,
    .encode = sei_stamper_encoder_encode,
    .update = sei_stamper_encoder_update,
    .get_defaults = sei_stamper_encoder_defaults,
    .get_properties = sei_stamper_encoder_properties,
    .get_extra_data = sei_stamper_encoder_extra_data,
    .caps = OBS_ENCODER_CAP_DEPRECATED | OBS_ENCODER_CAP_PASS_TEXTURE,
};
