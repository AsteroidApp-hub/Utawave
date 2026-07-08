// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

#pragma once
#include <JuceHeader.h>
#include <map>

class AudioEngine;

class ExportEngine
{
public:
    enum class Format { WAV, AIFF, MP3 };

    struct Options
    {
        juce::File file;
        Format     format       { Format::WAV };
        int        bitDepth     { 24 };       // 16 / 24 / 32（32 = float）
        double     sampleRate   { 0.0 };      // 0 = エンジンSRをそのまま
        double     startSec     { 0.0 };
        double     endSec       { 0.0 };
        bool       dither       { true };     // 16/24bit のみ有効（TPDF）
        bool       stems        { false };    // true: 1トラック=1ファイル、false: 1ミックスダウンファイル
        int        numChannels  { 2 };        // 出力チャンネル数（1=モノラル、2=ステレオ）
        bool       preFader     { false };    // true: 素のクリップ音のみ（プラグインもトラックVol/Pan/マスターも通さない）
        bool       peakGuard    { true };     // true: ピーク超過時に内部で減衰してクリップを防ぐ
        int        mp3BitrateKbps { 192 };    // MP3 出力時のビットレート（kbps）
        bool       autoRename     { true };   // 同名ファイル存在時に連番を付与
        bool       revealAfter    { true };   // 完了後にフォルダを Finder で開く（呼び出し側で処理）
        bool       realtime       { false };  // 実時間レンダリング（VST 等の互換用）
        // CLICK トラック (メトロノーム合成) をミックスに混ぜる (2 ミックス用)。
        // 呼び出し側が「鳴っている状態か (存在・非ミュート・ソロ規則)」を判定して立てる。
        // 2 ミックスは selectedTrackIndices を常に明示指定するため、このフラグが唯一の経路
        bool       includeClick   { false };
        juce::String           baseName;                 // mix-down 時のファイル名（拡張子なし）
        // 書き出し対象のトラック index 一覧
        // ・mix-down: ここに含まれるトラックのみをミックスして単一ファイルに書き出し
        // ・stems   : 各トラックを個別ファイルに書き出し
        // ・空      : 全トラック（Solo/Mute 考慮）
        std::vector<int>       selectedTrackIndices;
        // stems 時の各トラックの出力チャンネル数（1 or 2）。
        // 未指定のトラックは Options.numChannels が使用される。
        std::map<int, int>     trackChannelsMap;
        // stems 時の各トラックの Pre/Post 設定（true = Pre / false = Post）
        // 未指定のトラックは Options.preFader が使用される。
        std::map<int, bool>    trackPreFaderMap;
    };

    // 同期書き出し（呼び出しスレッドで実行）。書き出し中の進捗は progress(0..1) で通知。
    // shouldCancel が true を返した時点で中止し false を返す。
    static bool render(AudioEngine& engine, const Options& opts,
                       std::function<void(double)> progress = {},
                       std::function<bool()> shouldCancel = {},
                       juce::String* errorOut = nullptr);
};
