# Utawave

[English](README.md) | **日本語**

歌い手（ボーカルカバー投稿者）向けの**録音特化 DAW**。macOS / Windows のクロスプラットフォームで無料配布します。

Copyright (C) 2025-2026 Utawave
Licensed under the **GNU Affero General Public License v3.0 or later (AGPL-3.0-or-later)**.

- ソースコード: https://github.com/AsteroidApp-hub/Utawave
- ライセンス全文: [LICENSE](LICENSE)
- 同梱ライブラリの表記: [THIRD_PARTY_LICENSES.txt](THIRD_PARTY_LICENSES.txt)

---

## 特長

- マルチトラック録音 / 再生（複数トラック同時録音・パンチイン・遡及録音・ループ録音）
- 波形編集（移動 / リサイズ / 分割 / 結合 / フェード / クロスフェード / 無音カット）
- MIDI トラック・ピアノロール・内蔵シンセ
- ミキサー / メータリング / 簡易リバーブ / プラグイン（VST3）ホスト
- WAV / AIFF / MP3 書き出し（内蔵エンコーダ・高品質ディザ）
- 日本語 / English

詳細は同梱マニュアル（[Docs/MANUAL.html](Docs/MANUAL.html)）と仕様書（[Docs/SPEC.md](Docs/SPEC.md)）を参照。

---

## ビルド

### 前提

- CMake 3.22 以上
- C++17 対応コンパイラ（macOS: Xcode / Windows: MSVC）
- JUCE 8（CMake が自動取得、またはローカル `JUCE/` に配置）

### 手順

```sh
git clone https://github.com/AsteroidApp-hub/Utawave.git
cd Utawave
cmake -S . -B build-mac -DCMAKE_BUILD_TYPE=Release
cmake --build build-mac --config Release
```

初回の configure で **JUCE 8 (8.0.12) を自動ダウンロード**します（CMake FetchContent）。
リポジトリに JUCE は含まれません。オフラインや特定バージョンでビルドしたい場合は、JUCE 8 を
リポジトリ直下 `JUCE/` に配置すると、ダウンロードせずローカルの `JUCE/` を使います。

### Windows での ASIO 対応（任意）

ASIO SDK は再配布が制限されているためリポジトリには含まれません。利用する場合は
[Steinberg Developer](https://www.steinberg.net/developers/) から SDK を取得し、
`Source/ThirdParty/asiosdk/` に配置してください（配置されていれば CMake が自動で有効化します）。
未配置の場合は WASAPI / DirectSound のみで動作します。

---

## テスト

```sh
cmake --build build-mac --target UtawaveTests --config Debug
./build-mac/UtawaveTests_artefacts/Debug/UtawaveTests
```

`juce::UnitTest` ベースのコンソールアプリです（全合格で終了コード 0）。

---

## ライセンス

本アプリ本体は **AGPL-3.0-or-later** で配布されます。AGPL ではバイナリを受け取った人に対して
対応するソースコードを提供する必要があり、本リポジトリがその対応ソースです。利用しているオープンソース
ライブラリ・SDK のライセンスは [THIRD_PARTY_LICENSES.txt](THIRD_PARTY_LICENSES.txt) を参照してください。

## 免責事項

本ソフトウェアは無料で「現状のまま（AS IS）」提供されます。動作の確認には努めていますが、
すべての環境での動作や特定の目的への適合をお約束するものではありません。

万一、本ソフトウェアの使用（または使用できないこと）により損害（録音・プロジェクトデータの
消失や破損、他のソフトウェア・機材への影響などを含みます）が生じた場合でも、作者は責任を
負いかねます。大切なデータは事前のバックアップをお願いします。

詳細は AGPL-3.0-or-later 第15条（保証の否認）および第16条（責任の制限）をご覧ください。

## 支援 / Support

開発の継続を応援していただけると励みになります 🙇 いただいた寄付は署名証明書の費用と
今後の開発に充てさせていただきます。

- **寄付 (Stripe)**: https://donate.stripe.com/8x29ATdTI6g07vo49S38404
- **Donate (English)**: https://donate.stripe.com/fZubJ16rg5bW9Dw9uc38405

## ダウンロード / Download

ビルド済みアプリ（macOS / Windows）は **[Releases](https://github.com/AsteroidApp-hub/Utawave/releases)**
からダウンロードできます（公式サイト <https://utawave.com/download.html> でも同じものを配布しています）。
リリース手順は [RELEASE.md](RELEASE.md) を参照。

## 貢献

Pull Request やバグ報告・提案を受け付けています。ただし個人開発のため、内容の確認・反映に
お時間をいただくことや、内容によっては取り込みをお約束できない場合があります。送る際は
[CONTRIBUTING.md](CONTRIBUTING.md) の条件（AGPL-3.0-or-later / DCO サインオフ）をご確認ください。
