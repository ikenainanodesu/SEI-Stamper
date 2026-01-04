/******************************************************************************
    NTP Client Module - Implementation
    Copyright (C) 2026

    Implements NTP (Network Time Protocol) client for time synchronization
******************************************************************************/

#include "ntp-client.h"
#include <obs-module.h>
#include <string.h>
#include <util/platform.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#endif

/* NTP常量 */
#define NTP_TIMESTAMP_DELTA 2208988800ULL /* 1900到1970的秒数 */
#define NTP_VERSION 3
#define NTP_MODE_CLIENT 3
#define NTP_PACKET_SIZE 48

/* 日志宏 */
#define ntp_log(level, format, ...)                                            \
  blog(level, "[NTP Client] " format, ##__VA_ARGS__)

/* 辅助函数:将网络字节序转换为主机字节序 */
static uint32_t ntohl_swap(uint32_t netlong) { return ntohl(netlong); }

/* 辅助函数:将主机字节序转换为网络字节序 */
static uint32_t htonl_swap(uint32_t hostlong) { return htonl(hostlong); }

/* 辅助函数:获取当前时间(纳秒) */
static uint64_t get_current_time_ns(void) { return os_gettime_ns(); }

/* 辅助函数:将NTP时间戳转换为纳秒 */
static uint64_t ntp_to_ns(ntp_timestamp_t *ntp) {
  uint64_t seconds = (uint64_t)ntp->seconds;
  uint64_t fraction = (uint64_t)ntp->fraction;

  /* 转换为Unix时间戳 */
  if (seconds > NTP_TIMESTAMP_DELTA) {
    seconds -= NTP_TIMESTAMP_DELTA;
  }

  /* 秒转纳秒 */
  uint64_t ns = seconds * 1000000000ULL;

  /* 分数部分转纳秒: fraction / 2^32 * 10^9 */
  ns += (fraction * 1000000000ULL) >> 32;

  return ns;
}

/* 辅助函数:将纳秒转换为NTP时间戳 */
static void ns_to_ntp(uint64_t ns, ntp_timestamp_t *ntp) {
  /* 纳秒转秒 */
  uint64_t seconds = ns / 1000000000ULL;
  uint64_t fraction_ns = ns % 1000000000ULL;

  /* 转换为NTP时间戳(从1900年开始) */
  ntp->seconds = (uint32_t)(seconds + NTP_TIMESTAMP_DELTA);

  /* 分数部分: fraction_ns / 10^9 * 2^32 */
  ntp->fraction = (uint32_t)((fraction_ns << 32) / 1000000000ULL);
}

/* 初始化Winsock(仅Windows) */
#ifdef _WIN32
static bool init_winsock(void) {
  static bool initialized = false;
  if (initialized) {
    return true;
  }

  WSADATA wsa_data;
  int result = WSAStartup(MAKEWORD(2, 2), &wsa_data);
  if (result != 0) {
    ntp_log(LOG_ERROR, "WSAStartup failed: %d", result);
    return false;
  }

  initialized = true;
  return true;
}
#endif

/* 初始化NTP客户端 */
bool ntp_client_init(ntp_client_t *client, const char *server, uint16_t port) {
  if (!client || !server) {
    ntp_log(LOG_ERROR, "Invalid parameters");
    return false;
  }

  memset(client, 0, sizeof(ntp_client_t));

#ifdef _WIN32
  if (!init_winsock()) {
    return false;
  }
#endif

  /* 复制服务器地址 */
  strncpy(client->server_address, server, sizeof(client->server_address) - 1);
  client->server_port = port;
  client->socket_fd = -1;
  client->is_initialized = true;

  ntp_log(LOG_INFO, "NTP client initialized (server: %s:%d)", server, port);

  return true;
}

/* 执行NTP时间同步 */
bool ntp_client_sync(ntp_client_t *client) {
  if (!client || !client->is_initialized) {
    ntp_log(LOG_ERROR, "Client not initialized");
    return false;
  }

  int sock = -1;
  struct addrinfo hints, *server_info = NULL;
  ntp_packet_t packet;
  bool success = false;

  /* 创建UDP socket */
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC; /* IPv4或IPv6 */
  hints.ai_socktype = SOCK_DGRAM;

  char port_str[16];
  snprintf(port_str, sizeof(port_str), "%d", client->server_port);

  int ret = getaddrinfo(client->server_address, port_str, &hints, &server_info);
  if (ret != 0) {
    ntp_log(LOG_ERROR, "getaddrinfo failed for %s: %d", client->server_address,
            ret);
    goto cleanup;
  }

  sock = (int)socket(server_info->ai_family, server_info->ai_socktype,
                     server_info->ai_protocol);
  if (sock < 0) {
    ntp_log(LOG_ERROR, "socket creation failed");
    goto cleanup;
  }

  /* 设置超时 */
  struct timeval timeout;
  timeout.tv_sec = 5;
  timeout.tv_usec = 0;
  setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout,
             sizeof(timeout));

  /* 构建NTP请求包 */
  memset(&packet, 0, sizeof(packet));
  packet.li_vn_mode = (0 << 6) | (NTP_VERSION << 3) | NTP_MODE_CLIENT;

  /* 记录发送时间 (T1) */
  uint64_t t1 = get_current_time_ns();
  ns_to_ntp(t1, &packet.transmit_timestamp);
  packet.transmit_timestamp.seconds =
      htonl_swap(packet.transmit_timestamp.seconds);
  packet.transmit_timestamp.fraction =
      htonl_swap(packet.transmit_timestamp.fraction);

  /* 发送请求 */
  ret = sendto(sock, (const char *)&packet, sizeof(packet), 0,
               server_info->ai_addr, (int)server_info->ai_addrlen);
  if (ret < 0) {
    ntp_log(LOG_ERROR, "sendto failed");
    goto cleanup;
  }

  /* 接收响应 */
  struct sockaddr_storage from_addr;
  socklen_t from_len = sizeof(from_addr);
  ret = recvfrom(sock, (char *)&packet, sizeof(packet), 0,
                 (struct sockaddr *)&from_addr, &from_len);

  /* 记录接收时间 (T4) */
  uint64_t t4 = get_current_time_ns();

  if (ret < sizeof(packet)) {
    ntp_log(LOG_ERROR, "recvfrom failed or incomplete packet");
    goto cleanup;
  }

  /* 解析响应 */
  ntp_timestamp_t t2, t3;
  t2.seconds = ntohl_swap(packet.receive_timestamp.seconds);
  t2.fraction = ntohl_swap(packet.receive_timestamp.fraction);
  t3.seconds = ntohl_swap(packet.transmit_timestamp.seconds);
  t3.fraction = ntohl_swap(packet.transmit_timestamp.fraction);

  uint64_t t2_ns = ntp_to_ns(&t2);
  uint64_t t3_ns = ntp_to_ns(&t3);

  /* 计算时间偏移: offset = ((T2 - T1) + (T3 - T4)) / 2 */
  int64_t offset = ((int64_t)(t2_ns - t1) + (int64_t)(t3_ns - t4)) / 2;

  /* 更新客户端状态 */
  client->time_offset_ns = offset;
  client->last_sync_local_time = t4;
  client->last_sync_time = t3;
  client->is_synced = true;
  client->sync_count++;

  ntp_log(LOG_INFO, "NTP sync successful (offset: %lld ms, count: %u)",
          offset / 1000000, client->sync_count);

  success = true;

cleanup:
  if (server_info) {
    freeaddrinfo(server_info);
  }
  if (sock >= 0) {
#ifdef _WIN32
    closesocket(sock);
#else
    close(sock);
#endif
  }

  if (!success) {
    client->error_count++;
  }

  return success;
}

