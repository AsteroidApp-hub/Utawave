// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

#pragma once
#include <atomic>
#include <vector>
#include <cstdint>
#include <cmath>
#include <algorithm>

// ── 配信ミラー出力のリングバッファ (SPSC・上書き許容・JUCE 非依存) ──
// writer = メインの audio thread (最終ミックスを push・確保/ロック無し)、
// reader = ミラー出力デバイスのコールバックスレッド (StreamMirrorReader::pull 経由)。
// 2 デバイスはクロックが別 (水晶が違う) なので、reader 側が水位 (溜まり具合) を目標に保つよう
// 読み出しレートを微調整するドリフト補正リサンプラで読む。ここが無いとズレが数分で成長して
// 溢れ/枯れによる周期的な音飛びになる。
// writePos は単調増加 (リセットしない)。SR 変更/デバイス再起動は epoch を進めて reader に
// 溜め直し (re-prime) を指示する (writer 停止中に呼ぶ reset() 以外の同期は不要)。
class StreamMirrorRing
{
public:
    explicit StreamMirrorRing(int capacityFrames = 1 << 17)   // ~2.7s @48k (2ch float ≈ 1MB)
        : capacity(nextPow2(capacityFrames)), mask(capacity - 1),
          bufL((size_t) capacity, 0.0f), bufR((size_t) capacity, 0.0f) {}

    // writer (audio thread): 最終出力を複製する。R が nullptr なら L を両 ch へ (モノ出力)。
    void push(const float* L, const float* R, int n) noexcept
    {
        if (L == nullptr || n <= 0) return;
        const std::uint64_t w = writePos.load(std::memory_order_relaxed);
        for (int i = 0; i < n; ++i)
        {
            const size_t idx = (size_t) ((w + (std::uint64_t) i) & (std::uint64_t) mask);
            bufL[idx] = L[i];
            bufR[idx] = (R != nullptr) ? R[i] : L[i];
        }
        writePos.store(w + (std::uint64_t) n, std::memory_order_release);
    }

    // writer 側の管理スレッド (message thread) から: ソース SR の設定 + reader へ溜め直し指示。
    // メインデバイスの audioDeviceAboutToStart (コールバック再開前) と setMirrorRing が呼ぶ。
    // writePos は触らない (reader が並行して読んでいても安全)。
    void reset(double sourceSampleRate) noexcept
    {
        srcRate.store(sourceSampleRate, std::memory_order_relaxed);
        epoch.fetch_add(1, std::memory_order_release);
    }

    double        getSourceSampleRate() const noexcept { return srcRate.load(std::memory_order_relaxed); }
    std::uint32_t getEpoch()            const noexcept { return epoch.load(std::memory_order_acquire); }
    std::uint64_t getWritePos()         const noexcept { return writePos.load(std::memory_order_acquire); }
    int           getCapacity()         const noexcept { return capacity; }

    // reader: 絶対フレーム位置のサンプル読み (呼び出し側が pos < writePos を保証する)
    float sampleL(std::uint64_t pos) const noexcept { return bufL[(size_t) (pos & (std::uint64_t) mask)]; }
    float sampleR(std::uint64_t pos) const noexcept { return bufR[(size_t) (pos & (std::uint64_t) mask)]; }

private:
    static int nextPow2(int v) noexcept
    {
        int p = 1;
        while (p < v && p < (1 << 24)) p <<= 1;
        return p;
    }

    const int capacity;
    const int mask;
    std::vector<float> bufL, bufR;
    std::atomic<std::uint64_t> writePos { 0 };
    std::atomic<std::uint32_t> epoch    { 0 };
    std::atomic<double>        srcRate  { 0.0 };
};

