#include "qsv-encoder.h"
#include "sei-stamper-encoder.h" /* For NAL unit helpers if we move them, or redefine */
#include <util/dstr.h>
#include <util/platform.h>

// Re-implementing simplified SEI helpers valid for this module to avoid linking
// issues or we can expose them from sei-stamper-encoder.h if we modify it. For
// safety, providing local versions.

#ifdef ENABLE_VPL

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ALIGN16(value) (((value + 15) >> 4) << 4)
#define ALIGN32(value) (((value + 31) >> 5) << 5)

#include "sei-handler.h"
#if 0
/* NTP Helpers (Copied/Adapted) */
static bool qsv_build_ntp_sei_payload(int64_t pts, ntp_timestamp_t *ntp_time,
                                      uint8_t **payload, size_t *size) {
  /* UUID: 2f2f2f53-4549-2f2f-2f53-45492f2f2f53 (Example user data unregistered)
   */
  /* Using generic UUID for our stamper (Matches sei-handler.c) */
  const uint8_t uuid[16] = {0xa5, 0xb3, 0xc2, 0xd1, 0xe4, 0xf5, 0x67, 0x89,
                            0xab, 0xcd, 0xef, 0x01, 0x23, 0x45, 0x67, 0x89};

  *size = 16 + 8; // UUID + 64bit NTP
  *payload = bmalloc(*size);
  if (!*payload)
    return false;

  memcpy(*payload, uuid, 16);

  /* Big Endian NTP Timestamp from struct */
  uint32_t ntp_sec = ntp_time->seconds;
  uint32_t ntp_frac = ntp_time->fraction;

  uint8_t *data = *payload + 16;
  data[0] = (ntp_sec >> 24) & 0xFF;
  data[1] = (ntp_sec >> 16) & 0xFF;
  data[2] = (ntp_sec >> 8) & 0xFF;
  data[3] = (ntp_sec) & 0xFF;
  data[4] = (ntp_frac >> 24) & 0xFF;
  data[5] = (ntp_frac >> 16) & 0xFF;
  data[6] = (ntp_frac >> 8) & 0xFF;
  data[7] = (ntp_frac) & 0xFF;

  return true;
}

static bool qsv_build_sei_nal_unit(uint8_t *payload, size_t payload_size,
                                   int payload_type, int codec_type,
                                   uint8_t **nal_unit, size_t *nal_size) {
  /* Build the SEI NAL unit for this codec type */
  /* H.264: 1-byte NAL header (0x06)
   * H.265: 2-byte NAL header (0x4E 0x01 for PREFIX_SEI_NUT)
   */

  size_t size_bytes = 1;
  if (payload_size >= 255)
    size_bytes += (payload_size / 255);

  size_t nal_header_size = (codec_type == 1) ? 2 : 1; // H.265=2, H.264=1
  size_t total_size = 4 + nal_header_size + 1 + size_bytes + payload_size + 1;

  *nal_unit = bmalloc(total_size);
  if (!*nal_unit)
    return false;

  uint8_t *p = *nal_unit;

  // Start Code (Annex B)
  *p++ = 0x00;
  *p++ = 0x00;
  *p++ = 0x00;
  *p++ = 0x01;

  // NAL Header
  if (codec_type == 1) {
    // H.265/HEVC: 2-byte NAL header
    // byte 0: forbidden(1) + nal_unit_type(6) + nuh_layer_id(6 bits, high)
    // byte 1: nuh_layer_id(3 bits, low) + nuh_temporal_id_plus1(3)
    // PREFIX_SEI_NUT = 39 (0x27)
    // (0 << 7) | (39 << 1) | (0 >> 5) = 0x4E
    *p++ = 0x4E; // forbidden=0, type=39(PREFIX_SEI), layer_id[5:0]=0
    *p++ = 0x01; // layer_id[2:0]=0, temporal_id_plus1=1
  } else {
    // H.264/AVC: 1-byte NAL header
    // forbidden_zero_bit(1) + nal_ref_idc(2) + nal_unit_type(5)
    // SEI = 6
    *p++ = 0x06; // forbidden=0, ref_idc=0, type=6(SEI)
  }

  // Payload Type (User Data Unregistered = 5)
  *p++ = 0x05;

  // Payload Size
  size_t s = payload_size;
  while (s >= 255) {
    *p++ = 0xFF;
    s -= 255;
  }
  *p++ = (uint8_t)s;

  // Payload
  memcpy(p, payload, payload_size);
  p += payload_size;

  // Trailing bits (rbsp_trailing_bits) -> 1 followed by 0s to byte align
  *p++ = 0x80;

  *nal_size = (p - *nal_unit);
  return true;
}
#endif

/* ------------------------------------------------------------------------- */
/* NAL Unit Extraction Helpers */
/* ------------------------------------------------------------------------- */

/* NAL unit type definitions */
#define H264_NAL_SPS 7
#define H264_NAL_PPS 8
#define H265_NAL_VPS 32
#define H265_NAL_SPS 33
#define H265_NAL_PPS 34

/* Find a NAL unit start code */
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

