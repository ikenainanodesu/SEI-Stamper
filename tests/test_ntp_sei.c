/*
 * Unit tests for SEI-Stamper pure functions.
 *
 * Compiles standalone — no OBS or GPU dependencies.
 * Build:  gcc -o test_ntp_sei test_ntp_sei.c -Wall -Wextra
 * Run:    ./test_ntp_sei
 *
 * Exit 0 = all tests passed, non-zero = failure count.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ─── OBS shims ────────────────────────────────────────────────────────── */

/* Replace OBS allocator with stdlib */
#define bmalloc(sz) malloc(sz)
#define bfree(p) free(p)

/* Suppress OBS logging */
#define LOG_DEBUG 0
#define LOG_INFO 1
#define LOG_WARNING 2
#define LOG_ERROR 3
#define blog(level, fmt, ...) ((void)0)

/* ─── Inline the functions under test ──────────────────────────────────── */

#define NTP_TIMESTAMP_DELTA 2208988800ULL

typedef struct {
  uint32_t seconds;
  uint32_t fraction;
} ntp_timestamp_t;

/* From ntp-client.c */
static uint64_t ntp_to_ns(ntp_timestamp_t *ntp) {
  uint64_t seconds = (uint64_t)ntp->seconds;
  uint64_t fraction = (uint64_t)ntp->fraction;

  if (seconds > NTP_TIMESTAMP_DELTA) {
    seconds -= NTP_TIMESTAMP_DELTA;
  }

  uint64_t ns = seconds * 1000000000ULL;
  ns += (fraction * 1000000000ULL) >> 32;
  return ns;
}

static void ns_to_ntp(uint64_t ns, ntp_timestamp_t *ntp) {
  uint64_t seconds = ns / 1000000000ULL;
  uint64_t fraction_ns = ns % 1000000000ULL;

  ntp->seconds = (uint32_t)(seconds + NTP_TIMESTAMP_DELTA);
  ntp->fraction = (uint32_t)((fraction_ns << 32) / 1000000000ULL);
}

/* SEI payload builder — mirrors build_ntp_sei_payload in sei-handler.c */
static int build_ntp_sei_payload(int64_t pts, ntp_timestamp_t *ntp_time,
                                 uint8_t **payload, size_t *size) {
  const uint8_t uuid[16] = {0xa5, 0xb3, 0xc2, 0xd1, 0xe4, 0xf5, 0x67, 0x89,
                            0xab, 0xcd, 0xef, 0x01, 0x23, 0x45, 0x67, 0x89};

  *size = 16 + 8 + 4 + 4;
  *payload = (uint8_t *)bmalloc(*size);
  if (!*payload)
    return 0;

  memcpy(*payload, uuid, 16);

  uint8_t *data = *payload + 16;
  size_t i = 0;

  for (int shift = 56; shift >= 0; shift -= 8)
    data[i++] = (uint8_t)((pts >> shift) & 0xFF);

  uint32_t ntp_sec = ntp_time->seconds;
  uint32_t ntp_frac = ntp_time->fraction;
  data[i++] = (ntp_sec >> 24) & 0xFF;
  data[i++] = (ntp_sec >> 16) & 0xFF;
  data[i++] = (ntp_sec >> 8) & 0xFF;
  data[i++] = (ntp_sec) & 0xFF;
  data[i++] = (ntp_frac >> 24) & 0xFF;
  data[i++] = (ntp_frac >> 16) & 0xFF;
  data[i++] = (ntp_frac >> 8) & 0xFF;
  data[i++] = (ntp_frac) & 0xFF;

  return 1;
}

/* NAL start code finder — identical across all three encoders */
static const uint8_t *find_nal_start_code(const uint8_t *data, size_t size,
                                          size_t *start_code_size) {
  if (size < 3)
    return NULL;

  for (size_t i = 0; i < size - 2; i++) {
    if (data[i] == 0 && data[i + 1] == 0) {
      if (data[i + 2] == 1) {
        *start_code_size = 3;
        return data + i;
      } else if (i < size - 3 && data[i + 2] == 0 && data[i + 3] == 1) {
        *start_code_size = 4;
        return data + i;
      }
    }
  }
  return NULL;
}

