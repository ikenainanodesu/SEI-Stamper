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
                                   int payload_type, uint8_t **nal_unit,
                                   size_t *nal_size) {
  /* Standard H.264 SEI NAL construction */
  /* Start Code (00 00 00 01) + NAL Header (SEI=6) */
  /* Payload Type + Payload Size + Payload + Trailing Bits */

  // Simplified RBSP handling (no emulation prevention for simplicity, though
  // required for robust) For fixed UUIDs and Timestamps it's usually fine, but
  // let's be careful.
  size_t rbsp_size = 0;

  // Payload Type
  uint8_t type_byte = 5; // User Data Unregistered
  // Payload Size
  // If size < 255.

  size_t size_bytes = 1;
  if (payload_size >= 255)
    size_bytes += (payload_size / 255);

  size_t total_size =
      4 + 1 + 1 + size_bytes + payload_size + 1; // +1 for trailing
  *nal_unit = bmalloc(total_size);
  if (!*nal_unit)
    return false;

  uint8_t *p = *nal_unit;
  // Start Code
  *p++ = 0x00;
  *p++ = 0x00;
  *p++ = 0x00;
  *p++ = 0x01;
  // NAL Header (Forbidden=0, RefIdc=0, Type=SEI(6))
  *p++ = 0x06;

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

/* Create */
static void *qsv_create(obs_data_t *settings, obs_encoder_t *encoder) {
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

  /* NTP Init */
  /* NTP Init */
  const char *ntp_server = obs_data_get_string(settings, "ntp_server");
  ntp_client_init(&enc->ntp_client, ntp_server, 123);
  enc->ntp_enabled = true; // Always on for stamper

  if (!init_vpl_session(enc)) {
    qsv_encoder_destroy(enc);
    return NULL;
  }

  /* Configure Encoder */
  memset(&enc->mfxParams, 0, sizeof(enc->mfxParams));
  enc->mfxParams.mfx.CodecId = MFX_CODEC_AVC;
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

  /* Extract SPS/PPS for Extra Data */
  mfxExtCodingOptionSPSPPS spspps;
  memset(&spspps, 0, sizeof(spspps));
  spspps.Header.BufferId = MFX_EXTBUFF_CODING_OPTION_SPSPPS;
  spspps.Header.BufferSz = sizeof(spspps);

  uint8_t sps_buf[1024];
  uint8_t pps_buf[1024];
  spspps.SPSBuffer = sps_buf;
  spspps.SPSBufSize = 1024;
  spspps.PPSBuffer = pps_buf;
  spspps.PPSBufSize = 1024;

  mfxExtBuffer *ext_bufs[] = {(mfxExtBuffer *)&spspps};
  mfxVideoParam par;
  memset(&par, 0, sizeof(par));
  par.ExtParam = ext_bufs;
  par.NumExtParam = 1;

  sts = MFXVideoENCODE_GetVideoParam(enc->session, &par);
  if (sts == MFX_ERR_NONE) {
    /* Construct AVCC Header */
    /* Ref: ISO/IEC 14496-15 5.2.4.1 */
    uint16_t sps_len = spspps.SPSBufSize;
    uint16_t pps_len = spspps.PPSBufSize;

    enc->extra_data_size = 5 + 1 + 2 + sps_len + 1 + 2 + pps_len;
    enc->extra_data = bmalloc(enc->extra_data_size);

    if (enc->extra_data && sps_len > 3) {
      uint8_t *p = enc->extra_data;
      p[0] = 0x01;       // ConfigurationVersion
      p[1] = sps_buf[1]; // Profile
      p[2] = sps_buf[2]; // Profile Compatibility
      p[3] = sps_buf[3]; // Level
      p[4] = 0xFF;       // 111111 + 2 bits length size minus 1 (3 -> 4 bytes)

      p[5] = 0xE1; // 111 + 5 bits number of SPS (1)

      // SPS Length (Big Endian)
      p[6] = (sps_len >> 8) & 0xFF;
      p[7] = sps_len & 0xFF;
      memcpy(p + 8, sps_buf, sps_len);

      p += 8 + sps_len;

      p[0] = 0x01; // Number of PPS (1)
      p[1] = (pps_len >> 8) & 0xFF;
      p[2] = pps_len & 0xFF;
      memcpy(p + 3, pps_buf, pps_len);

      blog(LOG_INFO, "[QSV Native] Extradata generated: %zu bytes",
           enc->extra_data_size);
    } else {
      blog(LOG_ERROR, "[QSV Native] Failed to parse SPS for extradata");
    }
  } else {
    blog(LOG_WARNING, "[QSV Native] GetVideoParam(SPSPPS) failed: %d", sts);
  }

  return enc;
}

static bool qsv_encode(void *data, struct encoder_frame *frame,
                       struct encoder_packet *packet, bool *received_packet) {
  qsv_encoder_t *enc = data;

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

  /* NTP Time Update */
  uint64_t now = os_gettime_ns();
  if (enc->last_ntp_sync_time == 0 ||
      (now - enc->last_ntp_sync_time) > 60000000000ULL) {
    if (ntp_client_sync(&enc->ntp_client))
      enc->last_ntp_sync_time = now;
  }
  ntp_client_get_time(&enc->ntp_client, &enc->current_ntp_time);

  /* SEI Insertion */
  // Check if IDR/I frame to insert SEI.
  // MFXBS FrameType check.
  bool keyframe = (enc->mfxBS.FrameType & MFX_FRAMETYPE_I) ||
                  (enc->mfxBS.FrameType & MFX_FRAMETYPE_IDR);

  uint8_t *sei_nal = NULL;
  size_t sei_nal_size = 0;

  if (keyframe) {
    uint8_t *payload = NULL;
    size_t payload_size = 0;
    if (qsv_build_ntp_sei_payload(frame->pts, &enc->current_ntp_time, &payload,
                                  &payload_size)) {
      qsv_build_sei_nal_unit(payload, payload_size, 6, &sei_nal, &sei_nal_size);
      bfree(payload);

      blog(LOG_DEBUG, "[QSV Native] Inserted SEI: PTS=%lld NTP=%u.%u Size=%zu",
           frame->pts, enc->current_ntp_time.seconds,
           enc->current_ntp_time.fraction, sei_nal_size);
    } else {
      blog(LOG_WARNING, "[QSV Native] Failed to build NTP SEI payload");
    }
  }

  /* Copy to OBS packet */
  size_t total_size = enc->mfxBS.DataLength + sei_nal_size;
  packet->data = bmalloc(total_size);

  size_t offset = 0;
  if (sei_nal) {
    memcpy(packet->data, sei_nal, sei_nal_size);
    offset += sei_nal_size;
    bfree(sei_nal);
  }
  memcpy(packet->data + offset, enc->mfxBS.Data + enc->mfxBS.DataOffset,
         enc->mfxBS.DataLength);

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
}

static obs_properties_t *qsv_properties(void *unused) {
  obs_properties_t *props = obs_properties_create();
  obs_properties_add_int(props, "bitrate", "Bitrate (kbps)", 50, 50000, 50);
  obs_properties_add_int(props, "keyint_sec", "Keyframe Interval (s)", 1, 10,
                         1);
  obs_properties_add_int(props, "bframes", "B-Frames", 0, 4, 1);
  obs_properties_add_text(props, "ntp_server", "NTP Server", OBS_TEXT_DEFAULT);
  return props;
}

static const char *qsv_get_name(void *type_data) {
  return "SEI Stamper (Intel QuickSync)";
}

static void qsv_get_video_info(void *data, struct video_scale_info *info) {
  info->format = VIDEO_FORMAT_NV12;
}

static bool qsv_get_extra_data(void *data, uint8_t **extra_data, size_t *size) {
  qsv_encoder_t *enc = (qsv_encoder_t *)data;
  if (!enc || !enc->extra_data)
    return false;
  *extra_data = enc->extra_data;
  *size = enc->extra_data_size;
  return true;
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