/* Extract SPS/PPS from an H.264 bitstream */
static bool extract_h264_params(uint8_t *data, size_t size, uint8_t **sps,
                                size_t *sps_size, uint8_t **pps,
                                size_t *pps_size) {
  *sps = NULL;
  *pps = NULL;
  *sps_size = 0;
  *pps_size = 0;

  const uint8_t *current = data;
  size_t remaining = size;

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

    uint8_t nal_type = nal_data[0] & 0x1F;

    // Find the next start code to size this NAL
    size_t next_sc_size = 0;
    const uint8_t *next_nal =
        find_nal_start_code(nal_data, nal_remaining, &next_sc_size);
    size_t nal_size = next_nal ? (next_nal - nal_data) : nal_remaining;

    if (nal_type == H264_NAL_SPS && !*sps) {
      *sps_size = nal_size;
      *sps = bmalloc(*sps_size);
      memcpy(*sps, nal_data, *sps_size);
    } else if (nal_type == H264_NAL_PPS && !*pps) {
      *pps_size = nal_size;
      *pps = bmalloc(*pps_size);
      memcpy(*pps, nal_data, *pps_size);
    }

    if (*sps && *pps)
      return true; // found every required parameter set

    current = nal_data + nal_size;
    remaining = size - (current - data);
  }

  // Clean up
  if (*sps && !*pps) {
    bfree(*sps);
    *sps = NULL;
    *sps_size = 0;
  }

  return (*sps && *pps);
}

/* Extract VPS/SPS/PPS from an H.265 bitstream */
static bool extract_h265_params(uint8_t *data, size_t size, uint8_t **vps,
                                size_t *vps_size, uint8_t **sps,
                                size_t *sps_size, uint8_t **pps,
                                size_t *pps_size) {
  *vps = *sps = *pps = NULL;
  *vps_size = *sps_size = *pps_size = 0;

  const uint8_t *current = data;
  size_t remaining = size;

  while (remaining > 0) {
    size_t sc_size = 0;
    const uint8_t *nal_start =
        find_nal_start_code(current, remaining, &sc_size);

    if (!nal_start)
      break;

    const uint8_t *nal_data = nal_start + sc_size;
    size_t nal_remaining = remaining - (nal_data - current);

    if (nal_remaining < 2)
      break;

    // The H.265 NAL type is in the top 6 bits of the first byte
    uint8_t nal_type = (nal_data[0] >> 1) & 0x3F;

    // Find the next start code
    size_t next_sc_size = 0;
    const uint8_t *next_nal =
        find_nal_start_code(nal_data, nal_remaining, &next_sc_size);
    size_t nal_size = next_nal ? (next_nal - nal_data) : nal_remaining;

    if (nal_type == H265_NAL_VPS && !*vps) {
      *vps_size = nal_size;
      *vps = bmalloc(*vps_size);
      memcpy(*vps, nal_data, *vps_size);
      blog(LOG_INFO, "[QSV Native] Found VPS: size=%zu", *vps_size);
    } else if (nal_type == H265_NAL_SPS && !*sps) {
      *sps_size = nal_size;
      *sps = bmalloc(*sps_size);
      memcpy(*sps, nal_data, *sps_size);
      blog(LOG_INFO, "[QSV Native] Found SPS: size=%zu", *sps_size);
    } else if (nal_type == H265_NAL_PPS && !*pps) {
      *pps_size = nal_size;
      *pps = bmalloc(*pps_size);
      memcpy(*pps, nal_data, *pps_size);
      blog(LOG_INFO, "[QSV Native] Found PPS: size=%zu", *pps_size);
    }

    if (*vps && *sps && *pps)
      return true;

    current = nal_data + nal_size;
    remaining = size - (current - data);
  }

  // Clean up
  if (!(*vps && *sps && *pps)) {
    blog(LOG_WARNING,
         "[QSV Native] H.265 param extraction incomplete: VPS=%s SPS=%s PPS=%s",
         *vps ? "YES" : "NO", *sps ? "YES" : "NO", *pps ? "YES" : "NO");
    if (*vps) {
      bfree(*vps);
      *vps = NULL;
      *vps_size = 0;
    }
    if (*sps) {
      bfree(*sps);
      *sps = NULL;
      *sps_size = 0;
    }
    if (*pps) {
      bfree(*pps);
      *pps = NULL;
      *pps_size = 0;
    }
  }

  return (*vps && *sps && *pps);
}

/* Build H.264 AVCC extra data */
static bool build_h264_extradata(uint8_t *sps, size_t sps_size, uint8_t *pps,
                                 size_t pps_size, uint8_t **extradata,
                                 size_t *extradata_size) {
  if (!sps || sps_size < 4 || !pps)
    return false;

  *extradata_size = 5 + 1 + 2 + sps_size + 1 + 2 + pps_size;
  *extradata = bmalloc(*extradata_size);

  uint8_t *p = *extradata;
  p[0] = 0x01;   // ConfigurationVersion
  p[1] = sps[1]; // Profile
  p[2] = sps[2]; // Profile Compatibility
  p[3] = sps[3]; // Level
  p[4] = 0xFF;   // 6 bits '111111' + 2 bits lengthSizeMinusOne

  p[5] = 0xE1; // 3 bits '111' + 5 bits numOfSPS (1)

  // SPS Length (Big Endian)
  p[6] = (sps_size >> 8) & 0xFF;
  p[7] = sps_size & 0xFF;
  memcpy(p + 8, sps, sps_size);

  p += 8 + sps_size;

  p[0] = 0x01; // numOfPPS (1)
  p[1] = (pps_size >> 8) & 0xFF;
  p[2] = pps_size & 0xFF;
  memcpy(p + 3, pps, pps_size);

  return true;
}

