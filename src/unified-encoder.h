/******************************************************************************
    SEI Stamper - Unified Encoder Header
    Copyright (C) 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#ifndef UNIFIED_ENCODER_H
#define UNIFIED_ENCODER_H

#include <obs-module.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Hardware encoder type */
typedef enum {
  HARDWARE_TYPE_INTEL = 0,  /* Intel QuickSync */
  HARDWARE_TYPE_NVIDIA = 1, /* NVIDIA NVENC */
  HARDWARE_TYPE_AMD = 2,    /* AMD AMF */
  HARDWARE_TYPE_COUNT
} hardware_type_t;

/* Codec format type */
typedef enum {
  CODEC_TYPE_H264 = 0, /* H.264/AVC */
  CODEC_TYPE_H265 = 1, /* H.265/HEVC */
  CODEC_TYPE_AV1 = 2,  /* AV1 */
  CODEC_TYPE_COUNT
} codec_type_t;

/* Unified encoder data structure */
typedef struct unified_encoder {
  obs_encoder_t *encoder; /* OBS encoder context */

  /* User selection */
  hardware_type_t hardware_type; /* Selected hardware type */
  codec_type_t codec_type;       /* Selected codec format */

  /* Backend encoder instances (only one is ever used) */
  void *qsv_encoder;   /* qsv_encoder_t* */
  void *nvenc_encoder; /* nvenc_encoder_t* */
  void *amd_encoder;   /* amd_encoder_t* */

} unified_encoder_t;

/* Encoder function declarations */
/* Function declarations */
void *unified_encoder_create(obs_data_t *settings, obs_encoder_t *encoder);
void unified_encoder_destroy(void *data);
bool unified_encoder_encode(void *data, struct encoder_frame *frame,
                            struct encoder_packet *packet,
                            bool *received_packet);
void unified_encoder_get_defaults(obs_data_t *settings);
obs_properties_t *unified_encoder_properties(void *unused);
const char *unified_encoder_get_name(void *type_data);
void unified_encoder_get_video_info(void *data, struct video_scale_info *info);
bool unified_encoder_get_extra_data(void *data, uint8_t **extra_data,
                                    size_t *size);

/* OBS encoder info structures - three independent encoders */
extern struct obs_encoder_info unified_encoder_info_h264;
extern struct obs_encoder_info unified_encoder_info_h265;
extern struct obs_encoder_info unified_encoder_info_av1;

#ifdef __cplusplus
}
#endif

#endif // UNIFIED_ENCODER_H
