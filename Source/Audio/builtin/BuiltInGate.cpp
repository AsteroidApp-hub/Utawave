// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

#include "BuiltInGate.h"
#include "../../Localisation.h"
#include <cmath>

BuiltInGate::BuiltInGate()
{
    addParam({ "thresholdDb", u8"スレッショルド", "dB", -80.0f,    0.0f, -45.0f, 1.0f  });
    addParam({ "rangeDb",     u8"レンジ",         "dB",   0.0f,   80.0f,  60.0f, 1.0f  });
    addParam({ "attackMs",    u8"アタック",       "ms",   0.1f,   50.0f,   3.0f, 0.35f });
    addParam({ "holdMs",      u8"ホールド",       "ms",   0.0f, 1000.0f, 200.0f, 0.5f  });
    addParam({ "releaseMs",   u8"リリース",       "ms",   5.0f, 1000.0f, 250.0f, 0.35f });

    //          thr     range  atk    hold    rel
    addPreset(u8"オフ",       { -80.0f,  0.0f,  3.0f, 200.0f, 250.0f });
    addPreset(u8"配信ノイズ", { -45.0f, 60.0f,  3.0f, 200.0f, 250.0f });
    addPreset(u8"ボーカル",   { -50.0f, 40.0f,  1.0f, 150.0f, 200.0f });
    addPreset(u8"しっかり",   { -40.0f, 80.0f,  1.0f,  80.0f, 120.0f });
    addPreset(u8"ゆるめ",     { -55.0f, 30.0f, 10.0f, 300.0f, 400.0f });
}

const juce::String BuiltInGate::getName() const { return tr(u8"ゲート"); }

void BuiltInGate::prepareToPlay(double sampleRate, int)
{
    sr = sampleRate > 0.0 ? sampleRate : 48000.0;
    gainDb = 0.0f;
    holdCounter = 0;
    open = false;
    meterReductionDb.store(0.0f, std::memory_order_relaxed);
    meterInputDb.store(-100.0f, std::memory_order_relaxed);
    gateOpen.store(false, std::memory_order_relaxed);
}

void BuiltInGate::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const float threshold = getP(ThresholdDb);
    const float range     = getP(RangeDb);
    const float atkMs     = getP(AttackMs);
    const float holdMs    = getP(HoldMs);
    const float relMs     = getP(ReleaseMs);

    // 1 サンプル平滑化係数 (時定数 t 秒 → exp(-1/(sr*t)))
    const float atkCoef = (float) std::exp(-1.0 / (sr * juce::jmax(0.0001, atkMs * 0.001)));
    const float relCoef = (float) std::exp(-1.0 / (sr * juce::jmax(0.0001, relMs * 0.001)));
    const int   holdSamples = (int) (holdMs * 0.001f * (float) sr);
    const float floorDb = -range;   // 全閉時の目標ゲイン (range=0 なら 0dB=素通り)

    const int numCh = juce::jmin(2, buffer.getNumChannels());
    const int n     = buffer.getNumSamples();
    float* L = buffer.getWritePointer(0);
    float* R = numCh > 1 ? buffer.getWritePointer(1) : nullptr;

    // gainDb はブロック跨ぎの状態。NaN/Inf が混入すると以降ずっと出力を汚すため点検して復帰。
    if (! std::isfinite(gainDb)) gainDb = floorDb;

    float maxReductionDb = 0.0f;
    float maxIn = 0.0f;

    for (int i = 0; i < n; ++i)
    {
        const float l = L[i];
        const float r = R ? R[i] : l;
        float side = juce::jmax(std::abs(l), std::abs(r));
        if (! std::isfinite(side)) side = 0.0f;   // NaN/Inf 入力でエンベロープを壊さない
        if (side > maxIn) maxIn = side;

        const float lvlDb = 20.0f * std::log10(juce::jmax(side, 1.0e-7f));

        // 開閉判定: スレッショルドを超えたら即開き hold をリセット。下回ったら hold を消化し、
        // 尽きたら閉じる。超えるたびに hold が再充填されるので、しきい値近傍のばたつきは出ない。
        if (lvlDb > threshold) { open = true; holdCounter = holdSamples; }
        else if (holdCounter > 0) { --holdCounter; }
        else { open = false; }

        const float desiredGainDb = open ? 0.0f : floorDb;
        // 開く (ゲイン上昇) = アタック、閉じる (ゲイン下降) = リリースの時定数で平滑化
        const float coef = (desiredGainDb > gainDb) ? atkCoef : relCoef;
        gainDb = coef * gainDb + (1.0f - coef) * desiredGainDb;

        const float g = juce::Decibels::decibelsToGain(gainDb);
        L[i] = l * g;
        if (R) R[i] = r * g;

        if (-gainDb > maxReductionDb) maxReductionDb = -gainDb;
    }

    auto toDb = [] (float lin) { return lin > 1.0e-5f ? 20.0f * std::log10(lin) : -100.0f; };
    meterInputDb.store(toDb(maxIn), std::memory_order_relaxed);
    gateOpen.store(open, std::memory_order_relaxed);
    // メータは生のブロック内最大減衰量のみ公開 (バリスティクスは UI 側 30Hz で 1 回掛ける)。
    meterReductionDb.store(maxReductionDb, std::memory_order_relaxed);
}
