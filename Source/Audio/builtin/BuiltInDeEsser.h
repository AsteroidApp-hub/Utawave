// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

#pragma once
#include "BuiltInEffect.h"
#include <atomic>

/**
    内蔵ディエッサー。サ行 (歯擦音) の帯域だけを動的に抑えるスプリットバンド方式。
    検出した高域の量に応じて「高域成分だけ」を引くので、無サ行時は完全素通り (out = x)。
    先読み無しでゼロレイテンシー。ステレオはリンク (両 ch の最大で 1 つのゲインを共有)。
*/
class BuiltInDeEsser : public BuiltInEffect
{
public:
    enum Param { FreqHz = 0, ThresholdDb, Ratio, NumParams };

    BuiltInDeEsser();

    const juce::String getName() const override;
    juce::String getIdentifier() const override { return "utawave.deesser"; }
    juce::int32  getUid()        const override { return 0x55744453; } // 'UtDS'

    void prepareToPlay(double sampleRate, int blockSize) override;
    void releaseResources() override {}
    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;   // BuiltInDeEsserEditor.cpp で定義

    bool  hasReductionMeter() const override { return true; }
    float getReductionDb()    const override { return meterReductionDb.load(std::memory_order_relaxed); }

private:
    double sr { 48000.0 };
    // 1 次の相補分割: hp = x - lp (lp は一極ローパス)。LP+HP=x を厳密に満たすので
    // out = x - (1-g)*hp = lp + g*hp となり、無サ行(g=1)で完全素通り・サ行時は位相ズレ無く減衰する。
    float  lpState[2] { 0.0f, 0.0f };
    float  gainDb { 0.0f };               // 平滑化済みゲイン (dB, 通常 <= 0)
    std::atomic<float> meterReductionDb { 0.0f };

    // ディエッサーのアタック/リリースは速め固定 (初心者向けに露出しない)
    static constexpr float kAttackS  { 0.001f };
    static constexpr float kReleaseS { 0.060f };
    static constexpr float kKneeDb   { 4.0f };
};
