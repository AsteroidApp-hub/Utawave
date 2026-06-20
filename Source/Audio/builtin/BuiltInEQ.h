// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

#pragma once
#include "BuiltInEffect.h"
#include "Biquad.h"

/**
    内蔵 EQ。ローカット (HPF) + 低域シェルフ + 中域ピーク (プレゼンス) + 高域シェルフ (エア)。
    すべて IIR 双二次 (ゼロレイテンシー)。歌の整音に必要十分なバンドに絞る。
*/
class BuiltInEQ : public BuiltInEffect
{
public:
    enum Param { HpfHz = 0, LowHz, LowDb, MidHz, MidDb, AirHz, AirDb, NumParams };

    BuiltInEQ();

    const juce::String getName() const override;
    juce::String getIdentifier() const override { return "utawave.eq"; }
    juce::int32  getUid()        const override { return 0x55744551; } // 'UtEQ'

    void prepareToPlay(double sampleRate, int blockSize) override;
    void releaseResources() override {}
    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override;

private:
    double sr { 48000.0 };
    Biquad hpf, low, mid, air;
};
