// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

// Utawave — AudioEngine 実時間パス (audioDeviceIOCallbackWithContext) のユニットテスト
//
// オーディオデバイスの代わりにスタブ (FakeAudioIODevice) で audioDeviceAboutToStart を
// 通し、デバイスコールバックをテストスレッドから直接駆動する。これにより従来
// renderOfflineRange 経由でしか検証できなかった「再生ブランチ」(スナップショット grab /
// renderClip / Mute・Solo / 停止時無音 / clearPlayback バリア / 遅延破棄 + 再構築) を
// デバイス無し・決定論的に検証する。
//
// 定石:
// - コールバックは単一スレッド (テストスレッド) から逐次呼ぶ。audio thread と UI thread の
//   並行性そのものは対象外 (lock-free 構造のレースは実機 QA / TSan の領分)
// - 内容検証は ExportEngineTests と同じく const 値 WAV + fade 0 + vol 0dB / pan 0 で行う
//   (pan はリニアバランス則・center 減衰なし、mono は L/R 複製)

#include <JuceHeader.h>
#include <cmath>

#include "../Source/Audio/AudioEngine.h"
#include "../Source/Tracks/TrackManager.h"
#include "../Source/Tracks/Track.h"
#include "../Source/Tracks/AudioClip.h"
#include "../Source/VST/PluginChain.h"
#include "../Source/Recording/RecordingManager.h"

namespace
{
constexpr double kSR    = 48000.0;
constexpr int    kBlock = 512;

// 一定ゲインを掛けるだけの最小スタブプラグイン (モニタ FX 経路の検証用)。
// 信号がチェーンを通ったか (= ゲインが乗ったか) を出力ピークで判定できる。
class GainFakePlugin : public juce::AudioPluginInstance
{
public:
    explicit GainFakePlugin(float g)
        : juce::AudioPluginInstance(BusesProperties()
              .withInput ("In",  juce::AudioChannelSet::stereo())
              .withOutput("Out", juce::AudioChannelSet::stereo())),
          gain(g) {}
    float gain { 1.0f };
    const juce::String getName() const override            { return "Gain"; }
    void prepareToPlay(double, int) override               {}
    void releaseResources() override                       {}
    void processBlock(juce::AudioBuffer<float>& b, juce::MidiBuffer&) override { b.applyGain(gain); }
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
};

// prepareToPlay されたときだけゲインを掛けるスタブ (未 prepare では素通り)。
// 「モニター返しのチェーンが確実に prepare される」保証を検証するため。
class PrepareGatedFakePlugin : public juce::AudioPluginInstance
{
public:
    explicit PrepareGatedFakePlugin(float g)
        : juce::AudioPluginInstance(BusesProperties()
              .withInput ("In",  juce::AudioChannelSet::stereo())
              .withOutput("Out", juce::AudioChannelSet::stereo())),
          gain(g) {}
    float gain { 1.0f };
    bool  prepared { false };
    const juce::String getName() const override            { return "PrepGate"; }
    void prepareToPlay(double, int) override               { prepared = true; }
    void releaseResources() override                       { prepared = false; }
    void processBlock(juce::AudioBuffer<float>& b, juce::MidiBuffer&) override
        { if (prepared) b.applyGain(gain); }   // 未 prepare なら何もしない
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
};

// 入力に関係なく buffer を定数で埋める最小スタブ (Melodyne が「停止中に自前でプレビュー音を
// 生成する」挙動を模す)。停止中プラグインプレビューの検証用。
class GeneratorFakePlugin : public juce::AudioPluginInstance
{
public:
    explicit GeneratorFakePlugin(float v)
        : juce::AudioPluginInstance(BusesProperties()
              .withInput ("In",  juce::AudioChannelSet::stereo())
              .withOutput("Out", juce::AudioChannelSet::stereo())),
          value(v) {}
    float value { 0.0f };
    const juce::String getName() const override            { return "Gen"; }
    void prepareToPlay(double, int) override               {}
    void releaseResources() override                       {}
    void processBlock(juce::AudioBuffer<float>& b, juce::MidiBuffer&) override
    {
        for (int ch = 0; ch < b.getNumChannels(); ++ch)
            juce::FloatVectorOperations::fill(b.getWritePointer(ch), value, b.getNumSamples());
    }
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
};

// audioDeviceAboutToStart に渡す最小スタブ。SR / buffer size / チャンネル構成だけを返す。
struct FakeAudioIODevice : public juce::AudioIODevice
{
    FakeAudioIODevice() : juce::AudioIODevice("FakeDevice", "FakeType") {}

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
    int getOutputLatencyInSamples() override                  { return outLatency; }
    int getInputLatencyInSamples() override                   { return inLatency; }

    // 録音レイテンシ補正テスト用 (既定 0 = 他テストへの影響なし)
    int inLatency  { 0 };
    int outLatency { 0 };
};

bool writeMonoConstWav(const juce::File& f, int numSamples, float value)
{
    juce::AudioBuffer<float> b(1, numSamples);
    juce::FloatVectorOperations::fill(b.getWritePointer(0), value, numSamples);
    juce::WavAudioFormat waf;
    using SF = juce::AudioFormatWriterOptions::SampleFormat;
    auto wopts = juce::AudioFormatWriterOptions{}
                     .withSampleRate(kSR).withNumChannels(1)
                     .withBitsPerSample(32).withSampleFormat(SF::floatingPoint);
    f.getParentDirectory().createDirectory();
    f.deleteFile();
    auto fos = std::make_unique<juce::FileOutputStream>(f);
    if (!fos->openedOk()) return false;
    std::unique_ptr<juce::OutputStream> os = std::move(fos);
    std::unique_ptr<juce::AudioFormatWriter> w(waf.createWriterFor(os, wopts));
    return w != nullptr && w->writeFromAudioSampleBuffer(b, 0, numSamples);
}

// 変化する (正弦波) モノ WAV。const だとストリーミングが壊れていても一致してしまうため、
// ディスクストリーミングの決定論テストには「位置で値が変わる」ソースを使う。
bool writeMonoSineWav(const juce::File& f, int numSamples, double freq)
{
    juce::AudioBuffer<float> b(1, numSamples);
    for (int i = 0; i < numSamples; ++i)
        b.setSample(0, i, 0.5f * (float) std::sin(2.0 * juce::MathConstants<double>::pi * freq * i / kSR));
    juce::WavAudioFormat waf;
    using SF = juce::AudioFormatWriterOptions::SampleFormat;
    auto wopts = juce::AudioFormatWriterOptions{}
                     .withSampleRate(kSR).withNumChannels(1)
                     .withBitsPerSample(32).withSampleFormat(SF::floatingPoint);
    f.getParentDirectory().createDirectory();
    f.deleteFile();
    auto fos = std::make_unique<juce::FileOutputStream>(f);
    if (!fos->openedOk()) return false;
    std::unique_ptr<juce::OutputStream> os = std::move(fos);
    std::unique_ptr<juce::AudioFormatWriter> w(waf.createWriterFor(os, wopts));
    return w != nullptr && w->writeFromAudioSampleBuffer(b, 0, numSamples);
}

// numBlocks 分コールバックを駆動し、全出力サンプル (2ch) をまとめたバッファを返す。
// sleepEvery>0 なら sleepEvery ブロックごとに少し眠り、先読みスレッドにリングを満たす隙を与える
// (ストリーミング ON 経路でリングヒットを実際に行使するため。出力比較自体は値のみで時刻非依存)。
juce::AudioBuffer<float> captureOutput(AudioEngine& engine, int numBlocks, int sleepEvery = 0)
{
    juce::AudioBuffer<float> all(2, numBlocks * kBlock);
    all.clear();
    juce::AudioBuffer<float> out(2, kBlock);
    for (int b = 0; b < numBlocks; ++b)
    {
        out.clear();
        float* chans[2] = { out.getWritePointer(0), out.getWritePointer(1) };
        engine.audioDeviceIOCallbackWithContext(nullptr, 0, chans, 2, kBlock, {});
        all.copyFrom(0, b * kBlock, out, 0, 0, kBlock);
        all.copyFrom(1, b * kBlock, out, 1, 0, kBlock);
        if (sleepEvery > 0 && (b % sleepEvery) == 0) juce::Thread::sleep(3);
    }
    return all;
}

// numBlocks 分、const 値のモノ入力つきでコールバックを駆動する (録音ゲートテスト用)
void runBlocksWithInput(AudioEngine& engine, int numBlocks, float inputValue)
{
    juce::AudioBuffer<float> out(2, kBlock), in(1, kBlock);
    juce::FloatVectorOperations::fill(in.getWritePointer(0), inputValue, kBlock);
    const float* ins[1] = { in.getReadPointer(0) };
    for (int i = 0; i < numBlocks; ++i)
    {
        out.clear();
        float* chans[2] = { out.getWritePointer(0), out.getWritePointer(1) };
        engine.audioDeviceIOCallbackWithContext(ins, 1, chans, 2, kBlock, {});
    }
}

// numBlocks 分コールバックを駆動し、全ブロックの L/R 絶対値ピークを返す
juce::Range<float> runBlocks(AudioEngine& engine, int numBlocks,
                             float* outPeakL = nullptr, float* outPeakR = nullptr)
{
    juce::AudioBuffer<float> out(2, kBlock);
    float peakL = 0.0f, peakR = 0.0f;
    for (int i = 0; i < numBlocks; ++i)
    {
        out.clear();
        float* chans[2] = { out.getWritePointer(0), out.getWritePointer(1) };
        engine.audioDeviceIOCallbackWithContext(nullptr, 0, chans, 2, kBlock, {});
        peakL = juce::jmax(peakL, out.getMagnitude(0, 0, kBlock));
        peakR = juce::jmax(peakR, out.getMagnitude(1, 0, kBlock));
    }
    if (outPeakL) *outPeakL = peakL;
    if (outPeakR) *outPeakR = peakR;
    return { juce::jmin(peakL, peakR), juce::jmax(peakL, peakR) };
}
} // namespace

