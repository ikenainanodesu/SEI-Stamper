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
- 🎞️ **Multi-Codec Support**: **H.264**, **H.265 (HEVC)**, and **AV1**
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
   - **SEI Stamper (H.265)** - High efficiency (HEVC), 30-50% bandwidth savings (Recommended)
   - **SEI Stamper (AV1)** - Next-gen efficiency (*Note: SRT streaming support depends on OBS version*)
3. In the encoder settings, select your **Hardware Encoder**:
   - Intel QuickSync
   - NVIDIA NVENC
   - AMD AMF
4. Configure encoder properties:
   - **NTP Server**: `time.windows.com` (or your preferred NTP server)
   - **Enable NTP Sync**: ✓
5. Start streaming/recording

> **⚠️ Note on AV1**: While the plugin supports AV1 encoding, current versions of OBS Studio may not support AV1 for SRT streaming output. H.264 and H.265 are fully verified for SRT streaming.

### Receiver (Source)

1. In your OBS scene, click **Add Source +**
2. Select **SEI Receiver**
3. Configure the source:
   - **SRT URL**: `srt://sender-ip:port` (e.g., `srt://192.168.1.100:9000`)
   - **Enable NTP Synchronization**: ✓
   - **NTP Server**: Same as sender
4. Click **OK**

**Note**: The receiver **automatically detects** the codec format (H.264/H.265/AV1). No manual selection is needed.

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
| SEI Stamper (H.264) | H.264/AVC | Intel, NVIDIA, AMD | ✅ Verified |
| SEI Stamper (H.265) | H.265/HEVC | Intel, NVIDIA, AMD | ✅ Verified (Rec.)|
| SEI Stamper (AV1) | AV1 | Intel, NVIDIA, AMD | ⚠️ (OBS SRT limit)|

---

## Disclaimer

Portions of this project's code and documentation were generated with the assistance of AI tools. By using this software, you acknowledge and agree that:
1. The software is provided "as is", without warranty of any kind.
2. The authors and contributors shall not be liable for any damages or data loss arising from the use of this plugin.

---

## Release Notes

### v1.2.0 (2026-01-22)

**🎉 New Features:**
- ✨ **Multi-Codec Support**: Added full support for **H.265 (HEVC)** and **AV1** encoding.
- 🛠️ **Three Independent Encoders**:
  - `SEI STAMPER (H.264)`
  - `SEI STAMPER (H.265)`
  - `SEI STAMPER (AV1)`
  - Each supports hardware acceleration (Intel/NVIDIA/AMD).
- 🧠 **Smart Receiver**: Automatically detects stream codec format. Removed manual "Codec Format" selection.

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

**Current Version**: 1.2.0  
**Last Updated**: 2026-01-22
