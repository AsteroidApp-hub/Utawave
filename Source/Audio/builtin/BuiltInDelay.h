// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

#pragma once
#include "BuiltInEffect.h"
#include <atomic>
#include <vector>

/**
    内蔵ディレイ (エコー)。歌ってみた/歌枠の定番。**テンポ同期** (ホスト BPM を AudioPlayHead
    から取得・`EnginePlayHead` 供給済み) で 1/8 や付点 8 分などにスナップでき、フリー (ms) 指定も可。

    - フィードバックループに**トーン (ダンピング)** の一極ローパスを入れ、リピートが進むほど暗く
      なるアナログ/テープ風のキャラクタにする。
    - **ピンポン** (L↔R を交互に跳ねる) 切替。
    - ドライは常にフル、ウェットを**ミックス**で足す (声を消さずエコーを乗せる send 感)。
    - ディレイラインは prepareToPlay で確保し audio スレッドでは再確保しない。時間変更は
      サンプル単位で平滑化してクリック/ジッパーノイズを防ぐ。ゼロレイテンシー (PDC 不要)。
*/
class BuiltInDelay : public BuiltInEffect
{
public:
    enum Param { Sync = 0, Division, TimeMs, Feedback, Tone, Mix, PingPong, NumParams };

    BuiltInDelay();

    const juce::String getName() const override;
    juce::String getIdentifier() const override { return "utawave.delay"; }
    juce::int32  getUid()        const override { return 0x5574444C; } // 'UtDL'

    void prepareToPlay(double sampleRate, int blockSize) override;
    void releaseResources() override {}
    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;   // BuiltInDelayEditor.cpp で定義

    // UI メーター用の出力ピーク (dBFS, 生値・平滑化は UI 側)
    float getOutputDb()     const { return meterOutputDb.load(std::memory_order_relaxed); }
    // 実効ディレイ時間 (秒・同期解決後)。UI のタップ間隔/時間表示に使う。
    float getDelaySeconds() const { return curDelaySec.load(std::memory_order_relaxed); }

    // テンポ同期の音価リスト (UI のボタン/表示で共用・ASCII)
    static int         numDivisions();
    static const char* divisionLabel(int idx);   // "1/4" / "1/8." 等
    static double      divisionBeats(int idx);    // 四分音符を 1.0 とした拍数

    static constexpr double kMaxDelaySec { 3.0 };

private:
    double sr { 48000.0 };
    int    bufLen { 1 };
    int    writePos { 0 };
    float  delaySmoothed { 0.0f };   // 平滑化済みディレイ長 (サンプル)
    float  lpL { 0.0f }, lpR { 0.0f };
    double lastBpm { 120.0 };        // playhead から取れない時のフォールバック

    std::vector<float> bufL, bufR;   // ステレオ独立ディレイライン

    std::atomic<float> meterOutputDb { -100.0f };
    std::atomic<float> curDelaySec   { 0.35f };
};
