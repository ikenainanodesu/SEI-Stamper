#ifndef QSV_ENCODER_H
#define QSV_ENCODER_H

#include <obs-module.h>

#ifdef ENABLE_VPL
#include <vpl/mfx.h>
/* In case mfx.h doesn't include everything on this version */
#include <vpl/mfxdispatcher.h>
#include <vpl/mfxvideo.h>

#include "ntp-client.h"

typedef struct qsv_encoder {
  obs_encoder_t *encoder;

  /* VPL Session */
  mfxLoader loader;
  mfxSession session;

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
  int codec_type; /* 0=H.264, 1=H.265, 2=AV1 */

  /* Surfaces */
  mfxFrameAllocResponse mfxResponse;
  mfxFrameSurface1 **pmfxSurfaces;
  int nSurfNum;

  /* Bitstream Buffer */
  mfxBitstream mfxBS;
  uint8_t *bs_buffer;

  /* VPL Params */
  mfxVideoParam mfxParams;

  /* Header Data */
  uint8_t *extra_data;
  size_t extra_data_size;

  /* NTP Synchronization */
  struct ntp_client ntp_client;     // NTP客户端
  uint64_t last_ntp_sync_time;      // 上次NTP同步时间
  ntp_timestamp_t current_ntp_time; // 当前编码帧的NTP时间戳
  bool ntp_enabled;                 // NTP是否启用
  uint32_t ntp_sync_interval_ms;    /* NTP同步间隔（毫秒）*/

} qsv_encoder_t;

/* Public API functions for unified encoder */
void *qsv_encoder_create_internal(obs_data_t *settings, obs_encoder_t *encoder);
void qsv_encoder_destroy_internal(void *data);
bool qsv_encoder_encode_internal(void *data, struct encoder_frame *frame,
                                 struct encoder_packet *packet,
                                 bool *received_packet);
void qsv_encoder_get_video_info_internal(void *data,
                                         struct video_scale_info *info);
bool qsv_encoder_get_extra_data_internal(void *data, uint8_t **extra_data,
                                         size_t *size);

/* OBS encoder info structure */
extern struct obs_encoder_info qsv_encoder_info;

bool qsv_encoder_init(qsv_encoder_t *enc, obs_data_t *settings, video_t *video);
void qsv_encoder_destroy(qsv_encoder_t *enc);
bool qsv_encoder_encode(qsv_encoder_t *enc, struct encoder_frame *frame,
                        struct encoder_packet *packet, bool *received_packet);
void qsv_encoder_get_defaults(obs_data_t *settings);
obs_properties_t *qsv_encoder_properties(void *unused);
bool qsv_encoder_extra_data(void *data, uint8_t **extra_data, size_t *size);
void qsv_encoder_video_info(void *data, struct video_scale_info *info);

#endif // ENABLE_VPL

#endif // QSV_ENCODER_H
