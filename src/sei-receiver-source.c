/******************************************************************************
    SEI Receiver Source - Implementation
    Copyright (C) 2026

    Receives SRT streams, decodes video, extracts NTP SEI and synchronizes
******************************************************************************/

#include "sei-receiver-source.h"
#include <media-io/video-io.h>
#include <obs-module.h>
#include <util/platform.h>
#include <util/threading.h>

/* FFmpeg Headers */
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>

/* SRT Headers */
#ifdef _WIN32
#include <srt/srt.h>
#else
#include <srt.h>
#endif

/* Logging macros */
#define receiver_log(level, src, format, ...)                                  \
  blog(level, "[SEI Receiver: '%s'] " format,                                  \
       obs_source_get_name(src->context), ##__VA_ARGS__)

/* SRT receive buffer size */
#define SRT_BUFFER_SIZE (1024 * 1024) /* 1MB */

/*============================================================================
 * Frame buffer management
 *============================================================================*/

/* Initialise the frame buffer */
bool frame_buffer_init(frame_buffer_t *buffer) {
  if (!buffer) {
    return false;
  }

  memset(buffer, 0, sizeof(frame_buffer_t));
  if (os_sem_init(&buffer->semaphore, 1) != 0)
    return false;

  return true;
}

/* Destroy the frame buffer */
void frame_buffer_destroy(frame_buffer_t *buffer) {
  if (!buffer) {
    return;
  }

  os_sem_wait(buffer->semaphore);

  /* Free every frame's data */
  for (size_t i = 0; i < buffer->count; i++) {
    if (buffer->frames[i].data) {
      bfree(buffer->frames[i].data);
      buffer->frames[i].data = NULL;
    }
  }

  buffer->count = 0;
  buffer->read_index = 0;
  buffer->write_index = 0;

  os_sem_post(buffer->semaphore);
  os_sem_destroy(buffer->semaphore);
}

/* Add a frame to the buffer */
bool frame_buffer_push(frame_buffer_t *buffer, video_frame_data_t *frame) {
  if (!buffer || !frame) {
    return false;
  }

  os_sem_wait(buffer->semaphore);

  /* Check whether it is full */
  if (buffer->count >= MAX_FRAME_BUFFER) {
    os_sem_post(buffer->semaphore);
    return false;
  }

  /* Copy the frame data */
  size_t write_idx = buffer->write_index;
  buffer->frames[write_idx] = *frame;

  /* Copy the raw data */
  if (frame->data && frame->size > 0) {
    buffer->frames[write_idx].data = bmalloc(frame->size);
    memcpy(buffer->frames[write_idx].data, frame->data, frame->size);
  }

  /* Update the indices */
  buffer->write_index = (buffer->write_index + 1) % MAX_FRAME_BUFFER;
  buffer->count++;

  os_sem_post(buffer->semaphore);

  return true;
}

/* Take a frame from the buffer */
bool frame_buffer_pop(frame_buffer_t *buffer, video_frame_data_t *frame) {
  if (!buffer || !frame) {
    return false;
  }

  os_sem_wait(buffer->semaphore);

  /* Check whether it is empty */
  if (buffer->count == 0) {
    os_sem_post(buffer->semaphore);
    return false;
  }

  /* Take the frame */
  size_t read_idx = buffer->read_index;
  *frame = buffer->frames[read_idx];

  /* Clear the source pointer (the caller frees) */
  buffer->frames[read_idx].data = NULL;

  /* Update the indices */
  buffer->read_index = (buffer->read_index + 1) % MAX_FRAME_BUFFER;
  buffer->count--;

  os_sem_post(buffer->semaphore);

  return true;
}

/* Get the buffer size */
size_t frame_buffer_size(frame_buffer_t *buffer) {
  if (!buffer) {
    return 0;
  }

  os_sem_wait(buffer->semaphore);
  size_t count = buffer->count;
  os_sem_post(buffer->semaphore);

  return count;
}

/*============================================================================
 * Stats updates and error recovery
 *============================================================================*/

/* Forward declarations */
static enum AVPixelFormat get_hw_format(AVCodecContext *ctx,
                                        const enum AVPixelFormat *pix_fmts);

/* Update the live statistics */
static void update_statistics(sei_receiver_source_t *source) {
  uint64_t current_time = os_gettime_ns();

  /* Update the stats once a second */
  if (source->last_stats_update_time == 0 ||
      (current_time - source->last_stats_update_time) >= 1000000000ULL) {

    if (source->last_stats_update_time > 0) {
      /* Compute the frame rate */
      uint64_t frames_in_period =
          source->frames_rendered - source->stats_frame_count;
      double time_elapsed =
          (current_time - source->last_stats_update_time) / 1000000000.0;
      source->current_fps = (float)(frames_in_period / time_elapsed);

      /* Compute the SEI detection rate */
      if (source->frames_rendered > 0) {
        source->sei_detection_rate =
            (float)(source->sei_found_count * 100.0 / source->frames_rendered);
      }
    }

    source->last_stats_update_time = current_time;
    source->stats_frame_count = source->frames_rendered;
  }
}

/* Decoder reset (for error recovery) */
static bool reset_decoder(sei_receiver_source_t *source) {
  receiver_log(LOG_WARNING, source, "Resetting decoder due to errors...");

  /* Close the existing decoder */
  if (source->codec_context) {
    avcodec_free_context((AVCodecContext **)&source->codec_context);
    source->codec_context = NULL;
  }

  if (source->format_context && source->video_stream_index >= 0) {
    AVFormatContext *fmt_ctx = (AVFormatContext *)source->format_context;
    AVStream *vstream = fmt_ctx->streams[source->video_stream_index];

    /* Recreate the decoder */
    const AVCodec *codec = avcodec_find_decoder(vstream->codecpar->codec_id);
    if (!codec) {
      receiver_log(LOG_ERROR, source, "Failed to find decoder for reset");
      return false;
    }

    AVCodecContext *cctx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(cctx, vstream->codecpar);

    /* Reconfigure hardware decode */
    if (source->hw_decode_enabled && source->hw_device_ctx) {
      cctx->hw_device_ctx = av_buffer_ref((AVBufferRef *)source->hw_device_ctx);
      cctx->get_format = get_hw_format;
    }

    if (avcodec_open2(cctx, codec, NULL) < 0) {
      receiver_log(LOG_ERROR, source, "Failed to reopen codec");
      avcodec_free_context(&cctx);
      return false;
    }

    source->codec_context = cctx;
    source->decode_error_count = 0;
    receiver_log(LOG_INFO, source, "Decoder reset successful");
    return true;
  }

  return false;
}

/*============================================================================
 * Hardware decode support
 *============================================================================*/

/* Hardware pixel format callback */
static enum AVPixelFormat get_hw_format(AVCodecContext *ctx,
                                        const enum AVPixelFormat *pix_fmts) {
  const enum AVPixelFormat *p;

  /* Prefer the hardware format */
  for (p = pix_fmts; *p != AV_PIX_FMT_NONE; p++) {
    switch (*p) {
    case AV_PIX_FMT_QSV:
    case AV_PIX_FMT_CUDA:
    case AV_PIX_FMT_D3D11:
      return *p;
    default:
      break;
    }
  }
  return AV_PIX_FMT_NONE;
}

/* Initialise the hardware device context */
static bool init_hw_device(sei_receiver_source_t *source) {
  if (!source || !source->hw_decode_enabled)
    return false;

  enum AVHWDeviceType hw_type = AV_HWDEVICE_TYPE_NONE;

  /* Pick the hardware type from the configuration */
  if (strcmp(source->hw_decoder_type, "qsv") == 0) {
    hw_type = AV_HWDEVICE_TYPE_QSV;
  } else if (strcmp(source->hw_decoder_type, "nvdec") == 0) {
    hw_type = AV_HWDEVICE_TYPE_CUDA;
  } else if (strcmp(source->hw_decoder_type, "amf") == 0) {
    hw_type = AV_HWDEVICE_TYPE_D3D11VA;
  }

  if (hw_type == AV_HWDEVICE_TYPE_NONE)
    return false;

  AVBufferRef *hw_device_ctx = NULL;
  int ret = av_hwdevice_ctx_create(&hw_device_ctx, hw_type, NULL, NULL, 0);
  if (ret < 0) {
    receiver_log(LOG_WARNING, source,
                 "Failed to create HW device (%s), falling back to SW decode",
                 source->hw_decoder_type);
    source->hw_decode_enabled = false;
    return false;
  }

  source->hw_device_ctx = hw_device_ctx;
  receiver_log(LOG_INFO, source, "Hardware decoder initialized: %s",
               source->hw_decoder_type);
  return true;
}

/*============================================================================
 * Video decode and SEI extraction
 *============================================================================*/

/* Decode and extract the SEI */
bool decode_and_extract_sei(sei_receiver_source_t *source, AVPacket *packet,
                            video_frame_data_t *frame_out) {
  if (!source || !packet || !frame_out) {
    return false;
  }

  AVCodecContext *codec_ctx = (AVCodecContext *)source->codec_context;
  if (!codec_ctx) {
    return false;
  }

  /* Send the packet to the decoder */
  static uint64_t send_count = 0;
  send_count++;

  int ret = avcodec_send_packet(codec_ctx, packet);
  if (ret < 0) {
    receiver_log(LOG_ERROR, source, "Failed to send packet to decoder: %d (%s)",
                 ret, av_err2str(ret));
    return false;
  }

  if (send_count % 30 == 1) {
    receiver_log(LOG_INFO, source, "Packet sent to decoder: #%llu", send_count);
  }

  /* Receive the decoded frame */
  AVFrame *av_frame = av_frame_alloc();
  if (!av_frame) {
    return false;
  }

  ret = avcodec_receive_frame(codec_ctx, av_frame);
  if (ret < 0) {
    av_frame_free(&av_frame);

    /* EAGAIN means the decoder wants more input, which is normal */
    if (ret == AVERROR(EAGAIN)) {
      static uint64_t eagain_count = 0;
      eagain_count++;
      if (eagain_count % 100 == 1) {
        receiver_log(LOG_DEBUG, source,
                     "Decoder needs more data (EAGAIN count: %llu)",
                     eagain_count);
      }
      return true; /* packet consumed, no frame out yet */
    }

    /* EOF is a normal end too */
    if (ret == AVERROR_EOF) {
      receiver_log(LOG_INFO, source, "Decoder EOF");
      return false;
    }

    /* anything else is a real problem */
    receiver_log(LOG_ERROR, source, "Video decode failed: %d (%s)", ret,
                 av_err2str(ret));

    /* Error recovery: bump the error count */
    source->decode_error_count++;
    if (source->decode_error_count >= source->decode_error_threshold) {
      receiver_log(LOG_WARNING, source,
                   "Decoder error threshold reached (%u), attempting reset",
                   source->decode_error_count);
      reset_decoder(source);
    }
    return false;
  }

  static uint64_t video_frame_count = 0;
  video_frame_count++;
  if (video_frame_count % 30 == 1) {
    receiver_log(
        LOG_INFO, source, "Video frame decoded: #%llu, format=%d, size=%dx%d",
        video_frame_count, av_frame->format, av_frame->width, av_frame->height);
  }

  /* Decode succeeded; reset the error count */
  source->decode_error_count = 0;

  /* Hardware frames: copy to system memory */
  if (av_frame->format == AV_PIX_FMT_QSV ||
      av_frame->format == AV_PIX_FMT_CUDA ||
      av_frame->format == AV_PIX_FMT_D3D11) {
    AVFrame *sw_frame = av_frame_alloc();
    if (!sw_frame) {
      av_frame_free(&av_frame);
      return false;
    }

    /* hardware frame -> system memory */
    ret = av_hwframe_transfer_data(sw_frame, av_frame, 0);
    if (ret < 0) {
      receiver_log(LOG_ERROR, source, "Failed to transfer HW frame to SW: %d",
                   ret);
      av_frame_free(&sw_frame);
      av_frame_free(&av_frame);
      return false;
    }

    sw_frame->pts = av_frame->pts;
    av_frame_free(&av_frame);
    av_frame = sw_frame;
  }

  /* Take the PTS from the AVFrame (FFmpeg converted it from the packet) */
  int64_t pts = av_frame->pts;
  if (pts == AV_NOPTS_VALUE) {
    /* With no PTS, use the current time */
    pts = os_gettime_ns();
  } else {
    /* Convert the PTS from the stream timebase to nanoseconds */
    AVRational time_base = {1, 90000}; // MPEG-TS normally uses 90 kHz
    pts = av_rescale_q(pts, time_base, (AVRational){1, 1000000000});
  }

  /* Fill in the frame info */
  frame_out->width = av_frame->width;
  frame_out->height = av_frame->height;
  frame_out->pts = av_frame->pts;
  frame_out->format = VIDEO_FORMAT_I420; /* YUV420P by default */

  /* Compute the frame size and allocate (align 32 for OBS) */
  /* BGRA Output */
  int frame_size = av_image_get_buffer_size(AV_PIX_FMT_BGRA, av_frame->width,
                                            av_frame->height, 32);
  frame_out->data = bmalloc(frame_size);
  frame_out->size = frame_size;

  /* Format conversion (SwsScale to BGRA) */
  if (!source->sws_ctx || av_frame->width != source->width ||
      av_frame->height != source->height ||
      av_frame->format != ((AVCodecContext *)source->codec_context)->pix_fmt) {

    if (source->sws_ctx)
      sws_freeContext(source->sws_ctx);

    source->sws_ctx = sws_getContext(
        av_frame->width, av_frame->height, av_frame->format, av_frame->width,
        av_frame->height, AV_PIX_FMT_BGRA, SWS_BILINEAR, NULL, NULL, NULL);

    source->width = av_frame->width;
    source->height = av_frame->height;
    ((AVCodecContext *)source->codec_context)->pix_fmt = av_frame->format;

    if (!source->sws_ctx) {
      receiver_log(LOG_ERROR, source, "Failed to initialize SwsContext");
      av_frame_free(&av_frame);
      bfree(frame_out->data);
      frame_out->data = NULL;
      return false;
    }
  }

  /* Copy the image data */
  uint8_t *dest[4] = {0};
  int linesize[4] = {0};

  av_image_fill_arrays(dest, linesize, frame_out->data, AV_PIX_FMT_BGRA,
                       av_frame->width, av_frame->height, 32);

  sws_scale(source->sws_ctx, (const uint8_t *const *)av_frame->data,
            av_frame->linesize, 0, av_frame->height, dest, linesize);

  /* Try to pull the SEI out of the side data */
  frame_out->has_ntp = false;

  AVFrameSideData *sei_data =
      av_frame_get_side_data(av_frame, AV_FRAME_DATA_SEI_UNREGISTERED);

  if (sei_data) {
    /* Parse the NTP SEI */
    ntp_sei_data_t ntp_data;
    if (parse_ntp_sei(sei_data->data, sei_data->size, &ntp_data)) {
      frame_out->ntp_time = ntp_data.ntp_time;
      frame_out->has_ntp = true;
      source->sei_found_count++;

      receiver_log(LOG_DEBUG, source,
                   "Extracted NTP SEI: seconds=%u, fraction=%u",
                   ntp_data.ntp_time.seconds, ntp_data.ntp_time.fraction);
    }
  } else {
    /* If the side data has none, look in the raw data */
    const uint8_t *payload = NULL;
    size_t payload_size = 0;

    if (extract_sei_payload(packet->data, packet->size, &payload,
                            &payload_size)) {
      ntp_sei_data_t ntp_data;
      if (parse_ntp_sei(payload, payload_size, &ntp_data)) {
        frame_out->ntp_time = ntp_data.ntp_time;
        frame_out->has_ntp = true;
        source->sei_found_count++;
      }
    }
  }

  /* NTP sync policy:
   * 1. Sync on a keyframe (IDR) that carries an SEI timestamp
   * 2. Sync when local time differs from NTP time by over 50 ms
   */
  if (frame_out->has_ntp && source->ntp_enabled) {
    bool should_sync = false;
    uint64_t now = os_gettime_ns();

    /* Condition 1: keyframe sync, with a 10 s minimum gap */
    bool is_keyframe = (av_frame->flags & AV_FRAME_FLAG_KEY) != 0;

    /* Time since the last sync*/
    uint64_t time_since_last_sync = 0;
    if (source->last_ntp_sync_time > 0) {
      time_since_last_sync = now - source->last_ntp_sync_time;
    }

    /* Use the configured minimum sync interval */
    uint64_t min_interval_ns =
        (uint64_t)source->ntp_sync_interval_ms * 1000000ULL;

    if (is_keyframe && time_since_last_sync >= min_interval_ns) {
      should_sync = true;
      receiver_log(LOG_DEBUG, source,
                   "Keyframe + interval met, triggering NTP sync");
    }

    /* Condition 2: drift detection, also bounded by the minimum gap */
    if (!should_sync && source->ntp_client.is_synced &&
        time_since_last_sync >= min_interval_ns) {
      /* Nanoseconds for this frame's NTP timestamp */
      uint64_t frame_ntp_ns =
          ((uint64_t)frame_out->ntp_time.seconds * 1000000000ULL) +
          (((uint64_t)frame_out->ntp_time.fraction * 1000000000ULL) >> 32);

      /* NTP time for the current local time */
      ntp_timestamp_t current_ntp;
      if (ntp_client_get_time(&source->ntp_client, &current_ntp)) {
        uint64_t current_ntp_ns =
            ((uint64_t)current_ntp.seconds * 1000000000ULL) +
            (((uint64_t)current_ntp.fraction * 1000000000ULL) >> 32);

        /* Compute the difference */
        int64_t time_diff = (int64_t)(frame_ntp_ns - current_ntp_ns);
        if (time_diff < 0)
          time_diff = -time_diff; // absolute value

        /* Use the configured drift threshold */
        uint64_t drift_threshold_ns =
            (uint64_t)source->ntp_drift_threshold_ms * 1000000ULL;
        if (time_diff > (int64_t)drift_threshold_ns) {
          should_sync = true;
          receiver_log(LOG_DEBUG, source,
                       "Time drift detected: %lld ms, triggering NTP sync",
                       time_diff / 1000000);
        }
      }
    }

    /* Perform the NTP sync */
    if (should_sync) {
      /* Update the time either way: a network fault must not retry every frame
       */
      source->last_ntp_sync_time = now;

      if (ntp_client_sync(&source->ntp_client)) {
        receiver_log(LOG_INFO, source, "NTP synchronized (syncs: %u)",
                     source->ntp_client.sync_count);
      } else {
        receiver_log(LOG_WARNING, source, "NTP sync failed");
      }
    }
  }

  /* Output to OBS */
  struct obs_source_frame obs_frame = {0};

  obs_frame.data[0] = dest[0];
  obs_frame.linesize[0] = linesize[0];

  obs_frame.width = frame_out->width;
  obs_frame.height = frame_out->height;
  obs_frame.format = VIDEO_FORMAT_BGRA;

  /* Calculate correct Sync Timestamp */
  obs_frame.timestamp = calculate_display_time(source, frame_out);

  /* Debug log: confirm the timestamp */
  receiver_log(
      LOG_DEBUG, source, "Video Decoded: %dx%d, PTS_IN=%lld, TS_OUT=%lld",
      obs_frame.width, obs_frame.height, packet->pts, obs_frame.timestamp);

  /* Push the frame to OBS */
  obs_source_output_video(source->context, &obs_frame);

  /* Update the statistics */
  update_statistics(source);

  /* Free the AVFrame */
  av_frame_free(&av_frame);

  /*
   * OBS outputs frames synchronously (copying data) by default
   * So we should free our buffer here.
   */
  if (frame_out->data) {
    bfree(frame_out->data);
    frame_out->data = NULL;
  }

  return true;
}

/* Synchronised timestamp (PTS -> SystemTime) */
int64_t get_sync_timestamp(sei_receiver_source_t *source, int64_t pts) {
  if (!source)
    return 0;

  int64_t current_time = os_gettime_ns();

  /* PTS to ns: FFmpeg PTS is timebase-based; assume 90 kHz (SRT/MPEG-TS)
   */
  /* Take care if the PTS is already in ns (e.g. after AV_NOPTS_VALUE) */
  /* callers normally pass an already-converted ns PTS */

  if (!source->has_pts_offset) {
    source->pts_offset = current_time - pts;
    source->has_pts_offset = true;

    receiver_log(LOG_INFO, source,
                 "Initialized Sync Offset: PTS=%lld, Local=%lld, Offset=%lld",
                 pts, current_time, source->pts_offset);
  }

  return pts + source->pts_offset;
}

/* Compute the video presentation time */
int64_t calculate_display_time(sei_receiver_source_t *source,
                               video_frame_data_t *frame) {
  if (!source || !frame) {
    return 0;
  }

  /* With an NTP timestamp, and NTP sync enabled */
  if (frame->has_ntp && source->ntp_enabled) {
    /* Nanoseconds for the NTP timestamp */
    uint64_t ntp_ns =
        ((uint64_t)frame->ntp_time.seconds * 1000000000ULL) +
        (((uint64_t)frame->ntp_time.fraction * 1000000000ULL) >> 32);

    /* NTP mode: absolute-time sync */
    /* Relies on the NTP client's global offset, not first-frame alignment */
    /* Offset = NTP_Server - Local_System */
    /* Local_Render_Time = Frame_NTP - Offset */

    int64_t ntp_offset = ntp_client_get_offset(&source->ntp_client);
    int64_t display_time = (int64_t)ntp_ns - ntp_offset;

    /* Log periodically only, to avoid flooding */
    if (source->sei_found_count % 300 == 0) {
      receiver_log(LOG_DEBUG, source,
                   "Absolute Sync: NTP=%llu, Offset=%lld, Display=%lld", ntp_ns,
                   ntp_offset, display_time);
    }

    /* Update the shared PTS offset so audio follows the video's pacing */
    /* this matters because audio may still be using PTS */
    /* AudioTarget = VideoDisplayTime */
    /* VideoPTS + PTSOffset = VideoDisplayTime */
    /* PTSOffset = VideoDisplayTime - VideoPTS */

    /* Update the PTS offset on every SEI (keyframe/IDR) */
    // if (source->sei_found_count % 1 == 0) // Always
    source->pts_offset = display_time - frame->pts;
    source->has_pts_offset = true;

    return display_time;
  }

  /* No NTP, or disabled: fall back to plain PTS sync */
  return get_sync_timestamp(source, frame->pts);
}

/* Decode audio */
bool decode_audio(sei_receiver_source_t *source, AVPacket *packet) {
  if (!source || !packet || !source->audio_codec_context)
    return false;

  AVCodecContext *ctx = (AVCodecContext *)source->audio_codec_context;
  int ret = avcodec_send_packet(ctx, packet);
  if (ret < 0) {
    receiver_log(LOG_ERROR, source, "Error sending audio packet: %d", ret);
    return false;
  }

  AVFrame *frame = av_frame_alloc();
  if (!frame)
    return false;

  bool success = true;
  while (ret >= 0) {
    ret = avcodec_receive_frame(ctx, frame);
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
      break;
    } else if (ret < 0) {
      receiver_log(LOG_ERROR, source, "Error receiving audio frame: %d", ret);
      success = false;
      break;
    }

    struct obs_source_audio audio = {0};
    int channels = frame->ch_layout.nb_channels;
    for (int i = 0; i < MAX_AUDIO_CHANNELS && i < channels; i++) {
      audio.data[i] = frame->data[i];
    }

    audio.frames = frame->nb_samples;

    if (channels == 1)
      audio.speakers = SPEAKERS_MONO;
    else if (channels == 2)
      audio.speakers = SPEAKERS_STEREO;
    else if (channels == 4)
      audio.speakers = SPEAKERS_4POINT0;
    else if (channels == 6)
      audio.speakers = SPEAKERS_5POINT1;
    else
      audio.speakers = SPEAKERS_UNKNOWN;

    audio.format = source->audio_format;
    audio.samples_per_sec = source->audio_sample_rate;

    /* Timestamp sync */
    /* Get the audio PTS (timebase-based) */
    int64_t pts = frame->pts;
    int64_t pts_ns = 0;

    if (pts != AV_NOPTS_VALUE) {
      AVRational tb = ctx->time_base;
      /* If the timebase is 0, try 1/sample_rate */
      if (tb.num == 0 || tb.den == 0) {
        tb.num = 1;
        tb.den = ctx->sample_rate;
      }
      pts_ns = av_rescale_q(pts, tb, (AVRational){1, 1000000000});
    } else {
      /* With no PTS, derive it from the current offset */
      pts_ns = (int64_t)audio.timestamp - source->pts_offset;
      if (!source->has_pts_offset)
        pts_ns = 0;
    }

    /* Use the shared timestamp sync */
    audio.timestamp = (uint64_t)get_sync_timestamp(source, pts_ns);

    obs_source_output_audio(source->context, &audio);
  }

  av_frame_free(&frame);
  return success;
}