/* 获取当前的NTP时间戳 */
bool ntp_client_get_time(ntp_client_t *client, ntp_timestamp_t *timestamp) {
  if (!client || !timestamp || !client->is_synced) {
    return false;
  }

  /* 计算当前NTP时间 = 最后同步时间 + (当前本地时间 - 最后同步本地时间) + 偏移
   */
  uint64_t current_local = get_current_time_ns();
  uint64_t elapsed = current_local - client->last_sync_local_time;
  uint64_t current_ntp_ns = ntp_to_ns(&client->last_sync_time) + elapsed;

  ns_to_ntp(current_ntp_ns, timestamp);

  return true;
}

/* 获取时间偏移 */
int64_t ntp_client_get_offset(ntp_client_t *client) {
  if (!client) {
    return 0;
  }
  return client->time_offset_ns;
}

/* 检查是否需要重新同步 */
bool ntp_client_needs_resync(ntp_client_t *client, uint32_t max_age_seconds) {
  if (!client || !client->is_synced) {
    return true;
  }

  uint64_t current = get_current_time_ns();
  uint64_t age_ns = current - client->last_sync_local_time;
  uint64_t max_age_ns = (uint64_t)max_age_seconds * 1000000000ULL;

  return age_ns > max_age_ns;
}

/* 销毁NTP客户端 */
void ntp_client_destroy(ntp_client_t *client) {
  if (!client) {
    return;
  }

  ntp_log(LOG_INFO, "NTP client destroyed (syncs: %u, errors: %u)",
          client->sync_count, client->error_count);

  memset(client, 0, sizeof(ntp_client_t));
}
