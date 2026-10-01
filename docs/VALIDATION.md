# 公開用ソースの検証記録

確認日：2026-10-01。

このファイルは公開用ソースの準備時にローカルで実行した確認を記録する。実機録音、外部プラグインの聴感確認とは区別する。公開後のGitHub Actionsの結果は、下記のリンクから確認できる。

## 確認環境

- Windows x64、MSVC 19.44、CMake 4.0、Ninja、Python 3.12.14。
- JUCE 8.0.13、ARA SDK 2.1.0、Signalsmith Stretch 1.1.0、FFmpeg 7.1.1 essentials build。
- ASIOは無効。音声・プロジェクトのテストには合成音声を使用。

## ローカルの実行結果

| 確認 | 結果 |
| --- | --- |
| 公開用ソースからのReleaseビルド | アプリと`SimpleRecProTests`の両方で成功 |
| 標準経路での依存取得と構成 | パスの上書きなしで、JUCE・ARA・Signalsmithを新しいビルドフォルダーへ取得し、CMakeの構成と生成に成功 |
| CTest | `AudioEngineDeterministicTests` 1/1成功。保存・編集・音声処理・出力・UI等の複数検証をこのテスト実行に含む |
| MCP単体テスト | 11件成功 |
| MCP E2E | 対象選択、音質調整、Undo／Redo・弱める操作、範囲編集・効果、音声解析、3案、参考曲・好み、変更前後の比較が成功 |
| 画面レイアウトと画像出力 | レイアウト・スクロール・選択の連動テストが成功。READMEの画像を生成して確認 |
| Gitleaks 8.30.1 | 公開候補のファイルに対する検査で検出なし |
| actionlint 1.7.12 | GitHub Actionsの構文検査に成功。ShellCheck・Pyflakesはこの構文検査では使用していない |
| 公開ソースの照合 | `Source`・`Tests`・`tools`の80ファイルをSHA-256で照合。元の機能コードと一致 |

Releaseビルドは事前取得した同じ版の依存ソースをCMakeの明示的なパス指定で利用した。JUCEは公式固定コミットのアーカイブを取得し、SHA-256を検査した。FFmpegも固定版の公式ZIPを使用した。

## GitHubでの自動検証

[Source checksの実行結果](https://github.com/nbtns/SimpleRecPro_Public/actions/workflows/checks.yml)から、各コミットの結果を確認できる。SemgrepとGitleaksが成功した場合にのみ、Windows Server 2022／Visual Studio 2022でのビルド、CTest、MCP単体・E2Eテストを実行する。

## 実機で確認していない項目
- 実オーディオ機器での録音・再生、利用者による音の聴き比べ。
- 外部VST3・PitchNetの実製品を使った互換性・音質・編集画面・保存復元の確認。
- 長時間・大容量の実プロジェクトでの操作評価、ASIO有効構成。

自動テストの成功は、これらの実機・聴感確認を完了したことを意味しない。Gitleaksの検出なしは、秘密情報やすべての脆弱性が存在しないことを保証するものではない。