/* Build H.265 HVCC extra data */
static bool build_h265_extradata(uint8_t *vps, size_t vps_size, uint8_t *sps,
                                 size_t sps_size, uint8_t *pps, size_t pps_size,
                                 uint8_t **extradata, size_t *extradata_size) {
  if (!vps || !sps || !pps)
    return false;

  /* HVCC layout per ISO/IEC 14496-15:2017 Section 8.3.3 */

  // Total: 23-byte header + 3 arrays
  // (3 bytes each + 2 bytes per NAL + NAL data)
  *extradata_size = 23 + 3 + 2 + vps_size + // VPS array
                    3 + 2 + sps_size +      // SPS array
                    3 + 2 + pps_size;       // PPS array

  *extradata = bzalloc(*extradata_size);
  uint8_t *p = *extradata;

  // General configuration
  p[0] = 0x01; // configurationVersion

  // H.265 NAL header: 2 bytes
  // byte 0: forbidden_zero_bit(1) + nal_unit_type(6) + nuh_layer_id(6 bits,
  // high) byte 1: nuh_layer_id(low 3 bits) + nuh_temporal_id_plus1(3)
  // profile_tier_level follows the NAL header

  // Extract profile_tier_level, skipping the NAL header's 2 bytes
  if (sps_size >= 2) {
    // general_profile_space(2) + general_tier_flag(1) + general_profile_idc(5)
    // Conservative: use Main profile (profile_idc = 1)
    p[1] = sps_size >= 3 ? sps[2] : 0x01; // try the SPS, else default to Main

    // general_profile_compatibility_flags (4 bytes)
    // Conservative: set Main profile compatibility
    if (sps_size >= 7) {
      memcpy(p + 2, sps + 3, 4);
    } else {
      p[2] = 0x60; // Main profile compatibility
      p[3] = 0x00;
      p[4] = 0x00;
      p[5] = 0x00;
    }

    // general_constraint_indicator_flags (6 bytes)
    if (sps_size >= 13) {
      memcpy(p + 6, sps + 7, 6);
    } else {
      memset(p + 6, 0, 6);
    }

    // general_level_idc
    p[12] = sps_size >= 14 ? sps[13] : 0x5D; // Level 3.1 as default
  } else {
    // If the SPS is too small, use safe defaults
    p[1] = 0x01; // Main profile
    p[2] = 0x60; // Main profile compatibility
    p[3] = p[4] = p[5] = 0x00;
    memset(p + 6, 0, 6); // No constraints
    p[12] = 0x5D;        // Level 3.1
  }

  // min_spatial_segmentation_idc (reserved + 12 bits)
  p[13] = 0xF0;
  p[14] = 0x00;

  // parallelismType (reserved + 2 bits, 0=unknown)
  p[15] = 0xFC;

  // chromaFormat (reserved + 2 bits, 1=4:2:0)
  p[16] = 0xFC | 0x1;

  // bitDepthLumaMinus8 (reserved + 3 bits, 0=8bit)
  p[17] = 0xF8;

  // bitDepthChromaMinus8 (reserved + 3 bits, 0=8bit)
  p[18] = 0xF8;

  // avgFrameRate (16 bits, 0=unspecified)
  p[19] = 0x00;
  p[20] = 0x00;

  // constantFrameRate(2) + numTemporalLayers(3) + temporalIdNested(1) +
  // lengthSizeMinusOne(2) lengthSizeMinusOne = 3 means a 4-byte length prefix
  p[21] = 0x0F;

  // numOfArrays (3: VPS, SPS, PPS)
  p[22] = 0x03;

  p += 23;

  // VPS array
  p[0] = 0x80 |
         H265_NAL_VPS; // array_completeness(1) + reserved(0) + NAL_unit_type(6)
  p[1] = 0x00;         // numNalus high byte
  p[2] = 0x01;         // numNalus low byte (1)
  p[3] = (vps_size >> 8) & 0xFF;
  p[4] = vps_size & 0xFF;
  memcpy(p + 5, vps, vps_size);
  p += 5 + vps_size;

  // SPS array
  p[0] = 0x80 | H265_NAL_SPS;
  p[1] = 0x00;
  p[2] = 0x01;
  p[3] = (sps_size >> 8) & 0xFF;
  p[4] = sps_size & 0xFF;
  memcpy(p + 5, sps, sps_size);
  p += 5 + sps_size;

  // PPS array
  p[0] = 0x80 | H265_NAL_PPS;
  p[1] = 0x00;
  p[2] = 0x01;
  p[3] = (pps_size >> 8) & 0xFF;
  p[4] = pps_size & 0xFF;
  memcpy(p + 5, pps, pps_size);

  blog(LOG_INFO,
       "[QSV Native] Built HVCC: total_size=%zu, VPS=%zu, SPS=%zu, PPS=%zu",
       *extradata_size, vps_size, sps_size, pps_size);

  return true;
}

/* ------------------------------------------------------------------------- */

/* Find where the H.264/H.265 parameter sets end: the first non-parameter-set
 * NAL after SPS/PPS/VPS
 * Returns where the SEI belongs: after the parameter sets, before the
 * first slice */
