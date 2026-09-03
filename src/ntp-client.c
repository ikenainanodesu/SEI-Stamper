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
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#endif

/* NTP constants */
#define NTP_TIMESTAMP_DELTA 2208988800ULL /* Seconds between 1900 and 1970 */
#define NTP_VERSION 3
#define NTP_MODE_CLIENT 3
#define NTP_PACKET_SIZE 48

/* Logging macros */
#define ntp_log(level, format, ...)                                            \
  blog(level, "[NTP Client] " format, ##__VA_ARGS__)

/* Helper: network byte order to host byte order */
static uint32_t ntohl_swap(uint32_t netlong) { return ntohl(netlong); }

/* Helper: host byte order to network byte order */
static uint32_t htonl_swap(uint32_t hostlong) { return htonl(hostlong); }

/* Helper: monotonic time (ns) - for measuring intervals */
static uint64_t get_current_time_ns(void) { return os_gettime_ns(); }

/* Helper: system wall-clock time (ns, Unix epoch) */
static uint64_t get_wall_clock_ns(void) {
#ifdef _WIN32
  FILETIME ft;
  GetSystemTimeAsFileTime(&ft);
  /* FILETIME: 100ns intervals since 1601-01-01 */
  uint64_t t = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
  /* Convert to Unix epoch (subtract 11644473600 seconds) and to nanoseconds */
  t -= 116444736000000000ULL;
  return t * 100ULL;
#else
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
#endif
}

/* Helper: NTP timestamp to nanoseconds */
static uint64_t ntp_to_ns(ntp_timestamp_t *ntp) {
  uint64_t seconds = (uint64_t)ntp->seconds;
  uint64_t fraction = (uint64_t)ntp->fraction;

  /* Convert to a Unix timestamp */
  if (seconds > NTP_TIMESTAMP_DELTA) {
    seconds -= NTP_TIMESTAMP_DELTA;
  }

  /* Seconds to nanoseconds */
  uint64_t ns = seconds * 1000000000ULL;

  /* Fraction to ns: fraction / 2^32 * 10^9 */
  ns += (fraction * 1000000000ULL) >> 32;

  return ns;
}

/* Helper: nanoseconds to an NTP timestamp */
static void ns_to_ntp(uint64_t ns, ntp_timestamp_t *ntp) {
  /* Nanoseconds to seconds */
  uint64_t seconds = ns / 1000000000ULL;
  uint64_t fraction_ns = ns % 1000000000ULL;

  /* Convert to an NTP timestamp (epoch 1900) */
  ntp->seconds = (uint32_t)(seconds + NTP_TIMESTAMP_DELTA);

  /* Fraction: fraction_ns / 10^9 * 2^32 */
  ntp->fraction = (uint32_t)((fraction_ns << 32) / 1000000000ULL);
}

/* Initialise Winsock (Windows only) */
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

/* Initialise the NTP client */
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

  /* Copy the server address */
  strncpy(client->server_address, server, sizeof(client->server_address) - 1);
  client->server_port = port;
  client->socket_fd = -1;
  client->is_initialized = true;
  pthread_mutex_init(&client->state_lock, NULL);

  ntp_log(LOG_INFO, "NTP client initialized (server: %s:%d)", server, port);

  return true;
}

static int ntp_last_sockerr(void) {
#ifdef _WIN32
  return WSAGetLastError();
#else
  return errno;
#endif
}

/* One NTP round-trip against a single address; failures log at debug so the
 * caller can fall through to the next without noise. */
static bool ntp_sync_one_addr(ntp_client_t *client, struct addrinfo *ai) {
  const char *fam = ai->ai_family == AF_INET    ? "IPv4"
                    : ai->ai_family == AF_INET6 ? "IPv6"
                                                : "other";

  int sock = (int)socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
  if (sock < 0) {
    ntp_log(LOG_DEBUG, "socket() failed (%s) err=%d", fam, ntp_last_sockerr());
    return false;
  }

  bool success = false;
  ntp_packet_t packet;

  /* Windows SO_RCVTIMEO wants a DWORD of milliseconds, not a struct timeval:
   * a timeval is read as tv_sec ms (5 -> 5 ms), too short for the reply. */
#ifdef _WIN32
  DWORD timeout_ms = 5000;
  setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout_ms,
             sizeof(timeout_ms));
#else
  struct timeval timeout;
  timeout.tv_sec = 5;
  timeout.tv_usec = 0;
  setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout,
             sizeof(timeout));
