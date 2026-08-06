// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

// Utawave — PluginChain の安全網ユニットテスト
//
// (1) 出力サニタイズ: プラグインが内部再構成中 (UI のモード切替等) に吐く NaN / Inf /
//     未初期化メモリ由来の巨大値を processBlock が修復し、爆音がそのまま出力へ直行したり
//     後段プラグインの内部状態を汚染したりしないこと (「VST のボタン操作で爆音バチッ」の回帰テスト)。
//     正常な信号 (±kMaxPluginSample 以内) はビット単位で不変であること。
// (2) レイテンシ変更通知: プラグインの setLatencySamples (ルックアヘッド切替等) が
//     onLatencyChanged としてメッセージスレッドへ届くこと (PDC 再構築の配線の土台)。
//     レイテンシ以外の変更では発火せず、チェーンから取り出したプラグインからは届かないこと。
//
// 依存: PluginChain.cpp (リンク済み)。実プラグインの代わりに最小スタブを使う
// (PluginActionsTests の FakePlugin と同じ作法・デバイス/スキャン不要)。

#include <JuceHeader.h>
#include <cmath>
#include "../Source/VST/PluginChain.h"

namespace
{

// 最小の AudioPluginInstance スタブ基底 (stereo I/O)
class ChainFakePluginBase : public juce::AudioPluginInstance
{
public:
    ChainFakePluginBase()
        : juce::AudioPluginInstance(BusesProperties()
              .withInput ("In",  juce::AudioChannelSet::stereo())
              .withOutput("Out", juce::AudioChannelSet::stereo())) {}

    const juce::String getName() const override            { return "ChainFake"; }
    void prepareToPlay(double, int) override               {}
    void releaseResources() override                       {}
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override {}
    using juce::AudioProcessor::processBlock;
    double getTailLengthSeconds() const override           { return 0.0; }
    bool acceptsMidi() const override                      { return false; }
    bool producesMidi() const override                     { return false; }
    juce::AudioProcessorEditor* createEditor() override    { return nullptr; }
    bool hasEditor() const override                        { return false; }
    int getNumPrograms() override                          { return 1; }
    int getCurrentProgram() override                       { return 0; }
    void setCurrentProgram(int) override                   {}
    const juce::String getProgramName(int) override        { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock&) override  {}
    void setStateInformation(const void*, int) override    {}
    void fillInPluginDescription(juce::PluginDescription& d) const override { d.name = getName(); }

    // protected の setLatencySamples をテストから叩けるように公開する
    void reportLatency(int samples) { setLatencySamples(samples); }
};

// 全サンプルを固定値で上書きする (NaN / Inf / 巨大値 = 再構成中のゴミ出力を模す)
class FillFakePlugin : public ChainFakePluginBase
{
public:
    explicit FillFakePlugin(float v) : fillValue(v) {}
    float fillValue;
    void processBlock(juce::AudioBuffer<float>& b, juce::MidiBuffer&) override
    {
        for (int ch = 0; ch < b.getNumChannels(); ++ch)
        {
            float* d = b.getWritePointer(ch);
            for (int i = 0; i < b.getNumSamples(); ++i)
                d[i] = fillValue;
        }
    }
};

// 自分に入ってきた信号を記録する (前段のサニタイズが効いているかの観測用)。信号は素通し
class CaptureFakePlugin : public ChainFakePluginBase
{
public:
    float seenMaxAbs { 0.0f };
    bool  seenNonFinite { false };
    void processBlock(juce::AudioBuffer<float>& b, juce::MidiBuffer&) override
    {
        for (int ch = 0; ch < b.getNumChannels(); ++ch)
        {
            const float* d = b.getReadPointer(ch);
            for (int i = 0; i < b.getNumSamples(); ++i)
            {
                if (! std::isfinite(d[i])) seenNonFinite = true;
                else seenMaxAbs = juce::jmax(seenMaxAbs, std::abs(d[i]));
            }
        }
    }
};

class PluginChainTests : public juce::UnitTest
{
public:
    PluginChainTests() : juce::UnitTest("PluginChain (safety)") {}

    void runTest() override
    {
        testNaNIsRepairedToSilence();
        testInfIsRepairedToSilence();
        testHugeValuesAreClamped();
        testCleanSignalIsBitExact();
        testHotButLegitSignalUntouched();
        testDownstreamPluginIsProtected();
        testLatencyChangeNotification();
        testBypassSkipsProcessing();
    }

private:
    static juce::AudioBuffer<float> makeBuffer(float value, int numSamples = 256)
    {
        juce::AudioBuffer<float> b(2, numSamples);
        for (int ch = 0; ch < 2; ++ch)
        {
            float* d = b.getWritePointer(ch);
            for (int i = 0; i < numSamples; ++i)
                d[i] = value;
        }
        return b;
    }

