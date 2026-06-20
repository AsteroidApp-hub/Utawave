// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

#pragma once
#include <JuceHeader.h>
#include "UtawaveLookAndFeel.h"
#include "../Audio/builtin/BuiltInEffect.h"

/**
    内蔵エフェクト共通の汎用エディタ。BuiltInEffect のパラメータモデルから
    ロータリーノブとプリセットコンボを自動生成する。コンプ等 GR メータを持つ
    エフェクトでは右端に減衰メータを表示する。ダークテーマに合わせる。
*/
class BuiltInEffectEditor : public juce::AudioProcessorEditor,
                            private juce::Timer
{
public:
    explicit BuiltInEffectEditor(BuiltInEffect& effect);
    ~BuiltInEffectEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    void syncKnobsFromEffect();

    BuiltInEffect& fx;
    UtawaveLookAndFeel laf;

    juce::Label  presetLabel;
    juce::ComboBox presetBox;
    juce::OwnedArray<juce::Slider> knobs;
    juce::OwnedArray<juce::Label>  knobLabels;

    bool hasMeter { false };
    juce::Rectangle<int> meterArea;
    float meterShownDb { 0.0f };

    int cols { 4 };
    int rows { 1 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(BuiltInEffectEditor)
};