static size_t find_parameter_sets_end(const uint8_t *data, size_t size,
                                      int codec_type) {
  const uint8_t *current = data;
  size_t remaining = size;
  size_t last_param_end = 0;

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
    bool is_param_set = false;

    if (codec_type == 0) { // H.264
      nal_type = nal_data[0] & 0x1F;
      // SPS=7, PPS=8, AUD=9
      is_param_set = (nal_type == H264_NAL_SPS || nal_type == H264_NAL_PPS ||
                      nal_type == 9);
    } else if (codec_type == 1) { // H.265
      nal_type = (nal_data[0] >> 1) & 0x3F;
      // VPS=32, SPS=33, PPS=34, AUD=35, PREFIX_SEI=39, SUFFIX_SEI=40
      // Include AUD, matching NVENC/AMF behaviour
      is_param_set = (nal_type == H265_NAL_VPS || nal_type == H265_NAL_SPS ||
                      nal_type == H265_NAL_PPS || nal_type == 35);

      // Debug: log the NAL types seen
      if (last_param_end == 0 || is_param_set) {
        blog(LOG_DEBUG, "[QSV H.265] Found NAL type=%u at offset=%zu%s",
             nal_type, (nal_data - data), is_param_set ? " (param set)" : "");
      }
    } else {
      // AV1 has no NAL structure; return 0
      return 0;
    }

    // Find the next start code
    size_t next_sc_size = 0;
    const uint8_t *next_nal =
        find_nal_start_code(nal_data, nal_remaining, &next_sc_size);

    if (is_param_set) {
      // This is a parameter set; record where it ends
      if (next_nal) {
        last_param_end = next_nal - data;
      } else {
        last_param_end = size;
      }
    } else {
      // Non-parameter-set NAL (usually IDR slice): return the previous end
      return last_param_end;
    }

    if (!next_nal)
      break;

    current = next_nal;
    remaining = size - (current - data);
  }

  return last_param_end;
}

/* ------------------------------------------------------------------------- */

void qsv_encoder_destroy(qsv_encoder_t *enc) {
  if (enc->loader) {
    MFXUnload(enc->loader);
    enc->loader = NULL;
  }
  if (enc->pmfxSurfaces) {
    for (int i = 0; i < enc->nSurfNum; i++) {
      if (enc->pmfxSurfaces[i]) {
        bfree(enc->pmfxSurfaces[i]->Data.Y);
        free(enc->pmfxSurfaces[i]);
      }
    }
    free(enc->pmfxSurfaces);
  }
  if (enc->mfxBS.Data)
    bfree(enc->mfxBS.Data);
  if (enc->extra_data)
    bfree(enc->extra_data);
  if (enc->profile)
    bfree(enc->profile);
  if (enc->preset)
    bfree(enc->preset);

  ntp_client_destroy(&enc->ntp_client);
  bfree(enc);
}

bool qsv_encoder_init(qsv_encoder_t *enc, obs_data_t *settings,
                      video_t *video) {
  // Basic settings mapping not used here, OBS calls create directly
  return true;
}

/* Helper to setup MFX session */
static bool init_vpl_session(qsv_encoder_t *enc) {
  mfxStatus sts = MFX_ERR_NONE;
  enc->loader = MFXLoad();
  if (!enc->loader)
    return false;

  mfxConfig cfg = MFXCreateConfig(enc->loader);
  mfxVariant impl_value;
  impl_value.Type = MFX_VARIANT_TYPE_U32;
  impl_value.Data.U32 = MFX_IMPL_TYPE_HARDWARE;

  /* MFXSetConfigFilterProperty expects mfxVariant by value in OneVPL 2.x */
  sts = MFXSetConfigFilterProperty(
      cfg, (const mfxU8 *)"mfxImplDescription.Impl", impl_value);
  if (sts != MFX_ERR_NONE) {
    // Log warning but continue?
  }

  sts = MFXCreateSession(enc->loader, 0, &enc->session);
  if (sts != MFX_ERR_NONE) {
    // Fallback to software?
    blog(LOG_WARNING, "[QSV Native] Hardware not found, trying Software...");
    // Reset loader/cfg needed logic basically, simpler to just try create
    // session without filter MFXUnload(enc->loader); enc->loader = MFXLoad();
    // ... For now, fail if no HW.
    return false;
  }

  blog(LOG_INFO, "[QSV Native] VPL Session Created (Impl: Hardware)");
  return true;
}

