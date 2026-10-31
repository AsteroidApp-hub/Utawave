# Utawave へのコントリビューションについて

[English](CONTRIBUTING.md) | **日本語**

Pull Request やバグ報告・改善のご提案を受け付けています。
ただし個人開発のため、内容の確認や反映までお時間をいただくことがあり、
また内容によっては取り込みをお約束できない場合があります。あらかじめご了承ください。
Pull Request を送る場合は、以下の条件に同意したものとみなします。

## ライセンス（重要 / inbound = outbound）

- 本プロジェクトへのコントリビューションは、本体と同じ
  **AGPL-3.0-or-later** で提供いただくものとします（inbound = outbound）。
- プルリクエストを送る = あなたの変更を AGPL-3.0-or-later で提供することに同意した、とみなします。
- Utawave は **JUCE**（AGPLv3 / 商用のデュアルライセンス）にリンクしています。
  本プロジェクトは AGPL の選択肢で利用しており、コントリビューションも AGPL で受け入れます。

## DCO（Developer Certificate of Origin）

各コミットに **サインオフ** を付けてください。これは
[Developer Certificate of Origin](https://developercertificate.org/) に同意し、
「自分にその変更を貢献する権利がある」ことを表明するものです。

```sh
git commit -s -m "Fix: ..."
```

`-s` を付けると以下の行がコミットメッセージに追加されます:

```
Signed-off-by: Your Name <your.email@example.com>
```

## 開発環境

ビルド手順は [README.md](README.md) を参照してください
（CMake 3.22+ / C++17 / JUCE 9 は CMake が自動取得）。変更後はユニットテストを実行してください:

```sh
cmake --build build --target UtawaveTests --config Debug
./build/UtawaveTests_artefacts/Debug/UtawaveTests
```

## コーディング方針（抜粋）

- C++17。JUCE のクラス・型（`juce::String` / `juce::File` 等）を活用。
- **オーディオスレッドではメモリ確保・ロック・I/O を行わない**。
- **エンドユーザーに見える文言は `tr(u8"...")` で多言語化**し、英訳テーブルにキーを追加する
  （キーは末尾スペース・改行まで完全一致させること）。
- **ユーザーに見える UI 文言に他社製品名・外部ライブラリ名を露出させない**
  （ライセンス表記ファイルを除く）。