/* Parameter sets end finder — from the encoder files */
#define H264_NAL_SPS 7
#define H264_NAL_PPS 8
#define H265_NAL_VPS 32
#define H265_NAL_SPS 33
#define H265_NAL_PPS 34

static size_t find_parameter_sets_end(const uint8_t *data, size_t size,
                                      int codec_type, bool *sps_seen) {
  const uint8_t *current = data;
  size_t remaining = size;
  size_t last_param_end = 0;

  *sps_seen = false;

  while (remaining > 0) {
    size_t sc_size = 0;
    const uint8_t *nal_start =
        find_nal_start_code(current, remaining, &sc_size);

    if (!nal_start)
      break;

    const uint8_t *nal_data = nal_start + sc_size;
    size_t nal_remaining = remaining - (nal_data - current);

    if (nal_remaining < 1)
      break;

    uint8_t nal_type;
    int is_param_set = 0;

    if (codec_type == 0) { /* H.264 */
      nal_type = nal_data[0] & 0x1F;
      is_param_set = (nal_type == H264_NAL_SPS || nal_type == H264_NAL_PPS ||
                      nal_type == 9);
      if (nal_type == H264_NAL_SPS)
        *sps_seen = true;
    } else if (codec_type == 1) { /* H.265 */
      nal_type = (nal_data[0] >> 1) & 0x3F;
      is_param_set = (nal_type == H265_NAL_VPS || nal_type == H265_NAL_SPS ||
                      nal_type == H265_NAL_PPS || nal_type == 35);
      if (nal_type == H265_NAL_SPS)
        *sps_seen = true;
    } else {
      return 0;
    }

    size_t next_sc_size = 0;
    const uint8_t *next_nal =
        find_nal_start_code(nal_data, nal_remaining, &next_sc_size);

    if (is_param_set) {
      if (next_nal) {
        last_param_end = next_nal - data;
      } else {
        last_param_end = size;
      }
    } else {
      return last_param_end;
    }

    if (!next_nal)
      break;

    current = next_nal;
    remaining = size - (current - data);
  }

  return last_param_end;
}

/* Extradata → Annex-B converter — from the encoder files */
static uint8_t *extradata_to_annexb(const uint8_t *extradata,
                                    size_t extradata_size, int codec_type,
                                    size_t *out_size) {
  *out_size = 0;
  if (!extradata || extradata_size < 4)
    return NULL;

  /* Annex-B already? Either 0x00 0x00 0x00 0x01 or 0x00 0x00 0x01. */
  bool is_annexb =
      (extradata[0] == 0 && extradata[1] == 0 &&
       ((extradata[2] == 0 && extradata[3] == 1) || extradata[2] == 1));
  if (is_annexb) {
    uint8_t *out = bmalloc(extradata_size);
    memcpy(out, extradata, extradata_size);
    *out_size = extradata_size;
    return out;
  }

  /* AVCDecoderConfigurationRecord: configurationVersion must be 1. */
  if (codec_type != 0 || extradata[0] != 0x01 || extradata_size < 7)
    return NULL;

  size_t pos = 5;
  int num_sps = extradata[pos++] & 0x1F;
  size_t total = 0;

  /* Pass 1: validate ranges and tally the output size. */
  size_t scan = pos;
  for (int i = 0; i < num_sps; i++) {
    if (scan + 2 > extradata_size)
      return NULL;
    uint16_t len = ((uint16_t)extradata[scan] << 8) | extradata[scan + 1];
    scan += 2;
    if (scan + len > extradata_size)
      return NULL;
    total += 4 + len; /* 4-byte start code + NAL */
    scan += len;
  }
  if (scan + 1 > extradata_size)
    return NULL;
  int num_pps = extradata[scan++];
  for (int i = 0; i < num_pps; i++) {
    if (scan + 2 > extradata_size)
      return NULL;
    uint16_t len = ((uint16_t)extradata[scan] << 8) | extradata[scan + 1];
    scan += 2;
    if (scan + len > extradata_size)
      return NULL;
    total += 4 + len;
    scan += len;
  }
  if (total == 0)
    return NULL;

  /* Pass 2: emit Annex-B. */
  uint8_t *out = bmalloc(total);
  uint8_t *wp = out;
  scan = pos;
  for (int i = 0; i < num_sps; i++) {
    uint16_t len = ((uint16_t)extradata[scan] << 8) | extradata[scan + 1];
    scan += 2;
    *wp++ = 0;
    *wp++ = 0;
    *wp++ = 0;
    *wp++ = 1;
    memcpy(wp, extradata + scan, len);
    wp += len;
    scan += len;
  }
  scan++; /* skip num_pps */
  for (int i = 0; i < num_pps; i++) {
    uint16_t len = ((uint16_t)extradata[scan] << 8) | extradata[scan + 1];
    scan += 2;
    *wp++ = 0;
    *wp++ = 0;
    *wp++ = 0;
    *wp++ = 1;
    memcpy(wp, extradata + scan, len);
    wp += len;
    scan += len;
  }

  *out_size = total;
  return out;
}