/* Create - Internal (public for unified encoder) */
void *qsv_encoder_create_internal(obs_data_t *settings,
                                  obs_encoder_t *encoder) {
  qsv_encoder_t *enc = bzalloc(sizeof(qsv_encoder_t));
  enc->encoder = encoder;

  video_t *video = obs_encoder_video(encoder);
  const struct video_output_info *voi = video_output_get_info(video);

  enc->width = voi->width;
  enc->height = voi->height;
  enc->fps_num = voi->fps_num;
  enc->fps_den = voi->fps_den;
  enc->bitrate = (int)obs_data_get_int(settings, "bitrate");
  enc->keyint = (int)obs_data_get_int(settings, "keyint_sec") * enc->fps_num /
                enc->fps_den;
  enc->bframes = (int)obs_data_get_int(settings, "bframes");

  /* Codec Type */
  enc->codec_type = (int)obs_data_get_int(settings, "codec_type");
  if (enc->codec_type < 0 || enc->codec_type > 2)
    enc->codec_type = 0; // Default to H.264

  /* NTP Init */
  const char *ntp_server = obs_data_get_string(settings, "ntp_server");
  uint16_t ntp_port = (uint16_t)obs_data_get_int(settings, "ntp_port");
  if (ntp_port == 0) ntp_port = 123;
  ntp_client_init(&enc->ntp_client, ntp_server, ntp_port);
  enc->ntp_enabled = true; // Always on for stamper
  enc->ntp_sync_interval_ms =
      (uint32_t)obs_data_get_int(settings, "ntp_sync_interval_ms");
  if (enc->ntp_sync_interval_ms == 0)
    enc->ntp_sync_interval_ms = 60000; // default 60 s
  ntp_client_start_background_sync(&enc->ntp_client, enc->ntp_sync_interval_ms);

  if (!init_vpl_session(enc)) {
    qsv_encoder_destroy(enc);
    return NULL;
  }

  /* Configure Encoder */
  memset(&enc->mfxParams, 0, sizeof(enc->mfxParams));

  /* Set CodecId based on codec_type */
  switch (enc->codec_type) {
  case 0: // H.264
    enc->mfxParams.mfx.CodecId = MFX_CODEC_AVC;
    blog(LOG_INFO, "[QSV Native] Using H.264 (AVC) codec");
    break;
  case 1: // H.265
    enc->mfxParams.mfx.CodecId = MFX_CODEC_HEVC;
    blog(LOG_INFO, "[QSV Native] Using H.265 (HEVC) codec");
    break;
  case 2: // AV1
    enc->mfxParams.mfx.CodecId = MFX_CODEC_AV1;
    blog(LOG_INFO, "[QSV Native] Using AV1 codec");
    break;
  default:
    enc->mfxParams.mfx.CodecId = MFX_CODEC_AVC;
    blog(LOG_WARNING, "[QSV Native] Unknown codec type %d, defaulting to H.264",
         enc->codec_type);
    break;
  }
  enc->mfxParams.mfx.TargetUsage = MFX_TARGETUSAGE_BALANCED; // Defaults
  enc->mfxParams.mfx.TargetKbps = enc->bitrate;
  enc->mfxParams.mfx.RateControlMethod = MFX_RATECONTROL_CBR;
  enc->mfxParams.mfx.FrameInfo.FrameRateExtN = enc->fps_num;
  enc->mfxParams.mfx.FrameInfo.FrameRateExtD = enc->fps_den;
  enc->mfxParams.mfx.FrameInfo.FourCC = MFX_FOURCC_NV12;
  enc->mfxParams.mfx.FrameInfo.ChromaFormat = MFX_CHROMAFORMAT_YUV420;
  enc->mfxParams.mfx.FrameInfo.PicStruct = MFX_PICSTRUCT_PROGRESSIVE;
  enc->mfxParams.mfx.FrameInfo.CropX = 0;
  enc->mfxParams.mfx.FrameInfo.CropY = 0;
  enc->mfxParams.mfx.FrameInfo.CropW = enc->width;
  enc->mfxParams.mfx.FrameInfo.CropH = enc->height;
  enc->mfxParams.mfx.FrameInfo.Width = ALIGN16(enc->width);
  enc->mfxParams.mfx.FrameInfo.Height = ALIGN16(enc->height);
  enc->mfxParams.mfx.GopPicSize = enc->keyint;
  enc->mfxParams.mfx.GopRefDist = enc->bframes + 1;
  enc->mfxParams.mfx.NumRefFrame = 3;
  enc->mfxParams.IOPattern = MFX_IOPATTERN_IN_SYSTEM_MEMORY;

  /* Init Encoder */
  mfxStatus sts = MFXVideoENCODE_Init(enc->session, &enc->mfxParams);
  if (sts != MFX_ERR_NONE) {
    blog(LOG_ERROR, "[QSV Native] MFXVideoENCODE_Init failed: %d", sts);
    qsv_encoder_destroy(enc);
    return NULL;
  }

  /* Query Allocation Requirements */
  mfxFrameAllocRequest Request;
  memset(&Request, 0, sizeof(Request));
  sts = MFXVideoENCODE_QueryIOSurf(enc->session, &enc->mfxParams, &Request);
  if (sts != MFX_ERR_NONE && sts != MFX_WRN_PARTIAL_ACCELERATION) {
    blog(LOG_ERROR, "[QSV Native] QueryIOSurf failed: %d", sts);
    // Try to proceed with manual guess if it fails? No, dangerous.
    // But maybe system memory mode doesn't strictly require this call?
    // Just in case, define defaults if 0.
  }

  enc->nSurfNum = Request.NumFrameSuggested;
  if (enc->nSurfNum < 1)
    enc->nSurfNum = 1; // Safety fallback

  blog(LOG_INFO, "[QSV Native] Allocating %d surfaces (Size: %dx%d)",
       enc->nSurfNum, enc->mfxParams.mfx.FrameInfo.Width,
       enc->mfxParams.mfx.FrameInfo.Height);

  enc->pmfxSurfaces = calloc(enc->nSurfNum, sizeof(mfxFrameSurface1 *));
  for (int i = 0; i < enc->nSurfNum; i++) {
    enc->pmfxSurfaces[i] = calloc(1, sizeof(mfxFrameSurface1));
    memcpy(&(enc->pmfxSurfaces[i]->Info), &(enc->mfxParams.mfx.FrameInfo),
           sizeof(mfxFrameInfo));
    // Allocate raw buffer
    size_t surfaceSize = enc->mfxParams.mfx.FrameInfo.Width *
                         enc->mfxParams.mfx.FrameInfo.Height * 3 / 2;

    // Check for overflow or zero
    if (surfaceSize == 0) {
      blog(LOG_ERROR, "[QSV Native] Surface size calc failed (W=%d, H=%d)",
           enc->mfxParams.mfx.FrameInfo.Width,
           enc->mfxParams.mfx.FrameInfo.Height);
      return NULL;
    }

    uint8_t *pData = bmalloc(surfaceSize);
    if (!pData) {
      blog(LOG_ERROR, "[QSV Native] bmalloc failed for surface %d (%zu bytes)",
           i, surfaceSize);
      return NULL;
    }

    // Clear memory (green/black?) or leave junk?
    memset(pData, 0, surfaceSize);

    enc->pmfxSurfaces[i]->Data.Y = pData;
    enc->pmfxSurfaces[i]->Data.UV =
        pData + enc->mfxParams.mfx.FrameInfo.Width *
                    enc->mfxParams.mfx.FrameInfo.Height;
    enc->pmfxSurfaces[i]->Data.Pitch = enc->mfxParams.mfx.FrameInfo.Width;
  }

  /* Bitstream Buffer */
  enc->mfxBS.MaxLength = enc->mfxParams.mfx.FrameInfo.Width *
                         enc->mfxParams.mfx.FrameInfo.Height * 4;
  enc->mfxBS.Data = bmalloc(enc->mfxBS.MaxLength);
  if (!enc->mfxBS.Data) {
    blog(LOG_ERROR, "[QSV Native] Bitstream buffer alloc failed");
    return NULL;
  }

  blog(LOG_INFO, "[QSV Native] Encoder Initialized: %dx%d %d kbps", enc->width,
       enc->height, enc->bitrate);

  /* Extra data is pulled from the bitstream after the first keyframe */
  enc->extra_data = NULL;
  enc->extra_data_size = 0;
  enc->extra_data_ready = false;

  blog(LOG_INFO,
       "[QSV Native] Extra data will be extracted from first keyframe");

  return enc;
}