/*============================================================================
 * SRT receive thread
 *============================================================================*/

/* SRT receive thread */

/*============================================================================
 * OBS source callbacks
 *============================================================================*/

/* Source name */
static const char *receiver_source_getname(void *unused) {
  UNUSED_PARAMETER(unused);
  return obs_module_text("SEIReceiver");
}

static void start_receiver(void *data);
static void stop_receiver(void *data);

/* Create the source */
static void *receiver_source_create(obs_data_t *settings,
                                    obs_source_t *source) {
  sei_receiver_source_t *ctx = bzalloc(sizeof(sei_receiver_source_t));
  ctx->context = source;

  /* Initialise the frame buffer */
  if (!frame_buffer_init(&ctx->frame_buffer)) {
    bfree(ctx);
    return NULL;
  }

  /* Initialise the sync state */
  ctx->sync_state = SYNC_STATE_WAITING;
  ctx->has_pts_offset = false;
  ctx->pts_offset = 0;

  /* Load the configuration from settings */
  const char *srt_url = obs_data_get_string(settings, "srt_url");
  if (srt_url && srt_url[0]) {
    strncpy(ctx->srt_url, srt_url, sizeof(ctx->srt_url) - 1);
  }

  /* Hardware decoder settings */
  const char *hw_decoder = obs_data_get_string(settings, "hw_decoder");
  if (hw_decoder && hw_decoder[0]) {
    strncpy(ctx->hw_decoder_type, hw_decoder, sizeof(ctx->hw_decoder_type) - 1);
    ctx->hw_decode_enabled = (strcmp(hw_decoder, "none") != 0);
  } else {
    strcpy(ctx->hw_decoder_type, "none");
    ctx->hw_decode_enabled = false;
  }

  /* Codec format settings (a manual choice wins) */
  const char *codec_format = obs_data_get_string(settings, "codec_format");
  if (codec_format && codec_format[0]) {
    strncpy(ctx->codec_format, codec_format, sizeof(ctx->codec_format) - 1);
  } else {
    strcpy(ctx->codec_format, "auto"); // auto-detect by default
  }

  const char *ntp_server = obs_data_get_string(settings, "ntp_server");
  if (ntp_server && ntp_server[0]) {
    strncpy(ctx->ntp_server, ntp_server, sizeof(ctx->ntp_server) - 1);
  } else {
    strcpy(ctx->ntp_server, "time.windows.com");
  }

  ctx->ntp_port = (uint16_t)obs_data_get_int(settings, "ntp_port");
  if (ctx->ntp_port == 0) {
    ctx->ntp_port = 123;
  }

  ctx->ntp_enabled = obs_data_get_bool(settings, "ntp_enabled");
  ctx->ntp_drift_threshold_ms =
      (uint32_t)obs_data_get_int(settings, "ntp_drift_threshold");
  if (ctx->ntp_drift_threshold_ms == 0) {
    ctx->ntp_drift_threshold_ms = 50; // default 50 ms
  }

  ctx->ntp_sync_interval_ms =
      (uint32_t)obs_data_get_int(settings, "ntp_sync_interval");
  if (ctx->ntp_sync_interval_ms == 0) {
    ctx->ntp_sync_interval_ms = 10000; // default 10 s, to be safe
  }

  /* Initialise statistics and error recovery */
  ctx->last_stats_update_time = 0;
  ctx->stats_frame_count = 0;
  ctx->current_fps = 0.0f;
  ctx->sei_detection_rate = 0.0f;
  ctx->decode_error_count = 0;
  ctx->decode_error_threshold = 10; /* reset after 10 consecutive errors */

  /* Initialise the NTP client */
  if (ctx->ntp_enabled) {
    if (ntp_client_init(&ctx->ntp_client, ctx->ntp_server, ctx->ntp_port)) {
      receiver_log(LOG_INFO, ctx, "NTP client initialized");

      /* Do the initial sync */
      if (ntp_client_sync(&ctx->ntp_client)) {
        ctx->last_ntp_sync_time = os_gettime_ns();
        receiver_log(LOG_INFO, ctx, "Initial NTP sync successful");
      }
    }
  }

  receiver_log(LOG_INFO, ctx, "SEI Receiver source created");

  /* Start immediately in the background */
  start_receiver(ctx);

  return ctx;
}

