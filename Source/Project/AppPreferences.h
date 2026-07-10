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
    // 追加の手動オフセット (ms, +で手前へ、±300 まで)。報告値が不正確なデバイス向けの調整。
    double recLatencyManualMs { 0.0 };

    // Q (リテイク) で中断したテイクをテイクレーンに残すか (既定: OFF = 完全破棄)。
    // ON にすると Q で録音をやり直しても直前のテイクがテイクリストへ確定される
    // (Lane 0 には置かない。録り直し後に Shift+↑ で採用できる)。
    bool retakeKeepsTake { false };

    // 入力モニター中、トラックの INS (インサート FX) を返し音にも通すか (既定: ON)。
    // INS が空のトラックでは無影響 (チェーンをスキップ)。ルックアヘッド系プラグインで
    // モニタ遅延が気になるユーザー向けの逃げ道として OFF にできる。録音ファイルには焼かない。
    bool monitorThroughInserts { true };

    // ホバー時のツールチップ (ボタン等の説明) を表示するか (既定: 表示)。
    // 操作に慣れたユーザーが煩わしさを減らせるよう OFF にできる。
    bool showTooltips { true };

    // フォルダトラック (グループバス) を追加できるようにするか (既定: OFF)。
    // ON にすると「+ トラック追加」メニューに「フォルダトラックを追加」が出る。
    // 初心者向けに普段は隠しておき、トラックが増えたユーザーだけが有効化する想定。
    // OFF にしても既存プロジェクトのフォルダトラックはそのまま動く (追加導線が消えるだけ)。
    bool enableFolderTracks { false };

    // フォルダトラックに Pan / Rev / INS スロットを表示するか (既定: 表示)。
    // 「フォルダは M/S/Vol だけのシンプルなまとめ役でいい」ユーザー向けに隠せる。
    // 表示のみの設定で、非表示でも既存の Pan/Rev/INS の値・効果はそのまま生きる。
    bool showFolderPanRevIns { true };

    // 書き出し完了後に完了ダイアログを表示するか (既定: 表示。2 ミックス / stems 共用)。
    // 毎回の確認が煩わしいユーザーは、完了ダイアログの「次回から表示しない」チェック、または
    // 環境設定でここを OFF にできる。OFF でも書き出し自体・出力フォルダを開く動作は変わらない。
    bool showExportCompleteDialog { true };

    // 前回作成したプロジェクトのサンプルレート / ビット深度 (新規作成ダイアログの既定に引き継ぐ)。
    // lastProjectSampleRate は 0 = 未設定 (初回はデバイスの現在 SR にフォールバック)。
    // 作成後は実際に確定した projectSampleRate / projectBitDepth を保存する。
    double lastProjectSampleRate { 0.0 };
    int    lastProjectBitDepth   { 32 };

    // 前回使った新規プロジェクトの保存先フォルダ (新規作成ダイアログの既定に引き継ぐ)。
    // 空 = 未設定 (既定の ~/Documents/Utawave を使う。「デフォルト」ボタンで空へ戻す)。
    juce::String lastProjectLocation;

    // ディスクストリーミング (再生時の音声読み込みを先読みスレッドへ分離) を使うか (既定: ON)。
    // ON で audio スレッドのディスク I/O が先読みヒット時ゼロになり、多トラック/低速ディスクでの
    // 取りこぼしを減らす。ミス時は従来の同期読みへフォールバックするので出力は常に正確。
    // 切り分け/万一の不具合時に OFF で完全に従来経路へ戻せる。
    bool diskStreaming { true };

    // オーディオのマルチコア処理 (再生時のトラック描画を複数コアへ分散) を使うか (既定: ON)。
    // ON で多トラック/重いプラグインのセッションが全コアを使えてスケールする。マスター加算は
    // 固定順なので出力はビット同一。稀にスレッド安全でないプラグイン向けに OFF で単一スレッドへ。
    bool multicoreAudio { true };

    // 歌詞表示窓の文字サイズ (px)。歌唱中の見やすさは人それぞれなのでアプリ全体で記憶する
    // (歌詞テキスト自体はプロジェクトごとに .uta へ保存)。load 時に妥当範囲へクランプ。
    int lyricsFontSize { 16 };

    // 画面全体の表示倍率 (1.0 = 等倍)。高解像度/大画面のモニタで文字やレイアウトが小さく
    // 見えるとき、アプリ全体を拡大して見やすくする (juce::Desktop::setGlobalScaleFactor)。
    // ハードウェア (モニタ) 依存のためプロジェクトではなくアプリ全体で記憶する。
    // load 時に妥当範囲 (0.75〜2.0) へクランプ。
    double uiScale { 1.0 };
    static constexpr double minUiScale { 0.75 };
    static constexpr double maxUiScale { 2.0 };

    // ユーザーが環境設定で表示倍率を明示的に選んだか (既定: 未設定)。
    // false の間は起動時にディスプレイ幅から自動決定する (autoUiScaleForMainDisplay)。
    // 一度ユーザーが選ぶと true になり、以降は uiScale を尊重する (モニタが変わっても勝手に変えない)。
    bool uiScaleUserSet { false };

    // 自動判定に使うディスプレイの論理幅 (px)。取得できなければ 0。
    // macOS は主ディスプレイの [NSScreen frame] のポイント幅、Windows/Linux は論理幅。
    // 複数モニタでは最も広いディスプレイを採用する (外部の大画面で開く想定)。
    static int autoScaleDisplayWidth();

    // autoScaleDisplayWidth() から推奨表示倍率を求める (1920 幅で 125% など)。
    // autoScaleDisplayWidth 側で globalScaleFactor を掛け戻しているため、スケール適用後に
    // 呼んでも安定する (適用→読み直しで倍率が崩れない)。
    static double autoUiScaleForMainDisplay();

    // 実効表示倍率。ユーザー未設定なら自動判定、設定済みなら保存値。
    double resolvedUiScale() const
    {
        return uiScaleUserSet ? uiScale : autoUiScaleForMainDisplay();
    }

    // 広告機能のコンパイル時マスタースイッチ。公開ソースの既定は OFF (起動画面は 2 列・通信なし)。
    // 公式配布ビルドのみ CMake の UTAWAVE_ADS_ENABLED=1 で有効化する (詳細は CMakeLists / CLAUDE.md)。
    static bool adsCompiledIn();

    // 実効的に起動画面へ広告を出すか (= コンパイル時に有効 かつ ユーザーが showAds を ON)。
    bool adsEffective() const { return adsCompiledIn() && showAds; }

    static juce::File getStoreFile();
    static AppPreferences load();
    bool save() const;  // 書き込み失敗 (ディスク満杯等) は false。呼び出し側は無視してもよい
};