    // バイパスの基本動作: バイパス中のプラグインは processBlock で叩かれず信号が素通りし、
    // 解除すると再び処理される (チップ左のバイパススイッチ / Cmd+クリックの土台)
    void testBypassSkipsProcessing()
    {
        beginTest("Bypassed plugin is skipped; un-bypass processes again");
        PluginChain chain;
        chain.addPlugin(std::make_unique<FillFakePlugin>(0.75f));

        {
            auto buf = makeBuffer(0.25f);
            juce::MidiBuffer midi;
            chain.processBlock(buf, midi);
            expectEquals(buf.getSample(0, 0), 0.75f, "active plugin processes the block");
        }

        chain.setBypassed(0, true);
        expect(chain.isBypassed(0), "bypass flag is set");
        {
            auto buf = makeBuffer(0.25f);
            juce::MidiBuffer midi;
            chain.processBlock(buf, midi);
            bool untouched = true;
            for (int ch = 0; ch < buf.getNumChannels(); ++ch)
            {
                const float* d = buf.getReadPointer(ch);
                for (int i = 0; i < buf.getNumSamples(); ++i)
                    untouched = untouched && (d[i] == 0.25f);
            }
            expect(untouched, "bypassed plugin leaves the signal bit-exact");
        }

        chain.setBypassed(0, false);
        expect(!chain.isBypassed(0), "bypass flag is cleared");
        {
            auto buf = makeBuffer(0.25f);
            juce::MidiBuffer midi;
            chain.processBlock(buf, midi);
            expectEquals(buf.getSample(0, 0), 0.75f, "un-bypassed plugin processes again");
        }
    }

    void testNaNIsRepairedToSilence()
    {
        beginTest("NaN output is repaired to silence");
        PluginChain chain;
        chain.addPlugin(std::make_unique<FillFakePlugin>(std::numeric_limits<float>::quiet_NaN()));

        auto buf = makeBuffer(0.25f);
        juce::MidiBuffer midi;
        chain.processBlock(buf, midi);

        bool allZero = true;
        for (int ch = 0; ch < buf.getNumChannels(); ++ch)
        {
            const float* d = buf.getReadPointer(ch);
            for (int i = 0; i < buf.getNumSamples(); ++i)
                allZero = allZero && (d[i] == 0.0f);
        }
        expect(allZero, "NaN block must become silence (0), not reach the output");
    }

    void testInfIsRepairedToSilence()
    {
        beginTest("Inf output is repaired to silence");
        PluginChain chain;
        chain.addPlugin(std::make_unique<FillFakePlugin>(std::numeric_limits<float>::infinity()));

        auto buf = makeBuffer(0.25f);
        juce::MidiBuffer midi;
        chain.processBlock(buf, midi);

        bool allZero = true;
        for (int ch = 0; ch < buf.getNumChannels(); ++ch)
        {
            const float* d = buf.getReadPointer(ch);
            for (int i = 0; i < buf.getNumSamples(); ++i)
                allZero = allZero && (d[i] == 0.0f);
        }
        expect(allZero, "Inf block must become silence (0)");
    }

    void testHugeValuesAreClamped()
    {
        beginTest("Huge garbage values are clamped to +/- kMaxPluginSample");
        {
            PluginChain chain;
            chain.addPlugin(std::make_unique<FillFakePlugin>(1.0e10f));
            auto buf = makeBuffer(0.25f);
            juce::MidiBuffer midi;
            chain.processBlock(buf, midi);
            bool clamped = true;
            for (int ch = 0; ch < 2; ++ch)
            {
                const float* d = buf.getReadPointer(ch);
                for (int i = 0; i < buf.getNumSamples(); ++i)
                    clamped = clamped && (d[i] == PluginChain::kMaxPluginSample);
            }
            expect(clamped, "1e10 must be clamped to +kMaxPluginSample");
        }
        {
            PluginChain chain;
            chain.addPlugin(std::make_unique<FillFakePlugin>(-1.0e10f));
            auto buf = makeBuffer(0.25f);
            juce::MidiBuffer midi;
            chain.processBlock(buf, midi);
            bool clamped = true;
            for (int ch = 0; ch < 2; ++ch)
            {
                const float* d = buf.getReadPointer(ch);
                for (int i = 0; i < buf.getNumSamples(); ++i)
                    clamped = clamped && (d[i] == -PluginChain::kMaxPluginSample);
            }
            expect(clamped, "-1e10 must be clamped to -kMaxPluginSample (sign preserved)");
        }
    }

