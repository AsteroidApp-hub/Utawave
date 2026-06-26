// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

#pragma once
#include <vector>
#include <string>

enum class SnapMode {
    Off,
    Bar,           // 1/1
    Half,          // 1/2
    Quarter,       // 1/4
    Eighth,        // 1/8
    Sixteenth,     // 1/16
    ThirtySecond,  // 1/32
    QuarterT,      // 1/4 三連
    EighthT,       // 1/8 三連
    SixteenthT     // 1/16 三連
};

// SnapMode の 1 グリッド単位を秒で返す (BPM 依存)。Off は 0.0。
// タイムライン / ピアノロールのスナップ計算で共通利用する。
inline double snapModeUnitSecs(SnapMode mode, double bpm)
{
    const double spb = 60.0 / (bpm > 1.0 ? bpm : 1.0);  // 1 拍 (四分音符) の秒数
    switch (mode)
    {
        case SnapMode::Bar:          return spb * 4.0;
        case SnapMode::Half:         return spb * 2.0;
        case SnapMode::Quarter:      return spb;
        case SnapMode::Eighth:       return spb * 0.5;
        case SnapMode::Sixteenth:    return spb * 0.25;
        case SnapMode::ThirtySecond: return spb * 0.125;
        case SnapMode::QuarterT:     return spb * (2.0 / 3.0);
        case SnapMode::EighthT:      return spb * (1.0 / 3.0);
        case SnapMode::SixteenthT:   return spb * (1.0 / 6.0);
        case SnapMode::Off:
        default:                     return 0.0;
    }
}

enum class ToolMode {
    Click,      // クリック/ハンドツールのみ
    Selection,  // 範囲選択ツールのみ
    Both        // リンク: 上半分=範囲、下半分=ハンド（スマートツール）
};

// 拍子の途中変更（小節頭で挿入）
struct MeterChange
{
    int barIndex   { 0 };  // 0-based の小節番号（0=曲頭）
    int numerator  { 4 };
    int denominator{ 4 };
};

// BPMの途中変更（時間ベース）
struct BpmChange
{
    double timeSec { 0.0 };
    double bpm     { 120.0 };
};

struct AppSettings
{
    bool     autoCrossfade     { false };
    bool     zeroCrossingFade  { false };
    double   crossfadeDuration { 0.020 };
    bool     showClipGain      { false };
    SnapMode snapMode          { SnapMode::Off };
    bool     useMarkerColors   { true };
    ToolMode toolMode          { ToolMode::Both };
    int      meterNumerator    { 4 };
    int      meterDenominator  { 4 };
    double   initialBpm        { 120.0 };  // 曲頭のBPM
    std::vector<MeterChange> meterChanges;  // bar 1 (index 0) は使わない: 0番目以外を昇順で保持
    std::vector<BpmChange>   bpmChanges;    // timeSec 昇順
    // インポート時のリサンプル出力ビット深度: 32 = 32bit float（既定）、24 = 24bit PCM (TPDFディザ)
    int      resampleOutputBits { 32 };
    // プロジェクト全体のサンプリングレート / ビット深度（録音・書き出しのターゲット）
    double   projectSampleRate { 48000.0 };
    int      projectBitDepth   { 32 };
    // クリップ/範囲選択時に再生バーを選択先頭へジャンプさせる (Insertion Follows Selection)
    bool     playheadFollowsSelection { false };
    // 録音フロー
    int      countInBars              { 0 };       // 0/1/2/4 小節
    double   preRollSecs              { 0.0 };     // 0/1/2/3 秒
    // ループ録音はループ範囲設定 + LOOP ボタン ON + 録音 で自動的に有効
    bool     retrospectiveEnabled     { true };    // 再生中バックグラウンド録音
    // マスターパネルの INS スロット表示
    bool     masterInsertSlotsVisible { true };
    // マスターパネル本体の折りたたみ（既定: 閉じた状態）
    bool     masterPanelCollapsed     { true };
    // タイムラインルーラーの時刻行表示
    bool     rulerTimeRowVisible      { true };
    bool     rulerBarsRowVisible      { true };
    // 自動保存（分）。0 = 無効
    int      autoSaveIntervalMinutes  { 5 };
    // 残しておく世代バックアップ（日時付き）の最大数。古い世代は自動で間引く
    int      maxBackups               { 20 };
    // VU メータの 0 VU 基準レベル (dBFS)。EBU R68 = -18, SMPTE = -20, 配信向け = -14
    float    vuReferenceLevel         { -18.0f };
    // ラウドネス自動調整のターゲット (LUFS)
    // -14=配信, -16=Apple Music, -18=Web/動画, -23=EBU R128, -24=ATSC
    float    loudnessTargetLufs       { -24.0f };
    // 停止時に再生開始位置へ戻る (RTZ / Return To Zero)
    bool     returnToStartOnStop      { true };
    // インポート時に loudnessTargetLufs に合わせてクリップゲインを自動調整
    bool     autoNormalizeOnImport    { true };
    // インポート時に WAV の iXML / bext メタデータを除去 (他DAWで埋め込まれたテンポ情報の流入を防ぐ)
    bool     stripImportedMetadata    { true };
    // 書き出し時にピークが 0 dBFS を超えていたら内部で減衰させてクリップを防ぐ
    bool     exportPeakGuard          { true };
    // Cmd+スクロール拡大の支点: false=再生バー中央、true=マウスカーソル位置
    bool     zoomToMousePosition      { true };