/* ─── Test framework ───────────────────────────────────────────────────── */

static int tests_run = 0;
static int tests_failed = 0;

#define ASSERT_EQ(a, b, msg)                                                   \
  do {                                                                         \
    tests_run++;                                                               \
    if ((a) != (b)) {                                                          \
      fprintf(stderr, "FAIL [%s:%d] %s: expected %llu, got %llu\n", __FILE__,  \
              __LINE__, msg, (unsigned long long)(b),                           \
              (unsigned long long)(a));                                         \
      tests_failed++;                                                          \
    }                                                                          \
  } while (0)

#define ASSERT_TRUE(cond, msg)                                                 \
  do {                                                                         \
    tests_run++;                                                               \
    if (!(cond)) {                                                             \
      fprintf(stderr, "FAIL [%s:%d] %s\n", __FILE__, __LINE__, msg);           \
      tests_failed++;                                                          \
    }                                                                          \
  } while (0)

#define ASSERT_NEAR(a, b, tol, msg)                                            \
  do {                                                                         \
    tests_run++;                                                               \
    int64_t diff = (int64_t)(a) - (int64_t)(b);                                \
    if (diff < 0) diff = -diff;                                                \
    if (diff > (int64_t)(tol)) {                                               \
      fprintf(stderr, "FAIL [%s:%d] %s: %lld vs %lld (tol %lld)\n",           \
              __FILE__, __LINE__, msg,                                          \
              (long long)(a), (long long)(b), (long long)(tol));                \
      tests_failed++;                                                          \
    }                                                                          \
  } while (0)

/* ─── Test cases ───────────────────────────────────────────────────────── */

static void test_ntp_epoch_conversion_roundtrip(void) {
  printf("  ntp_to_ns / ns_to_ntp round-trip...\n");

  /* Known timestamp: 2024-01-01 00:00:00 UTC = Unix 1704067200 */
  uint64_t unix_sec = 1704067200ULL;
  uint64_t input_ns = unix_sec * 1000000000ULL;

  ntp_timestamp_t ntp;
  ns_to_ntp(input_ns, &ntp);

  /* NTP seconds should be Unix + delta */
  ASSERT_EQ(ntp.seconds, (uint32_t)(unix_sec + NTP_TIMESTAMP_DELTA),
            "ns_to_ntp seconds");
  ASSERT_EQ(ntp.fraction, 0, "ns_to_ntp fraction (whole second)");

  /* Round-trip */
  uint64_t result_ns = ntp_to_ns(&ntp);
  ASSERT_EQ(result_ns, input_ns, "ntp_to_ns round-trip");
}

