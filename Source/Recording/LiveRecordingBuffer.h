// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

#pragma once
#include <JuceHeader.h>

// Lock-free, audio-thread-safe リアルタイム波形バッファ
//
// 480KB のピーク配列を遅延確保する: 一度も録音されないトラックは 0 バイト。
// reset() (録音開始) 時に確保し、Track 破棄まで保持する。
// (録音停止毎に解放するのは pushSamples との競合が複雑なので避けている)
class LiveRecordingBuffer
{
public:
    static constexpr int maxPeaks       { 120000 };
    static constexpr int samplesPerPeak { 256 };  // ~5.3ms @48kHz

    void reset()
    {
        // 初回 reset で確保。以降は再利用してゼロクリアのみ。
        // 確保完了を release-store し、audio thread 側の acquire-load と
        // happens-before 関係を明示的に張る (#Minor-3)。
        if (peaksStorage == nullptr)
        {
            peaksStorage = std::make_unique<float[]>((size_t) maxPeaks);
            peaksPtr.store(peaksStorage.get(), std::memory_order_release);
        }

        peakCount.store(0, std::memory_order_release);
        accumMax           = 0.0f;
        samplesAccumulated = 0;
    }

    // Audio thread only
    void pushSamples(const float* data, int numSamples)
    {
        float* p = peaksPtr.load(std::memory_order_acquire);
        if (p == nullptr) return;  // reset() 未呼び出し (録音アームしていないトラック)
        for (int i = 0; i < numSamples; ++i)
        {
            float s = std::abs(data[i]);
            if (s > accumMax) accumMax = s;

            if (++samplesAccumulated >= samplesPerPeak)
            {
                int idx = peakCount.load(std::memory_order_relaxed);
                if (idx < maxPeaks)
                {
                    p[(size_t) idx] = accumMax;
                    peakCount.store(idx + 1, std::memory_order_release);
                }
                accumMax           = 0.0f;
                samplesAccumulated = 0;
            }
        }
    }

    // UI thread (peaksStorage の唯一の書き手は reset() = UI thread なので直接参照で安全)
    int   getPeakCount() const       { return peaksStorage ? peakCount.load(std::memory_order_acquire) : 0; }
    float getPeak(int i)  const      { return peaksStorage ? peaksStorage[(size_t) i] : 0.0f; }

    double getDurationSeconds(double sampleRate) const
    {
        return (double)getPeakCount() * samplesPerPeak / sampleRate;
    }

    // Draw peaks as a waveform into bounds
    // verticalZoom: 波形振幅の縦ズーム (Shift+Option+スクロール)。確定クリップの描画
    // (drawClipWaveform) と同じ係数を掛け、録音中と録音後で波形の見た目を一致させる
    void draw(juce::Graphics& g, juce::Rectangle<int> bounds,
              juce::Colour colour, double startSeconds, double totalSeconds,
              double sampleRate, float verticalZoom = 1.0f) const
    {
        if (peaksStorage == nullptr) return;
        const int count = getPeakCount();
        if (count == 0 || totalSeconds <= 0.0) return;

        const double secsPerPeak = (double)samplesPerPeak / sampleRate;
        const double startPeak   = startSeconds / secsPerPeak;
        const double endPeak     = (startSeconds + totalSeconds) / secsPerPeak;
        const double peaksPerPixel = (endPeak - startPeak) / bounds.getWidth();

        g.setColour(colour.withAlpha(0.85f));

        // 可視範囲 (現在のクリップ矩形) と交差する px 列だけを組み立てる。bounds は録音全長ぶんの
        // 幅を持つ (画面外まで伸びる) ので、全幅を毎回 0..width で組むと録音が長くなるほど線形に
        // 重くなり、20Hz の再描画でメッセージスレッドが詰まる (再生バー/ライブ波形のカクツキ)。
        // クリップ域に絞ることで、組み立てるパスは録音長に依存せずビューポート幅で頭打ちになる。
        const auto clipB = g.getClipBounds();
        const int pxStart = juce::jmax(0, clipB.getX() - bounds.getX());
        const int pxEnd   = juce::jmin(bounds.getWidth(), clipB.getRight() - bounds.getX());
        if (pxEnd <= pxStart) return;

        // 1px 毎に drawLine を呼ぶと長尺で UI が詰まるので、
        // 全列を 1 つの Path に積んでから strokePath で一括描画する。
        juce::Path wavePath;
        wavePath.preallocateSpace((pxEnd - pxStart) * 3);
        const float cyF = (float) bounds.getCentreY();
        const float boundsH = (float) bounds.getHeight();
        for (int px = pxStart; px < pxEnd; ++px)
        {
            double p0 = startPeak + px * peaksPerPixel;
            // まだ書き込まれていない領域は描かない (クランプすると末尾ピークが右へ
            // 引き伸ばされる)。レイテンシ補正込み表示では枠の右端 (再生バー) より
            // 内容が comp ぶん遅れて届くため、この打ち切りが波形の先端になる
            if (p0 >= (double) count) break;
            double p1 = p0 + peaksPerPixel;
            // 内容開始前の領域も描かない (startSeconds が負 = 負のレイテンシ補正で
            // 内容を遅く描くケース。クランプすると先頭ピークが左へ引き伸ばされる)
            if (p1 <= 0.0) continue;
            int i0 = juce::jlimit(0, count - 1, (int)p0);
            int i1 = juce::jlimit(0, count - 1, (int)p1);

            float maxVal = 0.0f;
            for (int pi = i0; pi <= i1; ++pi)
                maxVal = std::max(maxVal, peaksStorage[(size_t) pi]);

            // ズーム超過分はレーン高で頭打ち (確定クリップの jlimit(topY,botY) と同じ挙動)
            const float halfH = juce::jlimit(0.5f, 0.5f * boundsH,
                                             maxVal * verticalZoom * 0.5f * boundsH);
            const float xF    = (float)(bounds.getX() + px);
            wavePath.startNewSubPath(xF, cyF - halfH);
            wavePath.lineTo       (xF, cyF + halfH);
        }
        g.strokePath(wavePath, juce::PathStrokeType(1.0f));
    }

private:
    // 遅延ヒープ確保 (録音アームしていないトラックでは 0 バイト)。
    // reset() (UI スレッド) で確保し、peaksStorage が所有する。
    // peaksPtr は audio スレッドが acquire-load する公開ポインタで、
    // reset() の release-store と happens-before 関係を明示的に張る (#Minor-3)。
    std::unique_ptr<float[]>    peaksStorage;
    std::atomic<float*>         peaksPtr  { nullptr };
    std::atomic<int>            peakCount { 0 };
    float accumMax           { 0.0f };
    int   samplesAccumulated { 0 };
};
