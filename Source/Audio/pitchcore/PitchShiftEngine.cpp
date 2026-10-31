// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave
//
// PitchCore — オフライン ピッチシフトエンジン (コア層・実装)
//
// アルゴリズムの全体像・数式・設計判断の経緯は README.md にまとめてある。要点:
//   ピッチシフト = フェーズボコーダ・タイムストレッチ (×r) → リサンプル (sr→sr/r)
//   - ピーク位相ロック (領域単位の複素回転) で縦コヒーレンス維持
//   - スペクトラルフラックスでトランジェント検出、リセットは立ち上がり領域のみ
//   - 時間写像 y = r·x − (r−1)·N/2 (フレーム中心対応) + 前後 N サンプルの無音パディング
//   - 解析ホップは double 累積、位相伝播は実整数差分 dt でドリフトゼロ

#include "PitchShiftEngine.h"
#include "RealFFT.h"
#include "CDSPResampler.h"  // r8brain (MIT, header-only)

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>

namespace
{
    constexpr double kPi = 3.14159265358979323846;

    // 合成ホップ = N / kOverlapDiv (75% オーバーラップ)
    constexpr int kOverlapDiv = 4;

    // トランジェント判定: フラックス > 中央値 * kFluxRatio かつ 直近最大 * kFluxFloor
    constexpr double kFluxRatio = 2.0;
    constexpr double kFluxFloor = 0.1;
    constexpr int    kFluxHistLen = 47;   // ~1 秒分 (Hs=2048 @48k で約 2 秒・十分)
    constexpr int    kMinResetGapFrames = 4;
    // トランジェントフレーム内で位相リセットする領域の条件 (ピーク振幅の立ち上がり比)
    constexpr float  kAttackRise = 2.0f;

    inline double princarg(double x) { return std::remainder(x, 2.0 * kPi); }

