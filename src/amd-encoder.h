#ifndef AMD_ENCODER_H
#define AMD_ENCODER_H

#include <obs-module.h>

#ifdef ENABLE_AMD

#include "ntp-client.h"
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>

typedef struct amd_encoder {
  obs_encoder_t *encoder;

  /* FFmpeg 编码器 */
  const AVCodec *codec;
  AVCodecContext *codec_context;
  AVFrame *frame;
  AVPacket *packet;

  /* 配置 */
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

  /* Extra Data (SPS/PPS) */
  uint8_t *extra_data;
  size_t extra_data_size;

  /* NTP 同步 */
  struct ntp_client ntp_client;
  uint64_t last_ntp_sync_time;
  ntp_timestamp_t current_ntp_time;
  bool ntp_enabled;
  uint32_t ntp_sync_interval_ms; /* NTP同步间隔（毫秒） */

  /* Packet 缓冲区 */
  uint8_t *packet_buffer;    // 临时packet缓冲区
  size_t packet_buffer_size; // packet缓冲区大小

} amd_encoder_t;

/* Public API functions for unified encoder */
void *amd_encoder_create_internal(obs_data_t *settings, obs_encoder_t *encoder);
bool amd_encoder_encode_internal(void *data, struct encoder_frame *frame,
                                 struct encoder_packet *packet,
                                 bool *received_packet);
void amd_encoder_get_video_info_internal(void *data,
                                         struct video_scale_info *info);
bool amd_encoder_get_extra_data_internal(void *data, uint8_t **extra_data,
                                         size_t *size);

bool amd_encoder_init(amd_encoder_t *enc, obs_data_t *settings, video_t *video);
void amd_encoder_destroy(amd_encoder_t *enc);
bool amd_encoder_encode(amd_encoder_t *enc, struct encoder_frame *frame,
                        struct encoder_packet *packet, bool *received_packet);
void amd_encoder_get_defaults(obs_data_t *settings);
obs_properties_t *amd_encoder_properties(void *unused);
bool amd_encoder_extra_data(void *data, uint8_t **extra_data, size_t *size);
void amd_encoder_video_info(void *data, struct video_scale_info *info);

extern struct obs_encoder_info amd_encoder_info;

#endif // ENABLE_AMD

#endif // AMD_ENCODER_H
