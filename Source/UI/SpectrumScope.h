// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

#pragma once
#include <JuceHeader.h>
#include <array>
#include <vector>
#include <cmath>
#include "../Audio/pitchcore/RealFFT.h"

/**
    スペクトラムアナライザーの共通描画ヘルパ (EQ / ディエッサー で共用)。
    エフェクトの readAnalyzerSamples で取り出した入力波形を Hann 窓 → 自前 RealFFT に通し、
    bin 毎の線形振幅をピーク減衰で時間平滑。draw() で対数周波数軸に**1/6 オクターブの範囲平均**
    (累積和で O(1)) + フラクショナル補間で滑らかな塗り + 上辺ラインを描く (juce_dsp 非依存)。
    インスタンスは message thread から使う (内部に FFT のワークバッファを持つ)。
*/
class SpectrumScope
{
public:
    static constexpr int N = 2048;   // = BuiltInEQ::kAnalyzerSize (2 の冪)

    SpectrumScope()
    {
        for (int i = 0; i < N; ++i)
            window[(size_t) i] = 0.5f - 0.5f * std::cos(2.0f * juce::MathConstants<float>::pi
                                                        * (float) i / (float) (N - 1));
        binMag.assign((size_t) (N / 2 + 1), 0.0f);
        binMagPrefix.assign((size_t) (N / 2 + 2), 0.0f);
    }

    // reader(dest, N): 最新 N サンプルを dest へコピー (= effect.readAnalyzerSamples)
    template <class Reader>
    void update(Reader&& reader)
    {
        const int got = (int) reader(samples.data(), N);
        for (int i = got; i < N; ++i) samples[(size_t) i] = 0.0f;   // 取得不足分は 0 埋め (古い値で FFT しない)
        for (int i = 0; i < N; ++i)
            samples[(size_t) i] *= window[(size_t) i];
        fft.forward(samples.data(), spec.data());

        const float scale = 4.0f / (float) N;   // 0 dBFS 正弦 ≈ 振幅 1
        for (int k = 0; k <= N / 2; ++k)
        {
            const float re = spec[(size_t) (2 * k)], im = spec[(size_t) (2 * k + 1)];
            const float mag = std::sqrt(re * re + im * im) * scale;
            float& s = binMag[(size_t) k];
            s = mag > s ? mag : s * 0.82f + mag * 0.18f;
        }
        binMagPrefix[0] = 0.0f;
        for (int k = 0; k <= N / 2; ++k)
            binMagPrefix[(size_t) (k + 1)] = binMagPrefix[(size_t) k] + binMag[(size_t) k];
    }

    // rect 内に対数周波数 [fMin,fMax]・dB [anMin,anMax] で塗り + 上辺ラインを描く
    void draw(juce::Graphics& g, juce::Rectangle<float> rect, double sr,
              double fMin, double fMax, float anMin, float anMax,
              juce::Colour fillTop, juce::Colour line) const
    {
        const int w = juce::jmax(1, (int) rect.getWidth());
        const float span = anMax - anMin;
        const double binPerHz = N / juce::jmax(1.0, sr);
        const double octHalf = std::pow(2.0, 1.0 / 12.0);
        const double logRange = std::log(fMax / fMin);

        std::vector<float> ys((size_t) (w + 1));
        for (int px = 0; px <= w; ++px)
        {
            const double t = (double) px / juce::jmax(1, w);
            const double f = fMin * std::exp(t * logRange);
            const double fbin = f * binPerHz;

            int lo = juce::jlimit(0, N / 2, (int) std::floor(fbin / octHalf));
            int hi = juce::jlimit(0, N / 2, (int) std::ceil (fbin * octHalf));
            float mag;
            if (hi > lo)
                mag = (binMagPrefix[(size_t) (hi + 1)] - binMagPrefix[(size_t) lo]) / (float) (hi - lo + 1);
            else
            {
                const int b0 = juce::jlimit(0, N / 2, (int) std::floor(fbin));
                const int b1 = juce::jmin(N / 2, b0 + 1);
                const float fr = (float) (fbin - b0);
                mag = binMag[(size_t) b0] * (1.0f - fr) + binMag[(size_t) b1] * fr;
            }
            const float db = juce::jlimit(anMin, anMax, 20.0f * std::log10(mag + 1.0e-9f));
            ys[(size_t) px] = rect.getBottom() - ((db - anMin) / span) * rect.getHeight();
        }

        juce::Path fill, top;
        fill.startNewSubPath(rect.getX(), rect.getBottom());
        top.startNewSubPath(rect.getX(), ys[0]);
        for (int px = 0; px <= w; ++px)
        {
            const float x = rect.getX() + (float) px;
            fill.lineTo(x, ys[(size_t) px]);
            top.lineTo(x, ys[(size_t) px]);
        }
        fill.lineTo(rect.getRight(), rect.getBottom());
        fill.closeSubPath();

        juce::ColourGradient grad(fillTop.withAlpha(0.40f), rect.getX(), rect.getY(),
                                  fillTop.withAlpha(0.05f), rect.getX(), rect.getBottom(), false);
        g.setGradientFill(grad);
        g.fillPath(fill);
        g.setColour(line);
        g.strokePath(top, juce::PathStrokeType(1.2f));
    }

private:
    pitchcore::RealFFT fft { N };
    std::array<float, N>     samples {};
    std::array<float, N>     window {};
    std::array<float, N + 2> spec {};
    std::vector<float> binMag, binMagPrefix;
};
