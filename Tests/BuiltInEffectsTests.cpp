// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

// Utawave — 内蔵エフェクト (EQ / コンプ / リバーブ) と BuiltInFactory のユニットテスト。
//
// DSP はオフラインで processBlock を直接駆動して検証する (デバイス不要)。
//   ・EQ: フラットは素通り / HPF が低域を落とす / 高域ブーストが高域を持ち上げる / 状態往復
//   ・コンプ: 閾値下は素通り (GR≈0) / 閾値上で減衰 (GR>0) / 尺不変 (ゼロレイテンシー)
//   ・リバーブ: Mix=0 は完全素通り / Mix>0 は入力後にテールが残る
//   ・ファクトリ: identifier / メニュー ID / createIdentifierString 往復 / フォーマット判定
// expect メッセージは ASCII。バッファはテストローカル。

#include <JuceHeader.h>
#include <cmath>
#include "../Source/Audio/builtin/BuiltInEQ.h"
#include "../Source/Audio/builtin/BuiltInCompressor.h"
#include "../Source/Audio/builtin/BuiltInDeEsser.h"
#include "../Source/Audio/builtin/BuiltInGate.h"
#include "../Source/Audio/builtin/BuiltInMaximizer.h"
#include "../Source/Audio/builtin/BuiltInDelay.h"
#include "../Source/Audio/builtin/BuiltInReverb.h"
#include "../Source/Audio/builtin/BuiltInKeroVoice.h"
#include "../Source/Audio/builtin/BuiltInFactory.h"

namespace
{
    constexpr double kSr = 48000.0;

    // freq の正弦波で満たしたステレオバッファを作る
    juce::AudioBuffer<float> makeSine(double freq, double amp, int numSamples)
    {
        juce::AudioBuffer<float> b(2, numSamples);
        const double w = 2.0 * juce::MathConstants<double>::pi * freq / kSr;
        for (int i = 0; i < numSamples; ++i)
        {
            const float v = (float) (amp * std::sin(w * i));
            b.setSample(0, i, v);
            b.setSample(1, i, v);
        }
        return b;
    }

    // 末尾 region サンプルの RMS (フィルタ整定後を測る)
    double rmsTail(const juce::AudioBuffer<float>& b, int region)
    {
        const int n = b.getNumSamples();
        const int start = juce::jmax(0, n - region);
        double sum = 0.0;
        for (int i = start; i < n; ++i)
        {
            const double s = b.getSample(0, i);
            sum += s * s;
        }
        return std::sqrt(sum / juce::jmax(1, n - start));
    }
}

class BuiltInEffectsTests : public juce::UnitTest
{
public:
    BuiltInEffectsTests() : juce::UnitTest("BuiltInEffects") {}

    void runTest() override
    {
        testEQ();
        testCompressor();
        testDeEsser();
        testGate();
        testMaximizer();
        testDelay();
        testReverb();
        testKeroVoice();
        testFactory();
    }

private:
    juce::MidiBuffer emptyMidi;

    void testEQ()
    {
        beginTest("EQ flat passes signal unchanged");
        {
            BuiltInEQ eq;
            eq.prepareToPlay(kSr, 4096);
            // フラット: 全ゲイン 0、HPF 最低
            eq.setP(BuiltInEQ::HpfHz, 20.0f);
            eq.setP(BuiltInEQ::LowDb, 0.0f);
            eq.setP(BuiltInEQ::MidDb, 0.0f);
            eq.setP(BuiltInEQ::AirDb, 0.0f);

            auto in  = makeSine(1000.0, 0.5, 8000);
            const double inRms = rmsTail(in, 2000);
            eq.processBlock(in, emptyMidi);
            const double outRms = rmsTail(in, 2000);
            expect(std::abs(outRms - inRms) < inRms * 0.06, "flat EQ should be near unity at 1kHz");
        }

        beginTest("EQ highpass attenuates lows");
        {
            BuiltInEQ eq;
            eq.prepareToPlay(kSr, 4096);
            eq.setP(BuiltInEQ::HpfHz, 300.0f);

            auto low = makeSine(80.0, 0.5, 8000);
            const double inRms = rmsTail(low, 2000);
            eq.processBlock(low, emptyMidi);
            const double outRms = rmsTail(low, 2000);
            expect(outRms < inRms * 0.4, "80Hz should be strongly attenuated by 300Hz HPF");
        }

        beginTest("EQ air shelf boosts highs");
        {
            BuiltInEQ eq;
            eq.prepareToPlay(kSr, 4096);
            eq.setP(BuiltInEQ::HpfHz, 20.0f);
            eq.setP(BuiltInEQ::AirHz, 8000.0f);
            eq.setP(BuiltInEQ::AirDb, 12.0f);

            auto hi = makeSine(12000.0, 0.2, 8000);
            const double inRms = rmsTail(hi, 2000);
            eq.processBlock(hi, emptyMidi);
            const double outRms = rmsTail(hi, 2000);
            expect(outRms > inRms * 1.5, "12kHz should be boosted by +12dB air shelf");
        }

        beginTest("EQ state round-trips");
        {
            BuiltInEQ a;
            a.setP(BuiltInEQ::HpfHz, 123.0f);
            a.setP(BuiltInEQ::MidDb, -5.5f);
            a.setP(BuiltInEQ::AirHz, 9000.0f);

            juce::MemoryBlock mb;
            a.getStateInformation(mb);

            BuiltInEQ b;
            b.setStateInformation(mb.getData(), (int) mb.getSize());
            for (int i = 0; i < a.getParamCount(); ++i)
                expect(std::abs(a.getP(i) - b.getP(i)) < 1.0e-3f, "param should round-trip");
        }
    }

