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

// prepareToPlay の呼び出し回数を数えるスタブ。「preparePlayback の再構築が prepare 済みチェーンを
// 再 prepare しない」(再生中編集で全プラグインがフル再初期化され音が止まる問題の修正) の検証用。
class PrepareCountFakePlugin : public juce::AudioPluginInstance
{
public:
    PrepareCountFakePlugin()
        : juce::AudioPluginInstance(BusesProperties()
              .withInput ("In",  juce::AudioChannelSet::stereo())
              .withOutput("Out", juce::AudioChannelSet::stereo())) {}
    int prepareCount { 0 };
    const juce::String getName() const override            { return "PrepCount"; }
    void prepareToPlay(double, int) override               { ++prepareCount; }
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

// L サンプルの純遅延 + getLatencySamples()==L を報告するスタブ (PDC 遅延ラインの行使用)。
// これを挿したトラックが「最遅」になり、プラグイン無しの他トラックに遅延ラインが付く。
class LatencyFakePlugin : public juce::AudioPluginInstance
{
public:
    explicit LatencyFakePlugin(int latencySamples)
        : juce::AudioPluginInstance(BusesProperties()
              .withInput ("In",  juce::AudioChannelSet::stereo())
              .withOutput("Out", juce::AudioChannelSet::stereo())),
          lat(latencySamples)
    { setLatencySamples(lat); }
    const juce::String getName() const override            { return "Latency"; }
    void prepareToPlay(double, int) override
    {
        dl.setSize(2, juce::jmax(1, lat), false, true, true);
        dl.clear();
        pos = 0;
        setLatencySamples(lat);
    }
    void releaseResources() override                       {}
    void processBlock(juce::AudioBuffer<float>& b, juce::MidiBuffer&) override
    {
        if (lat <= 0) return;
        const int nCh = juce::jmin(2, b.getNumChannels());
        for (int i = 0; i < b.getNumSamples(); ++i)
        {
            for (int ch = 0; ch < nCh; ++ch)
            {
                const float delayed = dl.getSample(ch, pos);
                dl.setSample(ch, pos, b.getSample(ch, i));
                b.setSample(ch, i, delayed);
            }
            pos = (pos + 1) % lat;
        }
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
private:
    int lat { 0 };
    juce::AudioBuffer<float> dl;
    int pos { 0 };
};

// ノートが押されている間だけ 0.5 を出力する MIDI 反応スタブ (VSTi 音源の代役)。
// 「ライブ MIDI が INS チェーンへ届いたか」を出力レベルで判定できる
class MidiTriggeredFakePlugin : public juce::AudioPluginInstance
{
public:
    MidiTriggeredFakePlugin()
        : juce::AudioPluginInstance(BusesProperties()
              .withInput ("In",  juce::AudioChannelSet::stereo())
              .withOutput("Out", juce::AudioChannelSet::stereo())) {}
    int heldNotes { 0 };
    const juce::String getName() const override            { return "MidiTriggered"; }
    void prepareToPlay(double, int) override               {}
    void releaseResources() override                       {}
    void processBlock(juce::AudioBuffer<float>& b, juce::MidiBuffer& midi) override
    {
        for (const auto meta : midi)
        {
            const auto m = meta.getMessage();
            if      (m.isNoteOn())      ++heldNotes;
            else if (m.isNoteOff())     heldNotes = juce::jmax(0, heldNotes - 1);
            else if (m.isAllNotesOff()) heldNotes = 0;
        }
        if (heldNotes > 0)
            for (int ch = 0; ch < b.getNumChannels(); ++ch)
                juce::FloatVectorOperations::fill(b.getWritePointer(ch), 0.5f, b.getNumSamples());
    }
    using juce::AudioProcessor::processBlock;
    double getTailLengthSeconds() const override           { return 0.0; }
    bool acceptsMidi() const override                      { return true; }
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
    int getCurrentBufferSizeSamples() override                { return blockSize; }
    double getCurrentSampleRate() override                    { return kSR; }
    int getCurrentBitDepth() override                         { return 32; }
    juce::BigInteger getActiveOutputChannels() const override { juce::BigInteger b; b.setRange(0, 2, true); return b; }
    juce::BigInteger getActiveInputChannels() const override  { juce::BigInteger b; b.setRange(0, 2, true); return b; }
    int getOutputLatencyInSamples() override                  { return outLatency; }
    int getInputLatencyInSamples() override                   { return inLatency; }

