// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

#include "BuiltInKeroVoice.h"
#include "../../Localisation.h"
#include <cmath>

namespace
{
    int nextPow2(int v)
    {
        int p = 1;
        while (p < v) p <<= 1;
        return p;
    }

    const char* kKeyLabels[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };

    // スケールの構成音 (キーからの半音オフセット)
    const int kMajorPc[7]      = { 0, 2, 4, 5, 7, 9, 11 };
    const int kMinorPc[7]      = { 0, 2, 3, 5, 7, 8, 10 };
    const int kMajorPentaPc[5] = { 0, 2, 4, 7, 9 };
    const int kMinorPentaPc[5] = { 0, 3, 5, 7, 10 };

    constexpr float kYinThreshold     = 0.15f;   // CMNDF の第一極小しきい値
    constexpr float kVoicedMaxCmndf   = 0.35f;   // これを超える周期性は無声扱い
    constexpr float kSnapHysteresis   = 0.12f;   // ノート境界のヒステリシス (半音)
}

const char* BuiltInKeroVoice::keyLabel(int idx) { return kKeyLabels[juce::jlimit(0, 11, idx)]; }

bool BuiltInKeroVoice::isPitchClassInScale(int pc, int scaleType, int key)
{
    if (scaleType == Chromatic) return true;
    const int* tbl = kMajorPc;
    int n = 7;
    switch (scaleType)
    {
        case Minor:      tbl = kMinorPc;      n = 7; break;
        case MajorPenta: tbl = kMajorPentaPc; n = 5; break;
        case MinorPenta: tbl = kMinorPentaPc; n = 5; break;
        default: break;
    }
    int rel = (pc - key) % 12;
    if (rel < 0) rel += 12;
    for (int i = 0; i < n; ++i)
        if (tbl[i] == rel) return true;
    return false;
}

BuiltInKeroVoice::BuiltInKeroVoice()
{
    addParam({ "scale",   u8"スケール", "",   0.0f,  4.0f,   0.0f, 1.0f });   // 0=半音階/1=メジャー/2=マイナー/3=メジャーペンタ/4=マイナーペンタ (UI はボタン)
    addParam({ "key",     u8"キー",     "",   0.0f, 11.0f,   0.0f, 1.0f });   // C..B (UI はボタン)
    addParam({ "speedMs", u8"スピード", "ms", 0.0f, 200.0f,  0.0f, 0.5f });   // 0 = 即スナップ (ケロケロ)
    addParam({ "mix",     u8"ミックス", "%",  0.0f, 100.0f, 100.0f, 1.0f });
    addParam({ "amount",  u8"補正量",   "%",  0.0f, 100.0f, 100.0f, 1.0f });  // スナップへの寄せ具合 (100=完全)
    addParam({ "transpose", u8"トランスポーズ", "st", -12.0f, 12.0f, 0.0f, 1.0f });  // 補正後の移調
    addParam({ "sens",    u8"感度",     "dB", -70.0f, -30.0f, -55.0f, 1.0f }); // 補正が効き始める入力レベル

    //          scale key  speed  mix     amount transpose sens
    addPreset(u8"ケロケロ",     { 0.0f, 0.0f,   0.0f, 100.0f, 100.0f,   0.0f, -55.0f });
    addPreset(u8"しっかり補正", { 0.0f, 0.0f,  35.0f, 100.0f, 100.0f,   0.0f, -55.0f });
    addPreset(u8"ナチュラル",   { 0.0f, 0.0f, 110.0f, 100.0f,  80.0f,   0.0f, -55.0f });
    addPreset(u8"オクターブ下", { 0.0f, 0.0f,   0.0f, 100.0f, 100.0f, -12.0f, -55.0f });
}

const juce::String BuiltInKeroVoice::getName() const { return tr(u8"ケロケロボイス"); }