struct AudioEngineRealtimeTests : public juce::UnitTest
{
    AudioEngineRealtimeTests() : juce::UnitTest("AudioEngine realtime callback") {}

    juce::File tempDir;

    void runTest() override
    {
        tempDir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                      .getChildFile("UtawaveAudioEngineTests");
        tempDir.deleteRecursively();
        tempDir.createDirectory();

        testPlaybackRendersClip();
        testNotPlayingIsSilent();
        testMuteAndSolo();
        testSoloSilencesClick();
        testMultiTrackClipIndexRouting();
        testClearPlaybackBarrier();
        testDeferredDestructionRebuild();
        testRecordingLatencyComp();
        testRecordingWriteGate();
        testRecordingFirstWriteMarker();
        testRetroFirstWriteMarker();
        testStereoWriterMonoInput();
        testLoopWrapFromOutside();
        testMonitorThroughInserts();
        testStoppedPluginPreview();
        testDiskStreamingDeterminism();
        testMulticoreDeterminism();
        testEmptyRangeOfflineRender();

        tempDir.deleteRecursively();
    }

    // シーン構築 + デバイス開始の共通部
    struct Scene
    {
        juce::AudioFormatManager fmt;
        std::unique_ptr<TrackManager> tm;
        FakeAudioIODevice device;
        AudioEngine engine;
        Scene()
        {
            fmt.registerBasicFormats();
            tm = std::make_unique<TrackManager>(fmt);
        }
        Track* addConstTrack(const juce::File& wav, double dur)
        {
            auto* t = tm->addTrack({}, false);
            auto* c = t->addClip(wav, 0.0, dur);
            c->setFadeInSecs(0.0); c->setFadeOutSecs(0.0);
            t->setVolume(0.0f); t->setPan(0.0f);
            return t;
        }
        void start()
        {
            engine.audioDeviceAboutToStart(&device);
            engine.preparePlayback(*tm);
            engine.setPosition(0.0);
        }
    };

    void testEmptyRangeOfflineRender()
    {
        beginTest("offline render: sub-sample range returns a valid empty 2ch buffer");

        // endSec > startSec だが丸めで 0 サンプルになる極小範囲 (1µs < 半サンプル @48k)。
        // 最大ズームのルーラードラッグで作れる。旧実装は早期 return が outBuffer.setSize より
        // 前にあり、呼び出し側 (ExportEngine::render) に 0 チャンネルバッファがそのまま返って
        // writer の writeFromAudioSampleBuffer が debug で jassert していた (回帰テスト)。
        Scene s;
        s.tm->addTrack({}, false);
        s.start();

        juce::AudioBuffer<float> out;   // デフォルト構築 = 0ch (ExportEngine::render と同じ渡し方)
        s.engine.renderOfflineRange(0.0, 1.0e-6, out);
        expectEquals(out.getNumChannels(), 2, "empty range yields a 2-channel buffer");
        expectEquals(out.getNumSamples(),  0, "empty range yields 0 samples");
    }

    void testDiskStreamingDeterminism()
    {
        beginTest("disk streaming: output is bit-identical to direct read");
        // ストリーミング ON (先読みボイス経由・リングヒット行使) と OFF (従来の同期読み) で、
        // 再生出力がサンプル単位で完全一致することを確認する (FileStreamVoice の配線が出力を
        // 変えない = 決定論)。値が位置で変わる正弦波ソースを使い、誤読をサンプル差として検出する。
        auto wav = tempDir.getChildFile("stream_sine.wav");
        expect(writeMonoSineWav(wav, (int) (kSR * 2.0), 220.0), "sine source write");

        const int N = 120;   // 120 * 512 / 48000 ≈ 1.28s

        // OFF: ボイスを作らせない (start 前に無効化)
        Scene off;
        off.addConstTrack(wav, 2.0);
        off.engine.setDiskStreamingEnabled(false);
        off.start();
        off.engine.play();
        auto aOff = captureOutput(off.engine, N);

        // ON: ボイス生成 (既定 ON)。sleepEvery で先読みを行使し、リングヒット経路を通す。
        Scene on;
        on.addConstTrack(wav, 2.0);
        on.engine.setDiskStreamingEnabled(true);
        on.start();
        on.engine.play();
        auto bOn = captureOutput(on.engine, N, /*sleepEvery*/ 4);

        bool identical = true;
        float maxDiff = 0.0f;
        for (int ch = 0; ch < 2 && identical; ++ch)
            for (int i = 0; i < N * kBlock; ++i)
            {
                const float d = std::abs(aOff.getSample(ch, i) - bOn.getSample(ch, i));
                if (d > maxDiff) maxDiff = d;
                if (d > 1.0e-7f) { identical = false; break; }
            }
        expect(identical, "streaming ON output must equal streaming OFF (direct) output sample-for-sample");
        // ストリーミング ON でも有音 (ソースが届いている) ことを確認 (両方無音で一致する偽合格を防ぐ)
        expect(bOn.getMagnitude(0, 0, N * kBlock) > 0.1f, "streaming ON actually renders audio");
    }

