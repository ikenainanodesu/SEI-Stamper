# OBS SEI Stamper プラグイン

<img src="pic\sei_stamper_gau.png" alt="isolated" width="250"/>


**OBS Studio用フレームレベルビデオ同期**

[English](README.md) | [中文](README.chs.md) | [日本語](#日本語)

---

## 日本語

### 概要

OBS SEI Stamperは、SEI（補足拡張情報）を使用してビデオストリームにNTPタイムスタンプを埋め込むことで、複数のストリーム間で**フレームレベルのビデオ同期**を実現するOBS Studioプラグインです。

**主な機能：**
- 🎯 **フレーム精度の同期** - NTPタイムスタンプを使用
- 📡 **Intel QuickSync H.264エンコーダ** - SEI対応のハードウェアアクセラレーションエンコード
- 🔄 **送信機と受信機** - エンコードとデコードの完全なソリューション
- 🌐 **SRTストリーミング** - 低遅延ストリーミング用のSRT受信機を内蔵
- ⏱️ **マイクロ秒精度** - プロフェッショナルアプリケーション向けのNTPベースタイミング

### 使用例

- マルチカメラライブプロダクション同期
- リモートスタジオのフレームレベル同期
- 放送品質のマルチソース位置合わせ
- ライブコンサート/イベントのマルチアングル録画

---

## インストール

### クイックインストール（推奨）

[Releases](https://github.com/yourusername/obs-sei-stamper/releases)ページから最新リリースをダウンロードしてください。

リリースパッケージには以下が含まれます：
- `obs-sei-stamper.dll` - メインプラグイン
- `srt.dll` - 受信機機能用のSRTライブラリ
- 多言語サポート用のロケールファイル

### 必要条件

- OBS Studio 28.0以降
- Windows 10/11 (64ビット)

### 手動インストール手順

1. **[Releases](https://github.com/yourusername/obs-sei-stamper/releases)ページからリリースパッケージをダウンロード**

2. **OBSプラグインディレクトリにコピー：**
   ```powershell
   # プラグインDLLをコピー
   Copy-Item obs-sei-stamper.dll "C:\Program Files\obs-studio\obs-plugins\64bit\"
   
   # SRTライブラリをコピー
   Copy-Item srt.dll "C:\Program Files\obs-studio\obs-plugins\64bit\"
   ```

3. **ロケールファイルをコピー：**
   ```powershell
   # ディレクトリを作成
   New-Item -ItemType Directory -Force `
       "C:\Program Files\obs-studio\data\obs-plugins\obs-sei-stamper\locale"
   
   # ロケールファイルをコピー
   Copy-Item data\locale\* `
       "C:\Program Files\obs-studio\data\obs-plugins\obs-sei-stamper\locale\" -Recurse
   ```

4. **OBS Studioを再起動**

---

## 使用方法

### 送信機（エンコーダ）

1. **設定 → 出力 → 出力モード：詳細**を開く
2. SEI Stamperエンコーダを選択：
   - **SEI Stamper (H.264 QuickSync)**
3. エンコーダプロパティを設定：
   - **NTPサーバー**: `time.windows.com`（または任意のNTPサーバー）
   - **NTPポート**: `123`（デフォルト）
   - **NTP同期を有効化**: ✓
4. ストリーミング/録画を開始

エンコーダは、SEIメタデータを使用して全フレームにNTPタイムスタンプを自動的に挿入します。

### 受信機（ソース）

1. OBSシーンで、**ソースを追加 +**をクリック
2. **SEI Receiver**を選択
3. ソースを設定：
   - **SRT URL**: `srt://送信機IP:ポート`（例：`srt://192.168.1.100:9000`）
   - **NTP同期を有効化**: ✓
   - **NTPサーバー**: 送信機と同じ
4. **OK**をクリック

受信機は以下を実行します：
- SRTストリームに接続
- ビデオフレームをデコード
- SEIからNTPタイムスタンプを抽出
- フレームレベル精度で再生を同期

---

## 検証

### FFprobeでSEIデータを確認

```powershell
# フレーム情報を表示
ffprobe -select_streams v:0 -show_frames output.mp4 2>&1 | Select-String "SEI"

# 詳細なフレームデータ
ffprobe -select_streams v:0 -show_frames -show_entries frame=pict_type output.mp4
```

### MediaInfoで確認

```powershell
MediaInfo --Full output.mp4 | Select-String "SEI"
```

---

## 技術詳細

### アーキテクチャ

```
┌─────────────┐         ┌──────────────┐         ┌─────────────┐
│    送信機    │         │  SRTストリーム│         │    受信機    │
│ (エンコーダ) │───────▶│   + SEI      │───────▶│  (ソース)   │
└─────────────┘         └──────────────┘         └─────────────┘
      │                                                  │
      ▼                                                  ▼
┌─────────────┐                                  ┌─────────────┐
│ NTPクライアント│◀────────────────────────────────▶│ NTPクライアント│
└─────────────┘         NTPサーバー              └─────────────┘
```

### SEI形式

- **UUID**: カスタム識別子（`a5b3c2d1-e4f5-6789-abcd-ef0123456789`）
- **ペイロードタイプ**: User Data Unregistered（タイプ5）
- **データ構造**:
  - UUID（16バイト）
  - PTS（8バイト）
  - NTPタイムスタンプ（8バイト：秒4バイト + 小数4バイト）

### サポートされているエンコーダ

| エンコーダ | SEI NALタイプ | ハードウェアアクセラレーション | ステータス |
|-----------|--------------|----------------------------|-----------|
| H.264     | Type 6       | Intel QuickSync            | ✅        |

---

## ソースからビルド

### 必要条件

- **CMake** 3.20以降
- **Visual Studio 2022**（C++デスクトップ開発ワークロード付き）
- **OBS Studioソースコード**（依存関係として含まれる）
- **FFmpegライブラリ**（OBSによって提供）
- **libsrt**（リポジトリに含まれる）

### クイックビルド（推奨）

ビルド経験のないユーザー向けに、自動ビルドスクリプトを使用：

1. **ビルドスクリプトを実行：**
   ```powershell
   # プロジェクトディレクトリに移動
   cd obs-sei-stamper
   
   # 自動ビルドスクリプトを実行
   .\build_and_install.bat
   ```

2. **プラグインを取得：**
   - ビルド成功後、プラグインファイルは`out/obs-studio/`ディレクトリに生成されます
   - プラグイン構造はOBSインストールディレクトリを反映

3. **インストール：**
   - `out/obs-studio/`の内容をOBSインストールディレクトリにコピー
   - デフォルト場所：`C:\Program Files\obs-studio`
   - **管理者権限が必要**

### 手動ビルド手順

ビルドプロセスを手動で制御したい場合：

1. **リポジトリをクローン：**
   ```bash
   git clone https://github.com/yourusername/obs-sei-stamper.git
   cd obs-sei-stamper
   ```

2. **CMakeを設定：**
   ```powershell
   mkdir build
   cd build
   cmake .. -G "Visual Studio 17 2022" -A x64
   ```

3. **ビルド：**
   ```powershell
   cmake --build . --config Release
   ```

4. **インストール（オプション）：**
   ```powershell
   cmake --install . --config Release
   ```

5. **出力ファイル：**
   - プラグイン：`build/plugin/Release/obs-sei-stamper.dll`
   - または簡単インストール用の`out/obs-studio/`ディレクトリ構造を使用

---

## トラブルシューティング

### 問題：エンコーダがOBSに表示されない

**解決策：**
- プラグインDLLが`obs-plugins/64bit/`ディレクトリにあることを確認
- OBSログで読み込みエラーを確認
- OBSバージョンが28.0以降であることを確認

### 問題：受信機がSRTに接続できない

**解決策：**
- `srt.dll`がインストールされていることを確認
- ファイアウォール設定を確認
- SRT URL形式を確認：`srt://ip:port`

### 問題：SEIデータが見つからない

**解決策：**
- NTPサーバーがアクセス可能であることを確認
- 「NTP同期を有効化」がチェックされていることを確認
- OBSログでNTP同期ステータスを確認

---

## パフォーマンス

- **CPUオーバーヘッド**: < 1%（SEI挿入）
- **NTP同期頻度**: 60秒ごと
- **フレーム精度**: 60fpsで±1フレーム
- **レイテンシ**: ~100ms（SRT 120msレイテンシ設定）

---

## コントリビューション

コントリビューションを歓迎します！プルリクエストをお気軽に送信してください。

### 開発ガイドライン

1. 既存のコードスタイルに従う
2. 複雑なロジックにはコメントを追加
3. 変更を徹底的にテスト
4. 必要に応じてドキュメントを更新

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

## サポート

- **問題**: [GitHub Issues](https://github.com/yourusername/obs-sei-stamper/issues)
- **ディスカッション**: [GitHub Discussions](https://github.com/yourusername/obs-sei-stamper/discussions)
- **ドキュメント**: [Wiki](https://github.com/yourusername/obs-sei-stamper/wiki)

---

**バージョン**: 1.0.0  
**最終更新**: 2026-01-04
