// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

#pragma once
#include <JuceHeader.h>

// アプリ全体の UI 設定 (プロジェクトに依存しないグローバル設定)。
// 言語 (Localisation) や ウィンドウサイズ (WindowState) と同じく、プロジェクトファイル
// (.uta) ではなく ~/Library/Application Support/Utawave/app_prefs.xml に保存する。
class AppPreferences
{
public:
    // 「MIDI を書き出す」メニュー項目を表示するか (既定: 非表示)。
    // 普段はあまり使わない機能のため、設定で ON にした時だけメニューに出す。
    bool showMidiExportMenu { false };

    // 起動画面に広告枠を表示するか (既定: 表示)。公式ビルド内でのユーザー個別 ON/OFF。
    // ただし実効表示はコンパイル時マスタースイッチ (adsCompiledIn) との AND になる。
    bool showAds { true };

    // ピアノロール (MIDI クリップ編集) の自動ページング (既定: ON)。
    // 再生中に再生バーがビュー右端を越えたら、次のページが見えるよう横スクロールを飛ばす。
    // 小さい窓で MIDI を見ながら歌う用途で、手動スクロールせずに続きを追える。
    bool midiPagingEnabled { true };

    // ── 録音レイテンシ補正 (ハードウェア依存のためアプリ全体設定) ──
    // 録音クリップをデバイス報告の入出力レイテンシ分だけ手前へずらすか (既定: ON)。
    bool recLatencyAutoComp { true };
    // 追加の手動オフセット (ms, +で手前へ)。報告値が不正確なデバイス向けの微調整。
    double recLatencyManualMs { 0.0 };

    // 入力モニター中、トラックの INS (インサート FX) を返し音にも通すか (既定: ON)。
    // INS が空のトラックでは無影響 (チェーンをスキップ)。ルックアヘッド系プラグインで
    // モニタ遅延が気になるユーザー向けの逃げ道として OFF にできる。録音ファイルには焼かない。
    bool monitorThroughInserts { true };

    // ホバー時のツールチップ (ボタン等の説明) を表示するか (既定: 表示)。
    // 操作に慣れたユーザーが煩わしさを減らせるよう OFF にできる。
    bool showTooltips { true };

    // ディスクストリーミング (再生時の音声読み込みを先読みスレッドへ分離) を使うか (既定: ON)。
    // ON で audio スレッドのディスク I/O が先読みヒット時ゼロになり、多トラック/低速ディスクでの
    // 取りこぼしを減らす。ミス時は従来の同期読みへフォールバックするので出力は常に正確。
    // 切り分け/万一の不具合時に OFF で完全に従来経路へ戻せる。
    bool diskStreaming { true };

    // 歌詞表示窓の文字サイズ (px)。歌唱中の見やすさは人それぞれなのでアプリ全体で記憶する
    // (歌詞テキスト自体はプロジェクトごとに .uta へ保存)。load 時に妥当範囲へクランプ。
    int lyricsFontSize { 16 };

    // 広告機能のコンパイル時マスタースイッチ。公開ソースの既定は OFF (起動画面は 2 列・通信なし)。
    // 公式配布ビルドのみ CMake の UTAWAVE_ADS_ENABLED=1 で有効化する (詳細は CMakeLists / CLAUDE.md)。
    static bool adsCompiledIn();

    // 実効的に起動画面へ広告を出すか (= コンパイル時に有効 かつ ユーザーが showAds を ON)。
    bool adsEffective() const { return adsCompiledIn() && showAds; }

    static juce::File getStoreFile();
    static AppPreferences load();
    bool save() const;  // 書き込み失敗 (ディスク満杯等) は false。呼び出し側は無視してもよい
};