    void testMulticoreDeterminism()
    {
        beginTest("multicore: output is bit-identical to single-thread");
        // 複数トラック (>= kMinTracksForThreads) を並列描画 (ワーカー強制起動) した出力が、
        // 単一スレッド処理とサンプル単位で完全一致することを確認する。produce(並列) + 直列 sum の
        // 再構成が、加算順を固定する限りスレッド数に依らずビット同一であることの担保。
        const int kTracks = 6;
        std::vector<juce::File> wavs;
        for (int k = 0; k < kTracks; ++k)
        {
            auto f = tempDir.getChildFile("mc_" + juce::String(k) + ".wav");
            expect(writeMonoSineWav(f, (int) (kSR * 1.5), 160.0 + 50.0 * k), "mc source write");
            wavs.push_back(f);
        }

        auto buildScene = [&] (Scene& s, bool multi, int workers)
        {
            for (int k = 0; k < kTracks; ++k)
            {
                auto* t = s.addConstTrack(wavs[(size_t) k], 1.5);
                t->setVolume(-2.0f * (float) k);                 // フェーダーをばらけさせる
                t->setPan((k % 2) ? 0.3f : -0.3f);               // パンもばらけさせる
                if (k == 2) t->setReverbSend(0.4f);              // リバーブ送りも経路に含める
            }
            s.engine.setForcedAudioWorkerCountForTests(workers); // start 前に固定
            s.engine.setMulticoreAudioEnabled(multi);
            s.start();
            s.engine.play();
        };

        const int N = 100;
        Scene off; buildScene(off, /*multi*/ false, /*workers*/ 0);
        auto aOff = captureOutput(off.engine, N);

        Scene on;  buildScene(on,  /*multi*/ true,  /*workers*/ 3);
        expect(on.engine.getAudioWorkerCount() == 3, "3 audio workers started (parallel path engaged)");
        auto bOn = captureOutput(on.engine, N, /*sleepEvery*/ 2);

        bool identical = true;
        for (int ch = 0; ch < 2 && identical; ++ch)
            for (int i = 0; i < N * kBlock; ++i)
                if (std::abs(aOff.getSample(ch, i) - bOn.getSample(ch, i)) > 1.0e-7f)
                    { identical = false; break; }
        expect(identical, "multicore output must equal single-thread output sample-for-sample");
        expect(bOn.getMagnitude(0, 0, N * kBlock) > 0.1f, "multicore path actually renders audio");
    }

    void testPlaybackRendersClip()
    {
        beginTest("playing: clip content reaches both output channels");
        auto wav = tempDir.getChildFile("c05.wav");
        expect(writeMonoConstWav(wav, (int)kSR, 0.5f), "source write");

        Scene s;
        s.addConstTrack(wav, 1.0);
        s.start();
        s.engine.play();

        float pl = 0, pr = 0;
        runBlocks(s.engine, 10, &pl, &pr);
        // mono クリップは L/R 複製・pan center は減衰なし・vol 0dB → ピークはほぼ 0.5
        expect(std::abs(pl - 0.5f) < 0.01f, "L peak ~0.5");
        expect(std::abs(pr - 0.5f) < 0.01f, "R peak ~0.5");

        // 再生位置がブロック数ぶん前進している (10 * 512 / 48000)
        const double expected = 10.0 * kBlock / kSR;
        expect(std::abs(s.engine.getCurrentPositionSeconds() - expected) < (kBlock / kSR) + 1e-6,
               "position advances by rendered blocks");
    }

    void testNotPlayingIsSilent()
    {
        beginTest("stopped: callback outputs silence and position holds");
        auto wav = tempDir.getChildFile("c05b.wav");
        expect(writeMonoConstWav(wav, (int)kSR, 0.5f), "source write");

        Scene s;
        s.addConstTrack(wav, 1.0);
        s.start();
        // play() しない
        auto peaks = runBlocks(s.engine, 4);
        expectEquals(peaks.getEnd(), 0.0f, "silent while stopped");
        expectEquals(s.engine.getCurrentPositionSeconds(), 0.0, "position unchanged");

        // 再生 → 停止後も無音に戻る
        s.engine.play();
        runBlocks(s.engine, 2);
        s.engine.stop();
        peaks = runBlocks(s.engine, 2);
        expectEquals(peaks.getEnd(), 0.0f, "silent after stop");
    }

    void testMuteAndSolo()
    {
        beginTest("mute / solo are honoured by the realtime mix");
        auto wavA = tempDir.getChildFile("a04.wav");
        auto wavB = tempDir.getChildFile("b02.wav");
        expect(writeMonoConstWav(wavA, (int)kSR, 0.4f), "source A write");
        expect(writeMonoConstWav(wavB, (int)kSR, 0.2f), "source B write");

        Scene s;
        auto* ta = s.addConstTrack(wavA, 1.0);
        auto* tb = s.addConstTrack(wavB, 1.0);
        s.start();
        s.engine.play();

        // 両方有効 → 0.4 + 0.2 = 0.6
        float pl = 0, pr = 0;
        runBlocks(s.engine, 3, &pl, &pr);
        expect(std::abs(pl - 0.6f) < 0.01f, "both tracks mix to ~0.6");

        // A を Mute → 0.2 のみ (Track::muted は atomic、再構築不要で即反映)
        ta->setMuted(true);
        runBlocks(s.engine, 3, &pl, &pr);
        expect(std::abs(pl - 0.2f) < 0.01f, "muted track drops out (~0.2)");
        ta->setMuted(false);

        // B を Solo → 0.2 のみ
        tb->setSoloed(true);
        runBlocks(s.engine, 3, &pl, &pr);
        expect(std::abs(pl - 0.2f) < 0.01f, "solo silences the other track (~0.2)");
    }

    // ── ソロ中は CLICK (メトロノーム) も黙る (要望 2026-07 の回帰テスト) ──
    // 旧実装はメトロノーム合成にソロ規則が無く、他トラックをソロにしても
    // クリックだけ鳴り続けていた。クリック自身をソロにした場合は鳴る。
    void testSoloSilencesClick()
    {
        beginTest("solo on another track silences the click (metronome)");
        auto wav = tempDir.getChildFile("clk_solo.wav");
        expect(writeMonoConstWav(wav, (int)(2 * kSR), 0.5f), "source write");

        Scene s;
        auto* inst  = s.addConstTrack(wav, 2.0);
        auto* click = s.tm->addClickTrack();
        click->setVolume(0.0f);   // クリック基本音量 = 0.5 (gain 1.0 × 0.5)
        s.start();                // preparePlayback で metronomeEnabled = 非ミュート
        s.engine.play();

        // (A) inst をソロ: クリックは鳴らず、出力は定数 0.5 のみ
        //     (拍 0 のクリックがソロブロックで抑止されることも兼ねて先に検証)
        inst->setSoloed(true);
        float pl = 0, pr = 0;
        runBlocks(s.engine, 10, &pl, &pr);
        expect(std::abs(pl - 0.5f) < 0.005f, "solo blocks the click (const 0.5 only)");

        // (B) ソロ解除: 次の拍 (0.5s) からクリックが混ざり、ピークが定数を超える
        inst->setSoloed(false);
        runBlocks(s.engine, 60, &pl, &pr);   // ~0.64s ぶん = 拍 0.5s を跨ぐ
        expect(pl > 0.55f, "click audible again after unsolo");

        // (C) CLICK 自身をソロ: クリックだけが鳴る = 拍でピークが立ち、拍間は無音
        //     (CLICK のソロが anySolo に含まれず inst が鳴り続けていたバグの回帰テスト)
        click->setSoloed(true);
        float maxPk = 0.0f, minPk = 1.0e9f;
        for (int i = 0; i < 60; ++i)
        {
            float a = 0, b = 0;
            runBlocks(s.engine, 1, &a, &b);
            maxPk = juce::jmax(maxPk, a);
            minPk = juce::jmin(minPk, a);
        }
        expect(maxPk > 0.3f,  "click audible when click itself is soloed");
        expect(minPk < 0.01f, "instrument silenced by click solo (quiet between beats)");
        click->setSoloed(false);
    }

