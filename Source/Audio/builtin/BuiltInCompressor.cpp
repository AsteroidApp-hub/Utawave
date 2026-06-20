// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

#include "BuiltInCompressor.h"
#include "GainDynamics.h"
#include "../../Localisation.h"
#include <cmath>

BuiltInCompressor::BuiltInCompressor()
{
    addParam({ "thresholdDb", u8"スレッショルド", "dB", -48.0f,   0.0f, -18.0f, 1.0f  });
    addParam({ "ratio",       u8"レシオ",         ":1",   1.0f,  20.0f,   3.0f, 0.4f  });
    addParam({ "attackMs",    u8"アタック",       "ms",   0.1f, 100.0f,  10.0f, 0.35f });
    addParam({ "releaseMs",   u8"リリース",       "ms",  10.0f, 1000.0f, 120.0f, 0.35f });
    addParam({ "makeupDb",    u8"メイクアップ",   "dB",   0.0f,  24.0f,   0.0f, 1.0f  });
    addParam({ "autoGain",    u8"オートゲイン",   "",     0.0f,   1.0f,   0.0f, 1.0f  });

    //          thr     ratio atk    rel    makeup autoGain
    addPreset(u8"オフ",          { 0.0f,   1.0f, 10.0f, 120.0f, 0.0f, 0.0f });
    addPreset(u8"ボーカル軽め",  { -18.0f, 2.5f, 15.0f, 150.0f, 3.0f, 0.0f });
    addPreset(u8"ボーカルしっかり", { -24.0f, 4.0f,  5.0f, 100.0f, 6.0f, 0.0f });
    addPreset(u8"ナレーション",  { -20.0f, 3.0f, 10.0f, 200.0f, 4.0f, 0.0f });
    addPreset(u8"リミッター風",  { -6.0f, 12.0f,  1.0f,  80.0f, 0.0f, 0.0f });
}

const juce::String BuiltInCompressor::getName() const { return tr(u8"内蔵コンプ"); }

float BuiltInCompressor::currentMakeupDb() const
{
    if (getP(AutoGain) > 0.5f)
    {
        // しきい値/レシオから半量補正の自動メイクアップ (0dBFS 基準の減衰量の半分)。静的ゲイン=遅延無し。
        const float over  = juce::jmax(0.0f, -getP(ThresholdDb));
        const float slope = 1.0f - 1.0f / juce::jmax(1.0f, getP(Ratio));
        return juce::jlimit(0.0f, 24.0f, over * slope * 0.5f);
    }
    return getP(MakeupDb);
}

void BuiltInCompressor::prepareToPlay(double sampleRate, int)
{
    sr = sampleRate > 0.0 ? sampleRate : 48000.0;
    gainDb = 0.0f;
    meterReductionDb.store(0.0f, std::memory_order_relaxed);
}

void BuiltInCompressor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const float threshold = getP(ThresholdDb);
    const float ratio     = juce::jmax(1.0f, getP(Ratio));
    const float atkMs     = getP(AttackMs);
    const float relMs     = getP(ReleaseMs);
    const float makeup    = currentMakeupDb();

    // 1 サンプル平滑化係数 (時定数 t秒 → exp(-1/(sr*t)))
    const float atkCoef = (float) std::exp(-1.0 / (sr * juce::jmax(0.0001, atkMs * 0.001)));
    const float relCoef = (float) std::exp(-1.0 / (sr * juce::jmax(0.0001, relMs * 0.001)));
    const float slope   = 1.0f - 1.0f / ratio;       // 圧縮の傾き

    const int numCh = juce::jmin(2, buffer.getNumChannels());
    const int n     = buffer.getNumSamples();
    float* L = buffer.getWritePointer(0);
    float* R = numCh > 1 ? buffer.getWritePointer(1) : nullptr;

    // gainDb はブロック跨ぎで保持する状態。NaN/Inf が一度混入すると以降ずっと出力を
    // 汚染するため、ブロック先頭で有限性を点検して復帰させる (ScopedNoDenormals は NaN を救わない)。
    if (! std::isfinite(gainDb)) gainDb = 0.0f;

    float maxReductionDb = 0.0f;

    for (int i = 0; i < n; ++i)
    {
        const float l = L[i];
        const float r = R ? R[i] : l;
        float side = juce::jmax(std::abs(l), std::abs(r));
        if (! std::isfinite(side)) side = 0.0f;   // NaN/Inf 入力でエンベロープを壊さない

        const float lvlDb = 20.0f * std::log10(juce::jmax(side, 1.0e-6f));
        const float over  = lvlDb - threshold;
        const float grDb  = builtin::softKneeReductionDb(over, slope, kKneeDb);

        const float desiredGainDb = -grDb;
        // より深い減衰へはアタック、戻りはリリースの時定数で平滑化
        const float coef = (desiredGainDb < gainDb) ? atkCoef : relCoef;
        gainDb = coef * gainDb + (1.0f - coef) * desiredGainDb;

        const float g = juce::Decibels::decibelsToGain(gainDb + makeup);
        L[i] = l * g;
        if (R) R[i] = r * g;

        if (-gainDb > maxReductionDb) maxReductionDb = -gainDb;
    }

    // メータは**生のブロック内最大減衰量**だけを公開する。バリスティクス (立ち上がり即・
    // 立ち下がりゆっくり) はブロック長非依存にするため UI 側 (30Hz timer) で 1 回だけ掛ける
    // (ここで平滑化するとブロック長でメータの落下速度が変わり二重平滑化にもなる)。
    meterReductionDb.store(maxReductionDb, std::memory_order_relaxed);
}
