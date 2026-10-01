# ソースと依存ソフトウェアの利用条件

## SimpleRecPro

独自ソースの条件はルートの[LICENSE](../LICENSE)にある。閲覧と私的・非商用の評価ビルドを許可し、再配布・派生物の公開・商用利用の許諾は含めない。OSSライセンスとしての公開ではない。

依存SDK、実行ファイル、プラグイン、録音素材はこのソースセットに同梱していない。依存の利用条件は独自ソースの条件とは別に確認する。

## 依存一覧

| ソフトウェア | 使用箇所 | 版・利用条件 |
| --- | --- | --- |
| [JUCE](https://github.com/juce-framework/JUCE/tree/8.0.13) | UI、音声処理、デバイス、VST3ホスト | 8.0.13。AGPLv3またはJUCEの商用ライセンス。ビルド者が適用条件を確認する |
| [ARA SDK](https://github.com/Celemony/ARA_SDK/tree/releases/2.1.0) | ARAプラグインと音声・編集状態の共有 | releases/2.1.0。Apache 2.0。SDKとARA_API・ARA_LibraryのLICENSE・NOTICEを参照 |
| [Signalsmith Stretch](https://github.com/Signalsmith-Audio/signalsmith-stretch/tree/1.1.0) | キー／速度変更 | 1.1.0。MIT。内包するSignalsmith DSP 1.6.1のMIT表示も参照 |
| [FFmpeg](https://ffmpeg.org/legal.html) | MP3変換用の別プロセス | 実行ファイルのビルド構成によってLGPL／GPL等が異なる。CIではgyan.devのGPLv3ビルドを私的検証に使う |

JUCEの同梱依存（VST3 SDK、音声コーデック等）は、取得したJUCEの`LICENSE.md`と各依存ディレクトリの条件を参照する。ASIOは標準構成で無効。外部VST3・PitchNetは各提供元から入手する。

取得するソースは固定している。

- JUCE：`7c9d3783b127263d72bb65fe0a7e2dc8a02a7ac2`（8.0.13）、アーカイブのSHA-256もCMakeで検査。
- ARA SDK：`e87afbd2dd2693937bc9795b6b5ae87460fa8c1f`（releases/2.1.0）。必要なARA_API・ARA_Libraryのみ取得。
- Signalsmith Stretch：`44c8f865af9da8c29cc4a70a2d5a3ec83639c711`（1.1.0）。
- CI用FFmpeg：gyan.dev 7.1.1 essentials build。公式配布ZIPのSHA-256を検査。

参考として[licenses](licenses)にARAのApache 2.0全文・NOTICEと、Signalsmith Stretch／DSPのMIT全文を収録している。依存コードそのものは取得先から入手する。

## バイナリを配布する場合

このソースセットはアプリの実行ファイルを配布しない。ビルド済みアプリの配布には、JUCEの適用ライセンス、ARA等の権利表示、同梱FFmpegのライセンス・対応ソース提供条件を別途確認する必要がある。

独自ソースの評価ライセンスはJUCEのAGPLv3条件を代替しない。AGPLv3を選んだ組み合わせを再配布する場合も、このLICENSEだけで許可されたとは扱わない。

参照：[JUCE 8 EULA](https://juce.com/legal/juce-8-licence/)、[JUCE 8.0.13 LICENSE](https://github.com/juce-framework/JUCE/blob/8.0.13/LICENSE.md)、[FFmpegの法的情報](https://ffmpeg.org/legal.html)。