#endif

  memset(&packet, 0, sizeof(packet));
  packet.li_vn_mode = (0 << 6) | (NTP_VERSION << 3) | NTP_MODE_CLIENT;

  /* Send time (T1), on wall-clock to match the server's time base */
  uint64_t t1_wall = get_wall_clock_ns();
  ns_to_ntp(t1_wall, &packet.transmit_timestamp);
  packet.transmit_timestamp.seconds =
      htonl_swap(packet.transmit_timestamp.seconds);
  packet.transmit_timestamp.fraction =
      htonl_swap(packet.transmit_timestamp.fraction);

  int ret = sendto(sock, (const char *)&packet, sizeof(packet), 0, ai->ai_addr,
                   (int)ai->ai_addrlen);
  if (ret < 0) {
    ntp_log(LOG_DEBUG, "sendto failed (%s) err=%d", fam, ntp_last_sockerr());
    goto done;
  }

  struct sockaddr_storage from_addr;
  socklen_t from_len = sizeof(from_addr);
  ret = recvfrom(sock, (char *)&packet, sizeof(packet), 0,
                 (struct sockaddr *)&from_addr, &from_len);
  int recv_err = ntp_last_sockerr();

  uint64_t t4_mono = get_current_time_ns();
  uint64_t t4_wall = get_wall_clock_ns();

  /* recvfrom returns ssize_t/int; -1 on failure. Compare against signed 0
   * first, then cast for the size check — without this, ret==-1 promotes to
   * SIZE_MAX and silently skips the error path, causing us to parse our own
   * outgoing packet as the response. */
  if (ret < 0 || (size_t)ret < sizeof(packet)) {
    ntp_log(LOG_DEBUG, "recvfrom failed (%s) ret=%d err=%d", fam, ret, recv_err);
    goto done;
  }

  /* Validate response: must be server-mode, synchronized, with a stratum in
   * the usable range (1..15). Stratum 0 is Kiss-o'-Death; 16 means the
   * server itself is unsynchronized and is freewheeling on its own RTC. */
  uint8_t li = (packet.li_vn_mode >> 6) & 0x3;
  uint8_t mode = packet.li_vn_mode & 0x7;
  if (mode != 4 || li == 3 || packet.stratum == 0 || packet.stratum >= 16) {
    ntp_log(LOG_WARNING, "Rejecting NTP response: mode=%u li=%u stratum=%u",
            mode, li, packet.stratum);
    goto done;
  }

  ntp_timestamp_t t2, t3;
  t2.seconds = ntohl_swap(packet.receive_timestamp.seconds);
  t2.fraction = ntohl_swap(packet.receive_timestamp.fraction);
  t3.seconds = ntohl_swap(packet.transmit_timestamp.seconds);
  t3.fraction = ntohl_swap(packet.transmit_timestamp.fraction);

  uint64_t t2_ns = ntp_to_ns(&t2);
  uint64_t t3_ns = ntp_to_ns(&t3);

  /* Standard NTP offset, on wall-clock time so it shares the server's base. */
  int64_t offset = ((int64_t)(t2_ns - t1_wall) + (int64_t)(t3_ns - t4_wall)) / 2;

  pthread_mutex_lock(&client->state_lock);
  client->time_offset_ns = offset;
  client->last_sync_local_time = t4_mono;
  client->last_sync_time = t3;
  client->is_synced = true;
  client->sync_count++;
  pthread_mutex_unlock(&client->state_lock);

  ntp_log(LOG_INFO, "NTP sync successful (offset: %lld ms, count: %u)",
          offset / 1000000, client->sync_count);
  success = true;

done:
#ifdef _WIN32
  closesocket(sock);
#else
  close(sock);
#endif
  return success;
}

/* getaddrinfo(AF_UNSPEC) may return an unreachable address first (e.g. IPv6 on
 * an IPv4-only host), so try every resolved address, not just the first. */
bool ntp_client_sync(ntp_client_t *client) {
  if (!client || !client->is_initialized) {
    ntp_log(LOG_ERROR, "Client not initialized");
    return false;
  }

  struct addrinfo hints;
  struct addrinfo *server_info = NULL;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC; /* IPv4 or IPv6 */
  hints.ai_socktype = SOCK_DGRAM;

  char port_str[16];
  snprintf(port_str, sizeof(port_str), "%d", client->server_port);

  int ret = getaddrinfo(client->server_address, port_str, &hints, &server_info);
  if (ret != 0) {
    ntp_log(LOG_ERROR, "getaddrinfo failed for %s: %d", client->server_address,
            ret);
    client->error_count++;
    return false;
  }

  bool success = false;
  int tried = 0;
  for (struct addrinfo *ai = server_info; ai != NULL && !success;
       ai = ai->ai_next) {
    tried++;
    success = ntp_sync_one_addr(client, ai);
  }
  freeaddrinfo(server_info);

  if (!success) {
    client->error_count++;
    ntp_log(LOG_WARNING, "NTP sync failed: no address answered (tried %d)",
            tried);
  }

  return success;
}