    void testMultiTrackClipIndexRouting()
    {
        beginTest("multi-track / multi-clip routing via clipsByTrack index");
        // PlaybackSnapshot のトラック別インデックス (clipsByTrack / clipTracks) 経路の検証:
        // 複数トラック × 複数クリップで「正しいトラックの正しいクリップだけが鳴る」こと、
        // Mute / Solo / 両方の組み合わせが clipTracks ベースの判定で効くことを確認する。
        auto wavA1 = tempDir.getChildFile("idx_a1.wav");   // Track A クリップ1 = 0.1
        auto wavA2 = tempDir.getChildFile("idx_a2.wav");   // Track A クリップ2 = 0.3
        auto wavB  = tempDir.getChildFile("idx_b.wav");    // Track B = 0.2
        auto wavC  = tempDir.getChildFile("idx_c.wav");    // Track C = 0.15 (常時 Mute)
        expect(writeMonoConstWav(wavA1, (int)kSR, 0.1f),  "source A1 write");
        expect(writeMonoConstWav(wavA2, (int)kSR, 0.3f),  "source A2 write");
        expect(writeMonoConstWav(wavB,  (int)kSR, 0.2f),  "source B write");
        expect(writeMonoConstWav(wavC,  (int)kSR, 0.15f), "source C write");

        Scene s;
        auto* ta = s.addConstTrack(wavA1, 0.5);            // クリップ1 [0, 0.5)
        auto* a2 = ta->addClip(wavA2, 0.5, 0.5);           // クリップ2 [0.5, 1.0) 同一トラック
        a2->setFadeInSecs(0.0); a2->setFadeOutSecs(0.0);
        auto* tb = s.addConstTrack(wavB, 1.0);
        auto* tc = s.addConstTrack(wavC, 1.0);
        tc->setMuted(true);                                // 構築時から Mute (active 収集から除外)
        s.start();
        s.engine.play();

        // t≈0: A クリップ1 + B (C は Mute) → 0.1 + 0.2 = 0.3
        float pl = 0, pr = 0;
        runBlocks(s.engine, 3, &pl, &pr);
        expect(std::abs(pl - 0.3f) < 0.01f, "t~0: A clip1 + B mix to ~0.3");

        // 後半へシーク: 同一トラックの 2 つ目のクリップに切り替わる → 0.3 + 0.2 = 0.5
        s.engine.setPosition(0.6);
        runBlocks(s.engine, 3, &pl, &pr);
        expect(std::abs(pl - 0.5f) < 0.01f, "t~0.6: A clip2 + B mix to ~0.5");

        // B を Solo → B のみ (~0.2)。clipTracks ベースの Solo 判定
        tb->setSoloed(true);
        runBlocks(s.engine, 3, &pl, &pr);
        expect(std::abs(pl - 0.2f) < 0.01f, "solo B: only B audible (~0.2)");
        tb->setSoloed(false);

        // A を Solo → A クリップ2 のみ (~0.3)
        ta->setSoloed(true);
        runBlocks(s.engine, 3, &pl, &pr);
        expect(std::abs(pl - 0.3f) < 0.01f, "solo A: only A clip2 audible (~0.3)");

        // Solo 中の A を Mute → Mute が優先され無音 (B は非 Solo で除外)
        ta->setMuted(true);
        auto peaks = runBlocks(s.engine, 3);
        expectEquals(peaks.getEnd(), 0.0f, "muted solo track yields silence");
    }

    void testClearPlaybackBarrier()
    {
        beginTest("clearPlayback: returns promptly and callback stays safe");
        auto wav = tempDir.getChildFile("c05c.wav");
        expect(writeMonoConstWav(wav, (int)kSR, 0.5f), "source write");

        Scene s;
        s.addConstTrack(wav, 1.0);
        s.start();
        s.engine.play();
        runBlocks(s.engine, 2);

        // audio thread が居ないので旧スナップショットは即時 drain される (テストが
        // ハングせず完走すること自体が 500ms バリアの検証)
        const auto t0 = juce::Time::getMillisecondCounterHiRes();
        s.engine.clearPlayback();
        expect(juce::Time::getMillisecondCounterHiRes() - t0 < 400.0,
               "clearPlayback returns without waiting for the timeout");

        // スナップショット切替直後は旧音からのデクリック・クロスフェードが入る (仕様)。
        // フェードを流し切ってから無音を確認する
        runBlocks(s.engine, 10);
        auto peaks = runBlocks(s.engine, 3);
        expectEquals(peaks.getEnd(), 0.0f, "silent after clearPlayback (post declick)");
    }

    void testDeferredDestructionRebuild()
    {
        beginTest("deferClipDestruction + invalidatePlayback while playing");
        auto wav = tempDir.getChildFile("c05d.wav");
        expect(writeMonoConstWav(wav, (int)kSR, 0.5f), "source write");

        Scene s;
        auto* t = s.addConstTrack(wav, 1.0);
        s.start();
        s.engine.play();
        runBlocks(s.engine, 2);

        // 破棄系編集の経路を再現: クリップを lane から外し、所有権を遅延破棄へ渡してから
        // invalidatePlayback (再生中なので即 rebuild)。コールバックは無音になり、クラッシュしない
        auto* lane = t->getLane(0);
        expect(lane != nullptr && !lane->clips.empty(), "lane has the clip");
        std::vector<std::unique_ptr<AudioClip>> removed;
        removed.push_back(std::move(lane->clips[0]));
        lane->clips.erase(lane->clips.begin());
        s.engine.deferClipDestruction(std::move(removed));
        s.engine.invalidatePlayback();

        // 切替デクリックを流し切ってから判定 (clearPlayback と同様)
        runBlocks(s.engine, 10);
        auto peaks = runBlocks(s.engine, 3);
        expectEquals(peaks.getEnd(), 0.0f, "silent after the clip was removed (post declick)");
    }

    // ── 録音レイテンシ補正: デバイス報告値 + 手動オフセットの合成 ──
    void testRecordingLatencyComp()
    {
        beginTest("recording latency comp = device round trip + manual offset");
        AudioEngine engine;
        FakeAudioIODevice dev;
        dev.inLatency  = 480;   // 10ms @48k
        dev.outLatency = 960;   // 20ms @48k
        engine.audioDeviceAboutToStart(&dev);

        expectWithinAbsoluteError(engine.getDeviceRoundTripLatencySecs(), 0.030, 1e-9);

        engine.setRecordingLatencyComp(true, 5.0);
        expectWithinAbsoluteError(engine.getRecordingLatencyCompSecs(), 0.035, 1e-9);

        engine.setRecordingLatencyComp(false, 12.0);   // 自動 OFF は手動分のみ
        expectWithinAbsoluteError(engine.getRecordingLatencyCompSecs(), 0.012, 1e-9);

        engine.setRecordingLatencyComp(true, -40.0);   // 手動マイナスで自動分を打ち消せる
        expectWithinAbsoluteError(engine.getRecordingLatencyCompSecs(), -0.010, 1e-9);

        engine.audioDeviceStopped();
    }

    // ── 録音書き込みゲート: カウントイン遡及録音 + ループラップ後の継続 ──
    // recordingWriteFromSecs (書き込み開始) と recordingStartSecs (パンチインミュート位置) の
    // 分離、およびラップ時にゲートがループ頭へ移動することを実コールバック駆動で検証する。
    // ゲートが動かないと 2 周目以降「ループ頭〜録音開始位置」が録られず、テイクのスライスが
    // 累積的にずれる (本番で起きた回帰)
    void testRecordingWriteGate()
    {
        beginTest("recording write gate: count-in retro capture + keeps writing across loop wrap");
        const double blockSecs = (double)kBlock / kSR;

        Scene s;
        s.start();   // トラック/クリップ無しで再生だけ回す (録音ブランチは入力から書く)

        // 実 ThreadedWriter (録音ターゲット登録には非 null writer が必要)
        juce::TimeSliceThread bg("RecGateTestThread");
        bg.startThread();
        auto wav = tempDir.getChildFile("gate.wav");
        std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter> tw;
        {
            juce::WavAudioFormat waf;
            using SF = juce::AudioFormatWriterOptions::SampleFormat;
            auto wopts = juce::AudioFormatWriterOptions{}
                             .withSampleRate(kSR).withNumChannels(1)
                             .withBitsPerSample(32).withSampleFormat(SF::floatingPoint);
            wav.deleteFile();
            auto fos = std::make_unique<juce::FileOutputStream>(wav);
            expect(fos->openedOk(), "writer stream open");
            std::unique_ptr<juce::OutputStream> os = std::move(fos);
            std::unique_ptr<juce::AudioFormatWriter> w(waf.createWriterFor(os, wopts));
            expect(w != nullptr, "writer create");
            tw = std::make_unique<juce::AudioFormatWriter::ThreadedWriter>(w.release(), bg, 65536);
        }
        s.engine.setRecordingTarget(tw.get(), nullptr, 0, false);

        // ── A: カウントイン遡及録音 (writeFrom 0.2 < recStart 0.5) ──
        // 再生 0.0 から開始し、書き込みは 0.2 から (R 位置 0.5 を待たない)
        s.engine.setRecordingActive(true, 0.5, 0.2);
        s.engine.setPosition(0.0);
        s.engine.play();
        const int blocksA = 94;   // ~1.0s
        runBlocksWithInput(s.engine, blocksA, 0.25f);
        const double elapsedA  = blocksA * blockSecs;
        const double recordedA = (double)s.engine.getRecordedSampleCount() / kSR;
        expectWithinAbsoluteError(recordedA, elapsedA - 0.2, 2.0 * blockSecs);
        s.engine.stop();
        s.engine.setRecordingActive(false);

        // ── B: ループ内の途中から録音 → ラップ後も書き続ける (ゲートがループ頭へ移動) ──
        // loop [0.3, 0.8] / recStart = writeFrom = 0.5 / 再生は 0.45 から
        s.engine.setLoopRange(0.3, 0.8, true);
        s.engine.setRecordingActive(true, 0.5, 0.5);
        s.engine.setPosition(0.45);
        s.engine.play();
        const int blocksB = 60;   // ~0.64s (途中で 0.8 → 0.3 へ 1 回ラップ)
        runBlocksWithInput(s.engine, blocksB, 0.25f);
        const double elapsedB  = blocksB * blockSecs;
        const double recordedB = (double)s.engine.getRecordedSampleCount() / kSR;
        // 期待値 = 全駆動時間 − ゲート前 (0.45 → 0.5 の 0.05s)。ラップ後 (0.3 → ...) も
        // 全て録音される。ゲートが移動しない旧バグでは 0.3 → 0.5 が毎周欠けて大きく下回る
        expectWithinAbsoluteError(recordedB, elapsedB - 0.05, 3.0 * blockSecs);
        s.engine.stop();
        s.engine.setRecordingActive(false);
        s.engine.setLoopRange(0.0, 0.0, false);

        // 後始末 (teardown バリアは audio thread 不在のため即時)
        s.engine.setRecordingTarget(nullptr, nullptr);
        tw.reset();
        bg.stopThread(2000);
        expect(wav.existsAsFile() && wav.getSize() > 0, "recorded file has data");
    }