/* Destroy the source */
static void receiver_source_destroy(void *data) {
  sei_receiver_source_t *ctx = (sei_receiver_source_t *)data;

  /* Stop the receiver */
  stop_receiver(ctx);

  /* avformat owns the SRT connection; no manual socket close */

  /* Destroy the decoder */
  if (ctx->codec_context) {
    AVCodecContext *codec_ctx = (AVCodecContext *)ctx->codec_context;
    avcodec_free_context(&codec_ctx);
  }

  /* Destroy the audio decoder */
  if (ctx->audio_codec_context) {
    AVCodecContext *actx = (AVCodecContext *)ctx->audio_codec_context;
    avcodec_free_context(&actx);
    ctx->audio_codec_context = NULL;
  }

  /* Destroy the NTP client */
  ntp_client_destroy(&ctx->ntp_client);

  /* Destroy the frame buffer */
  frame_buffer_destroy(&ctx->frame_buffer);

  /* Free the SwsContext */
  if (ctx->sws_ctx) {
    sws_freeContext(ctx->sws_ctx);
    ctx->sws_ctx = NULL;
  }

  receiver_log(LOG_INFO, ctx,
               "SEI Receiver destroyed (received: %llu, rendered: %llu, "
               "dropped: %llu, SEI found: %llu)",
               ctx->frames_received, ctx->frames_rendered, ctx->frames_dropped,
               ctx->sei_found_count);

  bfree(ctx);
}

