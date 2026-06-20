// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

#include "BuiltInEQ.h"
#include "../../Localisation.h"

BuiltInEQ::BuiltInEQ()
{
    // id は言語非依存で固定 (シリアライズキー)。label は tr() で英訳される表示名。
    addParam({ "hpfHz", u8"ローカット", "Hz",   20.0f, 400.0f,   80.0f, 0.35f });
    addParam({ "lowHz", u8"低域 Hz",   "Hz",   60.0f, 500.0f,  200.0f, 0.5f  });
    addParam({ "lowDb", u8"低域",       "dB",  -12.0f,  12.0f,    0.0f, 1.0f  });
    addParam({ "midHz", u8"中域 Hz",   "Hz",  300.0f, 6000.0f, 2800.0f, 0.4f  });
    addParam({ "midDb", u8"中域",       "dB",  -12.0f,  12.0f,    0.0f, 1.0f  });
    addParam({ "airHz", u8"高域 Hz",   "Hz", 3000.0f, 16000.0f, 10000.0f, 0.5f });
    addParam({ "airDb", u8"高域",       "dB",  -12.0f,  12.0f,    0.0f, 1.0f  });

    //          hpf   lowHz lowDb  midHz  midDb  airHz   airDb
    addPreset(u8"フラット",      { 20.0f, 200.0f,  0.0f, 2800.0f,  0.0f, 10000.0f,  0.0f });
    addPreset(u8"ボーカル明瞭",  { 90.0f, 200.0f, -1.0f, 3000.0f,  3.0f, 12000.0f,  2.5f });
    addPreset(u8"低域すっきり",  { 120.0f, 180.0f, -3.0f, 2500.0f,  1.0f, 11000.0f,  1.0f });
    addPreset(u8"温かみ",        { 80.0f, 180.0f,  2.0f, 1500.0f,  0.0f,  9000.0f, -1.5f });
    addPreset(u8"ラジオ風",      { 220.0f, 200.0f, -6.0f, 1800.0f,  4.0f,  8000.0f, -5.0f });
}

const juce::String BuiltInEQ::getName() const { return tr(u8"内蔵EQ"); }

void BuiltInEQ::prepareToPlay(double sampleRate, int)
{
    sr = sampleRate > 0.0 ? sampleRate : 48000.0;
    hpf.reset(); low.reset(); mid.reset(); air.reset();
}

void BuiltInEQ::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    // NaN/Inf がフィルタ状態に入ると以降ずっと出力を汚染するため、混入を検知したら全段リセット
    // (ScopedNoDenormals は NaN を救わない)。代表として各段の z1[0] を点検する。
    if (! (std::isfinite(hpf.z1[0]) && std::isfinite(low.z1[0])
        && std::isfinite(mid.z1[0]) && std::isfinite(air.z1[0])))
    {
        hpf.reset(); low.reset(); mid.reset(); air.reset();
    }

    // 係数はブロック先頭で 1 回算出 (ノブ操作は低頻度)。
    hpf.setHighpass (sr, getP(HpfHz), 0.707);
    low.setLowShelf (sr, getP(LowHz), getP(LowDb), 0.9);
    mid.setPeak     (sr, getP(MidHz), getP(MidDb), 0.9);
    air.setHighShelf(sr, getP(AirHz), getP(AirDb), 0.9);

    const int numCh = juce::jmin(2, buffer.getNumChannels());
    const int n     = buffer.getNumSamples();

    for (int ch = 0; ch < numCh; ++ch)
    {
        float* d = buffer.getWritePointer(ch);
        for (int i = 0; i < n; ++i)
        {
            float x = d[i];
            x = hpf.process(ch, x);
            x = low.process(ch, x);
            x = mid.process(ch, x);
            x = air.process(ch, x);
            d[i] = x;
        }
    }
}
