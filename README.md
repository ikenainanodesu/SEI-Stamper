# SEI Stamper Plugin

<img src="pic/sei_stamper_gau.png" alt="SEI Stamper" width="250"/>

NTP timestamps and custom SEI metadata for aligning multiple video streams in OBS Studio.

[English](README.md) | [中文](README.chs.md) | [日本語](README.jpn.md)

## Current project status

The source version is **1.3.0**. This document describes the checked-in implementation; a source version does not establish that a matching release package has been published. (Sources: [CMakeLists.txt](CMakeLists.txt), [module registration](src/sei-stamper-plugin.c).)

| Feature | Implementation and limits |
| --- | --- |
| H.264 / H.265 | Intel QuickSync, NVIDIA NVENC and AMD AMF backends insert NTP SEI on keyframes |
| ~~AV1~~ | **Synchronization is currently unavailable.** Encoder and receiver options remain registered, but timestamp insertion still uses the H.264/H.265 NAL path; no AV1-specific metadata packaging is implemented, so AV1 is not a supported timestamp synchronization path |
| Receiver | Opens SRT through FFmpeg, decodes video and an available audio track, and defaults to automatic codec detection with a manual override |
| NTP | Sender sync runs in the background; the receiver still makes synchronous requests during initialization and receive processing |
| Validation | Windows x64 Release build and 60 standalone checks passed; DLL dependency loading, API version and SRT protocol lookup passed against official OBS 32.2.2 runtimes; OBS plugin initialization, GPU encoding and multi-stream SRT were not tested |

Implementation sources: [unified encoder](src/unified-encoder.c), [QSV](src/qsv-encoder.c), [NVENC](src/nvenc-encoder.c), [AMF](src/amd-encoder.c), [receiver](src/sei-receiver-source.c), [tests](tests/test_ntp_sei.c).

H.265 through SLS retains the project's existing limited-support status and was not retested here. AMF includes FFmpeg 8 compatibility changes, but AMD hardware was not tested in this update. Measure synchronization error, end-to-end latency and CPU overhead on the actual setup; no fixed accuracy or performance guarantee is made.