void BuiltInKeroVoice::prepareToPlay(double sampleRate, int)
{
    sr = sampleRate > 0.0 ? sampleRate : 48000.0;

    // シフタのリング: 最大遅延 (2.2 周期 @60Hz) + フェード + 余裕が収まる 2 の冪
    const int need = (int) std::ceil(sr * 0.12) + 64;
    const int len  = nextPow2(need);
    bufMask = len - 1;
    ringL.assign((size_t) len, 0.0f);
    ringR.assign((size_t) len, 0.0f);
    writeCount = 0;
    curPeriod  = (float) (sr / 220.0);
    readPos    = -(kMinDelay + (double) curPeriod * 0.5);   // 初期遅延 ≈ 半周期 (数 ms)
    curRatio = targetRatio = 1.0;
    fadeRemain = 0;
    fadeLen    = 1;

    // 検出: 1/4 デシメーション + YIN。積分窓 = 最大ラグ (60Hz の 1 周期)
    const double decSr = sr / (double) kDecim;
    minLagDec = juce::jmax(2, (int) std::floor(decSr / kMaxHz));
    maxLagDec = juce::jmax(minLagDec + 2, (int) std::ceil(decSr / kMinHz));
    winDec    = maxLagDec;
    decMask   = nextPow2(winDec + maxLagDec + 4) - 1;
    decRing.assign((size_t) (decMask + 1), 0.0f);
    decCount = 0;
    decimPhase = 0;
    detScratch.assign((size_t) (winDec + maxLagDec), 0.0f);
    cmndf.assign((size_t) (maxLagDec + 1), 1.0f);
    detLp = 0.0f;
    // デシメーション前の一極ローパス (~2.5kHz): 折り返しを抑えつつ基音は素通し
    detLpCoef = (float) juce::jlimit(0.0, 0.999,
        1.0 - std::exp(-2.0 * juce::MathConstants<double>::pi * 2500.0 / sr));
    detInterval = juce::jmax(64, (int) (sr * 0.005));
    detCounter = 0;
    lvl = 0.0f;
    voiced = false;
    lastTargetMidi = -1.0f;
    setUnvoiced(1.0);
}

void BuiltInKeroVoice::setUnvoiced(double transposeRatio)
{
    voiced = false;
    targetRatio = transposeRatio;   // 無声区間もトランスポーズは維持 (ロボ声/移調用途で子音が浮かない)
    lastTargetMidi = -1.0f;
    uiInMidi.store(-1.0f, std::memory_order_relaxed);
    uiOutMidi.store(-1.0f, std::memory_order_relaxed);
    uiInHz.store(0.0f, std::memory_order_relaxed);
}

float BuiltInKeroVoice::snapToScale(float midiF, int scaleType, int key)
{
    float cand;
    if (scaleType == Chromatic)
    {
        cand = (float) juce::roundToInt(midiF);
    }
    else
    {
        const int base = (int) std::floor(midiF);
        float bestDist = 1.0e9f;
        cand = (float) base;
        for (int m = base - 6; m <= base + 7; ++m)
        {
            int pc = m % 12; if (pc < 0) pc += 12;
            if (! isPitchClassInScale(pc, scaleType, key)) continue;
            const float d = std::abs(midiF - (float) m);
            if (d < bestDist) { bestDist = d; cand = (float) m; }
        }
    }

    // ヒステリシス: 直前のノートと差が僅かなら飛び移らない (境界のばたつき防止)
    if (lastTargetMidi >= 0.0f && cand != lastTargetMidi)
    {
        int lastPc = (int) lastTargetMidi % 12; if (lastPc < 0) lastPc += 12;
        if (isPitchClassInScale(lastPc, scaleType, key)
            && std::abs(midiF - lastTargetMidi) - std::abs(midiF - cand) < kSnapHysteresis)
            cand = lastTargetMidi;
    }
    return cand;
}

