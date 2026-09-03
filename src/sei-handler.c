/******************************************************************************
    SEI Handler Module - Implementation
    Copyright (C) 2026

    Handles SEI (Supplemental Enhancement Information) construction and parsing
******************************************************************************/

#include "sei-handler.h"
#include <obs-module.h> /* blog, bmalloc, LOG_ERROR, LOG_DEBUG */
#include <stdlib.h>
#include <string.h>

/* UUID for our custom SEI: a5b3c2d1-e4f5-6789-abcd-ef0123456789 */
const uint8_t SEI_STAMPER_UUID[16] = {0xa5, 0xb3, 0xc2, 0xd1, 0xe4, 0xf5,
                                      0x67, 0x89, 0xab, 0xcd, 0xef, 0x01,
                                      0x23, 0x45, 0x67, 0x89};

/* Logging macros */
#define sei_log(level, format, ...)                                            \
  blog(level, "[SEI Handler] " format, ##__VA_ARGS__)

/* Helper: write a variable-length code (used for the SEI size) */
static size_t write_variable_length(uint8_t *buf, size_t value) {
  size_t written = 0;
  while (value >= 0xFF) {
    buf[written++] = 0xFF;
    value -= 0xFF;
  }
  buf[written++] = (uint8_t)value;
  return written;
}

/* Helper: read a variable-length code */
static size_t read_variable_length(const uint8_t *buf, size_t max_size,
                                   size_t *value_out) {
  size_t value = 0;
  size_t read = 0;

  while (read < max_size && buf[read] == 0xFF) {
    value += 0xFF;
    read++;
  }

  if (read < max_size) {
    value += buf[read];
    read++;
  }

  *value_out = value;
  return read;
}

/* Build the NTP timestamp SEI payload */
bool build_ntp_sei_payload(int64_t pts, const ntp_timestamp_t *ntp_time,
                           uint8_t **payload_out, size_t *payload_size) {
  if (!ntp_time || !payload_out || !payload_size) {
    sei_log(LOG_ERROR, "Invalid parameters for build_ntp_sei_payload");
    return false;
  }

  /* Payload layout:
   * - UUID: 16 bytes
   * - PTS: 8 bytes (int64_t, big-endian)
   * - NTP seconds: 4 bytes (uint32_t, big-endian)
   * - NTP fraction: 4 bytes (uint32_t, big-endian)
   * Total: 32 bytes
   */
  size_t payload_sz = 16 + 8 + 4 + 4;
  uint8_t *payload = (uint8_t *)bmalloc(payload_sz);
  if (!payload) {
    sei_log(LOG_ERROR, "Failed to allocate memory for SEI payload");
    return false;
  }

  size_t offset = 0;

  /* UUID */
  memcpy(payload + offset, SEI_STAMPER_UUID, 16);
  offset += 16;

  /* PTS (big-endian) */
  payload[offset++] = (uint8_t)((pts >> 56) & 0xFF);
  payload[offset++] = (uint8_t)((pts >> 48) & 0xFF);
  payload[offset++] = (uint8_t)((pts >> 40) & 0xFF);
  payload[offset++] = (uint8_t)((pts >> 32) & 0xFF);
  payload[offset++] = (uint8_t)((pts >> 24) & 0xFF);
  payload[offset++] = (uint8_t)((pts >> 16) & 0xFF);
  payload[offset++] = (uint8_t)((pts >> 8) & 0xFF);
  payload[offset++] = (uint8_t)(pts & 0xFF);

  /* NTP seconds (big-endian) */
  payload[offset++] = (uint8_t)((ntp_time->seconds >> 24) & 0xFF);
  payload[offset++] = (uint8_t)((ntp_time->seconds >> 16) & 0xFF);
  payload[offset++] = (uint8_t)((ntp_time->seconds >> 8) & 0xFF);
  payload[offset++] = (uint8_t)(ntp_time->seconds & 0xFF);

  /* NTP fraction (big-endian) */
  payload[offset++] = (uint8_t)((ntp_time->fraction >> 24) & 0xFF);
  payload[offset++] = (uint8_t)((ntp_time->fraction >> 16) & 0xFF);
  payload[offset++] = (uint8_t)((ntp_time->fraction >> 8) & 0xFF);
  payload[offset++] = (uint8_t)(ntp_time->fraction & 0xFF);

  *payload_out = payload;
  *payload_size = payload_sz;

  return true;
}

/* Build a complete SEI NAL unit */
bool build_sei_nal_unit(const uint8_t *payload, size_t payload_size,
                        sei_nal_type_t nal_type, uint8_t **nal_unit_out,
                        size_t *nal_unit_size) {
  if (!payload || !nal_unit_out || !nal_unit_size) {
    sei_log(LOG_ERROR, "Invalid parameters for build_sei_nal_unit");
    return false;
  }

  /* NAL unit layout (Annex B):
   * - start code: 0x00 0x00 0x00 0x01 (4 bytes, not part of the RBSP)
   * - NAL header: 1 byte (H.264) or 2 bytes (H.265)
   * - SEI payloadType: variable-length code
   * - SEI payloadSize: the raw (pre-escaping) byte count, per ISO/IEC 23008-2
   * - Payload: must be EPB (Emulation Prevention Bytes) escaped
   * - RBSP trailing bits: 0x80 (1 byte)
   */

  size_t header_size = (nal_type == SEI_NAL_H264) ? 1 : 2;
  uint8_t type_buf[10];
  uint8_t size_buf[10];

  size_t type_len =
      write_variable_length(type_buf, SEI_TYPE_USER_DATA_UNREGISTERED);
  /* payloadSize is the raw RBSP length, before EPB escaping, per the spec */
  size_t size_len = write_variable_length(size_buf, payload_size);

  /* Worst case inserts an EPB (0x03) per 2-byte 00 run, so the payload can
   * grow 1.5x. Reserve 4(start) + header + type + size + payload*1.5 +
   * 4 margin + 1(trailing). */
  size_t max_escaped_payload = payload_size + (payload_size / 2) + 4;
  size_t max_total = 4 + header_size + type_len + size_len + max_escaped_payload + 1;
  uint8_t *nal_unit = (uint8_t *)bmalloc(max_total);
  if (!nal_unit) {
    sei_log(LOG_ERROR, "Failed to allocate memory for SEI NAL unit");
    return false;
  }

  size_t offset = 0;

  /* Annex B start code: not part of the RBSP, so not EPB-counted*/
  nal_unit[offset++] = 0x00;
  nal_unit[offset++] = 0x00;
  nal_unit[offset++] = 0x00;
  nal_unit[offset++] = 0x01;

  /* NAL header */
  if (nal_type == SEI_NAL_H264) {
    /* H.264: forbidden_bit(1) + nal_ref_idc(2) + nal_unit_type(5) */
    nal_unit[offset++] = (0 << 7) | (0 << 5) | SEI_NAL_H264;
  } else {
    /* H.265: forbidden_zero(1) + nal_unit_type(6) + nuh_layer_id(6) + nuh_temporal_id_plus1(3)
     * Type=39(Prefix SEI) → byte1: 0x4E, LayerId=0,TID=1 → byte2: 0x01 */
    nal_unit[offset++] = (0 << 7) | (nal_type << 1) | 0; /* 0x4E */
    nal_unit[offset++] = (0 << 5) | 1;                   /* 0x01 */
  }

  /* SEI payloadType */
  memcpy(nal_unit + offset, type_buf, type_len);
  offset += type_len;

  /* SEI payloadSize (the raw, unescaped length)*/
  memcpy(nal_unit + offset, size_buf, size_len);
  offset += size_len;

  /* Payload (EPB escaping required)
   * EPB rule (ISO/IEC 23008-2 §7.4.1):
   *   where an RBSP contains the sequence 00 00 {00 | 01 | 02 | 03},
   *   insert emulation prevention byte 0x03 before the third byte.
   * The zero run resets after NAL header+type+size: their fixed values
   * never end in 00. Strictly zero_count should be tracked across the whole
   * RBSP; starting at 0 is conservative and sufficient for the 32-byte
   * UUID+timestamp payload */
  int zero_count = 0; /* consecutive 0x00 bytes in the current RBSP */
  for (size_t i = 0; i < payload_size; i++) {
    uint8_t b = payload[i];
    /* >=2 consecutive 00s and this byte <= 0x03: insert an EPB */
    if (zero_count >= 2 && b <= 0x03) {
      nal_unit[offset++] = 0x03;
      zero_count = 0;
    }
    nal_unit[offset++] = b;
    zero_count = (b == 0x00) ? (zero_count + 1) : 0;
  }

  /* RBSP trailing bits: 0x80 (a 1 bit, then zeros to byte-align)*/
  nal_unit[offset++] = 0x80;

  *nal_unit_out = nal_unit;
  *nal_unit_size = offset;

  sei_log(LOG_DEBUG, "Built SEI NAL unit (%zu bytes, raw payload=%zu bytes)",
          offset, payload_size);

  return true;
}


/* Merge SEI data */
bool merge_sei_data(const uint8_t *original_sei, size_t original_size,
                    const uint8_t *custom_sei, size_t custom_size,
                    uint8_t **merged_sei_out, size_t *merged_size) {
  if (!custom_sei || !merged_sei_out || !merged_size) {
    sei_log(LOG_ERROR, "Invalid parameters for merge_sei_data");
    return false;
  }

  /* With no original SEI, return the custom SEI as-is */
  if (!original_sei || original_size == 0) {
    uint8_t *merged = (uint8_t *)bmalloc(custom_size);
    if (!merged) {
      return false;
    }
    memcpy(merged, custom_sei, custom_size);
    *merged_sei_out = merged;
    *merged_size = custom_size;
    return true;
  }

  /* Merge: custom SEI + original SEI */
  size_t total_size = custom_size + original_size;
  uint8_t *merged = (uint8_t *)bmalloc(total_size);
  if (!merged) {
    sei_log(LOG_ERROR, "Failed to allocate memory for merged SEI");
    return false;
  }

  memcpy(merged, custom_sei, custom_size);
  memcpy(merged + custom_size, original_sei, original_size);

  *merged_sei_out = merged;
  *merged_size = total_size;

  sei_log(LOG_DEBUG, "Merged SEI data (custom: %zu, original: %zu, total: %zu)",
          custom_size, original_size, total_size);

  return true;
}

/* Parse an NTP timestamp out of an SEI payload */
bool parse_ntp_sei(const uint8_t *sei_data, size_t sei_size,
                   ntp_sei_data_t *ntp_data_out) {
  if (!sei_data || !ntp_data_out) {
    return false;
  }

  uint8_t unescaped[2048];
  size_t unescaped_size = 0;
  for (size_t i = 0; i < sei_size; i++) {
    if (i >= 2 && sei_data[i] == 0x03 && sei_data[i-1] == 0 && sei_data[i-2] == 0) {
      continue;
    }
    if (unescaped_size < sizeof(unescaped)) {
      unescaped[unescaped_size++] = sei_data[i];
    }
  }

  /* Look for our UUID */
  for (size_t i = 0; i + 32 <= unescaped_size; i++) {
    if (memcmp(unescaped + i, SEI_STAMPER_UUID, 16) == 0) {
      /* Found it - parse the data */
      size_t offset = i;

      /* UUID */
      memcpy(ntp_data_out->uuid, unescaped + offset, 16);
      offset += 16;

      /* PTS */
      int64_t pts = 0;
      pts |= ((int64_t)unescaped[offset++] << 56);
      pts |= ((int64_t)unescaped[offset++] << 48);
      pts |= ((int64_t)unescaped[offset++] << 40);
      pts |= ((int64_t)unescaped[offset++] << 32);
      pts |= ((int64_t)unescaped[offset++] << 24);
      pts |= ((int64_t)unescaped[offset++] << 16);
      pts |= ((int64_t)unescaped[offset++] << 8);
      pts |= (int64_t)unescaped[offset++];
      ntp_data_out->pts = pts;

      /* NTP seconds */
      uint32_t seconds = 0;
      seconds |= ((uint32_t)unescaped[offset++] << 24);
      seconds |= ((uint32_t)unescaped[offset++] << 16);
      seconds |= ((uint32_t)unescaped[offset++] << 8);
      seconds |= (uint32_t)unescaped[offset++];
      ntp_data_out->ntp_time.seconds = seconds;

      /* NTP fraction */
      uint32_t fraction = 0;
      fraction |= ((uint32_t)unescaped[offset++] << 24);
      fraction |= ((uint32_t)unescaped[offset++] << 16);
      fraction |= ((uint32_t)unescaped[offset++] << 8);
      fraction |= (uint32_t)unescaped[offset++];
      ntp_data_out->ntp_time.fraction = fraction;

      sei_log(LOG_DEBUG, "Parsed NTP SEI (PTS: %lld, NTP: %u.%u)", pts, seconds,
              fraction);

      return true;
    }
  }

  return false;
}

/* Extract the SEI payload from a NAL unit */
static const uint8_t *find_start_code_sei(const uint8_t *data, size_t size, size_t *sc_size) {
  if (size < 3) return NULL;
  for (size_t i = 0; i < size - 2; i++) {
    if (data[i] == 0 && data[i+1] == 0) {
      if (data[i+2] == 1) {
        *sc_size = 3; return data + i;
      } else if (i < size - 3 && data[i+2] == 0 && data[i+3] == 1) {
        *sc_size = 4; return data + i;
      }
    }
  }
  return NULL;
}

bool extract_sei_payload(const uint8_t *nal_data, size_t nal_size,
                         const uint8_t **payload_out, size_t *payload_size) {
  if (!nal_data || !payload_out || !payload_size || nal_size < 5) {
    return false;
  }

  const uint8_t *current = nal_data;
  size_t remaining = nal_size;

  while (remaining > 0) {
    size_t sc_size = 0;
    const uint8_t *nal_start = find_start_code_sei(current, remaining, &sc_size);
    if (!nal_start) break;

    const uint8_t *data = nal_start + sc_size;
    size_t data_rem = remaining - (data - current);
    if (data_rem < 2) break; /* Need at least 2 bytes for H.265 header */

    uint8_t nal_type_byte = data[0];
    uint8_t nal_type = nal_type_byte & 0x1F;
    size_t offset = 0;
    bool is_sei = false;

    if (nal_type == SEI_NAL_H264) {
      is_sei = true;
      offset = 1;
    } else {
      uint8_t h265_type = (nal_type_byte >> 1) & 0x3F;
      if (h265_type == SEI_NAL_H265_PREFIX || h265_type == SEI_NAL_H265_SUFFIX) {
        is_sei = true;
        offset = 2;
      }
    }

    if (is_sei && data_rem > offset) {
      size_t sei_type;
      size_t type_read = read_variable_length(data + offset, data_rem - offset, &sei_type);
      offset += type_read;
      
      size_t sei_size;
      size_t size_read = read_variable_length(data + offset, data_rem - offset, &sei_size);
      offset += size_read;

      if (sei_type == SEI_TYPE_USER_DATA_UNREGISTERED && offset + sei_size <= data_rem) {
        *payload_out = data + offset;
        *payload_size = data_rem - offset;
        return true;
      }
    }

    current = nal_start + 1; // Advance past current start code
    remaining = nal_size - (current - nal_data);
  }

  return false;
}