/* Defaults */
static void receiver_source_defaults(obs_data_t *settings) {
  obs_data_set_default_string(settings, "srt_url", "srt://127.0.0.1:9000");
  obs_data_set_default_string(settings, "ntp_server", "time.windows.com");
  obs_data_set_default_int(settings, "ntp_port", 123);
  obs_data_set_default_bool(settings, "ntp_enabled", true);
  obs_data_set_default_string(settings, "hw_decoder", "none");
  obs_data_set_default_string(settings, "codec_format", "auto"); // auto-detect
  obs_data_set_default_int(settings, "ntp_drift_threshold", 50); // 50 ms
  obs_data_set_default_int(settings, "ntp_sync_interval",
                           10000); // default 10000 ms (10 s)
}

/* Properties */
static obs_properties_t *receiver_source_properties(void *data) {
  UNUSED_PARAMETER(data);

  obs_properties_t *props = obs_properties_create();

  /* SRT URL */
  obs_properties_add_text(props, "srt_url", obs_module_text("SRTUrl"),
                          OBS_TEXT_DEFAULT);

  /* Hardware decoder choice */
  obs_property_t *hw_list =
      obs_properties_add_list(props, "hw_decoder", obs_module_text("HWDecoder"),
                              OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
  obs_property_list_add_string(hw_list, obs_module_text("HWDecoder.None"),
                               "none");
  obs_property_list_add_string(hw_list, obs_module_text("HWDecoder.QSV"),
                               "qsv");
  obs_property_list_add_string(hw_list, obs_module_text("HWDecoder.NVDEC"),
                               "nvdec");
  obs_property_list_add_string(hw_list, obs_module_text("HWDecoder.AMF"),
                               "amf");

  /* Codec format choice (manual beats auto-detect) */
  obs_property_t *codec_list = obs_properties_add_list(
      props, "codec_format", obs_module_text("CodecFormat"),
      OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
  obs_property_list_add_string(codec_list, obs_module_text("CodecFormat.Auto"),
                               "auto");
  obs_property_list_add_string(codec_list, "H.264", "h264");
  obs_property_list_add_string(codec_list, "H.265 (HEVC)", "h265");
  obs_property_list_add_string(codec_list, "AV1", "av1");

  /* NTP settings group */
  obs_properties_add_group(props, "ntp_group", obs_module_text("NTPSettings"),
                           OBS_GROUP_NORMAL, NULL);

  /* NTP properties are added straight to props */
  obs_properties_add_bool(props, "ntp_enabled", obs_module_text("EnableNTP"));

  obs_properties_add_text(props, "ntp_server", obs_module_text("NTPServer"),
                          OBS_TEXT_DEFAULT);

  obs_properties_add_int(props, "ntp_port", obs_module_text("NTPPort"), 1,
                         65535, 1);

  obs_properties_add_int(props, "ntp_drift_threshold",
                         "NTP Drift Threshold (ms)", 10, 1000,
                         10); // 10 ms to 1000 ms

  obs_properties_add_int(props, "ntp_sync_interval", "NTP Sync Interval (ms)",
                         100, 3600000, 100); // 100 ms to 1 hour

  /* Warning text */
  obs_properties_add_text(props, "ntp_interval_warning",
                          "⚠️ Warning: Setting interval < 1000ms may cause "
                          "stuttering on slow networks.",
                          OBS_TEXT_INFO);

  /* Status info (read-only) */
  obs_properties_add_text(props, "status", obs_module_text("Status"),
                          OBS_TEXT_INFO);

  /* Version info */
  obs_properties_add_text(props, "plugin_version", "v1.3.0",
                          OBS_TEXT_INFO);

  return props;
}

/* Update settings */
static void receiver_source_update(void *data, obs_data_t *settings) {
  sei_receiver_source_t *ctx = (sei_receiver_source_t *)data;

  bool settings_changed = false;

  /* Update the SRT URL */
  const char *srt_url = obs_data_get_string(settings, "srt_url");
  if (srt_url && srt_url[0] && strcmp(ctx->srt_url, srt_url) != 0) {
    /* A changed URL means restarting the receiver */
    receiver_log(LOG_INFO, ctx, "SRT URL changed, restarting...");
    stop_receiver(ctx);
    strncpy(ctx->srt_url, srt_url, sizeof(ctx->srt_url) - 1);
    settings_changed = true;
  }

  /* Update the NTP settings */
  bool ntp_enabled = obs_data_get_bool(settings, "ntp_enabled");
  if (ntp_enabled != ctx->ntp_enabled) {
    ctx->ntp_enabled = ntp_enabled;

    if (ntp_enabled) {
      const char *ntp_server = obs_data_get_string(settings, "ntp_server");
      uint16_t ntp_port = (uint16_t)obs_data_get_int(settings, "ntp_port");

      if (ntp_client_init(&ctx->ntp_client, ntp_server, ntp_port)) {
        if (ntp_client_sync(&ctx->ntp_client)) {
          ctx->last_ntp_sync_time = os_gettime_ns();
          receiver_log(LOG_INFO, ctx, "NTP enabled and synchronized");
        }
      }
    } else {
      ntp_client_destroy(&ctx->ntp_client);
      receiver_log(LOG_INFO, ctx, "NTP disabled");
    }
  }

  /* Update the hardware decoder settings */
  const char *hw_decoder = obs_data_get_string(settings, "hw_decoder");
  if (hw_decoder && hw_decoder[0] &&
      strcmp(ctx->hw_decoder_type, hw_decoder) != 0) {
    /* A changed hardware decoder type means restarting the receiver */
    receiver_log(LOG_INFO, ctx,
                 "Hardware decoder changed from '%s' to '%s', restarting...",
                 ctx->hw_decoder_type, hw_decoder);

    /* Free the old hardware device context, if any */
    if (ctx->hw_device_ctx) {
      av_buffer_unref((AVBufferRef **)&ctx->hw_device_ctx);
      ctx->hw_device_ctx = NULL;
    }

    /* Stop the receiver */
    stop_receiver(ctx);

    /* Update the configuration */
    strncpy(ctx->hw_decoder_type, hw_decoder, sizeof(ctx->hw_decoder_type) - 1);
    ctx->hw_decode_enabled = (strcmp(hw_decoder, "none") != 0);

    settings_changed = true;
  }

  /* Update the codec format settings */
  const char *codec_format = obs_data_get_string(settings, "codec_format");
  if (codec_format && codec_format[0] &&
      strcmp(ctx->codec_format, codec_format) != 0) {
    /* A changed codec format means restarting the receiver */
    receiver_log(LOG_INFO, ctx,
                 "Codec format changed from '%s' to '%s', restarting...",
                 ctx->codec_format, codec_format);

    stop_receiver(ctx);
    strncpy(ctx->codec_format, codec_format, sizeof(ctx->codec_format) - 1);
    settings_changed = true;
  }

  /* If a settings change stopped it, restart now */
  if (settings_changed) {
    start_receiver(ctx);
  } else if (!ctx->is_connected) {
    /* Make sure it is always running */
    start_receiver(ctx);
  }

  receiver_log(LOG_INFO, ctx, "Settings updated");
}
/* Helper: tear down the connection's resources */
static void cleanup_connection(sei_receiver_source_t *source) {
  if (source->format_context) {
    avformat_close_input((AVFormatContext **)&source->format_context);
    source->format_context = NULL;
  }
  if (source->codec_context) {
    avcodec_free_context((AVCodecContext **)&source->codec_context);
    source->codec_context = NULL;
  }
  if (source->sws_ctx) {
    sws_freeContext(source->sws_ctx);
    source->sws_ctx = NULL;
  }
  if (source->audio_codec_context) {
    avcodec_free_context((AVCodecContext **)&source->audio_codec_context);
    source->audio_codec_context = NULL;
  }
  source->is_connected = false;
  receiver_log(LOG_INFO, source, "Connection closed and resources freed");
}

/* Helper: try to connect */
static bool try_connect(sei_receiver_source_t *source) {
  receiver_log(LOG_INFO, source, "Attempting to connect to: %s",
               source->srt_url);

  AVFormatContext *fmt_ctx = avformat_alloc_context();
  if (!fmt_ctx)
    return false;

  /* Set a timeout so it cannot block for long */
  AVDictionary *options = NULL;
  av_dict_set(&options, "timeout", "2000000", 0); /* 2 s timeout */

  /* Use the configured SRT URL as-is (it may carry ?streamid=xxx etc.) */
  if (avformat_open_input(&fmt_ctx, source->srt_url, NULL, &options) < 0) {
    receiver_log(LOG_WARNING, source,
                 "Failed to open input (sender might be offline)");
    avformat_free_context(fmt_ctx);
    if (options)
      av_dict_free(&options);
    return false;
  }
  if (options)
    av_dict_free(&options);

  if (avformat_find_stream_info(fmt_ctx, NULL) < 0) {
    receiver_log(LOG_ERROR, source, "Failed to find stream info");
    avformat_close_input(&fmt_ctx);
    return false;
  }

  /* Find the video stream */
  int video_idx = -1;
  for (unsigned int i = 0; i < fmt_ctx->nb_streams; i++) {
    if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
      video_idx = i;
      break;
    }
  }

  if (video_idx == -1) {
    receiver_log(LOG_ERROR, source, "No video stream found");
    avformat_close_input(&fmt_ctx);
    return false;
  }

  source->video_stream_index = video_idx;
  AVStream *vstream = fmt_ctx->streams[video_idx];

  /* Initialise the hardware device, if enabled */
  if (source->hw_decode_enabled) {
    init_hw_device(source);
  }

  /* Decide the codec: a manual choice beats auto-detect */
  enum AVCodecID codec_id;
  const char *codec_name;

  if (strcmp(source->codec_format, "auto") != 0) {
    /* The user picked a codec format */
    if (strcmp(source->codec_format, "h264") == 0) {
      codec_id = AV_CODEC_ID_H264;
    } else if (strcmp(source->codec_format, "h265") == 0 ||
               strcmp(source->codec_format, "hevc") == 0) {
      codec_id = AV_CODEC_ID_HEVC;
    } else if (strcmp(source->codec_format, "av1") == 0) {
      codec_id = AV_CODEC_ID_AV1;
    } else {
      /* Unrecognised format; fall back to auto-detect */
      codec_id = vstream->codecpar->codec_id;
    }
    codec_name = avcodec_get_name(codec_id);
    receiver_log(LOG_INFO, source, "Using manually selected codec: %s (ID: %d)",
                 codec_name, codec_id);
  } else {
    /* Auto-detect the stream's codec */
    codec_id = vstream->codecpar->codec_id;
    codec_name = avcodec_get_name(codec_id);
    receiver_log(LOG_INFO, source, "Auto-detected codec: %s (ID: %d)",
                 codec_name, codec_id);
  }

  /* Initialise the video decoder */
  const AVCodec *codec = avcodec_find_decoder(codec_id);
  if (!codec) {
    receiver_log(LOG_ERROR, source, "Video decoder not found for codec: %s",
                 codec_name);
    avformat_close_input(&fmt_ctx);
    return false;
  }

  AVCodecContext *cctx = avcodec_alloc_context3(codec);

  /* Copy stream parameters (resolution, profile) even for a manual codec */
  /* but use the stream's codec_id only when auto-detecting */
  if (strcmp(source->codec_format, "auto") == 0) {
    /* Auto-detect: take the stream's parameters wholesale */
    avcodec_parameters_to_context(cctx, vstream->codecpar);
  } else {
    /* Manual: keep the stream's parameters but replace the codec type */
    /* copy the stream's parameters first */
    avcodec_parameters_to_context(cctx, vstream->codecpar);

    /* then override codec_id with the user's choice */
    cctx->codec_id = codec_id;

    receiver_log(LOG_INFO, source,
                 "Applied stream parameters with manual codec override");
  }

  /* Configure hardware decode */
  if (source->hw_decode_enabled && source->hw_device_ctx) {
    cctx->hw_device_ctx = av_buffer_ref((AVBufferRef *)source->hw_device_ctx);
    cctx->get_format = get_hw_format;
    receiver_log(LOG_INFO, source, "Hardware decoder configured");
  }

  /* Check the extradata (VPS/SPS/PPS for H.265) */
  if (cctx->extradata && cctx->extradata_size > 0) {
    receiver_log(LOG_INFO, source, "Decoder has extradata: %d bytes",
                 cctx->extradata_size);
  } else {
    receiver_log(LOG_WARNING, source,
                 "Decoder has NO extradata - H.265 may fail to decode!");
  }

  if (avcodec_open2(cctx, codec, NULL) < 0) {
    receiver_log(LOG_ERROR, source, "Failed to open video codec");
    avcodec_free_context(&cctx);
    avformat_close_input(&fmt_ctx);
    return false;
  }

  source->format_context = fmt_ctx;
  source->codec_context = cctx;
  source->width = cctx->width;
  source->height = cctx->height;

  /* Find the audio stream (optional) */
  source->audio_stream_index = -1;
  int audio_idx = -1;
  for (unsigned int i = 0; i < fmt_ctx->nb_streams; i++) {
    if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
      audio_idx = i;
      break;
    }
  }

  if (audio_idx != -1) {
    AVStream *astream = fmt_ctx->streams[audio_idx];
    const AVCodec *acodec = avcodec_find_decoder(astream->codecpar->codec_id);
    if (acodec) {
      AVCodecContext *actx = avcodec_alloc_context3(acodec);
      avcodec_parameters_to_context(actx, astream->codecpar);
      if (avcodec_open2(actx, acodec, NULL) >= 0) {
        source->audio_codec_context = actx;
        source->audio_stream_index = audio_idx;
        /* Audio format mapping simplified for brevity, assume valid */
        source->audio_channels = actx->ch_layout.nb_channels;
        source->audio_sample_rate = actx->sample_rate;
        source->audio_format = AUDIO_FORMAT_FLOAT_PLANAR; /* Default safe */

        switch (actx->sample_fmt) {
        case AV_SAMPLE_FMT_S16:
          source->audio_format = AUDIO_FORMAT_16BIT;
          break;
        case AV_SAMPLE_FMT_S16P:
          source->audio_format = AUDIO_FORMAT_16BIT_PLANAR;
          break;
        case AV_SAMPLE_FMT_FLT:
          source->audio_format = AUDIO_FORMAT_FLOAT;
          break;
        default:
          break;
        }

        receiver_log(LOG_INFO, source, "Audio stream opened (idx: %d)",
                     audio_idx);
      } else {
        avcodec_free_context(&actx);
      }
    }
  }

  source->is_connected = true;
  receiver_log(LOG_INFO, source, "Connected successfully!");
  return true;
}

