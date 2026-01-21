# OBS SEI Stamper 插件

<img src="pic\sei_stamper_gau.png" alt="isolated" width="250"/>

**OBS Studio 帧级别视频同步**

[English](README.md) | [中文](#中文) | [日本語](README.jpn.md)

---

## 中文

### 概述

OBS SEI Stamper 是一个 OBS Studio 插件，通过在视频流中嵌入 NTP 时间戳的 SEI（补充增强信息）来实现**帧级别的视频同步**。

**主要特性：**
- 🎯 **帧精确同步** - 使用 NTP 时间戳
- 📡 **多种硬件编码器** - Intel QuickSync、NVIDIA NVENC、AMD AMF
- 🎞️ **多编码格式支持** - 支持 **H.264**、**H.265 (HEVC)** 和 **AV1**
- 🚀 **GPU 加速** - 支持 SEI 的硬件加速编码
- 🔄 **完整方案** - 包含发送端和接收端
- 🌐 **SRT 流媒体** - 内置 SRT 接收器,低延迟传输
- ⏱️ **微秒精度** - 基于 NTP 的专业级时间同步

### 应用场景

- 多机位直播同步
- 远程演播室帧级同步
- 广播级多路信号对齐
- 演唱会/活动多角度录制

### 演示视频

📺 **[在 YouTube 上观看演示视频](https://youtu.be/9aJCHxzy-ME)** *(中文讲解)*

演示使用OBS输出4路相同设置的SRT视频，局域网接收这4路SRT。原生的OBS媒体源无法做到同步，对比本插件，可以做到大概±1帧的同步。

---

## 安装

### 快速安装（推荐）

从 [Releases](https://github.com/ikenainanodesu/obs-sei-stamper/releases) 页面下载最新版本。

发布包包含：
- `obs-sei-stamper.dll` - 主插件
- `srt.dll` - 接收端功能所需的 SRT 库
- 多语言支持的本地化文件

### 系统要求

- OBS Studio 28.0 或更高版本
- Windows 10/11 (64位)

### 手动安装步骤

1. **从 [Releases](https://github.com/ikenainanodesu/obs-sei-stamper/releases) 页面下载发布包**

2. **复制到 OBS 插件目录：**
   ```powershell
   # 复制插件 DLL
   Copy-Item obs-sei-stamper.dll "C:\Program Files\obs-studio\obs-plugins\64bit\"
   
   # 复制 SRT 库
   Copy-Item srt.dll "C:\Program Files\obs-studio\obs-plugins\64bit\"
   ```

3. **复制本地化文件：**
   ```powershell
   # 创建目录
   New-Item -ItemType Directory -Force `
       "C:\Program Files\obs-studio\data\obs-plugins\obs-sei-stamper\locale"
   
   # 复制语言文件
   Copy-Item data\locale\* `
       "C:\Program Files\obs-studio\data\obs-plugins\obs-sei-stamper\locale\" -Recurse
   ```

4. **重启 OBS Studio**

---

## 使用方法

### 发送端（编码器）

1. 打开 **设置 → 输出 → 输出模式：高级**
2. 根据需要的编码格式选择 SEI Stamper 编码器：
   - **SEI Stamper (H.264)** - 兼容性最好
   - **SEI Stamper (H.265)** - 高压缩率 (HEVC)，相比 H.264 节省 30-50% 带宽（**推荐**）
   - **SEI Stamper (AV1)** - 下一代高效率编码 (*注意: SRT 流媒体支持取决于 OBS 版本*)
3. 在编码器设置中选择您的 **硬件编码器 (Hardware Encoder)**：
   - Intel QuickSync
   - NVIDIA NVENC
   - AMD AMF
4. 配置编码器属性：
   - **NTP 服务器**：`time.windows.com`（或您的 NTP 服务器）
   - **NTP 端口**：`123`（默认）
   - **启用 NTP 同步**：✓
5. 开始推流/录制

> **⚠️ 关于 AV1 的注意**: 虽然插件支持 AV1 编码，但当前版本的 OBS Studio 可能不支持通过 SRT 输出 AV1 流。H.264 和 H.265 经充分验证可用于 SRT 流媒体传输。

### 接收端（源）

1. 在 OBS 场景中，点击 **添加源 +**
2. 选择 **SEI Receiver（SEI 接收器）**
3. 配置源：
   - **SRT URL**：`srt://发送端IP:端口`（例如：`srt://192.168.1.100:9000`）
   - **启用 NTP 同步**：✓
   - **NTP 服务器**：与发送端相同
4. 点击 **确定**

**注意**：接收端会自动检测流的编码格式（H.264/H.265/AV1）。无需手动选择。

---

## 验证

### 使用 FFprobe 检查 SEI 数据

```powershell
# 查看帧信息
ffprobe -select_streams v:0 -show_frames output.mp4 2>&1 | Select-String "SEI"

# 详细帧数据
ffprobe -select_streams v:0 -show_frames -show_entries frame=pict_type output.mp4
```

### 使用 MediaInfo 检查

```powershell
MediaInfo --Full output.mp4 | Select-String "SEI"
```

---

## 技术细节

### 架构图

```
┌─────────────┐         ┌──────────────┐         ┌─────────────┐
│   发送端    │         │  SRT 流      │         │   接收端    │
│  (编码器)   │───────▶│  + SEI数据   │───────▶│   (源)      │
└─────────────┘         └──────────────┘         └─────────────┘
      │                                                  │
      ▼                                                  ▼
┌─────────────┐                                  ┌─────────────┐
│  NTP客户端  │◀────────────────────────────────▶│  NTP客户端  │
└─────────────┘         NTP服务器                └─────────────┘
```

### SEI 格式

- **UUID**：自定义标识符（`a5b3c2d1-e4f5-6789-abcd-ef0123456789`）
- **负载类型**：User Data Unregistered（类型 5）
- **数据结构**：
  - UUID（16 字节）
  - PTS（8 字节）
  - NTP 时间戳（8 字节：4 字节秒 + 4 字节小数）

### NTP 同步策略

#### 编码器（发送端）
- **同步间隔**: 每 60 秒
- **触发条件**: 编码过程中自动定期同步
- **目的**: 确保编码器的 NTP 时间保持准确

#### 接收器（源）
- **智能同步**: 使用两个触发条件的自适应同步
  1. **关键帧触发**: 带 SEI 的关键帧（IDR）时同步（**最小10秒间隔**）
  2. **漂移检测**: 当时间漂移超过配置阈值时（默认50ms）
- **目的**: 在最小化网络开销的同时保持高精度
- **性能**: 最小同步间隔防止过多 NTP 请求

### NTP 配置推荐

#### 编码器设置

| 参数 | 范围 | 默认值 | 推荐值 |
|------|------|--------|--------|
| NTP 同步间隔 | 1-600秒 | 60秒 | **30-120秒** |

#### 接收器设置

| 参数 | 范围 | 默认值 | 推荐值 |
|------|------|--------|--------|
| NTP 漂移阈值 | 10-1000ms | 50ms | **30-100ms** |
| NTP 同步间隔 | 100ms-1h | 10s | **10-60秒** |

> **ℹ️ 注意**: 同步间隔现在完全可配置。将应用您设置的值（默认10秒）作为最小间隔以防止性能问题。如果您的网络和 CPU 能够处理频繁的 NTP 请求，您可以降低此值。

### 支持的编码器

| 编码器名称 | 编码格式 | 支持硬件 | 状态 |
|----------|---------|---------|------|
| SEI Stamper (H.264) | H.264/AVC | Intel, NVIDIA, AMD | ✅ 已验证 |
| SEI Stamper (H.265) | H.265/HEVC | Intel, NVIDIA, AMD | ✅ 已验证 (推荐)|
| SEI Stamper (AV1) | AV1 | Intel, NVIDIA, AMD | ⚠️ (OBS SRT限制)|

---

## 从源码编译

### 编译要求

- **CMake** 3.20 或更高版本
- **Visual Studio 2022**（带 C++ 桌面开发工作负载）
- **OBS Studio 源代码**（作为依赖项包含）
- **FFmpeg 库**（由 OBS 提供）
- **libsrt**（包含在仓库中）

### 快速编译（推荐）

为了方便无编译基础的用户，使用自动编译脚本：

1. **运行编译脚本：**
   ```powershell
   # 进入项目目录
   cd obs-sei-stamper
   
   # 运行自动编译脚本
   .\build_and_install.bat
   ```

2. **获取插件：**
   - 编译成功后，插件文件会生成在 `out/obs-studio/` 目录下
   - 插件结构镜像 OBS 安装目录

3. **安装：**
   - 将 `out/obs-studio/` 的内容复制到您的 OBS 安装目录
   - 默认位置：`C:\Program Files\obs-studio`
   - **需要管理员权限**

### 手动编译步骤

1. **克隆仓库：**
   ```bash
   git clone https://github.com/ikenainanodesu/obs-sei-stamper.git
   cd obs-sei-stamper
   ```

2. **配置 CMake：**
   ```powershell
   mkdir build
   cd build
   cmake .. -G "Visual Studio 17 2022" -A x64
   ```

3. **编译：**
   ```powershell
   cmake --build . --config Release
   ```

4. **安装（可选）：**
   ```powershell
   cmake --install . --config Release
   ```

5. **输出文件：**
   - 插件：`build/plugin/Release/obs-sei-stamper.dll`
   - 或使用 `out/obs-studio/` 目录结构方便安装

---

## 故障排除

### 问题：OBS 中找不到编码器

**解决方案：**
- 确认插件 DLL 在 `obs-plugins/64bit/` 目录
- 检查 OBS 日志查看加载错误
- 确保 OBS 版本为 28.0+

### 问题：接收端无法连接 SRT

**解决方案：**
- 确认已安装 `srt.dll`
- 检查防火墙设置
- 确认 SRT URL 格式：`srt://ip:端口`

### 问题：找不到 SEI 数据

**解决方案：**
- 确保 NTP 服务器可访问
- 确认已勾选"启用 NTP 同步"
- 检查 OBS 日志查看 NTP 同步状态

---

## 性能指标

- **CPU 开销**：< 1%（SEI 插入）
- **NTP 同步频率**：每 60 秒
- **帧精度**：60fps 时 ±1 帧
- **延迟**：~100ms（SRT 120ms 延迟设置）

---

## 贡献

欢迎贡献！请随时提交 Pull Request。

### 开发指南

1. 遵循现有的代码风格
2. 为复杂逻辑添加注释
3. 彻底测试您的更改
4. 根据需要更新文档

---

## 许可证

GPL-2.0 License - 遵循 OBS Studio 许可

详情请参阅 [LICENSE](LICENSE) 文件。

---

## 致谢

- **OBS Studio**：https://obsproject.com
- **libsrt**：https://github.com/Haivision/srt
- **FFmpeg**：https://ffmpeg.org
- **NTP 协议**：RFC 5905

---

## 版本更新记录

### v1.2.0 (2026-01-22)

**🎉 新功能:**
- ✨ **多编码格式支持**: 全面支持 **H.265 (HEVC)** 和 **AV1** 编码格式
  - **H.265**: 相比 H.264 节省 30-50% 带宽
  - **AV1**: 下一代高效压缩
- 🛠️ **三个独立编码器**:
  - `SEI STAMPER (H.264)`
  - `SEI STAMPER (H.265)`
  - `SEI STAMPER (AV1)`
  - 均支持硬件加速 (Intel/NVIDIA/AMD)
- 🧠 **智能接收器**: 自动检测流的编码格式，移除了手动选择“Codec Format”的步骤

**⚠️ 重要限制**:
- **H.264** 和 **H.265** 已完全验证支持 SRT 流媒体。
- **AV1** 编码功能可用，但 OBS Studio 的 SRT 输出可能不支持 AV1（取决于 OBS 版本）。

### v1.1.3 (2026-01-04)

**✨ 新功能:**
- ⚙️ **自定义接收器同步间隔**: 为接收器添加 `NTP Sync Interval` 设置

### v1.1.2 (2026-01-04)

**🔧 Bug 修复:**
- 🐛 **修复接收器卡顿**: 为关键帧触发的 NTP 同步添加最小10秒间隔
- 🛡️ **网络稳健性**: 添加智能退避（Backoff）机制

### v1.1.0 (2026-01-04)

**🎉 新增功能：**
- ✨ **NVIDIA NVENC 支持**
- ✨ **AMD AMF 支持**
- 🚀 **多 GPU 支持**

### v1.0.0 (2026-01-04)

**首次发布**

---

**当前版本**：1.2.0  
**最后更新**：2026-01-22