static void test_ntp_fractional_precision(void) {
  printf("  NTP fractional second precision...\n");

  /* 0.5 seconds = 500ms */
  uint64_t input_ns = 1704067200ULL * 1000000000ULL + 500000000ULL;
  ntp_timestamp_t ntp;
  ns_to_ntp(input_ns, &ntp);

  /* fraction should be ~2^31 (half of 2^32) */
  uint32_t expected_frac = (uint32_t)(((uint64_t)500000000ULL << 32) / 1000000000ULL);
  ASSERT_EQ(ntp.fraction, expected_frac, "0.5s fraction");

  /* Round-trip should be within 1ns (integer rounding) */
  uint64_t result_ns = ntp_to_ns(&ntp);
  ASSERT_NEAR(result_ns, input_ns, 1, "0.5s round-trip");

  /* 0.25 seconds */
  input_ns = 1704067200ULL * 1000000000ULL + 250000000ULL;
  ns_to_ntp(input_ns, &ntp);
  result_ns = ntp_to_ns(&ntp);
  ASSERT_NEAR(result_ns, input_ns, 1, "0.25s round-trip");

  /* 0.001 seconds (1ms) */
  input_ns = 1704067200ULL * 1000000000ULL + 1000000ULL;
  ns_to_ntp(input_ns, &ntp);
  result_ns = ntp_to_ns(&ntp);
  /* NTP fraction has ~233ps resolution, so 1ms should be exact within 1ns */
  ASSERT_NEAR(result_ns, input_ns, 1, "1ms round-trip");
}

static void test_ntp_sanity_check(void) {
  printf("  NTP sanity check (year 2020 threshold)...\n");

  /* Good timestamp: 2024-01-01 in NTP epoch */
  ntp_timestamp_t good = {
    .seconds = (uint32_t)(1704067200ULL + NTP_TIMESTAMP_DELTA),
    .fraction = 0
  };
  ASSERT_TRUE(good.seconds > 3786825600UL, "2024 timestamp > year 2020");

  /* Bad timestamp: close to zero (OBS-start-time bug) */
  ntp_timestamp_t bad = {
    .seconds = 12345, /* way too small */
    .fraction = 0
  };
  ASSERT_TRUE(bad.seconds < 3786825600UL, "tiny timestamp fails sanity");

  /* Edge: exactly year 2020 */
  ntp_timestamp_t edge = {
    .seconds = 3786825600UL,
    .fraction = 0
  };
  ASSERT_TRUE(!(edge.seconds < 3786825600UL), "year 2020 exact passes");
}

static void test_sei_payload_layout(void) {
  printf("  SEI payload byte layout...\n");

  ntp_timestamp_t ntp = {
    .seconds = 0xAABBCCDD,
    .fraction = 0x11223344
  };
  uint8_t *payload = NULL;
  size_t size = 0;

  int ok = build_ntp_sei_payload(42, &ntp, &payload, &size);
  ASSERT_TRUE(ok, "build_ntp_sei_payload returns true");
  ASSERT_EQ(size, 32, "size = 16 UUID + 8 PTS + 4 sec + 4 frac");

  /* Check UUID prefix */
  const uint8_t expected_uuid[16] = {0xa5, 0xb3, 0xc2, 0xd1, 0xe4, 0xf5,
                                     0x67, 0x89, 0xab, 0xcd, 0xef, 0x01,
                                     0x23, 0x45, 0x67, 0x89};
  ASSERT_TRUE(memcmp(payload, expected_uuid, 16) == 0, "UUID matches");

  /* PTS 42 as int64 big-endian */
  ASSERT_EQ(payload[16], 0x00, "PTS byte 0");
  ASSERT_EQ(payload[22], 0x00, "PTS byte 6");
  ASSERT_EQ(payload[23], 0x2A, "PTS byte 7 = 42");

  /* Check NTP bytes (big-endian) */
  ASSERT_EQ(payload[24], 0xAA, "NTP sec byte 0");
  ASSERT_EQ(payload[25], 0xBB, "NTP sec byte 1");
  ASSERT_EQ(payload[26], 0xCC, "NTP sec byte 2");
  ASSERT_EQ(payload[27], 0xDD, "NTP sec byte 3");
  ASSERT_EQ(payload[28], 0x11, "NTP frac byte 0");
  ASSERT_EQ(payload[29], 0x22, "NTP frac byte 1");
  ASSERT_EQ(payload[30], 0x33, "NTP frac byte 2");
  ASSERT_EQ(payload[31], 0x44, "NTP frac byte 3");

  bfree(payload);
}