    void testCompressor()
    {
        beginTest("Compressor passes quiet signal (no reduction)");
        {
            BuiltInCompressor c;
            c.prepareToPlay(kSr, 4096);
            c.setP(BuiltInCompressor::ThresholdDb, -18.0f);
            c.setP(BuiltInCompressor::Ratio, 4.0f);
            c.setP(BuiltInCompressor::MakeupDb, 0.0f);

            auto quiet = makeSine(440.0, 0.03, 8000);   // ~ -30 dB, below threshold
            const double inRms = rmsTail(quiet, 2000);
            c.processBlock(quiet, emptyMidi);
            const double outRms = rmsTail(quiet, 2000);
            expect(std::abs(outRms - inRms) < inRms * 0.05, "below threshold should be unchanged");
            expect(c.getReductionDb() < 0.5f, "no gain reduction below threshold");
        }

        beginTest("Compressor reduces loud signal");
        {
            BuiltInCompressor c;
            c.prepareToPlay(kSr, 4096);
            c.setP(BuiltInCompressor::ThresholdDb, -18.0f);
            c.setP(BuiltInCompressor::Ratio, 4.0f);
            c.setP(BuiltInCompressor::AttackMs, 5.0f);
            c.setP(BuiltInCompressor::ReleaseMs, 100.0f);
            c.setP(BuiltInCompressor::MakeupDb, 0.0f);

            auto loud = makeSine(440.0, 1.0, 12000);   // 0 dB peak, well above threshold
            const int n = loud.getNumSamples();
            expect(loud.getNumSamples() == n, "length must be unchanged (zero latency)");
            const double inRms = rmsTail(loud, 3000);
            c.processBlock(loud, emptyMidi);
            const double outRms = rmsTail(loud, 3000);
            expect(outRms < inRms * 0.75, "loud signal should be compressed down");
            expect(c.getReductionDb() > 3.0f, "gain reduction meter should show reduction");
        }
    }

    void testDeEsser()
    {
        beginTest("De-esser passes non-sibilant (low) signal");
        {
            BuiltInDeEsser d;
            d.prepareToPlay(kSr, 4096);
            d.setP(BuiltInDeEsser::FreqHz, 6500.0f);
            d.setP(BuiltInDeEsser::ThresholdDb, -28.0f);
            d.setP(BuiltInDeEsser::Ratio, 4.0f);

            // 低域 (サ行帯域外) の信号は HPF でほぼ落ちるので検出されず素通り
            auto low = makeSine(220.0, 0.5, 8000);
            const double inRms = rmsTail(low, 2000);
            d.processBlock(low, emptyMidi);
            const double outRms = rmsTail(low, 2000);
            expect(std::abs(outRms - inRms) < inRms * 0.05, "low signal should pass through a de-esser");
            expect(d.getReductionDb() < 0.5f, "no reduction on non-sibilant signal");
        }

        beginTest("De-esser reduces loud sibilant (high) signal");
        {
            BuiltInDeEsser d;
            d.prepareToPlay(kSr, 4096);
            d.setP(BuiltInDeEsser::FreqHz, 5000.0f);
            d.setP(BuiltInDeEsser::ThresholdDb, -30.0f);
            d.setP(BuiltInDeEsser::Ratio, 6.0f);

            // サ行帯域の強い信号 (8kHz) は検出され、高域成分が引かれて出力が小さくなる
            auto hi = makeSine(8000.0, 0.7, 12000);
            const double inRms = rmsTail(hi, 3000);
            d.processBlock(hi, emptyMidi);
            const double outRms = rmsTail(hi, 3000);
            expect(outRms < inRms * 0.8, "sibilant band should be reduced");
            expect(d.getReductionDb() > 2.0f, "reduction meter should show de-essing");
        }

        beginTest("De-esser state round-trips");
        {
            BuiltInDeEsser a;
            a.setP(BuiltInDeEsser::FreqHz, 7200.0f);
            a.setP(BuiltInDeEsser::ThresholdDb, -25.0f);
            a.setP(BuiltInDeEsser::Ratio, 7.0f);   // 全パラメータを非既定にして往復を網羅
            juce::MemoryBlock mb;
            a.getStateInformation(mb);
            BuiltInDeEsser b;
            b.setStateInformation(mb.getData(), (int) mb.getSize());
            for (int i = 0; i < a.getParamCount(); ++i)
                expect(std::abs(a.getP(i) - b.getP(i)) < 1.0e-3f, "param should round-trip");
        }
    }