    // 録音レイテンシ補正テスト用 (既定 0 = 他テストへの影響なし)
    int inLatency  { 0 };
    int outLatency { 0 };
    // デバイス再起動 (バッファサイズ変更) テスト用 (既定 kBlock = 他テストへ影響なし)
    int blockSize  { kBlock };
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
        testShutdownReleasesPendingGraveyard();
        testRebuildSkipsPreparedChains();
        testRecordingLatencyComp();
        testRecordingWriteGate();
        testRecordingFirstWriteMarker();
        testRetroFirstWriteMarker();
        testStereoWriterMonoInput();
        testLoopWrapFromOutside();
        testMonitorThroughInserts();
        testStoppedPluginPreview();
        testLiveMidiInput();
        testAppCaptureMix();
        testDiskStreamingDeterminism();
        testMulticoreDeterminism();
        testEmptyRangeOfflineRender();
        testFolderBus();
        testBufferSizeChangeWhilePlaying();

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

    // 再生中のデバイス再起動 (Audio Settings のバッファサイズ変更) の回帰テスト (実クラッシュ id46)。
    // 旧実装は aboutToStart が dirty を立てるだけで、再生中は次の play() までスナップショットを
    // 再構築しなかった。旧 blockSize (512) で確保した PDC 遅延ラインへ大きい numSamples (2048) を
    // 一括コピーすると範囲外書き込み = ヒープ破壊になり、後続の malloc で落ちていた
    // (「再生しながらバッファ変更 → 音が壊れる → 戻すと落ちる」の実機再現手順と一致)。
    // 修正 = (1) aboutToStart が再生中は即 preparePlayback (message thread のとき)、
    //        (2) applyDelayLine のチャンク分割ガード (不整合期間も境界内で処理)。
    void testBufferSizeChangeWhilePlaying()
    {
        beginTest("device restart while playing: PDC delay lines survive a buffer size change");

        auto wav = tempDir.getChildFile("bufchg.wav");
        expect(writeMonoConstWav(wav, (int)(kSR * 6.0), 0.5f), "write const wav");

        Scene s;
        auto* heavy = s.addConstTrack(wav, 6.0);   // レイテンシ持ち = 最遅トラック (自身の遅延ラインは 0)
        s.addConstTrack(wav, 6.0);                 // プラグイン無し = こちらに 600 サンプルの遅延ラインが付く
        heavy->getPluginChain().addPlugin(std::make_unique<LatencyFakePlugin>(600));
        s.start();
        s.engine.play();

        // 512 ブロックで定常状態へ (PDC 整列後 0.5 + 0.5 = ~1.0)
        float pL = 0, pR = 0;
        runBlocks(s.engine, 10, &pL, &pR);
        expectWithinAbsoluteError(pL, 1.0f, 0.03f, "steady mix before device restart");

        // ── 再生したままデバイス再起動 (バッファサイズ 512 → 2048) ──
        FakeAudioIODevice bigDevice;
        bigDevice.blockSize = 2048;
        s.engine.audioDeviceAboutToStart(&bigDevice);

        // 新ブロックサイズで駆動しても壊れず、数ブロックで定常ミックスへ戻る
        juce::AudioBuffer<float> out(2, 2048);
        float peak = 0.0f;
        bool finite = true;
        for (int b = 0; b < 20; ++b)
        {
            out.clear();
            float* chans[2] = { out.getWritePointer(0), out.getWritePointer(1) };
            s.engine.audioDeviceIOCallbackWithContext(nullptr, 0, chans, 2, 2048, {});
            peak = out.getMagnitude(0, 0, 2048);
            for (int i = 0; i < 2048 && finite; ++i)
                if (!std::isfinite(out.getSample(0, i)) || std::abs(out.getSample(0, i)) > 4.0f)
                    finite = false;
        }
        expect(finite, "output stays finite/bounded after the block-size change");
        expectWithinAbsoluteError(peak, 1.0f, 0.05f, "steady mix restored at the new block size");

        // ── バッファを元へ戻す (実機の再現手順ではここで落ちていた) ──
        FakeAudioIODevice smallDevice;   // 既定 512
        s.engine.audioDeviceAboutToStart(&smallDevice);
        runBlocks(s.engine, 10, &pL, &pR);
        expectWithinAbsoluteError(pL, 1.0f, 0.05f, "steady mix restored after changing back");
    }