    void testRecordingFirstWriteMarker()
    {
        // 回帰テスト: 録音ターゲットの登録は play() 後の message thread で行われるため、
        // 再生開始〜登録完了の 0〜数ブロックはファイルに書かれない (ファイル先頭が writeFrom
        // より遅れる)。旧実装は「ファイル先頭 = writeFrom」仮定で配置しており、その取りこぼし
        // 分だけクリップ内容が手前へずれていた (Q リテイクで並べたテイクがブロック粒度で
        // ランダムにずれて見えた報告の原因)。エンジンの実書き込み開始マーカー
        // (getRecordingFirstWritePosSecs) を使う修正で、登録が遅れても時間対応が正確になる。
        beginTest("recording: placement uses actual first-write position (late target registration)");
        const double blockSecs = (double)kBlock / kSR;

        Scene s;
        s.start();

        RecordingManager mgr(s.engine, *s.tm, s.fmt);
        auto recDir = tempDir.getChildFile("firstwrite_rec");
        mgr.getAudioFolder = [recDir] { return recDir; };

        auto* track = s.tm->addTrack({}, false);
        track->setRecArmed(true);

        // ── 通常録音: 再生を先に回してから録音登録 (登録遅れ 4 ブロックを模擬) ──
        const int lateBlocks = 4;
        s.engine.setPosition(1.0);
        s.engine.play();
        runBlocksWithInput(s.engine, lateBlocks, 0.25f);   // 登録前 = まだ書かれない
        expect(mgr.startRecording(1.0, 1.0), "recording starts");
        runBlocksWithInput(s.engine, 40, 0.25f);
        mgr.stopRecording(s.engine.getCurrentPositionSeconds());
        s.engine.stop();

        // クリップ左端は実ファイル先頭 (1.0 + 4 ブロック) に置かれ fileOffset は 0 (comp 0)。
        // 旧実装は start=1.0 / fileOffset=0 で内容が 4 ブロック手前にずれていた
        const double expFileStart = 1.0 + lateBlocks * blockSecs;
        auto* lane0 = track->getLane(0);
        expect(lane0 != nullptr && lane0->clips.size() == 1, "one clip on lane 0");
        if (lane0 != nullptr && !lane0->clips.empty())
        {
            auto* c = lane0->clips.front().get();
            expectWithinAbsoluteError(c->getStartPosition(), expFileStart, blockSecs * 1.5);
            expectWithinAbsoluteError(c->getFileOffset(), 0.0, 1e-6);
        }

        // ── Q リテイクの「テイクを残す」(takesOnly): テイクレーンにも同じ規則で置かれ、
        //    Lane 0 は触らない (配置式が通常停止と一致するパリティの担保) ──
        s.engine.setPosition(3.0);
        s.engine.play();
        runBlocksWithInput(s.engine, lateBlocks, 0.25f);
        expect(mgr.startRecording(3.0, 3.0), "second recording starts");
        runBlocksWithInput(s.engine, 40, 0.25f);
        mgr.stopRecording(s.engine.getCurrentPositionSeconds(), /*takesOnly*/ true);
        s.engine.stop();

        expect(lane0 != nullptr && lane0->clips.size() == 1, "takesOnly leaves lane 0 untouched");
        AudioClip* take = nullptr;
        for (int li = 1; li < track->getLaneCount(); ++li)
            if (auto* l = track->getLane(li))
                for (auto& cp : l->clips)
                    if (cp->getStartPosition() > 2.0) take = cp.get();
        expect(take != nullptr, "takesOnly placed the take on a take lane");
        if (take != nullptr)
        {
            expectWithinAbsoluteError(take->getStartPosition(), 3.0 + lateBlocks * blockSecs,
                                      blockSecs * 1.5);
            expectWithinAbsoluteError(take->getFileOffset(), 0.0, 1e-6);
        }
    }

    void testRetroFirstWriteMarker()
    {
        // 遡及録音 (retro) 側の実書き込み開始マーカー。retro writer はゲート無しで登録直後の
        // ブロックから書き始めるため、登録が再生開始より遅れるとファイル先頭は playStart より
        // 後ろになる。旧実装は「ファイル先頭 = playStart」仮定で確定クリップを置いており、
        // その分内容が手前へずれていた (targets 側と同じ登録遅れズレの retro 版)。
        beginTest("retrospective: commit placement uses actual first-write position");
        const double blockSecs = (double)kBlock / kSR;

        Scene s;
        s.start();

        RecordingManager mgr(s.engine, *s.tm, s.fmt);
        auto recDir = tempDir.getChildFile("retro_firstwrite");
        mgr.getAudioFolder = [recDir] { return recDir; };

        auto* track = s.tm->addTrack({}, false);

        // 再生を先に回してから遡及キャプチャ登録 (登録遅れ 3 ブロックを模擬)
        const int lateBlocks = 3;
        s.engine.setPosition(1.0);
        s.engine.play();
        runBlocksWithInput(s.engine, lateBlocks, 0.25f);
        expect(mgr.startRetrospective(track, 1.0), "retro capture starts");
        runBlocksWithInput(s.engine, 40, 0.25f);

        const double expFileStart = 1.0 + lateBlocks * blockSecs;
        expectWithinAbsoluteError(s.engine.getRetroFirstWritePosSecs(0.0), expFileStart,
                                  blockSecs * 1.5);

        const double endPos = s.engine.getCurrentPositionSeconds();
        mgr.stopRetrospective(true, endPos);   // Cmd+Shift+R の確定
        s.engine.stop();

        // クリップ左端 = 実ファイル先頭 / fileOffset 0 / 尺 = 実書き込み分
        auto* lane0 = track->getLane(0);
        expect(lane0 != nullptr && lane0->clips.size() == 1, "retro commit placed one clip");
        if (lane0 != nullptr && !lane0->clips.empty())
        {
            auto* c = lane0->clips.front().get();
            expectWithinAbsoluteError(c->getStartPosition(), expFileStart, blockSecs * 1.5);
            expectWithinAbsoluteError(c->getFileOffset(), 0.0, 1e-6);
            expectWithinAbsoluteError(c->getDuration(), endPos - expFileStart, blockSecs * 1.5);
        }

        // マーカーは解除では消えず (停止処理が配置で読むため)、次の登録でリセットされる
        expect(s.engine.getRetroFirstWritePosSecs(-1.0) >= 0.0, "marker retained after teardown");
        expect(mgr.startRetrospective(track, 2.0), "second retro capture starts");
        expectWithinAbsoluteError(s.engine.getRetroFirstWritePosSecs(123.0), 123.0, 1e-9);
        mgr.stopRetrospective(false, 2.0);   // 破棄 (後始末)
    }

