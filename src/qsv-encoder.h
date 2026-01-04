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
  struct ntp_client ntp_client; // struct ntp_client is typedef'd as
                                // ntp_client_t but struct tag exists
  // Actually safer to use ntp_client_t
  uint64_t last_ntp_sync_time;
  ntp_timestamp_t current_ntp_time;
  bool ntp_enabled;

} qsv_encoder_t;

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
