// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

#include "BuiltInDelay.h"
#include "../../Localisation.h"
#include <cmath>

namespace
{
    // テンポ同期の音価 (四分音符 = 1.0 拍)。ラベルは ASCII (UI のボタン/表示で共用)。
    struct Division { const char* label; double beats; };
    const Division kDivisions[] =
    {
        { "1/4",   1.0      },
        { "1/4.",  1.5      },
        { "1/8.",  0.75     },
        { "1/8",   0.5      },
        { "1/8T",  1.0 / 3.0 },
        { "1/16",  0.25     },
    };
    constexpr int kNumDivisions = (int) (sizeof(kDivisions) / sizeof(kDivisions[0]));
}

int         BuiltInDelay::numDivisions()           { return kNumDivisions; }
const char* BuiltInDelay::divisionLabel(int idx)   { return kDivisions[juce::jlimit(0, kNumDivisions - 1, idx)].label; }
double      BuiltInDelay::divisionBeats(int idx)    { return kDivisions[juce::jlimit(0, kNumDivisions - 1, idx)].beats; }

BuiltInDelay::BuiltInDelay()
{
    addParam({ "sync",     u8"同期",       "",   0.0f, 1.0f, 1.0f, 1.0f });   // トグル (UI はボタン)
    addParam({ "division", u8"音価",       "",   0.0f, (float) (kNumDivisions - 1), 3.0f, 1.0f }); // 既定 1/8
    addParam({ "timeMs",   u8"タイム",     "ms", 10.0f, 2000.0f, 350.0f, 0.35f });
    addParam({ "feedback", u8"フィードバック", "%", 0.0f, 95.0f, 35.0f, 1.0f });
    addParam({ "tone",     u8"トーン",     "",   0.0f, 1.0f, 0.5f, 1.0f });
    addParam({ "mix",      u8"ミックス",   "%",  0.0f, 100.0f, 28.0f, 1.0f });
    addParam({ "pingpong", u8"ピンポン",   "",   0.0f, 1.0f, 1.0f, 1.0f });   // トグル (UI はボタン)

    //          sync div   time   fb    tone  mix   ping
    addPreset(u8"1/8 ディレイ", { 1.0f, 3.0f, 350.0f, 32.0f, 0.50f, 25.0f, 1.0f });
    addPreset(u8"付点ディレイ", { 1.0f, 2.0f, 350.0f, 38.0f, 0.45f, 28.0f, 1.0f });
    addPreset(u8"スラップバック", { 0.0f, 3.0f, 110.0f,  8.0f, 0.70f, 35.0f, 0.0f });
    addPreset(u8"アンビ",       { 1.0f, 0.0f, 350.0f, 50.0f, 0.35f, 22.0f, 1.0f });
}

const juce::String BuiltInDelay::getName() const { return tr(u8"ディレイ"); }

void BuiltInDelay::prepareToPlay(double sampleRate, int)
{
    sr = sampleRate > 0.0 ? sampleRate : 48000.0;
    bufLen = (int) std::ceil(kMaxDelaySec * sr) + 4;
    bufL.assign((size_t) bufLen, 0.0f);
    bufR.assign((size_t) bufLen, 0.0f);
    writePos = 0;
    lpL = lpR = 0.0f;
    delaySmoothed = (float) (juce::jlimit(0.001, kMaxDelaySec, (double) getP(TimeMs) * 0.001) * sr);
}