bool qsv_encoder_encode_internal(void *data, struct encoder_frame *frame,
                                 struct encoder_packet *packet,
                                 bool *received_packet) {
  qsv_encoder_t *enc = data;

  static uint64_t frame_count = 0;
  frame_count++;

  // Log once every 30 frames (~1 s)
  if (frame_count % 30 == 1) {
    blog(LOG_INFO, "[QSV Native] Encode called: frame #%llu, PTS=%lld",
         frame_count, frame->pts);
  }

  /* Find Free Surface */
  int nIndex = -1;
  for (int i = 0; i < enc->nSurfNum; i++) {
    if (!enc->pmfxSurfaces[i]->Data.Locked) {
      nIndex = i;
      break;
    }
  }
  if (nIndex == -1) {
    blog(LOG_ERROR, "[QSV Native] No free surfaces");
    return false;
  }

  mfxFrameSurface1 *pSurface = enc->pmfxSurfaces[nIndex];
  if (!pSurface) {
    blog(LOG_ERROR, "[QSV Native] Surface pointer is NULL at index %d", nIndex);
    return false;
  }
  if (!pSurface->Data.Y) {
    blog(LOG_ERROR, "[QSV Native] Surface Y plane is NULL at index %d", nIndex);
    return false;
  }
  if (!frame->data[0]) {
    blog(LOG_ERROR, "[QSV Native] Input frame data[0] is NULL");
    return false;
  }

  int width = enc->mfxParams.mfx.FrameInfo.Width;
  int height = enc->mfxParams.mfx.FrameInfo.Height;

  /* Safety Check sizes */
  if (enc->width > width || enc->height > height) {
    blog(LOG_ERROR,
         "[QSV Native] Frame dimensions mismatch: enc %dx%d vs surface %dx%d",
         enc->width, enc->height, width, height);
    return false;
  }

  // Y Plane
  for (int i = 0; i < enc->height; i++) { // Use input height
    memcpy(pSurface->Data.Y + i * pSurface->Data.Pitch,
           frame->data[0] + i * frame->linesize[0], enc->width);
  }
  // UV Plane
  if (frame->data[1] && pSurface->Data.UV) {
    for (int i = 0; i < enc->height / 2; i++) {
      memcpy(pSurface->Data.UV + i * pSurface->Data.Pitch,
             frame->data[1] + i * frame->linesize[1], enc->width);
    }
  } else {
    // NV12 expects UV. If missing, log?
    // blog(LOG_WARNING, "UV data missing?");
  }

  pSurface->Data.TimeStamp =
      (mfxU64)frame->pts * 90000 / 1000000; // Rescale? OBS is ns?
  // OBS PTS is ns? No, obs_encoder_frame pts is usually relative to start.
  // Let's assume passed frame->pts needs conversion to 90kHz if MFX expects it.
  // Actually MFX timestamp is arbitrary ticks roughly 90kHz usually.

  mfxSyncPoint syncp;
  mfxStatus sts = MFXVideoENCODE_EncodeFrameAsync(enc->session, NULL, pSurface,
                                                  &enc->mfxBS, &syncp);

  if (sts > MFX_ERR_NONE && enc->mfxBS.DataLength > 0) {
    // Ignore warnings
  }
  if (sts == MFX_ERR_MORE_DATA || sts == MFX_WRN_DEVICE_BUSY) {
    *received_packet = false;
    return true; // Needed more input
  }
  if (sts != MFX_ERR_NONE && sts > 0) {
    // Warning
  } else if (sts < 0) {
    blog(LOG_ERROR, "[QSV Native] Encode failed: %d", sts);
    return false;
  }

  sts = MFXVideoCORE_SyncOperation(enc->session, syncp, 60000);
  if (sts != MFX_ERR_NONE) {
    blog(LOG_ERROR, "[QSV Native] Sync failed: %d", sts);
    return false;
  }

  /* Packet Ready */
  *received_packet = true;

  bool keyframe = (enc->mfxBS.FrameType & MFX_FRAMETYPE_I) ||
                  (enc->mfxBS.FrameType & MFX_FRAMETYPE_IDR);

  /* On the first keyframe, build extra_data from the bitstream if absent */
  if (keyframe && !enc->extra_data_ready) {
    bool extract_success = false;

    switch (enc->codec_type) {
    case 0: { // H.264
      uint8_t *sps = NULL, *pps = NULL;
      size_t sps_size = 0, pps_size = 0;

      if (extract_h264_params(enc->mfxBS.Data + enc->mfxBS.DataOffset,
                              enc->mfxBS.DataLength, &sps, &sps_size, &pps,
                              &pps_size)) {
        if (build_h264_extradata(sps, sps_size, pps, pps_size, &enc->extra_data,
                                 &enc->extra_data_size)) {
          enc->extra_data_ready = true;
          extract_success = true;
          blog(LOG_INFO, "[QSV Native] H.264 extra data extracted (%zu bytes)",
               enc->extra_data_size);
        }
        bfree(sps);
        bfree(pps);
      }
      break;
    }
    case 1: { // H.265
      uint8_t *vps = NULL, *sps = NULL, *pps = NULL;
      size_t vps_size = 0, sps_size = 0, pps_size = 0;

      if (extract_h265_params(enc->mfxBS.Data + enc->mfxBS.DataOffset,
                              enc->mfxBS.DataLength, &vps, &vps_size, &sps,
                              &sps_size, &pps, &pps_size)) {
        if (build_h265_extradata(vps, vps_size, sps, sps_size, pps, pps_size,
                                 &enc->extra_data, &enc->extra_data_size)) {
          enc->extra_data_ready = true;
          extract_success = true;
          blog(LOG_INFO, "[QSV Native] H.265 extra data extracted (%zu bytes)",
               enc->extra_data_size);
        }
        bfree(vps);
        bfree(sps);
        bfree(pps);
      }
      break;
    }
    case 2: { // AV1
      blog(LOG_INFO,
           "[QSV Native] AV1 extra data extraction not yet implemented");
      // AV1 is left alone for now: OBS SRT support for AV1 is limited
      enc->extra_data_ready = true; // mark handled so we stop retrying
      break;
    }
    }

    if (!extract_success && enc->codec_type < 2) {
      blog(LOG_WARNING,
           "[QSV Native] Failed to extract extra data from keyframe");
    }
  }

  /* SEI insertion (keyframes only — IVS player is hot-loop sensitive to
   * per-frame SEI) */
  uint8_t *sei_nal = NULL;
  size_t sei_nal_size = 0;

  /* NTP is refreshed on a background thread; just read the latest here. */
  bool ntp_valid =
      keyframe && ntp_client_get_time(&enc->ntp_client, &enc->current_ntp_time);

  if (ntp_valid) {
    uint8_t *payload = NULL;
    size_t payload_size = 0;
    if (build_ntp_sei_payload(frame->pts, &enc->current_ntp_time, &payload,
                              &payload_size)) {
      sei_nal_type_t nal_type = (enc->codec_type == 1) ? SEI_NAL_H265_PREFIX : SEI_NAL_H264;
      build_sei_nal_unit(payload, payload_size, nal_type, &sei_nal, &sei_nal_size);
      bfree(payload);

      blog(LOG_DEBUG,
           "[QSV Native] SEI stamped: PTS=%lld NTP_sec=%u (0x%08X) NTP_frac=%u keyframe=%d",
           frame->pts,
           enc->current_ntp_time.seconds, enc->current_ntp_time.seconds,
           enc->current_ntp_time.fraction, keyframe);
    } else {
      blog(LOG_ERROR, "[QSV Native] Failed to build NTP SEI payload");
    }
  } else if (keyframe) {
    blog(LOG_WARNING,
         "[QSV Native] Keyframe at PTS=%lld but NTP not synced, "
         "skipping SEI insertion", frame->pts);
  }

  /* Copy to OBS packet with correct SEI insertion position */
  size_t total_size = enc->mfxBS.DataLength + sei_nal_size;
  packet->data = bmalloc(total_size);

  const uint8_t *bitstream_data = enc->mfxBS.Data + enc->mfxBS.DataOffset;
  size_t bitstream_size = enc->mfxBS.DataLength;

  if (sei_nal) {
    if (keyframe) {
      /* Keyframe: insert SEI after parameter sets, before IDR slice */
      size_t param_sets_end = find_parameter_sets_end(
          bitstream_data, bitstream_size, enc->codec_type);

      if (param_sets_end > 0 && param_sets_end < bitstream_size) {
        /* Correct order: parameter sets -> SEI -> IDR slice */
        memcpy(packet->data, bitstream_data, param_sets_end);
        size_t offset = param_sets_end;

        memcpy(packet->data + offset, sei_nal, sei_nal_size);
        offset += sei_nal_size;

        size_t remaining = bitstream_size - param_sets_end;
        memcpy(packet->data + offset, bitstream_data + param_sets_end, remaining);

        blog(LOG_DEBUG,
             "[QSV Native] %s SEI inserted after parameter sets (offset: %zu)",
             enc->codec_type == 0   ? "H.264"
             : enc->codec_type == 1 ? "H.265"
                                    : "AV1",
             param_sets_end);
      } else {
        /* Keyframe without parameter sets: prepend SEI */
        memcpy(packet->data, sei_nal, sei_nal_size);
        memcpy(packet->data + sei_nal_size, bitstream_data, bitstream_size);
      }
    } else {
      /* Non-keyframe: prepend SEI before slice data */
      memcpy(packet->data, sei_nal, sei_nal_size);
      memcpy(packet->data + sei_nal_size, bitstream_data, bitstream_size);
    }

    bfree(sei_nal);
  } else {
    /* No SEI (NTP unavailable) */
    memcpy(packet->data, bitstream_data, bitstream_size);
  }

  packet->size = total_size;
  packet->type = OBS_ENCODER_VIDEO;
  packet->pts = frame->pts;
  packet->dts = frame->pts; // Approximate
  packet->keyframe = keyframe;

  /* Reset BS */
  enc->mfxBS.DataLength = 0;
  enc->mfxBS.DataOffset = 0;

  return true;
}