    void testFolderBus()
    {
        beginTest("folder track: bus routing, folder volume, mute/solo inheritance");

        auto wav = tempDir.getChildFile("folderbus.wav");
        expect(writeMonoConstWav(wav, (int)(kSR * 4.0), 0.5f), "write const wav");

        Scene s;
        auto* child   = s.addConstTrack(wav, 4.0);   // フォルダ配下にする
        auto* sibling = s.addConstTrack(wav, 4.0);   // トップレベルのまま
        auto* folder  = s.tm->addFolderTrack();
        expect(folder != nullptr && folder->isFolderTrack(), "folder track created");
        child->setFolderParent(folder);
        s.tm->normalizeFolderContiguity();           // 子=フォルダ直後の不変条件を回復
        s.start();
        s.engine.play();

        // (1) フォルダ Vol 0dB: 子 0.5 (バス経由) + 兄弟 0.5 = ~1.0 (センター素通り)
        float pL = 0, pR = 0;
        runBlocks(s.engine, 12, &pL, &pR);
        expectWithinAbsoluteError(pL, 1.0f, 0.03f, "child routes through folder at unity");
        expectWithinAbsoluteError(pR, 1.0f, 0.03f, "R matches L");
        const int folderIdx = s.tm->indexOf(folder);
        expect(s.engine.getTrackOutputPeakL(folderIdx) > -20.0f,
               "folder meter shows bus level while playing");

        // (2) フォルダ Vol -6.02dB (gain 0.5): 子だけ半分 → 0.25 + 0.5 = 0.75
        folder->setVolume(-6.0206f);
        runBlocks(s.engine, 4, &pL, &pR);
        runBlocks(s.engine, 4, &pL, &pR);   // 変更後の定常ブロックで測る
        expectWithinAbsoluteError(pL, 0.75f, 0.03f, "folder volume scales children only");
        folder->setVolume(0.0f);

        // (3) フォルダ Mute → 子が黙る (兄弟のみ 0.5)
        folder->setMuted(true);
        runBlocks(s.engine, 4, &pL, &pR);
        runBlocks(s.engine, 4, &pL, &pR);
        expectWithinAbsoluteError(pL, 0.5f, 0.03f, "folder mute silences its children");
        // メータの凍結防止 (回帰テスト): ミュートで fed されなくなったバスのメータは
        // 最後の値で止まらず減衰する。Peak (0.80 乗算 ≈ 0.5 秒で底) は 80 ブロックで -90 以下、
        // VU は時定数 300ms の追従なので同じ時間では途中 (-30dB 前後) — 凍結 (-6dB 付近) との
        // 区別には -20dB 閾値で十分
        runBlocks(s.engine, 80);
        expect(s.engine.getTrackOutputPeakL(folderIdx) <= -90.0f,
               "muted folder Peak decays instead of freezing");
        expect(s.engine.getTrackOutputVUL(folderIdx) <= -20.0f,
               "muted folder VU decays instead of freezing");
        folder->setMuted(false);

        // (4) フォルダ Solo → 配下の子だけが鳴る (0.5)
        folder->setSoloed(true);
        runBlocks(s.engine, 4, &pL, &pR);
        runBlocks(s.engine, 4, &pL, &pR);
        expectWithinAbsoluteError(pL, 0.5f, 0.03f, "folder solo plays only its children");
        folder->setSoloed(false);

        // (5) 兄弟の Solo → フォルダの子は黙る (0.5)
        sibling->setSoloed(true);
        runBlocks(s.engine, 4, &pL, &pR);
        runBlocks(s.engine, 4, &pL, &pR);
        expectWithinAbsoluteError(pL, 0.5f, 0.03f, "sibling solo silences folder children");
        sibling->setSoloed(false);

        // (5b) フォルダの Pan 左いっぱい → 子の R 成分が消える (L 1.0 / R 0.5 = 兄弟のみ)
        folder->setPan(-1.0f);
        runBlocks(s.engine, 4, &pL, &pR);
        runBlocks(s.engine, 4, &pL, &pR);
        expectWithinAbsoluteError(pL, 1.0f, 0.03f, "folder pan hard-left keeps L");
        expectWithinAbsoluteError(pR, 0.5f, 0.03f, "folder pan hard-left removes child from R");
        folder->setPan(0.0f);

        // (5c) フォルダの Rev 送り → リバーブウェットが加算されて出力が dry 合計を上回る
        folder->setReverbSend(1.0f);
        runBlocks(s.engine, 8, &pL, &pR);
        runBlocks(s.engine, 8, &pL, &pR);
        expect(pL > 1.02f, "folder reverb send adds wet on top of dry sum");
        folder->setReverbSend(0.0f);

        // (6) 書き出し (renderOfflineRange) も同じ規則: フォルダ Vol -6.02dB で 0.75
        s.engine.stop();
        folder->setVolume(-6.0206f);
        juce::AudioBuffer<float> out;
        s.engine.renderOfflineRange(1.0, 1.5, out);
        expect(out.getNumSamples() > 0, "offline render produced samples");
        const float mid = out.getSample(0, out.getNumSamples() / 2);
        expectWithinAbsoluteError(mid, 0.75f, 0.03f,
                                  "offline render routes children through folder bus");
        folder->setVolume(0.0f);
    }

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