    // ── フェーズボコーダ・タイムストレッチ ──
    // in (パディング済み planar) を factor 倍の尺へ。全チャンネルをフレーム単位で同時処理。
    bool stretchBuffer(const std::vector<std::vector<float>>& in, double factor, int fftSize,
                       std::vector<std::vector<float>>& out, int& outLenOut,
                       const std::function<bool(double)>& onProgress,
                       double progressBegin, double progressEnd)
    {
        const int numCh = (int) in.size();
        const int inLen = (int) in[0].size();
        const int N     = fftSize;
        const int half  = N / 2;
        const int Hs    = N / kOverlapDiv;
        const double Ha = (double) Hs / factor;

        pitchcore::RealFFT fft(N);

        const int outLen   = (int) std::llround((double) inLen * factor);
        const int allocLen = outLen + N + Hs;
        out.assign((size_t) numCh, std::vector<float>((size_t) allocLen, 0.0f));
        outLenOut = outLen;

        // Hann (periodic)
        std::vector<float> window((size_t) N);
        for (int i = 0; i < N; ++i)
            window[(size_t) i] = (float)(0.5 - 0.5 * std::cos(2.0 * kPi * i / N));

        std::vector<float> norm((size_t) allocLen, 0.0f);

        // チャンネル別の状態
        std::vector<std::vector<float>>  frame((size_t) numCh);   // 時間領域フレーム (N)
        std::vector<std::vector<float>>  spec((size_t) numCh);    // スペクトル (N+2)
        std::vector<std::vector<float>>  mag((size_t) numCh);
        std::vector<std::vector<double>> phase((size_t) numCh);
        std::vector<std::vector<double>> prevPhase((size_t) numCh);
        std::vector<std::vector<double>> synthPhase((size_t) numCh);
        std::vector<std::vector<float>>  prevMag((size_t) numCh);
        for (int ch = 0; ch < numCh; ++ch)
        {
            frame[(size_t) ch].resize((size_t) N);
            spec[(size_t) ch].resize((size_t)(N + 2));
            mag[(size_t) ch].resize((size_t)(half + 1));
            phase[(size_t) ch].resize((size_t)(half + 1));
            prevPhase[(size_t) ch].assign((size_t)(half + 1), 0.0);
            synthPhase[(size_t) ch].assign((size_t)(half + 1), 0.0);
            prevMag[(size_t) ch].assign((size_t)(half + 1), 0.0f);
        }

        std::vector<double> fluxHist;
        fluxHist.reserve(kFluxHistLen);
        int fluxHistPos = 0;
        std::vector<double> fluxSorted;

        std::vector<int> peaks;
        peaks.reserve((size_t) half);

        const int numFrames = outLen / Hs + 1;
        int prevTa = 0;
        int lastResetFrame = -1000;

        for (int m = 0; m * Hs < outLen; ++m)
        {
            if (onProgress && (m & 31) == 0)
            {
                const double frac = (double) m / (double) std::max(1, numFrames);
                if (! onProgress(progressBegin + (progressEnd - progressBegin) * frac))
                    return false;
            }

            const int ts = m * Hs;
            const int ta = (int) std::llround((double) m * Ha);
            const int dt = std::max(1, ta - prevTa);

            // ── パス1: 全チャンネルの FFT → mag/phase、フラックス算出 ──
            double frameFlux = 0.0;
            for (int ch = 0; ch < numCh; ++ch)
            {
                auto& fr = frame[(size_t) ch];
                std::fill(fr.begin(), fr.end(), 0.0f);
                const float* src = in[(size_t) ch].data();
                const int copyBegin = std::max(0, -ta);
                const int copyEnd   = std::min(N, inLen - ta);
                for (int i = copyBegin; i < copyEnd; ++i)
                    fr[(size_t) i] = src[ta + i] * window[(size_t) i];

                auto& sp = spec[(size_t) ch];
                fft.forward(fr.data(), sp.data());

                auto& mg = mag[(size_t) ch];
                auto& ph = phase[(size_t) ch];
                auto& pm = prevMag[(size_t) ch];
                double flux = 0.0;
                for (int k = 0; k <= half; ++k)
                {
                    const double re = sp[(size_t)(2 * k)];
                    const double im = sp[(size_t)(2 * k + 1)];
                    const float  a  = (float) std::sqrt(re * re + im * im);
                    mg[(size_t) k] = a;
                    ph[(size_t) k] = std::atan2(im, re);
                    const double d = (double) a - (double) pm[(size_t) k];
                    if (d > 0.0) flux += d;
                }
                frameFlux = std::max(frameFlux, flux);
            }

            // ── トランジェント判定 (チャンネル共有) ──
            bool transient = (m == 0);
            if (! transient && (int) fluxHist.size() >= kMinResetGapFrames
                && m - lastResetFrame >= kMinResetGapFrames)
            {
                fluxSorted = fluxHist;
                const size_t mid = fluxSorted.size() / 2;
                std::nth_element(fluxSorted.begin(), fluxSorted.begin() + (long) mid,
                                 fluxSorted.end());
                const double med  = fluxSorted[mid];
                const double rmax = *std::max_element(fluxHist.begin(), fluxHist.end());
                if (frameFlux > kFluxRatio * med && frameFlux > kFluxFloor * rmax
                    && frameFlux > 1.0e-12)
                    transient = true;
            }
            if (transient && m > 0) lastResetFrame = m;

            if ((int) fluxHist.size() < kFluxHistLen)
                fluxHist.push_back(frameFlux);
            else
            {
                fluxHist[(size_t) fluxHistPos] = frameFlux;
                fluxHistPos = (fluxHistPos + 1) % kFluxHistLen;
            }

            // ── パス2: 位相決定 → 回転 → IFFT → OLA ──
            for (int ch = 0; ch < numCh; ++ch)
            {
                auto& sp = spec[(size_t) ch];
                auto& mg = mag[(size_t) ch];
                auto& ph = phase[(size_t) ch];
                auto& pp = prevPhase[(size_t) ch];
                auto& sy = synthPhase[(size_t) ch];

                // ピーク検出 (±2 近傍より大きい局所最大)
                peaks.clear();
                for (int k = 2; k <= half - 2; ++k)
                {
                    const float a = mg[(size_t) k];
                    if (a > 1.0e-9f
                        && a >  mg[(size_t)(k - 1)] && a >= mg[(size_t)(k + 1)]
                        && a >  mg[(size_t)(k - 2)] && a >= mg[(size_t)(k + 2)])
                        peaks.push_back(k);
                }

                const bool passthrough = (m == 0) || peaks.empty();
                if (passthrough)
                {
                    for (int k = 0; k <= half; ++k)
                        sy[(size_t) k] = ph[(size_t) k];
                }
                else
                {
                    int regionStart = 1;
                    for (size_t pi = 0; pi < peaks.size(); ++pi)
                    {
                        const int p = peaks[pi];
                        int regionEnd = half - 1;
                        if (pi + 1 < peaks.size())
                        {
                            // 次のピークとの間の谷を境界に
                            int valley = p + 1;
                            float vmin = mg[(size_t) valley];
                            for (int k = p + 2; k < peaks[pi + 1]; ++k)
                                if (mg[(size_t) k] < vmin) { vmin = mg[(size_t) k]; valley = k; }
                            regionEnd = valley;
                        }

                        // トランジェントフレームでも、リセットは立ち上がっている領域のみ
                        // (持続音の領域は位相伝播を続け、コードのうねり/クリックを防ぐ)
                        const bool regionReset = transient
                            && mg[(size_t) p] > kAttackRise
                                                * prevMag[(size_t) ch][(size_t) p]
                                                + 1.0e-9f;
                        if (regionReset)
                        {
                            for (int k = regionStart; k <= regionEnd; ++k)
                                sy[(size_t) k] = ph[(size_t) k];
                        }
                        else
                        {
                            // ピーク bin の真の角周波数 (位相差分から推定) → 合成位相
                            const double omegaP = 2.0 * kPi * (double) p / (double) N;
                            const double delta  = princarg(ph[(size_t) p] - pp[(size_t) p]
                                                           - omegaP * (double) dt);
                            const double omega  = omegaP + delta / (double) dt;
                            const double psiNew = sy[(size_t) p] + omega * (double) Hs;
                            const double theta  = psiNew - ph[(size_t) p];
                            const float  cosT   = (float) std::cos(theta);
                            const float  sinT   = (float) std::sin(theta);

                            for (int k = regionStart; k <= regionEnd; ++k)
                            {
                                const float re = sp[(size_t)(2 * k)];
                                const float im = sp[(size_t)(2 * k + 1)];
                                sp[(size_t)(2 * k)]     = re * cosT - im * sinT;
                                sp[(size_t)(2 * k + 1)] = re * sinT + im * cosT;
                                sy[(size_t) k] = princarg(ph[(size_t) k] + theta);
                            }
                        }
                        regionStart = regionEnd + 1;
                    }
                    // 領域外 (先頭ピーク前の bin 0 / 末尾) と DC・ナイキストは無回転
                    sy[0]             = ph[0];
                    sy[(size_t) half] = ph[(size_t) half];
                }

                // 状態更新
                for (int k = 0; k <= half; ++k)
                {
                    pp[(size_t) k] = ph[(size_t) k];
                    prevMag[(size_t) ch][(size_t) k] = mg[(size_t) k];
                }

                // IFFT → 窓掛け OLA
                auto& fr = frame[(size_t) ch];
                fft.inverse(sp.data(), fr.data());
                float* dst = out[(size_t) ch].data();
                for (int i = 0; i < N; ++i)
                    dst[ts + i] += fr[(size_t) i] * window[(size_t) i];
            }

            // OLA 正規化係数 (チャンネル共通なので 1 回)
            for (int i = 0; i < N; ++i)
                norm[(size_t)(ts + i)] += window[(size_t) i] * window[(size_t) i];

            prevTa = ta;
        }

        for (int ch = 0; ch < numCh; ++ch)
        {
            float* dst = out[(size_t) ch].data();
            for (int i = 0; i < allocLen; ++i)
                if (norm[(size_t) i] > 1.0e-4f)
                    dst[i] /= norm[(size_t) i];
        }

        return true;
    }

