# SEI Stamper Plugin

<img src="pic\sei_stamper_gau.png" alt="isolated" width="250"/>


**Frame-Level Video Synchronization for OBS Studio**

[English](#english) | [中文](README.chs.md) | [日本語](README.jpn.md)

---

## English

### Overview

SEI Stamper is an OBS Studio plugin that enables **frame-level video synchronization** across multiple streams by embedding NTP timestamps into video streams using SEI (Supplemental Enhancement Information).

**Key Features:**
- 🎯 **Frame-accurate synchronization** using NTP timestamps
- 📡 **Multiple hardware encoders**: Intel QuickSync, NVIDIA NVENC, AMD AMF
- 🛠️ **NVENC/AMF frame transport fix**: H.264/H.265 video frame output is restored for NVIDIA NVENC and AMD AMF in the latest code
- 🎞️ **Multi-Codec Support**: **H.264** and **H.265 (HEVC)**
- 🚀 **GPU acceleration**: Hardware-accelerated encoding with SEI support
- 🔄 **Sender & Receiver**: Complete solution for encoding and decoding
- 🌐 **SRT streaming**: Built-in SRT receiver for low-latency streaming
- ⏱️ **Microsecond precision**: NTP-based timing for professional applications

### Use Cases

- Multi-camera live production synchronization
- Remote studio frame-level sync
- Broadcast-quality multi-source alignment
- Live concert/event multi-angle recording

### Demo Video

📺 **[Watch Demo Video on YouTube](https://youtu.be/JhizRlUpSlg)** 

This demonstration shows OBS sending 4 SRT streams with identical settings over a local network. with this plugin, all 4 streams are synchronized to within ±2 frame accuracy.

---

## Installation

### Quick Install (Recommended)

Download the latest release from [Releases](https://github.com/ikenainanodesu/sei-stamper/releases) page.

The release package includes:
- `sei-stamper.dll` - Main plugin
- `srt.dll` - SRT library for receiver functionality
- Locale files for multi-language support

### Prerequisites

- OBS Studio 32.0.4 or later
- Latest validated build environment: OBS Studio 32.1.2
- Windows 10/11 (64-bit)

### Manual Install Steps

1. **Download the release package** from the [Releases](https://github.com/ikenainanodesu/sei-stamper/releases) page

2. **Copy to OBS plugins directory:**
   ```powershell
   # Copy plugin DLL
   Copy-Item sei-stamper.dll "C:\Program Files\obs-studio\obs-plugins\64bit\"
   
   # Copy SRT library
   Copy-Item srt.dll "C:\Program Files\obs-studio\obs-plugins\64bit\"
   ```

3. **Copy locale files:**
   ```powershell
   # Create directory
   New-Item -ItemType Directory -Force `
       "C:\Program Files\obs-studio\data\obs-plugins\sei-stamper\locale"
   
   # Copy locale files
   Copy-Item data\locale\* `
       "C:\Program Files\obs-studio\data\obs-plugins\sei-stamper\locale\" -Recurse
   ```

4. **Restart OBS Studio**

---

## Usage

### Sender (Encoder)

1. Open **Settings → Output → Output Mode: Advanced**
2. Select a SEI Stamper encoder based on your desired codec:
   - **SEI Stamper (H.264)** - Best compatibility
   - **SEI Stamper (H.265)** - High efficiency (HEVC), 30-50% bandwidth savings. (Note: Supports P2P caller-listener sync. SLS server support is limited, currently unstable, seeking optimization solutions)
3. In the encoder settings, select your **Hardware Encoder**:
   - Intel QuickSync
   - NVIDIA NVENC
   - AMD AMF

> **NVENC/AMF status**: The latest code includes the PR #6 fixes for NVIDIA NVENC and AMD AMF H.264/H.265 video frame transmission. If you are upgrading from an older package and see audio-only output or missing video frames, rebuild or install a release that includes this fix.

4. Configure encoder properties:
   - **NTP Server**: `time.windows.com` (or your preferred NTP server)
   - **Enable NTP Sync**: ✓
5. Start streaming/recording

> **⚠️ Note on H.265**: H.265 supports Point-to-Point (Caller-Listener) synchronization. Support for SLS servers is currently limited and may exhibit unstable performance. We are actively looking for solutions to improve it.

### Receiver (Source)

1. In your OBS scene, click **Add Source +**
2. Select **SEI Receiver**
3. Configure the source:
   - **SRT URL**: `srt://sender-ip:port` (e.g., `srt://192.168.1.100:9000`)
   - **Enable NTP Synchronization**: ✓
   - **NTP Server**: Same as sender
4. Click **OK**

**Note**: The receiver **requires a manual match** of the sender's codec format (H.264/H.265). Please ensure the "Codec Format" in the receiver settings matches the encoder used by the sender.

---

## Verification

### Check SEI Data with FFprobe

```powershell
# View frame information
ffprobe -select_streams v:0 -show_frames output.mp4 2>&1 | Select-String "SEI"

# Detailed frame data
ffprobe -select_streams v:0 -show_frames -show_entries frame=pict_type output.mp4
```

### Check with MediaInfo

```powershell
MediaInfo --Full output.mp4 | Select-String "SEI"
```

---

## Technical Details

### Architecture

```
┌─────────────┐         ┌──────────────┐         ┌─────────────┐
│   Sender    │         │  SRT Stream  │         │  Receiver   │
│  (Encoder)  │────────▶│   + SEI      │────────▶│  (Source)   │
└─────────────┘         └──────────────┘         └─────────────┘
      │                                                  │
      ▼                                                  ▼
┌─────────────┐                                  ┌─────────────┐
│ NTP Client  │◀─────────────────────────────────▶│ NTP Client  │
└─────────────┘         NTP Server               └─────────────┘
```

### SEI Format

- **UUID**: Custom identifier (`a5b3c2d1-e4f5-6789-abcd-ef0123456789`)
- **Payload Type**: User Data Unregistered (Type 5)
- **Data Structure**:
  - UUID (16 bytes)
  - PTS (8 bytes)
  - NTP Timestamp (8 bytes: 4 bytes seconds + 4 bytes fraction)

### NTP Synchronization Strategy

#### Encoder (Sender)
- **Sync Interval**: Every 60 seconds
- **Trigger**: Automatic periodic sync during encoding
- **Purpose**: Ensure encoder's NTP time remains accurate

#### Receiver (Source)
- **Intelligent Sync**: Adaptive synchronization using two triggers
  1. **Keyframe Trigger**: Syncs on keyframes (IDR) with **minimum 10-second interval**
  2. **Drift Detection**: Syncs when time drift exceeds configured threshold (default 50ms)
- **Purpose**: Maintain high precision while minimizing network overhead

### Supported Encoders

| Encoder Name | Codec | Supported Hardware | Status |
|--------------|-------|--------------------|--------|
| SEI Stamper (H.264) | H.264/AVC | Intel, NVIDIA, AMD | ✅ Verified; NVENC/AMF frame transport fixed |
| SEI Stamper (H.265) | H.265/HEVC | Intel, NVIDIA, AMD | ✅ P2P verified; NVENC/AMF frame transport fixed; ⚠️ limited SLS support |

---

## Disclaimer

Portions of this project's code and documentation were generated with the assistance of AI tools. By using this software, you acknowledge and agree that:
1. The software is provided "as is", without warranty of any kind.
2. The authors and contributors shall not be liable for any damages or data loss arising from the use of this plugin.

---

## License

GPL-2.0 License - aligned with OBS Studio license.

See the [LICENSE](LICENSE) file for details.

---

## Release Notes

### v1.3.0 (2026-09-03)

**🔧 NTP reliability (cross-platform):**
- 🪟 **Fixed NTP on Windows**: `SO_RCVTIMEO` was set with a `struct timeval`, but Winsock expects a `DWORD` of milliseconds — it read `tv_sec` (5) as a **5 ms** timeout, so every sync failed with `WSAETIMEDOUT`. NTP never worked on Windows before this.
- 🌐 **Try every resolved address**: `getaddrinfo` can return several addresses (often an IPv6 record first); the client now tries each until one answers instead of only the first.
- 🛡️ **Response validation**: reject Kiss-o'-Death (stratum 0), unsynchronized (stratum ≥ 16) and non-server-mode replies instead of anchoring on a bogus time.
- 🐛 **Receive error handling**: a failed `recvfrom` no longer slips past a signed/unsigned length check; failures log the socket error code so the cause is visible.

**🚀 Non-blocking sync:**
- ⏱️ **Background NTP thread**: sync now runs on its own thread (opt-in `ntp_client_start_background_sync`), so the network round-trip never blocks the encode path; encoders just read the latest cached time. State is mutex-guarded for a consistent snapshot.
- 🔧 **Configurable NTP port** via the `ntp_port` setting (defaults to 123).

**🎞️ Encoding:**
- 🔑 **Keyframe-keyed parameter-set re-injection (NVENC)**: SPS/PPS are re-injected on *every* keyframe, not only when an SEI is present — so a keyframe emitted before the first NTP sync is still decodable by a mid-stream MPEG-TS/SRT joiner.
- 🛠️ **AMF preset mapping**: the unified encoder's `fast` preset is mapped to AMF's `speed` (AMF's `quality` option rejects `fast`).

**🧩 Compatibility:**
- ✅ **FFmpeg 8 / avcodec 62**: use `AV_FRAME_FLAG_KEY` instead of the removed `AVFrame::key_frame` field so the receiver source builds on current FFmpeg.

**📝 Housekeeping:**
- Source comments translated to English throughout.

### v1.2.3-beta (2026-06-07)

**🔧 Fixes & Validation:**
- ✅ **NVENC/AMF video frame transmission fixed**: Resolved the issue where NVIDIA NVENC and AMD AMF H.264/H.265 encoders could fail to deliver video frames to streaming outputs.
- 🛠️ **NVENC compatibility**: Normalized NVENC presets to the modern `p1`-`p7` preset family, corrected codec-specific profile handling, restored container headers for RTMP/recording, and keeps SPS/PPS/VPS available inline for MPEG-TS/SRT keyframes.
- 🛠️ **AMF compatibility**: Added codec-specific AMF profile handling so H.265 no longer inherits invalid H.264 profile names such as `high`.
- ✅ **Build validation**: Confirmed local Release build against OBS Studio **32.1.2** dependencies.

### v1.2.2 (2026-03-26)

**✨ Updates:**
- 🛠️ **Encoder Refactoring**: Refactored the H.265 (HEVC) encoder core logic by removing global header flags to force in-band parameter sets, significantly improving compatibility with MPEG-TS containers and streaming servers like SLS.
- 📝 **Documentation**: Removed AV1 encoding descriptions. Updated H.265 status to reflect limited SLS server support.
- 🔧 **Code Maintenance**: Fixed IDE thread compilation warnings (`pthread.h`).

### v1.2.1 (2026-03-24)

**✨ New Features & Improvements:**
- 🏷️ **Receiver Version Display**: Added a plugin version label at the bottom of the SEI Receiver's properties UI for easier identification.

**🔧 Configuration & Building:**
- ⚙️ **Dynamic CMake Dependency Parsing**: Updated `CMakeLists.txt` to dynamically search for the newest `obs-deps-*` folder. This guarantees build script compatibility with future OBS Studio releases without manual modification.
- ✅ Verified build compatibility with OBS Studio **32.1.0** framework and dependencies.

### v1.2.0 (2026-01-22)

**🎉 New Features:**
- ✨ **Multi-Codec Support**: Added full support for **H.265 (HEVC)** and **AV1** encoding.
- 🛠️ **Three Independent Encoders**:
  - `SEI STAMPER (H.264)`
  - `SEI STAMPER (H.265)`
  - `SEI STAMPER (AV1)`
  - Each supports hardware acceleration (Intel/NVIDIA/AMD).
- 🧠 **Receiver Configuration**: The receiver requires manual selection of the matching codec format for proper decoding.

**⚠️ Important limitation:**
- **H.264** and **H.265** are fully supported for SRT streaming.
- **AV1** encoding is available, but OBS Studio's SRT output may not support AV1 depending on the version.

### v1.1.3 (2026-01-04)

**✨ New Features:**
- ⚙️ **Custom Receiver Sync Interval**: Added `NTP Sync Interval` setting to the receiver.

### v1.1.2 (2026-01-04)

**🔧 Bug Fixes:**
- 🐛 **Fixed Receiver Stuttering**: Added minimum 10-second interval for keyframe-triggered NTP sync.
- 🛡️ **Network Resilience**: Added intelligent backoff mechanism.

### v1.1.0 (2026-01-04)

**🎉 New Features:**
- ✨ **NVIDIA NVENC Support**
- ✨ **AMD AMF Support**
- 🚀 **Multi-GPU Support**

### v1.0.0 (2026-01-04)

**Initial Release**

---

**Current Version**: 1.3.0
**Last Updated**: 2026-09-03
