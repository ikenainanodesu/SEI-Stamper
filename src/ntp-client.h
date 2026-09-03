/******************************************************************************
    NTP Client Module - Header File
    Copyright (C) 2026

    Implements NTP (Network Time Protocol) client for time synchronization
******************************************************************************/

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <util/threading.h>


#ifdef __cplusplus
extern "C" {
#endif

/* NTP timestamp - 64 bits: seconds plus fractional part */
typedef struct ntp_timestamp {
  uint32_t seconds;  /* Seconds since 1900-01-01 */
  uint32_t fraction; /* Fractional seconds (units of 2^-32 s) */
} ntp_timestamp_t;

/* NTP packet layout (48 bytes) */
typedef struct ntp_packet {
  uint8_t li_vn_mode; /* Leap Indicator (2b) + Version (3b) + Mode (3b) */
  uint8_t stratum;    /* Stratum (0-15) */
  uint8_t poll;       /* Poll interval */
  uint8_t precision;  /* Precision */

  uint32_t root_delay;      /* Root delay */
  uint32_t root_dispersion; /* Root dispersion */
  uint32_t reference_id;    /* Reference clock identifier */

  ntp_timestamp_t reference_timestamp; /* Reference timestamp */
  ntp_timestamp_t originate_timestamp; /* Originate timestamp (T1) */
  ntp_timestamp_t receive_timestamp;   /* Receive timestamp (T2) */
  ntp_timestamp_t transmit_timestamp;  /* Transmit timestamp (T3) */
} ntp_packet_t;

/* NTP client context */
typedef struct ntp_client {
  char server_address[256]; /* NTP server address */
  uint16_t server_port;     /* NTP server port (usually 123) */

  int socket_fd;       /* UDP socket file descriptor */
  bool is_initialized; /* Whether initialised */
  bool is_synced;      /* Whether synced */

  ntp_timestamp_t last_sync_time; /* NTP time at the last sync */
  uint64_t last_sync_local_time;  /* os_gettime_ns at the last sync */
  int64_t time_offset_ns;         /* Time offset (ns) */

  uint32_t sync_count;  /* Sync count */
  uint32_t error_count; /* Error count */

  /* Guards the sync-result fields (last_sync_time, last_sync_local_time,
   * is_synced, time_offset_ns) so a reader gets a consistent snapshot while the
   * background thread updates them. */
  pthread_mutex_t state_lock;
  pthread_t sync_thread;
  os_event_t *sync_stop;
  bool sync_thread_active;
  uint32_t sync_interval_ms;
} ntp_client_t;

/*
 * Initialise the NTP client
 * Params:
 *   client - NTP client context
 *   server - NTP server address (e.g. "time.windows.com")
 *   port - NTP server port (usually 123)
 * Returns:
 *   true - success
 *   false - failure
 */
bool ntp_client_init(ntp_client_t *client, const char *server, uint16_t port);

/*
 * Perform an NTP time sync
 * Params:
 *   client - NTP client context
 * Returns:
 *   true - sync succeeded
 *   false - sync failed
 */
bool ntp_client_sync(ntp_client_t *client);

/*
 * Get the current NTP timestamp
 * Params:
 *   client - NTP client context
 *   timestamp - output NTP timestamp
 * Returns:
 *   true - success
 *   false - failure (not synced, or the time has expired)
 */
bool ntp_client_get_time(ntp_client_t *client, ntp_timestamp_t *timestamp);

/*
 * Get the time offset (local time - NTP time)
 * Params:
 *   client - NTP client context
 * Returns:
 *   Time offset (ns)
 */
int64_t ntp_client_get_offset(ntp_client_t *client);

/*
 * Check whether a re-sync is needed
 * Params:
 *   client - NTP client context
 *   max_age_seconds - maximum allowed age of the last sync (seconds)
 * Returns:
 *   true - a re-sync is needed
 *   false - not needed
 */
bool ntp_client_needs_resync(ntp_client_t *client, uint32_t max_age_seconds);

/*
 * Start a background thread that re-syncs every interval_ms so callers never
 * block on the network; read the result with ntp_client_get_time. Opt-in --
 * callers that sync manually (e.g. the receiver) don't call this.
 */
void ntp_client_start_background_sync(ntp_client_t *client,
                                      uint32_t interval_ms);

/*
 * Destroy the NTP client
 * Params:
 *   client - NTP client context
 */
void ntp_client_destroy(ntp_client_t *client);

#ifdef __cplusplus
}
#endif
