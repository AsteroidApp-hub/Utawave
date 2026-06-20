// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

#pragma once
#include <JuceHeader.h>
#include <vector>
#include "UtawaveLookAndFeel.h"
#include "../Audio/builtin/BuiltInEffect.h"

/**
    内蔵エフェクトのモダン UI 共通基盤。
    上部にプリセットコンボ、中央に**グラフ域** (各エフェクト固有のビジュアライズ)、
    下部に**コンパクトなロータリーノブ列** (指定パラメータ) を置く。
    派生は paintGraph() でグラフを描き、必要ならグラフ上のドラッグ操作を実装する。
    ノブは fx の値へ 30Hz で追従し (ドラッグ/入力中は除く)、GR メータ値も平滑化して提供する。
*/
class ModernEffectEditor : public juce::AudioProcessorEditor,
                           private juce::Timer
{
public:
    ModernEffectEditor(BuiltInEffect& fx, std::vector<int> knobParams, int graphHeight = 188);
    ~ModernEffectEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;

protected:
    virtual void paintGraph(juce::Graphics&, juce::Rectangle<float> area) = 0;
    virtual void onTick() {}
    virtual void onGraphMouseDown(const juce::MouseEvent&) {}
    virtual void onGraphMouseDrag(const juce::MouseEvent&) {}
    virtual void onGraphMouseUp(const juce::MouseEvent&) {}
    // 派生がグラフ上にオーバーレイ部品 (トグル等) を置くためのフック (resized 末尾で呼ばれる)
    virtual void layoutOverlay(juce::Rectangle<int> /*graphArea*/) {}

    void  clearPresetSelection() { presetBox.setSelectedId(0, juce::dontSendNotification); }
    float smoothedReductionDb() const noexcept { return smReductionDb; }
    juce::Slider* knobForParam(int param) const;   // 指定パラメータのノブ (無ければ nullptr)
    // GR メータ (右端の縦バー) 描画。コンプ/ディエッサー共用。fullScaleDb = 上限の減衰量。
    void drawReductionMeter(juce::Graphics&, juce::Rectangle<float> area, float fullScaleDb);
    // レベルメータ (下から上へ・シアングラデ) を rect に描く。コンプ IN/OUT・リバーブ OUT 共用。
    void drawLevelBar(juce::Graphics&, juce::Rectangle<float> rect, float db,
                      float minDb, float maxDb, juce::Colour col, const juce::String& label);

    BuiltInEffect& fx;
    juce::Rectangle<int> graph;

private:
    void timerCallback() override;

    UtawaveLookAndFeel laf;
    juce::ComboBox presetBox;
    juce::Label    presetLabel;
    juce::OwnedArray<juce::Slider> knobs;
    juce::OwnedArray<juce::Label>  knobLabels;
    std::vector<int> knobParamIdx;
    int   graphH;
    float smReductionDb { 0.0f };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ModernEffectEditor)
};
