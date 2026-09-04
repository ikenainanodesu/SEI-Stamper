#ifndef NVENC_ENCODER_H
#define NVENC_ENCODER_H

#include <obs-module.h>

#ifdef ENABLE_NVENC

#include "ntp-client.h"
#include "sei-handler.h"
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>

typedef struct nvenc_encoder {
  obs_encoder_t *encoder;

  /* FFmpeg encoder */
  const AVCodec *codec;
  AVCodecContext *codec_context;
  AVFrame *frame;
  AVPacket *packet;

  /* Configuration */
  int width;
  int height;
  int fps_num;
  int fps_den;
  int bitrate; // kbps
  int keyint;  // frames
  int bframes;
  char *profile;
  char *preset;

  /* Codec Type */
  int codec_type;      /* 0=H.264, 1=H.265, 2=AV1 */
  char codec_name[32]; /* FFmpeg encoder name */

  /* Extra data (SPS/PPS) handed to OBS via get_extra_data */
  uint8_t *extra_data;
  size_t extra_data_size;

  /* Parameter sets in Annex-B form, injected into keyframes that carry no
   * SPS so MPEG-TS/SRT consumers joining mid-stream can decode. */
  uint8_t *inline_params;
  size_t inline_params_size;

  /* NTP sync */
  struct ntp_client ntp_client;
  ntp_timestamp_t current_ntp_time;
  bool ntp_enabled;
  uint32_t ntp_sync_interval_ms; /* NTP sync interval (ms) */

  /* Packet buffer */
  uint8_t *packet_buffer;    // Temporary packet buffer
  size_t packet_buffer_size; // Packet buffer size
} nvenc_encoder_t;

/* Public API functions for unified encoder */
void *nvenc_encoder_create_internal(obs_data_t *settings,
                                    obs_encoder_t *encoder);
bool nvenc_encoder_encode_internal(void *data, struct encoder_frame *frame,
                                   struct encoder_packet *packet,
                                   bool *received_packet);
void nvenc_encoder_get_video_info_internal(void *data,
                                           struct video_scale_info *info);
bool nvenc_encoder_get_extra_data_internal(void *data, uint8_t **extra_data,
                                           size_t *size);

bool nvenc_encoder_init(nvenc_encoder_t *enc, obs_data_t *settings,
                        video_t *video);
void nvenc_encoder_destroy(nvenc_encoder_t *enc);
bool nvenc_encoder_encode(nvenc_encoder_t *enc, struct encoder_frame *frame,
                          struct encoder_packet *packet, bool *received_packet);
void nvenc_encoder_get_defaults(obs_data_t *settings);
obs_properties_t *nvenc_encoder_properties(void *unused);
bool nvenc_encoder_extra_data(void *data, uint8_t **extra_data, size_t *size);
void nvenc_encoder_video_info(void *data, struct video_scale_info *info);

extern struct obs_encoder_info nvenc_encoder_info;

#endif // ENABLE_NVENC

#endif // NVENC_ENCODER_H
