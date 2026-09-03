/******************************************************************************
    SEI Receiver Source - Header
    Copyright (C) 2026

    OBS source plugin for receiving SRT streams with NTP-timestamped SEI
******************************************************************************/

#pragma once

#include "ntp-client.h"
#include "sei-handler.h"
#include <libavcodec/avcodec.h> /* AVPacket */
#include <obs-module.h>
#include <util/threading.h> /* OBS threading API */

#ifdef __cplusplus
extern "C" {
#endif

/* Sync buffer size */
#define MAX_FRAME_BUFFER 60 /* buffer at most 60 frames */

/* Frame sync state */
typedef enum {
  SYNC_STATE_WAITING,     /* waiting for the first frame */
  SYNC_STATE_BUFFERING,   /* buffering */
  SYNC_STATE_SYNCHRONIZED /* synchronised */
} sync_state_t;

/* Video frame data */
typedef struct video_frame_data {
  uint8_t *data;            /* frame data */
  size_t size;              /* data size */
  int64_t pts;              /* presentation timestamp */
  ntp_timestamp_t ntp_time; /* NTP timestamp, extracted from the SEI */
  bool has_ntp;             /* whether an NTP timestamp is present */
  uint32_t width;           /* video width */
  uint32_t height;          /* video height */
  enum video_format format; /* video format */
} video_frame_data_t;

/* Frame buffer */
typedef struct frame_buffer {
  video_frame_data_t frames[MAX_FRAME_BUFFER]; /* frame array */
  size_t count;                                /* current frame count */
  size_t read_index;                           /* read index */
  size_t write_index;                          /* write index */
  os_sem_t *semaphore;                         /* OBS semaphore */
} frame_buffer_t;

/* SEI receiver source data */
typedef struct sei_receiver_source {
  obs_source_t *context; /* OBS source context */

  /* SRT connection */
  char srt_url[256];           /* SRT URL (may carry ?streamid=xxx etc.) */
  bool is_connected;           /* whether connected */
  pthread_t receive_thread;    /* receive thread */
  volatile bool thread_active; /* thread-active flag */

  /* Video decode */
  void *format_context;       /* FFmpeg format context (demux) */
  void *decoder_context;      /* FFmpeg decoder context */
  void *codec_context;        /* FFmpeg codec context */
  struct SwsContext *sws_ctx; /* SwsScale context */
  int video_stream_index;     /* video stream index */
  enum video_format format;   /* output video format */
  uint32_t width;             /* video width */
  uint32_t height;            /* video height */

  /* Hardware decode */
  void *hw_device_ctx;      /* AVBufferRef* - hardware device context */
  char hw_decoder_type[32]; /* hardware decoder type (none/qsv/nvdec/amf) */
  bool hw_decode_enabled;   /* whether hardware decode is enabled */

  /* Codec format */
  char codec_format[16]; /* codec format the user chose (auto/h264/h265/av1) */
  char codec_type[16];   /* codec format actually in use (h264/h265/av1) */

  /* NTP sync */
  ntp_client_t ntp_client;         /* NTP client */
  bool ntp_enabled;                /* whether NTP is enabled */
  char ntp_server[128];            /* NTP server address */
  uint16_t ntp_port;               /* NTP server port */
  uint64_t last_ntp_sync_time;     /* local time of the last NTP sync (ns) */
  uint32_t ntp_drift_threshold_ms; /* NTP drift threshold (ms) */
  uint32_t ntp_sync_interval_ms;   /* minimum NTP sync interval (ms) */

  /* Frame sync */
  frame_buffer_t frame_buffer; /* frame buffer */
  sync_state_t sync_state;     /* sync state */
  int64_t time_offset_ns;      /* time offset (ns) */
  uint64_t first_ntp_time;     /* NTP time of the first frame */
  uint64_t first_local_time;   /* local time of the first frame */

  /* PTS sync */
  int64_t pts_offset;  /* PTS to SystemTime offset */
  bool has_pts_offset; /* whether the offset has been computed */

  /* Audio decode */
  void *audio_codec_context;      /* FFmpeg audio codec context */
  int audio_stream_index;         /* audio stream index */
  enum audio_format audio_format; /* OBS audio format */
  uint32_t audio_channels;        /* channel count */
  uint32_t audio_sample_rate;     /* sample rate */

  /* Statistics */
  uint64_t frames_received;       /* total frames received */
  uint64_t frames_rendered;       /* total frames rendered */
  uint64_t frames_dropped;        /* frames dropped */
  uint64_t sei_found_count;       /* frames where an SEI was found */
  uint64_t last_sync_frame_count; /* frame count at the last sync */

  /* Live statistics */
  uint64_t last_stats_update_time; /* last stats update (ns) */
  uint64_t stats_frame_count;      /* frames in the current stats window */
  float current_fps;               /* current frame rate */
  float sei_detection_rate;        /* SEI detection rate (%) */

  /* Error recovery */
  uint32_t decode_error_count;     /* consecutive decode errors */
  uint32_t decode_error_threshold; /* error threshold; reset once exceeded */

} sei_receiver_source_t;

/* Source plugin info */
extern struct obs_source_info sei_receiver_source_info;

/* Core functions */

/**
 * Initialise the frame buffer
 */
bool frame_buffer_init(frame_buffer_t *buffer);

/**
 * Destroy the frame buffer
 */
void frame_buffer_destroy(frame_buffer_t *buffer);

/**
 * Add a frame to the buffer
 */
bool frame_buffer_push(frame_buffer_t *buffer, video_frame_data_t *frame);

/**
 * Take a frame from the buffer
 */
bool frame_buffer_pop(frame_buffer_t *buffer, video_frame_data_t *frame);

/**
 * Get the buffer size
 */
size_t frame_buffer_size(frame_buffer_t *buffer);

/**
 * SRT receive thread
 */
void *srt_receive_thread(void *data);

/**
 * Decode and extract the SEI
 */
bool decode_and_extract_sei(sei_receiver_source_t *source, AVPacket *packet,
                            video_frame_data_t *frame_out);

/**
 * Compute a frame's presentation time
 */
int64_t calculate_display_time(sei_receiver_source_t *source,
                               video_frame_data_t *frame);

#ifdef __cplusplus
}
#endif