static void qsv_get_defaults(obs_data_t *settings) {
  obs_data_set_default_int(settings, "bitrate", 2500);
  obs_data_set_default_int(settings, "keyint_sec", 2);
  obs_data_set_default_int(settings, "bframes", 2);
  obs_data_set_default_string(settings, "ntp_server", "time.windows.com");
  obs_data_set_default_int(settings, "ntp_sync_interval_ms", 60000); // 60 s
}

static obs_properties_t *qsv_properties(void *unused) {
  obs_properties_t *props = obs_properties_create();
  obs_properties_add_int(props, "bitrate", "Bitrate (kbps)", 50, 50000, 50);
  obs_properties_add_int(props, "keyint_sec", "Keyframe Interval (s)", 1, 10,
                         1);
  obs_properties_add_int(props, "bframes", "B-Frames", 0, 4, 1);
  obs_properties_add_text(props, "ntp_server", "NTP Server", OBS_TEXT_DEFAULT);
  obs_properties_add_int(props, "ntp_sync_interval_ms", "NTP Sync Interval (ms)",
                         1000, 600000, 1000); // 1 s to 10 min
  return props;
}

static const char *qsv_get_name(void *type_data) {
  return "SEI Stamper (Intel QuickSync)";
}

void qsv_encoder_get_video_info_internal(void *data,
                                         struct video_scale_info *info) {
  info->format = VIDEO_FORMAT_NV12;
}

