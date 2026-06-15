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
        testMultiTrackClipIndexRouting();
        testClearPlaybackBarrier();
        testDeferredDestructionRebuild();
        testRecordingLatencyComp();
        testRecordingWriteGate();
        testMonitorThroughInserts();

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
            s.engine.setMonitorChain(nullptr);
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
            s.engine.setMonitorChain(&t->getPluginChain());
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
            s.engine.setMonitorChain(nullptr);
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
                s.engine.setMonitorChain(&t->getPluginChain());
                s.engine.play();
                const float peak = runWithInput(s.engine, 12, 0.0f);
                expect(std::abs(peak - 0.25f) < 0.01f,
                       "monitor target: clip bypasses the chain during playback (no double-process ~0.25)");
            }
        }
    }
};

static AudioEngineRealtimeTests audioEngineRealtimeTests;