    // アプリ音声取り込み (アプリケーショントラック) の統合テスト。writer (キャプチャスレッド)
    // の代わりにテストが ring へ push し、reader (audio thread) 側の経路 = 「モニタ返し合算後・
    // ミラー tap 前の voice 加算」を停止/再生ブランチの両方で固定する。
    // 録音に乗らないことは経路が構造的に別 (録音は inputChannelData を writer へ書く・
    // 取り込みは outputChannelData にしか触れない) なので個別検証しない。
    void testAppCaptureMix()
    {
        using Voice = AudioEngine::AppCaptureVoice;
        auto makeVoices = [](std::initializer_list<std::shared_ptr<Voice>> vs)
        {
            auto out = std::make_shared<AudioEngine::AppCaptureVoices>();
            for (auto& v : vs) out->push_back(v);
            return std::shared_ptr<const AudioEngine::AppCaptureVoices>(out);
        };

        beginTest("app capture: voice mixes into output + mirror, honours gain, detaches");
        {
            AudioEngine engine;
            FakeAudioIODevice device;
            engine.audioDeviceAboutToStart(&device);

            auto ring = std::make_shared<StreamMirrorRing>();
            ring->reset(kSR);                    // キャプチャ側 SR = エンジン SR (ratio 1)
            auto voice = std::make_shared<Voice>();
            voice->ring = ring;
            voice->gainL.store(0.5f);
            voice->gainR.store(0.5f);
            engine.setAppCaptureVoices(makeVoices({ voice }));
            expect(engine.hasAppCaptureVoices(), "hasAppCaptureVoices true after publish");

            auto mirror = std::make_shared<StreamMirrorRing>();
            engine.setMirrorRing(mirror);

            std::vector<float> inSilence((size_t) kBlock, 0.0f);
            const float* ins[2] = { inSilence.data(), inSilence.data() };
            std::vector<float> outL((size_t) kBlock), outR((size_t) kBlock);
            float* outs[2] = { outL.data(), outR.data() };

            auto pushConst = [&ring](int n)
            {
                std::vector<float> l((size_t) n, 0.8f), r((size_t) n, 0.4f);
                ring->push(l.data(), r.data(), n);
            };
            auto runBlock = [&] { engine.audioDeviceIOCallbackWithContext(ins, 2, outs, 2, kBlock, {}); };

            // (1) priming: 目標水位 (50ms) に達するまでは無音。最初の pull で primeFrom = 0 が確定する
            runBlock();
            expect(std::abs(outL[0]) < 1.0e-6f, "silent before any capture data");

            const int target = (int) (kSR * 0.05);   // reader の既定目標水位 = 2400 @48k
            pushConst(target);
            runBlock();
            expect(std::abs(outL[0] - 0.8f * 0.5f) < 1.0e-4f, "L = capture * gainL (stopped branch)");
            expect(std::abs(outR[0] - 0.4f * 0.5f) < 1.0e-4f, "R = capture * gainR (stopped branch)");

            // (2) ミラー (→OBS) にも乗る: ミラーの内容 = 最終出力 (取り込み音込み)
            {
                bool match = true;
                const juce::uint64 w = mirror->getWritePos();
                for (int i = 0; i < kBlock; ++i)
                {
                    if (mirror->sampleL(w - (juce::uint64) kBlock + (juce::uint64) i) != outL[(size_t) i]) match = false;
                    if (mirror->sampleR(w - (juce::uint64) kBlock + (juce::uint64) i) != outR[(size_t) i]) match = false;
                }
                expect(match, "mirror receives final output including capture audio");
            }

            // (3) ゲイン (トラック Vol 相当) の即時反映 (push/pull を釣り合わせて水位を保つ)
            voice->gainL.store(1.0f);
            voice->gainR.store(1.0f);
            pushConst(kBlock);
            runBlock();
            expect(std::abs(outL[0] - 0.8f) < 1.0e-4f, "gain change applies immediately");

            // (4) ミュート: 出力には乗らないがリングは読み進める (解除で現在の音から即復帰)
            voice->mute.store(true);
            pushConst(kBlock);
            runBlock();
            expect(std::abs(outL[0]) < 1.0e-6f, "muted voice is silent");
            voice->mute.store(false);
            pushConst(kBlock);
            runBlock();
            expect(std::abs(outL[0] - 0.8f) < 1.0e-4f, "unmute resumes at the current audio (ring kept flowing)");

            // (5) 枯渇 (ブラウザ一時停止相当): push 無しで水位が尽きると無音へ落ちる → 溜め直しで復帰
            for (int b = 0; b < 8; ++b) runBlock();
            expect(std::abs(outL[0]) < 1.0e-6f, "starved ring yields silence");
            pushConst(target);
            runBlock();
            expect(std::abs(outL[0] - 0.8f) < 1.0e-4f, "resumes after re-priming");

            // (6) 解除後は混ざらない
            engine.setAppCaptureVoices(nullptr);
            expect(!engine.hasAppCaptureVoices(), "hasAppCaptureVoices false after detach");
            pushConst(kBlock);
            runBlock();
            expect(std::abs(outL[0]) < 1.0e-6f, "detached voices no longer mix");

            // (7) voice 差し替え (取り込み対象アプリの変更相当): 旧リングと同じ epoch (=1) の
            //     新リングを事前充填付きで登録しても、新 voice = 新 reader なので必ず溜め直し
            //     (priming) から始まる (stale readPos の持ち越しが構造的に無い)
            auto ring2 = std::make_shared<StreamMirrorRing>();
            ring2->reset(kSR);   // epoch 1 = 旧リングと同値 (衝突ケースを意図的に作る)
            {
                std::vector<float> l((size_t) 8000, 0.8f), r((size_t) 8000, 0.8f);
                ring2->push(l.data(), r.data(), 8000);
            }
            auto voice2 = std::make_shared<Voice>();
            voice2->ring = ring2;
            engine.setAppCaptureVoices(makeVoices({ voice2 }));
            runBlock();
            expect(std::abs(outL[0]) < 1.0e-6f,
                   "fresh voice re-primes (no stale reader state carryover)");
            {
                std::vector<float> l((size_t) target, 0.8f), r((size_t) target, 0.8f);
                ring2->push(l.data(), r.data(), target);
            }
            runBlock();
            expect(std::abs(outL[0] - 0.8f) < 1.0e-4f, "fresh voice plays after re-priming");

            // (8) 複数 voice の合算とソロ規則 (voice 間): solo した voice だけが鳴る
            auto ring3 = std::make_shared<StreamMirrorRing>();
            ring3->reset(kSR);
            auto voice3 = std::make_shared<Voice>();
            voice3->ring = ring3;
            engine.setAppCaptureVoices(makeVoices({ voice2, voice3 }));
            auto push3 = [&ring3](int n)
            {
                std::vector<float> l((size_t) n, 0.2f), r((size_t) n, 0.2f);
                ring3->push(l.data(), r.data(), n);
            };
            runBlock();                          // 両 voice の primeFrom 確定 (voice2 は継続 primed)
            push3(target);
            {
                std::vector<float> l((size_t) target + kBlock, 0.8f), r((size_t) target + kBlock, 0.8f);
                ring2->push(l.data(), r.data(), target + kBlock);
            }
            runBlock();
            expect(std::abs(outL[0] - 1.0f) < 1.0e-3f, "two voices sum (0.8 + 0.2)");
            voice3->solo.store(true);
            push3(kBlock);
            { std::vector<float> l((size_t) kBlock, 0.8f); ring2->push(l.data(), l.data(), kBlock); }
            runBlock();
            expect(std::abs(outL[0] - 0.2f) < 1.0e-3f, "soloed voice plays alone (other voice silenced)");

            // (9) INS チェーン: 取り込み音がトラックの FX (×2 ゲイン) を通り、
            //     マスターメータ (Peak) にも取り込み音が乗る (トータルレベル管理)
            voice3->solo.store(false);
            PluginChain chain;
            chain.addPlugin(std::make_unique<GainFakePlugin>(2.0f));
            chain.prepareToPlay(kSR, kBlock);
            voice2->chain = &chain;
            engine.setAppCaptureVoices(makeVoices({ voice2 }));
            // リングには 0.8 の在庫 (目標水位分) が残っているので、読まれるのは 0.8 → ×2 = 1.6
            { std::vector<float> l((size_t) kBlock, 0.8f); ring2->push(l.data(), l.data(), kBlock); }
            runBlock();
            expect(std::abs(outL[0] - 1.6f) < 1.0e-3f, "INS chain applies to captured audio (0.8 x2)");
            expect(engine.getPeakL() > 3.0f,
                   "master peak meter reflects captured audio (~+4 dBFS)");
            engine.setAppCaptureVoices(nullptr);
        }

        beginTest("app capture: mixes on top of playback and follows track solo rules");
        {
            auto wav = tempDir.getChildFile("appcap_c05.wav");
            expect(writeMonoConstWav(wav, (int) kSR * 4, 0.5f), "source write");

            Scene s;
            auto* clipTrack = s.addConstTrack(wav, 4.0);
            s.start();

            auto ring = std::make_shared<StreamMirrorRing>();
            ring->reset(kSR);
            auto voice = std::make_shared<AudioEngine::AppCaptureVoice>();
            voice->ring = ring;
            s.engine.setAppCaptureVoices(makeVoices({ voice }));

            std::vector<float> inSilence((size_t) kBlock, 0.0f);
            const float* ins[2] = { inSilence.data(), inSilence.data() };
            std::vector<float> outL((size_t) kBlock), outR((size_t) kBlock);
            float* outs[2] = { outL.data(), outR.data() };
            auto runBlock = [&] { s.engine.audioDeviceIOCallbackWithContext(ins, 2, outs, 2, kBlock, {}); };
            auto pushConst = [&ring](int n)
            {
                std::vector<float> l((size_t) n, 0.8f), r((size_t) n, 0.8f);
                ring->push(l.data(), r.data(), n);
            };

            s.engine.play();
            runBlock();                              // primeFrom 確定
            pushConst((int) (kSR * 0.05));           // 目標水位ちょうどでプライム
            // デクリック過渡を流しつつ push/pull を釣り合わせる
            for (int b = 0; b < 10; ++b) { runBlock(); pushConst(kBlock); }
            runBlock();
            // クリップ 0.5 (mono→L/R 複製・center 減衰なし) + 取り込み 0.8 = 1.3
            expect(std::abs(outL[0] - 1.3f) < 0.01f, "L = clip + capture while playing");
            expect(std::abs(outR[0] - 1.3f) < 0.01f, "R = clip + capture while playing");

            // 通常トラックのソロ → ソロでない voice は黙る (クリップだけが鳴る)
            clipTrack->setSoloed(true);
            for (int b = 0; b < 10; ++b) { pushConst(kBlock); runBlock(); }
            expect(std::abs(outL[0] - 0.5f) < 0.01f, "track solo silences the app voice");

            // アプリ voice のソロ → 通常トラックが黙り voice だけが鳴る
            clipTrack->setSoloed(false);
            voice->solo.store(true);
            for (int b = 0; b < 12; ++b) { pushConst(kBlock); runBlock(); }
            expect(std::abs(outL[0] - 0.8f) < 0.01f, "app voice solo silences normal tracks");

            s.engine.setAppCaptureVoices(nullptr);
        }
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

    // ── shutdown が遅延破棄待ちクリップを解放する (crash id37 の回帰テスト) ──
    // pendingGraveyard のクリップが ~AudioEngine のメンバ破棄まで残ると、MainComponent では
    // trackManager (thumbnail が参照する thumbnailCache スレッドの所有者) が先に破棄されるため
    // ~AudioThumbnail → removeTimeSliceClient が破棄済みスレッドを触る UAF になる。
    // ~MainComponent 本体が trackManager 生存中に呼ぶ shutdown() で解放されることを固定する。
    void testShutdownReleasesPendingGraveyard()
    {
        beginTest("shutdown releases deferred clips (dtor-order UAF guard)");
        auto wav = tempDir.getChildFile("c05g.wav");
        expect(writeMonoConstWav(wav, (int)kSR, 0.5f), "source write");

        Scene s;
        auto* t = s.addConstTrack(wav, 1.0);
        s.start();

        // クリップを lane から外して遅延破棄へ渡すが、preparePlayback は挟まない
        // (= スナップショットの graveyard へ移らず pendingGraveyard に残ったまま終了、を再現)
        auto* lane = t->getLane(0);
        expect(lane != nullptr && !lane->clips.empty(), "lane has the clip");
        std::vector<std::unique_ptr<AudioClip>> removed;
        removed.push_back(std::move(lane->clips[0]));
        lane->clips.erase(lane->clips.begin());
        s.engine.deferClipDestruction(std::move(removed));
        expectEquals((int) s.engine.getPendingGraveyardSizeForTests(), 1, "clip is pending");

        s.engine.shutdown();
        expectEquals((int) s.engine.getPendingGraveyardSizeForTests(), 0,
                     "shutdown must release deferred clips while the thumbnail cache is alive");
    }

    // ── preparePlayback の再構築は prepare 済みチェーンを再 prepare しない ──
    // 旧実装は再生中編集の再構築のたびに全チェーンへ無条件 prepareToPlay を呼び、
    // chainLock 保持下の重いフル再初期化 (releaseResources → prepareToPlay) を audio thread の
    // processBlock が待って全トラックの音が止まっていた (プラグイン挿入時のみ発生する回帰テスト)。
    // SR/blockSize が変わった時は従来どおり再 prepare されることも固定する。
    void testRebuildSkipsPreparedChains()
    {
        beginTest("preparePlayback rebuild skips already-prepared chains");
        auto wav = tempDir.getChildFile("c05p.wav");
        expect(writeMonoConstWav(wav, (int)kSR, 0.5f), "source write");

        Scene s;
        auto* t = s.addConstTrack(wav, 1.0);
        auto trackPlug  = std::make_unique<PrepareCountFakePlugin>();
        auto masterPlug = std::make_unique<PrepareCountFakePlugin>();
        auto* tp = trackPlug.get();
        auto* mp = masterPlug.get();
        t->getPluginChain().addPlugin(std::move(trackPlug));
        s.engine.getMasterChain().addPlugin(std::move(masterPlug));
        s.start();

        expect(tp->prepareCount >= 1, "track plugin prepared at start");
        expect(mp->prepareCount >= 1, "master plugin prepared at start");
        const int tc = tp->prepareCount, mc = mp->prepareCount;

        // 再生中編集の再構築を模す: 同じ SR/blockSize での preparePlayback は prepare を呼ばない
        s.engine.play();
        s.engine.preparePlayback(*s.tm);
        s.engine.preparePlayback(*s.tm);
        expectEquals(tp->prepareCount, tc, "rebuild does not re-prepare track chain");
        expectEquals(mp->prepareCount, mc, "rebuild does not re-prepare master chain");

        // releaseResources 後 (prepared フラグが落ちる) は再 prepare される (従来動作の維持)
        t->getPluginChain().releaseResources();
        s.engine.preparePlayback(*s.tm);
        expectEquals(tp->prepareCount, tc + 1, "released chain is re-prepared on rebuild");
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

    // MIDI キーボードのライブ入力 (pushLiveMidi / setLiveMidiTargetTrack)。
    // クリップ 0 個の MIDI トラックも snap->midi / synths に載る (preparePlayback の変更)、
    // 停止中は内蔵シンセ + INS チェーン (VSTi) の両方が鳴る、再生中に二重発音しない
    // (drain の直接 noteOn と mb 経由が重ならない)、ターゲット解除で鳴り止む、を固定する
    void testLiveMidiInput()
    {
        beginTest("live MIDI input: empty MIDI track sounds via synth and INS chain (stopped/playing)");

        // 停止中プレビューのパンは sin/cos 則 (センター ≒ 0.707)。再生ブランチはリニア
        // バランス則 (センター減衰なし = 1.0)。内蔵シンセ 1 ボイスのフル振幅は velocity*0.25
        const float kCenterPan = std::cos(0.5f * juce::MathConstants<float>::halfPi);

        auto addEmptyMidiTrack = [](Scene& s, bool synthOn) -> Track*
        {
            auto* t = s.tm->addTrack({}, false);
            t->setMidiTrack(true);
            t->setSynthEnabled(synthOn);
            t->setVolume(0.0f);
            t->setPan(0.0f);
            return t;
        };

        // (1) 停止中 + 内蔵シンセ: noteOn で鳴り、noteOff のリリース (50ms) 後に無音へ。
        //     ターゲット解除 (トラック選択の切替相当) でも allNotesOff で鳴り止む
        {
            Scene s;
            auto* t = addEmptyMidiTrack(s, /*synthOn*/ true);
            s.start();
            s.engine.setLiveMidiTargetTrack(0);

            s.engine.pushLiveMidi(juce::MidiMessage::noteOn(1, 69, (juce::uint8) 127));
            float pL = 0, pR = 0;
            runBlocks(s.engine, 8, &pL, &pR);
            expectWithinAbsoluteError(pL, 0.25f * kCenterPan, 0.03f,
                                      "stopped: synth sounds on live note-on (L)");
            expectWithinAbsoluteError(pR, 0.25f * kCenterPan, 0.03f,
                                      "stopped: synth sounds on live note-on (R)");
            // 停止中のライブ演奏でも Peak/VU が反応する (マスター + トラック出力メータ)
            expect(s.engine.getPeakL() > -40.0f, "stopped: master peak reacts to live playing");
            expect(s.engine.getTrackOutputPeakL(0) > -40.0f, "stopped: track meter reacts to live playing");

            s.engine.pushLiveMidi(juce::MidiMessage::noteOff(1, 69));
            runBlocks(s.engine, 30);   // リリース 50ms を通過
            runBlocks(s.engine, 4, &pL, &pR);
            expect(pL < 0.01f, "stopped: silent after live note-off");

            s.engine.pushLiveMidi(juce::MidiMessage::noteOn(1, 69, (juce::uint8) 127));
            runBlocks(s.engine, 4, &pL, &pR);
            expect(pL > 0.1f, "held note sounds before target change");
            s.engine.setLiveMidiTargetTrack(-1);
            runBlocks(s.engine, 30);
            runBlocks(s.engine, 4, &pL, &pR);
            expect(pL < 0.01f, "target cleared: hanging note is stopped (allNotesOff)");

            // 押しっぱなしのまま synth を ON→OFF に切替えても note-off は落とさない
            // (ゲートは note-on のみ。落とすと停止中プレビューがボイスを鳴らし続ける)
            s.engine.setLiveMidiTargetTrack(0);
            s.engine.pushLiveMidi(juce::MidiMessage::noteOn(1, 69, (juce::uint8) 127));
            runBlocks(s.engine, 4, &pL, &pR);
            expect(pL > 0.1f, "note sounds before synth toggle");
            t->setSynthEnabled(false);
            s.engine.pushLiveMidi(juce::MidiMessage::noteOff(1, 69));
            runBlocks(s.engine, 30);
            runBlocks(s.engine, 4, &pL, &pR);
            expect(pL < 0.01f, "note-off delivered even after synth was disabled mid-hold");
        }

        // (2) 停止中 + INS チェーン (VSTi 相当): synth OFF でもチェーンへ MIDI が届いて鳴る
        {
            Scene s;
            auto* t = addEmptyMidiTrack(s, /*synthOn*/ false);
            t->getPluginChain().addPlugin(std::make_unique<MidiTriggeredFakePlugin>());
            s.start();
            s.engine.setLiveMidiTargetTrack(0);

            s.engine.pushLiveMidi(juce::MidiMessage::noteOn(1, 60, (juce::uint8) 100));
            float pL = 0, pR = 0;
            runBlocks(s.engine, 4, &pL, &pR);
            expectWithinAbsoluteError(pL, 0.5f * kCenterPan, 0.03f,
                                      "stopped: live MIDI reaches the INS chain (VSTi)");
            expect(s.engine.getPeakL() > -40.0f, "stopped: master peak reacts to chain output");
            expect(s.engine.getTrackOutputPeakL(0) > -40.0f, "stopped: track meter reacts to chain output");

            s.engine.pushLiveMidi(juce::MidiMessage::noteOff(1, 60));
            runBlocks(s.engine, 2);
            runBlocks(s.engine, 4, &pL, &pR);
            expect(pL < 0.01f, "stopped: chain silent after note-off");

            // ターゲット解除でチェーンの処理自体が止まる (押しっぱなしでも鳴り続けない)
            s.engine.pushLiveMidi(juce::MidiMessage::noteOn(1, 60, (juce::uint8) 100));
            runBlocks(s.engine, 2, &pL, &pR);
            expect(pL > 0.3f, "held chain note sounds before target change");
            s.engine.setLiveMidiTargetTrack(-1);
            runBlocks(s.engine, 4, &pL, &pR);
            expect(pL < 0.01f, "target cleared: chain stops rendering");

            // ターゲットを戻しても鳴り出さない: 停止中の切替時に旧チェーンへ all-notes-off
            // (liveMidiChainFlush) が届いている (旧実装は flush が再生ループ専用で、押しっぱなし
            // ノートがプラグイン内部に残り、再ターゲットで復活して鳴り続けた)
            s.engine.setLiveMidiTargetTrack(0);
            runBlocks(s.engine, 4, &pL, &pR);
            expect(pL < 0.01f, "re-target: stale held note was flushed while stopped");

            // ミュート中もイベントはチェーンへ届く (出力だけ混ぜない):
            // ミュート中に離した鍵がミュート解除後に鳴り残らない
            s.engine.pushLiveMidi(juce::MidiMessage::noteOn(1, 62, (juce::uint8) 100));
            runBlocks(s.engine, 2, &pL, &pR);
            expect(pL > 0.3f, "note sounds before mute");
            t->setMuted(true);
            runBlocks(s.engine, 2, &pL, &pR);
            expect(pL < 0.01f, "muted: chain output not mixed");
            s.engine.pushLiveMidi(juce::MidiMessage::noteOff(1, 62));
            runBlocks(s.engine, 2);
            t->setMuted(false);
            runBlocks(s.engine, 4, &pL, &pR);
            expect(pL < 0.01f, "unmuted: note released during mute stays off");
        }

        // (3) 再生中 + 内蔵シンセ: ちょうど 1 ボイス (~0.25)。二重発音なら ~0.5 になる
        {
            Scene s;
            addEmptyMidiTrack(s, /*synthOn*/ true);
            s.start();
            s.engine.setLiveMidiTargetTrack(0);
            s.engine.play();

            s.engine.pushLiveMidi(juce::MidiMessage::noteOn(1, 69, (juce::uint8) 127));
            float pL = 0, pR = 0;
            runBlocks(s.engine, 8, &pL, &pR);
            expectWithinAbsoluteError(pL, 0.25f, 0.03f,
                                      "playing: exactly one voice (no double trigger)");
        }

        // (4) 再生中 + INS チェーン: ライブ MIDI が mb 経由でチェーンへ届く
        {
            Scene s;
            auto* t = addEmptyMidiTrack(s, /*synthOn*/ false);
            t->getPluginChain().addPlugin(std::make_unique<MidiTriggeredFakePlugin>());
            s.start();
            s.engine.setLiveMidiTargetTrack(0);
            s.engine.play();

            s.engine.pushLiveMidi(juce::MidiMessage::noteOn(1, 60, (juce::uint8) 100));
            float pL = 0, pR = 0;
            runBlocks(s.engine, 4, &pL, &pR);
            expectWithinAbsoluteError(pL, 0.5f, 0.03f,
                                      "playing: live MIDI reaches the INS chain");
        }
    }
};

static AudioEngineRealtimeTests audioEngineRealtimeTests;