    // ── r8brain リサンプル (src の startSample 以降を入力に、outLenTarget まで生成) ──
    bool resampleSection(const std::vector<std::vector<float>>& src, int srcLen,
                         int startSample, double srcRate, double dstRate,
                         std::vector<std::vector<float>>& out, int outLenTarget,
                         const std::function<bool(double)>& onProgress,
                         double progressBegin, double progressEnd)
    {
        const int numCh = (int) src.size();
        const int avail = srcLen - startSample;
        if (numCh <= 0 || avail <= 0 || outLenTarget <= 0) return false;

        const int blockSize = 4096;
        std::vector<std::unique_ptr<r8b::CDSPResampler24>> resamps;
        resamps.reserve((size_t) numCh);
        for (int ch = 0; ch < numCh; ++ch)
            resamps.emplace_back(std::make_unique<r8b::CDSPResampler24>(
                srcRate, dstRate, blockSize));

        out.assign((size_t) numCh, std::vector<float>((size_t) outLenTarget, 0.0f));

        std::vector<std::vector<double>> inDouble((size_t) numCh);
        for (auto& v : inDouble) v.resize((size_t) blockSize);
        std::vector<double*> outPtrs((size_t) numCh, nullptr);

        int readPos = 0;
        int written = 0;
        int flushGuard = 0;

        while (written < outLenTarget)
        {
            if (onProgress
                && ! onProgress(progressBegin + (progressEnd - progressBegin)
                                * (double) written / (double) outLenTarget))
                return false;

            int toRead = std::min(blockSize, avail - readPos);
            const bool flushing = (toRead <= 0);
            if (flushing)
            {
                // 入力が尽きたらゼロを送ってリサンプラの遅延分を取り出す
                toRead = blockSize;
                if (++flushGuard > 64) break;
                for (auto& v : inDouble) std::fill(v.begin(), v.end(), 0.0);
            }
            else
            {
                for (int ch = 0; ch < numCh; ++ch)
                {
                    const float* sp = src[(size_t) ch].data() + startSample + readPos;
                    for (int i = 0; i < toRead; ++i)
                        inDouble[(size_t) ch][(size_t) i] = (double) sp[i];
                    if (toRead < blockSize)
                        std::fill(inDouble[(size_t) ch].begin() + toRead,
                                  inDouble[(size_t) ch].end(), 0.0);
                }
                readPos += toRead;
            }

            int writeCount = 0;
            for (int ch = 0; ch < numCh; ++ch)
            {
                double* opp = nullptr;
                writeCount = resamps[(size_t) ch]->process(
                    inDouble[(size_t) ch].data(), toRead, opp);
                outPtrs[(size_t) ch] = opp;
            }

            if (writeCount <= 0)
            {
                if (flushing && flushGuard > 8) break;
                continue;
            }

            const int nOut = std::min(writeCount, outLenTarget - written);
            for (int ch = 0; ch < numCh; ++ch)
            {
                float* dst = out[(size_t) ch].data() + written;
                const double* op = outPtrs[(size_t) ch];
                for (int i = 0; i < nOut; ++i)
                    dst[i] = (float) op[i];
            }
            written += nOut;
        }

        return true;
    }
}

