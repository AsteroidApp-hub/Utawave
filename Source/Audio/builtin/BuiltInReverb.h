// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

#pragma once
#include "BuiltInEffect.h"

/**
    内蔵リバーブ。アルゴリズミック (FFT 非使用) なのでゼロレイテンシー (テールのみ)。
    ドライ/ウェットを Mix で混ぜるインサート型。歌に乗せやすいプリセットを用意する。
*/
class BuiltInReverb : public BuiltInEffect
{
public:
    enum Param { MixPct = 0, Size, Damping, Width, NumParams };

    BuiltInReverb();

    const juce::String getName() const override;
    juce::String getIdentifier() const override { return "utawave.reverb"; }
    juce::int32  getUid()        const override { return 0x55745256; } // 'UtRV'

    void prepareToPlay(double sampleRate, int blockSize) override;
    void releaseResources() override {}
    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;   // BuiltInReverbEditor.cpp で定義

    // ウェットのテールが残るため、停止後もしばらく鳴らせるよう申告する
    double getTailLengthSeconds() const override { return 4.0; }

private:
    juce::Reverb reverb;
    juce::AudioBuffer<float> wetBuf;
};
