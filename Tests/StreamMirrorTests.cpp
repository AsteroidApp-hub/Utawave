// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave
//
// 配信ミラー出力 (StreamMirrorRing / StreamMirrorReader + AudioEngine のミラータップ) の
// ユニットテスト。リング/リーダーは JUCE 非依存の純ロジックなので決定論的に検証できる。
//
// 定石:
// - 「pull を 1 回呼んでから push する」と primeFrom = 0 になり、目標水位 (target) ちょうどで
//   プライム完了 → readPos = 0 から読み始める (先頭からの厳密比較ができる)
// - push/pull のフレーム数を ratio と正確に釣り合わせると水位が動かず corr = 0 のまま
//   (= 読み出しは厳密な等間隔) なので、線形ランプ入力なら出力を厳密値で検証できる
// - ドリフトは push 側を意図的に速くして注入する (corr 上限 ±0.5% 未満のズレを使う)

#include <JuceHeader.h>
#include <cmath>
#include <vector>

#include "../Source/Audio/StreamMirrorRing.h"
#include "../Source/Audio/AudioEngine.h"

namespace
{
constexpr double kSR    = 48000.0;
constexpr int    kBlock = 480;
constexpr double kFill  = 0.05;                       // reader の目標水位 (秒)
constexpr int    kTargetFrames = (int) (kSR * kFill); // = 2400 @48k

// エンジンタップ検証用の最小スタブデバイス (AudioEngineTests と同じ作法)
struct MirrorFakeDevice : public juce::AudioIODevice
{
    MirrorFakeDevice() : juce::AudioIODevice("FakeDevice", "FakeType") {}
    juce::StringArray getOutputChannelNames() override        { return { "L", "R" }; }
    juce::StringArray getInputChannelNames() override         { return { "In 1", "In 2" }; }
    juce::Array<double> getAvailableSampleRates() override    { return { kSR }; }
    juce::Array<int> getAvailableBufferSizes() override       { return { kBlock }; }
    int getDefaultBufferSize() override                       { return kBlock; }
    juce::String open(const juce::BigInteger&, const juce::BigInteger&,
                      double, int) override                   { return {}; }
    void close() override                                     {}
    bool isOpen() override                                    { return true; }
    void start(juce::AudioIODeviceCallback*) override         {}
    void stop() override                                      {}
    bool isPlaying() override                                 { return false; }
    juce::String getLastError() override                      { return {}; }
    int getCurrentBufferSizeSamples() override                { return kBlock; }
    double getCurrentSampleRate() override                    { return kSR; }
    int getCurrentBitDepth() override                         { return 32; }
    juce::BigInteger getActiveOutputChannels() const override { juce::BigInteger b; b.setRange(0, 2, true); return b; }
    juce::BigInteger getActiveInputChannels() const override  { juce::BigInteger b; b.setRange(0, 2, true); return b; }
    int getOutputLatencyInSamples() override                  { return 0; }
    int getInputLatencyInSamples() override                   { return 0; }
};

class StreamMirrorTests : public juce::UnitTest
{
public:
    StreamMirrorTests() : juce::UnitTest("StreamMirror (broadcast mirror output)") {}

    // 連番ランプ (frame k = base + k*step) を n フレーム push する
    static void pushRamp(StreamMirrorRing& ring, juce::int64& counter, int n, float step)
    {
        std::vector<float> l((size_t) n), r((size_t) n);
        for (int i = 0; i < n; ++i)
        {
            l[(size_t) i] = (float) (counter + i) * step;
            r[(size_t) i] = (float) (counter + i) * step * 0.5f;
        }
        ring.push(l.data(), r.data(), n);
        counter += n;
    }

    static bool allZero(const std::vector<float>& v)
    {
        for (float s : v) if (s != 0.0f) return false;
        return true;
    }

