/******************************************************************************
    SEI Handler Module - Implementation
    Copyright (C) 2026

    Handles SEI (Supplemental Enhancement Information) construction and parsing
******************************************************************************/

#include "sei-handler.h"
#include <obs-module.h>
#include <stdlib.h>
#include <string.h>

/* UUID for our custom SEI: a5b3c2d1-e4f5-6789-abcd-ef0123456789 */
const uint8_t SEI_STAMPER_UUID[16] = {0xa5, 0xb3, 0xc2, 0xd1, 0xe4, 0xf5,
                                      0x67, 0x89, 0xab, 0xcd, 0xef, 0x01,
                                      0x23, 0x45, 0x67, 0x89};

/* 日志宏 */
#define sei_log(level, format, ...)                                            \
  blog(level, "[SEI Handler] " format, ##__VA_ARGS__)

/* 辅助函数:写入可变长度编码(用于SEI size) */
static size_t write_variable_length(uint8_t *buf, size_t value) {
  size_t written = 0;
  while (value >= 0xFF) {
    buf[written++] = 0xFF;
    value -= 0xFF;
  }
  buf[written++] = (uint8_t)value;
  return written;
}

/* 辅助函数:读取可变长度编码 */
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

/* 构建NTP时间戳SEI payload */
bool build_ntp_sei_payload(int64_t pts, const ntp_timestamp_t *ntp_time,
                           uint8_t **payload_out, size_t *payload_size) {
  if (!ntp_time || !payload_out || !payload_size) {
    sei_log(LOG_ERROR, "Invalid parameters for build_ntp_sei_payload");
    return false;
  }

  /* Payload结构:
   * - UUID: 16字节
   * - PTS: 8字节(int64_t, big-endian)
   * - NTP seconds: 4字节(uint32_t, big-endian)
   * - NTP fraction: 4字节(uint32_t, big-endian)
   * 总计: 32字节
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

/* 构建完整的SEI NAL单元 */
bool build_sei_nal_unit(const uint8_t *payload, size_t payload_size,
                        sei_nal_type_t nal_type, uint8_t **nal_unit_out,
                        size_t *nal_unit_size) {
  if (!payload || !nal_unit_out || !nal_unit_size) {
    sei_log(LOG_ERROR, "Invalid parameters for build_sei_nal_unit");
    return false;
  }

  /* NAL单元结构:
   * - 起始码: 0x00 0x00 0x00 0x01 (4字节)
   * - NAL header: 1字节(H.264)或2字节(H.265)
   * - SEI type: 可变长度编码
   * - SEI size: 可变长度编码
   * - Payload: payload_size字节
   * - RBSP trailing bits: 0x80 (1字节)
   */

  size_t header_size = (nal_type == SEI_NAL_H264) ? 1 : 2;
  uint8_t type_buf[10];
  uint8_t size_buf[10];

  size_t type_len =
      write_variable_length(type_buf, SEI_TYPE_USER_DATA_UNREGISTERED);
  size_t size_len = write_variable_length(size_buf, payload_size);

  size_t total_size = 4 + header_size + type_len + size_len + payload_size + 1;
  uint8_t *nal_unit = (uint8_t *)bmalloc(total_size);
  if (!nal_unit) {
    sei_log(LOG_ERROR, "Failed to allocate memory for SEI NAL unit");
    return false;
  }

  size_t offset = 0;

  /* 起始码 */
  nal_unit[offset++] = 0x00;
  nal_unit[offset++] = 0x00;
  nal_unit[offset++] = 0x00;
  nal_unit[offset++] = 0x01;

  /* NAL header */
  if (nal_type == SEI_NAL_H264) {
    /* H.264: forbidden_bit(1) + nal_ref_idc(2) + nal_unit_type(5) */
    nal_unit[offset++] = (0 << 7) | (0 << 5) | SEI_NAL_H264;
  } else {
    /* H.265: forbidden_bit(1) + nal_unit_type(6) + nuh_layer_id(6) +
     * nuh_temporal_id_plus1(3) */
    nal_unit[offset++] = (0 << 7) | (nal_type << 1) | 0;
    nal_unit[offset++] = (0 << 5) | 1; /* temporal_id = 0 */
  }

  /* SEI type */
  memcpy(nal_unit + offset, type_buf, type_len);
  offset += type_len;

  /* SEI size */
  memcpy(nal_unit + offset, size_buf, size_len);
  offset += size_len;

  /* Payload */
  memcpy(nal_unit + offset, payload, payload_size);
  offset += payload_size;

  /* RBSP trailing bits */
  nal_unit[offset++] = 0x80;

  *nal_unit_out = nal_unit;
  *nal_unit_size = offset;

  sei_log(LOG_DEBUG, "Built SEI NAL unit (%zu bytes)", offset);

  return true;
}

/* 合并SEI数据 */
bool merge_sei_data(const uint8_t *original_sei, size_t original_size,
                    const uint8_t *custom_sei, size_t custom_size,
                    uint8_t **merged_sei_out, size_t *merged_size) {
  if (!custom_sei || !merged_sei_out || !merged_size) {
    sei_log(LOG_ERROR, "Invalid parameters for merge_sei_data");
    return false;
  }

  /* 如果没有原始SEI,直接返回自定义SEI */
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

  /* 合并:自定义SEI + 原始SEI */
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

/* 从SEI payload中解析NTP时间戳 */
bool parse_ntp_sei(const uint8_t *sei_data, size_t sei_size,
                   ntp_sei_data_t *ntp_data_out) {
  if (!sei_data || !ntp_data_out) {
    return false;
  }

  /* 查找我们的UUID */
  for (size_t i = 0; i + 32 <= sei_size; i++) {
    if (memcmp(sei_data + i, SEI_STAMPER_UUID, 16) == 0) {
      /* 找到了!解析数据 */
      size_t offset = i;

      /* UUID */
      memcpy(ntp_data_out->uuid, sei_data + offset, 16);
      offset += 16;

      /* PTS */
      int64_t pts = 0;
      pts |= ((int64_t)sei_data[offset++] << 56);
      pts |= ((int64_t)sei_data[offset++] << 48);
      pts |= ((int64_t)sei_data[offset++] << 40);
      pts |= ((int64_t)sei_data[offset++] << 32);
      pts |= ((int64_t)sei_data[offset++] << 24);
      pts |= ((int64_t)sei_data[offset++] << 16);
      pts |= ((int64_t)sei_data[offset++] << 8);
      pts |= (int64_t)sei_data[offset++];
      ntp_data_out->pts = pts;

      /* NTP seconds */
      uint32_t seconds = 0;
      seconds |= ((uint32_t)sei_data[offset++] << 24);
      seconds |= ((uint32_t)sei_data[offset++] << 16);
      seconds |= ((uint32_t)sei_data[offset++] << 8);
      seconds |= (uint32_t)sei_data[offset++];
      ntp_data_out->ntp_time.seconds = seconds;

      /* NTP fraction */
      uint32_t fraction = 0;
      fraction |= ((uint32_t)sei_data[offset++] << 24);
      fraction |= ((uint32_t)sei_data[offset++] << 16);
      fraction |= ((uint32_t)sei_data[offset++] << 8);
      fraction |= (uint32_t)sei_data[offset++];
      ntp_data_out->ntp_time.fraction = fraction;

      sei_log(LOG_DEBUG, "Parsed NTP SEI (PTS: %lld, NTP: %u.%u)", pts, seconds,
              fraction);

      return true;
    }
  }

  return false;
}

/* 从NAL单元中提取SEI payload */
bool extract_sei_payload(const uint8_t *nal_data, size_t nal_size,
                         const uint8_t **payload_out, size_t *payload_size) {
  if (!nal_data || !payload_out || !payload_size || nal_size < 5) {
    return false;
  }

  size_t offset = 0;

  /* 跳过起始码 */
  if (nal_size >= 4 && nal_data[0] == 0x00 && nal_data[1] == 0x00) {
    if (nal_data[2] == 0x00 && nal_data[3] == 0x01) {
      offset = 4;
    } else if (nal_data[2] == 0x01) {
      offset = 3;
    }
  }

  if (offset == 0) {
    return false;
  }

  /* 检查NAL类型 */
  uint8_t nal_type_byte = nal_data[offset];
  uint8_t nal_type = nal_type_byte & 0x1F; /* H.264 */

  if (nal_type != SEI_NAL_H264) {
    /* 尝试H.265 */
    nal_type = (nal_type_byte >> 1) & 0x3F;
    if (nal_type != SEI_NAL_H265_PREFIX && nal_type != SEI_NAL_H265_SUFFIX) {
      return false;
    }
    offset += 2; /* H.265有2字节头 */
  } else {
    offset += 1; /* H.264有1字节头 */
  }

  /* 读取SEI type */
  size_t sei_type;
  offset +=
      read_variable_length(nal_data + offset, nal_size - offset, &sei_type);

  /* 读取SEI size */
  size_t sei_size;
  offset +=
      read_variable_length(nal_data + offset, nal_size - offset, &sei_size);

  /* 验证大小 */
  if (offset + sei_size > nal_size) {
    return false;
  }

  *payload_out = nal_data + offset;
  *payload_size = sei_size;

  return true;
}