/* SRT receive thread (owns connection management and reads) */
static void *srt_receive_thread(void *data) {
  sei_receiver_source_t *source = (sei_receiver_source_t *)data;
  receiver_log(LOG_INFO, source, "Thread started (Auto-Reconnect Mode)");

  // struct os_timespec ts; // Removed unused variable
  // ts.tv_sec = 1;
  // ts.tv_nsec = 0;

  AVPacket *packet = av_packet_alloc();

  while (source->thread_active) {

    /* 1. Not connected: try to connect */
    if (!source->is_connected) {
      if (try_connect(source)) {
        /* connected, so enter the read loop */
      } else {
        /* connect failed; wait and retry */
        /* pthread_testcancel(); // Allow exit (Not supported in w32-pthreads) */
        os_sleep_ms(2000);    // Sleep 2s
        continue;
      }
    }

    /* 2. Read data (connected) */
    if (source->is_connected && source->format_context) {
      int ret =
          av_read_frame((AVFormatContext *)source->format_context, packet);

      if (ret < 0) {
        /* Read error or EOF -> drop the connection and reconnect */
        if (ret == AVERROR_EOF) {
          receiver_log(LOG_INFO, source, "End of Stream");
        } else {
          receiver_log(LOG_WARNING, source, "Read Error: %d", ret);
        }
        cleanup_connection(source);
        continue; /* back to the top of the loop, triggering a reconnect */
      }

      /* 3. Handle the packet */
      static uint64_t video_packet_count = 0;
      static uint64_t audio_packet_count = 0;

      if (packet->stream_index == source->video_stream_index) {
        video_packet_count++;
        if (video_packet_count % 30 == 1) {
          receiver_log(LOG_INFO, source,
                       "Video packet received: #%llu, size=%d",
                       video_packet_count, packet->size);
        }
        video_frame_data_t frame = {0};
        if (decode_and_extract_sei(source, packet, &frame)) {
          source->frames_rendered++;
        }
      } else if (source->audio_stream_index >= 0 &&
                 packet->stream_index == source->audio_stream_index) {
        audio_packet_count++;
        if (audio_packet_count % 300 == 1) {
          receiver_log(LOG_INFO, source, "Audio packet received: #%llu",
                       audio_packet_count);
        }
        decode_audio(source, packet);
      }

      av_packet_unref(packet);
    }
  }

  av_packet_free(&packet);
  cleanup_connection(source);
  receiver_log(LOG_INFO, source, "Thread stopped");
  return NULL;
}