void BuiltInKeroVoice::runDetection(int scaleType, int key, float amount, float transpose, float sensDb)
{
    const int m = winDec + maxLagDec;
    if (decCount < (std::int64_t) m) return;

    const double transposeRatio = std::pow(2.0, (double) transpose / 12.0);

    // レベルゲート (感度): 静かな入力 (ブレス/ノイズ床) は補正しない
    const float gate = juce::Decibels::decibelsToGain(sensDb);
    if (lvl < gate * gate) { setUnvoiced(transposeRatio); return; }

    const std::int64_t start = decCount - (std::int64_t) m;
    for (int j = 0; j < m; ++j)
        detScratch[(size_t) j] = decRing[(size_t) ((start + (std::int64_t) j) & (std::int64_t) decMask)];

    // YIN: 差分関数 d(τ) → 累積平均正規化 (CMNDF)
    const float* x = detScratch.data();
    double cum = 0.0;
    cmndf[0] = 1.0f;
    for (int tau = 1; tau <= maxLagDec; ++tau)
    {
        double s = 0.0;
        for (int i = 0; i < winDec; ++i)
        {
            const double d = (double) x[i] - (double) x[i + tau];
            s += d * d;
        }
        cum += s;
        cmndf[(size_t) tau] = cum > 0.0 ? (float) (s * (double) tau / cum) : 1.0f;
    }

    // 第一極小 (しきい値下) を探し、無ければ大域最小
    int best = -1;
    for (int tau = minLagDec; tau <= maxLagDec; ++tau)
    {
        if (cmndf[(size_t) tau] < kYinThreshold)
        {
            while (tau + 1 <= maxLagDec && cmndf[(size_t) (tau + 1)] < cmndf[(size_t) tau]) ++tau;
            best = tau;
            break;
        }
    }
    if (best < 0)
    {
        float bv = 1.0e9f;
        for (int tau = minLagDec; tau <= maxLagDec; ++tau)
            if (cmndf[(size_t) tau] < bv) { bv = cmndf[(size_t) tau]; best = tau; }
    }
    if (best < minLagDec || cmndf[(size_t) best] > kVoicedMaxCmndf) { setUnvoiced(transposeRatio); return; }

    // 放物線補間で小数ラグへ
    double tauF = (double) best;
    if (best > minLagDec && best < maxLagDec)
    {
        const double a = cmndf[(size_t) (best - 1)], b = cmndf[(size_t) best], c = cmndf[(size_t) (best + 1)];
        const double den = a - 2.0 * b + c;
        if (std::abs(den) > 1.0e-12)
            tauF += juce::jlimit(-0.5, 0.5, 0.5 * (a - c) / den);
    }

    const double decSr = sr / (double) kDecim;
    const double freq = decSr / tauF;
    if (! std::isfinite(freq) || freq < kMinHz * 0.9 || freq > kMaxHz * 1.1) { setUnvoiced(transposeRatio); return; }

    voiced = true;
    curPeriod = (float) juce::jlimit(sr / (double) kMaxHz, sr / (double) kMinHz, sr / freq);

    const float midiF   = 69.0f + 12.0f * (float) (std::log2(freq / 440.0));
    const float snapped = snapToScale(midiF, scaleType, key);
    // 補正量でスナップへの寄せ具合を薄め、その後トランスポーズ (ヒステリシスはスナップ値で判定)
    const float target  = midiF + (snapped - midiF) * amount + transpose;
    targetRatio = juce::jlimit(0.25, 4.0, std::pow(2.0, (double) (target - midiF) / 12.0));
    lastTargetMidi = snapped;

    uiInMidi.store(midiF, std::memory_order_relaxed);
    uiOutMidi.store(target, std::memory_order_relaxed);
    uiInHz.store((float) freq, std::memory_order_relaxed);
}

