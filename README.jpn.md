# SEI Stamper プラグイン

<img src="pic/sei_stamper_gau.png" alt="SEI Stamper" width="250"/>

NTPタイムスタンプと独自SEIによる、OBS Studioの複数映像ストリームの時刻合わせ。

[English](README.md) | [中文](README.chs.md) | [日本語](README.jpn.md)

## 現在のプロジェクト状態

ソースのバージョンは **1.3.0** です。本書は現在のコードとビルドスクリプトを説明します。ソースのバージョン番号だけでは、対応するリリースパッケージの公開は確認できません。（出典：[CMakeLists.txt](CMakeLists.txt)、[モジュール登録](src/sei-stamper-plugin.c)）

| 機能 | 現在の実装と制限 |
| --- | --- |
| H.264 / H.265 | Intel QuickSync、NVIDIA NVENC、AMD AMFのバックエンドでキーフレームにNTP SEIを挿入 |
| ~~AV1~~ | **現在、同期は利用できません。** エンコーダと受信側の選択肢は残っていますが、タイムスタンプ挿入はH.264/H.265 NAL方式のままです。AV1専用メタデータ実装がないため、時刻同期の対応形式には含めません |
| 受信側 | FFmpeg経由でSRTを開き、映像と利用可能な音声トラックをデコード。コーデックは自動検出が既定で、手動指定も可能 |
| NTP | 送信側はバックグラウンド同期。受信側は初期化時と受信処理中に同期リクエストを実行 |
| 検証範囲 | Windows x64 Releaseビルドと独立テスト60項目が成功。公式OBS 32.2.2ランタイムでDLL依存関係のロード・APIバージョン・SRTプロトコル照会を確認。OBSプラグイン初期化、GPUエンコード、複数SRTストリームの実測は未実施 |

実装の出典：[統合エンコーダ](src/unified-encoder.c)、[QSV](src/qsv-encoder.c)、[NVENC](src/nvenc-encoder.c)、[AMF](src/amd-encoder.c)、[受信側](src/sei-receiver-source.c)、[テスト](tests/test_ntp_sei.c)。

H.265のSLS経由での利用は、従来の「限定的な対応」という状態を維持しており、今回は再検証していません。AMFにはFFmpeg 8向け修正がありますが、今回AMD実機では検証していません。同期誤差、総遅延、CPU負荷は実際の機器・ネットワーク・出力設定で測定してください。固定の精度や性能は保証しません。