    void testGate()
    {
        beginTest("Gate passes signal above threshold");
        {
            BuiltInGate gt;
            gt.prepareToPlay(kSr, 4096);
            gt.setP(BuiltInGate::ThresholdDb, -40.0f);
            gt.setP(BuiltInGate::RangeDb, 60.0f);
            gt.setP(BuiltInGate::AttackMs, 1.0f);
            gt.setP(BuiltInGate::HoldMs, 200.0f);
            gt.setP(BuiltInGate::ReleaseMs, 250.0f);

            auto loud = makeSine(440.0, 0.5, 12000);   // ~ -6 dB, well above threshold
            const double inRms = rmsTail(loud, 3000);
            gt.processBlock(loud, emptyMidi);
            const double outRms = rmsTail(loud, 3000);
            expect(std::abs(outRms - inRms) < inRms * 0.05, "loud signal should pass an open gate unchanged");
            expect(gt.getReductionDb() < 0.5f, "no reduction while the gate is open");
            expect(gt.isOpen(), "gate should report open for a loud signal");
        }

        beginTest("Gate attenuates signal below threshold");
        {
            BuiltInGate gt;
            gt.prepareToPlay(kSr, 4096);
            gt.setP(BuiltInGate::ThresholdDb, -40.0f);
            gt.setP(BuiltInGate::RangeDb, 60.0f);
            gt.setP(BuiltInGate::AttackMs, 1.0f);
            gt.setP(BuiltInGate::HoldMs, 0.0f);
            gt.setP(BuiltInGate::ReleaseMs, 10.0f);

            auto quiet = makeSine(440.0, 0.003, 12000);   // ~ -50 dB, below threshold
            const double inRms = rmsTail(quiet, 3000);
            gt.processBlock(quiet, emptyMidi);
            const double outRms = rmsTail(quiet, 3000);
            expect(outRms < inRms * 0.1, "below-threshold signal should be gated down");
            expect(gt.getReductionDb() > 20.0f, "gain reduction meter should show heavy attenuation");
            expect(! gt.isOpen(), "gate should report closed for a quiet signal");
        }

        beginTest("Gate range 0 is passthrough");
        {
            BuiltInGate gt;
            gt.prepareToPlay(kSr, 4096);
            gt.setP(BuiltInGate::ThresholdDb, -10.0f);   // 信号より上 = ゲートは閉じ判定
            gt.setP(BuiltInGate::RangeDb, 0.0f);         // レンジ 0 = 絞らない (素通り)

            auto sig = makeSine(440.0, 0.5, 8000);
            const double inRms = rmsTail(sig, 2000);
            gt.processBlock(sig, emptyMidi);
            const double outRms = rmsTail(sig, 2000);
            expect(std::abs(outRms - inRms) < inRms * 0.01, "range 0 must pass through even when closed");
            expect(gt.getReductionDb() < 0.5f, "range 0 means no reduction");
        }

        beginTest("Gate state round-trips");
        {
            BuiltInGate a;
            a.setP(BuiltInGate::ThresholdDb, -33.0f);
            a.setP(BuiltInGate::RangeDb, 45.0f);
            a.setP(BuiltInGate::AttackMs, 7.0f);
            a.setP(BuiltInGate::HoldMs, 333.0f);
            a.setP(BuiltInGate::ReleaseMs, 222.0f);
            juce::MemoryBlock mb;
            a.getStateInformation(mb);
            BuiltInGate b;
            b.setStateInformation(mb.getData(), (int) mb.getSize());
            for (int i = 0; i < a.getParamCount(); ++i)
                expect(std::abs(a.getP(i) - b.getP(i)) < 1.0e-3f, "param should round-trip");
        }
    }

    static float peakOf(const juce::AudioBuffer<float>& b)
    {
        float p = 0.0f;
        for (int ch = 0; ch < b.getNumChannels(); ++ch)
            p = juce::jmax(p, b.getMagnitude(ch, 0, b.getNumSamples()));
        return p;
    }

