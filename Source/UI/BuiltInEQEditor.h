// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

#pragma once
#include <JuceHeader.h>
#include <array>
#include <vector>
#include "UtawaveLookAndFeel.h"
#include "SpectrumScope.h"
#include "../Audio/builtin/BuiltInEQ.h"

/**
    内蔵 EQ の専用エディタ (モダン UI)。
    背面にリアルタイムのスペクトラムアナライザー、その上に周波数特性カーブを描き、
    各バンドを**カーブ上のノードをドラッグ**して操作する (横=周波数 / 縦=ゲイン)。
    上部にプリセットコンボ。ノブは出さない。
*/
class BuiltInEQEditor : public juce::AudioProcessorEditor,
                        private juce::Timer
{
public:
    explicit BuiltInEQEditor(BuiltInEQ& eq);
    ~BuiltInEQEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;

    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseMove(const juce::MouseEvent&) override;
    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

private:
    void timerCallback() override;

    // 座標変換 (graph 矩形基準)
    float freqToX(double hz) const;
    double xToFreq(float x) const;
    float gainToY(double db) const;
    double yToGain(float y) const;

    // gainParam<0 = ゲイン無し(HPF) / qParam<0 = Q 無し(HPF/シェルフ)
    struct Node { int freqParam; int gainParam; int qParam; juce::Colour colour; };
    juce::Point<float> nodePos(const Node&) const;
    int  hitTestNode(juce::Point<float> p) const;   // -1 = なし
    static juce::String formatFreq(double hz);

    BuiltInEQ& eq;
    UtawaveLookAndFeel laf;

    juce::ComboBox presetBox;
    juce::TextButton resetBtn;       // 全バンドをフラットへ
    juce::TextButton spectrumBtn;    // アナライザー表示 ON/OFF (トグル)
    bool spectrumOn { true };
    juce::Rectangle<int> graph;     // アナライザー/カーブ描画域
    std::vector<Node> nodes;
    int dragNode { -1 };
    int hoverNode { -1 };

    SpectrumScope scope;   // アナライザー (共通ヘルパ)

    // EQ 特性カーブのキャッシュ (パラメータ/サイズ変化時だけ再計算。アナライザーの 30Hz
    // アニメーションごとに 6 biquad × ピクセル数を再評価しないため)。
    juce::Path cachedCurve, cachedCurveFill;
    bool curveDirty { true };
    void rebuildCurveIfNeeded();

    static constexpr float kFMin { 20.0f };
    static constexpr float kFMax { 20000.0f };
    static constexpr float kGMax { 18.0f };    // 縦軸 ±kGMax dB
    static constexpr float kAnMinDb { -90.0f };
    static constexpr float kAnMaxDb { 6.0f };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(BuiltInEQEditor)
};