static void test_find_parameter_sets_h264(void) {
  printf("  find_parameter_sets_end H.264...\n");

  /* Synthetic H.264 bitstream: [SPS][PPS][IDR slice] */
  uint8_t bitstream[] = {
    /* SPS: start code + NAL type 7 */
    0x00, 0x00, 0x00, 0x01, 0x67, 0xAA, 0xBB,
    /* PPS: start code + NAL type 8 */
    0x00, 0x00, 0x00, 0x01, 0x68, 0xCC, 0xDD,
    /* IDR slice: start code + NAL type 5 */
    0x00, 0x00, 0x00, 0x01, 0x65, 0x11, 0x22, 0x33
  };
  size_t bs_size = sizeof(bitstream);

  bool sps_seen = false;
  size_t end = find_parameter_sets_end(bitstream, bs_size, 0, &sps_seen);

  /* Should point to the start of the IDR NAL (byte offset 14) */
  ASSERT_EQ(end, 14, "H.264 param sets end before IDR");
  ASSERT_TRUE(sps_seen, "H.264 SPS reported in-band");
}

static void test_find_parameter_sets_h264_with_aud(void) {
  printf("  find_parameter_sets_end H.264 with AUD...\n");

  /* [AUD][SPS][PPS][IDR] */
  uint8_t bitstream[] = {
    /* AUD: start code + NAL type 9 */
    0x00, 0x00, 0x00, 0x01, 0x09, 0xF0,
    /* SPS: start code + NAL type 7 */
    0x00, 0x00, 0x00, 0x01, 0x67, 0xAA, 0xBB,
    /* PPS: start code + NAL type 8 */
    0x00, 0x00, 0x00, 0x01, 0x68, 0xCC, 0xDD,
    /* IDR: start code + NAL type 5 */
    0x00, 0x00, 0x00, 0x01, 0x65, 0x11, 0x22
  };

  bool sps_seen = false;
  size_t end = find_parameter_sets_end(bitstream, sizeof(bitstream), 0,
                                       &sps_seen);
  /* AUD (type 9) is treated as param set, so end should be at IDR start */
  ASSERT_EQ(end, 20, "H.264 AUD+SPS+PPS end before IDR");
  ASSERT_TRUE(sps_seen, "H.264 SPS reported when present after AUD");
}

static void test_find_parameter_sets_aud_only(void) {
  printf("  find_parameter_sets_end with a lone AUD (no SPS)...\n");

  /* [AUD][IDR] — the GLOBAL_HEADER shape with AUD insertion enabled: the
   * leading run ends after the AUD but must NOT count as params in-band. */
  uint8_t h264[] = {
    /* AUD: start code + NAL type 9 */
    0x00, 0x00, 0x00, 0x01, 0x09, 0xF0,
    /* IDR: start code + NAL type 5 */
    0x00, 0x00, 0x00, 0x01, 0x65, 0x11, 0x22
  };
  bool sps_seen = true;
  size_t end = find_parameter_sets_end(h264, sizeof(h264), 0, &sps_seen);
  ASSERT_EQ(end, 6, "H.264 lone AUD: leading run ends at IDR");
  ASSERT_TRUE(!sps_seen, "H.264 lone AUD reports no SPS");

  uint8_t h265[] = {
    /* AUD: start code + 2-byte NAL header (type 35) + payload */
    0x00, 0x00, 0x00, 0x01, (35 << 1), 0x01, 0x50,
    /* IDR_W_RADL: type 19 */
    0x00, 0x00, 0x00, 0x01, (19 << 1), 0x01, 0xDD
  };
  sps_seen = true;
  end = find_parameter_sets_end(h265, sizeof(h265), 1, &sps_seen);
  ASSERT_EQ(end, 7, "H.265 lone AUD: leading run ends at IDR");
  ASSERT_TRUE(!sps_seen, "H.265 lone AUD reports no SPS");
}