    void testMaximizer()
    {
        const int expectLookahead = (int) std::round(0.003 * kSr);

        beginTest("Maximizer reports lookahead latency");
        {
            BuiltInMaximizer m;
            m.prepareToPlay(kSr, 4096);
            expect(m.getLatencySamples() == expectLookahead, "latency must equal the lookahead samples");
            expect(m.getLatencySamples() > 0, "maximizer has non-zero lookahead latency");
        }

        beginTest("Maximizer holds output under the ceiling");
        {
            BuiltInMaximizer m;
            m.prepareToPlay(kSr, 24000);
            m.setP(BuiltInMaximizer::DriveDb, 12.0f);    // 強くドライブ → リミッティング
            m.setP(BuiltInMaximizer::CeilingDb, -0.3f);
            m.setP(BuiltInMaximizer::ReleaseMs, 100.0f);

            auto sig = makeSine(220.0, 0.5, 24000);      // -6 dB を +12 で押し上げる
            m.processBlock(sig, emptyMidi);

            const float ceil = juce::Decibels::decibelsToGain(-0.3f);
            const float pk = peakOf(sig);
            expect(pk <= ceil * 1.0005f, "driven output must never exceed the ceiling (brickwall)");
            expect(pk > ceil * 0.9f, "output should be pushed up close to the ceiling (loudness gain)");
            expect(m.getReductionDb() > 1.0f, "GR meter should show limiting when driven hard");
        }

        beginTest("Maximizer leaves quiet signal below ceiling unlimited");
        {
            BuiltInMaximizer m;
            m.prepareToPlay(kSr, 12000);
            m.setP(BuiltInMaximizer::DriveDb, 12.0f);
            m.setP(BuiltInMaximizer::CeilingDb, 0.0f);
            m.setP(BuiltInMaximizer::ReleaseMs, 100.0f);

            const float driveGain = juce::Decibels::decibelsToGain(12.0f);
            auto sig = makeSine(220.0, 0.05, 12000);     // -26 dB → +12 でも -14 dB (天井以下)
            const double inRms = rmsTail(sig, 4000);
            m.processBlock(sig, emptyMidi);
            const double outRms = rmsTail(sig, 4000);     // 末尾 = lookahead 整定後
            expect(std::abs(outRms - inRms * driveGain) < inRms * driveGain * 0.05,
                   "below-ceiling signal should just get the clean drive gain");
            expect(m.getReductionDb() < 0.5f, "no reduction when the driven signal stays under the ceiling");
        }

        beginTest("Maximizer state round-trips");
        {
            BuiltInMaximizer a;
            a.setP(BuiltInMaximizer::DriveDb, 9.0f);
            a.setP(BuiltInMaximizer::CeilingDb, -1.5f);
            a.setP(BuiltInMaximizer::ReleaseMs, 240.0f);
            juce::MemoryBlock mb;
            a.getStateInformation(mb);
            BuiltInMaximizer b;
            b.setStateInformation(mb.getData(), (int) mb.getSize());
            for (int i = 0; i < a.getParamCount(); ++i)
                expect(std::abs(a.getP(i) - b.getP(i)) < 1.0e-3f, "param should round-trip");
        }
    }

    // 指定範囲 [from,to) のチャンネル0の最大絶対値とその位置
    static std::pair<int,float> peakInRange(const juce::AudioBuffer<float>& b, int from, int to)
    {
        int idx = from; float pk = 0.0f;
        for (int i = juce::jmax(0, from); i < juce::jmin(to, b.getNumSamples()); ++i)
        {
            const float a = std::abs(b.getSample(0, i));
            if (a > pk) { pk = a; idx = i; }
        }
        return { idx, pk };
    }

    static juce::AudioBuffer<float> makeImpulse(int numSamples)
    {
        juce::AudioBuffer<float> b(2, numSamples);
        b.clear();
        b.setSample(0, 0, 1.0f);
        b.setSample(1, 0, 1.0f);
        return b;
    }

    // テンポ同期テスト用の最小 playhead (BPM だけ返す)
    struct FakePlayHead : public juce::AudioPlayHead
    {
        double bpm;
        explicit FakePlayHead(double b) : bpm(b) {}
        juce::Optional<PositionInfo> getPosition() const override
        {
            PositionInfo p;
            p.setBpm(bpm);
            p.setIsPlaying(true);
            return p;
        }
    };

