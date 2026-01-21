/******************************************************************************
    OBS SEI Stamper - Unified Encoder Implementation
    Copyright (C) 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#include "unified-encoder.h"
#include "amd-encoder.h"
#include "nvenc-encoder.h"
#include "qsv-encoder.h"
#include <util/dstr.h>

/* 日志宏 */
#define encoder_log(level, enc, format, ...)                                   \
  blog(level, "[Unified Encoder: '%s'] " format,                               \
       obs_encoder_get_name(enc->encoder), ##__VA_ARGS__)

/* 硬件类型名称 */
static const char *hardware_type_names[] = {
    "Intel QuickSync", // HARDWARE_TYPE_INTEL
    "NVIDIA NVENC",    // HARDWARE_TYPE_NVIDIA
    "AMD AMF",         // HARDWARE_TYPE_AMD
};

/* 编码格式名称 */
static const char *codec_type_names[] = {
    "H.264", // CODEC_TYPE_H264
    "H.265", // CODEC_TYPE_H265
    "AV1",   // CODEC_TYPE_AV1
};

/* 编码格式对应的codec字符串 */
static const char *codec_type_to_string(codec_type_t type) {
  switch (type) {
  case CODEC_TYPE_H264:
    return "h264";
  case CODEC_TYPE_H265:
    return "hevc";
  case CODEC_TYPE_AV1:
    return "av1";
  default:
    return "h264";
  }
}

/* 硬件编码器名称映射 */
static const char *get_encoder_name(hardware_type_t hw, codec_type_t codec) {
  switch (hw) {
  case HARDWARE_TYPE_INTEL:
    switch (codec) {
    case CODEC_TYPE_H264:
      return "h264_qsv";
    case CODEC_TYPE_H265:
      return "hevc_qsv";
    case CODEC_TYPE_AV1:
      return "av1_qsv";
    default:
      return "h264_qsv";
    }
  case HARDWARE_TYPE_NVIDIA:
    switch (codec) {
    case CODEC_TYPE_H264:
      return "h264_nvenc";
    case CODEC_TYPE_H265:
      return "hevc_nvenc";
    case CODEC_TYPE_AV1:
      return "av1_nvenc";
    default:
      return "h264_nvenc";
    }
  case HARDWARE_TYPE_AMD:
    switch (codec) {
    case CODEC_TYPE_H264:
      return "h264_amf";
    case CODEC_TYPE_H265:
      return "hevc_amf";
    case CODEC_TYPE_AV1:
      return "av1_amf";
    default:
      return "h264_amf";
    }
  default:
    return "h264_qsv";
  }
}

/*===========================================================================
 * 创建编码器
 *===========================================================================*/

void *unified_encoder_create(obs_data_t *settings, obs_encoder_t *encoder) {
  unified_encoder_t *enc = bzalloc(sizeof(unified_encoder_t));
  enc->encoder = encoder;

  // 读取用户选择的硬件类型
  enc->hardware_type =
      (hardware_type_t)obs_data_get_int(settings, "hardware_type");

  // 读取预设或用户选择的编码格式
  // 如果settings中有codec_type_preset，优先使用它（用于区分三个编码器）
  int64_t preset_codec = obs_data_get_int(settings, "codec_type_preset");
  if (preset_codec >= 0 && preset_codec < CODEC_TYPE_COUNT) {
    enc->codec_type = (codec_type_t)preset_codec;
  } else {
    // 否则从用户选择读取（向后兼容）
    enc->codec_type = (codec_type_t)obs_data_get_int(settings, "codec_type");
  }

  // 验证范围
  if (enc->hardware_type >= HARDWARE_TYPE_COUNT) {
    enc->hardware_type = HARDWARE_TYPE_INTEL;
  }
  if (enc->codec_type >= CODEC_TYPE_COUNT) {
    enc->codec_type = CODEC_TYPE_H264;
  }

  blog(LOG_INFO,
       "[Unified Encoder] Creating encoder with Hardware=%s, Codec=%s",
       hardware_type_names[enc->hardware_type],
       codec_type_names[enc->codec_type]);

  // 获取视频信息
  video_t *video = obs_encoder_video(encoder);
  if (!video) {
    blog(LOG_ERROR, "[Unified Encoder] Failed to get video context");
    bfree(enc);
    return NULL;
  }

  // 根据硬件类型创建底层编码器
  bool success = false;

  switch (enc->hardware_type) {
  case HARDWARE_TYPE_INTEL: {
#ifdef ENABLE_VPL
    enc->qsv_encoder = bzalloc(sizeof(qsv_encoder_t));
    qsv_encoder_t *qsv = (qsv_encoder_t *)enc->qsv_encoder;
    qsv->encoder = encoder;

    // 初始化QSV编码器（需要传递codec类型）
    const char *codec_str = codec_type_to_string(enc->codec_type);
    qsv->encoder = encoder;
    void *result = qsv_encoder_create_internal(settings, encoder);
    if (result) {
      enc->qsv_encoder = result;
      success = true;
    } else {
      blog(LOG_ERROR, "[Unified Encoder] Failed to initialize QSV encoder");
      bfree(enc->qsv_encoder);
      enc->qsv_encoder = NULL;
    }
#else
    blog(LOG_ERROR,
         "[Unified Encoder] Intel QuickSync not enabled in this build");
#endif
    break;
  }

  case HARDWARE_TYPE_NVIDIA: {
#ifdef ENABLE_NVENC
    enc->nvenc_encoder = bzalloc(sizeof(nvenc_encoder_t));
    nvenc_encoder_t *nvenc = (nvenc_encoder_t *)enc->nvenc_encoder;
    nvenc->encoder = encoder;

    // 初始化NVENC编码器
    void *result = nvenc_encoder_create_internal(settings, encoder);
    if (result) {
      enc->nvenc_encoder = result;
      success = true;
    } else {
      blog(LOG_ERROR, "[Unified Encoder] Failed to initialize NVENC encoder");
      bfree(enc->nvenc_encoder);
      enc->nvenc_encoder = NULL;
    }
#else
    blog(LOG_ERROR, "[Unified Encoder] NVIDIA NVENC not enabled in this build");
#endif
    break;
  }

  case HARDWARE_TYPE_AMD: {
#ifdef ENABLE_AMD
    enc->amd_encoder = bzalloc(sizeof(amd_encoder_t));
    amd_encoder_t *amd = (amd_encoder_t *)enc->amd_encoder;
    amd->encoder = encoder;

    // 初始化AMD编码器
    void *result = amd_encoder_create_internal(settings, encoder);
    if (result) {
      enc->amd_encoder = result;
      success = true;
    } else {
      blog(LOG_ERROR, "[Unified Encoder] Failed to initialize AMD encoder");
      bfree(enc->amd_encoder);
      enc->amd_encoder = NULL;
    }
#else
    blog(LOG_ERROR, "[Unified Encoder] AMD AMF not enabled in this build");
#endif
    break;
  }

  default:
    blog(LOG_ERROR, "[Unified Encoder] Unknown hardware type: %d",
         enc->hardware_type);
    break;
  }

  if (!success) {
    blog(LOG_ERROR, "[Unified Encoder] Encoder creation failed");
    bfree(enc);
    return NULL;
  }

  blog(LOG_INFO, "[Unified Encoder] Encoder created successfully");
  return enc;
}

/*===========================================================================
 * 销毁编码器
 *===========================================================================*/

void unified_encoder_destroy(void *data) {
  unified_encoder_t *enc = (unified_encoder_t *)data;
  if (!enc) {
    return;
  }

  blog(LOG_INFO, "[Unified Encoder] Destroying encoder");

  // 销毁底层编码器 - create_internal返回的是完整的encoder对象
  // 所以只需调用对应的destroy函数，不需要额外bfree
#ifdef ENABLE_VPL
  if (enc->qsv_encoder) {
    qsv_encoder_destroy((qsv_encoder_t *)enc->qsv_encoder);
    // 不要bfree，因为qsv_encoder_destroy已经释放了
    enc->qsv_encoder = NULL;
  }
#endif

#ifdef ENABLE_NVENC
  if (enc->nvenc_encoder) {
    nvenc_encoder_destroy((nvenc_encoder_t *)enc->nvenc_encoder);
    // 不要bfree，因为nvenc_encoder_destroy已经释放了
    enc->nvenc_encoder = NULL;
  }
#endif

#ifdef ENABLE_AMD
  if (enc->amd_encoder) {
    amd_encoder_destroy((amd_encoder_t *)enc->amd_encoder);
    // 不要bfree，因为amd_encoder_destroy已经释放了
    enc->amd_encoder = NULL;
  }
#endif

  bfree(enc);
}

/*===========================================================================
 * 编码视频帧
 *===========================================================================*/

bool unified_encoder_encode(void *data, struct encoder_frame *frame,
                            struct encoder_packet *packet,
                            bool *received_packet) {
  unified_encoder_t *enc = (unified_encoder_t *)data;
  if (!enc) {
    return false;
  }

  // 转发到相应的底层编码器
  switch (enc->hardware_type) {
  case HARDWARE_TYPE_INTEL:
#ifdef ENABLE_VPL
    if (enc->qsv_encoder) {
      return qsv_encoder_encode_internal(enc->qsv_encoder, frame, packet,
                                         received_packet);
    }
#endif
    break;

  case HARDWARE_TYPE_NVIDIA:
#ifdef ENABLE_NVENC
    if (enc->nvenc_encoder) {
      return nvenc_encoder_encode_internal(enc->nvenc_encoder, frame, packet,
                                           received_packet);
    }
#endif
    break;

  case HARDWARE_TYPE_AMD:
#ifdef ENABLE_AMD
    if (enc->amd_encoder) {
      return amd_encoder_encode_internal(enc->amd_encoder, frame, packet,
                                         received_packet);
    }
#endif
    break;

  default:
    break;
  }

  blog(LOG_ERROR, "[Unified Encoder] No valid encoder for encoding");
  return false;
}

/*===========================================================================
 /* 获取默认设置 - H.264专用 */
void unified_encoder_get_defaults_h264(obs_data_t *settings) {
  obs_data_set_default_int(settings, "hardware_type", HARDWARE_TYPE_INTEL);
  obs_data_set_default_int(settings, "codec_type_preset", CODEC_TYPE_H264);
  obs_data_set_default_int(settings, "bitrate", 2500);
  obs_data_set_default_int(settings, "keyint_sec", 2);
  obs_data_set_default_int(settings, "bframes", 0);
  obs_data_set_default_string(settings, "profile", "high");
  obs_data_set_default_string(settings, "preset", "balanced");
  obs_data_set_default_bool(settings, "ntp_enabled", true);
  obs_data_set_default_string(settings, "ntp_server", "pool.ntp.org");
  obs_data_set_default_int(settings, "ntp_port", 123);
  obs_data_set_default_int(settings, "ntp_sync_interval_ms", 60000);
}

/* 获取默认设置 - H.265专用 */
void unified_encoder_get_defaults_h265(obs_data_t *settings) {
  obs_data_set_default_int(settings, "hardware_type", HARDWARE_TYPE_INTEL);
  obs_data_set_default_int(settings, "codec_type_preset", CODEC_TYPE_H265);
  obs_data_set_default_int(settings, "bitrate", 2500);
  obs_data_set_default_int(settings, "keyint_sec", 2);
  obs_data_set_default_int(settings, "bframes", 0);
  obs_data_set_default_string(settings, "profile", "high");
  obs_data_set_default_string(settings, "preset", "balanced");
  obs_data_set_default_bool(settings, "ntp_enabled", true);
  obs_data_set_default_string(settings, "ntp_server", "pool.ntp.org");
  obs_data_set_default_int(settings, "ntp_port", 123);
  obs_data_set_default_int(settings, "ntp_sync_interval_ms", 60000);
}

/* 获取默认设置 - AV1专用 */
void unified_encoder_get_defaults_av1(obs_data_t *settings) {
  obs_data_set_default_int(settings, "hardware_type", HARDWARE_TYPE_INTEL);
  obs_data_set_default_int(settings, "codec_type_preset", CODEC_TYPE_AV1);
  obs_data_set_default_int(settings, "bitrate", 2500);
  obs_data_set_default_int(settings, "keyint_sec", 2);
  obs_data_set_default_int(settings, "bframes", 0);
  obs_data_set_default_string(settings, "profile", "high");
  obs_data_set_default_string(settings, "preset", "balanced");
  obs_data_set_default_bool(settings, "ntp_enabled", true);
  obs_data_set_default_string(settings, "ntp_server", "pool.ntp.org");
  obs_data_set_default_int(settings, "ntp_port", 123);
  obs_data_set_default_int(settings, "ntp_sync_interval_ms", 60000);
}

/*===========================================================================
 * 通用默认设置（向后兼容）
 *===========================================================================*/

void unified_encoder_get_defaults(obs_data_t *settings) {
  // 默认硬件类型：Intel QuickSync
  obs_data_set_default_int(settings, "hardware_type", HARDWARE_TYPE_INTEL);

  // 默认编码格式：H.264
  obs_data_set_default_int(settings, "codec_type", CODEC_TYPE_H264);

  // 编码参数默认值
  obs_data_set_default_int(settings, "bitrate", 2500);         // kbps
  obs_data_set_default_int(settings, "keyint_sec", 2);         // seconds
  obs_data_set_default_int(settings, "bframes", 0);            // B frames
  obs_data_set_default_string(settings, "profile", "high");    // profile
  obs_data_set_default_string(settings, "preset", "balanced"); // preset

  // NTP同步默认值
  obs_data_set_default_bool(settings, "ntp_enabled", true);
  obs_data_set_default_string(settings, "ntp_server", "pool.ntp.org");
  obs_data_set_default_int(settings, "ntp_port", 123);
  obs_data_set_default_int(settings, "ntp_sync_interval_ms",
                           60000); // 60秒
}

/*===========================================================================
 * 获取编码器属性（UI）
 *===========================================================================*/

obs_properties_t *unified_encoder_properties(void *unused) {
  UNUSED_PARAMETER(unused);

  obs_properties_t *props = obs_properties_create();

  // 硬件编码器选择下拉框
  obs_property_t *hw_list =
      obs_properties_add_list(props, "hardware_type", "Hardware Encoder",
                              OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);

  obs_property_list_add_int(hw_list, "Intel QuickSync", HARDWARE_TYPE_INTEL);
  obs_property_list_add_int(hw_list, "NVIDIA NVENC", HARDWARE_TYPE_NVIDIA);
  obs_property_list_add_int(hw_list, "AMD AMF", HARDWARE_TYPE_AMD);

  // Codec Format 已经通过注册不同的encoder固定，不再需要UI选择

  // 编码参数
  obs_properties_add_int(props, "bitrate", "Bitrate (kbps)", 500, 50000, 100);
  obs_properties_add_int(props, "keyint_sec", "Keyframe Interval (seconds)", 1,
                         10, 1);
  obs_properties_add_int(props, "bframes", "B-frames", 0, 4, 1);

  // Profile选择
  obs_property_t *profile_list =
      obs_properties_add_list(props, "profile", "Profile", OBS_COMBO_TYPE_LIST,
                              OBS_COMBO_FORMAT_STRING);
  obs_property_list_add_string(profile_list, "Baseline", "baseline");
  obs_property_list_add_string(profile_list, "Main", "main");
  obs_property_list_add_string(profile_list, "High", "high");

  // Preset选择
  obs_property_t *preset_list = obs_properties_add_list(
      props, "preset", "Preset", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
  obs_property_list_add_string(preset_list, "Fast", "fast");
  obs_property_list_add_string(preset_list, "Balanced", "balanced");
  obs_property_list_add_string(preset_list, "Quality", "quality");

  // NTP同步设置
  obs_properties_add_bool(props, "ntp_enabled", "Enable NTP Sync");
  obs_properties_add_text(props, "ntp_server", "NTP Server", OBS_TEXT_DEFAULT);
  obs_properties_add_int(props, "ntp_port", "NTP Port", 1, 65535, 1);
  obs_properties_add_int(props, "ntp_sync_interval_ms",
                         "NTP Sync Interval (ms)", 1000, 300000, 1000);

  return props;
}

/*===========================================================================
 * 获取编码器名称
 *===========================================================================*/

const char *unified_encoder_get_name(void *type_data) {
  UNUSED_PARAMETER(type_data);

  // 注意：type_data在这里是NULL，因为这是在注册时调用的
  // 我们需要从obs_encoder_info的id来区分，但get_name不提供这个信息
  // 所以我们创建三个独立的get_name函数
  return "SEI STAMPER";
}

const char *unified_encoder_get_name_h264(void *type_data) {
  UNUSED_PARAMETER(type_data);
  return "SEI STAMPER (H.264)";
}

const char *unified_encoder_get_name_h265(void *type_data) {
  UNUSED_PARAMETER(type_data);
  return "SEI STAMPER (H.265)";
}

const char *unified_encoder_get_name_av1(void *type_data) {
  UNUSED_PARAMETER(type_data);
  return "SEI STAMPER (AV1)";
}

/*===========================================================================
 * 获取视频信息
 *===========================================================================*/

void unified_encoder_get_video_info(void *data, struct video_scale_info *info) {
  unified_encoder_t *enc = (unified_encoder_t *)data;
  if (!enc) {
    return;
  }

  // 转发到底层编码器
  switch (enc->hardware_type) {
  case HARDWARE_TYPE_INTEL:
#ifdef ENABLE_VPL
    if (enc->qsv_encoder) {
      qsv_encoder_get_video_info_internal(enc->qsv_encoder, info);
      return;
    }
#endif
    break;

  case HARDWARE_TYPE_NVIDIA:
#ifdef ENABLE_NVENC
    if (enc->nvenc_encoder) {
      nvenc_encoder_get_video_info_internal(enc->nvenc_encoder, info);
      return;
    }
#endif
    break;

  case HARDWARE_TYPE_AMD:
#ifdef ENABLE_AMD
    if (enc->amd_encoder) {
      amd_encoder_get_video_info_internal(enc->amd_encoder, info);
      return;
    }
#endif
    break;

  default:
    break;
  }

  // 默认返回NV12格式
  info->format = VIDEO_FORMAT_NV12;
}

/*===========================================================================
 * 获取Extra Data
 *===========================================================================*/

bool unified_encoder_get_extra_data(void *data, uint8_t **extra_data,
                                    size_t *size) {
  unified_encoder_t *enc = (unified_encoder_t *)data;
  if (!enc) {
    return false;
  }

  // 转发到底层编码器
  switch (enc->hardware_type) {
  case HARDWARE_TYPE_INTEL:
#ifdef ENABLE_VPL
    if (enc->qsv_encoder) {
      return qsv_encoder_get_extra_data_internal(enc->qsv_encoder, extra_data,
                                                 size);
    }
#endif
    break;

  case HARDWARE_TYPE_NVIDIA:
#ifdef ENABLE_NVENC
    if (enc->nvenc_encoder) {
      return nvenc_encoder_get_extra_data_internal(enc->nvenc_encoder,
                                                   extra_data, size);
    }
#endif
    break;

  case HARDWARE_TYPE_AMD:
#ifdef ENABLE_AMD
    if (enc->amd_encoder) {
      return amd_encoder_get_extra_data_internal(enc->amd_encoder, extra_data,
                                                 size);
    }
#endif
    break;

  default:
    break;
  }

  return false;
}

/*===========================================================================
 * 编码器信息结构体 - 基础模板
 *===========================================================================*/

// 这些将在plugin.c中被复制并修改为三个独立的encoder info
struct obs_encoder_info unified_encoder_info_h264 = {
    .id = "sei_stamper_h264",
    .type = OBS_ENCODER_VIDEO,
    .codec = "h264",
    .get_name = unified_encoder_get_name_h264,
    .create = unified_encoder_create,
    .destroy = unified_encoder_destroy,
    .encode = unified_encoder_encode,
    .get_defaults = unified_encoder_get_defaults_h264,
    .get_properties = unified_encoder_properties,
    .get_video_info = unified_encoder_get_video_info,
    .get_extra_data = unified_encoder_get_extra_data,
};

struct obs_encoder_info unified_encoder_info_h265 = {
    .id = "sei_stamper_h265",
    .type = OBS_ENCODER_VIDEO,
    .codec = "hevc",
    .get_name = unified_encoder_get_name_h265,
    .create = unified_encoder_create,
    .destroy = unified_encoder_destroy,
    .encode = unified_encoder_encode,
    .get_defaults = unified_encoder_get_defaults_h265,
    .get_properties = unified_encoder_properties,
    .get_video_info = unified_encoder_get_video_info,
    .get_extra_data = unified_encoder_get_extra_data,
};

struct obs_encoder_info unified_encoder_info_av1 = {
    .id = "sei_stamper_av1",
    .type = OBS_ENCODER_VIDEO,
    .codec = "av1",
    .get_name = unified_encoder_get_name_av1,
    .create = unified_encoder_create,
    .destroy = unified_encoder_destroy,
    .encode = unified_encoder_encode,
    .get_defaults = unified_encoder_get_defaults_av1,
    .get_properties = unified_encoder_properties,
    .get_video_info = unified_encoder_get_video_info,
    .get_extra_data = unified_encoder_get_extra_data,
};