[プロジェクトのデモ動画](https://youtu.be/JhizRlUpSlg)は過去のデモとして掲載しており、現行版のベンチマークではありません。

## インストール

現在のビルド設定は **Windows x64 / OBS Studio 32.2系 / FFmpeg 8** 向けで、libavcodecのメジャーバージョンが62未満の依存パッケージを拒否します。ビルド時と実行時のOBS・FFmpegライブラリを合わせる必要があり、OBSのバージョン番号が新しいだけでは互換性を判断できません。（出典：[CMakeLists.txt](CMakeLists.txt)）

1. OBSを終了し、[Releases](https://github.com/ikenainanodesu/SEI-Stamper/releases)から環境に合うパッケージを選ぶか、下記の手順でビルドします。
2. プラグインとロケールをOBSのインストール先にコピーします。既定の `C:\Program Files\obs-studio` への書き込みには通常管理者権限が必要です。
3. 対応するSRTランタイムが同梱されていれば、パッケージの構成に従ってコピーします。ビルドスクリプトは発見した場合のみ `srt.dll` をコピーします。
4. OBSを再起動し、ログの `SEI Stamper Plugin loaded` とエンコーダ・受信ソースの登録を確認します。

```text
obs-studio/
├── obs-plugins/64bit/
│   ├── sei-stamper.dll
│   └── srt.dll                 # 同梱され、実行環境で必要な場合
└── data/obs-plugins/sei-stamper/locale/
    ├── en-US.ini
    └── zh-CN.ini
```

UIの言語リソースは英語と簡体字中国語です。日本語READMEがあっても日本語UIは同梱されていません。SRT受信には使用するFFmpegランタイムのSRT対応が必要で、DLLを1個コピーするだけで対応が保証されるわけではありません。（出典：[ビルドスクリプト](build_and_install.bat)、[ロケール](data/locale)、[受信側](src/sei-receiver-source.c)）

## 使用方法

### 送信側

1. OBSの「設定 → 出力」で出力モードを「詳細」にします。
2. `SEI STAMPER (H.264)` または `SEI STAMPER (H.265)` を選びます。
3. `Hardware Encoder` で対応するIntel QuickSync、NVIDIA NVENC、AMD AMFを選びます。既定はIntelで、自動的なGPU選択ではありません。
4. ビットレート、キーフレーム間隔、NTPサーバー、ポート、同期間隔を設定します。時刻合わせに使う各送信側・受信側でNTP時刻源をそろえます。
5. OBSの出力設定で配信URLまたは録画先を指定して出力を開始します。エンコーダ自体はSRTリスナーを作成しません。

既定値は **2500 kbps**、キーフレーム間隔 **2秒**、Bフレーム **0**、NTPサーバー `pool.ntp.org`、ポート **123**、同期間隔 **60000 ms** です。（出典：[統合エンコーダ](src/unified-encoder.c)）

現在の3つのハードウェアバックエンドはNTPを直接有効化し、統合UIの `ntp_enabled` を読み取っていません。「Enable NTP Sync」を外しても送信側NTPを無効化できるとは限りません。最初の同期成功前には有効なNTP SEIを挿入せず、成功後はキーフレームでのみ時計を読み取り、タイムスタンプを挿入します。（出典：[QSV](src/qsv-encoder.c)、[NVENC](src/nvenc-encoder.c)、[AMF](src/amd-encoder.c)）

### 受信側

1. シーンに **SEI Receiver** を追加します。
2. 送信機または中継サーバーのSRT URLを入力し、双方の接続モード・アドレス・ポートを合わせます。既定の `srt://127.0.0.1:9000` は他のマシンに接続する場合に変更が必要です。
3. 「Codec Format」は原則「Auto」のまま使用します。手動指定する場合は送信側のH.264/H.265と一致させます。
4. NTPを有効にして送信側と同じ時刻源を指定します。受信側の既定値 `time.windows.com` は送信側と異なります。
5. デコードはソフトウェアが既定です。QSV、NVDEC、AMFの利用可否は機器・ドライバー・FFmpegビルドに依存します。

URLはクエリーパラメーターを含め、そのままFFmpegへ渡されます。利用可能な音声トラックもデコードします。（出典：[受信側の設定・接続処理](src/sei-receiver-source.c)）

### NTP設定と動作

| 設定 | 送信側 | 受信側 |
| --- | --- | --- |
| 既定のサーバー | `pool.ntp.org` | `time.windows.com` |
| ポート既定値 / UI範囲 | 123 / 1–65535 | 123 / 1–65535 |
| 同期間隔の既定値 | 60000 ms | 10000 ms |
| 同期間隔のUI範囲 | 1000–300000 ms | 100–3600000 ms |
| ドリフト閾値の既定値 / UI範囲 | 項目なし | 50 ms / 10–1000 ms |

送信側はバックグラウンドスレッドの開始直後に同期を試み、各リクエスト終了後に設定した間隔だけ待機します。受信側ではNTP付きフレームのキーフレーム判定または時刻差が閾値を超えた場合に同期し、いずれも設定した最小間隔で制限されます。10秒は既定値で、固定の下限ではありません。受信側の同期は受信処理をブロックし得ます。受信タイムアウトはアドレスごとに5秒で、複数アドレスへの試行ではさらに時間がかかる場合があります。（出典：[NTPクライアント](src/ntp-client.c)、[エンコーダ設定](src/unified-encoder.c)、[受信側](src/sei-receiver-source.c)）

## ソースからビルド

CMake 3.20以降、C++デスクトップ開発ワークロード付きVisual Studio 2022、Releaseビルド済みのOBSソースツリーと対応する依存パッケージが必要です。OBS/SRTのソースやバイナリは**同梱されていません**。`obs/`、`obs-studio-master/`、`srt/` はGitの除外対象です。（出典：[CMakeLists.txt](CMakeLists.txt)、[.gitignore](.gitignore)）

| 設定 | 内容 / 既定値 |
| --- | --- |
| `OBS_SOURCE_DIR` | 必須。OBSソースのルート |
| `OBS_BUILD_DIR` | `OBS_SOURCE_DIR/build` |
| `OBS_DEPS_DIR` | `OBS_SOURCE_DIR/.deps` の `obs-deps-20??-??-??-x64` / `windows-deps-20??-??-??-x64` を名前順に並べた末尾の項目。複数ある場合は明示指定を推奨 |
| `CMAKE_GENERATOR` | バッチスクリプトのみ。既定は `Visual Studio 17 2022` |

最初の3項目はCMakeの `-D` または同名の環境変数で設定できます。`libobs/Release/obs.lib` などのRelease配置を前提としています。依存チェックは実際の `LIBAVCODEC_VERSION_MAJOR >= 62` を確認し、パッケージ名や日付だけでは互換性を保証しません。（出典：[CMakeLists.txt](CMakeLists.txt)）

プロジェクトルートで実行します。パスは実際の環境に合わせてください。

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
  -DOBS_SOURCE_DIR="C:/obs-studio" `
  -DOBS_BUILD_DIR="C:/obs-studio/build" `
  -DOBS_DEPS_DIR="C:/obs-studio/.deps/windows-deps-2026-08-26-x64"
cmake --build build --config Release
cmake --install build --config Release --prefix out/obs-studio
```

DLLは `build/plugin/Release/sei-stamper.dll`、インストール用ツリーは `out/obs-studio/` に出力されます。CMakeのインストール対象はプラグインと `data/` のみで、SRT等のランタイムは自動収集しません。

プロジェクトルートからバッチスクリプトを使う方法もあります。

```powershell
$env:OBS_SOURCE_DIR = "C:\obs-studio"
$env:OBS_DEPS_DIR = "C:\obs-studio\.deps\windows-deps-2026-08-26-x64"
.\build_and_install.bat
```

このスクリプトは**プロジェクトの `build/` を削除して作り直します**。成果物を `out/obs-studio/` に配置し、ローカルのSRTビルドまたはOBS実行ディレクトリから `srt.dll` のコピーを試みます。システムのOBSへ直接インストールする処理ではありません。（出典：[build_and_install.bat](build_and_install.bat)）

## 検証とトラブルシューティング

プロジェクトルートで独立テストを実行します。

```powershell
.\tests\run_tests.bat
```

**2026-09-05のローカル実行結果は60項目成功、0項目失敗**でした。NTP変換と同期アンカー、SEI配置、NAL走査、extradata変換を確認します。ただし本テストは純粋関数をテストファイル内に複製しており、本番ソースを直接リンクしません。NTP通信、スレッド、OBSへのロード、GPU処理、複数ストリームの同期は対象外です。（出典：[実行スクリプト](tests/run_tests.bat)、[テスト実装](tests/test_ntp_sei.c)）

同日、MSVC 19.50 / CMake 4.2.1 / Ninjaで1.3.0 Windows x64 Releaseビルドに成功し、公式OBS 32.2.2ポータブルランタイムでDLLロード、`obs_module_ver()`、SRTプロトコル照会を確認しました。公式32.2.2ヘッダーと、同版DLLのエクスポートから生成したOBS/pthreadインポートライブラリを使用しています。OBSプラグイン初期化やGPU・配信テストは未実施です。（出典：[v1.3.0リリース添付のビルド記録](https://github.com/ikenainanodesu/SEI-Stamper/releases/tag/v1.3.0)）

OBSログで以下を確認します。

- **プラグインが表示されない**：DLL・ロケールの配置とOBS/FFmpegランタイムの一致を確認。
- **エンコーダ生成失敗**：選択したGPU・コーデック・ドライバーと各バックエンドの初期化エラーを確認。
- **SRT接続失敗**：URL、接続モード、リスナー、ファイアウォール、FFmpegのSRT対応を確認。
- **NTP SEIがない**：`NTP sync successful` を確認し、次のキーフレームを待つ。失敗時はDNS・UDPポート・socketエラーを確認。
- **カクつき・同期ずれ**：時刻源、受信側の同期間隔、リクエスト所要時間を確認し、フレーム番号やタイムコードで実測。

`ffprobe -select_streams v:0 -show_frames output.mp4` でフレーム情報を補助的に確認できます。一般的なSEI表示だけでは、本プラグインのUUID・時刻の正しさや複数ストリームの同期は証明できません。以下の独自ペイロードを確認してください。（出典：[SEI構築・解析](src/sei-handler.c)）

## SEI形式

独自ペイロードは **32バイト** で、数値はビッグエンディアンです。NALヘッダー、タイプ・長さ、エスケープ、終端ビットは含みません。

| フィールド | サイズ |
| --- | --- |
| UUID `a5b3c2d1-e4f5-6789-abcd-ef0123456789` | 16バイト |
| フレームPTS（int64） | 8バイト |
| NTP秒（uint32） | 4バイト |
| NTP小数（uint32） | 4バイト |

User Data Unregistered（タイプ5）を使用し、H.264はNALタイプ6、H.265はPrefix SEIタイプ39です。AV1用の形式ではありません。（出典：[sei-handler.h](src/sei-handler.h)、[sei-handler.c](src/sei-handler.c)、[挿入処理](src/nvenc-encoder.c)）

---

## 免責事項

このプロジェクトのコードとドキュメントの一部は、AIツールの支援を受けて生成されました。本ソフトウェアを使用することにより、以下の事項に同意したものとみなされます：
1. 本ソフトウェアは「現状のまま」提供され、いかなる種類の保証もありません。
2. 著者および貢献者は、本プラグインの使用に起因するいかなる損害やデータの損失についても責任を負いません。

---

## ライセンス

GPL-2.0 License - OBS Studioのライセンスに準拠

詳細は[LICENSE](LICENSE)ファイルを参照してください。

---

## クレジット

- **OBS Studio**: https://obsproject.com
- **libsrt**: https://github.com/Haivision/srt
- **FFmpeg**: https://ffmpeg.org
- **NTPプロトコル**: RFC 5905

---


## リリースノート

### v1.3.0 (2026-09-04)

- WindowsのNTP受信タイムアウトを5000 msに修正し、解決された各アドレスへの順次試行と受信エラーログを追加。
- サーバーモード、閏秒指示、stratum、要求時刻のエコーを検証。要求と応答の照合は暗号認証ではありません。
- オフセット補正済み受信時刻（T4 + offset）を時計の基準にし、オフセットと往復遅延を記録。
- 送信側NTPをバックグラウンド化し、`ntp_sync_interval_ms` と設定可能なポートを使用。時刻の取得・挿入はキーフレームのみ。
- NVENC/AMFでSPSのないキーフレームに利用可能なSPS/PPS(/VPS)を再挿入。H.264 AVCC変換のフォールバックではHEVC HVCCを拒否するため、全HEVCストリームのヘッダー復元を保証するものではありません。
- AMFのグローバルヘッダー処理と `fast` → `speed` のプリセット変換を修正。AMD実機検証は未完了。
- FFmpeg 8の `AV_FRAME_FLAG_KEY` を使用。OBS SDKパスを設定可能にし、libavcodecメジャーバージョン62以上を要求。
- 独立した純粋関数テストを追加。現在の結果と検証範囲は上記を参照。

出典：[NTP](src/ntp-client.c)、[NVENC](src/nvenc-encoder.c)、[AMF](src/amd-encoder.c)、[QSV](src/qsv-encoder.c)、[受信側](src/sei-receiver-source.c)、[CMake](CMakeLists.txt)、[テスト](tests/test_ntp_sei.c)。

以下は過去のリリース記録です。当時の互換性・検証内容は現行版の状態を示すものではありません。

### v1.2.3-beta (2026-06-07)

**🔧 修正と検証:**
- ✅ **NVENC/AMFのビデオフレーム伝送を修正**: NVIDIA NVENCおよびAMD AMFのH.264/H.265エンコーダで、配信出力にビデオフレームが送られない場合がある問題を解決しました。
- 🛠️ **NVENC互換性の改善**: NVENC presetを現在の`p1`-`p7`系に正規化し、コーデックごとのprofile処理を修正しました。また、RTMP/録画に必要なコンテナヘッダーを復旧し、MPEG-TS/SRTのキーフレームにはSPS/PPS/VPSのインバンドパラメータセットを維持します。
- 🛠️ **AMF互換性の改善**: AMF profileをコーデックごとに処理するようにし、H.265が`high`などの無効なH.264 profile名を引き継がないようにしました。
- ✅ **ビルド検証**: OBS Studio **32.1.2** の依存関係を使用したローカルReleaseビルドを確認済みです。

### v1.2.2 (2026-03-26)

**✨ 更新とドキュメント:**
- 🛠️ **エンコーダの再構築**: H.265 (HEVC) エンコーダをリファクタリングし、グローバルヘッダーフラグを削除してインバンドパラメータセットを強制しました。これにより、MPEG-TSコンテナやSLSなどのストリーミングサーバーとの互換性が大幅に向上しました。
- 📝 **ドキュメント**: AV1エンコードの説明を削除しました。H.265のステータスを更新し、SLSサーバーの限定的なサポートを反映しました。
- 🔧 **コードメンテナンス**: IDEのpthread関連のコンパイル警告（`pthread.h`）を修正しました。

### v1.2.1 (2026-03-24)

**✨ 新機能と改善:**
- 🏷️ **受信機のバージョン表示**: ユーザーが識別しやすいように、SEI ReceiverのプロパティUIの右下（最下部）にプラグインのバージョンラベルを追加しました。

**🔧 ビルドおよび互換性の修正:**
- ⚙️ **CMakeの動的依存関係解決**: `CMakeLists.txt` を更新し、ダウンロードされた最新の `obs-deps-*` フォルダを自動的に検出するようにしました。これにより、OBSのメジャーバージョン更新に伴うビルドエラーを永続的に防ぎます。
- ✅ OBS Studio **32.1.0** の対応およびコンパイル検証を完了。

### v1.2.0 (2026-01-22)

**🎉 新機能:**
- ✨ **マルチコーデックサポート**: **H.265 (HEVC)** および ~~**AV1**~~ エンコード形式を完全サポート（AV1同期は現在利用できません）
  - **H.265**: H.264と比較して30-50%の帯域幅節約
  - ~~**AV1**: 次世代の高効率圧縮~~
- 🛠️ **3つの独立したエンコーダ**:
  - `SEI STAMPER (H.264)`
  - `SEI STAMPER (H.265)`
  - ~~`SEI STAMPER (AV1)`~~（現在、同期は利用できません）
  - 各エンコーダはハードウェアアクセラレーション（Intel/NVIDIA/AMD）をサポート
- 🧠 **受信機の構成**: 正しくデコードするために、送信機と一致するコーデック形式を手動で選択する必要があります。

**⚠️ 重要な制限事項**:

- **H.264** および **H.265** は、SRTストリーミングで完全に動作確認されています。
- ~~**AV1** エンコードは利用可能ですが、OBS Studioのバージョンによっては、SRT出力でのAV1ストリーミングがサポートされていない場合があります。~~ **現在の状態：AV1同期は利用できません。**

### v1.1.3 (2026-01-04)

**✨ 新機能:**
- ⚙️ **受信機同期間隔のカスタマイズ**: 受信機に `NTP Sync Interval` 設定を追加

### v1.1.2 (2026-01-04)

**🔧 バグ修正:**
- 🐛 **受信機のカクツキ修正**: NTP同期の重大なパフォーマンス修正
- 🛡️ **ネットワーク耐性**: インテリジェントなバックオフメカニズムを追加

### v1.1.0 (2026-01-04)

**🎉 新機能:**
- ✨ **NVIDIA NVENCサポート**
- ✨ **AMD AMFサポート**
- 🚀 **マルチGPUサポート**

### v1.0.0 (2026-01-04)

**初回リリース**

---

**現在のバージョン**: 1.3.0
**最終更新**: 2026-09-05
