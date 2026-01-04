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

/* 日志宏 */
#define receiver_log(level, src, format, ...)                                  \
  blog(level, "[SEI Receiver: '%s'] " format,                                  \
       obs_source_get_name(src->context), ##__VA_ARGS__)

/* SRT接收缓冲区大小 */
#define SRT_BUFFER_SIZE (1024 * 1024) /* 1MB */

/*============================================================================
 * 帧缓冲区管理
 *============================================================================*/

/* 初始化帧缓冲区 */
bool frame_buffer_init(frame_buffer_t *buffer) {
  if (!buffer) {
    return false;
  }

  memset(buffer, 0, sizeof(frame_buffer_t));
  if (os_sem_init(&buffer->semaphore, 1) != 0)
    return false;

  return true;
}

/* 销毁帧缓冲区 */
void frame_buffer_destroy(frame_buffer_t *buffer) {
  if (!buffer) {
    return;
  }

  os_sem_wait(buffer->semaphore);

  /* 释放所有帧数据 */
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

/* 向缓冲区添加帧 */
bool frame_buffer_push(frame_buffer_t *buffer, video_frame_data_t *frame) {
  if (!buffer || !frame) {
    return false;
  }

  os_sem_wait(buffer->semaphore);

  /* 检查是否已满 */
  if (buffer->count >= MAX_FRAME_BUFFER) {
    os_sem_post(buffer->semaphore);
    return false;
  }

  /* 复制帧数据 */
  size_t write_idx = buffer->write_index;
  buffer->frames[write_idx] = *frame;

  /* 复制原始数据 */
  if (frame->data && frame->size > 0) {
    buffer->frames[write_idx].data = bmalloc(frame->size);
    memcpy(buffer->frames[write_idx].data, frame->data, frame->size);
  }

  /* 更新索引 */
  buffer->write_index = (buffer->write_index + 1) % MAX_FRAME_BUFFER;
  buffer->count++;

  os_sem_post(buffer->semaphore);

  return true;
}

/* 从缓冲区获取帧 */
bool frame_buffer_pop(frame_buffer_t *buffer, video_frame_data_t *frame) {
  if (!buffer || !frame) {
    return false;
  }

  os_sem_wait(buffer->semaphore);

  /* 检查是否为空 */
  if (buffer->count == 0) {
    os_sem_post(buffer->semaphore);
    return false;
  }

  /* 获取帧 */
  size_t read_idx = buffer->read_index;
  *frame = buffer->frames[read_idx];

  /* 清空原数据指针(调用者负责释放) */
  buffer->frames[read_idx].data = NULL;

  /* 更新索引 */
  buffer->read_index = (buffer->read_index + 1) % MAX_FRAME_BUFFER;
  buffer->count--;

  os_sem_post(buffer->semaphore);

  return true;
}

/* 获取缓冲区大小 */
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
 * 统计更新和错误恢复
 *============================================================================*/

/* 前向声明 */
static enum AVPixelFormat get_hw_format(AVCodecContext *ctx,
                                        const enum AVPixelFormat *pix_fmts);

/* 更新实时统计信息 */
static void update_statistics(sei_receiver_source_t *source) {
  uint64_t current_time = os_gettime_ns();

  /* 每秒更新一次统计 */
  if (source->last_stats_update_time == 0 ||
      (current_time - source->last_stats_update_time) >= 1000000000ULL) {

    if (source->last_stats_update_time > 0) {
      /* 计算帧率 */
      uint64_t frames_in_period =
          source->frames_rendered - source->stats_frame_count;
      double time_elapsed =
          (current_time - source->last_stats_update_time) / 1000000000.0;
      source->current_fps = (float)(frames_in_period / time_elapsed);

      /* 计算SEI检测率 */
      if (source->frames_rendered > 0) {
        source->sei_detection_rate =
            (float)(source->sei_found_count * 100.0 / source->frames_rendered);
      }
    }

    source->last_stats_update_time = current_time;
    source->stats_frame_count = source->frames_rendered;
  }
}

/* 解码器重置（用于错误恢复） */
static bool reset_decoder(sei_receiver_source_t *source) {
  receiver_log(LOG_WARNING, source, "Resetting decoder due to errors...");

  /* 关闭现有解码器 */
  if (source->codec_context) {
    avcodec_free_context((AVCodecContext **)&source->codec_context);
    source->codec_context = NULL;
  }

  if (source->format_context && source->video_stream_index >= 0) {
    AVFormatContext *fmt_ctx = (AVFormatContext *)source->format_context;
    AVStream *vstream = fmt_ctx->streams[source->video_stream_index];

    /* 重新创建解码器 */
    const AVCodec *codec = avcodec_find_decoder(vstream->codecpar->codec_id);
    if (!codec) {
      receiver_log(LOG_ERROR, source, "Failed to find decoder for reset");
      return false;
    }

    AVCodecContext *cctx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(cctx, vstream->codecpar);

    /* 重新配置硬件解码 */
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
 * 硬件解码支持
 *============================================================================*/

/* 硬件像素格式回调 */
static enum AVPixelFormat get_hw_format(AVCodecContext *ctx,
                                        const enum AVPixelFormat *pix_fmts) {
  const enum AVPixelFormat *p;

  /* 优先返回硬件格式 */
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

/* 初始化硬件设备上下文 */
static bool init_hw_device(sei_receiver_source_t *source) {
  if (!source || !source->hw_decode_enabled)
    return false;

  enum AVHWDeviceType hw_type = AV_HWDEVICE_TYPE_NONE;

  /* 根据配置选择硬件类型 */
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
 * 视频解码和SEI提取
 *============================================================================*/

/* 解码并提取SEI */
bool decode_and_extract_sei(sei_receiver_source_t *source, AVPacket *packet,
                            video_frame_data_t *frame_out) {
  if (!source || !packet || !frame_out) {
    return false;
  }

  AVCodecContext *codec_ctx = (AVCodecContext *)source->codec_context;
  if (!codec_ctx) {
    return false;
  }

  /* 发送数据包到解码器 */
  int ret = avcodec_send_packet(codec_ctx, packet);
  if (ret < 0) {
    receiver_log(LOG_ERROR, source, "Failed to send packet to decoder: %d",
                 ret);
    return false;
  }

  /* 接收解码帧 */
  AVFrame *av_frame = av_frame_alloc();
  if (!av_frame) {
    return false;
  }

  ret = avcodec_receive_frame(codec_ctx, av_frame);
  if (ret < 0) {
    av_frame_free(&av_frame);
    if (ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) {
      receiver_log(LOG_ERROR, source, "Failed to receive frame: %d", ret);

      /* 错误恢复：增加错误计数 */
      source->decode_error_count++;
      if (source->decode_error_count >= source->decode_error_threshold) {
        receiver_log(LOG_WARNING, source,
                     "Decoder error threshold reached (%u), attempting reset",
                     source->decode_error_count);
        reset_decoder(source);
      }
    }
    return false;
  }

  /* 解码成功，重置错误计数 */
  source->decode_error_count = 0;

  /* 处理硬件帧：如果是硬件格式，转换到系统内存 */
  if (av_frame->format == AV_PIX_FMT_QSV ||
      av_frame->format == AV_PIX_FMT_CUDA ||
      av_frame->format == AV_PIX_FMT_D3D11) {
    AVFrame *sw_frame = av_frame_alloc();
    if (!sw_frame) {
      av_frame_free(&av_frame);
      return false;
    }

    /* 硬件帧 -> 系统内存 */
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

  /* 从AVFrame获取PTS（FFmpeg已经从packet转换） */
  int64_t pts = av_frame->pts;
  if (pts == AV_NOPTS_VALUE) {
    /* 如果没有PTS，使用当前时间 */
    pts = os_gettime_ns();
  } else {
    /* 转换PTS从stream timebase到纳秒 */
    AVRational time_base = {1, 90000}; // 通常MPEG-TS使用90kHz
    pts = av_rescale_q(pts, time_base, (AVRational){1, 1000000000});
  }

  /* 填充帧信息 */
  frame_out->width = av_frame->width;
  frame_out->height = av_frame->height;
  frame_out->pts = av_frame->pts;
  frame_out->format = VIDEO_FORMAT_I420; /* 默认YUV420P */

  /* 计算帧大小并分配内存 (Align 32 for OBS) */
  /* BGRA Output */
  int frame_size = av_image_get_buffer_size(AV_PIX_FMT_BGRA, av_frame->width,
                                            av_frame->height, 32);
  frame_out->data = bmalloc(frame_size);
  frame_out->size = frame_size;

  /* 格式转换 (SwsScale to BGRA) */
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

  /* 复制图像数据 */
  uint8_t *dest[4] = {0};
  int linesize[4] = {0};

  av_image_fill_arrays(dest, linesize, frame_out->data, AV_PIX_FMT_BGRA,
                       av_frame->width, av_frame->height, 32);

  sws_scale(source->sws_ctx, (const uint8_t *const *)av_frame->data,
            av_frame->linesize, 0, av_frame->height, dest, linesize);

  /* 尝试从side data中提取SEI */
  frame_out->has_ntp = false;

  AVFrameSideData *sei_data =
      av_frame_get_side_data(av_frame, AV_FRAME_DATA_SEI_UNREGISTERED);

  if (sei_data) {
    /* 解析NTP SEI */
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
    /* 如果side data中没有，尝试从原始数据中查找 */
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

  /* Output to OBS */
  struct obs_source_frame obs_frame = {0};

  obs_frame.data[0] = dest[0];
  obs_frame.linesize[0] = linesize[0];

  obs_frame.width = frame_out->width;
  obs_frame.height = frame_out->height;
  obs_frame.format = VIDEO_FORMAT_BGRA;

  /* Calculate correct Sync Timestamp */
  obs_frame.timestamp = calculate_display_time(source, frame_out);

  /* 调试日志: 确认时间戳 */
  receiver_log(
      LOG_DEBUG, source, "Video Decoded: %dx%d, PTS_IN=%lld, TS_OUT=%lld",
      obs_frame.width, obs_frame.height, packet->pts, obs_frame.timestamp);

  /* 输出帧到OBS */
  obs_source_output_video(source->context, &obs_frame);

  /* 更新统计信息 */
  update_statistics(source);

  /* 释放AVFrame */
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

/* 获取同步后的时间戳 (PTS -> SystemTime) */
int64_t get_sync_timestamp(sei_receiver_source_t *source, int64_t pts) {
  if (!source)
    return 0;

  int64_t current_time = os_gettime_ns();

  /* 转换PTS到纳秒: FFmpeg PTS通常基于timebase, 这里假设90kHz (SRT默认/MPEGTS)
   */
  /* 如果PTS已经是纳秒(例如AV_NOPTS_VALUE处理后)，需要注意 */
  /* 通常调用者会传入处理过的纳秒PTS */

  if (!source->has_pts_offset) {
    source->pts_offset = current_time - pts;
    source->has_pts_offset = true;

    receiver_log(LOG_INFO, source,
                 "Initialized Sync Offset: PTS=%lld, Local=%lld, Offset=%lld",
                 pts, current_time, source->pts_offset);
  }

  return pts + source->pts_offset;
}

/* 计算视频显示时间 */
int64_t calculate_display_time(sei_receiver_source_t *source,
                               video_frame_data_t *frame) {
  if (!source || !frame) {
    return 0;
  }

  /* 如果有NTP时间戳，且启用了NTP同步 */
  if (frame->has_ntp && source->ntp_enabled) {
    /* 计算NTP时间戳对应的纳秒 */
    uint64_t ntp_ns =
        ((uint64_t)frame->ntp_time.seconds * 1000000000ULL) +
        (((uint64_t)frame->ntp_time.fraction * 1000000000ULL) >> 32);

    /* NTP模式: 使用绝对时间同步 (Absolute NTP Time) */
    /* 我们不再依赖 "首帧对齐"，而是依赖 NTP Client 计算出的全局偏移 */
    /* Offset = NTP_Server - Local_System */
    /* Local_Render_Time = Frame_NTP - Offset */

    int64_t ntp_offset = ntp_client_get_offset(&source->ntp_client);
    int64_t display_time = (int64_t)ntp_ns - ntp_offset;

    /* 记录日志(仅定期，避免刷屏) */
    if (source->sei_found_count % 300 == 0) {
      receiver_log(LOG_DEBUG, source,
                   "Absolute Sync: NTP=%llu, Offset=%lld, Display=%lld", ntp_ns,
                   ntp_offset, display_time);
    }

    /* 更新通用PTS偏移，以便音频能跟随视频的节奏 */
    /* 这一点很重要，因为音频仍然可能使用 PTS */
    /* AudioTarget = VideoDisplayTime */
    /* VideoPTS + PTSOffset = VideoDisplayTime */
    /* PTSOffset = VideoDisplayTime - VideoPTS */

    /* 每次收到SEI(关键帧/IDR)都更新PTS Offset (按用户要求每帧校准) */
    // if (source->sei_found_count % 1 == 0) // Always
    source->pts_offset = display_time - frame->pts;
    source->has_pts_offset = true;

    return display_time;
  }

  /* 没有NTP或未启用，使用通用PTS同步 */
  return get_sync_timestamp(source, frame->pts);
}

/* 解码音频 */
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

    /* 时间戳同步 */
    /* 获取audio pts (time_base based) */
    int64_t pts = frame->pts;
    int64_t pts_ns = 0;

    if (pts != AV_NOPTS_VALUE) {
      AVRational tb = ctx->time_base;
      /* 如果timebase是0，尝试使用 1/sample_rate */
      if (tb.num == 0 || tb.den == 0) {
        tb.num = 1;
        tb.den = ctx->sample_rate;
      }
      pts_ns = av_rescale_q(pts, tb, (AVRational){1, 1000000000});
    } else {
      /* 如果没有PTS，使用当前Offset推算 */
      pts_ns = (int64_t)audio.timestamp - source->pts_offset;
      if (!source->has_pts_offset)
        pts_ns = 0;
    }

    /* 使用统一的 timestamp sync */
    audio.timestamp = (uint64_t)get_sync_timestamp(source, pts_ns);

    obs_source_output_audio(source->context, &audio);
  }

  av_frame_free(&frame);
  return success;
}

/*============================================================================
 * SRT接收线程
 *============================================================================*/

/* SRT接收线程 */

/*============================================================================
 * OBS Source 回调函数
 *============================================================================*/

/* 获取源名称 */
static const char *receiver_source_getname(void *unused) {
  UNUSED_PARAMETER(unused);
  return obs_module_text("SEIReceiver");
}

static void start_receiver(void *data);
static void stop_receiver(void *data);

/* 创建源 */
static void *receiver_source_create(obs_data_t *settings,
                                    obs_source_t *source) {
  sei_receiver_source_t *ctx = bzalloc(sizeof(sei_receiver_source_t));
  ctx->context = source;

  /* 初始化帧缓冲区 */
  if (!frame_buffer_init(&ctx->frame_buffer)) {
    bfree(ctx);
    return NULL;
  }

  /* 初始化同步状态 */
  ctx->sync_state = SYNC_STATE_WAITING;
  ctx->has_pts_offset = false;
  ctx->pts_offset = 0;

  /* 从设置中加载配置 */
  const char *srt_url = obs_data_get_string(settings, "srt_url");
  if (srt_url && srt_url[0]) {
    strncpy(ctx->srt_url, srt_url, sizeof(ctx->srt_url) - 1);
  }

  /* SLS Stream ID */
  const char *srt_streamid = obs_data_get_string(settings, "srt_streamid");
  if (srt_streamid && srt_streamid[0]) {
    strncpy(ctx->srt_streamid, srt_streamid, sizeof(ctx->srt_streamid) - 1);
  }

  /* 硬件解码器设置 */
  const char *hw_decoder = obs_data_get_string(settings, "hw_decoder");
  if (hw_decoder && hw_decoder[0]) {
    strncpy(ctx->hw_decoder_type, hw_decoder, sizeof(ctx->hw_decoder_type) - 1);
    ctx->hw_decode_enabled = (strcmp(hw_decoder, "none") != 0);
  } else {
    strcpy(ctx->hw_decoder_type, "none");
    ctx->hw_decode_enabled = false;
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

  /* 初始化统计和错误恢复 */
  ctx->last_stats_update_time = 0;
  ctx->stats_frame_count = 0;
  ctx->current_fps = 0.0f;
  ctx->sei_detection_rate = 0.0f;
  ctx->decode_error_count = 0;
  ctx->decode_error_threshold = 10; /* 连续10次错误后重置 */

  /* 初始化NTP客户端 */
  if (ctx->ntp_enabled) {
    if (ntp_client_init(&ctx->ntp_client, ctx->ntp_server, ctx->ntp_port)) {
      receiver_log(LOG_INFO, ctx, "NTP client initialized");

      /* 执行初始同步 */
      if (ntp_client_sync(&ctx->ntp_client)) {
        receiver_log(LOG_INFO, ctx, "Initial NTP sync successful");
      }
    }
  }

  receiver_log(LOG_INFO, ctx, "SEI Receiver source created");

  /* 在后台立即启动 */
  start_receiver(ctx);

  return ctx;
}

/* 销毁源 */
static void receiver_source_destroy(void *data) {
  sei_receiver_source_t *ctx = (sei_receiver_source_t *)data;

  /* 停止接收器 */
  stop_receiver(ctx);

  /* SRT连接由avformat管理，无需手动关闭socket */

  /* 销毁解码器 */
  if (ctx->codec_context) {
    AVCodecContext *codec_ctx = (AVCodecContext *)ctx->codec_context;
    avcodec_free_context(&codec_ctx);
  }

  /* 销毁音频解码器 */
  if (ctx->audio_codec_context) {
    AVCodecContext *actx = (AVCodecContext *)ctx->audio_codec_context;
    avcodec_free_context(&actx);
    ctx->audio_codec_context = NULL;
  }

  /* 销毁NTP客户端 */
  ntp_client_destroy(&ctx->ntp_client);

  /* 销毁帧缓冲区 */
  frame_buffer_destroy(&ctx->frame_buffer);

  /* 释放SwsContext */
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

/* 获取默认设置 */
static void receiver_source_defaults(obs_data_t *settings) {
  obs_data_set_default_string(settings, "srt_url", "srt://127.0.0.1:9000");
  obs_data_set_default_string(settings, "srt_streamid", "");
  obs_data_set_default_string(settings, "ntp_server", "time.windows.com");
  obs_data_set_default_int(settings, "ntp_port", 123);
  obs_data_set_default_bool(settings, "ntp_enabled", true);
  obs_data_set_default_string(settings, "hw_decoder", "none");
}

/* 获取属性 */
static obs_properties_t *receiver_source_properties(void *data) {
  UNUSED_PARAMETER(data);

  obs_properties_t *props = obs_properties_create();

  /* SRT URL */
  obs_properties_add_text(props, "srt_url", obs_module_text("SRTUrl"),
                          OBS_TEXT_DEFAULT);

  /* Stream ID (SLS) */
  obs_properties_add_text(props, "srt_streamid", obs_module_text("SRTStreamID"),
                          OBS_TEXT_DEFAULT);

  /* 硬件解码器选择 */
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

  /* NTP设置组 */
  obs_properties_add_group(props, "ntp_group", obs_module_text("NTPSettings"),
                           OBS_GROUP_NORMAL, NULL);

  /* NTP属性直接添加到props */
  obs_properties_add_bool(props, "ntp_enabled", obs_module_text("EnableNTP"));

  obs_properties_add_text(props, "ntp_server", obs_module_text("NTPServer"),
                          OBS_TEXT_DEFAULT);

  obs_properties_add_int(props, "ntp_port", obs_module_text("NTPPort"), 1,
                         65535, 1);

  /* 状态信息(只读) */
  obs_properties_add_text(props, "status", obs_module_text("Status"),
                          OBS_TEXT_INFO);

  return props;
}

/* 更新设置 */
static void receiver_source_update(void *data, obs_data_t *settings) {
  sei_receiver_source_t *ctx = (sei_receiver_source_t *)data;

  bool settings_changed = false;

  /* 更新SRT URL */
  const char *srt_url = obs_data_get_string(settings, "srt_url");
  if (srt_url && srt_url[0] && strcmp(ctx->srt_url, srt_url) != 0) {
    /* 如果URL改变，需要重启接收器 */
    receiver_log(LOG_INFO, ctx, "SRT URL changed, restarting...");
    stop_receiver(ctx);
    strncpy(ctx->srt_url, srt_url, sizeof(ctx->srt_url) - 1);
    settings_changed = true;
  }

  /* 更新NTP设置 */
  bool ntp_enabled = obs_data_get_bool(settings, "ntp_enabled");
  if (ntp_enabled != ctx->ntp_enabled) {
    ctx->ntp_enabled = ntp_enabled;
    // NTP设置变更不需要重启流，只需要启停NTP Client

    if (ntp_enabled) {
      const char *ntp_server = obs_data_get_string(settings, "ntp_server");
      uint16_t ntp_port = (uint16_t)obs_data_get_int(settings, "ntp_port");

      if (ntp_client_init(&ctx->ntp_client, ntp_server, ntp_port)) {
        ntp_client_sync(&ctx->ntp_client);
      }
    } else {
      ntp_client_destroy(&ctx->ntp_client);
    }
  }

  /* 如果因为设置改变而停止了，现在重新启动 */
  if (settings_changed) {
    start_receiver(ctx);
  } else if (!ctx->is_connected) {
    /* 确保始终运行 */
    start_receiver(ctx);
  }

  receiver_log(LOG_INFO, ctx, "Settings updated");
}
/* 辅助: 清理连接资源 */
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

/* 辅助: 尝试建立连接 */
static bool try_connect(sei_receiver_source_t *source) {
  receiver_log(LOG_INFO, source, "Attempting to connect to: %s",
               source->srt_url);

  AVFormatContext *fmt_ctx = avformat_alloc_context();
  if (!fmt_ctx)
    return false;

  /* 设置超时，避免长时间阻塞 */
  AVDictionary *options = NULL;
  av_dict_set(&options, "timeout", "2000000", 0); /* 2秒超时 */

  /* 构建完整URL (支持SLS streamid) */
  char full_url[1024];
  if (source->srt_streamid[0]) {
    snprintf(full_url, sizeof(full_url), "%s?streamid=%s", source->srt_url,
             source->srt_streamid);
    receiver_log(LOG_INFO, source, "Using Stream ID: %s", source->srt_streamid);
  } else {
    strncpy(full_url, source->srt_url, sizeof(full_url) - 1);
  }

  if (avformat_open_input(&fmt_ctx, full_url, NULL, &options) < 0) {
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

  /* 查找视频流 */
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

  /* 初始化硬件设备 (如果启用) */
  if (source->hw_decode_enabled) {
    init_hw_device(source);
  }

  /* 初始化视频解码器 */
  const AVCodec *codec = avcodec_find_decoder(vstream->codecpar->codec_id);
  if (!codec) {
    receiver_log(LOG_ERROR, source, "Video decoder not found");
    avformat_close_input(&fmt_ctx);
    return false;
  }

  AVCodecContext *cctx = avcodec_alloc_context3(codec);
  avcodec_parameters_to_context(cctx, vstream->codecpar);

  /* 配置硬件解码 */
  if (source->hw_decode_enabled && source->hw_device_ctx) {
    cctx->hw_device_ctx = av_buffer_ref((AVBufferRef *)source->hw_device_ctx);
    cctx->get_format = get_hw_format;
    receiver_log(LOG_INFO, source, "Hardware decoder configured");
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

  /* 查找音频流 (可选) */
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

/* SRT接收线程 (负责连接管理及数据接收) */
static void *srt_receive_thread(void *data) {
  sei_receiver_source_t *source = (sei_receiver_source_t *)data;
  receiver_log(LOG_INFO, source, "Thread started (Auto-Reconnect Mode)");

  // struct os_timespec ts; // Removed unused variable
  // ts.tv_sec = 1;
  // ts.tv_nsec = 0;

  AVPacket *packet = av_packet_alloc();

  while (source->thread_active) {

    /* 1. 如果未连接，尝试连接 */
    if (!source->is_connected) {
      if (try_connect(source)) {
        /* 连接成功，进入数据接收循环 */
      } else {
        /* 连接失败，等待后重试 */
        pthread_testcancel(); // Allow exit
        os_sleep_ms(2000);    // Sleep 2s
        continue;
      }
    }

    /* 2. 读取数据 (已连接状态) */
    if (source->is_connected && source->format_context) {
      int ret =
          av_read_frame((AVFormatContext *)source->format_context, packet);

      if (ret < 0) {
        /* 读取错误或EOF -> 断开连接，准备重连 */
        if (ret == AVERROR_EOF) {
          receiver_log(LOG_INFO, source, "End of Stream");
        } else {
          receiver_log(LOG_WARNING, source, "Read Error: %d", ret);
        }
        cleanup_connection(source);
        continue; /* 回到循环顶部，触发重连 */
      }

      /* 3. 处理数据包 */
      if (packet->stream_index == source->video_stream_index) {
        video_frame_data_t frame = {0};
        if (decode_and_extract_sei(source, packet, &frame)) {
          source->frames_rendered++;
        }
      } else if (source->audio_stream_index >= 0 &&
                 packet->stream_index == source->audio_stream_index) {
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

/* 启动接收器 (仅启动线程) */
static void start_receiver(void *data) {
  sei_receiver_source_t *ctx = (sei_receiver_source_t *)data;

  if (ctx->thread_active)
    return;

  receiver_log(LOG_INFO, ctx, "Starting background thread...");
  ctx->thread_active = true;
  pthread_create(&ctx->receive_thread, NULL, srt_receive_thread, ctx);
}

/* 停止接收器 (停止线程) */
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

/* 注意：ASYNC_VIDEO模式下不需要video_render回调 */
/* 视频帧通过obs_source_output_video()直接输出到OBS */

/* 获取视频宽度 */
static uint32_t receiver_source_get_width(void *data) {
  sei_receiver_source_t *ctx = (sei_receiver_source_t *)data;
  return ctx->width ? ctx->width : 1920; /* 默认1920 */
}

/* 获取视频高度 */
static uint32_t receiver_source_get_height(void *data) {
  sei_receiver_source_t *ctx = (sei_receiver_source_t *)data;
  return ctx->height ? ctx->height : 1080; /* 默认1080 */
}

/*============================================================================
 * 源插件信息结构
 *============================================================================*/

struct obs_source_info sei_receiver_source_info = {
    .id = "sei_receiver_source",
    .type = OBS_SOURCE_TYPE_INPUT,
    /* ASYNC_VIDEO: 使用obs_source_output_video()推送帧，不需要video_render回调
     * 移除 OBS_SOURCE_DO_NOT_DUPLICATE 以确保OBS复制数据，避免释放后使用
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
    /* 注意：ASYNC_VIDEO模式下不设置video_render */
    .get_width = receiver_source_get_width,
    .get_height = receiver_source_get_height,
};