static void test_find_parameter_sets_h265(void) {
  printf("  find_parameter_sets_end H.265...\n");

  /* [VPS][SPS][PPS][IDR_W_RADL slice (type 19)] */
  uint8_t bitstream[] = {
    /* VPS: start code + NAL type 32 (H.265 NAL header is 2 bytes: (type<<1) in first byte) */
    0x00, 0x00, 0x00, 0x01, (H265_NAL_VPS << 1), 0x01, 0xAA,
    /* SPS: type 33 */
    0x00, 0x00, 0x00, 0x01, (H265_NAL_SPS << 1), 0x01, 0xBB,
    /* PPS: type 34 */
    0x00, 0x00, 0x00, 0x01, (H265_NAL_PPS << 1), 0x01, 0xCC,
    /* IDR_W_RADL: type 19 */
    0x00, 0x00, 0x00, 0x01, (19 << 1), 0x01, 0xDD
  };

  bool sps_seen = false;
  size_t end = find_parameter_sets_end(bitstream, sizeof(bitstream), 1,
                                       &sps_seen);
  /* Should be at the start of the IDR NAL */
  ASSERT_EQ(end, 21, "H.265 VPS+SPS+PPS end before IDR");
  ASSERT_TRUE(sps_seen, "H.265 SPS reported in-band");
}

static void test_find_parameter_sets_no_params(void) {
  printf("  find_parameter_sets_end with no param sets...\n");

  /* Just a P-slice (H.264 type 1) — no SPS/PPS */
  uint8_t bitstream[] = {
    0x00, 0x00, 0x00, 0x01, 0x41, 0x11, 0x22, 0x33
  };

  bool sps_seen = true;
  size_t end = find_parameter_sets_end(bitstream, sizeof(bitstream), 0,
                                       &sps_seen);
  /* No param sets found → returns 0 */
  ASSERT_EQ(end, 0, "No param sets returns 0");
  ASSERT_TRUE(!sps_seen, "No param sets reports no SPS");
}

static void test_find_parameter_sets_3byte_start_code(void) {
  printf("  find_parameter_sets_end with 3-byte start codes...\n");

  /* Some encoders emit 3-byte start codes (00 00 01) */
  uint8_t bitstream[] = {
    /* SPS: 3-byte start code + NAL type 7 */
    0x00, 0x00, 0x01, 0x67, 0xAA,
    /* PPS: 3-byte start code + NAL type 8 */
    0x00, 0x00, 0x01, 0x68, 0xBB,
    /* IDR: 3-byte start code + NAL type 5 */
    0x00, 0x00, 0x01, 0x65, 0xCC
  };

  bool sps_seen = false;
  size_t end = find_parameter_sets_end(bitstream, sizeof(bitstream), 0,
                                       &sps_seen);
  ASSERT_EQ(end, 10, "3-byte start codes: param end before IDR");
  ASSERT_TRUE(sps_seen, "3-byte start codes: SPS reported in-band");
}

static void test_find_nal_start_code_empty(void) {
  printf("  find_nal_start_code edge cases...\n");

  size_t sc_size = 0;

  /* Empty input */
  ASSERT_TRUE(find_nal_start_code(NULL, 0, &sc_size) == NULL,
              "NULL data returns NULL");

  /* Too small */
  uint8_t tiny[] = {0x00, 0x00};
  ASSERT_TRUE(find_nal_start_code(tiny, 2, &sc_size) == NULL,
              "2-byte input returns NULL");

  /* Exactly 3-byte start code */
  uint8_t exact[] = {0x00, 0x00, 0x01};
  const uint8_t *found = find_nal_start_code(exact, 3, &sc_size);
  ASSERT_TRUE(found == exact, "3-byte exact match");
  ASSERT_EQ(sc_size, 3, "3-byte start code size");
}

