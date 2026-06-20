// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

#pragma once
#include <JuceHeader.h>
#include <array>
#include <vector>
#include "UtawaveLookAndFeel.h"
#include "../Audio/builtin/BuiltInEQ.h"
#include "../Audio/pitchcore/RealFFT.h"

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

private:
    void timerCallback() override;
    void updateAnalyzer();

    // 座標変換 (graph 矩形基準)
    float freqToX(double hz) const;
    double xToFreq(float x) const;
    float gainToY(double db) const;
    double yToGain(float y) const;

    struct Node { int freqParam; int gainParam; juce::Colour colour; };  // gainParam<0 = ゲイン無し(HPF)
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

    // アナライザー
    pitchcore::RealFFT fft { BuiltInEQ::kAnalyzerSize };
    std::array<float, BuiltInEQ::kAnalyzerSize>     samples {};
    std::array<float, BuiltInEQ::kAnalyzerSize>     window {};
    std::array<float, BuiltInEQ::kAnalyzerSize + 2> spec {};
    std::vector<float> binMag;       // bin 毎の平滑化済み線形振幅 (size = kAnalyzerSize/2 + 1)
    std::vector<float> binMagPrefix; // binMag の累積和 (オクターブ平滑の範囲平均を O(1) で引く)

    static constexpr float kFMin { 20.0f };
    static constexpr float kFMax { 20000.0f };
    static constexpr float kGMax { 18.0f };    // 縦軸 ±kGMax dB
    static constexpr float kAnMinDb { -90.0f };
    static constexpr float kAnMaxDb { 6.0f };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(BuiltInEQEditor)
};