// ── ミラー出力デバイス側の読み手 (reader スレッド専用の状態・JUCE 非依存) ──
// 線形補間のバリスピードで読み、水位 (avail) を目標 (targetFillSecs) に保つよう読み出し
// レートを ±0.5% まで比例制御する。これ 1 つで SR 変換 (44.1k デバイス等) とクロック
// ドリフト吸収の両方を担う。枯渇したら無音を出して目標水位まで溜め直す (priming) ので、
// 境界で連続プチプチにならない。ミラーの遅延はおおむね targetFillSecs で一定に安定する
// (配信側は OBS 等の同期オフセット 1 発で吸収できる)。
class StreamMirrorReader
{
public:
    explicit StreamMirrorReader(double targetFillSeconds = 0.05)
        : targetFillSecs(targetFillSeconds) {}

    // ring から nOut フレームを dstRate で読み出す。outR は nullptr 可 (モノ出力)。
    // データ不足 / 未プライム / SR 未設定の間は無音を書く。
    void pull(StreamMirrorRing& ring, float* outL, float* outR, int nOut, double dstRate) noexcept
    {
        if (outL == nullptr || nOut <= 0) return;
        auto silence = [&]() noexcept
        {
            std::fill(outL, outL + nOut, 0.0f);
            if (outR != nullptr) std::fill(outR, outR + nOut, 0.0f);
        };

        const double srcRate = ring.getSourceSampleRate();
        if (srcRate <= 0.0 || dstRate <= 0.0) { silence(); return; }

        // SR 変更/デバイス再起動 (epoch 変化): 旧データを捨てて溜め直す
        const std::uint32_t ep = ring.getEpoch();
        const std::uint64_t w  = ring.getWritePos();
        if (ep != seenEpoch)
        {
            seenEpoch = ep;
            priming   = true;
            primeFrom = w;
        }

        const double target = srcRate * targetFillSecs;

        if (priming)
        {
            if ((double) (w - primeFrom) < target) { silence(); return; }
            priming = false;
            readPos = (double) w - target;   // 目標水位ぶん後ろから読み始める
        }

        double avail = (double) w - readPos;

        // 大幅な取り残し (reader 一時停止等) は目標水位までスキップして追いつく
        if (avail > (double) ring.getCapacity() - 64.0)
        {
            readPos = (double) w - target;
            avail   = target;
        }

        // ドリフト補正: 水位の目標からのズレに比例して読み出しレートを可変 (上限 ±0.5%)。
        // 時定数 ≈ targetFillSecs / 0.05 = 約 1 秒で目標水位へ収束する。
        const double err   = (avail - target) / target;
        const double corr  = std::clamp(err * 0.05, -0.005, 0.005);
        const double ratio = (srcRate / dstRate) * (1.0 + corr);

        // このブロックで必要なソースフレーム (+補間の 1 サンプル先読みとマージン)
        if (avail < ratio * (double) nOut + 2.0)
        {
            silence();
            priming   = true;   // 枯渇 → 溜め直し (半端に読むと毎ブロック プチプチが続く)
            primeFrom = w;
            return;
        }

        double pos = readPos;
        for (int i = 0; i < nOut; ++i)
        {
            const std::uint64_t i0 = (std::uint64_t) pos;
            const float frac = (float) (pos - (double) i0);
            const float l0 = ring.sampleL(i0), l1 = ring.sampleL(i0 + 1);
            outL[i] = l0 + (l1 - l0) * frac;
            if (outR != nullptr)
            {
                const float r0 = ring.sampleR(i0), r1 = ring.sampleR(i0 + 1);
                outR[i] = r0 + (r1 - r0) * frac;
            }
            pos += ratio;
        }
        readPos = pos;
    }

    // テスト/状態表示用
    bool   isPrimed() const noexcept          { return !priming; }
    double currentAvail(const StreamMirrorRing& ring) const noexcept
    {
        return (double) ring.getWritePos() - readPos;
    }

private:
    double        targetFillSecs;   // 非 const (再初期化 reader = StreamMirrorReader() を許すため)
    double        readPos   { 0.0 };
    std::uint64_t primeFrom { 0 };
    std::uint32_t seenEpoch { 0xffffffffu };
    bool          priming   { true };
};