void BuiltInDelay::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const int numCh = juce::jmin(2, buffer.getNumChannels());
    const int n     = buffer.getNumSamples();
    if (numCh <= 0 || bufLen <= 1 || (int) bufL.size() != bufLen) return;

    // ── ディレイ時間を解決 (テンポ同期 or フリー ms) ──
    double bpm = lastBpm;
    if (auto* ph = getPlayHead())
        if (auto pos = ph->getPosition())
            if (auto b = pos->getBpm())
                if (*b > 0.0) bpm = *b;
    lastBpm = bpm;

    double delaySec;
    if (getP(Sync) > 0.5f)
    {
        const int div = juce::jlimit(0, kNumDivisions - 1, (int) std::lround(getP(Division)));
        delaySec = (60.0 / bpm) * divisionBeats(div);
    }
    else
    {
        delaySec = (double) getP(TimeMs) * 0.001;
    }
    delaySec = juce::jlimit(0.001, kMaxDelaySec, delaySec);
    curDelaySec.store((float) delaySec, std::memory_order_relaxed);

    const float targetSamples = (float) juce::jmin((double) (bufLen - 2), delaySec * sr);

    const float fb       = juce::jlimit(0.0f, 0.95f, getP(Feedback) * 0.01f);
    const float toneG    = 0.06f + 0.94f * juce::jlimit(0.0f, 1.0f, getP(Tone));  // 1=明るい / 小=暗い
    const float mix      = juce::jlimit(0.0f, 1.0f, getP(Mix) * 0.01f);
    const bool  pingpong = getP(PingPong) > 0.5f;

    // 時間変更を平滑化してクリック/ジッパーを防ぐ (約 40ms)
    const float dCoef = (float) std::exp(-1.0 / (sr * 0.04));

    float* L = buffer.getWritePointer(0);
    float* R = numCh > 1 ? buffer.getWritePointer(1) : nullptr;
    float* dataL = bufL.data();
    float* dataR = bufR.data();

    if (! std::isfinite(delaySmoothed)) delaySmoothed = targetSamples;
    if (! std::isfinite(lpL)) lpL = 0.0f;
    if (! std::isfinite(lpR)) lpR = 0.0f;

    auto readInterp = [bufLen = this->bufLen] (const float* buf, float readPos) -> float
    {
        while (readPos < 0.0f)               readPos += (float) bufLen;
        while (readPos >= (float) bufLen)    readPos -= (float) bufLen;
        const int i0 = (int) readPos;
        const int i1 = (i0 + 1) % bufLen;
        const float frac = readPos - (float) i0;
        return buf[i0] + frac * (buf[i1] - buf[i0]);
    };

    float maxOut = 0.0f;

    for (int i = 0; i < n; ++i)
    {
        delaySmoothed = dCoef * delaySmoothed + (1.0f - dCoef) * targetSamples;

        const float readPos = (float) writePos - delaySmoothed;
        const float dL = readInterp(dataL, readPos);
        const float dR = readInterp(dataR, readPos);

        const float inL = L[i];
        const float inR = R ? R[i] : inL;

        // フィードバックへ書き込む信号 (ピンポンは L↔R を交差させて跳ねさせる)
        float wL, wR;
        if (pingpong)
        {
            const float inMono = 0.5f * (inL + inR);
            wL = inMono + dR * fb;   // 入力は L に入り、R の戻りが L へ
            wR = dL * fb;            // L の戻りが R へ → L,R,L,R と跳ねる
        }
        else
        {
            wL = inL + dL * fb;
            wR = inR + dR * fb;
        }

        // トーン (ダンピング): 一極ローパスでリピートが進むほど暗くなる
        lpL += toneG * (wL - lpL);
        lpR += toneG * (wR - lpR);
        if (! std::isfinite(lpL)) lpL = 0.0f;
        if (! std::isfinite(lpR)) lpR = 0.0f;

        dataL[(size_t) writePos] = lpL;
        dataR[(size_t) writePos] = lpR;
        if (++writePos >= bufLen) writePos = 0;

        // ドライは常にフル + ウェットをミックスで加算 (声を消さない)
        const float outL = inL + dL * mix;
        const float outR = inR + dR * mix;
        L[i] = outL;
        if (R) R[i] = outR;

        const float outAbs = juce::jmax(std::abs(outL), R ? std::abs(outR) : 0.0f);
        if (outAbs > maxOut) maxOut = outAbs;
    }

    meterOutputDb.store(maxOut > 1.0e-5f ? 20.0f * std::log10(maxOut) : -100.0f, std::memory_order_relaxed);
}