    void testStereoWriterMonoInput()
    {
        // 回帰テスト: ステレオ arm したトラック (2ch writer / tgt.stereo=true) を
        // モノ入力デバイス (numInputChannels==1) で録音すると、旧コードは
        // 「tgt.stereo && numInputChannels>=2」が false になり else 分岐で 1 要素配列を
        // 2ch ThreadedWriter に渡していた → Buffer::write が data[1] を境界外参照して
        // クラッシュ (Mac SIGSEGV / Win AV・v0.5.6 から継続していた録音データ損失バグ)。
        // 修正で stereo 分岐は入力が 1ch でも常に 2 要素配列 (L を複製) を渡す。
        beginTest("recording: stereo writer with mono input does not crash (OOB write)");

        Scene s;
        s.start();

        juce::TimeSliceThread bg("StereoMonoTestThread");
        bg.startThread();
        auto wav = tempDir.getChildFile("stereo_mono.wav");
        std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter> tw;
        {
            juce::WavAudioFormat waf;
            using SF = juce::AudioFormatWriterOptions::SampleFormat;
            auto wopts = juce::AudioFormatWriterOptions{}
                             .withSampleRate(kSR).withNumChannels(2)   // ステレオ writer
                             .withBitsPerSample(32).withSampleFormat(SF::floatingPoint);
            wav.deleteFile();
            auto fos = std::make_unique<juce::FileOutputStream>(wav);
            expect(fos->openedOk(), "writer stream open");
            std::unique_ptr<juce::OutputStream> os = std::move(fos);
            std::unique_ptr<juce::AudioFormatWriter> w(waf.createWriterFor(os, wopts));
            expect(w != nullptr, "writer create");
            tw = std::make_unique<juce::AudioFormatWriter::ThreadedWriter>(w.release(), bg, 65536);
        }
        // stereo=true で登録するが、駆動は 1 入力チャンネルのみ (runBlocksWithInput)
        s.engine.setRecordingTarget(tw.get(), nullptr, 0, /*stereo=*/true);

        s.engine.setRecordingActive(true, 0.0, 0.0);
        s.engine.setPosition(0.0);
        s.engine.play();
        runBlocksWithInput(s.engine, 40, 0.25f);   // ここで旧コードは境界外参照でクラッシュ
        s.engine.stop();
        s.engine.setRecordingActive(false);

        expect(s.engine.getRecordedSampleCount() > 0, "stereo writer received samples from mono input");

        s.engine.setRecordingTarget(nullptr, nullptr);
        tw.reset();
        bg.stopThread(2000);
        expect(wav.existsAsFile() && wav.getSize() > 0, "stereo/mono recorded file has data");
    }

    void testLoopWrapFromOutside()
    {
        beginTest("loop: starting past loopEnd plays forward (no yank into loop)");
        // 回帰テスト: ルーラーでループ範囲を設定し、再生バーがループ末尾より後ろにある状態で
        // 再生すると、最初のブロックで newPos >= loopEnd が成立して fmod でループ範囲内へ
        // 引き戻されていた (再生位置が全然違う所から鳴るバグ)。ラップは「末尾を下から跨いだ」
        // 時だけにする修正の担保。位置の前進だけ見ればよいのでクリップは無しでよい。
        const double blockSecs = (double)kBlock / kSR;
        const double ls = 0.3, le = 0.8;

        // ── A: ループ末尾より後ろ (1.2) から再生 → ループ内へ引き戻されず前進する ──
        {
            Scene s;
            s.start();
            s.engine.setLoopRange(ls, le, true);
            const double startPos = 1.2;          // loopEnd(0.8) より後ろ
            s.engine.setPosition(startPos);
            s.engine.play();
            const int blocks = 20;                // ~0.21s
            runBlocks(s.engine, blocks);
            const double pos = s.engine.getCurrentPositionSeconds();
            // 引き戻されていれば pos は [0.3, 0.8) に入る。修正後は startPos から前進し続ける。
            const double expected = startPos + blocks * blockSecs;
            expectWithinAbsoluteError(pos, expected, 2.0 * blockSecs);
            expect(pos > le, "position stays past loopEnd (not wrapped into the loop)");
        }

        // ── B: ループ内 (末尾手前) から再生 → 末尾を跨いだら正しくラップする (通常動作不変) ──
        {
            Scene s;
            s.start();
            s.engine.setLoopRange(ls, le, true);
            const double startPos = 0.78;         // loopEnd(0.8) の手前 = 跨いでラップするはず
            s.engine.setPosition(startPos);
            s.engine.play();
            const int blocks = 20;                // 末尾を 1 回以上跨ぐのに十分
            runBlocks(s.engine, blocks);
            const double pos = s.engine.getCurrentPositionSeconds();
            expect(pos >= ls && pos < le, "position wrapped back inside [loopStart, loopEnd)");
            expect(pos < startPos, "position is behind the start (proves it wrapped)");
        }
    }