/* Background sync loop: re-sync on an interval, waking early to stop. */
static void *ntp_sync_thread(void *arg) {
  ntp_client_t *client = (ntp_client_t *)arg;
  while (true) {
    ntp_client_sync(client);
    if (os_event_timedwait(client->sync_stop, client->sync_interval_ms) == 0) {
      break; /* signaled to stop */
    }
  }
  return NULL;
}

void ntp_client_start_background_sync(ntp_client_t *client,
                                      uint32_t interval_ms) {
  if (!client || !client->is_initialized || client->sync_thread_active) {
    return;
  }
  client->sync_interval_ms = interval_ms ? interval_ms : 60000;
  if (os_event_init(&client->sync_stop, OS_EVENT_TYPE_MANUAL) != 0) {
    ntp_log(LOG_ERROR, "Failed to create NTP sync stop event");
    return;
  }
  if (pthread_create(&client->sync_thread, NULL, ntp_sync_thread, client) != 0) {
    ntp_log(LOG_ERROR, "Failed to start NTP sync thread");
    os_event_destroy(client->sync_stop);
    client->sync_stop = NULL;
    return;
  }
  client->sync_thread_active = true;
  ntp_log(LOG_INFO, "NTP background sync started (interval %u ms)",
          client->sync_interval_ms);
}

/* Get the current NTP timestamp */
bool ntp_client_get_time(ntp_client_t *client, ntp_timestamp_t *timestamp) {
  if (!client || !timestamp) {
    return false;
  }

  /* Snapshot the sync result under the lock so the background thread can't
   * update it mid-read and hand us a torn timestamp. */
  pthread_mutex_lock(&client->state_lock);
  bool synced = client->is_synced;
  ntp_timestamp_t sync_time = client->last_sync_time;
  uint64_t sync_local = client->last_sync_local_time;
  pthread_mutex_unlock(&client->state_lock);

  if (!synced) {
    ntp_log(LOG_WARNING, "get_time called but not synced yet");
    return false;
  }

  /* Current NTP time = server time at last sync + monotonic time since then */
  uint64_t current_local = get_current_time_ns();
  uint64_t elapsed = current_local - sync_local;
  uint64_t base_ns = ntp_to_ns(&sync_time);
  uint64_t current_ntp_ns = base_ns + elapsed;

  ns_to_ntp(current_ntp_ns, timestamp);

  /* Sanity check: NTP seconds should be past 2020 (NTP: 3786825600) */
  if (timestamp->seconds < 3786825600UL) {
    ntp_log(LOG_WARNING,
            "NTP time sanity check FAILED: seconds=%u (expected >3786825600). "
            "base_ns=%llu, elapsed=%llu, last_sync_time=%u.%u",
            timestamp->seconds, (unsigned long long)base_ns,
            (unsigned long long)elapsed, sync_time.seconds, sync_time.fraction);
    return false;
  }

  return true;
}

/* Get the time offset */
int64_t ntp_client_get_offset(ntp_client_t *client) {
  if (!client) {
    return 0;
  }
  return client->time_offset_ns;
}

/* Check whether a re-sync is needed */
bool ntp_client_needs_resync(ntp_client_t *client, uint32_t max_age_seconds) {
  if (!client || !client->is_synced) {
    return true;
  }

  uint64_t current = get_current_time_ns();
  uint64_t age_ns = current - client->last_sync_local_time;
  uint64_t max_age_ns = (uint64_t)max_age_seconds * 1000000000ULL;

  return age_ns > max_age_ns;
}

/* Destroy the NTP client */
void ntp_client_destroy(ntp_client_t *client) {
  if (!client) {
    return;
  }

  if (client->sync_thread_active) {
    os_event_signal(client->sync_stop);
    pthread_join(client->sync_thread, NULL);
    os_event_destroy(client->sync_stop);
    client->sync_stop = NULL;
    client->sync_thread_active = false;
  }
  if (client->is_initialized) {
    pthread_mutex_destroy(&client->state_lock);
  }

  ntp_log(LOG_INFO, "NTP client destroyed (syncs: %u, errors: %u)",
          client->sync_count, client->error_count);

  memset(client, 0, sizeof(ntp_client_t));
}
