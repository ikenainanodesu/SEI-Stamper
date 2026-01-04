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

/* 编码器包装器上下文 */
struct sei_stamper_encoder {
  obs_encoder_t *context; /* OBS编码器上下文 */

  /* FFmpeg 编码器 */
  const AVCodec *codec;
  AVCodecContext *codec_context;
  AVFrame *frame;
  AVPacket *packet;

  /* 编码器设置 */
  int bitrate;
  char *preset;
  char *profile;
  char *rate_control;
  int keyint_sec;
  int bframes;

  /* 编码器类型 */
  enum sei_stamper_codec_type codec_type;

  /* NTP客户端 */
  ntp_client_t ntp_client;
  bool ntp_enabled;
  uint64_t last_ntp_sync_time;

  /* SEI数据缓冲 */
  uint8_t *merged_sei_buffer;
  size_t merged_sei_size;

  /* 当前帧信息 */
  int64_t current_pts;
  ntp_timestamp_t current_ntp_time;

  /* packet数据缓冲 (用于重组packet) */
  uint8_t *packet_buffer;
  size_t packet_buffer_size;
};

/* 编码器类型 */
enum sei_stamper_codec_type {
  SEI_STAMPER_CODEC_H264,
  SEI_STAMPER_CODEC_H265,
  SEI_STAMPER_CODEC_AV1
};

/* 外部声明编码器info结构 */
extern struct obs_encoder_info sei_stamper_h264_encoder_info;
extern struct obs_encoder_info sei_stamper_h265_encoder_info;
extern struct obs_encoder_info sei_stamper_av1_encoder_info;

#ifdef __cplusplus
}
#endif