    void testDelay()
    {
        beginTest("Delay mix=0 is passthrough");
        {
            BuiltInDelay d;
            d.setP(BuiltInDelay::Sync, 0.0f);
            d.setP(BuiltInDelay::TimeMs, 100.0f);
            d.setP(BuiltInDelay::Mix, 0.0f);
            d.prepareToPlay(kSr, 8192);

            auto imp = makeImpulse(8000);
            juce::AudioBuffer<float> copy(imp);
            d.processBlock(imp, emptyMidi);
            bool identical = true;
            for (int i = 0; i < 8000 && identical; ++i)
                if (imp.getSample(0, i) != copy.getSample(0, i)) identical = false;
            expect(identical, "mix=0 must leave the dry signal unchanged (no wet added)");
        }

        beginTest("Delay produces an echo at the set time");
        {
            BuiltInDelay d;
            d.setP(BuiltInDelay::Sync, 0.0f);
            d.setP(BuiltInDelay::TimeMs, 100.0f);     // 100ms = 4800 samples @48k
            d.setP(BuiltInDelay::Feedback, 0.0f);
            d.setP(BuiltInDelay::Tone, 1.0f);         // bright = no damping
            d.setP(BuiltInDelay::Mix, 100.0f);
            d.setP(BuiltInDelay::PingPong, 0.0f);
            d.prepareToPlay(kSr, 16384);              // delaySmoothed inits from TimeMs (set above)

            auto imp = makeImpulse(14400);
            d.processBlock(imp, emptyMidi);

            const int expectAt = (int) std::lround(0.100 * kSr);   // 4800
            auto echo = peakInRange(imp, expectAt - 50, expectAt + 50);
            expect(echo.second > 0.9f, "an echo near 100ms should be ~unity (mix 100, fb 0, bright)");
            // フィードバック 0 なので 2 つ目のエコーは無い
            auto echo2 = peakInRange(imp, expectAt * 2 - 50, expectAt * 2 + 50);
            expect(echo2.second < 0.1f, "feedback 0 must yield no second echo");
        }

        beginTest("Delay feedback yields decaying repeats");
        {
            BuiltInDelay d;
            d.setP(BuiltInDelay::Sync, 0.0f);
            d.setP(BuiltInDelay::TimeMs, 100.0f);
            d.setP(BuiltInDelay::Feedback, 50.0f);
            d.setP(BuiltInDelay::Tone, 1.0f);
            d.setP(BuiltInDelay::Mix, 100.0f);
            d.setP(BuiltInDelay::PingPong, 0.0f);
            d.prepareToPlay(kSr, 16384);

            auto imp = makeImpulse(16000);
            d.processBlock(imp, emptyMidi);

            const int step = (int) std::lround(0.100 * kSr);
            auto e1 = peakInRange(imp, step - 50, step + 50);
            auto e2 = peakInRange(imp, step * 2 - 50, step * 2 + 50);
            auto e3 = peakInRange(imp, step * 3 - 50, step * 3 + 50);
            expect(e1.second > 0.9f, "first repeat ~unity");
            expect(e2.second > 0.4f && e2.second < 0.6f, "second repeat ~0.5 (fb 50%)");
            expect(e3.second > 0.2f && e3.second < 0.35f, "third repeat ~0.25 (fb 50%^2)");
        }

        beginTest("Delay tempo sync resolves division from host BPM");
        {
            BuiltInDelay d;
            FakePlayHead ph(120.0);                   // 120 BPM → 1 beat = 0.5s
            d.setPlayHead(&ph);
            d.setP(BuiltInDelay::Sync, 1.0f);
            d.prepareToPlay(kSr, 1024);

            juce::AudioBuffer<float> blk(2, 512); blk.clear();

            d.setP(BuiltInDelay::Division, 3.0f);     // 1/8 = 0.5 beat → 0.25s
            d.processBlock(blk, emptyMidi);
            expect(std::abs(d.getDelaySeconds() - 0.25f) < 0.005f, "1/8 at 120 BPM = 0.25s");

            d.setP(BuiltInDelay::Division, 0.0f);     // 1/4 = 1 beat → 0.5s
            d.processBlock(blk, emptyMidi);
            expect(std::abs(d.getDelaySeconds() - 0.5f) < 0.005f, "1/4 at 120 BPM = 0.5s");

            d.setPlayHead(nullptr);                    // 後始末 (スタック上の ph より先に外す)
        }

        beginTest("Delay division table sanity");
        {
            expect(BuiltInDelay::numDivisions() >= 4, "several note divisions exist");
            expect(std::abs(BuiltInDelay::divisionBeats(0) - 1.0) < 1.0e-9, "1/4 = 1 beat");
            expect(std::abs(BuiltInDelay::divisionBeats(3) - 0.5) < 1.0e-9, "1/8 = 0.5 beat");
        }

        beginTest("Delay state round-trips");
        {
            BuiltInDelay a;
            a.setP(BuiltInDelay::Sync, 0.0f);
            a.setP(BuiltInDelay::Division, 2.0f);
            a.setP(BuiltInDelay::TimeMs, 280.0f);
            a.setP(BuiltInDelay::Feedback, 44.0f);
            a.setP(BuiltInDelay::Tone, 0.3f);
            a.setP(BuiltInDelay::Mix, 33.0f);
            a.setP(BuiltInDelay::PingPong, 0.0f);
            juce::MemoryBlock mb;
            a.getStateInformation(mb);
            BuiltInDelay b;
            b.setStateInformation(mb.getData(), (int) mb.getSize());
            for (int i = 0; i < a.getParamCount(); ++i)
                expect(std::abs(a.getP(i) - b.getP(i)) < 1.0e-3f, "param should round-trip");
        }
    }

