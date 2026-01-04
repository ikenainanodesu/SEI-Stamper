/******************************************************************************
    NTP Client Module - Header File
    Copyright (C) 2026

    Implements NTP (Network Time Protocol) client for time synchronization
******************************************************************************/

#pragma once

#include <stdbool.h>
#include <stdint.h>


#ifdef __cplusplus
extern "C" {
#endif

/* NTP时间戳结构 - 64位,包含秒和分数部分 */
typedef struct ntp_timestamp {
  uint32_t seconds;  /* 从1900年1月1日开始的秒数 */
  uint32_t fraction; /* 秒的分数部分(2^-32秒为单位) */
} ntp_timestamp_t;

/* NTP数据包结构 (48字节) */
typedef struct ntp_packet {
  uint8_t li_vn_mode; /* Leap Indicator(2位) + Version(3位) + Mode(3位) */
  uint8_t stratum;    /* 层级(0-15) */
  uint8_t poll;       /* 轮询间隔 */
  uint8_t precision;  /* 精度 */

  uint32_t root_delay;      /* 根延迟 */
  uint32_t root_dispersion; /* 根离散度 */
  uint32_t reference_id;    /* 参考时钟标识符 */

  ntp_timestamp_t reference_timestamp; /* 参考时间戳 */
  ntp_timestamp_t originate_timestamp; /* 起始时间戳 (T1) */
  ntp_timestamp_t receive_timestamp;   /* 接收时间戳 (T2) */
  ntp_timestamp_t transmit_timestamp;  /* 传输时间戳 (T3) */
} ntp_packet_t;

/* NTP客户端上下文 */
typedef struct ntp_client {
  char server_address[256]; /* NTP服务器地址 */
  uint16_t server_port;     /* NTP服务器端口(通常是123) */

  int socket_fd;       /* UDP socket文件描述符 */
  bool is_initialized; /* 是否已初始化 */
  bool is_synced;      /* 是否已同步 */

  ntp_timestamp_t last_sync_time; /* 最后同步的NTP时间 */
  uint64_t last_sync_local_time;  /* 最后同步时的本地时间(os_gettime_ns) */
  int64_t time_offset_ns;         /* 时间偏移(纳秒) */

  uint32_t sync_count;  /* 同步次数 */
  uint32_t error_count; /* 错误次数 */
} ntp_client_t;

/*
 * 初始化NTP客户端
 * 参数:
 *   client - NTP客户端上下文
 *   server - NTP服务器地址(例如: "time.windows.com")
 *   port - NTP服务器端口(通常是123)
 * 返回:
 *   true - 成功
 *   false - 失败
 */
bool ntp_client_init(ntp_client_t *client, const char *server, uint16_t port);

/*
 * 执行NTP时间同步
 * 参数:
 *   client - NTP客户端上下文
 * 返回:
 *   true - 同步成功
 *   false - 同步失败
 */
bool ntp_client_sync(ntp_client_t *client);

/*
 * 获取当前的NTP时间戳
 * 参数:
 *   client - NTP客户端上下文
 *   timestamp - 输出的NTP时间戳
 * 返回:
 *   true - 成功
 *   false - 失败(未同步或时间过期)
 */
bool ntp_client_get_time(ntp_client_t *client, ntp_timestamp_t *timestamp);

/*
 * 获取时间偏移(本地时间 - NTP时间)
 * 参数:
 *   client - NTP客户端上下文
 * 返回:
 *   时间偏移(纳秒)
 */
int64_t ntp_client_get_offset(ntp_client_t *client);

/*
 * 检查是否需要重新同步
 * 参数:
 *   client - NTP客户端上下文
 *   max_age_seconds - 最大允许的同步时间间隔(秒)
 * 返回:
 *   true - 需要重新同步
 *   false - 不需要
 */
bool ntp_client_needs_resync(ntp_client_t *client, uint32_t max_age_seconds);

/*
 * 销毁NTP客户端
 * 参数:
 *   client - NTP客户端上下文
 */
void ntp_client_destroy(ntp_client_t *client);

#ifdef __cplusplus
}
#endif