static void test_extradata_annexb_passthrough(void) {
  printf("  extradata_to_annexb passes Annex-B through...\n");

  uint8_t annexb[] = {
    0x00, 0x00, 0x00, 0x01, 0x67, 0xAA, 0xBB,
    0x00, 0x00, 0x00, 0x01, 0x68, 0xCC
  };
  size_t out_size = 0;
  uint8_t *out = extradata_to_annexb(annexb, sizeof(annexb), 0, &out_size);
  ASSERT_TRUE(out != NULL, "H.264 Annex-B extradata accepted");
  ASSERT_EQ(out_size, sizeof(annexb), "H.264 Annex-B size preserved");
  ASSERT_TRUE(out && memcmp(out, annexb, sizeof(annexb)) == 0,
              "H.264 Annex-B bytes preserved");
  free(out);

  /* The Annex-B path applies to H.265 too (hevc extradata is VPS/SPS/PPS
   * with start codes). */
  out = extradata_to_annexb(annexb, sizeof(annexb), 1, &out_size);
  ASSERT_TRUE(out != NULL, "H.265 Annex-B extradata accepted");
  ASSERT_EQ(out_size, sizeof(annexb), "H.265 Annex-B size preserved");
  free(out);
}

static void test_extradata_annexb_from_avcc(void) {
  printf("  extradata_to_annexb converts AVCC (H.264)...\n");

  uint8_t avcc[] = {
    0x01,             /* configurationVersion */
    0x64, 0x00, 0x28, /* profile / compat / level */
    0xFF,             /* reserved + lengthSizeMinusOne */
    0xE1,             /* reserved + numOfSequenceParameterSets = 1 */
    0x00, 0x03, 0x67, 0xAA, 0xBB, /* SPS: len 3 */
    0x01,             /* numOfPictureParameterSets = 1 */
    0x00, 0x02, 0x68, 0xCC        /* PPS: len 2 */
  };
  uint8_t expected[] = {
    0x00, 0x00, 0x00, 0x01, 0x67, 0xAA, 0xBB,
    0x00, 0x00, 0x00, 0x01, 0x68, 0xCC
  };
  size_t out_size = 0;
  uint8_t *out = extradata_to_annexb(avcc, sizeof(avcc), 0, &out_size);
  ASSERT_TRUE(out != NULL, "AVCC extradata converted");
  ASSERT_EQ(out_size, sizeof(expected), "AVCC conversion size");
  ASSERT_TRUE(out && memcmp(out, expected, sizeof(expected)) == 0,
              "AVCC conversion bytes");
  free(out);

  /* Truncated AVCC (SPS length runs past the buffer) must be rejected. */
  uint8_t truncated[] = {0x01, 0x64, 0x00, 0x28, 0xFF, 0xE1, 0x00, 0x40, 0x67};
  out = extradata_to_annexb(truncated, sizeof(truncated), 0, &out_size);
  ASSERT_TRUE(out == NULL, "truncated AVCC rejected");
  ASSERT_EQ(out_size, 0, "truncated AVCC leaves size 0");
}

static void test_extradata_annexb_rejects_hvcc(void) {
  printf("  extradata_to_annexb rejects HVCC (H.265)...\n");

  /* HVCC also begins with configurationVersion 0x01; parsing it as AVCC
   * would read profile/constraint bytes as SPS counts and lengths. */
  uint8_t hvcc[] = {
    0x01, 0x01, 0x60, 0x00, 0x00, 0x00, 0x90, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x78, 0xF0, 0x00, 0xFC, 0xFD, 0xF8, 0xF8, 0x00
  };
  size_t out_size = 0xDEAD;
  uint8_t *out = extradata_to_annexb(hvcc, sizeof(hvcc), 1, &out_size);
  ASSERT_TRUE(out == NULL, "HVCC-shaped extradata rejected for H.265");
  ASSERT_EQ(out_size, 0, "HVCC rejection leaves size 0");
}