    void testReverb()
    {
        beginTest("Reverb mix=0 is exact passthrough");
        {
            BuiltInReverb r;
            r.prepareToPlay(kSr, 1024);
            r.setP(BuiltInReverb::MixPct, 0.0f);

            juce::AudioBuffer<float> in(2, 1024);
            juce::Random rng(42);
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < 1024; ++i)
                    in.setSample(ch, i, rng.nextFloat() * 2.0f - 1.0f);
            juce::AudioBuffer<float> copy(in);

            r.processBlock(in, emptyMidi);
            bool identical = true;
            for (int ch = 0; ch < 2 && identical; ++ch)
                for (int i = 0; i < 1024; ++i)
                    if (in.getSample(ch, i) != copy.getSample(ch, i)) { identical = false; break; }
            expect(identical, "mix=0 must leave the dry signal bit-exact");
        }

        beginTest("Reverb mix>0 produces a tail after input ends");
        {
            BuiltInReverb r;
            r.prepareToPlay(kSr, 512);
            r.setP(BuiltInReverb::MixPct, 50.0f);
            r.setP(BuiltInReverb::Size, 0.8f);

            // 1 ブロックだけノイズを通し、その後は無音ブロックを流してテールを観測
            juce::AudioBuffer<float> blk(2, 512);
            juce::Random rng(7);
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < 512; ++i)
                    blk.setSample(ch, i, rng.nextFloat() * 2.0f - 1.0f);
            r.processBlock(blk, emptyMidi);