    // 入力モニターの返しがトラックの INS チェーンを尊重すること (Phase A の回帰テスト)。
    // ゲイン×2 のスタブプラグインで「信号がチェーンを通ったか」を出力ピークで判定する。
    void testMonitorThroughInserts()
    {
        beginTest("input monitoring honours the track INS chain (FX on the live return)");

        // 入力 const を流して全ブロックの L ピークを返すローカルヘルパ (停止 / 再生どちらでも)。
        auto runWithInput = [](AudioEngine& eng, int n, float inVal) -> float
        {
            juce::AudioBuffer<float> out(2, kBlock), in(1, kBlock);
            juce::FloatVectorOperations::fill(in.getWritePointer(0), inVal, kBlock);
            const float* ins[1] = { in.getReadPointer(0) };
            float peak = 0.0f;
            for (int i = 0; i < n; ++i)
            {
                out.clear();
                float* chans[2] = { out.getWritePointer(0), out.getWritePointer(1) };
                eng.audioDeviceIOCallbackWithContext(ins, 1, chans, 2, kBlock, {});
                peak = juce::jmax(peak, out.getMagnitude(0, 0, kBlock));
            }
            return peak;
        };

        // ── (1) ドライモニタ: チェーン未設定なら入力がそのまま返る (~0.25) ──
        {
            Scene s;
            auto* t = s.tm->addTrack({}, false);   // 空トラック (クリップ無し)
            t->setVolume(0.0f); t->setPan(0.0f);
            s.start();                              // 停止中 (play しない)
            s.engine.setInputMonitoringActive(true);
            s.engine.setMonitorReverbSend(0.0f);
            s.engine.setMonitorChain(nullptr, t->getInputChannel(), t->isStereo(), t->getPan(), t->getVolume());
            const float peak = runWithInput(s.engine, 4, 0.25f);
            expect(std::abs(peak - 0.25f) < 0.01f, "dry monitor returns input unchanged (~0.25)");
        }

        // ── (2) FX モニタ: ゲイン×2 を通すと返しが ~0.5 になる ──
        {
            Scene s;
            auto* t = s.tm->addTrack({}, false);
            t->setVolume(0.0f); t->setPan(0.0f);
            t->getPluginChain().addPlugin(std::make_unique<GainFakePlugin>(2.0f));
            s.start();
            s.engine.setInputMonitoringActive(true);
            s.engine.setMonitorReverbSend(0.0f);
            s.engine.setMonitorChain(&t->getPluginChain(), t->getInputChannel(), t->isStereo(), t->getPan(), t->getVolume());
            const float peak = runWithInput(s.engine, 4, 0.25f);
            expect(std::abs(peak - 0.5f) < 0.01f, "INS (gain x2) is applied to the monitor return (~0.5)");
        }

        // ── (3) 逃げ道: プラグインがあっても setMonitorChain(nullptr) ならドライに戻る ──
        // (環境設定 monitorThroughInserts=false 相当の経路)
        {
            Scene s;
            auto* t = s.tm->addTrack({}, false);
            t->setVolume(0.0f); t->setPan(0.0f);
            t->getPluginChain().addPlugin(std::make_unique<GainFakePlugin>(2.0f));
            s.start();
            s.engine.setInputMonitoringActive(true);
            s.engine.setMonitorReverbSend(0.0f);
            s.engine.setMonitorChain(nullptr, t->getInputChannel(), t->isStereo(), t->getPan(), t->getVolume());
            const float peak = runWithInput(s.engine, 4, 0.25f);
            expect(std::abs(peak - 0.25f) < 0.01f,
                   "monitor chain off: dry return even with a plugin present (~0.25)");
        }

        // ── (4) 二重処理ガード: 再生中、モニタ対象トラックのクリップはチェーンを通さない ──
        // クリップ 0.25 + ゲイン×2。4a: モニタ OFF → 再生がチェーンを通り 0.5。
        // 4b: モニタ ON (対象=このトラック) + 入力無音 → クリップはチェーンを通らず dry 0.25
        // (ガードが無いと再生で 1 回 + モニタで 1 回 = 同一インスタンス二重処理になる)。
        {
            auto wav = tempDir.getChildFile("mon_clip.wav");
            expect(writeMonoConstWav(wav, (int)kSR, 0.25f), "source write");

            // 4a: モニタ OFF → 再生クリップにチェーンが掛かる
            {
                Scene s;
                auto* t = s.addConstTrack(wav, 1.0);
                t->getPluginChain().addPlugin(std::make_unique<GainFakePlugin>(2.0f));
                s.start();
                s.engine.play();
                float pl = 0, pr = 0;
                runBlocks(s.engine, 10, &pl, &pr);
                expect(std::abs(pl - 0.5f) < 0.01f, "monitor off: clip plays through the chain (x2 ~0.5)");
            }

            // 4b: モニタ ON (対象トラック) + 入力無音 → クリップはチェーンを通らず dry
            {
                Scene s;
                auto* t = s.addConstTrack(wav, 1.0);
                t->getPluginChain().addPlugin(std::make_unique<GainFakePlugin>(2.0f));
                s.start();
                s.engine.setInputMonitoringActive(true);
                s.engine.setMonitorReverbSend(0.0f);
                s.engine.setMonitorChain(&t->getPluginChain(), t->getInputChannel(), t->isStereo(), t->getPan(), t->getVolume());
                s.engine.play();
                const float peak = runWithInput(s.engine, 12, 0.0f);
                expect(std::abs(peak - 0.25f) < 0.01f,
                       "monitor target: clip bypasses the chain during playback (no double-process ~0.25)");
            }
        }

        // ── (4c) 書き出し中はモニタ返しが休止する (offlineRenderActive ガード・案 a) ──
        // 書き出しスレッドとモニタ経路が同一プラグインインスタンスを交互に processBlock して
        // 書き出しを破損させないため、書き出し中は返しを丸ごと止める。解除で復帰する。
        {
            Scene s;
            auto* t = s.tm->addTrack({}, false);
            t->setVolume(0.0f); t->setPan(0.0f);
            t->getPluginChain().addPlugin(std::make_unique<GainFakePlugin>(2.0f));
            s.start();
            s.engine.setInputMonitoringActive(true);
            s.engine.setMonitorReverbSend(0.0f);
            s.engine.setMonitorChain(&t->getPluginChain(), t->getInputChannel(), t->isStereo(), t->getPan(), t->getVolume());

            s.engine.setOfflineRenderActiveForTests(true);
            const float pOff = runWithInput(s.engine, 4, 0.25f);
            expect(pOff < 0.001f, "monitor return is muted while an offline render is active");

            s.engine.setOfflineRenderActiveForTests(false);
            const float pOn = runWithInput(s.engine, 4, 0.25f);
            expect(std::abs(pOn - 0.5f) < 0.01f,
                   "monitor return resumes (through the chain) after the render flag clears (~0.5)");
        }

        // ── (5) mono 入力の返しはセンター (L=R) になる (L/R 分離バグの回帰テスト) ──
        // 2ch デバイスで ch0=0.3 / ch1=0.7 を与え、mono トラックを inputCh=0 でモニタする。
        // 旧実装は device ch0→L / ch1→R 固定で L=0.3 / R=0.7 と分離した。修正後は選択 ch(0) を
        // L/R 両方へ返すので L=R=0.3 (センター)。
        {
            Scene s;
            auto* t = s.tm->addTrack({}, false);     // mono
            t->setVolume(0.0f); t->setPan(0.0f);
            t->setInputChannel(0);
            s.start();
            s.engine.setInputMonitoringActive(true);
            s.engine.setMonitorReverbSend(0.0f);
            s.engine.setMonitorChain(nullptr, t->getInputChannel(), t->isStereo(), t->getPan(), t->getVolume());

            juce::AudioBuffer<float> out(2, kBlock), in(2, kBlock);
            juce::FloatVectorOperations::fill(in.getWritePointer(0), 0.3f, kBlock);
            juce::FloatVectorOperations::fill(in.getWritePointer(1), 0.7f, kBlock);
            const float* ins[2] = { in.getReadPointer(0), in.getReadPointer(1) };
            float pL = 0.0f, pR = 0.0f;
            for (int i = 0; i < 4; ++i)
            {
                out.clear();
                float* chans[2] = { out.getWritePointer(0), out.getWritePointer(1) };
                s.engine.audioDeviceIOCallbackWithContext(ins, 2, chans, 2, kBlock, {});
                pL = juce::jmax(pL, out.getMagnitude(0, 0, kBlock));
                pR = juce::jmax(pR, out.getMagnitude(1, 0, kBlock));
            }
            expect(std::abs(pL - 0.3f) < 0.01f, "mono monitor: L = selected input ch (~0.3)");
            expect(std::abs(pR - 0.3f) < 0.01f, "mono monitor: R = same selected ch, centered (~0.3, not 0.7)");
        }

        // ── (6) stereo 入力の返しは inputCh→L / inputCh+1→R ──
        {
            Scene s;
            auto* t = s.tm->addTrack({}, true);      // stereo
            t->setVolume(0.0f); t->setPan(0.0f);
            t->setInputChannel(0);
            s.start();
            s.engine.setInputMonitoringActive(true);
            s.engine.setMonitorReverbSend(0.0f);
            s.engine.setMonitorChain(nullptr, t->getInputChannel(), t->isStereo(), t->getPan(), t->getVolume());

            juce::AudioBuffer<float> out(2, kBlock), in(2, kBlock);
            juce::FloatVectorOperations::fill(in.getWritePointer(0), 0.3f, kBlock);
            juce::FloatVectorOperations::fill(in.getWritePointer(1), 0.7f, kBlock);
            const float* ins[2] = { in.getReadPointer(0), in.getReadPointer(1) };
            float pL = 0.0f, pR = 0.0f;
            for (int i = 0; i < 4; ++i)
            {
                out.clear();
                float* chans[2] = { out.getWritePointer(0), out.getWritePointer(1) };
                s.engine.audioDeviceIOCallbackWithContext(ins, 2, chans, 2, kBlock, {});
                pL = juce::jmax(pL, out.getMagnitude(0, 0, kBlock));
                pR = juce::jmax(pR, out.getMagnitude(1, 0, kBlock));
            }
            expect(std::abs(pL - 0.3f) < 0.01f, "stereo monitor: L = inputCh (~0.3)");
            expect(std::abs(pR - 0.7f) < 0.01f, "stereo monitor: R = inputCh+1 (~0.7)");
        }

        // ── (7) パンが返しに反映される (mono, ハードL / ハードR) ──
        // mono 入力 0.3 を pan=-1 (左) でモニタ → L=0.3 / R=0、pan=+1 (右) → L=0 / R=0.3。
        auto runPanned = [](Scene& s, float pan, float* pL, float* pR)
        {
            auto* t = s.tm->getTrack(0);
            t->setPan(pan);
            s.engine.setMonitorChain(nullptr, t->getInputChannel(), t->isStereo(), t->getPan(), t->getVolume());
            juce::AudioBuffer<float> out(2, kBlock), in(1, kBlock);
            juce::FloatVectorOperations::fill(in.getWritePointer(0), 0.3f, kBlock);
            const float* ins[1] = { in.getReadPointer(0) };
            *pL = *pR = 0.0f;
            for (int i = 0; i < 4; ++i)
            {
                out.clear();
                float* chans[2] = { out.getWritePointer(0), out.getWritePointer(1) };
                s.engine.audioDeviceIOCallbackWithContext(ins, 1, chans, 2, kBlock, {});
                *pL = juce::jmax(*pL, out.getMagnitude(0, 0, kBlock));
                *pR = juce::jmax(*pR, out.getMagnitude(1, 0, kBlock));
            }
        };
        {
            Scene s;
            auto* t = s.tm->addTrack({}, false);     // mono
            t->setVolume(0.0f); t->setInputChannel(0);
            s.start();
            s.engine.setInputMonitoringActive(true);
            s.engine.setMonitorReverbSend(0.0f);

            float pL = 0, pR = 0;
            runPanned(s, -1.0f, &pL, &pR);           // ハードL
            expect(std::abs(pL - 0.3f) < 0.01f, "pan hard L: monitor L = input (~0.3)");
            expect(pR < 0.01f,                   "pan hard L: monitor R silent (~0)");

            runPanned(s, +1.0f, &pL, &pR);           // ハードR
            expect(pL < 0.01f,                   "pan hard R: monitor L silent (~0)");
            expect(std::abs(pR - 0.3f) < 0.01f, "pan hard R: monitor R = input (~0.3)");
        }

        // ── (8) フェーダー音量が返しに反映される (mono, -6dB ≒ ゲイン 0.5) ──
        // mono 入力 0.4 を -6.02dB のトラックでモニタ → 返しは ~0.2 (= 0.4 * 0.5)。
        // 0dB (gain 1.0) は従来どおり素通り (~0.4)。
        {
            Scene s;
            auto* t = s.tm->addTrack({}, false);     // mono
            t->setPan(0.0f); t->setInputChannel(0);
            s.start();
            s.engine.setInputMonitoringActive(true);
            s.engine.setMonitorReverbSend(0.0f);

            auto monitorPeak = [&s](Track* tr) -> float
            {
                s.engine.setMonitorChain(nullptr, tr->getInputChannel(), tr->isStereo(),
                                         tr->getPan(), tr->getVolume());
                juce::AudioBuffer<float> out(2, kBlock), in(1, kBlock);
                juce::FloatVectorOperations::fill(in.getWritePointer(0), 0.4f, kBlock);
                const float* ins[1] = { in.getReadPointer(0) };
                float peak = 0.0f;
                for (int i = 0; i < 4; ++i)
                {
                    out.clear();
                    float* chans[2] = { out.getWritePointer(0), out.getWritePointer(1) };
                    s.engine.audioDeviceIOCallbackWithContext(ins, 1, chans, 2, kBlock, {});
                    peak = juce::jmax(peak, out.getMagnitude(0, 0, kBlock));
                }
                return peak;
            };

            t->setVolume(0.0f);
            expect(std::abs(monitorPeak(t) - 0.4f) < 0.01f, "fader 0dB: monitor return unchanged (~0.4)");

            t->setVolume(-6.0206f);                  // gain ~0.5
            expect(std::abs(monitorPeak(t) - 0.2f) < 0.01f, "fader -6dB: monitor return halved (~0.2)");
        }

        // ── (9) setMonitorChain は未 prepare のチェーンを prepare する ──
        // 停止中に追加したトラック等、preparePlayback を経ていないチェーンに挿したプラグインが
        // モニター返しで効くことの保証 (UI 側で onChainChanged → sync → setMonitorChain を呼ぶ前提)。
        // PrepareGatedFakePlugin は prepareToPlay されないとゲインを掛けない。
        {
            Scene s;
            s.start();                               // device 開始 (この時点で tm にトラックは無い)
            auto* t = s.tm->addTrack({}, false);     // start 後に追加 = チェーンは未 prepare
            t->setVolume(0.0f); t->setPan(0.0f); t->setInputChannel(0);
            t->getPluginChain().addPlugin(std::make_unique<PrepareGatedFakePlugin>(2.0f));

            s.engine.setInputMonitoringActive(true);
            s.engine.setMonitorReverbSend(0.0f);
            // setMonitorChain が未 prepare を検出して prepareToPlay する → ゲインが効く
            s.engine.setMonitorChain(&t->getPluginChain(), t->getInputChannel(), t->isStereo(),
                                     t->getPan(), t->getVolume());
            const float peak = runWithInput(s.engine, 4, 0.25f);
            expect(std::abs(peak - 0.5f) < 0.01f,
                   "setMonitorChain prepares an unprepared chain so the plugin works (~0.5)");
        }
    }