void BuiltInKeroVoice::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const int numCh = juce::jmin(2, buffer.getNumChannels());
    const int n     = buffer.getNumSamples();
    if (numCh <= 0 || bufMask <= 0 || (int) ringL.size() != bufMask + 1) return;

    const int   scaleType = juce::jlimit(0, (int) NumScales - 1, (int) std::lround(getP(Scale)));
    const int   key       = juce::jlimit(0, 11, (int) std::lround(getP(Key)));
    const float speedMs   = getP(Speed);
    const float mix       = juce::jlimit(0.0f, 1.0f, getP(Mix) * 0.01f);
    const float amount    = juce::jlimit(0.0f, 1.0f, getP(Amount) * 0.01f);
    const float transpose = juce::jlimit(-12.0f, 12.0f, getP(Transpose));
    const float sensDb    = getP(Sens);

    // リトゥーンの平滑化係数 (speed 0 は即スナップ = ケロケロ)
    const double retCoef = speedMs < 1.0f ? 0.0 : std::exp(-1.0 / (sr * (double) speedMs * 0.001));
    const float  lvlCoef = (float) std::exp(-1.0 / (sr * 0.010));

    float* L = buffer.getWritePointer(0);
    float* R = numCh > 1 ? buffer.getWritePointer(1) : nullptr;

    if (! std::isfinite(curRatio) || ! std::isfinite((float) readPos))
    {
        curRatio = targetRatio = 1.0;
        readPos = (double) writeCount - (kMinDelay + (double) curPeriod * 0.5);
        fadeRemain = 0;
    }

    for (int i = 0; i < n; ++i)
    {
        // 入力サニタイズ (NaN/Inf はリング/フィルタ状態を汚染するため 0 に潰す)
        float xL = L[i]; if (! std::isfinite(xL)) xL = 0.0f;
        float xR = R ? R[i] : xL; if (! std::isfinite(xR)) xR = 0.0f;

        ringL[(size_t) (writeCount & (std::int64_t) bufMask)] = xL;
        ringR[(size_t) (writeCount & (std::int64_t) bufMask)] = xR;
        ++writeCount;

        const float mono = 0.5f * (xL + xR);
        lvl = lvlCoef * lvl + (1.0f - lvlCoef) * mono * mono;

        // 検出用: 一極ローパス → 1/4 デシメーション
        detLp += detLpCoef * (mono - detLp);
        if (++decimPhase >= kDecim)
        {
            decimPhase = 0;
            decRing[(size_t) (decCount & (std::int64_t) decMask)] = detLp;
            ++decCount;
        }
        if (++detCounter >= detInterval)
        {
            detCounter = 0;
            runDetection(scaleType, key, amount, transpose, sensDb);
        }

        // リトゥーン (比率の平滑化)
        if (retCoef <= 0.0) curRatio = targetRatio;
        else                curRatio = retCoef * curRatio + (1.0 - retCoef) * targetRatio;

        // スプライス管理: 遅延が範囲を外れたら検出周期ぶんジャンプ (ピッチ同期 = 波形が繋がる)
        const double T = (double) curPeriod;
        const double delay = (double) writeCount - readPos;
        if (delay < kMinDelay || delay > kMinDelay + 2.2 * T)
        {
            fadeOldPos = readPos;
            readPos += (delay < kMinDelay) ? -T : T;
            fadeLen    = juce::jlimit(32, 256, (int) (T * 0.4));
            fadeRemain = fadeLen;
        }

        float wetL = readCubic(ringL, readPos);
        float wetR = R ? readCubic(ringR, readPos) : wetL;
        if (fadeRemain > 0)
        {
            const float w = (float) fadeRemain / (float) fadeLen;   // 1 → 0 (旧ポインタの重み)
            wetL = wetL * (1.0f - w) + readCubic(ringL, fadeOldPos) * w;
            if (R) wetR = wetR * (1.0f - w) + readCubic(ringR, fadeOldPos) * w;
            fadeOldPos += curRatio;
            --fadeRemain;
        }
        readPos += curRatio;

        if (mix <= 0.0f)
        {
            L[i] = xL;
            if (R) R[i] = xR;
        }
        else
        {
            L[i] = xL * (1.0f - mix) + wetL * mix;
            if (R) R[i] = xR * (1.0f - mix) + wetR * mix;
        }
    }
}
