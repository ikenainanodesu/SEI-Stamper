/******************************************************************************
    SEI Handler Module - Header File
    Copyright (C) 2026

    Handles SEI (Supplemental Enhancement Information) construction and parsing
    for NTP timestamp embedding in H.264/H.265 video streams
******************************************************************************/

#pragma once

#include "ntp-client.h"
#include <stdbool.h>
#include <stdint.h>


#ifdef __cplusplus
extern "C" {
#endif

/* UUID for our custom SEI (用于识别我们的自定义SEI) */
/* 格式: a5b3c2d1-e4f5-6789-abcd-ef0123456789 */
extern const uint8_t SEI_STAMPER_UUID[16];

/* NTP时间戳SEI数据结构 */
typedef struct ntp_sei_data {
  uint8_t uuid[16];         /* UUID标识符 */
  int64_t pts;              /* 帧的PTS */
  ntp_timestamp_t ntp_time; /* NTP时间戳 */
} ntp_sei_data_t;

/* SEI NAL单元类型 */
typedef enum sei_nal_type {
  SEI_NAL_H264 = 6,         /* H.264 SEI NAL单元类型 */
  SEI_NAL_H265_PREFIX = 39, /* H.265 PREFIX_SEI_NUT */
  SEI_NAL_H265_SUFFIX = 40  /* H.265 SUFFIX_SEI_NUT */
} sei_nal_type_t;

/* SEI payload类型 */
#define SEI_TYPE_USER_DATA_UNREGISTERED 5

/*
 * 构建NTP时间戳SEI payload
 * 参数:
 *   pts - 当前帧的PTS
 *   ntp_time - NTP时间戳
 *   payload_out - 输出的SEI payload数据(需要调用者释放)
 *   payload_size - 输出的payload大小
 * 返回:
 *   true - 成功
 *   false - 失败
 */
bool build_ntp_sei_payload(int64_t pts, const ntp_timestamp_t *ntp_time,
                           uint8_t **payload_out, size_t *payload_size);

/*
 * 构建完整的SEI NAL单元(包含起始码)
 * 参数:
 *   payload - SEI payload数据
 *   payload_size - payload大小
 *   nal_type - NAL单元类型(H264或H265)
 *   nal_unit_out - 输出的完整NAL单元(需要调用者释放)
 *   nal_unit_size - 输出的NAL单元大小
 * 返回:
 *   true - 成功
 *   false - 失败
 */
bool build_sei_nal_unit(const uint8_t *payload, size_t payload_size,
                        sei_nal_type_t nal_type, uint8_t **nal_unit_out,
                        size_t *nal_unit_size);

/*
 * 合并SEI数据(将自定义SEI与原有SEI合并)
 * 参数:
 *   original_sei - 原始SEI数据(可以为NULL)
 *   original_size - 原始SEI大小
 *   custom_sei - 自定义SEI数据
 *   custom_size - 自定义SEI大小
 *   merged_sei_out - 输出的合并后SEI(需要调用者释放)
 *   merged_size - 输出的合并后大小
 * 返回:
 *   true - 成功
 *   false - 失败
 */
bool merge_sei_data(const uint8_t *original_sei, size_t original_size,
                    const uint8_t *custom_sei, size_t custom_size,
                    uint8_t **merged_sei_out, size_t *merged_size);

/*
 * 从SEI payload中解析NTP时间戳
 * 参数:
 *   sei_data - SEI数据
 *   sei_size - SEI数据大小
 *   ntp_data_out - 输出的NTP SEI数据
 * 返回:
 *   true - 成功找到并解析
 *   false - 未找到或解析失败
 */
bool parse_ntp_sei(const uint8_t *sei_data, size_t sei_size,
                   ntp_sei_data_t *ntp_data_out);

/*
 * 从NAL单元中提取SEI payload
 * 参数:
 *   nal_data - NAL单元数据
 *   nal_size - NAL单元大小
 *   payload_out - 输出的payload数据(指向nal_data内部,不需要释放)
 *   payload_size - 输出的payload大小
 * 返回:
 *   true - 成功
 *   false - 失败
 */
bool extract_sei_payload(const uint8_t *nal_data, size_t nal_size,
                         const uint8_t **payload_out, size_t *payload_size);

#ifdef __cplusplus
}
#endif
