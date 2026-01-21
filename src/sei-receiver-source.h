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
#include <util/threading.h> /* OBS线程API */

#ifdef __cplusplus
extern "C" {
#endif

/* 同步缓冲区大小 */
#define MAX_FRAME_BUFFER 60 /* 最多缓存60帧 */

/* 帧同步状态 */
typedef enum {
  SYNC_STATE_WAITING,     /* 等待首帧 */
  SYNC_STATE_BUFFERING,   /* 缓冲中 */
  SYNC_STATE_SYNCHRONIZED /* 已同步 */
} sync_state_t;

/* 视频帧数据 */
typedef struct video_frame_data {
  uint8_t *data;            /* 帧数据 */
  size_t size;              /* 数据大小 */
  int64_t pts;              /* 显示时间戳 */
  ntp_timestamp_t ntp_time; /* NTP时间戳(从SEI提取) */
  bool has_ntp;             /* 是否包含NTP时间戳 */
  uint32_t width;           /* 视频宽度 */
  uint32_t height;          /* 视频高度 */
  enum video_format format; /* 视频格式 */
} video_frame_data_t;

/* 帧缓冲区 */
typedef struct frame_buffer {
  video_frame_data_t frames[MAX_FRAME_BUFFER]; /* 帧数组 */
  size_t count;                                /* 当前帧数 */
  size_t read_index;                           /* 读取索引 */
  size_t write_index;                          /* 写入索引 */
  os_sem_t *semaphore;                         /* OBS信号量 */
} frame_buffer_t;

/* SEI接收源数据结构 */
typedef struct sei_receiver_source {
  obs_source_t *context; /* OBS源上下文 */

  /* SRT连接 */
  char srt_url[256];           /* SRT服务器URL (可包含?streamid=xxx等参数) */
  bool is_connected;           /* 是否已连接 */
  pthread_t receive_thread;    /* 接收线程 */
  volatile bool thread_active; /* 线程活动标志 */

  /* 视频解码 */
  void *format_context;       /* FFmpeg format上下文（demux） */
  void *decoder_context;      /* FFmpeg解码器上下文 */
  void *codec_context;        /* FFmpeg codec上下文 */
  struct SwsContext *sws_ctx; /* SwsScale上下文 */
  int video_stream_index;     /* 视频流索引 */
  enum video_format format;   /* 输出视频格式 */
  uint32_t width;             /* 视频宽度 */
  uint32_t height;            /* 视频高度 */

  /* 硬件解码 */
  void *hw_device_ctx;      /* AVBufferRef* - 硬件设备上下文 */
  char hw_decoder_type[32]; /* 硬件解码器类型 (none/qsv/nvdec/amf) */
  bool hw_decode_enabled;   /* 硬件解码是否启用 */

  /* NTP同步 */
  ntp_client_t ntp_client;         /* NTP客户端 */
  bool ntp_enabled;                /* NTP是否启用 */
  char ntp_server[128];            /* NTP服务器地址 */
  uint16_t ntp_port;               /* NTP服务器端口 */
  uint64_t last_ntp_sync_time;     /* 上次NTP同步的本地时间(纳秒) */
  uint32_t ntp_drift_threshold_ms; /* NTP漂移阈值（毫秒） */
  uint32_t ntp_sync_interval_ms;   /* NTP最小同步间隔（毫秒） */

  /* 帧同步 */
  frame_buffer_t frame_buffer; /* 帧缓冲区 */
  sync_state_t sync_state;     /* 同步状态 */
  int64_t time_offset_ns;      /* 时间偏移(纳秒) */
  uint64_t first_ntp_time;     /* 第一帧NTP时间 */
  uint64_t first_local_time;   /* 第一帧本地时间 */

  /* PTS同步 */
  int64_t pts_offset;  /* PTS 到 SystemTime 的偏移量 */
  bool has_pts_offset; /* 是否已计算偏移量 */

  /* 音频解码 */
  void *audio_codec_context;      /* FFmpeg音频相关codec上下文 */
  int audio_stream_index;         /* 音频流索引 */
  enum audio_format audio_format; /* OBS音频格式 */
  uint32_t audio_channels;        /* 声道数 */
  uint32_t audio_sample_rate;     /* 采样率 */

  /* 统计信息 */
  uint64_t frames_received;       /* 接收的总帧数 */
  uint64_t frames_rendered;       /* 渲染的总帧数 */
  uint64_t frames_dropped;        /* 丢弃的帧数 */
  uint64_t sei_found_count;       /* 找到SEI的帧数 */
  uint64_t last_sync_frame_count; /* 上次同步时的帧数 */

  /* 实时统计 */
  uint64_t last_stats_update_time; /* 上次统计更新时间(ns) */
  uint64_t stats_frame_count;      /* 统计周期内的帧数 */
  float current_fps;               /* 当前帧率 */
  float sei_detection_rate;        /* SEI检测率(%) */

  /* 错误恢复 */
  uint32_t decode_error_count;     /* 连续解码错误计数 */
  uint32_t decode_error_threshold; /* 错误阈值，超过则重置 */

} sei_receiver_source_t;

/* 源插件信息 */
extern struct obs_source_info sei_receiver_source_info;

/* 核心功能函数 */

/**
 * 初始化帧缓冲区
 */
bool frame_buffer_init(frame_buffer_t *buffer);

/**
 * 销毁帧缓冲区
 */
void frame_buffer_destroy(frame_buffer_t *buffer);

/**
 * 向缓冲区添加帧
 */
bool frame_buffer_push(frame_buffer_t *buffer, video_frame_data_t *frame);

/**
 * 从缓冲区获取帧
 */
bool frame_buffer_pop(frame_buffer_t *buffer, video_frame_data_t *frame);

/**
 * 获取缓冲区大小
 */
size_t frame_buffer_size(frame_buffer_t *buffer);

/**
 * SRT接收线程
 */
void *srt_receive_thread(void *data);

/**
 * 解码并提取SEI
 */
bool decode_and_extract_sei(sei_receiver_source_t *source, AVPacket *packet,
                            video_frame_data_t *frame_out);

/**
 * 计算帧显示时间
 */
int64_t calculate_display_time(sei_receiver_source_t *source,
                               video_frame_data_t *frame);

#ifdef __cplusplus
}
#endif