    // 停止中もトラックのプラグインチェーンを処理し、プラグインが自前生成する音 (Melodyne の編集
    // プレビュー等) が出力へ乗ることを検証する。GeneratorFakePlugin は入力に関係なく定数を出す。
    void testStoppedPluginPreview()
    {
        beginTest("stopped: track plugin chain is processed so it can output preview audio (Melodyne-style)");

        auto wav = tempDir.getChildFile("stopprev_clip.wav");
        expect(writeMonoConstWav(wav, (int) kSR, 0.0f), "silent clip source");  // クリップ自体は無音でよい

        // 停止中 (play しない) に n ブロック駆動し L ピークを返す。入力 nullptr = モニタ非干渉。
        auto stoppedPeak = [](AudioEngine& eng, int n) -> float
        {
            juce::AudioBuffer<float> out(2, kBlock);
            float peak = 0.0f;
            for (int i = 0; i < n; ++i)
            {
                out.clear();
                float* chans[2] = { out.getWritePointer(0), out.getWritePointer(1) };
                eng.audioDeviceIOCallbackWithContext(nullptr, 0, chans, 2, kBlock, {});
                peak = juce::jmax(peak, out.getMagnitude(0, 0, kBlock));
            }
            return peak;
        };
        // 停止時プレビューのパンは synth プレビューと同じ sin/cos 則 (センター = -3dB ≒ 0.707)
        const float kCenterPan = std::cos(0.5f * juce::MathConstants<float>::halfPi);

        // (1) プラグイン付きトラック: 停止中でもチェーンが処理され、生成音 (0.5) が出力に乗る
        {
            Scene s;
            auto* t = s.addConstTrack(wav, 1.0);     // クリップ有 = clipTracks に載る
            t->getPluginChain().addPlugin(std::make_unique<GeneratorFakePlugin>(0.5f));
            s.start();                               // 停止中
            const float peak = stoppedPeak(s.engine, 4);
            expect(std::abs(peak - 0.5f * kCenterPan) < 0.02f,
                   "stopped: plugin-generated preview reaches output (~0.5 * centerPan)");
        }

        // (2) プラグイン無しトラック: 停止中はチェーンを処理しない → 無音 (クリップも鳴らさない)
        {
            Scene s;
            s.addConstTrack(wav, 1.0);               // プラグイン無し
            s.start();
            expect(stoppedPeak(s.engine, 4) < 1.0e-4f,
                   "stopped: no plugin => silent (clips are not played when stopped)");
        }

        // (3) ミュートしたプラグイン付きトラック: 停止中もプレビューしない → 無音
        {
            Scene s;
            auto* t = s.addConstTrack(wav, 1.0);
            t->getPluginChain().addPlugin(std::make_unique<GeneratorFakePlugin>(0.5f));
            t->setMuted(true);
            s.start();
            expect(stoppedPeak(s.engine, 4) < 1.0e-4f, "stopped: muted track => no preview");
        }
    }
};

static AudioEngineRealtimeTests audioEngineRealtimeTests;
