# SEI Stamper 插件

<img src="pic/sei_stamper_gau.png" alt="SEI Stamper" width="250"/>

通过 NTP 时间戳与自定义 SEI 辅助 OBS Studio 多路视频时间对齐。

[English](README.md) | [中文](README.chs.md) | [日本語](README.jpn.md)

## 当前项目状态

源码版本为 **1.3.0**。本文依据当前代码和构建脚本整理；源码版本号不代表 Releases 中已存在对应安装包。（来源：[CMakeLists.txt](CMakeLists.txt)、[模块注册](src/sei-stamper-plugin.c)）

| 功能 | 当前实现与限制 |
| --- | --- |
| H.264 / H.265 | 提供 Intel QuickSync、NVIDIA NVENC、AMD AMF 后端，在关键帧中插入 NTP SEI |
| ~~AV1~~ | **目前同步不可用。** 仍注册编码器及接收端选项，但时间戳插入沿用 H.264/H.265 NAL 路径，尚未实现 AV1 专用元数据封装；不列为可用的时间戳同步方案 |
| 接收端 | 通过 FFmpeg 打开 SRT 流并解码视频和可用音轨；默认自动检测编码格式，也可手动覆盖 |
| NTP | 发送端后台同步并读取缓存时钟；接收端仍在初始化和接收处理路径中执行同步请求 |
| 验证范围 | Windows x64 Release 编译通过；60 项独立测试通过；在官方 OBS 32.2.2 运行库下通过 DLL 依赖加载、API 版本及 SRT 协议查询；未实测 OBS 插件初始化、GPU 编码或多路 SRT |

表中实现依据：[统一编码器](src/unified-encoder.c)、[QSV](src/qsv-encoder.c)、[NVENC](src/nvenc-encoder.c)、[AMF](src/amd-encoder.c)、[接收端](src/sei-receiver-source.c)、[测试](tests/test_ntp_sei.c)。

H.265 经 SLS 转发的稳定性仍沿用项目已有的“支持有限”状态，本轮未重新验证。AMF 的 FFmpeg 8 兼容性修复已在代码中，但缺少本轮 AMD 实机验证。同步误差、端到端延迟和 CPU 开销应在实际设备、网络及输出配置下测量，本文不作固定精度或性能保证。