static void test_ntp_conversion_known_values(void) {
  printf("  NTP conversion with known values...\n");

  /* NTP era 0 starts at 1900-01-01.
   * Unix epoch (1970-01-01) = NTP 2208988800.
   * 2024-06-15 12:00:00 UTC = Unix 1718452800 = NTP 3927441600 */
  ntp_timestamp_t ntp = {
    .seconds = 3927441600UL,
    .fraction = 0
  };

  uint64_t ns = ntp_to_ns(&ntp);
  uint64_t expected_unix_sec = 3927441600ULL - NTP_TIMESTAMP_DELTA;
  ASSERT_EQ(ns / 1000000000ULL, expected_unix_sec,
            "2024-06-15 12:00:00 conversion");
}

static void test_ntp_sync_anchor_is_offset_corrected(void) {
  printf("  NTP sync anchor = T4 + offset (== T3 + delay/2)...\n");

  /* Local clock 30 s behind the server, 18 ms each way on the wire, 4 ms of
   * server processing. */
  const uint64_t s = 1000000000ULL, ms = 1000000ULL;
  uint64_t t1_wall = 1704067200ULL * s;
  uint64_t t2_ns = t1_wall + 30 * s + 18 * ms;
  uint64_t t3_ns = t2_ns + 4 * ms;
  uint64_t t4_wall = t1_wall + 40 * ms;

  /* Mirrors ntp_sync_one_addr */
  int64_t offset = ((int64_t)(t2_ns - t1_wall) + (int64_t)(t3_ns - t4_wall)) / 2;
  int64_t delay = (int64_t)(t4_wall - t1_wall) - (int64_t)(t3_ns - t2_ns);
  uint64_t anchor_ns = (uint64_t)((int64_t)t4_wall + offset);

  ASSERT_EQ(offset, (int64_t)(30 * s), "offset recovers the 30 s clock error");
  ASSERT_EQ(delay, (int64_t)(36 * ms), "delay excludes server processing");
  ASSERT_EQ(anchor_ns, t3_ns + (uint64_t)delay / 2, "anchor == T3 + delay/2");
  ASSERT_EQ(anchor_ns - t3_ns, 18 * ms,
            "raw T3 would trail the anchor by half the round trip");

  ntp_timestamp_t anchor;
  ns_to_ntp(anchor_ns, &anchor);
  ASSERT_NEAR(ntp_to_ns(&anchor), anchor_ns, 1,
              "anchor survives the NTP timestamp round-trip");
}

/* ─── Main ─────────────────────────────────────────────────────────────── */

int main(void) {
  printf("SEI-Stamper unit tests\n");
  printf("======================\n\n");

  printf("[NTP epoch conversion]\n");
  test_ntp_epoch_conversion_roundtrip();
  test_ntp_fractional_precision();
  test_ntp_conversion_known_values();
  test_ntp_sync_anchor_is_offset_corrected();
  test_ntp_sanity_check();

  printf("\n[SEI payload]\n");
  test_sei_payload_layout();

  printf("\n[NAL parsing]\n");
  test_find_parameter_sets_h264();
  test_find_parameter_sets_h264_with_aud();
  test_find_parameter_sets_aud_only();
  test_find_parameter_sets_h265();
  test_find_parameter_sets_no_params();
  test_find_parameter_sets_3byte_start_code();
  test_find_nal_start_code_empty();

  printf("\n[Extradata conversion]\n");
  test_extradata_annexb_passthrough();
  test_extradata_annexb_from_avcc();
  test_extradata_annexb_rejects_hvcc();

  printf("\n======================\n");
  printf("%d tests run, %d passed, %d failed\n",
         tests_run, tests_run - tests_failed, tests_failed);

  return tests_failed;
}
