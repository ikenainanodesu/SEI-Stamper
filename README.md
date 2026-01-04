# OBS SEI Stamper Plugin

<img src="pic\sei_stamper_gau.png" alt="isolated" width="250"/>


**Frame-Level Video Synchronization for OBS Studio**

[English](#english) | [中文](README.chs.md) | [日本語](README.jpn.md)

---

## English

### Overview

OBS SEI Stamper is an OBS Studio plugin that enables **frame-level video synchronization** across multiple streams by embedding NTP timestamps into video streams using SEI (Supplemental Enhancement Information).

**Key Features:**
- 🎯 **Frame-accurate synchronization** using NTP timestamps
- 📡 **Intel QuickSync H.264 encoder**: Hardware-accelerated encoding with SEI support
- 🔄 **Sender & Receiver**: Complete solution for encoding and decoding
- 🌐 **SRT streaming**: Built-in SRT receiver for low-latency streaming
- ⏱️ **Microsecond precision**: NTP-based timing for professional applications

### Use Cases

- Multi-camera live production synchronization
- Remote studio frame-level sync
- Broadcast-quality multi-source alignment
- Live concert/event multi-angle recording

---

## Installation

### Quick Install (Recommended)

Download the latest release from [Releases](https://github.com/yourusername/obs-sei-stamper/releases) page.

The release package includes:
- `obs-sei-stamper.dll` - Main plugin
- `srt.dll` - SRT library for receiver functionality
- Locale files for multi-language support

### Prerequisites

- OBS Studio 28.0 or later
- Windows 10/11 (64-bit)

### Manual Install Steps

1. **Download the release package** from the [Releases](https://github.com/yourusername/obs-sei-stamper/releases) page

2. **Copy to OBS plugins directory:**
   ```powershell
   # Copy plugin DLL
   Copy-Item obs-sei-stamper.dll "C:\Program Files\obs-studio\obs-plugins\64bit\"
   
   # Copy SRT library
   Copy-Item srt.dll "C:\Program Files\obs-studio\obs-plugins\64bit\"
   ```

3. **Copy locale files:**
   ```powershell
   # Create directory
   New-Item -ItemType Directory -Force `
       "C:\Program Files\obs-studio\data\obs-plugins\obs-sei-stamper\locale"
   
   # Copy locale files
   Copy-Item data\locale\* `
       "C:\Program Files\obs-studio\data\obs-plugins\obs-sei-stamper\locale\" -Recurse
   ```

4. **Restart OBS Studio**

---

## Usage

### Sender (Encoder)

1. Open **Settings → Output → Output Mode: Advanced**
2. Select the SEI Stamper encoder:
   - **SEI Stamper (H.264 QuickSync)**
3. Configure encoder properties:
   - **NTP Server**: `time.windows.com` (or your preferred NTP server)
   - **NTP Port**: `123` (default)
   - **Enable NTP Sync**: ✓
4. Start streaming/recording

The encoder will automatically insert NTP timestamps into every frame using SEI metadata.

### Receiver (Source)

1. In your OBS scene, click **Add Source +**
2. Select **SEI Receiver**
3. Configure the source:
   - **SRT URL**: `srt://sender-ip:port` (e.g., `srt://192.168.1.100:9000`)
   - **Enable NTP Synchronization**: ✓
   - **NTP Server**: Same as sender
4. Click **OK**

The receiver will:
- Connect to the SRT stream
- Decode video frames
- Extract NTP timestamps from SEI
- Synchronize playback to frame-level accuracy

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

### Supported Encoder

| Encoder | SEI NAL Type | Hardware Acceleration | Status |
|---------|--------------|----------------------|--------|
| H.264   | Type 6       | Intel QuickSync      | ✅     |

---

## Building from Source

### Requirements

- **CMake** 3.20 or later
- **Visual Studio 2022** (with C++ Desktop Development workload)
- **OBS Studio source code** (included as dependency)
- **FFmpeg libraries** (provided by OBS)
- **libsrt** (included in repository)

### Quick Build (Recommended)

For users without build experience, use the automated build script:

1. **Run the build script:**
   ```powershell
   # Navigate to project directory
   cd obs-sei-stamper
   
   # Run the automated build script
   .\build_and_install.bat
   ```

2. **Get the plugin:**
   - After successful build, plugin files will be in `out/obs-studio/` directory
   - Plugin structure mirrors OBS installation directory

3. **Install:**
   - Copy contents of `out/obs-studio/` to your OBS installation directory
   - Default location: `C:\Program Files\obs-studio`
   - **Administrator privileges required**

### Manual Build Steps

If you prefer manual control over the build process:

1. **Clone the repository:**
   ```bash
   git clone https://github.com/yourusername/obs-sei-stamper.git
   cd obs-sei-stamper
   ```

2. **Configure CMake:**
   ```powershell
   mkdir build
   cd build
   cmake .. -G "Visual Studio 17 2022" -A x64
   ```

3. **Build:**
   ```powershell
   cmake --build . --config Release
   ```

4. **Install (optional):**
   ```powershell
   cmake --install . --config Release
   ```

5. **Output files:**
   - Plugin: `build/plugin/Release/obs-sei-stamper.dll`
   - Or use `out/obs-studio/` directory structure for easy installation


---

## Troubleshooting

### Issue: Encoder not appearing in OBS

**Solution:** 
- Verify plugin DLL is in `obs-plugins/64bit/` directory
- Check OBS logs for loading errors
- Ensure OBS version is 28.0+

### Issue: Receiver cannot connect to SRT

**Solution:**
- Verify `srt.dll` is installed
- Check firewall settings
- Confirm SRT URL format: `srt://ip:port`

### Issue: SEI data not found

**Solution:**
- Ensure NTP server is accessible
- Verify "Enable NTP Sync" is checked
- Check OBS logs for NTP sync status

---

## Performance

- **CPU Overhead**: < 1% (SEI insertion)
- **NTP Sync Frequency**: Every 60 seconds
- **Frame Accuracy**: ±1 frame at 60fps
- **Latency**: ~100ms (SRT with 120ms latency setting)

---

## License

GPL-2.0 License - following OBS Studio licensing

---

## Credits

- **OBS Studio**: https://obsproject.com
- **libsrt**: https://github.com/Haivision/srt
- **FFmpeg**: https://ffmpeg.org
- **NTP Protocol**: RFC 5905

---

## Support

- **Issues**: [GitHub Issues](https://github.com/yourusername/obs-sei-stamper/issues)
- **Documentation**: [Wiki](https://github.com/yourusername/obs-sei-stamper/wiki)

---

## Contributing

Contributions are welcome! Please feel free to submit a Pull Request.

### Development Guidelines

1. Follow the existing code style
2. Add comments for complex logic
3. Test your changes thoroughly
4. Update documentation as needed

---

## License

GPL-2.0 License - following OBS Studio licensing

See [LICENSE](LICENSE) file for details.

---

## Credits

- **OBS Studio**: https://obsproject.com
- **libsrt**: https://github.com/Haivision/srt
- **FFmpeg**: https://ffmpeg.org
- **NTP Protocol**: RFC 5905

---

## Support

- **Issues**: [GitHub Issues](https://github.com/yourusername/obs-sei-stamper/issues)
- **Discussions**: [GitHub Discussions](https://github.com/yourusername/obs-sei-stamper/discussions)
- **Documentation**: [Wiki](https://github.com/yourusername/obs-sei-stamper/wiki)

---

**Version**: 1.0.0  
**Last Updated**: 2026-01-04

