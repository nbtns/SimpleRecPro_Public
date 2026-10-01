# Codexと会話しながら音を編集する

SimpleRecProを起動すると、Codexチャットから内蔵の音量・音質・効果を操作できます。音を変える依頼、分析結果の説明、参考曲の指定、3案からの選択、好みの保存もチャットで進めます。SimpleRecProのメイン画面には、聴き比べ用の「変更前を聴く」「変更後を聴く」「試聴を止める」ボタンがあります。

音声処理と参考曲の分析はPC内で行います。音声ファイルを外部AI APIへ送る機能はありません。Codexへはトラック名、選択状態、調整値、測定結果などの操作情報が返ります。

## 最初の接続

1. [BUILD.md](BUILD.md)に従ってビルドし、SimpleRecProを起動します。
2. Python 3.10以降と、MCPを利用できるクライアントを用意します。Pythonの追加パッケージは不要です。
3. クライアントへ、`python`を実行コマンド、`tools/simplerecpro_mcp.py`の絶対パスを引数とするSTDIOサーバーを登録します。登録方法は使用するクライアントの説明を参照してください。

```json
{
  "command": "python",
  "args": ["C:/path/to/SimpleRecPro_Public/tools/simplerecpro_mcp.py"]
}
```

クライアントからPythonが見つからない場合は`command`に`python.exe`の絶対パスを指定します。この設定例にAPIキーは不要です。

4. 音声を読み込み、画面でトラック・クリップ・範囲を選択します。
5. 会話から「選択状態を確認して」「この範囲を少し明るくして」「変更前を聴かせて」と依頼できます。

参考曲はローカル音声ファイルのパスをチャットで指定します。例えば「この参考曲に近づけて」「この感じを『柔らかい声』で保存して」と依頼できます。

## できること

| 依頼の例 | 対応する操作 |
| --- | --- |
| 「ここの声を調整して」 | 選択中トラック・クリップ・時間範囲を共有。必要ならCodexが対象を選択 |
| 「もっと明るく、響きは少し控えめに」 | 内蔵の音質・響き・音量の調整。トラック全体なら圧縮、ノイズ低減、サ行抑制も操作 |
| 「最後の部分だけラジオ風に」 | 範囲指定のこもり、ラジオ、ノイズ、歪み、bitcrush。部分的な音量・明るさ・響きの調整も可能 |
| 「変更前と聴き比べたい」 | 実際に処理した前後の音声を測定し、大きい側を減衰して比較試聴 |
| 「元に戻して」「今の効果を半分に」 | 依頼単位のUndo/Redo、直前の調整量の変更 |
| 「音割れや音量の問題を調べて」 | 音声分析。結果を測定値として返す |
| 「自然・くっきり・広がりの3案を作って」 | 同じ変更前の状態から3案を生成し、選んだ案を反映 |
| 「この参考曲に近づけて」「この感じを保存して」 | ローカル参考曲との比較調整、名前付きの好みの保存・再適用 |

区間調整は音量・明るさ・響きに対応します。同じ区間への再調整は既存の設定を更新し、一部だけ重なる区間の指定はエラーで止まります。圧縮・ノイズ低減・サ行抑制を区間だけに頼むとエラーで止まり、勝手にトラック全体へ適用しません。外部VSTの任意パラメーター操作はこの接続には含まれません。音の好みの最終判断は実際の試聴で行ってください。

問題の分析はエフェクト適用前の元音声を対象にし、区間を指定した場合はその時間範囲を分析します。エフェクトを含む仕上がりは、処理済み音声を使う比較試聴で確認します。比較の音量揃えは処理済みWAVのRMS測定を使い、LUFSによる補正ではありません。参考曲の分析対象は先頭の最大60秒です。

## ツール契約

MCPは次の17ツールを公開します。`track_id`は`get_context`で返る固定IDです。省略時は選択トラックを使います。`scope`は`track`または`selection`、時間はプロジェクト上の秒です。

`adjust_sound`、`analyse_audio`、`create_variants`、`apply_preference` は `scope` を省略するとトラック全体を対象にします。「ここだけ」など範囲の依頼には必ず `scope: selection`、全体の依頼には `scope: track` を明示します。対象が変わった可能性がある場合は、操作直前に `get_context` を再取得してから対象を確定します。

