// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

#include "BuiltInMaximizer.h"
#include "../../Localisation.h"
#include <cmath>

BuiltInMaximizer::BuiltInMaximizer()
{
    addParam({ "driveDb",   u8"ドライブ",   "dB",  0.0f,  24.0f,   3.0f, 1.0f  });
    addParam({ "ceilingDb", u8"シーリング", "dB", -12.0f,  0.0f,  -0.5f, 1.0f  });
    // リリースは UI に出さず最速 (= 最小値) 固定。状態互換のためパラメータ自体は残す。
    addParam({ "releaseMs", u8"リリース",   "ms",  1.0f, 1000.0f,   1.0f, 0.35f });
}

const juce::String BuiltInMaximizer::getName() const { return tr(u8"マキシマイザー"); }

void BuiltInMaximizer::prepareToPlay(double sampleRate, int)
{
    sr = sampleRate > 0.0 ? sampleRate : 48000.0;
    lookahead = juce::jmax(1, (int) std::round(0.003 * sr));   // 3ms 先読み
    delayL.assign((size_t) lookahead, 0.0f);
    delayR.assign((size_t) lookahead, 0.0f);
    writePos = 0;
    holdCounter = 0;
    gain = 1.0f;
    meterReductionDb.store(0.0f, std::memory_order_relaxed);

    // PDC: 再生時は PluginChain の getTotalLatencySamples 経由でこの遅延が他トラックと整合される。
    setLatencySamples(lookahead);
}

void BuiltInMaximizer::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const int numCh = juce::jmin(2, buffer.getNumChannels());
    const int n     = buffer.getNumSamples();
    if (numCh <= 0 || lookahead <= 0 || (int) delayL.size() != lookahead) return;

    const float drive   = juce::Decibels::decibelsToGain(getP(DriveDb));
    const float ceiling = juce::Decibels::decibelsToGain(getP(CeilingDb));
    const float relMs   = getP(ReleaseMs);

    // 先読みでピークが届くまでにゲインを下げきるため、アタックの時定数は lookahead 内で
    // ほぼ収束するよう短く取る (tau ≈ lookahead/3 で約 3τ ≈ lookahead)。戻りはリリース時定数。
    const double atkTau = juce::jmax(1.0, (double) lookahead / 3.0) / sr;
    const float  atkCoef = (float) std::exp(-1.0 / (sr * atkTau));
    const float  relCoef = (float) std::exp(-1.0 / (sr * juce::jmax(0.0001, relMs * 0.001)));

    float* L = buffer.getWritePointer(0);
    float* R = numCh > 1 ? buffer.getWritePointer(1) : nullptr;
    float* dL = delayL.data();
    float* dR = delayR.data();

    if (! std::isfinite(gain)) gain = 1.0f;   // NaN 混入後も復帰 (コンプ/リバーブと同じ方針)

    float maxReductionDb = 0.0f;
    float maxIn = 0.0f, maxOut = 0.0f;

    for (int i = 0; i < n; ++i)
    {
        float inL = L[i] * drive;
        float inR = R ? R[i] * drive : inL;
        if (! std::isfinite(inL)) inL = 0.0f;
        if (! std::isfinite(inR)) inR = 0.0f;

        const float peak = juce::jmax(std::abs(inL), std::abs(inR));
        if (peak > maxIn) maxIn = peak;

        // このサンプル (= 先読み位置) が出力されるまでにシーリングへ収めるための目標ゲイン。
        // 減衰へはアタックで素早く追従し、その都度ホールドを lookahead サンプル充填する。
        // ホールド中はリリースさせない: そうしないと、最速リリース (1ms) が先読み (3ms) より
        // 速いと、減衰させたピークが遅延ラインから出る前にゲインが戻ってしまい、先読みが効かず
        // ハードクリップ任せ (= トランジェントで歪む) になる。ピークが通過しきってから戻す。
        const float desired = peak > ceiling ? ceiling / peak : 1.0f;
        if (desired < gain)
        {
            gain = atkCoef * gain + (1.0f - atkCoef) * desired;
            holdCounter = lookahead;
        }
        else if (holdCounter > 0)
        {
            --holdCounter;   // 減衰を保持 (遅延中のピークがまだ低ゲインを浴びる)
        }
        else
        {
            gain = relCoef * gain + (1.0f - relCoef) * desired;
        }

        // 遅延ラインから lookahead 分前のサンプルを読み出し、現在の (予測済み) ゲインを掛ける
        const float outLraw = dL[(size_t) writePos] * gain;
        const float outRraw = dR[(size_t) writePos] * gain;
        dL[(size_t) writePos] = inL;
        if (R) dR[(size_t) writePos] = inR;
        if (++writePos >= lookahead) writePos = 0;

        // 安全網: 残留オーバーシュートをシーリングへハードクリップ (真のブリックウォール化)
        const float outL = juce::jlimit(-ceiling, ceiling, outLraw);
        const float outR = juce::jlimit(-ceiling, ceiling, outRraw);

        L[i] = outL;
        if (R) R[i] = outR;

        const float outAbs = juce::jmax(std::abs(outL), R ? std::abs(outR) : 0.0f);
        if (outAbs > maxOut) maxOut = outAbs;
        const float grDb = gain < 1.0f ? -20.0f * std::log10(juce::jmax(gain, 1.0e-6f)) : 0.0f;
        if (grDb > maxReductionDb) maxReductionDb = grDb;
    }

    auto toDb = [] (float lin) { return lin > 1.0e-5f ? 20.0f * std::log10(lin) : -100.0f; };
    // IN メータ/スコープは**ドライブ適用後**のレベル (= リミッターに入る信号)。シーリング線に
    // 押し当たって GR がかかる様子を見せるための意図的な設計 (素の入力レベルではない)。
    meterInputDb.store (toDb(maxIn),  std::memory_order_relaxed);
    meterOutputDb.store(toDb(maxOut), std::memory_order_relaxed);
    meterReductionDb.store(maxReductionDb, std::memory_order_relaxed);  // 平滑化は UI 側
}