The [project demo video](https://youtu.be/JhizRlUpSlg) remains a historical demonstration, not a benchmark of the current source.

## Installation

The build configuration targets **Windows x64 / OBS Studio 32.2 series / FFmpeg 8** and rejects dependency bundles with libavcodec major versions below 62. Match the OBS and FFmpeg runtime libraries to those used for the plugin build; a higher OBS version alone does not establish compatibility. (Source: [CMakeLists.txt](CMakeLists.txt).)

1. Close OBS and choose an environment-compatible package from [Releases](https://github.com/ikenainanodesu/SEI-Stamper/releases), or build from source.
2. Copy the plugin and locale files into the OBS installation, normally `C:\Program Files\obs-studio`. Writing there normally requires administrator privileges.
3. If the package includes a matching SRT runtime, copy it according to the package layout. The build script copies `srt.dll` only if it finds one; its presence is not guaranteed.
4. Restart OBS and check for `SEI Stamper Plugin loaded` in the log and the encoder/receiver entries in the UI.

```text
obs-studio/
├── obs-plugins/64bit/
│   ├── sei-stamper.dll
│   └── srt.dll                 # When supplied and required by the runtime
└── data/obs-plugins/sei-stamper/locale/
    ├── en-US.ini
    └── zh-CN.ini
```

The repository supplies English and Simplified Chinese UI resources, not Japanese UI localization. SRT reception depends on SRT support in the FFmpeg runtime; copying one DLL does not by itself establish support. (Sources: [staging script](build_and_install.bat), [locale files](data/locale), [receiver](src/sei-receiver-source.c).)

## Usage

### Sender

1. Open **Settings → Output → Output Mode: Advanced**.
2. Select `SEI STAMPER (H.264)` or `SEI STAMPER (H.265)`.
3. Select a supported GPU under **Hardware Encoder**: Intel QuickSync, NVIDIA NVENC or AMD AMF. Intel is the default; selection is not automatic hardware detection.
4. Set bitrate, keyframe interval, NTP server, port and sync interval. Configure matching NTP time sources on all senders and receivers used for alignment.
5. Configure the streaming URL or recording destination in OBS output settings and start output. The encoder itself does not create an SRT listener.

Defaults are **2500 kbps**, a **2-second** keyframe interval and **0 B-frames**. Sender NTP defaults to `pool.ntp.org`, port **123**, interval **60000 ms**. (Source: [unified encoder defaults and properties](src/unified-encoder.c).)

All three hardware backends currently enable NTP directly and do not read the unified UI's `ntp_enabled` toggle. Do not rely on clearing “Enable NTP Sync” to disable sender NTP. No valid NTP SEI is inserted before the first successful sync; afterward the clock is read and timestamps are inserted only on keyframes. (Sources: [QSV](src/qsv-encoder.c), [NVENC](src/nvenc-encoder.c), [AMF](src/amd-encoder.c).)

### Receiver

1. Add **SEI Receiver** to the scene.
2. Enter the SRT URL for the sender or relay, matching connection modes, addresses and ports on both ends. The default `srt://127.0.0.1:9000` must be changed for another machine.
3. Leave **Codec Format** on **Auto** unless overriding detection is necessary; a manual H.264/H.265 choice must match the sender.
4. Enable NTP and use the sender's time source. The receiver defaults to `time.windows.com`, which differs from the sender default.
5. Software decoding is the default. QSV, NVDEC and AMF choices depend on the device, driver and FFmpeg build.

The URL, including query parameters, is passed to FFmpeg as entered. The receiver also attempts to decode an available audio track. (Source: [receiver defaults, properties and connection code](src/sei-receiver-source.c).)

### NTP settings and behavior

| Setting | Sender | Receiver |
| --- | --- | --- |
| Default server | `pool.ntp.org` | `time.windows.com` |
| Default port / UI range | 123 / 1–65535 | 123 / 1–65535 |
| Default sync interval | 60000 ms | 10000 ms |
| Sync interval UI range | 1000–300000 ms | 100–3600000 ms |
| Default drift threshold / UI range | Not exposed | 50 ms / 10–1000 ms |

The sender background thread attempts an immediate sync, then waits the configured interval after each request finishes. On NTP-bearing received frames, a keyframe or a time difference above the threshold can trigger sync; both triggers obey the configured minimum interval. Ten seconds is the receiver default, not a fixed lower bound. Receiver requests can still block receive processing: the receive timeout is five seconds per address, and multiple address attempts can take longer. (Sources: [NTP client](src/ntp-client.c), [encoder properties](src/unified-encoder.c), [receiver](src/sei-receiver-source.c).)

## Building from source

Use CMake 3.20+, Visual Studio 2022 with the C++ desktop workload, a Release-built OBS source tree and a matching dependency bundle. OBS and SRT sources/binaries are **not bundled** in this repository; `obs/`, `obs-studio-master/` and `srt/` are ignored. (Sources: [CMakeLists.txt](CMakeLists.txt), [.gitignore](.gitignore).)

| Setting | Meaning / default |
| --- | --- |
| `OBS_SOURCE_DIR` | Required OBS source root |
| `OBS_BUILD_DIR` | `OBS_SOURCE_DIR/build` |
| `OBS_DEPS_DIR` | Last name-sorted matching `obs-deps-20??-??-??-x64` / `windows-deps-20??-??-??-x64` directory under `OBS_SOURCE_DIR/.deps`; set explicitly when multiple bundles coexist |
| `CMAKE_GENERATOR` | Batch script only; defaults to `Visual Studio 17 2022` |

The first three settings accept CMake `-D` flags or environment variables. The build expects Release paths such as `libobs/Release/obs.lib`. Dependency validation checks the actual `LIBAVCODEC_VERSION_MAJOR >= 62`; bundle names/dates alone do not prove compatibility. (Source: [CMakeLists.txt](CMakeLists.txt).)

Run from the project root, replacing paths with your actual SDK locations:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
  -DOBS_SOURCE_DIR="C:/obs-studio" `
  -DOBS_BUILD_DIR="C:/obs-studio/build" `
  -DOBS_DEPS_DIR="C:/obs-studio/.deps/windows-deps-2026-08-26-x64"
cmake --build build --config Release
cmake --install build --config Release --prefix out/obs-studio
```

The DLL is `build/plugin/Release/sei-stamper.dll`; the installable tree is `out/obs-studio/`. CMake installation stages only the plugin and `data/`, not SRT or other runtime dependencies.

Alternatively, from the project root:

```powershell
$env:OBS_SOURCE_DIR = "C:\obs-studio"
$env:OBS_DEPS_DIR = "C:\obs-studio\.deps\windows-deps-2026-08-26-x64"
.\build_and_install.bat
```

The script **deletes and recreates the project's `build/` directory**, stages files under `out/obs-studio/`, and attempts to copy `srt.dll` from a local SRT build or the OBS runtime directory. It does not install directly into the system OBS installation. (Source: [build_and_install.bat](build_and_install.bat).)

## Verification and troubleshooting

Run from the project root:

```powershell
.\tests\run_tests.bat
```

The local run on **2026-09-05 passed 60 checks with 0 failures**. These cover NTP conversion and sync anchoring, SEI payload layout, NAL scanning and extradata conversion. The test file copies pure functions instead of linking production sources; it does not test NTP networking, threads, OBS loading, GPUs or multi-stream synchronization. (Sources: [runner](tests/run_tests.bat), [test implementation](tests/test_ntp_sei.c).)

On the same date, the 1.3.0 Windows x64 Release build passed using MSVC 19.50 / CMake 4.2.1 / Ninja. DLL loading, `obs_module_ver()` and SRT protocol lookup passed against the official OBS 32.2.2 portable runtime. This package uses official 32.2.2 headers and OBS/pthread import libraries generated from that version's DLL exports. OBS plugin initialization, GPU processing and streaming were not tested. (Source: build record included in the [v1.3.0 release assets](https://github.com/ikenainanodesu/SEI-Stamper/releases/tag/v1.3.0).)

Check the OBS log for deployment issues:

- **Plugin missing:** check DLL/locale paths and matching OBS/FFmpeg runtime libraries.
- **Encoder creation fails:** check the selected hardware, codec, driver and backend initialization error.
- **SRT connection fails:** check URL, connection modes, listener availability, firewall and FFmpeg SRT support.
- **No NTP SEI:** look for `NTP sync successful` and wait for the next keyframe. Check DNS, UDP port and socket errors if requests fail.
- **Stuttering or inconsistent alignment:** check time sources, receiver sync intervals and request duration, then measure with visible frame counters or timecode.

`ffprobe -select_streams v:0 -show_frames output.mp4` can help inspect frames. Generic SEI output does not establish that this plugin's UUID/timestamps are correct or that streams are synchronized; validate the custom payload below. (Source: [SEI construction and parsing](src/sei-handler.c).)

## SEI format

The custom payload is **32 bytes**, with numeric fields written in big-endian order. This excludes NAL headers, payload type/length, escaping and trailing bits.

| Field | Size |
| --- | --- |
| UUID `a5b3c2d1-e4f5-6789-abcd-ef0123456789` | 16 bytes |
| Frame PTS (int64) | 8 bytes |
| NTP seconds (uint32) | 4 bytes |
| NTP fraction (uint32) | 4 bytes |

The payload uses User Data Unregistered (type 5), H.264 NAL type 6 or H.265 Prefix SEI type 39. This packaging is not an AV1 format. (Sources: [sei-handler.h](src/sei-handler.h), [sei-handler.c](src/sei-handler.c), [insertion path](src/nvenc-encoder.c).)

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

### v1.3.0 (2026-09-04)

- Corrected the Windows NTP receive timeout to 5000 ms, added fallback across resolved addresses, and improved receive error logging.
- Validate server mode, leap indicator, stratum and the echoed originate timestamp. Request/reply matching is not cryptographic authentication.
- Anchor the clock at the offset-corrected receive time (T4 + offset), logging offset and round-trip delay.
- Move sender NTP requests to background threads, use the unified `ntp_sync_interval_ms` setting and configurable port, and read/stamp timestamps only on keyframes.
- Reinsert available SPS/PPS(/VPS) on NVENC/AMF keyframes missing SPS; reject HEVC HVCC data in the H.264 AVCC conversion fallback. This does not guarantee header recovery for every HEVC stream.
- Update AMF global-header handling and map the `fast` preset to `speed`; AMD hardware validation remains outstanding.
- Use `AV_FRAME_FLAG_KEY` for FFmpeg 8, configurable OBS SDK paths, and a libavcodec major-version floor of 62.
- Add standalone pure-function checks. See the validation section above for the current run and coverage limits.

Sources: [NTP client](src/ntp-client.c), [NVENC](src/nvenc-encoder.c), [AMF](src/amd-encoder.c), [QSV](src/qsv-encoder.c), [receiver](src/sei-receiver-source.c), [CMake](CMakeLists.txt), [tests](tests/test_ntp_sei.c).

The entries below are retained historical release notes, not current compatibility or validation claims.

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
- ✨ **Multi-Codec Support**: Added full support for **H.265 (HEVC)** and ~~**AV1**~~ encoding (AV1 synchronization is currently unavailable).
- 🛠️ **Three Independent Encoders**:
  - `SEI STAMPER (H.264)`
  - `SEI STAMPER (H.265)`
  - ~~`SEI STAMPER (AV1)`~~ (synchronization currently unavailable)
  - Each supports hardware acceleration (Intel/NVIDIA/AMD).
- 🧠 **Receiver Configuration**: The receiver requires manual selection of the matching codec format for proper decoding.

**⚠️ Important limitation:**
- **H.264** and **H.265** are fully supported for SRT streaming.
- ~~**AV1** encoding is available, but OBS Studio's SRT output may not support AV1 depending on the version.~~ **Current status: AV1 synchronization is unavailable.**

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
**Last Updated**: 2026-09-05