    void runTest() override
    {
        //==============================================================================
        beginTest("priming: silence until the target fill is reached");
        {
            StreamMirrorRing ring;
            ring.reset(kSR);
            StreamMirrorReader reader(kFill);
            std::vector<float> outL((size_t) kBlock, 1.0f), outR((size_t) kBlock, 1.0f);

            // データ無し → 無音 + 未プライム (primeFrom = 0 が確定する)
            reader.pull(ring, outL.data(), outR.data(), kBlock, kSR);
            expect(allZero(outL) && allZero(outR), "no data yields silence");
            expect(!reader.isPrimed(), "not primed yet");

            // 目標水位の一歩手前まで push しても、まだ無音
            juce::int64 counter = 0;
            pushRamp(ring, counter, kTargetFrames - 1, 1.0e-4f);
            reader.pull(ring, outL.data(), outR.data(), kBlock, kSR);
            expect(allZero(outL), "still silent just below target fill");
            expect(!reader.isPrimed(), "still priming just below target fill");

            // 目標到達 → プライム完了、先頭 (frame 0) から読み出される
            pushRamp(ring, counter, 1, 1.0e-4f);
            reader.pull(ring, outL.data(), outR.data(), kBlock, kSR);
            expect(reader.isPrimed(), "primed at target fill");
            bool exact = true;
            for (int i = 0; i < kBlock; ++i)
                if (std::abs(outL[(size_t) i] - (float) i * 1.0e-4f) > 1.0e-7f) { exact = false; break; }
            expect(exact, "primed read starts at frame 0 and is sample-exact (ratio 1, corr 0)");
        }

        //==============================================================================
        beginTest("steady state: balanced push/pull is sample-exact and keeps fill at target");
        {
            StreamMirrorRing ring;
            ring.reset(kSR);
            StreamMirrorReader reader(kFill);
            std::vector<float> outL((size_t) kBlock), outR((size_t) kBlock);
            juce::int64 counter = 0;

            reader.pull(ring, outL.data(), outR.data(), kBlock, kSR);   // primeFrom = 0
            pushRamp(ring, counter, kTargetFrames, 1.0e-4f);

            juce::int64 expected = 0;
            bool exact = true, rExact = true;
            for (int blk = 0; blk < 200; ++blk)
            {
                reader.pull(ring, outL.data(), outR.data(), kBlock, kSR);
                for (int i = 0; i < kBlock; ++i)
                {
                    if (std::abs(outL[(size_t) i] - (float) (expected + i) * 1.0e-4f) > 1.0e-6f) exact = false;
                    if (std::abs(outR[(size_t) i] - (float) (expected + i) * 0.5e-4f) > 1.0e-6f) rExact = false;
                }
                expected += kBlock;
                pushRamp(ring, counter, kBlock, 1.0e-4f);
            }
            expect(exact,  "L is continuous and sample-exact across 200 blocks");
            expect(rExact, "R is continuous and sample-exact across 200 blocks");
            // ループ末尾は push 直後なので水位は目標ちょうどに戻っている
            expect(std::abs(reader.currentAvail(ring) - (double) kTargetFrames) < 2.0,
                   "fill stays at target when clocks are balanced");
        }

        //==============================================================================
        beginTest("sample-rate conversion: 48k source read at 32k (ratio 1.5) is exact on a ramp");
        {
            StreamMirrorRing ring;
            ring.reset(kSR);
            StreamMirrorReader reader(kFill);
            const double dstRate = 32000.0;
            const int nOut = 320;                    // 320 out * 1.5 = 480 src / ブロック
            std::vector<float> outL((size_t) nOut), outR((size_t) nOut);
            juce::int64 counter = 0;

            reader.pull(ring, outL.data(), outR.data(), nOut, dstRate);   // primeFrom = 0
            pushRamp(ring, counter, kTargetFrames, 1.0e-4f);

            double expectedSrc = 0.0;   // 出力サンプル i のソース位置 = expectedSrc + i*1.5
            bool exact = true;
            for (int blk = 0; blk < 100; ++blk)
            {
                reader.pull(ring, outL.data(), outR.data(), nOut, dstRate);
                for (int i = 0; i < nOut; ++i)
                {
                    // 線形ランプの線形補間は厳密 (float 誤差のみ)
                    const double srcPos = expectedSrc + i * 1.5;
                    if (std::abs(outL[(size_t) i] - (float) (srcPos * 1.0e-4)) > 2.0e-6f) { exact = false; break; }
                }
                expectedSrc += nOut * 1.5;
                pushRamp(ring, counter, 480, 1.0e-4f);   // 消費と同量を補給 → corr 0 のまま
            }
            expect(exact, "resampled ramp matches linear interpolation exactly");
        }

        //==============================================================================
        beginTest("drift control: a 0.2% fast writer is absorbed (fill stays bounded, no overrun)");
        {
            StreamMirrorRing ring;
            ring.reset(kSR);
            StreamMirrorReader reader(kFill);
            std::vector<float> outL((size_t) kBlock), outR((size_t) kBlock);
            juce::int64 counter = 0;

            reader.pull(ring, outL.data(), outR.data(), kBlock, kSR);   // primeFrom = 0
            pushRamp(ring, counter, kTargetFrames, 1.0e-6f);

            // writer が公称より 1 frame/block (~0.2%) 速い = 実機のクロックドリフトを大きく
            // 上回る量。corr (±0.5%) の範囲内なので、水位は有界に保たれるはず
            double maxAvail = 0.0;
            bool stayedPrimed = true;
            for (int blk = 0; blk < 3000; ++blk)   // ~30 秒相当
            {
                reader.pull(ring, outL.data(), outR.data(), kBlock, kSR);
                if (blk > 10 && !reader.isPrimed()) stayedPrimed = false;
                pushRamp(ring, counter, kBlock + 1, 1.0e-6f);
                maxAvail = juce::jmax(maxAvail, reader.currentAvail(ring));
            }
            expect(stayedPrimed, "reader never starves under fast-writer drift");
            expect(maxAvail < kTargetFrames * 3.0,
                   "fill stays bounded (drift is absorbed by rate correction), got "
                   + juce::String(maxAvail));
        }

        //==============================================================================
        beginTest("underrun: starved reader emits silence, then re-primes and resumes");
        {
            StreamMirrorRing ring;
            ring.reset(kSR);
            StreamMirrorReader reader(kFill);
            std::vector<float> outL((size_t) kBlock), outR((size_t) kBlock);
            juce::int64 counter = 0;

            reader.pull(ring, outL.data(), outR.data(), kBlock, kSR);
            pushRamp(ring, counter, kTargetFrames, 1.0e-4f);
            reader.pull(ring, outL.data(), outR.data(), kBlock, kSR);
            expect(reader.isPrimed(), "primed before starving");

            // 補給を止めて水位を使い切らせる → 無音へ落ち、priming に戻る
            bool wentSilent = false;
            for (int blk = 0; blk < 20 && !wentSilent; ++blk)
            {
                reader.pull(ring, outL.data(), outR.data(), kBlock, kSR);
                if (allZero(outL) && !reader.isPrimed()) wentSilent = true;
            }
            expect(wentSilent, "starved reader goes silent and re-primes");

            // 目標水位まで補給すれば復帰する (溜め直しは「新しく積まれた分」で数える)
            pushRamp(ring, counter, kTargetFrames, 1.0e-4f);
            reader.pull(ring, outL.data(), outR.data(), kBlock, kSR);
            expect(reader.isPrimed(), "re-primed after refill");
            expect(!allZero(outL), "audio resumes after refill");
        }

        //==============================================================================
        beginTest("epoch reset: reset() discards backlog and re-primes with the new rate");
        {
            StreamMirrorRing ring;
            ring.reset(kSR);
            StreamMirrorReader reader(kFill);
            std::vector<float> outL((size_t) kBlock), outR((size_t) kBlock);
            juce::int64 counter = 0;

            reader.pull(ring, outL.data(), outR.data(), kBlock, kSR);
            pushRamp(ring, counter, kTargetFrames + kBlock * 4, 1.0e-4f);
            reader.pull(ring, outL.data(), outR.data(), kBlock, kSR);
            expect(reader.isPrimed(), "primed before reset");

            // デバイス再起動相当: 旧データが十分残っていても捨てて溜め直す
            ring.reset(44100.0);
            expectEquals(ring.getSourceSampleRate(), 44100.0, "source rate updated");
            reader.pull(ring, outL.data(), outR.data(), kBlock, kSR);
            expect(allZero(outL), "post-reset pull is silent (backlog discarded)");
            expect(!reader.isPrimed(), "re-priming after reset");

            // 新レートの目標水位 (44100*0.05 = 2205) を新規に積めば復帰
            pushRamp(ring, counter, (int) (44100.0 * kFill) + 1, 1.0e-4f);
            reader.pull(ring, outL.data(), outR.data(), kBlock, kSR);
            expect(reader.isPrimed(), "primed with new-rate fill");
        }

        //==============================================================================
        beginTest("mono paths: null R output and null R input are handled");
        {
            StreamMirrorRing ring;
            ring.reset(kSR);
            StreamMirrorReader reader(kFill);
            std::vector<float> outL((size_t) kBlock), outR((size_t) kBlock);

            reader.pull(ring, outL.data(), nullptr, kBlock, kSR);   // outR 無し (モノデバイス)

            // push の R = nullptr は L を両 ch へ複製する (モノ出力デバイスのミラー)
            std::vector<float> src((size_t) kTargetFrames + kBlock, 0.25f);
            ring.push(src.data(), nullptr, (int) src.size());
            reader.pull(ring, outL.data(), outR.data(), kBlock, kSR);
            expect(reader.isPrimed(), "primed");
            bool lOk = true, rOk = true;
            for (int i = 0; i < kBlock; ++i)
            {
                if (outL[(size_t) i] != 0.25f) lOk = false;
                if (outR[(size_t) i] != 0.25f) rOk = false;
            }
            expect(lOk && rOk, "mono push duplicated to both channels");

            // 無効な dstRate は無音 (ゼロ除算しない)
            reader.pull(ring, outL.data(), outR.data(), kBlock, 0.0);
            expect(allZero(outL), "dstRate 0 yields silence");
        }

        //==============================================================================
        beginTest("engine tap: final output (monitor return) is duplicated into the ring");
        {
            AudioEngine engine;   // initialise() しない (デバイス無し)
            MirrorFakeDevice fake;
            engine.audioDeviceAboutToStart(&fake);

            auto ring = std::make_shared<StreamMirrorRing>();
            engine.setMirrorRing(ring);
            expectEquals(ring->getSourceSampleRate(), kSR, "ring source rate = engine device rate");

            // 停止中 + 入力モニタ ON: 出力 = ドライ返し (mono 入力 ch0 → L/R センター)
            engine.setInputMonitoringActive(true);

            const int block = 512;   // FakeDevice の block
            std::vector<float> inL((size_t) block, 0.25f), inR((size_t) block, 0.0f);
            const float* ins[2] = { inL.data(), inR.data() };
            std::vector<float> outL((size_t) block), outR((size_t) block);
            float* outs[2] = { outL.data(), outR.data() };

            const int nBlocks = 8;
            for (int b = 0; b < nBlocks; ++b)
                engine.audioDeviceIOCallbackWithContext(ins, 2, outs, 2, block, {});

            expectEquals((juce::int64) ring->getWritePos(), (juce::int64) (nBlocks * block),
                         "ring advances by exactly numSamples per callback");

            // リング内容 = コールバックが出力した最終ミックス (最後のブロックと一致)
            bool match = true;
            const juce::uint64 w = ring->getWritePos();
            for (int i = 0; i < block; ++i)
            {
                if (ring->sampleL(w - (juce::uint64) block + (juce::uint64) i) != outL[(size_t) i]) match = false;
                if (ring->sampleR(w - (juce::uint64) block + (juce::uint64) i) != outR[(size_t) i]) match = false;
            }
            expect(match, "ring contents equal the final callback output (monitor return included)");
            expect(std::abs(outL[0] - 0.25f) < 0.01f, "sanity: monitor return actually present in output");

            // 解除 (drain) 後は push されない
            engine.setMirrorRing(nullptr);
            const juce::uint64 wBefore = ring->getWritePos();
            engine.audioDeviceIOCallbackWithContext(ins, 2, outs, 2, block, {});
            expectEquals((juce::int64) ring->getWritePos(), (juce::int64) wBefore,
                         "no pushes after the ring is detached");
        }
    }
};

static StreamMirrorTests streamMirrorTests;
}
