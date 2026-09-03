/******************************************************************************
    SEI Stamper Encoder - Header File
    Copyright (C) 2026

    Encoder wrapper that inserts NTP timestamp SEI into video streams
******************************************************************************/

#pragma once

#include "ntp-client.h"
#include "sei-handler.h"
#include <obs-module.h>

#ifdef __cplusplus
extern "C" {
#endif

#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>

/* Encoder wrapper context */
struct sei_stamper_encoder {
  obs_encoder_t *context; /* OBS encoder context */

  /* FFmpeg encoder */
  const AVCodec *codec;
  AVCodecContext *codec_context;
  AVFrame *frame;
  AVPacket *packet;

  /* Encoder settings */
  int bitrate;
  char *preset;
  char *profile;
  char *rate_control;
  int keyint_sec;
  int bframes;

  /* Encoder type */
  enum sei_stamper_codec_type codec_type;

  /* NTP client */
  ntp_client_t ntp_client;
  bool ntp_enabled;

  /* SEI data buffer */
  uint8_t *merged_sei_buffer;
  size_t merged_sei_size;

  /* Current frame info */
  int64_t current_pts;
  ntp_timestamp_t current_ntp_time;

  /* Packet data buffer (for reassembling packets) */
  uint8_t *packet_buffer;
  size_t packet_buffer_size;
};

/* Encoder type */
enum sei_stamper_codec_type {
  SEI_STAMPER_CODEC_H264,
  SEI_STAMPER_CODEC_H265,
  SEI_STAMPER_CODEC_AV1
};

/* External declarations of the encoder info structures */
extern struct obs_encoder_info sei_stamper_h264_encoder_info;
extern struct obs_encoder_info sei_stamper_h265_encoder_info;
extern struct obs_encoder_info sei_stamper_av1_encoder_info;

#ifdef __cplusplus
}
#endif