[项目演示视频](https://youtu.be/JhizRlUpSlg)保留作为历史演示入口，不作为当前版本的基准测试。

## 安装

当前构建配置面向 **Windows x64 / OBS Studio 32.2 系列 / FFmpeg 8**，CMake 会拒绝 libavcodec 主版本低于 62 的依赖包。安装时须匹配构建所用的 OBS/FFmpeg 运行库；不能仅凭 OBS 版本号更高就推定兼容。（来源：[CMakeLists.txt](CMakeLists.txt)）

1. 退出 OBS，从 [Releases](https://github.com/ikenainanodesu/SEI-Stamper/releases) 选择匹配环境的包，或按下文自行构建。
2. 将插件与本地化文件放入 OBS 安装目录；默认位置为 `C:\Program Files\obs-studio`，写入该目录通常需要管理员权限。
3. 若包中附带匹配的 SRT 运行库，按包的目录结构一并复制。自带构建脚本仅在找到 `srt.dll` 时复制它，不能假设每个构建包都有此文件。
4. 重启 OBS，检查日志是否出现 `SEI Stamper Plugin loaded`，以及编码器、SEI 接收器是否可选。

安装目录结构：

```text
obs-studio/
├── obs-plugins/64bit/
│   ├── sei-stamper.dll
│   └── srt.dll                 # 如构建/发布包提供且运行时需要
└── data/obs-plugins/sei-stamper/locale/
    ├── en-US.ini
    └── zh-CN.ini
```

仓库提供英文、简体中文界面资源；日文 README 不代表附带日文界面翻译。SRT 接收依赖实际使用的 FFmpeg 运行库支持 SRT，仅复制一个 DLL 不足以保证接收可用。（来源：[安装脚本](build_and_install.bat)、[本地化资源](data/locale)、[接收端连接实现](src/sei-receiver-source.c)）

## 使用方法

### 发送端

1. 在 OBS 打开“设置 → 输出”，切换到“高级”输出模式。
2. 选择 `SEI STAMPER (H.264)` 或 `SEI STAMPER (H.265)`。
3. 在 `Hardware Encoder` 中选择设备支持的 Intel QuickSync、NVIDIA NVENC 或 AMD AMF。默认选中 Intel，并不会自动选择本机可用的 GPU。
4. 设置码率、关键帧间隔、NTP 服务器、端口和同步间隔。用于时间对齐的各发送端与接收端应配置一致的 NTP 时间源。
5. 在 OBS 的输出设置中配置推流地址或录制目标，然后启动输出。编码器本身不创建 SRT 监听服务。

默认码率为 **2500 kbps**，关键帧间隔为 **2 秒**，B 帧为 **0**；发送端 NTP 默认服务器为 `pool.ntp.org`，端口 **123**，同步间隔 **60000 ms**。（来源：[统一编码器默认值及属性](src/unified-encoder.c)）

当前三个硬件后端均直接启用 NTP，没有读取统一界面的 `ntp_enabled` 开关；不要依赖取消“Enable NTP Sync”来禁用发送端 NTP。首次同步成功前不会插入有效的 NTP SEI；成功后只在关键帧上读取时钟并插入时间戳。（来源：[QSV](src/qsv-encoder.c)、[NVENC](src/nvenc-encoder.c)、[AMF](src/amd-encoder.c)）

### 接收端

1. 在场景中添加“SEI 接收器 / SEI Receiver”。
2. 填写与发送端或转发服务器对应的 SRT URL，确保两端的连接模式、地址和端口相匹配。代码默认值为 `srt://127.0.0.1:9000`，跨机器接收时需要改为实际地址。
3. “编码格式”默认保留“自动检测”。只有需要覆盖探测结果时才手动选择 H.264 或 H.265，手动选择须与发送端一致。
4. 启用 NTP 同步，并将服务器改为与发送端一致。接收端默认 `time.windows.com`，与发送端默认值不同。
5. 硬件解码默认关闭；可选 QSV、NVDEC、AMF，实际可用性取决于设备、驱动和 FFmpeg 构建。

SRT URL 按原样传递给 FFmpeg，包括 URL 中的查询参数。接收端会尝试解码流中可用的音轨。（来源：[接收端默认值、属性及连接逻辑](src/sei-receiver-source.c)）

### NTP 参数与同步行为

| 参数 | 发送端 | 接收端 |
| --- | --- | --- |
| 默认服务器 | `pool.ntp.org` | `time.windows.com` |
| 端口默认值 / 界面范围 | 123 / 1–65535 | 123 / 1–65535 |
| 同步间隔默认值 | 60000 ms | 10000 ms |
| 同步间隔界面范围 | 1000–300000 ms | 100–3600000 ms |
| 漂移阈值默认值 / 界面范围 | 无此选项 | 50 ms / 10–1000 ms |

发送端后台线程启动后立即尝试同步，此后在每次请求结束后等待配置间隔。接收端收到携带 NTP 数据的帧时，关键帧或时间差超过阈值可触发请求，两种触发都受配置的最小间隔约束，10 秒是默认值而非固定下限。接收端请求仍可能阻塞接收处理；单个地址的接收超时为 5 秒，多地址尝试可能耗时更长。（来源：[NTP 客户端](src/ntp-client.c)、[编码器属性](src/unified-encoder.c)、[接收端](src/sei-receiver-source.c)）

## 从源码构建

需要 CMake 3.20+、带 C++ 桌面开发工作负载的 Visual Studio 2022，以及已经完成 Release 构建的 OBS 源码树和匹配的依赖包。仓库**不附带** OBS 或 SRT 源码/二进制依赖；`obs/`、`obs-studio-master/` 和 `srt/` 均列在忽略规则中。（来源：[CMakeLists.txt](CMakeLists.txt)、[.gitignore](.gitignore)）

| 配置项 | 含义 / 默认值 |
| --- | --- |
| `OBS_SOURCE_DIR` | 必填，OBS 源码根目录 |
| `OBS_BUILD_DIR` | 默认为 `OBS_SOURCE_DIR/build` |
| `OBS_DEPS_DIR` | 默认在 `OBS_SOURCE_DIR/.deps` 中匹配 `obs-deps-20??-??-??-x64` / `windows-deps-20??-??-??-x64`，按名称排序后选择末项；多个包并存时建议显式指定 |
| `CMAKE_GENERATOR` | 仅批处理脚本读取，默认 `Visual Studio 17 2022` |

前三个变量均支持 CMake `-D` 参数或同名环境变量。当前构建脚本使用 `libobs/Release/obs.lib` 等固定 Release 子目录；指定 `OBS_BUILD_DIR` 时需匹配该布局。依赖包检查以实际 `LIBAVCODEC_VERSION_MAJOR >= 62` 为准，名称或日期本身不是兼容性证明。（来源：[CMakeLists.txt](CMakeLists.txt)）

从项目根目录执行，路径替换为本机实际值：

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
  -DOBS_SOURCE_DIR="C:/obs-studio" `
  -DOBS_BUILD_DIR="C:/obs-studio/build" `
  -DOBS_DEPS_DIR="C:/obs-studio/.deps/windows-deps-2026-08-26-x64"
cmake --build build --config Release
cmake --install build --config Release --prefix out/obs-studio
```

插件输出为 `build/plugin/Release/sei-stamper.dll`，可安装目录为 `out/obs-studio/`。`cmake --install` 只安装插件和 `data/`，不会自动收集 SRT 或其他运行库。

也可从项目根目录运行：

```powershell
$env:OBS_SOURCE_DIR = "C:\obs-studio"
$env:OBS_DEPS_DIR = "C:\obs-studio\.deps\windows-deps-2026-08-26-x64"
.\build_and_install.bat
```

该脚本会**删除并重建项目根目录的 `build/`**，完成构建后暂存到 `out/obs-studio/`，不会直接安装到系统 OBS 目录。它会尝试从本地 SRT 构建或 OBS 运行目录复制 `srt.dll`。（来源：[build_and_install.bat](build_and_install.bat)）

## 验证与排障

从项目根目录运行独立测试：

```powershell
.\tests\run_tests.bat
```

2026-09-05 本地执行结果为 **60 项通过、0 项失败**。测试覆盖 NTP 时间转换和同步锚点、SEI 负载布局、NAL 扫描及 extradata 转换。测试文件内复制了被测纯函数，并未直接链接生产源码；它不覆盖 NTP 网络请求、线程、OBS 加载、GPU 编解码或多流同步。（来源：[测试脚本](tests/run_tests.bat)、[测试实现](tests/test_ntp_sei.c)）

同日使用 MSVC 19.50 / CMake 4.2.1 / Ninja 完成 1.3.0 Windows x64 Release 构建，并在官方 OBS 32.2.2 便携运行库下验证 DLL 加载、`obs_module_ver()` 和 SRT 协议查询。此发布包使用官方 32.2.2 头文件，并从同版本 DLL 导出表生成 OBS/pthread 导入库。未启动 OBS 插件初始化或执行 GPU/推流测试。（来源：[v1.3.0 发布附件内的构建记录](https://github.com/ikenainanodesu/SEI-Stamper/releases/tag/v1.3.0)）

实际部署时检查 OBS 日志：

- **插件不显示**：检查 DLL/本地化目录、OBS 与 FFmpeg 运行库是否匹配。
- **编码器创建失败**：核对所选硬件、编码格式与驱动，查看 QSV/NVENC/AMF 初始化错误。
- **SRT 连接失败**：核对 URL、两端连接模式、监听端状态、防火墙及 FFmpeg 的 SRT 支持。
- **无 NTP SEI**：查找 `NTP sync successful`；首次同步完成后等待下一个关键帧。请求失败时检查 DNS、UDP 端口和日志中的 socket 错误。
- **接收卡顿或同步不稳**：核对双方时间源、接收端同步间隔及网络请求耗时；用同画面帧计数或时间码实测多流误差。

可用 `ffprobe -select_streams v:0 -show_frames output.mp4` 辅助检查帧信息，但出现一般 SEI 标记并不能证明本插件 UUID/时间戳正确，也不能证明多流已同步。自定义负载须按下述结构核对。（来源：[SEI 构造与解析](src/sei-handler.c)）

## SEI 数据格式

自定义负载为 **32 字节**，数值字段按大端序写入；NAL 头、负载类型/长度、转义字节及结束位不计入该长度。

| 字段 | 长度 |
| --- | --- |
| UUID `a5b3c2d1-e4f5-6789-abcd-ef0123456789` | 16 字节 |
| 帧 PTS（int64） | 8 字节 |
| NTP 秒（uint32） | 4 字节 |
| NTP 小数（uint32） | 4 字节 |

使用 User Data Unregistered（负载类型 5）；H.264 使用 NAL 类型 6，H.265 使用 Prefix SEI 类型 39。该封装不适用于 AV1。（来源：[sei-handler.h](src/sei-handler.h)、[sei-handler.c](src/sei-handler.c)、[硬件插入路径](src/nvenc-encoder.c)）

---

## 免责声明

本项目的部分代码和文档是在 AI 工具的辅助下生成的。使用本软件即表示您承认并同意：
1. 本软件按"原样"提供，没有任何形式的保证。
2. 作者和贡献者不对因使用本插件而产生的任何损坏或数据丢失承担责任。

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

### v1.3.0 (2026-09-04)

- 将 Windows NTP 接收超时修正为 5000 ms，依次尝试解析出的地址，并记录接收错误。
- 校验服务器模式、闰秒指示、stratum 及请求时间戳回显。请求与响应配对不等于加密认证。
- 使用偏移修正后的接收时间（T4 + offset）作为时钟锚点，日志记录时钟偏移与往返延迟。
- 发送端 NTP 请求移至后台线程，统一读取 `ntp_sync_interval_ms` 并支持配置端口，只在关键帧读取与插入时间戳。
- NVENC/AMF 在关键帧缺少 SPS 时重新插入可用的 SPS/PPS(/VPS)；H.264 AVCC 回退转换拒绝 HEVC HVCC，不能据此保证所有 HEVC 流的头信息都可恢复。
- 调整 AMF 全局头处理，并将 `fast` 预设映射到 `speed`；AMD 实机验证仍待完成。
- 接收端改用 FFmpeg 8 的 `AV_FRAME_FLAG_KEY`；构建支持配置 OBS SDK 路径，并检查 libavcodec 主版本不低于 62。
- 新增独立纯函数测试，当前执行结果和覆盖限制见上文。

来源：[NTP 客户端](src/ntp-client.c)、[NVENC](src/nvenc-encoder.c)、[AMF](src/amd-encoder.c)、[QSV](src/qsv-encoder.c)、[接收端](src/sei-receiver-source.c)、[CMake](CMakeLists.txt)、[测试](tests/test_ntp_sei.c)。

以下保留历史发布记录，其中的兼容性及验证描述不代表当前版本状态。

### v1.2.3-beta (2026-06-07)

**🔧 修复与验证:**
- ✅ **修复 NVENC/AMF 视频帧传输问题**: 解决 NVIDIA NVENC 与 AMD AMF 的 H.264/H.265 编码器在推流输出中可能无法传输视频帧的问题。
- 🛠️ **NVENC 兼容性改进**: 将 NVENC preset 规整到现代 `p1`-`p7` 系列，修正不同编码格式下的 profile 处理，恢复 RTMP/录制所需的容器头，并在 MPEG-TS/SRT 关键帧中继续保留 SPS/PPS/VPS 带内参数集。
- 🛠️ **AMF 兼容性改进**: 增加按编码格式处理 AMF profile 的逻辑，避免 H.265 继承 `high` 等无效的 H.264 profile 名称。
- ✅ **构建验证**: 已确认可使用 OBS Studio **32.1.2** 依赖完成本地 Release 构建。

### v1.2.2 (2026-03-26)

**✨ 更新与文档:**
- 🛠️ **编码器重构**: 针对 H.265 (HEVC) 编码器进行了底层代码重构，取消了全局头标志以强制带内传输参数集，显著提升了在 MPEG-TS 容器及部分流媒体服务器（如 SLS）下的兼容性。
- 📝 **文档更新**: 移除所有关于 AV1 的描述。修改全部关于 H.265 的描述，标明其目前对 SLS 服务器的有限支持及不稳定性，正在努力寻找改善方案中。
- 🔧 **代码维护**: 修复 C 源码中 `pthread` 等相关函数的未声明/编译告警问题。

### v1.2.1 (2026-03-24)

**✨ 新功能与改进:**
- 🏷️ **接收器版本显示**: 在 SEI 接收器属性 UI 界面的右下角（底部）添加了插件版本号标签，方便用户识别。

**🔧 构建与兼容性修复:**
- ⚙️ **动态 CMake 依赖解析**: 修改了 `CMakeLists.txt` 使其能自动匹配最新下载的 `obs-deps-*` 编译依赖文件夹，彻底解决了后续每次随 OBS 大版本更新带来的硬编码路径爆红问题。
- ✅ 验证通过了在 OBS Studio **32.1.0** 版本编译系统及 API 上的完美兼容性。

### v1.2.0 (2026-01-22)

**🎉 新功能:**
- ✨ **多编码格式支持**: 全面支持 **H.265 (HEVC)** 和 ~~**AV1**~~ 编码格式（AV1 目前同步不可用）
  - **H.265**: 相比 H.264 节省 30-50% 带宽
  - ~~**AV1**: 下一代高效压缩~~
- 🛠️ **三个独立编码器**:
  - `SEI STAMPER (H.264)`
  - `SEI STAMPER (H.265)`
  - ~~`SEI STAMPER (AV1)`~~（目前同步不可用）
  - 均支持硬件加速 (Intel/NVIDIA/AMD)
- 🧠 **接收器配置**: 接收端需要手动选择匹配的编码格式以确保正确解码。

**⚠️ 重要限制**:

- **H.264** 和 **H.265** 已完全验证支持 SRT 流媒体。
- ~~**AV1** 编码功能可用，但 OBS Studio 的 SRT 输出可能不支持 AV1（取决于 OBS 版本）。~~ **当前状态：AV1 同步不可用。**

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

**当前版本**：1.3.0
**最后更新**：2026-09-05