`get_context`はDAW内部から `selected_track_id`、`selected_clip_id`、`selection` を読み取ります。`selection.has_start/has_end` と開始・終了秒を確認し、`source: ab` はA-B範囲、`source: clip` は選択クリップの範囲として扱います。Codexはこの情報で「ここ」を解釈します。ユーザーが別の対象を言葉で指定した場合は、返されたIDと秒数を使って `select_target` を呼べます。操作のために専用パネルを表示する必要はありません。

| ツール | 主な引数 |
| --- | --- |
| `get_context` | なし |
| `select_target` | `track_id?`, `clip_id?`, `start_seconds?`, `end_seconds?` |
| `adjust_sound` | `track_id?`, `scope?`, `gain_db?`（相対-24〜+12 dB）, `brightness?`（-1〜+1、0が中立）, `ambience?`, `stability?`, `noise_reduction?`, `de_ess?`（各0〜1）, `label?` |
| `add_effect` | `type`: `muffled/radio/noise/distortion/bitcrush`, `track_id?`, `amount?`, `wet?`（0〜1）, `fade_seconds?`（0〜10）, 開始・終了秒? |
| `analyse_audio` | `track_id?`, `scope?` |
| `get_job` | `job_id` |
| `compare` | `mode`: `before/after/stop` |
| `undo_edit`, `redo_edit` | なし |
| `scale_last_edit` | `factor`（0〜2。0.5は変更量を半分） |
| `create_variants` | `track_id?`, `scope?` |
| `choose_variant` | `variant_id`（生成結果から選択） |
| `match_reference` | `file_path`（既存ローカル音声ファイル）, `track_id?`, `strength?`（0〜1） |
| `save_preference` | `name`, `track_id?` |
| `apply_preference` | `name`, `track_id?`, `scope?` |
| `list_preferences` | なし |
| `transport` | `command`: `play/pause/stop/loop_selection/seek`, `position_seconds?` |

`analyse_audio`、`match_reference`、`compare`は重い処理をバックグラウンドで実行し、`job_id`を返すことがあります。`get_job`で完了と結果を確認してからユーザーに完了を伝えます。生成した案や比較音声を使う前には、別の編集で元の状態が変わっていないか確認します。

## 接続方式と障害時の扱い

PythonプロセスとCodexの間はUTF-8・1行1JSONのMCP STDIOです。Pythonと起動中のSimpleRecProの間は、同じOSユーザーのローカルファイルIPCです。HTTP待ち受け、外部サービス、追加のAPIキーは不要です。[MCP STDIO仕様](https://modelcontextprotocol.io/specification/2025-11-25/basic/transports)

Windowsの標準場所は`%APPDATA%\SimpleRecPro\CodexControl`です。`SIMPLE_REC_CONTROL_DIR`を両プロセスに設定すれば変更でき、Python側は`--control-dir`でも指定できます。テストや複数の独立環境では別フォルダーを使います。

`manifest.json`にプロトコル版、起動ごとのsession/token、PID、相対要求・応答フォルダー、heartbeatを保持します。tokenはローカルの接続用であり、ソースコードに固定しません。同じユーザーで動く他のプログラムや管理者から隔離する仕組みではありません。manifestや要求ファイルを公開・Git登録しないでください。

要求は一時ファイルからatomicに配置し、C++側でサイズ（64 KiB）、期限（最大60秒）、session/token、IDを確認します。1回のタイマー処理で最大8件を処理し、編集はメッセージスレッドで実行します。同一セッションの同じ要求IDは重ねて実行しません。Python側の応答上限は4 MiBです。

アプリ終了、session変更、timeoutを検出したときは自動再送しません。返答が届かなくても音の変更が済んでいる可能性があるため、`get_context`で状態を確かめてから次の操作を行います。要求のタイムアウトは既定20秒、`--timeout`で0.1〜55秒の範囲に変更できます。通常の長い分析は`get_job`で追跡します。

## 開発用の検証

```powershell
python -m unittest discover -s tools -p test_simplerecpro_mcp.py -v
```

この検証はMCP初期化・ツール一覧・引数検証・IPC往復・期限切れ・session交換・終了検知を確認します。実際のスピーカーからの再生や録音品質の確認とは別です。

実装済みのC++操作経路まで含める場合は、ビルドしたテストホストを使います。8機能のMCP往復・数値変化・選択範囲・非同期処理・比較音量を検証し、結果と呼び出し記録を指定フォルダーに残します。

```powershell
python tools/test_assistant_e2e.py --host build/SimpleRecProTests_artefacts/Release/SimpleRecProTests.exe --workdir build/assistant-e2e
```
