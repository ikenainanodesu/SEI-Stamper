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

/* UUID for our custom SEI (identifies our custom SEI) */
/* Format: a5b3c2d1-e4f5-6789-abcd-ef0123456789 */
extern const uint8_t SEI_STAMPER_UUID[16];

/* NTP timestamp SEI data structure */
typedef struct ntp_sei_data {
  uint8_t uuid[16];         /* UUID identifier */
  int64_t pts;              /* Frame PTS */
  ntp_timestamp_t ntp_time; /* NTP timestamp */
} ntp_sei_data_t;

/* SEI NAL unit type */
typedef enum sei_nal_type {
  SEI_NAL_H264 = 6,         /* H.264 SEI NAL unit type */
  SEI_NAL_H265_PREFIX = 39, /* H.265 PREFIX_SEI_NUT */
  SEI_NAL_H265_SUFFIX = 40  /* H.265 SUFFIX_SEI_NUT */
} sei_nal_type_t;

/* SEI payload type */
#define SEI_TYPE_USER_DATA_UNREGISTERED 5

/*
 * Build the NTP timestamp SEI payload
 * Params:
 *   pts - PTS of the current frame
 *   ntp_time - NTP timestamp
 *   payload_out - output SEI payload data (caller frees)
 *   payload_size - output payload size
 * Returns:
 *   true - success
 *   false - failure
 */
bool build_ntp_sei_payload(int64_t pts, const ntp_timestamp_t *ntp_time,
                           uint8_t **payload_out, size_t *payload_size);

/*
 * Build a complete SEI NAL unit (including the start code)
 * Params:
 *   payload - SEI payload data
 *   payload_size - payload size
 *   nal_type - NAL unit type (H264 or H265)
 *   nal_unit_out - output complete NAL unit (caller frees)
 *   nal_unit_size - output NAL unit size
 * Returns:
 *   true - success
 *   false - failure
 */
bool build_sei_nal_unit(const uint8_t *payload, size_t payload_size,
                        sei_nal_type_t nal_type, uint8_t **nal_unit_out,
                        size_t *nal_unit_size);

/*
 * Merge SEI data (combine our custom SEI with any existing SEI)
 * Params:
 *   original_sei - original SEI data (may be NULL)
 *   original_size - original SEI size
 *   custom_sei - custom SEI data
 *   custom_size - custom SEI size
 *   merged_sei_out - output merged SEI (caller frees)
 *   merged_size - output merged size
 * Returns:
 *   true - success
 *   false - failure
 */
bool merge_sei_data(const uint8_t *original_sei, size_t original_size,
                    const uint8_t *custom_sei, size_t custom_size,
                    uint8_t **merged_sei_out, size_t *merged_size);

/*
 * Parse an NTP timestamp out of an SEI payload
 * Params:
 *   sei_data - SEI data
 *   sei_size - SEI data size
 *   ntp_data_out - output NTP SEI data
 * Returns:
 *   true - found and parsed
 *   false - not found, or parsing failed
 */
bool parse_ntp_sei(const uint8_t *sei_data, size_t sei_size,
                   ntp_sei_data_t *ntp_data_out);

/*
 * Extract the SEI payload from a NAL unit
 * Params:
 *   nal_data - NAL unit data
 *   nal_size - NAL unit size
 *   payload_out - output payload data (points inside nal_data; no free needed)
 *   payload_size - output payload size
 * Returns:
 *   true - success
 *   false - failure
 */
bool extract_sei_payload(const uint8_t *nal_data, size_t nal_size,
                         const uint8_t **payload_out, size_t *payload_size);

#ifdef __cplusplus
}
#endif
