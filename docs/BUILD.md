# Windowsビルドとテスト

## 必要なもの

- Windows 10／11、x64。
- Visual Studio 2022の「C++によるデスクトップ開発」（MSVC、Windows SDK）。
- CMake 3.22以降、Git。
- Python 3.10以降。MCPとPythonテストに追加パッケージは不要。
- `libmp3lame`を含むFFmpegのWindows実行ファイル。利用・配布条件を確認したものを指定する。
- 初回の依存取得にインターネット接続。[依存ライセンス](LICENSES.md)も参照。

## ビルド

リポジトリのルートでPowerShellを開く。FFmpegのパスは各自の配置に合わせる。

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
  -DSIMPLE_REC_FFMPEG_EXECUTABLE="C:/tools/ffmpeg/bin/ffmpeg.exe"
cmake --build build --config Release --parallel 3
```

JUCE 8.0.13、ARA SDK releases/2.1.0、Signalsmith Stretch 1.1.0はビルドフォルダーへ取得する。開発者PCの特定フォルダーを自動で参照する処理はない。

アプリは`build/SimpleRecPro_artefacts/Release/SimpleRec Pro.exe`。MP3変換に必要な`ffmpeg.exe`はアプリとテストの出力先へコピーされる。WindowsではFFmpegを指定できないと構成を失敗させる。

標準構成はASIOを無効にしている。ASIOを有効にする場合はSDKの条件と環境を確認し、構成時に`-DSIMPLE_REC_ENABLE_ASIO=ON`を指定する。第三者VST3・PitchNetは同梱していない。

## テスト

```powershell
ctest --test-dir build -C Release --output-on-failure
python tools/test_simplerecpro_mcp.py
python tools/test_assistant_e2e.py `
  --host "build/SimpleRecProTests_artefacts/Release/SimpleRecProTests.exe" `
  --workdir "build/assistant-e2e"
```

CTestは音声処理・保存・画面レイアウトなどのC++テストを実行する。MCP単体テストは通信と入力検証、E2Eテストは実際のC++テストホストとMCPを接続して操作を検証する。実際のマイク・スピーカーを使う録音や聴感の合格は意味しない。

画面レイアウトのテスト画像を生成する場合：

```powershell
& "build/SimpleRecProTests_artefacts/Release/SimpleRecProTests.exe" `
  --test-ui-layout "build/ui-review"
```

画像のトラックにはテスト用の合成音声を使う。

## オフライン構成

事前取得した同じ版の依存ソースを使う場合は、CMake標準の`FETCHCONTENT_SOURCE_DIR_JUCE`、`FETCHCONTENT_SOURCE_DIR_ARA_SDK`、`FETCHCONTENT_SOURCE_DIR_SIGNALSMITH_STRETCH`を絶対パスで指定できる。ARA SDKは必要なサブモジュールも取得済みであること。

## GitHub Actions

push、pull request、手動実行でSemgrepとGitleaksを実行する。両方に成功した場合のみWindowsのビルド、CTest、MCP単体・E2Eテストへ進む。バイナリの配布・アップロード・デプロイは行わない。
