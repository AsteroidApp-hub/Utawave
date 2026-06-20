// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

#include "BuiltInReverb.h"
#include "../../Localisation.h"
#include <cmath>

BuiltInReverb::BuiltInReverb()
{
    addParam({ "mixPct",  u8"ミックス",     "%", 0.0f, 100.0f, 18.0f, 1.0f });
    addParam({ "size",    u8"広さ",         "",  0.0f,   1.0f,  0.6f, 1.0f });
    addParam({ "damping", u8"ダンピング",   "",  0.0f,   1.0f,  0.4f, 1.0f });
    addParam({ "width",   u8"ステレオ幅",   "",  0.0f,   1.0f,  1.0f, 1.0f });

    //          mix    size  damp  width
    addPreset(u8"ボーカル", { 16.0f, 0.55f, 0.45f, 1.0f });
    addPreset(u8"プレート", { 22.0f, 0.82f, 0.22f, 1.0f });
    addPreset(u8"ルーム",   { 18.0f, 0.45f, 0.55f, 0.9f });
    addPreset(u8"ホール",   { 28.0f, 0.90f, 0.30f, 1.0f });
    addPreset(u8"薄く",     { 8.0f,  0.50f, 0.50f, 0.8f });
}

const juce::String BuiltInReverb::getName() const { return tr(u8"内蔵リバーブ"); }

void BuiltInReverb::prepareToPlay(double sampleRate, int blockSize)
{
    reverb.setSampleRate(sampleRate > 0.0 ? sampleRate : 48000.0);
    reverb.reset();
    wetBuf.setSize(2, juce::jmax(1, blockSize), false, false, true);
}

void BuiltInReverb::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    juce::Reverb::Parameters p;
    p.roomSize   = getP(Size);
    p.damping    = getP(Damping);
    p.width      = getP(Width);
    p.wetLevel   = 1.0f;   // ウェットのみ取り出し、ドライは手動で混ぜる
    p.dryLevel   = 0.0f;
    p.freezeMode = 0.0f;
    reverb.setParameters(p);

    const int n   = buffer.getNumSamples();
    const float mix = juce::jlimit(0.0f, 1.0f, getP(MixPct) * 0.01f);
    const float dryGain = 1.0f - mix;

    // wetBuf は prepareToPlay で blockSize 分だけ確保済み。**audio thread では再確保しない**
    // (CLAUDE.md: メモリ確保禁止)。万一 prepared 容量より大きいブロックが来ても、
    // 容量単位のチャンクに分けて処理し再確保を避ける。
    const int cap   = wetBuf.getNumSamples();
    const int numCh = juce::jmin(2, buffer.getNumChannels());
    if (cap <= 0 || numCh <= 0) return;

    for (int off = 0; off < n; off += cap)
    {
        const int len = juce::jmin(cap, n - off);

        // 入力をウェットバッファへコピー (ステレオ前提。モノなら L を複製)
        wetBuf.copyFrom(0, 0, buffer, 0, off, len);
        wetBuf.copyFrom(1, 0, buffer, buffer.getNumChannels() > 1 ? 1 : 0, off, len);

        reverb.processStereo(wetBuf.getWritePointer(0), wetBuf.getWritePointer(1), len);

        // NaN/Inf が juce::Reverb の内部ディレイに入ると以降ずっと残響を汚染するため、出力の
        // 非有限を検知したらリセットし、このチャンクのウェットは混ぜない (EQ/コンプと同じ方針)。
        if (! std::isfinite(wetBuf.getSample(0, 0)))
        {
            reverb.reset();
            continue;
        }

        for (int ch = 0; ch < numCh; ++ch)
        {
            float* d = buffer.getWritePointer(ch) + off;
            const float* w = wetBuf.getReadPointer(ch);
            for (int i = 0; i < len; ++i)
                d[i] = d[i] * dryGain + w[i] * mix;
        }
    }

    // UI メーター用の出力ピーク (dBFS)。平滑化は UI 側。
    float maxOut = 0.0f;
    for (int ch = 0; ch < numCh; ++ch)
    {
        const float* d = buffer.getReadPointer(ch);
        for (int i = 0; i < n; ++i) maxOut = juce::jmax(maxOut, std::abs(d[i]));
    }
    meterOutputDb.store(maxOut > 1.0e-5f ? 20.0f * std::log10(maxOut) : -100.0f, std::memory_order_relaxed);
}
