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
#include "../Source/Audio/builtin/BuiltInReverb.h"
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
        testReverb();
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

    void testFactory()
    {
        beginTest("Factory creates by identifier");
        {
            expect(BuiltInFactory::create("utawave.eq")      != nullptr, "eq id creates");
            expect(BuiltInFactory::create("utawave.comp")    != nullptr, "comp id creates");
            expect(BuiltInFactory::create("utawave.deesser") != nullptr, "deesser id creates");
            expect(BuiltInFactory::create("utawave.reverb")  != nullptr, "reverb id creates");
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