    // 歌詞表示窓に貼り付けたテキスト (UTF-8)。曲ごとに保持するためプロジェクトに保存する。
    // AppSettings.h を JUCE 非依存に保つため std::string (中身は UTF-8 バイト列)。
    std::string lyrics;

    // bar N (1-based) の拍子を返す
    void getMeterAtBar(int bar1, int& outNum, int& outDen) const
    {
        outNum = meterNumerator;
        outDen = meterDenominator;
        int barIdx = bar1 - 1;
        for (auto& mc : meterChanges)
            if (mc.barIndex <= barIdx) { outNum = mc.numerator; outDen = mc.denominator; }
            else break;
    }

    // 指定ビート（曲頭からの 0-based 通し拍）が小節頭か
    bool isDownbeatAtBeat(int beatTotal) const
    {
        if (beatTotal <= 0) return true;
        int bar = 1, accBeats = 0;
        while (accBeats < beatTotal)
        {
            int n, d; getMeterAtBar(bar, n, d);
            n = (n < 1 ? 1 : n);
            if (accBeats + n > beatTotal) return false;
            accBeats += n;
            ++bar;
            if (bar > 100000) return false;
        }
        return accBeats == beatTotal;
    }

    // 時刻 t におけるBPM
    double bpmAtTime(double t) const
    {
        double cur = initialBpm;
        for (auto& bc : bpmChanges)
            if (bc.timeSec <= t) cur = bc.bpm;
            else break;
        return cur;
    }

    // 曲頭から時刻 t までの累積ビート数（連続）
    double beatsAtTime(double t) const
    {
        double accT = 0.0, beats = 0.0, cur = initialBpm;
        for (auto& bc : bpmChanges)
        {
            if (bc.timeSec >= t) break;
            beats += (bc.timeSec - accT) * (cur / 60.0);
            accT = bc.timeSec;
            cur  = bc.bpm;
        }
        if (t > accT) beats += (t - accT) * (cur / 60.0);
        return beats;
    }

    // 時刻 t における (bar, beat) を 1-based で返す。
    // bpmChanges と meterChanges を両方考慮する。
    void barAndBeatAtTime(double t, int& outBar1, int& outBeat1) const
    {
        int totalBeats = (int) beatsAtTime(t);
        if (totalBeats < 0) totalBeats = 0;
        int bar = 1, acc = 0;
        while (true)
        {
            int n, d; getMeterAtBar(bar, n, d);
            if (n < 1) n = 1;
            if (acc + n > totalBeats) break;
            acc += n;
            ++bar;
            if (bar > 100000) break;  // 安全装置
        }
        outBar1  = bar;
        outBeat1 = (totalBeats - acc) + 1;
    }
};