/* Start the receiver (thread only) */
static void start_receiver(void *data) {
  sei_receiver_source_t *ctx = (sei_receiver_source_t *)data;

  if (ctx->thread_active)
    return;

  receiver_log(LOG_INFO, ctx, "Starting background thread...");
  ctx->thread_active = true;
  pthread_create(&ctx->receive_thread, NULL, srt_receive_thread, ctx);
}

/* Stop the receiver (stop the thread) */
static void stop_receiver(void *data) {
  sei_receiver_source_t *ctx = (sei_receiver_source_t *)data;

  if (!ctx->thread_active)
    return;

  receiver_log(LOG_INFO, ctx, "Stopping background thread...");
  ctx->thread_active = false;
  pthread_join(ctx->receive_thread, NULL);

  /* Resources are cleaned up at end of thread,
     but we can ensure safety here if needed */
}

/* Note: ASYNC_VIDEO needs no video_render callback */
/* frames go straight to OBS via obs_source_output_video() */

/* Video width */
static uint32_t receiver_source_get_width(void *data) {
  sei_receiver_source_t *ctx = (sei_receiver_source_t *)data;
  return ctx->width ? ctx->width : 1920; /* 1920 by default */
}

/* Video height */
static uint32_t receiver_source_get_height(void *data) {
  sei_receiver_source_t *ctx = (sei_receiver_source_t *)data;
  return ctx->height ? ctx->height : 1080; /* 1080 by default */
}

/*============================================================================
 * Source plugin info struct
 *============================================================================*/

struct obs_source_info sei_receiver_source_info = {
    .id = "sei_receiver_source",
    .type = OBS_SOURCE_TYPE_INPUT,
    /* ASYNC_VIDEO: frames pushed via obs_source_output_video(); no video_render
     * OBS_SOURCE_DO_NOT_DUPLICATE removed so OBS copies data, avoiding UAF
     */
    .output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_AUDIO,
    .get_name = receiver_source_getname,
    .create = receiver_source_create,
    .destroy = receiver_source_destroy,
    .get_defaults = receiver_source_defaults,
    .get_properties = receiver_source_properties,
    .update = receiver_source_update,
    /* .activate = receiver_source_activate, */
    /* .deactivate = receiver_source_deactivate, */
    /* Note: ASYNC_VIDEO does not set video_render */
    .get_width = receiver_source_get_width,
    .get_height = receiver_source_get_height,
};
