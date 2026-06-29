// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

#include "BuiltInDeEsser.h"
#include "GainDynamics.h"
#include "../../Localisation.h"
#include <cmath>

BuiltInDeEsser::BuiltInDeEsser()
{
    addParam({ "freqHz",      u8"周波数",       "Hz", 1500.0f, 16000.0f, 6500.0f, 0.5f });
    addParam({ "thresholdDb", u8"スレッショルド", "dB",  -48.0f,    0.0f,  -28.0f, 1.0f });
    addParam({ "ratio",       u8"レシオ",        ":1",    1.0f,   10.0f,    4.0f, 0.4f });

    //          freq    thr    ratio
    addPreset(u8"オフ",       { 6500.0f, -10.0f, 1.0f });
    addPreset(u8"ボーカル",   { 7000.0f, -28.0f, 3.0f });
    addPreset(u8"しっかり",   { 6500.0f, -32.0f, 5.0f });
    addPreset(u8"サ行 高め",  { 8500.0f, -28.0f, 4.0f });
    addPreset(u8"サ行 低め",  { 5500.0f, -28.0f, 4.0f });
}

const juce::String BuiltInDeEsser::getName() const { return tr(u8"ディエッサー"); }

int BuiltInDeEsser::readAnalyzerSamples(float* dest, int maxN) const
{
    const int count = juce::jmin(maxN, kAnalyzerSize);
    const int pos = analyzerWritePos.load(std::memory_order_relaxed);
    const int start = pos - count;
    for (int i = 0; i < count; ++i)
    {
        int idx = (start + i) % kAnalyzerSize;
        if (idx < 0) idx += kAnalyzerSize;
        dest[i] = analyzerRing[(size_t) idx];
    }
    return count;
}

void BuiltInDeEsser::prepareToPlay(double sampleRate, int)
{
    sr = sampleRate > 0.0 ? sampleRate : 48000.0;
    lpState[0] = lpState[1] = 0.0f;
    gainDb = 0.0f;
    meterReductionDb.store(0.0f, std::memory_order_relaxed);
}

void BuiltInDeEsser::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const float threshold = getP(ThresholdDb);
    const float ratio     = juce::jmax(1.0f, getP(Ratio));

    // 一極ローパスの係数 (1 次)。fc から alpha = 1 - exp(-2π fc / sr)。
    const float alpha = (float) juce::jlimit(0.0, 0.999,
        1.0 - std::exp(-2.0 * juce::MathConstants<double>::pi * getP(FreqHz) / sr));

    const float atkCoef = (float) std::exp(-1.0 / (sr * kAttackS));
    const float relCoef = (float) std::exp(-1.0 / (sr * kReleaseS));
    const float slope    = 1.0f - 1.0f / ratio;

    const int numCh = juce::jmin(2, buffer.getNumChannels());
    const int n     = buffer.getNumSamples();
    float* L = buffer.getWritePointer(0);
    float* R = numCh > 1 ? buffer.getWritePointer(1) : nullptr;

    // アナライザー (UI 表示中のみ): 入力 (処理前) のモノ和をリングへ
    if (analyzerActive.load(std::memory_order_relaxed) && numCh > 0)
    {
        int pos = analyzerWritePos.load(std::memory_order_relaxed);
        const float* rr = R ? R : L;
        for (int i = 0; i < n; ++i)
        {
            analyzerRing[(size_t) pos] = 0.5f * (L[i] + rr[i]);
            pos = (pos + 1) & (kAnalyzerSize - 1);
        }
        analyzerWritePos.store(pos, std::memory_order_relaxed);
    }

    if (! std::isfinite(gainDb)) { gainDb = 0.0f; lpState[0] = lpState[1] = 0.0f; }

    float maxReductionDb = 0.0f;

    for (int i = 0; i < n; ++i)
    {
        // 入力をブロック内でサニタイズ: NaN/Inf が lpState (生入力で更新) に入ると当該ブロックの
        // 残りが NaN 出力になるため、ここで 0 に潰してフィルタ状態と出力を汚染させない。
        float xL = L[i]; if (! std::isfinite(xL)) xL = 0.0f;
        float xR = R ? R[i] : xL; if (! std::isfinite(xR)) xR = 0.0f;

        // 1 次相補分割: lp を更新し hp = x - lp。検出にも減衰にも同じ hp を使う。
        lpState[0] += alpha * (xL - lpState[0]);
        const float hbL = xL - lpState[0];
        float hbR;
        if (R) { lpState[1] += alpha * (xR - lpState[1]); hbR = xR - lpState[1]; }
        else     hbR = hbL;

        const float side  = juce::jmax(std::abs(hbL), std::abs(hbR));
        const float lvlDb = 20.0f * std::log10(juce::jmax(side, 1.0e-6f));
        const float over  = lvlDb - threshold;
        const float grDb  = builtin::softKneeReductionDb(over, slope, kKneeDb);

        const float desiredGainDb = -grDb;
        const float coef = (desiredGainDb < gainDb) ? atkCoef : relCoef;
        gainDb = coef * gainDb + (1.0f - coef) * desiredGainDb;

        const float g = juce::Decibels::decibelsToGain(gainDb);   // 0..1
        // 高域成分を (1-g) だけ差し引く。g=1 (サ行なし) なら out = x で完全素通り。
        L[i] = xL - (1.0f - g) * hbL;
        if (R) R[i] = xR - (1.0f - g) * hbR;

        if (-gainDb > maxReductionDb) maxReductionDb = -gainDb;
    }

    meterReductionDb.store(maxReductionDb, std::memory_order_relaxed);
}
