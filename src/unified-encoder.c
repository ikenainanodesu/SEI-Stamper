/******************************************************************************
    SEI Stamper - Unified Encoder Implementation
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

/* Logging macros */
#define encoder_log(level, enc, format, ...)                                   \
  blog(level, "[Unified Encoder: '%s'] " format,                               \
       obs_encoder_get_name(enc->encoder), ##__VA_ARGS__)

/* Hardware type names */
static const char *hardware_type_names[] = {
    "Intel QuickSync", // HARDWARE_TYPE_INTEL
    "NVIDIA NVENC",    // HARDWARE_TYPE_NVIDIA
    "AMD AMF",         // HARDWARE_TYPE_AMD
};

/* Codec format names */
static const char *codec_type_names[] = {
    "H.264", // CODEC_TYPE_H264
    "H.265", // CODEC_TYPE_H265
    "AV1",   // CODEC_TYPE_AV1
};

/* Codec strings for each codec format */
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

/* Hardware encoder name map */
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
 * Create the encoder
 *===========================================================================*/

void *unified_encoder_create(obs_data_t *settings, obs_encoder_t *encoder) {
  unified_encoder_t *enc = bzalloc(sizeof(unified_encoder_t));
  enc->encoder = encoder;

  // Read the hardware type the user selected
  enc->hardware_type =
      (hardware_type_t)obs_data_get_int(settings, "hardware_type");

  // Determine the codec type from the encoder ID (the most reliable way)
  const char *encoder_id = obs_encoder_get_id(encoder);
  blog(LOG_INFO, "[Unified Encoder] Encoder ID: %s",
       encoder_id ? encoder_id : "NULL");

  if (encoder_id) {
    if (strcmp(encoder_id, "sei_stamper_h264") == 0) {
      enc->codec_type = CODEC_TYPE_H264;
      blog(LOG_INFO, "[Unified Encoder] Detected H.264 from encoder ID");
    } else if (strcmp(encoder_id, "sei_stamper_h265") == 0) {
      enc->codec_type = CODEC_TYPE_H265;
      blog(LOG_INFO, "[Unified Encoder] Detected H.265 from encoder ID");
    } else if (strcmp(encoder_id, "sei_stamper_av1") == 0) {
      enc->codec_type = CODEC_TYPE_AV1;
      blog(LOG_INFO, "[Unified Encoder] Detected AV1 from encoder ID");
    } else {
      // Unknown ID; fall back to reading it from settings
      enc->codec_type =
          (codec_type_t)obs_data_get_int(settings, "codec_type_preset");
      blog(LOG_WARNING,
           "[Unified Encoder] Unknown encoder ID, using codec_type_preset: %d",
           enc->codec_type);
    }
  } else {
    // encoder_id is NULL, so read it from settings
    enc->codec_type =
        (codec_type_t)obs_data_get_int(settings, "codec_type_preset");
    blog(LOG_WARNING,
         "[Unified Encoder] Encoder ID is NULL, using codec_type_preset: %d",
         enc->codec_type);
  }

  // Validate the range
  if (enc->hardware_type >= HARDWARE_TYPE_COUNT) {
    enc->hardware_type = HARDWARE_TYPE_INTEL;
  }
  if (enc->codec_type >= CODEC_TYPE_COUNT) {
    enc->codec_type = CODEC_TYPE_H264;
    blog(LOG_WARNING,
         "[Unified Encoder] codec_type out of range, defaulting to H.264");
  }

  blog(LOG_INFO,
       "[Unified Encoder] Creating encoder with Hardware=%s, Codec=%s",
       hardware_type_names[enc->hardware_type],
       codec_type_names[enc->codec_type]);

  // Get the video info
  video_t *video = obs_encoder_video(encoder);
  if (!video) {
    blog(LOG_ERROR, "[Unified Encoder] Failed to get video context");
    bfree(enc);
    return NULL;
  }

  // Create the backend encoder for the selected hardware type
  bool success = false;

  switch (enc->hardware_type) {
  case HARDWARE_TYPE_INTEL: {
#ifdef ENABLE_VPL
    // Put codec_type into settings so the QSV encoder can read it
    obs_data_set_int(settings, "codec_type", enc->codec_type);

    enc->qsv_encoder = bzalloc(sizeof(qsv_encoder_t));
    qsv_encoder_t *qsv = (qsv_encoder_t *)enc->qsv_encoder;
    qsv->encoder = encoder;

    // Initialise the QSV encoder (the codec type has to be passed in)
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
    // Put codec_type into settings
    obs_data_set_int(settings, "codec_type", enc->codec_type);

    enc->nvenc_encoder = bzalloc(sizeof(nvenc_encoder_t));
    nvenc_encoder_t *nvenc = (nvenc_encoder_t *)enc->nvenc_encoder;
    nvenc->encoder = encoder;

    // Initialise the NVENC encoder
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
    // Put codec_type into settings
    obs_data_set_int(settings, "codec_type", enc->codec_type);

    enc->amd_encoder = bzalloc(sizeof(amd_encoder_t));
    amd_encoder_t *amd = (amd_encoder_t *)enc->amd_encoder;
    amd->encoder = encoder;

    // Initialise the AMD encoder
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
 * Destroy the encoder
 *===========================================================================*/

void unified_encoder_destroy(void *data) {
  unified_encoder_t *enc = (unified_encoder_t *)data;
  if (!enc) {
    return;
  }

  blog(LOG_INFO, "[Unified Encoder] Destroying encoder");

  // create_internal returns a complete encoder object, so its destroy
  // function is all that is needed - no extra bfree.
#ifdef ENABLE_VPL
  if (enc->qsv_encoder) {
    qsv_encoder_destroy((qsv_encoder_t *)enc->qsv_encoder);
    // No bfree: qsv_encoder_destroy already freed it
    enc->qsv_encoder = NULL;
  }
#endif

#ifdef ENABLE_NVENC
  if (enc->nvenc_encoder) {
    nvenc_encoder_destroy((nvenc_encoder_t *)enc->nvenc_encoder);
    // No bfree: nvenc_encoder_destroy already freed it
    enc->nvenc_encoder = NULL;
  }
#endif

#ifdef ENABLE_AMD
  if (enc->amd_encoder) {
    amd_encoder_destroy((amd_encoder_t *)enc->amd_encoder);
    // No bfree: amd_encoder_destroy already freed it
    enc->amd_encoder = NULL;
  }
#endif

  bfree(enc);
}

/*===========================================================================
 * Encode a video frame
 *===========================================================================*/

bool unified_encoder_encode(void *data, struct encoder_frame *frame,
                            struct encoder_packet *packet,
                            bool *received_packet) {
  unified_encoder_t *enc = (unified_encoder_t *)data;
  if (!enc) {
    return false;
  }

  static uint64_t unified_frame_count = 0;
  unified_frame_count++;

  if (unified_frame_count % 30 == 1) {
    blog(LOG_INFO, "[Unified Encoder] encode() called: frame #%llu",
         unified_frame_count);
  }

  // Forward to the matching backend encoder
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
 /* Default settings - H.264 */
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

/* Default settings - H.265 */
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

/* Default settings - AV1 */
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
 * Generic defaults (backward compatibility)
 *===========================================================================*/

void unified_encoder_get_defaults(obs_data_t *settings) {
  // Default hardware type: Intel QuickSync
  obs_data_set_default_int(settings, "hardware_type", HARDWARE_TYPE_INTEL);

  // Default codec format: H.264
  obs_data_set_default_int(settings, "codec_type", CODEC_TYPE_H264);
  // Encoding parameter defaults
  obs_data_set_default_int(settings, "bitrate", 2500);         // kbps
  obs_data_set_default_int(settings, "keyint_sec", 2);         // seconds
  obs_data_set_default_int(settings, "bframes", 0);            // B frames
  obs_data_set_default_string(settings, "profile", "high");    // profile
  obs_data_set_default_string(settings, "preset", "balanced"); // preset

  // NTP sync defaults
  obs_data_set_default_bool(settings, "ntp_enabled", true);
  obs_data_set_default_string(settings, "ntp_server", "pool.ntp.org");
  obs_data_set_default_int(settings, "ntp_port", 123);
  obs_data_set_default_int(settings, "ntp_sync_interval_ms",
                           60000); // 60 s
}

/*===========================================================================
 * Encoder properties (UI)
 *===========================================================================*/

obs_properties_t *unified_encoder_properties(void *unused) {
  UNUSED_PARAMETER(unused);

  obs_properties_t *props = obs_properties_create();

  // Hardware encoder dropdown
  obs_property_t *hw_list =
      obs_properties_add_list(props, "hardware_type", "Hardware Encoder",
                              OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);

  obs_property_list_add_int(hw_list, "Intel QuickSync", HARDWARE_TYPE_INTEL);
  obs_property_list_add_int(hw_list, "NVIDIA NVENC", HARDWARE_TYPE_NVIDIA);
  obs_property_list_add_int(hw_list, "AMD AMF", HARDWARE_TYPE_AMD);

  // Codec format is fixed per registered encoder, so it needs no UI choice

  // Encoding parameters
  obs_properties_add_int(props, "bitrate", "Bitrate (kbps)", 500, 50000, 100);
  obs_properties_add_int(props, "keyint_sec", "Keyframe Interval (seconds)", 1,
                         10, 1);
  obs_properties_add_int(props, "bframes", "B-frames", 0, 4, 1);

  // Profile choice
  obs_property_t *profile_list =
      obs_properties_add_list(props, "profile", "Profile", OBS_COMBO_TYPE_LIST,
                              OBS_COMBO_FORMAT_STRING);
  obs_property_list_add_string(profile_list, "Baseline", "baseline");
  obs_property_list_add_string(profile_list, "Main", "main");
  obs_property_list_add_string(profile_list, "High", "high");

  // Preset choice
  obs_property_t *preset_list = obs_properties_add_list(
      props, "preset", "Preset", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
  obs_property_list_add_string(preset_list, "Fast", "fast");
  obs_property_list_add_string(preset_list, "Balanced", "balanced");
  obs_property_list_add_string(preset_list, "Quality", "quality");

  // NTP sync settings
  obs_properties_add_bool(props, "ntp_enabled", "Enable NTP Sync");
  obs_properties_add_text(props, "ntp_server", "NTP Server", OBS_TEXT_DEFAULT);
  obs_properties_add_int(props, "ntp_port", "NTP Port", 1, 65535, 1);
  obs_properties_add_int(props, "ntp_sync_interval_ms",
                         "NTP Sync Interval (ms)", 1000, 300000, 1000);

  return props;
}

/*===========================================================================
 * Encoder name
 *===========================================================================*/

const char *unified_encoder_get_name(void *type_data) {
  UNUSED_PARAMETER(type_data);

  // Note: type_data is NULL here because this is called at registration time
  // Telling them apart needs the obs_encoder_info id, which get_name lacks
  // hence three separate get_name functions
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
 * Get the video info
 *===========================================================================*/

void unified_encoder_get_video_info(void *data, struct video_scale_info *info) {
  unified_encoder_t *enc = (unified_encoder_t *)data;
  if (!enc) {
    return;
  }

  // Forward to the backend encoder
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

  // Default to NV12
  info->format = VIDEO_FORMAT_NV12;
}

/*===========================================================================
 * Get extra data
 *===========================================================================*/

bool unified_encoder_get_extra_data(void *data, uint8_t **extra_data,
                                    size_t *size) {
  unified_encoder_t *enc = (unified_encoder_t *)data;
  if (!enc) {
    return false;
  }

  // Forward to the backend encoder
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
 * Encoder info struct - base template
 *===========================================================================*/

// copied and adjusted in plugin.c into three independent encoder infos
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