bool qsv_encoder_get_extra_data_internal(void *data, uint8_t **extra_data,
                                         size_t *size) {
  qsv_encoder_t *enc = (qsv_encoder_t *)data;
  if (!enc || !enc->extra_data)
    return false;
  *extra_data = enc->extra_data;
  *size = enc->extra_data_size;
  return true;
}

/* Static wrappers for obs_encoder_info */
static void *qsv_create(obs_data_t *settings, obs_encoder_t *encoder) {
  return qsv_encoder_create_internal(settings, encoder);
}

static void qsv_destroy(void *data) { qsv_encoder_destroy(data); }

static bool qsv_encode(void *data, struct encoder_frame *frame,
                       struct encoder_packet *packet, bool *received_packet) {
  return qsv_encoder_encode_internal(data, frame, packet, received_packet);
}

static void qsv_get_video_info(void *data, struct video_scale_info *info) {
  qsv_encoder_get_video_info_internal(data, info);
}

static bool qsv_get_extra_data(void *data, uint8_t **extra_data, size_t *size) {
  return qsv_encoder_get_extra_data_internal(data, extra_data, size);
}

struct obs_encoder_info qsv_encoder_info = {
    .id = "h264_qsv_native",
    .type = OBS_ENCODER_VIDEO,
    .codec = "h264",
    .get_name = qsv_get_name,
    .create = qsv_create,
    .destroy = (void (*)(void *))qsv_encoder_destroy,
    .encode = qsv_encode,
    .get_defaults = qsv_get_defaults,
    .get_properties = qsv_properties,
    .get_video_info = qsv_get_video_info,
    .get_extra_data = qsv_get_extra_data,
};

#else

/* Dummy implementation if VPL not enabled */
#include <obs-module.h>

struct obs_encoder_info qsv_encoder_info = {0};

#endif