    void testCleanSignalIsBitExact()
    {
        beginTest("Clean pass-through signal is bit-exact (guard does not alter it)");
        PluginChain chain;
        chain.addPlugin(std::make_unique<ChainFakePluginBase>());   // 素通し

        const int n = 256;
        juce::AudioBuffer<float> buf(2, n);
        juce::Random rng(12345);
        juce::AudioBuffer<float> ref(2, n);
        for (int ch = 0; ch < 2; ++ch)
        {
            float* d = buf.getWritePointer(ch);
            float* r = ref.getWritePointer(ch);
            for (int i = 0; i < n; ++i)
                d[i] = r[i] = rng.nextFloat() * 2.0f - 1.0f;   // ±1.0 の通常信号
        }

        juce::MidiBuffer midi;
        chain.processBlock(buf, midi);

        bool identical = true;
        for (int ch = 0; ch < 2; ++ch)
        {
            const float* d = buf.getReadPointer(ch);
            const float* r = ref.getReadPointer(ch);
            for (int i = 0; i < n; ++i)
                identical = identical && (d[i] == r[i]);
        }
        expect(identical, "normal signal must pass bit-exact");
    }

    void testHotButLegitSignalUntouched()
    {
        beginTest("Hot-but-legit inter-plugin signal (below limit) is untouched");
        PluginChain chain;
        const float hot = PluginChain::kMaxPluginSample - 0.1f;
        chain.addPlugin(std::make_unique<FillFakePlugin>(hot));

        auto buf = makeBuffer(0.25f);
        juce::MidiBuffer midi;
        chain.processBlock(buf, midi);

        bool untouched = true;
        for (int ch = 0; ch < 2; ++ch)
        {
            const float* d = buf.getReadPointer(ch);
            for (int i = 0; i < buf.getNumSamples(); ++i)
                untouched = untouched && (d[i] == hot);
        }
        expect(untouched, "signal just below kMaxPluginSample must not be clamped");
    }

    void testDownstreamPluginIsProtected()
    {
        beginTest("Sanitization happens per plugin: downstream plugin never sees garbage");
        PluginChain chain;
        chain.addPlugin(std::make_unique<FillFakePlugin>(std::numeric_limits<float>::quiet_NaN()));
        auto capture = std::make_unique<CaptureFakePlugin>();
        auto* cap = capture.get();
        chain.addPlugin(std::move(capture));

        auto buf = makeBuffer(0.25f);
        juce::MidiBuffer midi;
        chain.processBlock(buf, midi);

        expect(! cap->seenNonFinite, "downstream plugin must not receive NaN/Inf");
        expect(cap->seenMaxAbs <= PluginChain::kMaxPluginSample,
               "downstream plugin must not receive over-limit samples");
    }

    void testLatencyChangeNotification()
    {
        beginTest("setLatencySamples reaches onLatencyChanged on the message thread");
        PluginChain chain;
        auto plugin = std::make_unique<ChainFakePluginBase>();
        auto* p = plugin.get();
        chain.addPlugin(std::move(plugin));

        int notified = 0;
        chain.onLatencyChanged = [&notified] { ++notified; };

        // レイテンシ変更 → AsyncUpdater 経由でメッセージスレッドに届く
        p->reportLatency(64);
        juce::MessageManager::getInstance()->runDispatchLoopUntil(80);
        expectEquals(notified, 1, "latency change must notify once");

        // レイテンシ以外の変更 (プログラム切替等) では発火しない
        p->updateHostDisplay(juce::AudioProcessorListener::ChangeDetails().withProgramChanged(true));
        juce::MessageManager::getInstance()->runDispatchLoopUntil(80);
        expectEquals(notified, 1, "non-latency change must not notify");

        // チェーンから取り出したプラグイン (D&D 移動でチェーンを離れた状態) からは届かない
        auto taken = chain.extractPlugin(0);
        expect(taken != nullptr, "extractPlugin must return the instance");
        static_cast<ChainFakePluginBase*>(taken.get())->reportLatency(128);
        juce::MessageManager::getInstance()->runDispatchLoopUntil(80);
        expectEquals(notified, 1, "extracted plugin must no longer notify this chain");
    }
};

static PluginChainTests pluginChainTests;

} // namespace