namespace pitchcore
{
    bool shiftPitch(const float* const* input, int numChannels, int numSamples,
                    double sampleRate, double semitones,
                    std::vector<std::vector<float>>& output,
                    const std::function<bool(double)>& onProgress)
    {
        return shiftPitchAndTime(input, numChannels, numSamples, sampleRate,
                                 semitones, 1.0, output, onProgress);
    }

    bool shiftPitchAndTime(const float* const* input, int numChannels, int numSamples,
                           double sampleRate, double semitones, double timeRatio,
                           std::vector<std::vector<float>>& output,
                           const std::function<bool(double)>& onProgress)
    {
        if (input == nullptr || numChannels <= 0 || numSamples <= 0 || sampleRate <= 0.0)
            return false;
        if (! (timeRatio >= 0.25 && timeRatio <= 4.0))
            return false;

        const bool noPitch = std::abs(semitones) < 1.0e-9;
        const bool noTime  = std::abs(timeRatio - 1.0) < 1.0e-9;
        if (noPitch && noTime)
        {
            output.assign((size_t) numChannels, std::vector<float>((size_t) numSamples));
            for (int ch = 0; ch < numChannels; ++ch)
                std::memcpy(output[(size_t) ch].data(), input[ch],
                            (size_t) numSamples * sizeof(float));
            if (onProgress) onProgress(1.0);
            return true;
        }

        const double ratio   = noPitch ? 1.0 : std::pow(2.0, semitones / 12.0);
        const double factor  = ratio * timeRatio;   // ストレッチ段の倍率 (尺 × ピッチ補償)
        const int    fftSize = (sampleRate >= 88200.0 ? 16384 : 8192);
        const int    pad     = fftSize;
        const int    outLen  = noTime ? numSamples
                                      : std::max(1, (int) std::llround((double) numSamples * timeRatio));

        std::vector<std::vector<float>> padded(
            (size_t) numChannels,
            std::vector<float>((size_t)(numSamples + 2 * pad), 0.0f));
        for (int ch = 0; ch < numChannels; ++ch)
            std::memcpy(padded[(size_t) ch].data() + pad, input[ch],
                        (size_t) numSamples * sizeof(float));

        const double stretchEnd = noPitch ? 1.0 : 0.7;
        std::vector<std::vector<float>> stretched;
        int stretchedLen = 0;
        if (! stretchBuffer(padded, factor, fftSize, stretched, stretchedLen,
                            onProgress, 0.0, stretchEnd))
            return false;
        // stretched の確保長は stretchedLen + N + Hs だが、有効な実データ全長を渡す
        const int stretchedAlloc = (int) stretched[0].size();

        // 先頭パディング相当を捨て、末尾側はルックアヘッド用に残したまま渡す。
        // フレーム中心の時間写像は y = f*x - (f-1)*N/2 (解析フレーム [ta, ta+N) の中心
        // ta+N/2 が合成 ts+N/2 に対応するため)。pad*f だけ捨てると (f-1)*N/2 ずれる
        const int contentStart = std::max(0, (int) std::llround(
            (double) pad * factor - (factor - 1.0) * (double) fftSize / 2.0));
        if (contentStart >= stretchedAlloc) return false;

        if (noPitch)
        {
            // ピッチ不変 = リサンプル不要。ストレッチ結果から尺ぶんを切り出す
            output.assign((size_t) numChannels, std::vector<float>((size_t) outLen, 0.0f));
            const int avail = std::min(outLen, stretchedAlloc - contentStart);
            for (int ch = 0; ch < numChannels; ++ch)
                std::memcpy(output[(size_t) ch].data(),
                            stretched[(size_t) ch].data() + contentStart,
                            (size_t) avail * sizeof(float));
        }
        else if (! resampleSection(stretched, stretchedAlloc, contentStart,
                                   sampleRate, sampleRate / ratio,
                                   output, outLen, onProgress, 0.7, 1.0))
            return false;

        if (onProgress) onProgress(1.0);
        return true;
    }
}