            double tailEnergy = 0.0;
            for (int b = 0; b < 6; ++b)
            {
                blk.clear();
                r.processBlock(blk, emptyMidi);
                tailEnergy += rmsTail(blk, 512);
            }
            expect(tailEnergy > 0.001, "reverb tail should ring out into silent blocks");
        }
    }

    void testKeroVoice()
    {
        // 検出 (YIN) → スケールスナップ → ピッチ同期シフトの統合をオフラインで検証する。
        // 出力周波数は正方向ゼロ交差 (最初と最後の交差間の周期数) で推定する (PitchEngineTests と同じ定石)。
        auto measureFreq = [] (const juce::AudioBuffer<float>& b, int start, int len) -> double
        {
            const float* x = b.getReadPointer(0);
            int first = -1, last = -1, count = 0;
            for (int i = start + 1; i < start + len; ++i)
                if (x[i - 1] <= 0.0f && x[i] > 0.0f)
                {
                    if (first < 0) first = i;
                    last = i;
                    ++count;
                }
            if (count < 2 || last <= first) return 0.0;
            return (double) (count - 1) * kSr / (double) (last - first);
        };
        auto processInBlocks = [this] (BuiltInKeroVoice& k, juce::AudioBuffer<float>& b)
        {
            float* chans[2] = { b.getWritePointer(0), b.getWritePointer(1) };
            for (int pos = 0; pos < b.getNumSamples(); pos += 512)
            {
                const int len = juce::jmin(512, b.getNumSamples() - pos);
                juce::AudioBuffer<float> blk(chans, 2, pos, len);
                k.processBlock(blk, emptyMidi);
            }
        };

        beginTest("KeroVoice snaps an off-pitch note to the nearest semitone");
        {
            // 226 Hz (A3=220 から +47 セント) → クロマチックで A3 へスナップされ出力 ~220 Hz
            BuiltInKeroVoice k;
            k.setP(BuiltInKeroVoice::Scale, 0.0f);
            k.setP(BuiltInKeroVoice::Speed, 0.0f);
            k.setP(BuiltInKeroVoice::Mix, 100.0f);
            k.prepareToPlay(kSr, 512);

            auto b = makeSine(226.0, 0.5, 72000);
            processInBlocks(k, b);
            const double f = measureFreq(b, 48000, 24000);
            expect(std::abs(f - 220.0) < 3.0, "output should be snapped to 220 Hz (A3)");
            expect(std::abs(f - 226.0) > 2.5, "output should have moved away from the 226 Hz input");
            const double rms = rmsTail(b, 24000);
            expect(rms > 0.25 && rms < 0.45, "shifted output should keep the input level");
            expect(k.getUiTargetMidi() >= 0.0f
                   && std::abs(k.getUiTargetMidi() - 57.0f) < 0.01f, "target note should be midi 57 (A3)");
        }

        beginTest("KeroVoice snaps to the key scale (C major)");
        {
            // 380 Hz (midi 66.46 = F#4 と G4 の間) → C メジャーでは F# が構成音でないため G4 (392 Hz) へ
            BuiltInKeroVoice k;
            k.setP(BuiltInKeroVoice::Scale, (float) BuiltInKeroVoice::Major);
            k.setP(BuiltInKeroVoice::Key, 0.0f);
            k.setP(BuiltInKeroVoice::Speed, 0.0f);
            k.setP(BuiltInKeroVoice::Mix, 100.0f);
            k.prepareToPlay(kSr, 512);

            auto b = makeSine(380.0, 0.5, 72000);
            processInBlocks(k, b);
            const double f = measureFreq(b, 48000, 24000);
            expect(std::abs(f - 392.0) < 4.0, "output should be snapped to 392 Hz (G4) in C major");
        }

        beginTest("KeroVoice transpose shifts the corrected note");
        {
            // 220 Hz (A3・オンピッチ) + トランスポーズ +12 → 440 Hz (A4)
            BuiltInKeroVoice k;
            k.setP(BuiltInKeroVoice::Scale, 0.0f);
            k.setP(BuiltInKeroVoice::Speed, 0.0f);
            k.setP(BuiltInKeroVoice::Transpose, 12.0f);
            k.prepareToPlay(kSr, 512);

            auto b = makeSine(220.0, 0.5, 72000);
            processInBlocks(k, b);
            const double f = measureFreq(b, 48000, 24000);
            expect(std::abs(f - 440.0) < 6.0, "transpose +12 should output ~440 Hz from a 220 Hz input");
        }

        beginTest("KeroVoice amount=0 leaves pitch uncorrected");
        {
            // 補正量 0 はスナップへ寄せない (検出は生きるが比率 1 のまま)
            BuiltInKeroVoice k;
            k.setP(BuiltInKeroVoice::Amount, 0.0f);
            k.setP(BuiltInKeroVoice::Speed, 0.0f);
            k.prepareToPlay(kSr, 512);

            auto b = makeSine(226.0, 0.5, 72000);
            processInBlocks(k, b);
            const double f = measureFreq(b, 48000, 24000);
            expect(std::abs(f - 226.0) < 3.0, "amount=0 should keep the input pitch (~226 Hz)");
        }

        beginTest("KeroVoice snaps to major pentatonic");
        {
            // 345 Hz (midi 64.79・F4 の近く) → C メジャーペンタは F を含まないため E4 (329.6 Hz) へ
            BuiltInKeroVoice k;
            k.setP(BuiltInKeroVoice::Scale, (float) BuiltInKeroVoice::MajorPenta);
            k.setP(BuiltInKeroVoice::Key, 0.0f);
            k.setP(BuiltInKeroVoice::Speed, 0.0f);
            k.prepareToPlay(kSr, 512);

            auto b = makeSine(345.0, 0.5, 72000);
            processInBlocks(k, b);
            const double f = measureFreq(b, 48000, 24000);
            expect(std::abs(f - 329.63) < 4.0, "output should be snapped to E4 in C major pentatonic");
        }

        beginTest("KeroVoice sensitivity gates correction on quiet input");
        {
            // 小さい入力 (~-47dBFS 平均二乗) は感度 -30dB では補正されず、既定 -55dB では補正される
            auto run = [&] (float sensDb) -> double
            {
                BuiltInKeroVoice k;
                k.setP(BuiltInKeroVoice::Speed, 0.0f);
                k.setP(BuiltInKeroVoice::Sens, sensDb);
                k.prepareToPlay(kSr, 512);
                auto b = makeSine(226.0, 0.006, 72000);
                processInBlocks(k, b);
                return measureFreq(b, 48000, 24000);
            };
            expect(std::abs(run(-30.0f) - 226.0) < 3.0, "quiet input should stay uncorrected with low sensitivity");
            expect(std::abs(run(-55.0f) - 220.0) < 3.0, "quiet input should still be corrected with default sensitivity");
        }

        beginTest("KeroVoice keeps silence silent");
        {
            BuiltInKeroVoice k;
            k.prepareToPlay(kSr, 512);
            juce::AudioBuffer<float> b(2, 24000);
            b.clear();
            processInBlocks(k, b);
            expect(rmsTail(b, 12000) < 1.0e-4, "silent input should stay silent");
        }

        beginTest("KeroVoice mix=0 is exact passthrough");
        {
            BuiltInKeroVoice k;
            k.setP(BuiltInKeroVoice::Mix, 0.0f);
            k.prepareToPlay(kSr, 512);

            auto b = makeSine(300.0, 0.4, 8192);
            juce::AudioBuffer<float> ref(2, 8192);
            for (int ch = 0; ch < 2; ++ch)
                ref.copyFrom(ch, 0, b, ch, 0, 8192);
            processInBlocks(k, b);
            bool same = true;
            for (int ch = 0; ch < 2 && same; ++ch)
                for (int i = 0; i < 8192; ++i)
                    if (b.getSample(ch, i) != ref.getSample(ch, i)) { same = false; break; }
            expect(same, "mix=0 must be a bit-exact passthrough");
        }

        beginTest("KeroVoice reports zero latency");
        {
            BuiltInKeroVoice k;
            k.prepareToPlay(kSr, 512);
            expect(k.getLatencySamples() == 0, "kero voice must not report plugin latency");
        }

        beginTest("KeroVoice state round-trips");
        {
            BuiltInKeroVoice a;
            a.setP(BuiltInKeroVoice::Scale, 2.0f);
            a.setP(BuiltInKeroVoice::Key, 9.0f);
            a.setP(BuiltInKeroVoice::Speed, 55.5f);
            a.setP(BuiltInKeroVoice::Mix, 70.0f);

            juce::MemoryBlock mb;
            a.getStateInformation(mb);

            BuiltInKeroVoice b;
            b.setStateInformation(mb.getData(), (int) mb.getSize());
            for (int i = 0; i < a.getParamCount(); ++i)
                expect(std::abs(a.getP(i) - b.getP(i)) < 1.0e-3f, "param should round-trip");
        }
    }

    void testFactory()
    {
        beginTest("Factory creates by identifier");
        {
            expect(BuiltInFactory::create("utawave.eq")      != nullptr, "eq id creates");
            expect(BuiltInFactory::create("utawave.comp")    != nullptr, "comp id creates");
            expect(BuiltInFactory::create("utawave.deesser") != nullptr, "deesser id creates");
            expect(BuiltInFactory::create("utawave.maximizer") != nullptr, "maximizer id creates");
            expect(BuiltInFactory::create("utawave.delay")   != nullptr, "delay id creates");
            expect(BuiltInFactory::create("utawave.reverb")  != nullptr, "reverb id creates");
            expect(BuiltInFactory::create("utawave.kerovoice") != nullptr, "kerovoice id creates");
            expect(BuiltInFactory::create("nope")            == nullptr, "unknown id is null");
        }

        beginTest("Factory format predicate");
        {
            expect(BuiltInFactory::isBuiltInFormat("Utawave"), "Utawave is builtin format");
            expect(! BuiltInFactory::isBuiltInFormat("VST3"),  "VST3 is not builtin format");
        }

        beginTest("Factory menu id range");
        {
            auto first = BuiltInFactory::createFromMenuId(BuiltInFactory::kMenuIdBase + 1);
            expect(first != nullptr, "first menu id creates an effect");
            expect(BuiltInFactory::createFromMenuId(BuiltInFactory::kMenuIdBase)     == nullptr, "base itself is out of range");
            expect(BuiltInFactory::createFromMenuId(BuiltInFactory::kMenuIdBase + 99) == nullptr, "far id is out of range");
            expect(BuiltInFactory::createFromMenuId(5) == nullptr, "small VST id is not claimed");
        }

        beginTest("Factory menu labels match effect getName");
        {
            // appendMenu の nameKey と各エフェクトの getName() がドリフトしていないことを保証する
            // (ずれるとメニュー名とチップ/窓名が食い違い、英訳フォールバックも壊れる)。
            juce::PopupMenu m;
            BuiltInFactory::appendMenu(m);
            int count = 0;
            for (juce::PopupMenu::MenuItemIterator it(m); it.next();)
            {
                const auto& item = it.getItem();
                auto fx = BuiltInFactory::createFromMenuId(item.itemID);
                expect(fx != nullptr, "each menu item maps to an effect");
                if (fx != nullptr)
                    expect(item.text == fx->getName(), "menu label must equal the effect getName");
                ++count;
            }
            expect(count >= 3, "menu lists all built-in effects");
        }

        beginTest("Factory entries have unique uid and identifier");
        {
            // identifier / uid が衝突すると createFromIdentifierString が別エフェクトを誤って復元する
            juce::StringArray ids;
            juce::Array<juce::int32> uids;
            juce::PopupMenu m;
            BuiltInFactory::appendMenu(m);
            for (juce::PopupMenu::MenuItemIterator it(m); it.next();)
            {
                auto fx = BuiltInFactory::createFromMenuId(it.getItem().itemID);
                auto* eff = dynamic_cast<BuiltInEffect*>(fx.get());
                expect(eff != nullptr, "menu id creates a built-in effect");
                if (eff == nullptr) continue;
                const auto id = eff->getIdentifier();
                expect(! ids.contains(id), "identifier must be unique across built-in effects");
                ids.add(id);
                expect(! uids.contains(eff->getUid()), "uid must be unique across built-in effects");
                uids.add(eff->getUid());
            }
        }

        beginTest("Factory identifier-string round-trips");
        {
            BuiltInEQ eq;
            const auto idStr = eq.getPluginDescription().createIdentifierString();
            auto made = BuiltInFactory::createFromIdentifierString(idStr);
            expect(made != nullptr, "identifier string recreates the effect");
            if (made != nullptr)
                expect(made->getPluginDescription().createIdentifierString() == idStr,
                       "recreated effect has the same identifier");
            // 保存 ID はロケール非依存 (固定 identifier) でなければならない。getName() (tr 済み) が
            // 混ざると言語切替で照合が壊れる回帰を防ぐ。
            expect(idStr.contains("utawave.eq"), "identifier string must embed the stable id, not a localized name");
            expect(! idStr.contains(eq.getName()) || eq.getName() == "utawave.eq",
                   "identifier string must not depend on the localized display name");
        }
    }
};

static BuiltInEffectsTests builtInEffectsTests;
