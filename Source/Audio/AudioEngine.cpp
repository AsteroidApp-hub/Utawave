// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

#include "AudioEngine.h"
#include "../Tracks/TrackManager.h"
#include "../Tracks/MidiClip.h"
#include "../VST/PluginChain.h"
#include "../MIDI/InternalSynth.h"
#include "AudioDeviceSettings.h"
#include "FileStreamVoice.h"
#include <cmath>
#include <unordered_map>
#include <unordered_set>

// 簡易リバーブのプレート風キャラクタ。再生用バスとモニター返し用バスで同一設定にして、
// モニターで聞こえる残響と書き出し時の残響を一致させる。
static inline juce::Reverb::Parameters makePlateReverbParams()
{
    juce::Reverb::Parameters p;
    p.roomSize   = 0.82f;   // 長めの残響
    p.damping    = 0.22f;   // 高域を残す
    p.wetLevel   = 1.0f;    // ウェットだけ返す (ドライは送り元で確保済み)
    p.dryLevel   = 0.0f;
    p.width      = 1.0f;
    p.freezeMode = 0.0f;
    return p;
}

// VU 時定数 (IEC 268-17 準拠 ~300ms 積分)。
// 1次 LPF を「ブロック単位」で適用するため、係数は SR とブロック長から都度算出する。
// coef = exp(-blockSec / tauSec) で、SR や buffer size が変わってもメーター応答は一定。
static constexpr double kVuTauSec = 0.300;
static inline float computeVuCoef(double sampleRate, int numSamples)
{
    if (sampleRate <= 0.0 || numSamples <= 0) return 0.97f;
    const double blockSec = (double) numSamples / sampleRate;
    return (float) std::exp(-blockSec / kVuTauSec);
}

// トラック出力メータ計測（plugin 通過後のステレオ trackBuf を読む）。
// gainL / gainR にトラックの Vol×Pan を渡すと、ポストフェーダー（フェーダーに追従する）の
// レベルを計測する。ピークは線形スケールなので magnitude に gain を掛けるだけで正確。
static inline void measureStereoBuf(juce::AudioBuffer<float>& buf, int numSamples,
                                    std::atomic<float>& peakL, std::atomic<float>& peakR,
                                    float& vuSmL, float& vuSmR,
                                    std::atomic<float>& vuL, std::atomic<float>& vuR,
                                    float vuCoef,
                                    float gainL = 1.0f, float gainR = 1.0f)
{
    // JUCE の SIMD 最適化された getMagnitude を使う（手書きループより速い）
    float magL = 0.0f, magR = 0.0f;
    if (buf.getNumChannels() >= 1)
        magL = buf.getMagnitude(0, 0, numSamples) * gainL;
    if (buf.getNumChannels() >= 2)
        magR = buf.getMagnitude(1, 0, numSamples) * gainR;
    else
        magR = magL;

    peakL.store(juce::Decibels::gainToDecibels(magL, -96.0f));
    peakR.store(juce::Decibels::gainToDecibels(magR, -96.0f));
    const float oneMinus = 1.0f - vuCoef;
    vuSmL = vuSmL * vuCoef + magL * oneMinus;
    vuSmR = vuSmR * vuCoef + magR * oneMinus;
    vuL.store(juce::Decibels::gainToDecibels(vuSmL, -96.0f));
    vuR.store(juce::Decibels::gainToDecibels(vuSmR, -96.0f));
}

// トラック出力メータの 1 ブロック分の減衰 (ピークは 0.80 乗算 ≈ 0.4〜0.5 秒で -96 へ、
// VU は vuCoef 追従で 0 へ)。停止時ブランチと、再生中に「このブロックで測定されなかった」
// トラック (ミュート/ソロ外/フォルダごとミュート/子が鳴らず fed されなかったフォルダバス) が
// 共用する。測定されないトラックを減衰させないとメータが最後の値のまま凍結する。
// 完全無音 (-96 到達) は早期 return して log10 を省く (アイドル負荷対策)。
static inline void decayTrackMeter(std::atomic<float>& peakL, std::atomic<float>& peakR,
                                   float& vuSmL, float& vuSmR,
                                   std::atomic<float>& vuL, std::atomic<float>& vuR,
                                   float vuCoef)
{
    const bool peakSilent = peakL.load() <= -96.0f && peakR.load() <= -96.0f;
    const bool vuSilent   = vuSmL < 1.0e-7f && vuSmR < 1.0e-7f;
    if (peakSilent && vuSilent) return;

    auto decayPeak = [](std::atomic<float>& vDb)
    {
        const float db = vDb.load();
        if (db <= -96.0f) return;
        const float g = juce::Decibels::decibelsToGain(db, -96.0f) * 0.80f;
        vDb.store(juce::Decibels::gainToDecibels(g, -96.0f));
    };
    decayPeak(peakL);
    decayPeak(peakR);
    vuSmL *= vuCoef;
    vuSmR *= vuCoef;
    vuL.store(juce::Decibels::gainToDecibels(vuSmL, -96.0f));
    vuR.store(juce::Decibels::gainToDecibels(vuSmR, -96.0f));
}

void AudioEngine::previewMidiNote(int trackIdx, int note, float velocity, bool isOn)
{
    const juce::ScopedLock sl(previewMidiLock);
    pendingPreviewMidi.push_back({ trackIdx, note, velocity, isOn });
}

void AudioEngine::pushLiveMidi(const juce::MidiMessage& msg)
{
    const juce::ScopedLock sl(liveMidiLock);
    // MIDI スレッド側で先に確保しておく (audio の trylock を realloc 中に外させない)
    if (pendingLiveMidi.capacity() < 600)
        pendingLiveMidi.reserve(600);
    // audio callback が止まっている (デバイス停止等) 間に無限に溜めない。
    // 溜まった分は stale なので捨てるが、note-off も一緒に落ちるため all-notes-off を仕込み、
    // callback 再開時に押しっぱなしノートが残らないようにする (synth / chain の両方が drain で受ける)
    if (pendingLiveMidi.size() >= 512)
    {
        pendingLiveMidi.clear();
        for (int ch = 1; ch <= 16; ++ch)
            pendingLiveMidi.push_back(juce::MidiMessage::allNotesOff(ch));
    }
    pendingLiveMidi.push_back(msg);
}

// ── 録音設定スナップショットの公開・回収 (recLock の lock-free 化) ──
void AudioEngine::publishRecConfig(std::shared_ptr<const RecordingConfig> next, bool drain)
{
    std::shared_ptr<const RecordingConfig> old;
    {
        const juce::SpinLock::ScopedLockType l(recConfigLock);
        old = std::move(activeRecConfig);
        activeRecConfig = std::move(next);
    }
    // teardown (writer 破棄を伴う) のときだけ、audio thread が旧 config を手放すまで待つ。
    // これにより呼び出し側が直後に ThreadedWriter を破棄しても UAF にならない (旧 recLock のバリア相当)。
    if (drain && old != nullptr)
    {
        // 通常は 1 ブロックで解消。500ms 超は audio thread 停止等の異常なので打ち切る
        // (UI ハング防止。打ち切り時も retiredRecConfigs が参照を保持する)
        const auto deadline = juce::Time::getMillisecondCounterHiRes() + 500.0;
        while (old.use_count() > 1)
        {
            if (juce::Time::getMillisecondCounterHiRes() > deadline)
            {
                DBG("AudioEngine: recording config drain timed out (audio thread stalled?)");
                jassertfalse;
                break;
            }
            juce::Thread::yield();
        }
    }
    {
        const juce::ScopedLock r(reclaimLock);
        if (old) retiredRecConfigs.push_back(std::move(old));
    }
    sweepRetiredRecConfigs();
}

// retiredRecConfigs も message + 書き出し bg スレッドから触られうるため reclaimLock で直列化する。
void AudioEngine::sweepRetiredRecConfigs()
{
    const juce::ScopedLock r(reclaimLock);
    retiredRecConfigs.erase(
        std::remove_if(retiredRecConfigs.begin(), retiredRecConfigs.end(),
                       [](const std::shared_ptr<const RecordingConfig>& c) { return c.use_count() == 1; }),
        retiredRecConfigs.end());
}

// ── モニター FX チェーン config の公開 (recConfig と同じ作法) ──
void AudioEngine::publishMonConfig(std::shared_ptr<const MonitorConfig> next, bool drain)
{
    std::shared_ptr<const MonitorConfig> old;
    {
        const juce::SpinLock::ScopedLockType l(monConfigLock);
        old = std::move(activeMonConfig);
        activeMonConfig = std::move(next);
    }
    // drain (チェーン破棄を伴う = Track 削除) のときだけ、audio thread が旧 config を手放すまで待つ。
    // これにより呼び出し側が直後に Track (= その PluginChain) を破棄しても UAF にならない。
    if (drain && old != nullptr)
    {
        const auto deadline = juce::Time::getMillisecondCounterHiRes() + 500.0;
        while (old.use_count() > 1)
        {
            if (juce::Time::getMillisecondCounterHiRes() > deadline)
            {
                DBG("AudioEngine: monitor config drain timed out (audio thread stalled?)");
                jassertfalse;
                break;
            }
            juce::Thread::yield();
        }
    }
    {
        const juce::ScopedLock r(reclaimLock);
        if (old) retiredMonConfigs.push_back(std::move(old));
    }
    sweepRetiredMonConfigs();
}

void AudioEngine::sweepRetiredMonConfigs()
{
    const juce::ScopedLock r(reclaimLock);
    retiredMonConfigs.erase(
        std::remove_if(retiredMonConfigs.begin(), retiredMonConfigs.end(),
                       [](const std::shared_ptr<const MonitorConfig>& c) { return c.use_count() == 1; }),
        retiredMonConfigs.end());
}

// ── 配信ミラー出力リングの公開 ──
// monConfig 等と違い drain (audio が手放すまで待つ) はしない: リングは呼び出し側
// (StreamMirrorOutput) も shared_ptr で所有するため use_count で「audio だけが残り」を
// 判定できず、また shared_ptr 所有そのものが UAF を防ぐ。退役リストが解放まで保持する
// ことで「audio thread が最後の所有者になって解放する」ことだけを防ぐ (回収は次の公開
// またはエンジン破棄時の message thread)。
void AudioEngine::publishMirrorRing(std::shared_ptr<StreamMirrorRing> next)
{
    std::shared_ptr<StreamMirrorRing> old;
    {
        const juce::SpinLock::ScopedLockType l(mirrorRingLock);
        old = std::move(activeMirrorRing);
        activeMirrorRing = std::move(next);
    }
    {
        const juce::ScopedLock r(reclaimLock);
        if (old) retiredMirrorRings.push_back(std::move(old));
    }
    sweepRetiredMirrorRings();
}

void AudioEngine::sweepRetiredMirrorRings()
{
    const juce::ScopedLock r(reclaimLock);
    retiredMirrorRings.erase(
        std::remove_if(retiredMirrorRings.begin(), retiredMirrorRings.end(),
                       [](const std::shared_ptr<StreamMirrorRing>& c) { return c.use_count() == 1; }),
        retiredMirrorRings.end());
}

void AudioEngine::setMirrorRing(std::shared_ptr<StreamMirrorRing> ring)
{
    if (ring != nullptr)
        ring->reset(currentSampleRate);   // 公開前にソース SR を確定 (以降のデバイス再起動は aboutToStart が追従)
    publishMirrorRing(std::move(ring));
}

// ── アプリ音声取り込みリングの公開 (mirror と同じ作法・drain 無し) ──
// リングは AppAudioCapture 側も shared_ptr で所有するため use_count で「audio だけが残り」を
// 判定できない。退役リストが「audio thread が最後の所有者になって解放する」ことだけを防ぐ。
void AudioEngine::publishAppCaptureRing(std::shared_ptr<StreamMirrorRing> next)
{
    std::shared_ptr<StreamMirrorRing> old;
    {
        const juce::SpinLock::ScopedLockType l(appCaptureRingLock);
        old = std::move(activeAppCaptureRing);
        activeAppCaptureRing = std::move(next);
    }
    {
        const juce::ScopedLock r(reclaimLock);
        if (old) retiredAppCaptureRings.push_back(std::move(old));
    }
    sweepRetiredAppCaptureRings();
}

void AudioEngine::sweepRetiredAppCaptureRings()
{
    const juce::ScopedLock r(reclaimLock);
    retiredAppCaptureRings.erase(
        std::remove_if(retiredAppCaptureRings.begin(), retiredAppCaptureRings.end(),
                       [](const std::shared_ptr<StreamMirrorRing>& c) { return c.use_count() == 1; }),
        retiredAppCaptureRings.end());
}

void AudioEngine::setAppCaptureRing(std::shared_ptr<StreamMirrorRing> ring)
{
    // mirror と違い SR は触らない: ソース SR はキャプチャ側のもので、登録側が
    // ring->reset(captureSR) してから渡す契約 (ヘッダのコメント参照)。
    publishAppCaptureRing(std::move(ring));
}

// アプリ音声取り込み: キャプチャリングから読んで出力へ加算する (ライブ専用)。
// scratch はデバイスバッファ長で確保済みだが、numSamples がそれを超えるブロック
// (デバイス再起動の不整合期間) はチャンク分割で境界内に収める (applyDelayLine と同じガード。
// reader は逐次状態なので分割しても連続読みになる)。
void AudioEngine::mixAppCapture(StreamMirrorRing& ring, float* const* outputChannelData,
                                int numOutputChannels, int numSamples) noexcept
{
    if (numOutputChannels <= 0 || outputChannelData[0] == nullptr) return;
    const int cap = (int) appCapScratchL.size();
    if (cap <= 0) return;

    const float g = appCaptureGainLinear.load(std::memory_order_relaxed);
    float* outL = outputChannelData[0];
    float* outR = (numOutputChannels > 1) ? outputChannelData[1] : nullptr;

    int done = 0;
    while (done < numSamples)
    {
        const int m = juce::jmin(numSamples - done, cap);
        appCaptureReader.pull(ring, appCapScratchL.data(), appCapScratchR.data(), m, currentSampleRate);
        juce::FloatVectorOperations::addWithMultiply(outL + done, appCapScratchL.data(), g, m);
        if (outR != nullptr)
            juce::FloatVectorOperations::addWithMultiply(outR + done, appCapScratchR.data(), g, m);
        done += m;
    }
}

void AudioEngine::setMonitorChain(PluginChain* chain, int inputCh, bool stereo, float pan, float volumeDb)
{
    // 停止中モニタでも audio thread が processBlock できるよう、現 SR/blockSize で prepare しておく
    // (未 prepare のプラグインを叩くとクラッシュしうる)。既に同設定なら何もしない (再 prepare で
    // プラグイン状態がリセットされてグリッチになるのを避ける)。prepare は message thread・
    // PluginChain::chainLock が audio thread の processBlock と直列化する。
    if (chain != nullptr && chain->getActivePluginCountAtomic() > 0
        && currentSampleRate > 0.0 && currentBufferSize > 0
        && !chain->isPreparedFor(currentSampleRate, currentBufferSize))
        chain->prepareToPlay(currentSampleRate, currentBufferSize);

    auto next = std::make_shared<MonitorConfig>();
    next->chain   = chain;
    next->inputCh = inputCh;
    next->stereo  = stereo;
    next->pan     = pan;
    next->gain    = juce::Decibels::decibelsToGain(volumeDb);
    // チェーンの破棄は伴わない (Track 所有・削除時は clearPlayback が drain する)。drain 不要。
    publishMonConfig(std::move(next), /*drain=*/ false);
}

void AudioEngine::setRecordingTarget(juce::AudioFormatWriter::ThreadedWriter* writer,
                                     LiveRecordingBuffer* liveBuffer, int inputCh, bool stereo)
{
    auto next = std::make_shared<RecordingConfig>();
    // 遡及録音の設定は現 config から引き継ぐ (この API は targets のみ差し替える)。
    {
        const juce::SpinLock::ScopedLockType l(recConfigLock);
        if (activeRecConfig)
        {
            next->retro        = activeRecConfig->retro;
            next->retroLiveBuf = activeRecConfig->retroLiveBuf;
            next->retroInputCh = activeRecConfig->retroInputCh;
            next->retroStereo  = activeRecConfig->retroStereo;
        }
    }
    if (writer != nullptr)
    {
        next->targets.push_back({ writer, liveBuffer, inputCh, stereo });
        // 新しい録音セッションの開始: 実書き込み開始位置マーカーをリセットする。
        // config 公開前に store するので、audio thread が新 config で書き始めるより
        // 必ず先になる (teardown の writer==nullptr ではリセットしない: 停止処理が
        // クリップ配置のためこの値を読むのは teardown の後)
        recFirstWritePos.store(-1.0);
    }
    // 既存ターゲットを破棄するため teardown バリアを張る (録音停止時の writer.reset() 前提)。
    publishRecConfig(std::move(next), /*drain=*/ true);
}

void AudioEngine::addRecordingTarget(juce::AudioFormatWriter::ThreadedWriter* writer,
                                     LiveRecordingBuffer* liveBuffer, int inputCh, bool stereo)
{
    if (writer == nullptr) return;
    auto next = std::make_shared<RecordingConfig>();
    {
        const juce::SpinLock::ScopedLockType l(recConfigLock);
        if (activeRecConfig) *next = *activeRecConfig;   // 既存 (targets + retro) をコピー
    }
    next->targets.push_back({ writer, liveBuffer, inputCh, stereo });
    // 追加のみ (既存 writer は新 config にも残る) なので drain 不要。
    publishRecConfig(std::move(next), /*drain=*/ false);
}

void AudioEngine::clearRecordingTargets()
{
    auto next = std::make_shared<RecordingConfig>();
    {
        const juce::SpinLock::ScopedLockType l(recConfigLock);
        if (activeRecConfig)   // 遡及録音設定は維持し、targets だけ空にする
        {
            next->retro        = activeRecConfig->retro;
            next->retroLiveBuf = activeRecConfig->retroLiveBuf;
            next->retroInputCh = activeRecConfig->retroInputCh;
            next->retroStereo  = activeRecConfig->retroStereo;
        }
    }
    publishRecConfig(std::move(next), /*drain=*/ true);
}

void AudioEngine::setRetrospectiveTarget(juce::AudioFormatWriter::ThreadedWriter* writer,
                                         LiveRecordingBuffer* liveBuf, int inputCh, bool stereo)
{
    auto next = std::make_shared<RecordingConfig>();
    {
        const juce::SpinLock::ScopedLockType l(recConfigLock);
        if (activeRecConfig) next->targets = activeRecConfig->targets;   // targets は維持
    }
    next->retro        = writer;
    next->retroLiveBuf = liveBuf;
    next->retroInputCh = inputCh;
    next->retroStereo  = stereo;
    // 新しい遡及キャプチャの開始: 実書き込み開始位置マーカーをリセット
    // (setRecordingTarget と同じ作法。解除時はリセットしない = 停止処理が配置で読む)
    if (writer != nullptr)
        retroFirstWritePos.store(-1.0);
    // writer==nullptr の確定時に直後 retroWriter.reset() するため teardown バリアを張る。
    publishRecConfig(std::move(next), /*drain=*/ true);
}

void AudioEngine::setRetrospectiveLiveBuffer(LiveRecordingBuffer* liveBuf)
{
    auto next = std::make_shared<RecordingConfig>();
    {
        const juce::SpinLock::ScopedLockType l(recConfigLock);
        if (activeRecConfig) *next = *activeRecConfig;
    }
    next->retroLiveBuf = liveBuf;
    // liveBuffer の所有は Track 側。ここで writer は破棄しないため drain 不要。
    publishRecConfig(std::move(next), /*drain=*/ false);
}

// ── アプリ設定の公開 (audio スレッドのメトロノーム区間が読む bpmChanges/meterChanges の data race 回避) ──
void AudioEngine::setAppSettings(const AppSettings& s)
{
    appSettings = s;  // UI スレッド読み出し用 (preparePlayback の autoCrossfade / zeroCrossingFade)

    // audio スレッド読み出し用スナップショットを公開する (message thread からのみ呼ばれる前提)。
    auto snap = std::make_shared<const AppSettings>(s);
    std::shared_ptr<const AppSettings> old;
    {
        const juce::SpinLock::ScopedLockType l(appSettingsLock);
        old = std::move(activeAppSettings);
        activeAppSettings = std::move(snap);
    }
    {
        const juce::ScopedLock r(reclaimLock);
        if (old) retiredAppSettings.push_back(std::move(old));
    }
    sweepRetiredAppSettings();
}

void AudioEngine::sweepRetiredAppSettings()
{
    const juce::ScopedLock r(reclaimLock);
    retiredAppSettings.erase(
        std::remove_if(retiredAppSettings.begin(), retiredAppSettings.end(),
                       [](const std::shared_ptr<const AppSettings>& a) { return a.use_count() == 1; }),
        retiredAppSettings.end());
}

AudioEngine::AudioEngine()
    : masterChain(std::make_unique<PluginChain>())
{
    // activeSnapshot / activeRecConfig / activeAppSettings は常に非 null に保つ
    // (audio thread の null チェックを不要にする)。
    activeSnapshot    = std::make_shared<PlaybackSnapshot>();
    activeRecConfig   = std::make_shared<const RecordingConfig>();
    activeAppSettings = std::make_shared<const AppSettings>();
    activeMonConfig   = std::make_shared<const MonitorConfig>();
    formatManager.registerBasicFormats();

    // ディスクストリーミングの先読みスレッドを起動 (エンジン存続中ずっと走る。
    // クライアント = FileStreamVoice が無い間はイベント待ちでスリープ)。
    streamThread.startThread();

    // dB メータ配列は無音 (-96 dB) で初期化する。0.0f のままだと、まだ一度も
    // 書き込まれていないスロット (再生していない新規トラック等) が 0 dBFS =
    // フルスケール表示になってしまう。
    for (auto& v : inputPeak)     v.store(-96.0f);
    for (auto& v : inputVU)       v.store(-96.0f);
    for (auto& v : trackOutPeakL) v.store(-96.0f);
    for (auto& v : trackOutPeakR) v.store(-96.0f);
    for (auto& v : trackOutVUL)   v.store(-96.0f);
    for (auto& v : trackOutVUR)   v.store(-96.0f);
}

AudioEngine::~AudioEngine()
{
    shutdown();
}

void AudioEngine::initialise()
{
    auto err = AudioDeviceSettings::initialise(deviceManager, 2, 2);
    if (err.isNotEmpty())
        DBG("AudioEngine init error: " << err);

    deviceManager.addAudioCallback(this);
}

void AudioEngine::shutdown()
{
    deviceManager.removeAudioCallback(this);   // 以後 audio thread は来ない
    workerPool.stop();                         // マルチコアワーカーを停止 (audio 停止後)

    // 先読みボイスを全て解放してからスレッドを停止する。ボイスを握っているのは
    // activeSnapshot / retiredSnapshots / voicePool の 3 か所なので、空スナップショットへ
    // 差し替えて手放す。各ボイス dtor は removeTimeSliceClient で当該スライス完了を待つ
    // (まだ streamThread は生きている)。最後にスレッドを止める。
    { const juce::SpinLock::ScopedLockType l(snapshotLock);
      activeSnapshot = std::make_shared<PlaybackSnapshot>(); }
    {
        const juce::ScopedLock sl(reclaimLock);
        retiredSnapshots.clear();
    }
    // 遅延破棄待ちのクリップもここで解放する。~AudioEngine のメンバ破棄まで残すと、
    // MainComponent では trackManager (AudioClip の thumbnail が参照する thumbnailCache の
    // TimeSliceThread を所有) が audioEngine より後に宣言 = 先に破棄されるため、
    // ~AudioThumbnail → removeTimeSliceClient が破棄済みスレッドを触って落ちる (crash id37)。
    // ~MainComponent 本体は trackManager 生存中に shutdown() を呼ぶのでここが安全な解放点。
    pendingGraveyard.clear();
    voicePool.clear();
    streamThread.stopThread(2000);

    mixer.releaseResources();
    deviceManager.closeAudioDevice();
}

void AudioEngine::play()
{
    // 停止中の編集で dirty が立っていたら、再生開始前に再構築する。
    // (UAF防止: 編集後の古い PlaybackClip ポインタを掃除してからオーディオスレッドへ)
    if (playbackDirty.load() && lastTrackManager != nullptr)
        preparePlayback(*lastTrackManager);

    playing.store(true);
}

void AudioEngine::stop()
{
    playing.store(false);
    clickLastBeatInt = -1;  // メトロノームの拍カウントリセット
    // 全 synth に「全ノートオフ」をキュー経由で送信し、stuck note を防ぐ。
    // trackIdx < 0 = 現スナップショットの全 synth が対象 (audio thread が解釈)。
    {
        const juce::ScopedLock sl(previewMidiLock);
        pendingPreviewMidi.push_back({ /*trackIdx=*/ -1, /*note=*/ -1, 0.0f, false });
    }
    // 退役スナップショットの回収は publishSnapshot 内の sweep に任せる (stop() は書き出し経路から
    // message thread 以外で呼ばれうるため、ここで retiredSnapshots を触らない)。
}

void AudioEngine::rewind()
{
    playing.store(false);
    currentPosition.store(0.0);
    const juce::ScopedLock sl(previewMidiLock);
    pendingPreviewMidi.push_back({ /*trackIdx=*/ -1, /*note=*/ -1, 0.0f, false });
}

// ゼロクロスポイント検索（メッセージスレッドで呼ぶ）
static juce::int64 findZeroCrossing(juce::AudioFormatReader* reader,
                                     juce::int64 nearSample,
                                     juce::int64 searchRange,
                                     bool backward)
{
    if (!reader || searchRange <= 0) return nearSample;

    const int bufSize = (int)juce::jmin((juce::int64)512, searchRange);
    juce::AudioBuffer<float> buf(1, bufSize);
    buf.clear();

    juce::int64 startSample = backward
                               ? juce::jmax((juce::int64)0, nearSample - searchRange)
                               : nearSample;
    reader->read(&buf, 0, bufSize, startSample, true, false);

    if (backward)
    {
        float prev = buf.getSample(0, bufSize - 1);
        for (int i = bufSize - 2; i >= 0; --i)
        {
            float curr = buf.getSample(0, i);
            if ((prev <= 0.0f && curr > 0.0f) || (prev > 0.0f && curr <= 0.0f))
                return startSample + i + 1;
            prev = curr;
        }
    }
    else
    {
        float prev = buf.getSample(0, 0);
        for (int i = 1; i < bufSize; ++i)
        {
            float curr = buf.getSample(0, i);
            if ((prev <= 0.0f && curr > 0.0f) || (prev > 0.0f && curr <= 0.0f))
                return startSample + i;
            prev = curr;
        }
    }
    return nearSample;
}

// reader / voice プールのキー = (トラック, ファイル)。3 箇所 (生成 / readerPool / voicePool 再構築) で
// 必ず同一にするため 1 関数に集約する (ずれると毎回プール miss = 再生中編集の度に WAV を開き直す回帰)。
static juce::String makePoolKey(int trackIdx, const juce::File& file)
{
    return juce::String(trackIdx) + "\n" + file.getFullPathName();
}

void AudioEngine::preparePlayback(TrackManager& tm)
{
    lastTrackManager = &tm;
    playbackDirty.store(false);
    meterTrackCount.store(juce::jmin(tm.getTrackCount(), kMaxTracksMeters));

    // 現スナップショットを取得 (SR/blockSize 不変なら synth 実体を持ち回しボイス状態を保つ)。
    std::shared_ptr<PlaybackSnapshot> prevSnap;
    { const juce::SpinLock::ScopedLockType l(snapshotLock); prevSnap = activeSnapshot; }

    std::vector<PlaybackClip> newClips;
    // 同一ファイルを参照する複数クリップで AudioFormatReader を共有する。
    // (100クリップ分割の同一録音などでファイルハンドル上限に当たるのを防ぐ)
    // 前回開いた reader を readerPool から流用する (再生中編集のたびに WAV を開き直して再構築が
    // 長引く = 一瞬の停止感、を避ける)。clearPlayback をまたいでも保持されるのが prevSnap との違い。
    std::unordered_map<juce::String, std::shared_ptr<juce::AudioFormatReader>> readerCache = readerPool;
    // 先読みボイスも同様に流用する (再生中編集のたびに reader 2 本を開き直さない)。
    const bool streamingOn = diskStreamingEnabled.load();
    std::unordered_map<juce::String, std::shared_ptr<FileStreamVoice>> voiceCache = voicePool;

    // Click Track を検出: 音量・ミュートをメトロノームに連動
    bool clickTrackFound = false;
    Track* foundClickTrack = nullptr;  // 合成音を INS チェーンに通すため snap へ載せる
    for (int ti = 0; ti < tm.getTrackCount(); ++ti)
    {
        auto* track = tm.getTrack(ti);
        if (!track->isClickTrack()) continue;
        clickTrackFound = true;
        foundClickTrack = track;
        float trackGainLin = juce::Decibels::decibelsToGain(track->getVolume());
        metronomeVolume.store(trackGainLin * 0.5f);  // dB → linear * 0.5（クリックの基本音量）
        metronomeEnabled.store(!track->isMuted());
        break;
    }

    for (int ti = 0; ti < tm.getTrackCount(); ++ti)
    {
        auto* track = tm.getTrack(ti);
        if (track->isClickTrack()) continue;  // Click Track はクリップ再生しない
        // フォルダトラックはクリップを持たない (グループバス)。万一クリップが載っても再生対象に
        // しない (自身のチェーンは bus 経路で処理されるため、ここで拾うと同一ブロックでチェーンを
        // 二重に叩いて内部状態が壊れる)。
        if (track->isFolderTrack()) continue;
        // Mute / Solo はオーディオスレッドでライブ判定するため、ここでは除外しない

        float trackGainLin = juce::Decibels::decibelsToGain(track->getVolume());

        // Take レーンに Solo があればそれを再生、なければ Lane 0
        int playLaneIdx = 0;
        for (int li = 1; li < track->getLaneCount(); ++li)
        {
            auto* l = track->getLane(li);
            if (l && l->soloed) { playLaneIdx = li; break; }
        }
        auto* lane = track->getLane(playLaneIdx);
        if (!lane) continue;

        for (auto& clipPtr : lane->clips)
        {
            auto* clip = clipPtr.get();
            // reader / voice は **(トラック, ファイル)** 単位で共有する (ファイルパス単体ではない)。
            // マルチスレッド再生 (Part B) ではトラックごとに別スレッドが描画するため、同一ファイルを
            // 複数トラックが参照しても reader/voice を共有すると seek/SPSC が競合する。トラック内
            // (分割クリップ等) の共有はそのまま活かしつつ、トラック間の共有だけを断つ。
            const auto key = makePoolKey(ti, clip->getFile());
            std::shared_ptr<juce::AudioFormatReader> sharedReader;
            auto cacheIt = readerCache.find(key);
            if (cacheIt != readerCache.end())
            {
                sharedReader = cacheIt->second;
            }
            else
            {
                auto* rawReader = formatManager.createReaderFor(clip->getFile());
                if (!rawReader) continue;
                sharedReader.reset(rawReader);
                readerCache.emplace(key, sharedReader);
            }

            // ディスクストリーミングの先読みボイス (ファイル単位で 1 つ・流用)。生成は bg/fallback
            // の reader を各 1 本開く。失敗時 / OFF 時は voice=null のまま (renderClip が reader 直読み)。
            std::shared_ptr<FileStreamVoice> voice;
            if (streamingOn)
            {
                auto vIt = voiceCache.find(key);
                if (vIt != voiceCache.end())
                    voice = vIt->second;
                else
                {
                    std::unique_ptr<juce::AudioFormatReader> bgR(formatManager.createReaderFor(clip->getFile()));
                    std::unique_ptr<juce::AudioFormatReader> fbR(formatManager.createReaderFor(clip->getFile()));
                    if (bgR != nullptr && fbR != nullptr)
                    {
                        voice = std::make_shared<FileStreamVoice>(std::move(bgR), std::move(fbR), streamThread);
                        voiceCache.emplace(key, voice);
                    }
                }
            }

            PlaybackClip pc;
            pc.trackIdx       = ti;
            pc.file           = clip->getFile();
            pc.sourceClip     = clip;
            pc.sourceTrack    = track;
            pc.clipStart      = clip->getStartPosition();
            pc.clipEnd        = clip->getEndPosition();
            pc.fileOffset     = clip->getFileOffset();
            pc.trackGain      = trackGainLin;
            pc.gain           = trackGainLin * clip->getGain();  // 互換のため
            pc.fadeInSecs     = (float)clip->getFadeInSecs();
            pc.fadeOutSecs    = (float)clip->getFadeOutSecs();
            pc.fileSampleRate = sharedReader->sampleRate;
            pc.reader         = sharedReader;
            pc.voice          = voice;
            newClips.push_back(std::move(pc));
        }
    }

    // ── 自動クロスフェード処理 ──
    // 重なっている隣接クリップに、再生用 PlaybackClip 上でだけクロスフェードを設定する。
    // 永続 AudioClip のフェード値は変更しない。書き戻すと Undo 不可・再生/書き出しの度に
    // jmax で伸びてプロジェクトに焼き込まれるため (#H2/#L1)。UI の X 表示は autoCrossfade
    // ON 時に drawTrackRows が幾何ベースで描くので、書き戻し無しでも表示は保たれる。
    if (appSettings.autoCrossfade && newClips.size() >= 2)
    {
        // トラック順 → クリップ開始順でソート
        std::sort(newClips.begin(), newClips.end(),
                  [](const PlaybackClip& a, const PlaybackClip& b)
                  {
                      if (a.trackIdx != b.trackIdx) return a.trackIdx < b.trackIdx;
                      return a.clipStart < b.clipStart;
                  });

        for (size_t i = 0; i + 1 < newClips.size(); ++i)
        {
            auto& a = newClips[i];
            auto& b = newClips[i + 1];
            if (a.trackIdx != b.trackIdx) continue;

            // 同一の連続音声 (Alt+Click 分割) のペアはスキップ。テイク (別リージョン) は許可。
            if (a.sameContinuousAs(b)) continue;

            // 実際に重なっている場合のみ（overlap > 1ms）
            double overlapSecs = a.clipEnd - b.clipStart;
            if (overlapSecs < 0.001) continue;

            // クロスフェード長 = 重なり幅（クリップ長の半分を超えない）
            const double durA = a.clipEnd - a.clipStart;
            const double durB = b.clipEnd - b.clipStart;
            double actualXfade = juce::jmin(overlapSecs, durA * 0.5, durB * 0.5);
            actualXfade = juce::jmax(0.001, actualXfade);

            double fadeOutA = actualXfade;
            double fadeInB  = actualXfade;
            if (appSettings.zeroCrossingFade && a.reader && b.reader)
            {
                // A の末端付近でゼロクロスを後方検索 → フェード境界を少し延長
                juce::int64 rangeA  = (juce::int64)(actualXfade * a.fileSampleRate);
                juce::int64 boundA  = (juce::int64)((a.fileOffset + durA) * a.fileSampleRate);
                juce::int64 zcA     = findZeroCrossing(a.reader.get(), boundA, rangeA, true);
                fadeOutA += juce::jmax(0.0, (double)(boundA - zcA) / a.fileSampleRate);

                // B の先端付近でゼロクロスを前方検索
                juce::int64 rangeB  = (juce::int64)(actualXfade * b.fileSampleRate);
                juce::int64 boundB  = (juce::int64)(b.fileOffset * b.fileSampleRate);
                juce::int64 zcB     = findZeroCrossing(b.reader.get(), boundB, rangeB, false);
                fadeInB  += juce::jmax(0.0, (double)(zcB - boundB) / b.fileSampleRate);
            }

            // PlaybackClip 上にのみ反映 (永続 AudioClip は変更しない)。
            // クリップ長を超えない範囲にクランプ (renderClip の fadeOutStartSec が負になるのを防ぐ)。
            a.fadeOutSecs = juce::jmin((float)durA, juce::jmax(a.fadeOutSecs, (float)fadeOutA));
            b.fadeInSecs  = juce::jmin((float)durB, juce::jmax(b.fadeInSecs,  (float)fadeInB));
        }
    }

    // ── 重なりミュート: 見える波形（後から追加された＝lane->clips の後ろの方）だけを再生 ──
    // 各レーンのクリップ並び順は「描画順 = 後ろが上」になるので、後の clip が前の clip を覆う
    // 中抜き (#H9) で分割した tail はループ中に newClips へ追加すると pcI ポインタが無効化される
    // ため、ここに溜めてループ後にまとめて追加する。
    std::vector<PlaybackClip> pendingMidCoverTails;
    for (int ti = 0; ti < tm.getTrackCount(); ++ti)
    {
        auto* track = tm.getTrack(ti);
        for (int li = 0; li < track->getLaneCount(); ++li)
        {
            auto* lane = track->getLane(li);
            if (!lane) continue;

            for (size_t i = 0; i < lane->clips.size(); ++i)
            {
                auto* clipI = lane->clips[i].get();
                PlaybackClip* pcI = nullptr;
                for (auto& pc : newClips)
                    if (pc.sourceClip == clipI) { pcI = &pc; break; }
                if (!pcI) continue;

                // i より後の clip（より新しい・前面）が pcI と重なっているなら、その部分をミュート
                for (size_t j = i + 1; j < lane->clips.size(); ++j)
                {
                    auto* clipJ = lane->clips[j].get();
                    double jStart = clipJ->getStartPosition();
                    double jEnd   = clipJ->getEndPosition();
                    if (jEnd <= pcI->clipStart || jStart >= pcI->clipEnd) continue;

                    // 相手 clip の PlaybackClip。「真のクロスフェードか」の判定に使う。
                    PlaybackClip* pcJ = nullptr;
                    for (auto& pc : newClips)
                        if (pc.sourceClip == clipJ) { pcJ = &pc; break; }

                    if (jStart <= pcI->clipStart && jEnd >= pcI->clipEnd)
                    {
                        // 完全に覆われる → 再生スキップ
                        pcI->clipEnd = pcI->clipStart;
                        break;
                    }
                    else if (jStart > pcI->clipStart && jEnd >= pcI->clipEnd)
                    {
                        // 右側を覆われる。両クリップが重なり全体でフェードしている
                        // (= 意図的クロスフェード) なら、トリムせず両方を再生し
                        // renderClip にフェードアウト＋フェードインを同時加算させる
                        // (= 真のクロスフェード。継ぎ目の音量落ち/穴を防ぐ #H1)。
                        // 単なる重ね置き (前面クリップで差し替え) は短いデフォルトフェードしか
                        // 持たないため条件を満たさず、従来通りトリムされ二重再生は起きない。
                        const double ovr = pcI->clipEnd - jStart;
                        const bool isXfade = pcJ && ovr > 0.001
                            && !pcI->sameContinuousAs(*pcJ)  // 同一連続音声(分割)はクロスフェードにしない (#I2)。テイクは許可
                            && pcI->fadeOutSecs >= (float)(ovr - 0.005)
                            && pcJ->fadeInSecs  >= (float)(ovr - 0.005);
                        if (!isXfade)
                            pcI->clipEnd = jStart;   // 右側を覆われる → end をトリム
                    }
                    else if (jStart <= pcI->clipStart && jEnd < pcI->clipEnd)
                    {
                        // 左側を覆われる。同様に真のクロスフェードなら触らず両方再生する。
                        const double ovr = jEnd - pcI->clipStart;
                        const bool isXfade = pcJ && ovr > 0.001
                            && !pcI->sameContinuousAs(*pcJ)  // 同一連続音声(分割)はクロスフェードにしない (#I2)。テイクは許可
                            && pcI->fadeInSecs  >= (float)(ovr - 0.005)
                            && pcJ->fadeOutSecs >= (float)(ovr - 0.005);
                        if (!isXfade)
                        {
                            // start を進める（fileOffset 調整）
                            double trim = jEnd - pcI->clipStart;
                            pcI->clipStart = jEnd;
                            pcI->fileOffset += trim;
                        }
                    }
                    else if (jStart > pcI->clipStart && jEnd < pcI->clipEnd)
                    {
                        // 中抜き: clipJ が pcI の中央だけを覆う。pcI を head [clipStart, jStart] に
                        // 縮め、tail [jEnd, clipEnd] を別の再生クリップとして後で追加する。これを
                        // しないと中央区間で pcI と clipJ が両方鳴り音量が倍化する (#H9)。
                        PlaybackClip tail = *pcI;                     // shared_ptr reader 等ごとコピー
                        tail.fileOffset  += (jEnd - pcI->clipStart);  // tail 開始まで読み位置を進める
                        tail.clipStart    = jEnd;
                        // tail.clipEnd は元の pcI->clipEnd のまま (末尾のフェードアウトも維持)
                        pcI->clipEnd      = jStart;                   // pcI は head に縮める
                        pendingMidCoverTails.push_back(std::move(tail));
                        // pcI は head [clipStart, jStart] になったので、以降の j は
                        // jStart >= pcI->clipEnd で自然にスキップされる (head の追加被覆も処理される)。
                        // tail への追加被覆 (二重中抜き) は未対応。
                    }
                }
            }
        }
    }
    // 中抜きで生成した tail を再生クリップ群へ追加 (ループ後にまとめて行いポインタ無効化を回避)
    for (auto& t : pendingMidCoverTails)
        newClips.push_back(std::move(t));

    // ── スナップショットを構築 (すべてロック外で実行) ──
    auto snap = std::make_shared<PlaybackSnapshot>();
    snap->clickTrack = foundClickTrack;
    snap->clips = std::move(newClips);

    // トラック別インデックスを構築 (audio thread の毎ブロック全 clips 走査を排除)
    snap->clipsByTrack.assign((size_t)tm.getTrackCount(), {});
    for (int ci = 0; ci < (int)snap->clips.size(); ++ci)
    {
        const auto& pc = snap->clips[(size_t)ci];
        if (pc.trackIdx < 0 || pc.trackIdx >= (int)snap->clipsByTrack.size()) continue;
        if (pc.sourceTrack == nullptr || pc.sourceTrack->isClickTrack()) continue;
        auto& lst = snap->clipsByTrack[(size_t)pc.trackIdx];
        if (lst.empty())
            snap->clipTracks.emplace_back(pc.trackIdx, pc.sourceTrack);
        lst.push_back(ci);
    }

    // 次回 preparePlayback で流用するため、実際に使った reader だけを pool に残す
    // (もう参照されないファイルのハンドルは解放される)。
    // キーは (トラック, ファイル) 単位 (上の clip ループと一致させること)。同一ファイルを複数
    // クリップ/トラックが参照しても重複 emplace は最初の 1 つだけ残る (同一トラックは同一 reader)。
    readerPool.clear();
    for (auto& pc : snap->clips)
        if (pc.reader) readerPool.emplace(makePoolKey(pc.trackIdx, pc.file), pc.reader);

    // 先読みボイスも同様に、使われているものだけ残す (未使用ボイスはここで解放され、その dtor が
    // removeTimeSliceClient で当該スライス完了を待つ = message thread で安全)。
    voicePool.clear();
    for (auto& pc : snap->clips)
        if (pc.voice) voicePool.emplace(makePoolKey(pc.trackIdx, pc.file), pc.voice);

    // 破棄系編集で取り除かれた AudioClip を、この新スナップショットの graveyard に載せて延命する。
    // 旧スナップショット (これらを生参照する PlaybackClip を持つ) はこの公開で退役し、audio が
    // 手放してから回収される。graveyard はこの新スナップショットと共に (それより後に) 解放されるため、
    // 旧スナップショットが生きている間は確実に AudioClip が存在する (UAF 回避)。
    snap->graveyard = std::move(pendingGraveyard);
    pendingGraveyard.clear();

    // SR/blockSize 不変なら synth 実体を持ち回しボイス状態を保つ (prevSnap は冒頭で取得済み)。
    const bool srUnchanged = (snapshotPreparedSr == currentSampleRate
                              && snapshotPreparedBlock == currentBufferSize
                              && currentSampleRate > 0.0);

    // 各トラックのプラグインチェーンを準備（オーディオデバイスの SR/blockSize に合わせる）。
    // PluginChain 自身の chainLock が processBlock と相互排他するため、playbackLock 無しでも安全。
    // 既に同じ SR/blockSize で prepare 済みなら呼ばない (setMonitorChain と同じ「停止中 prepare」の
    // 作法)。無条件に呼ぶと、再生中編集の再構築のたびに全プラグインが releaseResources → prepareToPlay
    // のフル再初期化になり、(1) chainLock 保持下の重い prepare を audio thread の processBlock が
    // 待って全トラックの音が止まる、(2) 編集と無関係なプラグインの内部状態 (テール等) もリセットされる。
    // プラグイン追加 (addPlugin / insertPluginAt) は prepare 済みチェーンへは挿入時に個別 prepare する
    // ため、「チェーンが prepare 済み = 中の全プラグインも prepare 済み」は保たれる。
    if (currentSampleRate > 0.0 && currentBufferSize > 0)
    {
        // マスターチェーンは常に準備（トラック 0 個でも使う可能性がある）
        if (!masterChain->isPreparedFor(currentSampleRate, currentBufferSize))
            masterChain->prepareToPlay(currentSampleRate, currentBufferSize);

        // ── MIDI 再生キャッシュ構築 ──
        // クリップ 0 個の MIDI トラックも含める (events は空)。MIDI キーボードのライブ入力
        // (pushLiveMidi) が空のトラックでも synth / INS チェーン (VSTi) を鳴らせるようにするため。
        // 空トラックの再生コストは synth の idle 早期 return + チェーンの空チェックでほぼゼロ
        for (int ti = 0; ti < tm.getTrackCount(); ++ti)
        {
            auto* tr = tm.getTrack(ti);
            if (!tr || !tr->isMidiTrack()) continue;
            MidiPlayback mp;
            mp.trackIdx = ti;
            mp.track    = tr;
            for (int ci = 0; ci < tr->getMidiClipCount(); ++ci)
            {
                auto* clip = tr->getMidiClip(ci);
                if (!clip) continue;
                const double clipStart = clip->getStartPosition();
                const auto& seq = clip->getSequence();
                for (int i = 0; i < seq.getNumEvents(); ++i)
                {
                    auto msg = seq.getEventPointer(i)->message;
                    msg.setTimeStamp(msg.getTimeStamp() + clipStart);
                    mp.events.push_back(msg);
                }
            }
            std::sort(mp.events.begin(), mp.events.end(),
                      [](const juce::MidiMessage& a, const juce::MidiMessage& b)
                      { return a.getTimeStamp() < b.getTimeStamp(); });
            // 初期トランスポーズ値を記録（最初のブロックで差分検知させない）
            mp.lastTranspose = tr->getTotalTransposeSemitones();
            snap->midi.push_back(std::move(mp));
        }

        // ── 内蔵シンセ: index = trackIdx でトラック数ぶん確保し、各 MIDI トラックへ割り当て ──
        // SR/blockSize 不変なら旧スナップショットの実体を持ち回す (ボイス状態保持・prepareToPlay を
        // 呼ばない = 公開中の共有インスタンスを mutate しない)。新規 or SR 変化時のみ生成し、
        // まだ公開前 (非共有) の段階で prepareToPlay / setWaveform を済ませる。
        snap->synths.resize((size_t)tm.getTrackCount());
        for (auto& mp : snap->midi)
        {
            if (mp.trackIdx < 0 || mp.trackIdx >= (int)snap->synths.size()) continue;
            std::shared_ptr<InternalSynth> syn;
            if (srUnchanged && prevSnap
                && mp.trackIdx < (int)prevSnap->synths.size()
                && prevSnap->synths[(size_t)mp.trackIdx])
            {
                syn = prevSnap->synths[(size_t)mp.trackIdx];   // 持ち回し (UI からは触らない)
                // 持ち越したシンセは旧 transpose で鳴っているボイスを保持している。lastTranspose も
                // 引き継がないと、再生中の Octave/Semitone 変更が invalidatePlayback で再構築を
                // 起こした際 (Undo の SnapshotAction::perform 経由) に lastTranspose が現在値へ
                // リセットされ、audio thread の差分検知が空振りして旧ピッチの音が鳴り続ける。
                for (const auto& pm : prevSnap->midi)
                    if (pm.trackIdx == mp.trackIdx) { mp.lastTranspose = pm.lastTranspose; break; }
            }
            else
            {
                syn = std::make_shared<InternalSynth>();
                syn->prepareToPlay(currentSampleRate, currentBufferSize);  // 公開前 = 非共有で安全
                if (mp.track) syn->setWaveform(mp.track->getSynthWaveform());
            }
            snap->synths[(size_t)mp.trackIdx] = std::move(syn);
        }
        lastBlockPosStart = -1.0;

        const int nTracks = tm.getTrackCount();
        if (nTracks > 0)
        {
            int maxIdx = 0;
            for (int ti = 0; ti < nTracks; ++ti)
            {
                auto* tr = tm.getTrack(ti);
                if (!tr) continue;
                auto& chain = tr->getPluginChain();
                if (!chain.isPreparedFor(currentSampleRate, currentBufferSize))
                    chain.prepareToPlay(currentSampleRate, currentBufferSize);
                maxIdx = juce::jmax(maxIdx, ti);
            }

            // トラック単位のドライバッファ (index = trackIdx)
            snap->trackBuffers.resize((size_t)(maxIdx + 1));
            for (auto& tb : snap->trackBuffers)
                tb.setSize(2, currentBufferSize, false, false, true);

            // トラック単位のクリップ読み出しスクラッチ (renderClip 用)。trackBuffers と同数・同容量。
            // トラックごとに別インスタンスなので、マルチコア描画でワーカーが並列に renderClip を
            // 呼んでも競合しない (renderClip 内の setSize は容量内で再確保しない)。
            snap->clipScratch.resize((size_t)(maxIdx + 1));
            for (auto& cs : snap->clipScratch)
                cs.setSize(2, currentBufferSize, false, false, true);

            // ── フォルダバス (グループバス) の構築 ──
            // フォルダトラックごとに合算バッファを 1 本用意し、各トラックの所属を
            // trackIdx → (バス index / 親 Track*) の配列に解決しておく (audio thread はルックアップのみ)。
            snap->folderBusOfTrack.assign((size_t)(maxIdx + 1), -1);
            snap->folderOfTrack.assign((size_t)(maxIdx + 1), nullptr);
            for (int ti = 0; ti <= maxIdx; ++ti)
            {
                auto* tr = (ti < nTracks) ? tm.getTrack(ti) : nullptr;
                if (!tr || !tr->isFolderTrack()) continue;
                PlaybackSnapshot::FolderBus fb;
                fb.trackIdx = ti;
                fb.track    = tr;
                fb.buf.setSize(2, currentBufferSize, false, false, true);
                snap->folderBuses.push_back(std::move(fb));
            }
            for (int ti = 0; ti <= maxIdx; ++ti)
            {
                auto* tr = (ti < nTracks) ? tm.getTrack(ti) : nullptr;
                if (!tr) continue;
                auto* parent = tr->getFolderParent();
                if (parent == nullptr || tr->isFolderTrack()) continue;
                for (int bi = 0; bi < (int)snap->folderBuses.size(); ++bi)
                    if (snap->folderBuses[(size_t)bi].track == parent)
                    {
                        snap->folderBusOfTrack[(size_t)ti] = bi;
                        snap->folderOfTrack[(size_t)ti]    = parent;
                        break;
                    }
            }

            // ── PDC: 全トラックの最大プラグイン遅延を求めて各トラックの補正量を確定 ──
            // フォルダ配下の子は「自身のチェーン遅延 + 親フォルダのチェーン遅延」が合計経路遅延
            // (バスはチェーン後に遅延ラインを通らないため、子側の遅延ラインで前借りして揃える)。
            // フォルダトラック自身の遅延ラインは使わない (= 0)。
            int newMaxLat = 0;
            std::vector<int> trackLats((size_t)(maxIdx + 1), 0);
            for (int ti = 0; ti <= maxIdx; ++ti)
            {
                auto* tr = (ti < nTracks) ? tm.getTrack(ti) : nullptr;
                if (!tr) continue;
                trackLats[(size_t)ti] = tr->getPluginChain().getTotalLatencySamples();
            }
            std::vector<int> totalLats((size_t)(maxIdx + 1), 0);
            for (int ti = 0; ti <= maxIdx; ++ti)
            {
                auto* tr = (ti < nTracks) ? tm.getTrack(ti) : nullptr;
                if (!tr || tr->isFolderTrack()) continue;   // フォルダ自身は遅延ライン対象外
                int total = trackLats[(size_t)ti];
                if (auto* parent = (ti < (int)snap->folderOfTrack.size())
                                       ? snap->folderOfTrack[(size_t)ti] : nullptr)
                {
                    const int pi = tm.indexOf(parent);
                    if (pi >= 0 && pi <= maxIdx) total += trackLats[(size_t)pi];
                }
                totalLats[(size_t)ti] = total;
                newMaxLat = juce::jmax(newMaxLat, total);
            }
            maxPluginLatency = newMaxLat;

            snap->trackDelays.resize((size_t)(maxIdx + 1));
            // バッファサイズ: 最大遅延 + 1ブロック分の余裕、最低でも 1
            const int delayBufLen = juce::jmax(1, newMaxLat + currentBufferSize);
            for (int ti = 0; ti <= maxIdx; ++ti)
            {
                auto* tr = (ti < nTracks) ? tm.getTrack(ti) : nullptr;
                auto& d = snap->trackDelays[(size_t)ti];
                d.delaySamples = (tr != nullptr && tr->isFolderTrack())
                                     ? 0
                                     : newMaxLat - totalLats[(size_t)ti];
                d.buf.setSize(2, delayBufLen, false, true, true);
                d.writePos = 0;
            }
        }
        else
        {
            maxPluginLatency = 0;  // トラックが無い場合は PDC を無効化
        }

        snapshotPreparedSr    = currentSampleRate;
        snapshotPreparedBlock = currentBufferSize;
    }

    // ── 公開 (lock-free): 旧スナップショットは退役へ。preparePlayback の旧スナップショットは
    // 有効な (破棄されていない) AudioClip を参照しているため drain 不要。clearPlayback (破棄前に
    // 呼ばれる) のみ drain する。──
    publishSnapshot(std::move(snap));
}

// 新スナップショットを公開し、旧を退役リストへ。解放可能になった退役を回収する (UI thread)。
void AudioEngine::publishSnapshot(std::shared_ptr<PlaybackSnapshot> next)
{
    std::shared_ptr<PlaybackSnapshot> old;
    {
        const juce::SpinLock::ScopedLockType l(snapshotLock);
        old = std::move(activeSnapshot);
        activeSnapshot = std::move(next);
    }
    playbackGen.fetch_add(1, std::memory_order_relaxed);  // デクリック: 切替を audio thread に通知
    {
        const juce::ScopedLock r(reclaimLock);
        if (old) retiredSnapshots.push_back(std::move(old));
    }
    sweepRetiredSnapshots();
}

// use_count()==1 (= この retiredSnapshots だけが保持、audio/export は手放した) の退役を解放する。
// 解放は呼び出した非 audio スレッド上で起こる (audio thread が最後の所有者になることはない)。
// retiredSnapshots は message + 書き出し bg スレッドから触られうるため reclaimLock で直列化する。
void AudioEngine::sweepRetiredSnapshots()
{
    const juce::ScopedLock r(reclaimLock);
    retiredSnapshots.erase(
        std::remove_if(retiredSnapshots.begin(), retiredSnapshots.end(),
                       [](const std::shared_ptr<PlaybackSnapshot>& s) { return s.use_count() == 1; }),
        retiredSnapshots.end());
}

// audio thread (と export) が old スナップショットを手放すまで UI thread で待つ。
// activeSnapshot は既に差し替え済みなので、余分な参照は現ブロックを処理中のコピーのみ。
// 1 ブロック (数ミリ秒) で解消する。旧 playbackLock の「現ブロック完了待ち」を等価再現する
// UAF バリア (呼び出し側が直後に AudioClip/Track を破棄しても安全にする)。
void AudioEngine::drainOldSnapshot(const std::shared_ptr<PlaybackSnapshot>& old)
{
    if (old == nullptr) return;
    // 通常は 1 オーディオブロック (数ミリ秒) で解消する。500ms 待っても手放されない場合は
    // audio thread の停止等の異常なので、UI を巻き込んでハングしないよう打ち切る
    // (打ち切り時の旧スナップショットは retiredSnapshots が保持し続けるので即 UAF にはならない)。
    const auto deadline = juce::Time::getMillisecondCounterHiRes() + 500.0;
    while (old.use_count() > 1)
    {
        if (juce::Time::getMillisecondCounterHiRes() > deadline)
        {
            DBG("AudioEngine::drainOldSnapshot timed out (audio thread stalled?)");
            jassertfalse;
            break;
        }
        juce::Thread::yield();
    }
}

void AudioEngine::applyTrackDelay(std::vector<TrackDelay>& delays, int trackIdx,
                                  juce::AudioBuffer<float>& trackBuf, int numSamples)
{
    if (trackIdx < 0 || trackIdx >= (int)delays.size()) return;
    applyDelayLine(delays[(size_t)trackIdx], trackBuf, numSamples);
}

void AudioEngine::applyDelayLine(TrackDelay& d, juce::AudioBuffer<float>& buf, int numSamples)
{
    if (d.delaySamples == 0) return;            // 遅延不要 = 最遅トラック自身
    const int bufLen = d.buf.getNumSamples();
    if (bufLen <= 0) return;

    // 一度に扱える最大チャンク = bufLen - delaySamples。通常は preparePlayback が
    // 「最大遅延 + blockSize」で確保するので numSamples がそのまま収まるが、**再生中の
    // デバイス再起動 (バッファサイズ変更) 直後**は旧 blockSize で確保したままの遅延ラインに
    // 大きい numSamples が来ることがあり、旧実装 (一括コピー) はバッファ範囲外へ書いて
    // ヒープを破壊していた (Release は境界チェック無し・実クラッシュ id46 の原因)。
    // チャンク分割なら任意の numSamples を境界内で処理でき、遅延の意味も保たれる
    // (numSamples <= maxChunk の通常時は 1 周で旧実装と完全に同じ動作)。
    const int maxChunk = bufLen - d.delaySamples;
    if (maxChunk <= 0) return;                  // 遅延がバッファ長以上 (再構築待ちの不整合) = 素通り

    for (int done = 0; done < numSamples; )
    {
        const int n = juce::jmin(maxChunk, numSamples - done);

        // Step 1: buf → 循環バッファへ書き込み（ラップ分割）
        const int wp = d.writePos;
        int firstChunk  = juce::jmin(n, bufLen - wp);
        int secondChunk = n - firstChunk;
        for (int ch = 0; ch < juce::jmin(2, buf.getNumChannels()); ++ch)
        {
            d.buf.copyFrom(ch, wp, buf, ch, done, firstChunk);
            if (secondChunk > 0)
                d.buf.copyFrom(ch, 0,  buf, ch, done + firstChunk, secondChunk);
        }

        // Step 2: 循環バッファから delaySamples 遅れた位置を読み出して buf 上書き
        int rp = (wp - d.delaySamples + bufLen) % bufLen;
        firstChunk  = juce::jmin(n, bufLen - rp);
        secondChunk = n - firstChunk;
        for (int ch = 0; ch < juce::jmin(2, buf.getNumChannels()); ++ch)
        {
            buf.copyFrom(ch, done,              d.buf, ch, rp, firstChunk);
            if (secondChunk > 0)
                buf.copyFrom(ch, done + firstChunk, d.buf, ch, 0, secondChunk);
        }

        d.writePos = (wp + n) % bufLen;
        done += n;
    }
}

void AudioEngine::clearPlayback()
{
    // clips/midi を空にした「クリア」スナップショットを公開する。synths は引き継ぐ
    // (synths は AudioClip/Track を参照しないので UAF 安全。clearPlayback→preparePlayback の
    //  流れで MIDI ボイス状態を保つため)。
    std::shared_ptr<PlaybackSnapshot> old;
    { const juce::SpinLock::ScopedLockType l(snapshotLock); old = activeSnapshot; }

    auto cleared = std::make_shared<PlaybackSnapshot>();
    if (old) cleared->synths = old->synths;   // ボイス状態を保つ (clips/midi/buffers は空)

    { const juce::SpinLock::ScopedLockType l(snapshotLock); activeSnapshot = cleared; }
    playbackGen.fetch_add(1, std::memory_order_relaxed);  // デクリック: 切替を audio thread に通知

    // UAF バリア: この後に呼び出し側が AudioClip/Track を破棄しても、audio thread が旧スナップショット
    // (破棄予定の AudioClip*/Track* を参照する PlaybackClip 群) を読まないことを保証する。
    // 旧 playbackLock 取得が果たしていた「audio thread の現ブロック完了待ち」を等価再現する。
    drainOldSnapshot(old);
    {
        const juce::ScopedLock r(reclaimLock);
        if (old) retiredSnapshots.push_back(std::move(old));
    }
    sweepRetiredSnapshots();

    // モニター FX チェーンも空にして drain する。この後に呼び出し側が Track (= その PluginChain) を
    // 破棄しても、audio thread が旧 config 経由でチェーンを叩かないことを保証する (UAF バリア)。
    publishMonConfig(std::make_shared<MonitorConfig>(), /*drain=*/ true);

    // allNotesOff は audio thread が processBlock で Voice 集合を触っている最中に
    // UI thread から直接呼ぶと未定義動作になりうるため、stop()/rewind() と同じく
    // MIDI イベントキュー経由で audio thread にリクエストする (trackIdx<0 = 全 synth)。
    const juce::ScopedLock ml(previewMidiLock);
    pendingPreviewMidi.push_back({ /*trackIdx=*/ -1, /*note=*/ -1, 0.0f, false });
}

void AudioEngine::deferClipDestruction(std::vector<std::unique_ptr<AudioClip>>&& clips)
{
    // すぐに破棄せず保持する。次の preparePlayback で公開スナップショットの graveyard へ移し、
    // それを参照する旧スナップショットが回収される時に (message thread 上で) 解放される。
    for (auto& c : clips)
        if (c) pendingGraveyard.push_back(std::move(c));
}

void AudioEngine::invalidatePlayback()
{
    // 編集後フック。ここでは clearPlayback() を呼ばない (再生中編集での「一瞬の停止」回避)。
    // 理由: AudioClip を取り除く破棄系編集は editBeforeChangeCb (= deferClipDestruction) でクリップの
    // 所有権を遅延破棄へ渡し、参照中スナップショットが回収されるまで AudioClip を延命する (即破棄せず
    // UAF を防ぐ)。よってこの時点で活きているスナップショットは常に valid なクリップを参照しており、
    // 空にせず安全に新スナップショットへ置き換えられる。
    //  - 再生中: preparePlayback で新スナップショットを構築し直接公開する。旧→新へ atomic に
    //    切り替わるため空 (無音) 窓・drain が無く、他トラックも止まらず滑らかに遷移する。
    //  - 停止中: dirty を立て、次の play() の冒頭で再構築する。
    // 注: Track 自体を破棄するトラック削除は別途明示 clearPlayback() を使う (Track* のダングリング対策)。
    if (playing.load() && lastTrackManager != nullptr)
        preparePlayback(*lastTrackManager);
    else
        playbackDirty.store(true);
}

void AudioEngine::renderClip(PlaybackClip& pc, juce::AudioBuffer<float>& output,
                              juce::AudioBuffer<float>& scratch,
                              double posStart, int numSamples, bool preFader, bool allowStreaming)
{
    // パンチイン中: 録音先トラック (recArmed) の古いクリップだけをミュート。
    // 他のトラック (インストなど) はそのまま再生を続ける。
    double effectiveClipEnd = pc.clipEnd;
    if (isRecordingActive.load()
        && pc.sourceTrack != nullptr
        && pc.sourceTrack->isRecArmed())
    {
        double recStart = recordingStartSecs.load();
        if (effectiveClipEnd > recStart)
            effectiveClipEnd = recStart;
    }

    double posEnd = posStart + numSamples / currentSampleRate;
    if (pc.clipStart >= posEnd || effectiveClipEnd <= posStart) return;
    if (!pc.reader) return;

    // 出力バッファへの書き込み開始オフセット（クリップが途中から始まる場合）
    int bufOffset = (pc.clipStart > posStart)
                    ? (int)std::round((pc.clipStart - posStart) * currentSampleRate)
                    : 0;
    bufOffset = juce::jlimit(0, numSamples - 1, bufOffset);

    // ファイル内の読み取り開始位置
    double playPosInClip = juce::jmax(0.0, posStart - pc.clipStart);
    double filePosSecs   = pc.fileOffset + playPosInClip;
    juce::int64 fileSample = (juce::int64)(filePosSecs * pc.fileSampleRate);

    // 読み取るサンプル数（クリップ末端でクランプ）
    int nRead = numSamples - bufOffset;
    double clipRemain = effectiveClipEnd - juce::jmax(posStart, pc.clipStart);
    nRead = juce::jmin(nRead, (int)std::ceil(clipRemain * currentSampleRate));
    if (nRead <= 0) return;

    // 一時バッファに読み取り。チャンネル数は 2 にクランプする (read の useLeft/useRight
    // 経路は L/R しか使わない)。事前確保 (audioDeviceAboutToStart) は 2ch × バッファ長なので、
    // クランプしないと多チャンネル WAV で audio thread 上の再確保が起きる
    scratch.setSize(juce::jmin(2, (int)pc.reader->numChannels), nRead, false, false, true);
    scratch.clear();
    // ディスクストリーミング: ボイスがあれば先読みリングから (ヒット時 audio スレッドの I/O ゼロ)、
    // 無ければ / OFF なら従来どおり reader を直接同期読み。ボイスのミスもボイス内部で同期読みする
    // ので、いずれの経路でも結果はビット同一 (FileStreamVoiceTests で担保)。
    if (allowStreaming && pc.voice != nullptr && diskStreamingEnabled.load(std::memory_order_relaxed))
        pc.voice->read(scratch, 0, nRead, fileSample);
    else
        pc.reader->read(&scratch, 0, nRead, fileSample, true, true);

    // フェードイン／アウト適用。ブロックがフェード範囲と重なる時だけサンプルループを回す。
    // パンチインで effectiveClipEnd が recStart まで詰められた場合 (#M4): ユーザーの長い
    // フェードアウトを「縮んだ末尾」基準で当てると、パンチ点の手前で旧音が早すぎる
    // フェードになる。トリム時は短いデクリックのみを当て、旧音はフル音量でパンチ点まで
    // 再生してから止める。
    // 位置計算は double で行う (#L5)。float だと長尺クリップ (20-30分) の末尾付近で
    // ULP が invSR を上回り、フェードアウトのゲインランプが階段状になりジッパーノイズが出る。
    const bool   punchTrimmed = (effectiveClipEnd < pc.clipEnd - 1.0e-9);
    const double clipDuration = effectiveClipEnd - pc.clipStart;
    const FadeCurve inCurve    = pc.sourceClip ? pc.sourceClip->getFadeInCurve()  : FadeCurve::Linear;
    const FadeCurve outCurve   = pc.sourceClip ? pc.sourceClip->getFadeOutCurve() : FadeCurve::Linear;
    const double fadeInSecs    = (double)pc.fadeInSecs;
    const double fadeOutSecs   = punchTrimmed
                                 ? juce::jmin((double)pc.fadeOutSecs, 0.010)
                                 : (double)pc.fadeOutSecs;
    const double blockStartInClip = playPosInClip;
    const double blockEndInClip   = blockStartInClip + (double)nRead / currentSampleRate;
    const double fadeOutStartSec  = clipDuration - fadeOutSecs;
    const bool fadeInActive  = (fadeInSecs  > 0.0) && (blockStartInClip < fadeInSecs);
    const bool fadeOutActive = (fadeOutSecs > 0.0) && (blockEndInClip   > fadeOutStartSec);

    if (fadeInActive || fadeOutActive)
    {
        const int numCh   = scratch.getNumChannels();
        const double invSR = 1.0 / currentSampleRate;

        // チャンネル毎の生ポインタ（setSample/getSample の bounds check を回避）
        float* writePtrs[8] = {};
        const int chCount = juce::jmin(numCh, 8);
        for (int ch = 0; ch < chCount; ++ch)
            writePtrs[ch] = scratch.getWritePointer(ch);

        double posInClip = blockStartInClip;
        for (int i = 0; i < nRead; ++i)
        {
            float fadeGain = 1.0f;
            if (fadeInActive && posInClip < fadeInSecs)
                fadeGain *= AudioClip::applyFadeCurve((float)(posInClip / fadeInSecs), inCurve);
            if (fadeOutActive && posInClip > fadeOutStartSec)
                fadeGain *= AudioClip::applyFadeCurve((float)((clipDuration - posInClip) / fadeOutSecs), outCurve);
            fadeGain = juce::jlimit(0.0f, 1.0f, fadeGain);
            for (int ch = 0; ch < chCount; ++ch)
                writePtrs[ch][i] *= fadeGain;
            posInClip += invSR;
        }
    }

    // ゲイン適用（クリップゲイン・トラックVol/Panはライブ参照 → 再生中の調整も反映）
    float liveClipGain  = pc.sourceClip  ? pc.sourceClip->getGain()                        : 1.0f;
    // Pre-fader はトラック Vol/Pan を無視（クリップゲインのみ・センター固定）するため、
    // その場合は decibelsToGain (pow) や getPan の読み出しを行わない。
    float liveTrackGain;
    float liveTrackPan;
    if (preFader)
    {
        liveTrackGain = 1.0f;
        liveTrackPan  = 0.0f;
    }
    else
    {
        liveTrackGain = pc.sourceTrack ? juce::Decibels::decibelsToGain(pc.sourceTrack->getVolume()) : pc.trackGain;
        liveTrackPan  = pc.sourceTrack ? pc.sourceTrack->getPan()                                    : 0.0f;
    }
    float baseGain      = liveTrackGain * liveClipGain;

    // パン: -1=左、0=中央、+1=右（等価電力）
    float panL = (liveTrackPan <= 0.0f) ? 1.0f : (1.0f - liveTrackPan);
    float panR = (liveTrackPan >= 0.0f) ? 1.0f : (1.0f + liveTrackPan);

    // エンベロープがある場合: ブロックの開始/終端でのdBから線形ランプ
    if (pc.sourceClip && pc.sourceClip->hasGainEnvelope())
    {
        double clipPlayStartT = juce::jmax(0.0, posStart - pc.clipStart);
        double clipPlayEndT   = clipPlayStartT + (double)nRead / currentSampleRate;
        float envDBStart      = pc.sourceClip->getEnvelopeDBAt(clipPlayStartT);
        float envDBEnd        = pc.sourceClip->getEnvelopeDBAt(clipPlayEndT);
        float envGainStart    = juce::Decibels::decibelsToGain(envDBStart, -60.0f);
        float envGainEnd      = juce::Decibels::decibelsToGain(envDBEnd, -60.0f);

        // scratch 自身にゲインランプを適用してミックス
        scratch.applyGainRamp(0, nRead, baseGain * envGainStart, baseGain * envGainEnd);

        const int numOutCh = output.getNumChannels();
        for (int ch = 0; ch < numOutCh; ++ch)
        {
            int srcCh = juce::jmin(ch, scratch.getNumChannels() - 1);
            float chPan = (numOutCh >= 2) ? (ch == 0 ? panL : panR) : 1.0f;
            output.addFrom(ch, bufOffset, scratch, srcCh, 0, nRead, chPan);
        }
    }
    else
    {
        const int numOutCh = output.getNumChannels();
        for (int ch = 0; ch < numOutCh; ++ch)
        {
            int srcCh = juce::jmin(ch, scratch.getNumChannels() - 1);
            float chPan = (numOutCh >= 2) ? (ch == 0 ? panL : panR) : 1.0f;
            output.addFrom(ch, bufOffset, scratch, srcCh, 0, nRead, baseGain * chPan);
        }
    }
}

void AudioEngine::audioDeviceAboutToStart(juce::AudioIODevice* device)
{
    currentSampleRate = device->getCurrentSampleRate();
    currentBufferSize = device->getCurrentBufferSizeSamples();
    // SR / buffer size が変わったら VU 係数を必ず再計算させる
    vuCoefForSamples = -1;

    // 録音レイテンシ補正用: デバイス報告の入出力レイテンシ合計を公開する
    if (currentSampleRate > 0.0)
    {
        deviceRoundTripSecs.store((device->getInputLatencyInSamples()
                                   + device->getOutputLatencyInSamples())
                                  / currentSampleRate);
        // MIDI 録音のタイムスタンプ補正用: 出力レイテンシ単体も公開する
        // (演奏者は「聞こえた音」= エンジン位置より出力レイテンシ分過去に合わせて弾く)
        deviceOutputLatencySecs.store(device->getOutputLatencyInSamples() / currentSampleRate);
    }
    mixer.prepareToPlay(currentBufferSize, currentSampleRate);
    workBuffer.setSize(2, currentBufferSize);
    // 停止中シンセプレビュー用に十分なサイズで先に確保 (audio thread での realloc を回避)
    const int previewCh = juce::jmax(2, device->getActiveOutputChannels().countNumberOfSetBits());
    previewBuf.setSize(previewCh, currentBufferSize);
    // ライブ MIDI の drain 先も先に確保 (最初の和音/キュー溢れ時の audio thread realloc を回避)
    liveMidiScratch.ensureSize(4096);
    // クリップ読み出し用スクラッチは PlaybackSnapshot::clipScratch (トラック単位) に移行した
    // (マルチコア描画でワーカーが並列に renderClip を呼んでも競合しないように)。確保は
    // preparePlayback で行う。これにより mono→stereo 切替時の renderClip 内 setSize も容量内に収まる。

    // モニター返し用リバーブはデバイス開始時に準備する (再生 preparePlayback に依存せず、
    // 停止中の入力モニターだけでも返しにリバーブを掛けられるようにするため)。
    monitorReverbBus.setSampleRate(currentSampleRate);
    monitorReverbBus.setParameters(makePlateReverbParams());
    monitorReverbBuf.setSize(2, currentBufferSize, false, true, true);

    // モニター FX 経由用スクラッチ。audio thread でのヒープ確保を避けるため先に確保。
    // (モニタ対象トラックのチェーン自体の prepare は下のトラックチェーン再 prepare ループが担う)
    monitorChainBuf.setSize(2, currentBufferSize, false, true, true);

    // 再生用の簡易リバーブ送りバスもデバイス開始時に準備する (preparePlayback から移動)。
    // これにより preparePlayback は audio thread と共有する masterReverbBus/reverbSendBuf を
    // 触らずに済み、スナップショットを lock-free に公開できる。aboutToStart はコールバック
    // 再開前に呼ばれるため audio thread とは競合しない。
    masterReverbBus.setSampleRate(currentSampleRate);
    masterReverbBus.setParameters(makePlateReverbParams());
    reverbPreparedSr = currentSampleRate;
    reverbSendBuf.setSize(2, currentBufferSize, false, true, true);

    // メトロノーム合成 → CLICK トラック INS チェーン用スクラッチ (audio thread でのヒープ確保回避)。
    clickSynthBuf.setSize(2, currentBufferSize, false, true, true);

    // 配信ミラー出力: デバイス再起動 (SR 変更含む) でソース SR を更新し、reader に溜め直しを
    // 指示する。aboutToStart はコールバック再開前なので writer (audio thread) とは競合しない。
    {
        const juce::SpinLock::ScopedLockType l(mirrorRingLock);
        if (activeMirrorRing != nullptr)
            activeMirrorRing->reset(currentSampleRate);
    }

    // アプリ音声取り込み用スクラッチ (audio thread でのヒープ確保回避)。リング自体は触らない
    // (ソース SR はキャプチャ側のもので、エンジンのデバイス再起動では変わらない。reader の
    // SR 変換は毎ブロック dstRate = currentSampleRate を読むので勝手に追従する)。
    appCapScratchL.assign((size_t) currentBufferSize, 0.0f);
    appCapScratchR.assign((size_t) currentBufferSize, 0.0f);

    // audio callback のアクティブトラック収集スクラッチを事前確保 (毎ブロックの再確保回避)。
    // clear() で長さ 0 に戻しても容量は保たれるため、以後 push_back で再確保が起きない。
    activeTrackIdxScratch.reserve(64);
    activeTracksScratch.reserve(64);

    // オーディオのマルチコア用ワーカーを起動 (コールバック再開前なので audio thread と競合しない)。
    // 既定はコア数-2 (audio スレッド本体 + システム/UI に各 1 つ残す)。テストは固定数で上書きできる。
    {
        const int autoN = juce::jmax(0, juce::SystemStats::getNumCpus() - 2);
        workerPool.start(forcedWorkerCount >= 0 ? forcedWorkerCount : autoN);
    }

    // デバイス変更で SR / blockSize が変わった場合、全プラグインチェーンを新しい
    // 設定で prepareToPlay し直す。これをしないとプラグインが旧 SR/blockSize のまま
    // 動作し続け、特に blockSize が大きくなったときに想定外のサンプル数を受け取って
    // 落ちるプラグインがある。aboutToStart はコールバック再開前に呼ばれるため、
    // ここでチェーンを触っても audio thread とは競合しない。
    if (currentSampleRate > 0.0 && currentBufferSize > 0)
    {
        masterChain->prepareToPlay(currentSampleRate, currentBufferSize);
        if (lastTrackManager != nullptr)
            for (int ti = 0; ti < lastTrackManager->getTrackCount(); ++ti)
                if (auto* tr = lastTrackManager->getTrack(ti))
                    tr->getPluginChain().prepareToPlay(currentSampleRate, currentBufferSize);
        // 再生バッファ (reader / 内蔵シンセ / PDC) の再構築は次の play() で行わせる。
        playbackDirty.store(true);

        // **再生中**のデバイス再起動 (Audio Settings のバッファサイズ/SR 変更等) は次の play() を
        // 待たず今すぐ再構築する。スナップショットの trackBuffers/clipScratch/trackDelays は
        // preparePlayback 時の blockSize で確保されており、旧サイズのまま大きい numSamples を
        // 受けると PDC の遅延ライン (applyDelayLine) が範囲外書き込み = ヒープ破壊になっていた
        // (実クラッシュ id46。applyDelayLine 側にもチャンクガードあり)。aboutToStart は
        // コールバック再開前なので audio thread と競合しない。preparePlayback は message thread
        // 専用 (readerPool) のため、外部要因のデバイス再起動が背景スレッドで来た場合 (Mac の
        // デバイス構成変更等) は呼ばず dirty のまま残す (その間はチャンクガードが破壊を防ぎ、
        // 次の play() / 編集で再構築される)。
        if (playing.load() && lastTrackManager != nullptr
            && juce::MessageManager::getInstanceWithoutCreating() != nullptr
            && juce::MessageManager::getInstanceWithoutCreating()->isThisTheMessageThread())
            preparePlayback(*lastTrackManager);
    }
}

void AudioEngine::audioDeviceStopped()
{
    workerPool.stop();   // audio コールバックはもう来ない → ワーカーも止める
    mixer.releaseResources();
    workBuffer.setSize(0, 0);
    previewBuf.setSize(0, 0);
}

void AudioEngine::beginRealtimeCapture(int totalSamples)
{
    const juce::ScopedLock sl(captureLock);
    captureBuffer.setSize(2, juce::jmax(1, totalSamples), false, true, true);
    captureBuffer.clear();
    captureTotalSamples.store(totalSamples);
    captureWritePos.store(0);
    captureActive.store(true);
}

void AudioEngine::endRealtimeCapture()
{
    captureActive.store(false);
    const juce::ScopedLock sl(captureLock);
    captureWritePos.store(0);
    captureTotalSamples.store(0);
    captureBuffer.setSize(0, 0);
}

void AudioEngine::copyRealtimeCaptureTo(juce::AudioBuffer<float>& dst) const
{
    const juce::ScopedLock sl(captureLock);
    const int total = captureTotalSamples.load();
    dst.setSize(2, total, false, true, true);
    dst.clear();
    const int written = juce::jmin(captureWritePos.load(), total);
    if (written <= 0) return;
    for (int ch = 0; ch < 2 && ch < captureBuffer.getNumChannels(); ++ch)
        dst.copyFrom(ch, 0, captureBuffer, ch, 0, written);
}

void AudioEngine::fillPlayHead(EnginePlayHead& ph, double posSecs, double sr,
                               const AppSettings& cfg, bool isPlaying, bool isRecording,
                               bool looping, double loopStartSec, double loopEndSec)
{
    const double bpm = cfg.bpmAtTime(posSecs);
    const double ppq = cfg.beatsAtTime(posSecs);   // 1 拍 = 四分音符なので拍数 = ppq
    int bar1 = 1, beat1 = 1;
    cfg.barAndBeatAtTime(posSecs, bar1, beat1);
    // 小節頭の ppq = floor(ppq) (= 完了拍数) − 現在小節内の拍数 (beat1-1)
    const double ppqBarStart = std::floor(ppq) - (double)(beat1 - 1);
    int tsNum = 4, tsDen = 4;
    cfg.getMeterAtBar(bar1, tsNum, tsDen);

    const double loopStartPpq = looping ? cfg.beatsAtTime(loopStartSec) : 0.0;
    const double loopEndPpq   = looping ? cfg.beatsAtTime(loopEndSec)   : 0.0;
    const juce::int64 samples = (juce::int64) std::llround(posSecs * sr);

    ph.update(samples, posSecs, bpm, ppq, ppqBarStart,
              tsNum, tsDen, isPlaying, isRecording,
              looping, loopStartPpq, loopEndPpq);
}

// posStart より前にあった Program Change / Control Change / Pitch Bend /
// Channel Pressure の「最後の値」をブロック先頭 (sample 0) へ再送する。
// シーク直後の再生 (needsStateRefresh) と書き出しの開始ブロックで共用する。
// CC 全部 → PC → PB → CP の順 (PC で音色が変わってから CC が効く事故を避ける)。
// events はタイムスタンプ昇順前提 (MidiPlayback::events)。確保はスタックのみ (audio 安全)
static void appendMidiStateResend(juce::MidiBuffer& mb,
                                  const std::vector<juce::MidiMessage>& events,
                                  double posStart)
{
    // channel ごとに: 最後の PC、各 CC の最終値、最後の Pitch Bend、最後の Channel Pressure
    std::array<int, 16> lastPC;            lastPC.fill(-1);
    std::array<int, 16> lastPitchBend;    lastPitchBend.fill(-1);
    std::array<int, 16> lastChanPressure; lastChanPressure.fill(-1);
    // CC は (channel * 128 + ccNum) でキー化
    std::array<int, 16 * 128> lastCC;     lastCC.fill(-1);

    for (const auto& m : events)
    {
        if (m.getTimeStamp() >= posStart) break;
        const int ch = m.getChannel() - 1;
        if (ch < 0 || ch >= 16) continue;
        if (m.isProgramChange())
            lastPC[(size_t)ch] = m.getProgramChangeNumber();
        else if (m.isController())
            lastCC[(size_t)(ch * 128 + m.getControllerNumber())] = m.getControllerValue();
        else if (m.isPitchWheel())
            lastPitchBend[(size_t)ch] = m.getPitchWheelValue();
        else if (m.isChannelPressure())
            lastChanPressure[(size_t)ch] = m.getChannelPressureValue();
    }

    for (int ch = 0; ch < 16; ++ch)
        for (int cc = 0; cc < 128; ++cc)
            if (lastCC[(size_t)(ch * 128 + cc)] >= 0)
                mb.addEvent(juce::MidiMessage::controllerEvent(ch + 1, cc,
                                lastCC[(size_t)(ch * 128 + cc)]), 0);
    for (int ch = 0; ch < 16; ++ch)
        if (lastPC[(size_t)ch] >= 0)
            mb.addEvent(juce::MidiMessage::programChange(ch + 1, lastPC[(size_t)ch]), 0);
    for (int ch = 0; ch < 16; ++ch)
        if (lastPitchBend[(size_t)ch] >= 0)
            mb.addEvent(juce::MidiMessage::pitchWheel(ch + 1, lastPitchBend[(size_t)ch]), 0);
    for (int ch = 0; ch < 16; ++ch)
        if (lastChanPressure[(size_t)ch] >= 0)
            mb.addEvent(juce::MidiMessage::channelPressureChange(ch + 1,
                            lastChanPressure[(size_t)ch]), 0);
}

// 書き出しで「このトラックをミックスに含めるか」の共通規則 (明示選択なら選択集合、無ければ
// Mute/Solo)。PDC セットアップと per-block 収集の両方から呼び、規則の二重定義 (ドリフト) を防ぐ。
static bool exportTrackActive(int ti, const Track* trk, bool explicitFilter,
                              const std::unordered_set<int>& includeSet, bool anySolo,
                              const Track* folder = nullptr)
{
    if (trk == nullptr) return false;
    if (explicitFilter) return includeSet.find(ti) != includeSet.end();
    if (trk->isMuted() || (folder != nullptr && folder->isMuted())) return false;
    if (anySolo && !trk->isSoloed() && !(folder != nullptr && folder->isSoloed())) return false;
    return true;
}

void AudioEngine::renderOfflineRange(double startSec, double endSec,
                                      juce::AudioBuffer<float>& outBuffer,
                                      std::function<void(double)> progress,
                                      const std::vector<int>& includeTracks,
                                      bool preFader, bool includeClick)
{
    if (endSec <= startSec || currentSampleRate <= 0.0) return;

    const double sr        = currentSampleRate;
    const int    totalSamp = (int)std::round((endSec - startSec) * sr);
    if (totalSamp <= 0)   // 半サンプル未満の範囲 (round で 0) を弾く (progress の 0 除算防止)
    {
        // 呼び出し側 (ExportEngine::render) はデフォルト構築のバッファを渡してくるため、
        // 0 チャンネルのまま返すと writer の writeFromAudioSampleBuffer が jassert する。
        // 旧実装 (blockSize 1024 固定時代) と同じ「2ch / 0 サンプル = 空の有効 WAV」を保証する。
        outBuffer.setSize(2, 0, false, true, true);
        return;
    }

    // 書き出し中は、audio thread の停止時ブランチが同じ PluginChain を並行 processBlock して
    // 書き出し音声を壊さないよう、プレビュー処理をスキップさせる (RAII で確実に戻す)。
    offlineRenderActive.store(true);
    struct RenderFlagReset { std::atomic<bool>& f; ~RenderFlagReset() { f.store(false); } }
        renderFlagReset { offlineRenderActive };
    // 書き出しのブロックサイズはデバイス (再生時) と同じ currentBufferSize に「厳密に」合わせる。
    // プラグインは prepareToPlay 時に宣言した最大ブロック (setPlayConfigDetails) と異なるブロックで
    // 叩かれると内部で再バッファして「報告外の遅延」が乗ることがある (Ozone 等のルックアヘッド系で
    // 顕著)。旧実装は 1024 固定で、512 で prepare されたプラグインが僅かに遅れる原因になっていた。
    // isPreparedFor ガードが機能する (= 再 prepare を避ける) のは blockSize==currentBufferSize の
    // ときだけなので、実在するバッファサイズ (16〜) はそのまま使い、下限クランプで丸めない
    // (下限で丸めると小バッファ機で blockSize がずれ、モニター中でも再 prepare が走ってしまう)。
    // 上限だけ非現実的な巨大値へのガードとして残す。
    const int    blockSize = (currentBufferSize > 0) ? juce::jmin(currentBufferSize, 8192) : 1024;

    // 書き出し中もプラグインへ再生位置を供給する (テンポ同期プラグイン等が正しく動くように)。
    // audio thread と競合しないよう、メンバ playHead ではなくローカルインスタンスを使う。
    EnginePlayHead exportHead;
    std::shared_ptr<const AppSettings> exportCfg;
    { const juce::SpinLock::ScopedLockType l(appSettingsLock); exportCfg = activeAppSettings; }

    if (outBuffer.getNumSamples() < totalSamp || outBuffer.getNumChannels() < 2)
        outBuffer.setSize(2, totalSamp, false, true, true);
    outBuffer.clear();

    juce::AudioBuffer<float> blockBuf(2, blockSize);

    // MIDI トラック書き出し用のローカルシンセ (trackIdx → 実体・ブロックを跨いでボイス状態を
    // 保持)。snapshot の synths は audio thread (停止中のプレビュー MIDI) と共有のため、
    // 書き出しスレッドから触ると状態が壊れる。書き出し専用に別インスタンスを起こす
    std::unordered_map<int, std::unique_ptr<InternalSynth>> offlineSynths;
    juce::MidiBuffer offlineMidi;

    // ── リバーブ送りバス (Rev スライダー) の書き出し用ローカル実体 (2026-07 追加) ──
    // 実時間の masterReverbBus は audio thread 専用のため共有しない。パラメータは同一
    // (makePlateReverbParams) なので鳴りは再生時と一致する。送りは再生と同じ post-fader。
    // rs > 0 のトラックがある限り毎ブロック処理するので、クリップ終了後のテールも乗る。
    // Pre-Fader 書き出しでは送り自体を出さない (素のクリップ音のみ・addTrackOut 参照)
    juce::Reverb offlineReverbBus;
    offlineReverbBus.setSampleRate(sr);
    offlineReverbBus.setParameters(makePlateReverbParams());
    juce::AudioBuffer<float> sendBuf(2, blockSize);

    // ── メトロノーム (CLICK トラック) 合成の書き出し用ローカル状態 (2026-07 追加) ──
    // 実時間と同じ合成式・同じ INS チェーン経由・同じ「マスターチェーン/ゲインを通さない」
    // 加算。通常書き出し (明示選択なし) で CLICK トラックが非ミュートのときだけ混ざる
    // (stems / 明示選択では従来どおり除外)。エンベロープ等は engine メンバでなくローカル
    // (audio thread の実時間クリック状態と競合しないように)
    juce::AudioBuffer<float> clickBlock(2, blockSize);
    juce::Random clickRng;
    double clkEnv = 0.0, clkPhase = 0.0, clkFreq = 1000.0;
    bool   clkDown = false;
    int    clkLastBeat = 0;
    bool   clkInit = false;

    // ── プラグイン遅延補正 (PDC) のセットアップ ──
    // 重いプラグイン (リニアフェーズ EQ / ルックアヘッド系リミッター等) を挿したトラックは、
    // プラグインの遅延サンプル分だけ遅れて出力される。再生時は applyTrackDelay で全トラックを
    // 最遅トラックに揃えているが、書き出しは従来これを一切行わず、重いトラックだけが 2 ミックス
    // 上で遅れてずれていた。ここで再生と同じ相対補正 (各トラックを pdcMaxLat - 自身の遅延 分
    // 遅らせる) を行い、さらに全体の絶対遅延 (最大遅延 + マスターチェーン遅延 = totalSkip) を
    // 先頭に余分にレンダリングして読み飛ばすことで、タイムラインに正確に揃える (先頭の空白や
    // 末尾のテール切れも解消)。プラグイン遅延が全く無ければ totalSkip=0 で従来と完全に同一。
    //
    // 【精度の肝】各チェーンが書き出し blockSize (= currentBufferSize) で prepare 済みであること。
    // プラグインは宣言した最大ブロックと異なるブロックで叩かれると内部で再バッファして報告外の遅延が
    // 乗るため、処理と同じブロックサイズで prepare されていれば報告値と実挙動が一致する。通常は
    // preparePlayback / audioDeviceAboutToStart が currentBufferSize で prepare 済みなので下の
    // prepareAndLat は素通りする。書き出し中はモニタ返しも休止する (mixInputMonitoring の
    // offlineRenderActive ガード) が、フラグ設定前に飛んでいた in-flight callback がまだモニタ
    // チェーンを処理している可能性があるため、モニタチェーンだけは isPreparedFor ガード付きの
    // realtime prepare に留める (下の bouncePrepareChain の除外参照)。
    std::unordered_map<int,int> pdcTrackLatById;   // 絶対トラック index → プラグイン遅延サンプル
    int  pdcMaxLat = 0, pdcMaxTrackIdx = -1, clickChainLat = 0;
    bool clickWillPlay = false;
    std::shared_ptr<PlaybackSnapshot> snap0;
    { const juce::SpinLock::ScopedLockType l(snapshotLock); snap0 = activeSnapshot; }

    // ── オフライン書き出し (バウンス) のためのプラグイン再構成 ──
    // 書き出しは JUCE のオフラインレンダーなので、各プラグインを setNonRealtime(true) にして
    // 再 prepare する (オーバーサンプリング/ルックアヘッド系が realtime 前提の確保のまま叩かれて
    // クラッシュするのを防ぐ)。書き出し後は setNonRealtime(false) で realtime へ復帰させる。
    // **入力モニター中のチェーンは除外**する — モニタ返し自体は書き出し中休止する (mixInputMonitoring
    // の offlineRenderActive ガード) が、フラグ設定前に飛んでいた in-flight callback がまだこの
    // チェーンを処理している可能性があり、chainLock 保持下の releaseResources がそれをブロック
    // (優先度逆転) するため。書き出し中は再生停止 (offlineRenderActive でプレビューもスキップ) +
    // モニタ休止なので、チェーンは書き出しスレッドが唯一の使用者 = 安全に再 prepare できる。
    PluginChain* monChainNow = nullptr;
    { const juce::SpinLock::ScopedLockType l(monConfigLock);
      if (activeMonConfig) monChainNow = activeMonConfig->chain; }
    std::vector<PluginChain*> bouncedChains;   // setNonRealtime(true) にしたチェーン (復帰用)
    const int restoreBlock = (currentBufferSize > 0) ? currentBufferSize : blockSize;
    auto bouncePrepareChain = [&](PluginChain& chain)
    {
        if (chain.getNumPlugins() == 0) return;
        if (&chain == monChainNow)
        {
            // モニタ中: realtime のまま。二重 prepare (状態リセット) を避けるため isPreparedFor ガード
            if (sr > 0.0 && blockSize > 0 && !chain.isPreparedFor(sr, blockSize))
                chain.prepareToPlay(sr, blockSize);
            return;
        }
        chain.setOfflineRenderMode(true, sr, blockSize);
        bouncedChains.push_back(&chain);
    };

    // チェーンを書き出し blockSize で prepare し「直す」のは、まだそのサイズで prepare されて
    // いないときだけにする (isPreparedFor ガード)。通常は blockSize == currentBufferSize なので
    // preparePlayback 済み = 素通りし、latency だけ読む。無条件に prepareToPlay すると、
    // 入力モニター中 (audio thread が同じチェーンを processBlock 中) の書き出しで chainLock 保持
    // 下の releaseResources + 再確保がオーディオスレッドをブロックし、プラグイン状態もリセット
    // されてグリッチになる (setMonitorChain と同じ作法・CLAUDE.md「停止中 prepare (肝)」)。
    auto prepareAndLat = [&](Track* trk) -> int
    {
        auto& chain = trk->getPluginChain();
        if (chain.getNumPlugins() == 0) return 0;
        bouncePrepareChain(chain);   // setNonRealtime(true) + 再 prepare (モニタ中は除外)
        return chain.getTotalLatencySamples();
    };
    // ── 書き出し対象フィルタ (書き出し全体で凍結・唯一の実体) ──
    // 旧実装は per-block でも explicitFilter/anySolo/includeSet を再評価していた。(1) 毎ブロックの
    // unordered_set 構築 = ヒープ確保 (blockSize が currentBufferSize になり、小バッファ環境では
    // 数十万ブロックに達する)、(2) フィルタを live 評価すると書き出し中に mute/solo が変わった
    // 場合にここで凍結する PDC 遅延マップと対象集合がずれる (遅延ライン無しのトラックが
    // totalSkip 分早く焼かれる)、の 2 点から一度だけ計算し、PDC セットアップと per-block
    // ループ (トラック / MIDI / クリックの全セクション) がこれを参照する。
    const bool explicitFilter = !includeTracks.empty();
    const std::unordered_set<int> includeSet(includeTracks.begin(), includeTracks.end());
    bool anySolo = false;
    if (!explicitFilter)
    {
        for (auto& [ti, trk] : snap0->clipTracks)
            if (trk && trk->isSoloed()) { anySolo = true; break; }
        if (!anySolo)
            for (auto& mp : snap0->midi)
                if (mp.track && mp.track->isSoloed()) { anySolo = true; break; }
        if (!anySolo && snap0->clickTrack != nullptr && snap0->clickTrack->isSoloed())
            anySolo = true;
        // フォルダトラックのソロ (フォルダはクリップを持たず clipTracks に出ない)
        if (!anySolo)
            for (auto& fb : snap0->folderBuses)
                if (fb.track != nullptr && fb.track->isSoloed()) { anySolo = true; break; }
    }
    // 所属フォルダの解決 (スナップショットの trackIdx → 親フォルダ Track*)。
    // 実時間ブランチと同じ配列を使い、Mute/Solo 継承とバスルーティングを一致させる
    auto folderOfIn = [](const PlaybackSnapshot& s, int ti) -> Track*
    {
        return (ti >= 0 && ti < (int)s.folderOfTrack.size())
                   ? s.folderOfTrack[(size_t)ti] : nullptr;
    };
    // ── フォルダバスの書き出し用ローカル実体 (実時間の snap->folderBuses は audio thread 専用) ──
    struct OfflineFolderBus
    {
        Track* track { nullptr };
        juce::AudioBuffer<float> buf;
        bool   fed { false };
    };
    std::vector<OfflineFolderBus> offFolderBuses;
    std::unordered_map<Track*, int> offFolderBusIdx;   // フォルダ Track* → offFolderBuses index
    for (auto& fb : snap0->folderBuses)
    {
        if (fb.track == nullptr) continue;
        offFolderBusIdx[fb.track] = (int) offFolderBuses.size();
        offFolderBuses.push_back({ fb.track, juce::AudioBuffer<float>(2, blockSize), false });
    }
    // フォルダチェーンの bounce 準備 + 遅延取得 (フォルダ 1 つにつき 1 回)
    std::unordered_map<Track*, int> folderChainLats;
    auto folderLatFor = [&](Track* folder) -> int
    {
        if (folder == nullptr) return 0;
        auto it = folderChainLats.find(folder);
        if (it != folderChainLats.end()) return it->second;
        const int l = prepareAndLat(folder);
        folderChainLats[folder] = l;
        return l;
    };
    {
        auto noteLat = [&](int ti, Track* trk, Track* folder)
        {
            if (!exportTrackActive(ti, trk, explicitFilter, includeSet, anySolo, folder)) return;
            // Pre-Fader は素のクリップ音のみ (プラグインを掛けない) なので遅延補正も不要 = 0。
            // フォルダ配下の子は「自身のチェーン + 親フォルダのチェーン」が合計経路遅延
            // (再生の preparePlayback と同じ式)。
            const int lat = preFader ? 0 : (prepareAndLat(trk) + folderLatFor(folder));
            pdcTrackLatById[ti] = lat;
            pdcMaxLat      = juce::jmax(pdcMaxLat, lat);
            pdcMaxTrackIdx = juce::jmax(pdcMaxTrackIdx, ti);
        };
        for (auto& [ti, trk] : snap0->clipTracks) noteLat(ti, trk, folderOfIn(*snap0, ti));
        for (auto& mp : snap0->midi)
            noteLat(mp.trackIdx, mp.track, folderOfIn(*snap0, mp.trackIdx));

        // クリック (メトロノーム) トラックのチェーン遅延も最大遅延に含める。クリックはトラックと
        // 同じ INS チェーンを通ってからマスター後に加算されるため、これを入れないとクリックに
        // 重いプラグインを挿したときクリックだけがずれる。
        Track* clickTr0 = snap0->clickTrack;
        const bool clickAuto0 = !explicitFilter && clickTr0 != nullptr && !clickTr0->isMuted()
                                && !(anySolo && !clickTr0->isSoloed());
        clickWillPlay = (includeClick && clickTr0 != nullptr) || clickAuto0;
        if (clickWillPlay && clickTr0 != nullptr)
        {
            clickChainLat = prepareAndLat(clickTr0);
            pdcMaxLat = juce::jmax(pdcMaxLat, clickChainLat);
        }
    }

    // マスターチェーン遅延 (Pre-Fader はマスターを通さないので 0)。全体の絶対遅延に含める。
    int masterLat = 0;
    if (!preFader && masterChain && masterChain->getNumPlugins() > 0)
    {
        bouncePrepareChain(*masterChain);   // オフライン再構成 (マスターはモニタ対象外)
        masterLat = masterChain->getTotalLatencySamples();
    }
    const int totalSkip = pdcMaxLat + masterLat;

    // 書き出し完了 (早期 return / 例外含む) で必ず realtime へ復帰させる。
    // setNonRealtime(false) + 再 prepare を realtime のブロックサイズ (currentBufferSize) で。
    // 復帰の SR/blockSize は開始時スナップショットではなく**破棄時点の現在値**を読む — 書き出し中に
    // デバイス再起動 (デバイス切替 / SR 変更) が起きると audioDeviceAboutToStart が新 SR で
    // 再 prepare するため、開始時値で上書きするとチェーン (と prepared フラグ) が誤った SR の
    // まま次の preparePlayback まで残る。停止中 (現在値 0) は開始時値へフォールバック。
    struct BounceRestore
    {
        AudioEngine& eng; std::vector<PluginChain*>& chains; double fbSr; int fbBs;
        ~BounceRestore()
        {
            const double s = eng.currentSampleRate > 0.0 ? eng.currentSampleRate : fbSr;
            const int    b = eng.currentBufferSize  > 0  ? eng.currentBufferSize  : fbBs;
            for (auto* c : chains) if (c) c->setOfflineRenderMode(false, s, b);
        }
    } bounceRestore { *this, bouncedChains, sr, restoreBlock };

    // 各トラックの遅延ライン (index = 絶対トラック index)。delaySamples = pdcMaxLat - 自身の遅延。
    std::vector<TrackDelay> trackDelays;
    if (pdcMaxLat > 0 && pdcMaxTrackIdx >= 0)
    {
        trackDelays.resize((size_t)(pdcMaxTrackIdx + 1));
        const int dlen = juce::jmax(1, pdcMaxLat + blockSize);
        for (auto& [ti, lat] : pdcTrackLatById)
        {
            auto& d = trackDelays[(size_t)ti];
            d.delaySamples = pdcMaxLat - lat;
            if (d.delaySamples > 0) { d.buf.setSize(2, dlen, false, true, true); d.writePos = 0; }
        }
    }
    // クリックの遅延ライン: クリックは自身の INS チェーンで clickChainLat 遅れて出るため、
    // マスター後のトラック (totalSkip) と揃えるには残り (totalSkip - clickChainLat) だけ足す。
    // pdcMaxLat >= clickChainLat なので cd >= masterLat >= 0。
    TrackDelay clickDelay;
    {
        const int cd = totalSkip - clickChainLat;
        if (cd > 0)
        {
            clickDelay.delaySamples = cd;
            clickDelay.buf.setSize(2, cd + blockSize, false, true, true);
        }
    }
    const int renderSamp = totalSamp + totalSkip;   // 絶対遅延分を余分にレンダリングして前詰めで読み飛ばす

    // per-block で使い回す作業領域 (ループ外確保・毎ブロックのヒープ確保を避ける)。
    // 中身は per-block snap から再構築するが、容量は保持される。
    std::vector<int>    activeIdx;
    std::vector<Track*> activeTracks;
    juce::AudioBuffer<float> trackBuf(2, blockSize);
    juce::AudioBuffer<float> offlineScratch(2, blockSize);   // 書き出しは単一スレッド = ローカル 1 本で十分

    int feedPos = 0;
    while (feedPos < renderSamp)
    {
        const int n = juce::jmin(blockSize, renderSamp - feedPos);
        blockBuf.setSize(2, n, false, false, true);
        blockBuf.clear();

        const double posStart = startSec + (double)feedPos / sr;
        if (exportCfg) fillPlayHead(exportHead, posStart, sr, *exportCfg,
                                    /*playing*/ true, /*recording*/ false,
                                    /*looping*/ false, 0.0, 0.0);

        // メトロノームの合成結果 (スコープ内で生成し、マスター処理後に加算する)
        bool  clickActive = false, clickChainStereo = false;
        float clickGL = 0.0f, clickGR = 0.0f;

        {
            // 再生スナップショットを per-block で取得する (旧 playbackLock 相当)。per-block にする
            // ことで clearPlayback() の drain がこのレンダリングを 1 ブロック分だけ待てば解放される。
            std::shared_ptr<PlaybackSnapshot> snap;
            { const juce::SpinLock::ScopedLockType l(snapshotLock); snap = activeSnapshot; }

            // アクティブトラック収集 (clipTracks ベースで O(トラック数)。全 clips 走査しない)。
            // フィルタ (explicitFilter / includeSet / anySolo) はセットアップで凍結済みの
            // 唯一の実体を参照する = PDC 遅延マップと構造的に一致 (書き出し中の mute/solo
            // 変更で対象集合だけがずれない)。snap は per-block なので集合の再構築のみ行う。
            activeIdx.clear();
            activeTracks.clear();
            for (auto& [ti, trk] : snap->clipTracks)
            {
                if (!exportTrackActive(ti, trk, explicitFilter, includeSet, anySolo,
                                       folderOfIn(*snap, ti))) continue;
                activeIdx.push_back(ti);
                activeTracks.push_back(trk);
            }

            trackBuf.setSize(2, n, false, false, true);
            offlineScratch.setSize(2, n, false, false, true);
            bool sendActive = false;                          // このブロックにリバーブ送りがあるか

            // トラックのドライを blockBuf へ、リバーブ送り (Rev スライダー) を sendBuf へ加算。
            // Post-Fader: gL/gR = vol/pan、送りは gL*rs (再生と同じ post-fader 送り)。
            // Pre-Fader: クリップの素の音のみ (Vol/Pan/Rev/master を一切通さない)。
            //   リバーブ送りは再生では post-fader なので、Pre では送り自体を出さない
            //   (Pre を選ぶ = 加工前の素材が欲しい、なので Rev も混ぜない・要望 2026-07)
            auto addTrackOut = [&](int tidx, Track* track, const juce::AudioBuffer<float>& buf)
            {
                if (preFader)
                {
                    // 素のクリップ音のみ (Vol/Pan/Rev なし。フォルダバス/チェーンも通さない)
                    blockBuf.addFrom(0, 0, buf, 0, 0, n, 1.0f);
                    blockBuf.addFrom(1, 0, buf, 1, 0, n, 1.0f);
                    return;
                }

                const float vol  = track ? juce::Decibels::decibelsToGain(track->getVolume()) : 1.0f;
                const float pan  = track ? track->getPan() : 0.0f;
                const float panL = (pan <= 0.0f) ? 1.0f : (1.0f - pan);
                const float panR = (pan >= 0.0f) ? 1.0f : (1.0f + pan);
                const float gL = vol * panL;
                const float gR = vol * panR;

                // フォルダ配下の子はフォルダバスへ、それ以外はドライミックスへ (再生と同じ)
                juce::AudioBuffer<float>* dest = &blockBuf;
                if (auto* fld = folderOfIn(*snap, tidx))
                {
                    auto it = offFolderBusIdx.find(fld);
                    if (it != offFolderBusIdx.end())
                    {
                        auto& ob = offFolderBuses[(size_t)it->second];
                        if (!ob.fed)
                        {
                            ob.buf.setSize(2, n, false, false, true);
                            ob.buf.clear();
                            ob.fed = true;
                        }
                        dest = &ob.buf;
                    }
                }
                dest->addFrom(0, 0, buf, 0, 0, n, gL);
                dest->addFrom(1, 0, buf, 1, 0, n, gR);

                const float rs = track ? Track::reverbSendGain(track->getReverbSend()) : 0.0f;
                if (rs > 0.0001f)
                {
                    if (!sendActive)
                    {
                        sendBuf.setSize(2, n, false, false, true);
                        sendBuf.clear();
                        sendActive = true;
                    }
                    sendBuf.addFrom(0, 0, buf, 0, 0, n, gL * rs);
                    sendBuf.addFrom(1, 0, buf, 1, 0, n, gR * rs);
                }
            };

            for (size_t ai = 0; ai < activeIdx.size(); ++ai)
            {
                trackBuf.clear();
                const int tidx = activeIdx[ai];
                if (tidx < (int)snap->clipsByTrack.size())
                    for (int ci : snap->clipsByTrack[(size_t)tidx])
                        renderClip(snap->clips[(size_t)ci], trackBuf, offlineScratch, posStart, n,
                                   /*preFader*/ true, /*allowStreaming*/ false);

                // Pre-Fader は「波形のみ」= プラグイン (Melodyne 等) を通さない (Vol/Pan/Rev/master も
                // 通さないのは addTrackOut 側)。Post-Fader のときだけチェーンを通す。
                auto* track = activeTracks[ai];
                if (!preFader && track && track->getPluginChain().getNumPlugins() > 0)
                {
                    juce::MidiBuffer midi;
                    track->getPluginChain().processBlock(trackBuf, midi, &exportHead);
                }

                // PDC: 自分より遅いトラックに合わせて遅延させミックスに揃える (再生時と同じ)。
                // Pre-Fader は上でプラグインを通さない = 遅延補正も無効 (noteLat で lat=0)。
                applyTrackDelay(trackDelays, tidx, trackBuf, n);

                addTrackOut(tidx, track, trackBuf);
            }

            // ── MIDI トラック (内蔵シンセ / INS 音源) のレンダリング ──
            // 従来はここが無く、MIDI トラックの書き出しが常に無音になっていた (2026-07 修正)。
            // イベント収集・移調・シンセ/チェーンの流れはリアルタイム再生ブランチと同じ。
            // MIDI トラックにも PDC (applyTrackDelay) をかける (下記)。
            const double posEndMidi = posStart + (double)n / sr;
            for (auto& mp : snap->midi)
            {
                if (!exportTrackActive(mp.trackIdx, mp.track, explicitFilter, includeSet, anySolo,
                                       folderOfIn(*snap, mp.trackIdx)))
                    continue;
                // クリップ 0 個で synth もチェーンも無い MIDI トラックは何も出さない
                // (空 MIDI トラックを snap に含めるようにした分のコスト節約・出力不変)
                if (mp.events.empty() && !mp.track->isSynthEnabled()
                    && mp.track->getPluginChain().getActivePluginCountAtomic() == 0)
                    continue;

                auto& syn = offlineSynths[mp.trackIdx];
                if (!syn)
                {
                    syn = std::make_unique<InternalSynth>();
                    syn->prepareToPlay(sr, blockSize);
                }
                syn->setWaveform(mp.track->getSynthWaveform());
                const int transpose = mp.track->getTotalTransposeSemitones();

                offlineMidi.clear();
                // 途中からの書き出しでも音色/ベンドが合うよう、開始ブロックで
                // startSec より前の PC/CC/PB/CP の最終値を再送する (再生のシーク時と同じ)
                if (feedPos == 0)
                    appendMidiStateResend(offlineMidi, mp.events, posStart);

                auto evIt = std::lower_bound(mp.events.begin(), mp.events.end(), posStart,
                    [](const juce::MidiMessage& m, double v) { return m.getTimeStamp() < v; });
                for (; evIt != mp.events.end(); ++evIt)
                {
                    const double t = evIt->getTimeStamp();
                    if (t >= posEndMidi) break;
                    juce::MidiMessage out = *evIt;
                    if (transpose != 0 && out.isNoteOnOrOff())
                        out.setNoteNumber(juce::jlimit(0, 127, out.getNoteNumber() + transpose));
                    offlineMidi.addEvent(out, juce::jlimit(0, n - 1,
                                                           (int)((t - posStart) * sr)));
                }

                trackBuf.clear();
                // 内蔵シンセ ON なら音を書き、OFF なら MIDI を INS チェーン (VST 音源等) だけに渡す
                if (mp.track->isSynthEnabled())
                    syn->processBlock(trackBuf, offlineMidi);
                if (mp.track->getPluginChain().getNumPlugins() > 0)
                    mp.track->getPluginChain().processBlock(trackBuf, offlineMidi, &exportHead);

                // PDC: 遅延の無い (または少ない) MIDI トラックも最遅トラックに合わせて遅延させる。
                applyTrackDelay(trackDelays, mp.trackIdx, trackBuf, n);

                addTrackOut(mp.trackIdx, mp.track, trackBuf);
            }

            // ── フォルダバス: 子の合算に INS チェーン → フォルダ Vol を掛けてドライへ加算 ──
            // 実時間ブランチと同じ順序 (トラック/MIDI 加算後・リバーブ送りウェットの前)。
            // Pre-Fader では子が直接 blockBuf へ入る (fed が立たない) ため no-op。
            for (auto& ob : offFolderBuses)
            {
                if (!ob.fed) continue;
                ob.fed = false;
                if (ob.track == nullptr) continue;
                if (ob.track->getPluginChain().getNumPlugins() > 0)
                {
                    juce::MidiBuffer midi;
                    ob.track->getPluginChain().processBlock(ob.buf, midi, &exportHead);
                }
                // フォルダの Vol / Pan / リバーブ送り (実時間ブランチと同じ式・同じ順序)
                const float fVol  = juce::Decibels::decibelsToGain(ob.track->getVolume());
                const float fPan  = ob.track->getPan();
                const float fPanL = (fPan <= 0.0f) ? 1.0f : (1.0f - fPan);
                const float fPanR = (fPan >= 0.0f) ? 1.0f : (1.0f + fPan);
                const float fGL = fVol * fPanL;
                const float fGR = fVol * fPanR;
                blockBuf.addFrom(0, 0, ob.buf, 0, 0, n, fGL);
                blockBuf.addFrom(1, 0, ob.buf, 1, 0, n, fGR);

                const float frs = Track::reverbSendGain(ob.track->getReverbSend());
                if (frs > 0.0001f)
                {
                    if (!sendActive)
                    {
                        sendBuf.setSize(2, n, false, false, true);
                        sendBuf.clear();
                        sendActive = true;
                    }
                    sendBuf.addFrom(0, 0, ob.buf, 0, 0, n, fGL * frs);
                    sendBuf.addFrom(1, 0, ob.buf, 1, 0, n, fGR * frs);
                }
            }

            // ── リバーブ送りバス: ウェットを生成してドライへ加算 (2026-07 追加) ──
            // 実時間と同じくマスターチェーンの前段。rs > 0 のトラックがある限り毎ブロック
            // 処理されるので、クリップ終了後のテールも書き出しに乗る。
            // 旧実装はここが無く、Rev スライダーを上げても書き出しには乗らなかった
            if (sendActive)
            {
                offlineReverbBus.processStereo(sendBuf.getWritePointer(0),
                                               sendBuf.getWritePointer(1), n);
                blockBuf.addFrom(0, 0, sendBuf, 0, 0, n, 1.0f);
                blockBuf.addFrom(1, 0, sendBuf, 1, 0, n, 1.0f);
            }

            // ── メトロノーム (CLICK トラック) の合成 (2026-07 追加) ──
            // 混ぜる条件は 2 系統:
            //  - includeClick=true (2 ミックス書き出し。ダイアログ経路は常に明示トラック
            //    リストを渡すため、呼び出し側が「鳴っている状態か」を判定してこのフラグで指示)
            //  - 明示選択なしの通常ミックスダウンでは非ミュート + Solo 規則で自動判定
            // stems (明示選択・フラグ無し) では従来どおり除外。
            // 合成式・INS チェーン経由・vol/pan 後段は実時間 (audioDeviceIOCallback) と同一。
            // 混ぜるか自体はセットアップで凍結済み (clickWillPlay) — per-block でミュート/ソロを
            // 再評価すると、クリック遅延ライン (clickDelay) の有無や PDC 算入と食い違うため。
            // clickTr は per-block snap から取る (null になり得るのでガード)
            Track* clickTr = snap->clickTrack;
            if (clickWillPlay && clickTr != nullptr)
            {
                const double bpmHere = (exportCfg && !exportCfg->bpmChanges.empty())
                                       ? exportCfg->bpmAtTime(posStart)
                                       : metronomeBpm.load();
                const double bps2 = bpmHere / 60.0;
                const int    beatsPerBar = juce::jmax(1, metronomeBeatsPerBar.load());
                const int    sound   = metronomeSound.load();
                const bool   accent  = metronomeAccent.load();
                const double rateMul = juce::jmax(0.01f, metronomeRateMul.load());
                const double beatsAtBlockStart = (exportCfg && !exportCfg->bpmChanges.empty())
                                                 ? exportCfg->beatsAtTime(posStart)
                                                 : posStart * bps2;
                if (!clkInit)
                {
                    // 開始位置ちょうどが拍境界なら、その拍から鳴らす (floor だけだと最初の
                    // 拍が「越えた」判定にならず 1 拍目が欠ける)
                    const double b0 = beatsAtBlockStart * rateMul;
                    clkLastBeat = (int)std::floor(b0);
                    if (std::abs(b0 - std::round(b0)) < 1e-9) clkLastBeat -= 1;
                    clkInit = true;
                }

                clickBlock.setSize(2, n, false, false, true);
                clickBlock.clear();
                float* cs = clickBlock.getWritePointer(0);
                for (int i = 0; i < n; ++i)
                {
                    const double localBeats = beatsAtBlockStart + bps2 * (double)i / sr;
                    const int beatInt = (int)std::floor(localBeats * rateMul);
                    if (beatInt > clkLastBeat)
                    {
                        clkEnv   = 1.0;
                        clkPhase = 0.0;
                        const double realBeatF = (double)beatInt / rateMul;
                        const int    realBeatI = (int)std::round(realBeatF);
                        const bool   onRealBeat = std::abs(realBeatF - (double)realBeatI) < 0.01;
                        clkDown = onRealBeat &&
                                  (!exportCfg || exportCfg->meterChanges.empty()
                                   ? (realBeatI % beatsPerBar == 0)
                                   : exportCfg->isDownbeatAtBeat(realBeatI));
                        const bool downHi = accent && clkDown;
                        switch (sound)
                        {
                            case 0: clkFreq = downHi ? 1500.0 : 1000.0; break;  // Beep
                            case 1: clkFreq = downHi ? 2000.0 : 1500.0; break;  // Stick
                            case 2: clkFreq = downHi ? 800.0  : 600.0;  break;  // Cowbell
                            case 3: clkFreq = downHi ? 600.0  : 400.0;  break;  // Wood
                            case 4: clkFreq = downHi ? 2200.0 : 1700.0; break;  // Tick
                            case 5: clkFreq = downHi ? 1200.0 : 900.0;  break;  // Bell
                            default: clkFreq = 1000.0;
                        }
                        clkLastBeat = beatInt;
                    }
                    if (clkEnv > 0.001)
                    {
                        float s = 0.0f;
                        clkPhase += 2.0 * juce::MathConstants<double>::pi * clkFreq / sr;
                        switch (sound)
                        {
                            case 0: s = (float)(std::sin(clkPhase) * clkEnv); break;
                            case 1: s = (clickRng.nextFloat() * 2.0f - 1.0f) * (float)clkEnv * 0.7f; break;
                            case 2: s = (std::sin(clkPhase) > 0 ? 1.0f : -1.0f) * (float)clkEnv * 0.5f; break;
                            case 3: s = (float)((std::asin(std::sin(clkPhase))
                                                 / juce::MathConstants<double>::halfPi) * clkEnv) * 0.5f
                                        + (clickRng.nextFloat() * 2.0f - 1.0f) * (float)clkEnv * 0.2f; break;
                            case 4: s = (float)(std::sin(clkPhase) * clkEnv * clkEnv); break;
                            case 5:
                            {
                                const double s1 = std::sin(clkPhase);
                                const double s2 = std::sin(clkPhase * 2.756);
                                s = (float)((s1 + s2 * 0.5) * clkEnv * 0.6);
                                break;
                            }
                            default: s = (float)(std::sin(clkPhase) * clkEnv);
                        }
                        s *= accent ? (clkDown ? 1.3f : 0.85f) : 1.0f;
                        cs[i] = s;
                        const double decay = (sound == 1 || sound == 4) ? 0.997 : 0.9985;
                        clkEnv *= decay;
                        if (clkEnv < 0.001) clkEnv = 0.0;
                    }
                }

                // CLICK トラックの INS チェーン (EQ 等) を通す
                if (clickTr->getPluginChain().getNumPlugins() > 0)
                {
                    clickBlock.copyFrom(1, 0, clickBlock, 0, 0, n);   // dual-mono 化
                    juce::MidiBuffer midi;
                    clickTr->getPluginChain().processBlock(clickBlock, midi, &exportHead);
                    clickChainStereo = true;
                }

                // PDC: クリックはマスター通過後に加算されるため、全体の絶対遅延 totalSkip 分
                // 遅延させてトラック (マスター後) と時間を揃える。
                applyDelayLine(clickDelay, clickBlock, n);

                // vol/pan はチェーン後段 (実時間と同じ)。クリック基本音量 = トラックゲイン × 0.5。
                // Pre-Fader ではフェーダー/パンを掛けない (他トラックと同じ扱い)
                if (preFader)
                {
                    clickGL = clickGR = 0.5f;
                }
                else
                {
                    const float vol  = juce::Decibels::decibelsToGain(clickTr->getVolume()) * 0.5f;
                    const float pan  = clickTr->getPan();
                    clickGL = vol * ((pan <= 0.0f) ? 1.0f : (1.0f - pan));
                    clickGR = vol * ((pan >= 0.0f) ? 1.0f : (1.0f + pan));
                }
                clickActive = true;
            }
        }

        // Pre-fader はマスターインサート/ゲインを適用しない（純粋なクリップ音）
        if (!preFader)
        {
            if (masterChain && masterChain->getNumPlugins() > 0)
            {
                juce::MidiBuffer midi;
                masterChain->processBlock(blockBuf, midi, &exportHead);
            }
            blockBuf.applyGain(masterGain.load());
        }

        // メトロノームは実時間と同じくマスターチェーン/ゲインを通さずここで加算する
        if (clickActive)
        {
            blockBuf.addFrom(0, 0, clickBlock, 0, 0, n, clickGL);
            blockBuf.addFrom(1, 0, clickBlock, clickChainStereo ? 1 : 0, 0, n, clickGR);
        }

        // ── 出力へコピー (PDC の絶対遅延 totalSkip 分を先頭で読み飛ばす) ──
        // このブロックの feed サンプル [feedPos, feedPos+n) は export サンプル
        // [feedPos-totalSkip, ...) に対応する。負域 (先頭のプラグイン鳴り込み) は捨てる。
        int srcOff = 0, dst = feedPos - totalSkip, cnt = n;
        if (dst < 0) { srcOff = -dst; dst = 0; cnt = n - srcOff; }
        if (cnt > 0 && dst < totalSamp)
        {
            cnt = juce::jmin(cnt, totalSamp - dst);
            for (int ch = 0; ch < 2; ++ch)
                outBuffer.copyFrom(ch, dst, blockBuf, ch, srcOff, cnt);
        }

        feedPos += n;
        // 進捗は feed 位置 / 全レンダリング長 (= totalSamp + totalSkip) で報告する。前詰めの
        // プリロール中も滑らかに進む (旧: exportWritten/totalSamp は先頭 totalSkip 分 0 に張り付いた)。
        if (progress) progress((double)feedPos / (double)renderSamp);
    }
}

void AudioEngine::mixInputMonitoring(const float* const* inputChannelData, int numInputChannels,
                                     float* const* outputChannelData, int numOutputChannels,
                                     int numSamples, PluginChain* monChain,
                                     int monInputCh, bool monStereo, float monPan, float monGain)
{
    // 書き出し (renderOfflineRange) 中はモニタ返しを休止する (案 a)。書き出しスレッドの
    // per-block ループと audio thread がモニタ対象チェーンの同一プラグインインスタンスを
    // 交互に processBlock すると、内部状態 (ディレイライン/包絡) にマイク音声とクリップ音声が
    // 混ざって書き出しファイルが破損し、モニタ返しもグリッチする (chainLock 待ちの優先度逆転も)。
    // 休止により書き出しスレッドがチェーンの唯一の使用者になる。フラグ解除 (RenderFlagReset) は
    // BounceRestore の realtime 復帰より後なので、再開時に offline のまま叩くことはない。
    const bool monitoring = inputMonitoringActive.load()
                            && !offlineRenderActive.load()
                            && numInputChannels  > 0
                            && numOutputChannels > 0
                            && inputChannelData != nullptr;
    const float rs = monitorReverbSend.load();

    // モニターしていない、またはリバーブ送りが 0 なら、内部に残ったテールを一度だけ
    // リセットする (凍結保持されたテールが次にモニター ON した瞬間に漏れるのを防ぐ)。
    if (!monitoring || rs <= 0.0001f)
        if (monitorReverbDirty) { monitorReverbBus.reset(); monitorReverbDirty = false; }

    if (!monitoring) return;

    // ── 返しの入力 → L/R マッピング (トラックの入力選択に合わせる) ──
    // mono トラックは選択 ch を L/R 両方へ (= センター)、stereo は inputCh→L / inputCh+1→R。
    // これにより「録音する入力」と同じ音がセンター/正しい定位で返る。device ch0→L/ch1→R 固定だと
    // mono でも別チャンネルが R に乗って L/R 分離して聞こえる不具合の修正。
    const int   srcL = juce::jlimit(0, numInputChannels - 1, monInputCh);
    const int   srcR = monStereo ? juce::jlimit(0, numInputChannels - 1, monInputCh + 1) : srcL;
    const float* inL = inputChannelData[srcL];
    const float* inR = inputChannelData[srcR];

    // パン + フェーダー音量: 返しにトラックのパンとフェーダーを反映する (再生時と同じリニア
    // バランス則・FX 後段で適用)。pan=0/gain=1 (0dB) なら gL=gR=1 = センター等倍 (従来挙動)。
    // mono は L/R 同信号 + このゲインで定位、stereo は L=srcL/R=srcR にバランスを掛ける。
    const float panL = (monPan <= 0.0f) ? 1.0f : (1.0f - monPan);
    const float panR = (monPan >= 0.0f) ? 1.0f : (1.0f + monPan);
    const float gL   = monGain * panL;
    const float gR   = monGain * panR;

    // 主モニタトラックに INS があれば、返し音をそのチェーンに通す (EQ/Comp/VST をライブに掛ける)。
    // 空 / 未設定なら従来のドライ返し。monitorChainBuf が未確保 (デバイス未開始) のときは安全側で
    // ドライにフォールバックし、audio thread でのヒープ確保を避ける。
    const bool useChain = (monChain != nullptr)
                          && (monChain->getActivePluginCountAtomic() > 0)
                          && (monitorChainBuf.getNumSamples() >= numSamples);

    if (useChain)
    {
        // 入力選択はドライ経路と同じ (srcL/srcR・mono はセンター複製)。FX の有無で「聞こえる入力」が
        // 変わらないようにする。input を in-place で触らず monitorChainBuf のコピーを処理するので、
        // 下流の録音 (生入力書き込み) には FX が一切焼き込まれない。
        monitorChainBuf.setSize(2, numSamples, false, false, true);
        if (inL != nullptr) monitorChainBuf.copyFrom(0, 0, inL, numSamples);
        else                monitorChainBuf.clear(0, 0, numSamples);
        if (inR != nullptr) monitorChainBuf.copyFrom(1, 0, inR, numSamples);
        else                monitorChainBuf.clear(1, 0, numSamples);

        monitorMidiScratch.clear();
        monChain->processBlock(monitorChainBuf, monitorMidiScratch, nullptr);

        // 処理済み (FX 後) の返しを出力へ加算 (= ドライ返しの置き換え)。フェーダー+パンを後段で適用。
        if (numOutputChannels >= 1)
            juce::FloatVectorOperations::addWithMultiply(outputChannelData[0],
                                             monitorChainBuf.getReadPointer(0), gL, numSamples);
        if (numOutputChannels >= 2)
            juce::FloatVectorOperations::addWithMultiply(outputChannelData[1],
                                             monitorChainBuf.getReadPointer(1), gR, numSamples);

        // リバーブ送りは FX 処理後の信号から (再生時の post-fader/post-FX 送りと同じセマンティクス)。
        // フェーダー音量も乗せる (post-fader send。フェーダーを下げると返しもリバーブも一緒に下がる)。
        if (rs > 0.0001f)
        {
            const float rsg = rs * monGain;
            monitorReverbBuf.setSize(2, numSamples, false, false, true);
            monitorReverbBuf.copyFrom(0, 0, monitorChainBuf.getReadPointer(0), numSamples, rsg);
            monitorReverbBuf.copyFrom(1, 0, monitorChainBuf.getReadPointer(1), numSamples, rsg);
            monitorReverbBus.processStereo(monitorReverbBuf.getWritePointer(0),
                                           monitorReverbBuf.getWritePointer(1), numSamples);
            monitorReverbDirty = true;
            for (int ch = 0; ch < juce::jmin(2, numOutputChannels); ++ch)
                juce::FloatVectorOperations::add(outputChannelData[ch],
                                                 monitorReverbBuf.getReadPointer(ch), numSamples);
        }
        return;
    }

    // ── ドライ返し: 入力をそのまま出力へ (INS 無し・従来通り) ──
    // mono は srcL を L/R 両方へ (センター)、stereo は L=srcL / R=srcR。フェーダー+パンを後段で適用。
    if (numOutputChannels >= 1 && inL != nullptr)
        juce::FloatVectorOperations::addWithMultiply(outputChannelData[0], inL, gL, numSamples);
    if (numOutputChannels >= 2 && inR != nullptr)
        juce::FloatVectorOperations::addWithMultiply(outputChannelData[1], inR, gR, numSamples);

    // ── モニターリバーブ: 返し音にだけウェットを足す (録音ファイルには焼き込まない) ──
    // ドライ返しと同じ入力 (srcL / srcR) を送り、Rev 量 × フェーダー音量でスケールしてプレートで処理
    // (post-fader send)。出力にだけ加算するので、下流の録音 (生入力書き込み) には一切影響しない。
    if (rs > 0.0001f)
    {
        const float rsg = rs * monGain;
        monitorReverbBuf.setSize(2, numSamples, false, false, true);
        if (inL != nullptr) monitorReverbBuf.copyFrom(0, 0, inL, numSamples, rsg);
        else                monitorReverbBuf.clear(0, 0, numSamples);
        if (inR != nullptr) monitorReverbBuf.copyFrom(1, 0, inR, numSamples, rsg);
        else                monitorReverbBuf.clear(1, 0, numSamples);

        monitorReverbBus.processStereo(monitorReverbBuf.getWritePointer(0),
                                       monitorReverbBuf.getWritePointer(1), numSamples);
        monitorReverbDirty = true;

        for (int ch = 0; ch < juce::jmin(2, numOutputChannels); ++ch)
            juce::FloatVectorOperations::add(outputChannelData[ch],
                                             monitorReverbBuf.getReadPointer(ch), numSamples);
    }
}

// マルチコア描画ジョブの文脈 (audio コールバックのスタックに置き、ワーカーへ void* で渡す)。
// PlaybackSnapshot は AudioEngine の private 入れ子型で匿名 namespace からは名前参照できないため
// void* で持ち、メンバ関数 produceTrackJob 内でキャストする。
namespace
{
    struct ProduceCtx
    {
        AudioEngine*               self;
        void*                      snap;       // = PlaybackSnapshot*
        const std::vector<int>*    activeTrackIdx;
        const std::vector<Track*>* activeTracks;
        double      posStart;
        int         numSamples;
        bool        monActive;
        PluginChain* monChain;
    };
}

void AudioEngine::produceTrackJob(void* ctx, int ai)
{
    auto* c = static_cast<ProduceCtx*>(ctx);
    c->self->renderActiveTrack(*static_cast<PlaybackSnapshot*>(c->snap), ai,
                               *c->activeTrackIdx, *c->activeTracks,
                               c->posStart, c->numSamples, c->monActive, c->monChain);
}

void AudioEngine::renderActiveTrack(PlaybackSnapshot& snap, int ai,
                                    const std::vector<int>& activeTrackIdx,
                                    const std::vector<Track*>& activeTracks,
                                    double posStart, int numSamples,
                                    bool monActive, PluginChain* monChain)
{
    const int tidx = activeTrackIdx[(size_t) ai];
    // trackBuffers と clipScratch は preparePlayback で同数確保される。両方を明示的に確認して
    // (size がずれた将来の構築経路でも) ワーカースレッドが範囲外アクセスしないようにする。
    if (tidx < 0 || tidx >= (int) snap.trackBuffers.size()
                 || tidx >= (int) snap.clipScratch.size()) return;
    auto& trackBuf = snap.trackBuffers[(size_t) tidx];
    // 容量はスナップショット構築時に確保済み (avoidReallocating=true で再確保しない)。
    trackBuf.setSize(2, numSamples, false, false, true);
    trackBuf.clear();

    // ドライ描画 (Pre-Fader)。Mute/Solo/Click はトラック単位で判定済み (activeTrackIdx 構築時)。
    // クリップ読み出しは **このトラック専用スクラッチ** を渡す (並列ジョブ間で共有しない)。
    // clipScratch は preparePlayback で trackBuffers と同数・同容量に確保済み。
    auto& scratch = snap.clipScratch[(size_t) tidx];
    if (tidx < (int) snap.clipsByTrack.size())
        for (int ci : snap.clipsByTrack[(size_t) tidx])
            renderClip(snap.clips[(size_t) ci], trackBuf, scratch, posStart, numSamples,
                       /*preFader*/ true, /*allowStreaming*/ true);

    // プラグインチェーン。二重処理ガード (入力モニタ対象トラックは mixInputMonitoring が叩く)。
    auto* track = activeTracks[(size_t) ai];
    const bool isMonTarget = monActive && track != nullptr
                             && &track->getPluginChain() == monChain;
    if (track != nullptr && ! isMonTarget
        && track->getPluginChain().getActivePluginCountAtomic() > 0)
    {
        // ジョブごとに独立した空 MIDI バッファ (audio トラックは MIDI を生成しないので確保されない)。
        // 共有スクラッチを使うと並列ジョブ間で aliasing するため、必ずローカルにする。
        juce::MidiBuffer mb;
        track->getPluginChain().processBlock(trackBuf, mb, &playHead);
    }

    // PDC: 自分より遅いトラックに合わせて trackBuf を遅延 (snap.trackDelays[tidx] のみ触る = 並列安全)。
    applyTrackDelay(snap.trackDelays, tidx, trackBuf, numSamples);
}

void AudioEngine::audioDeviceIOCallbackWithContext(
    const float* const* inputChannelData, int numInputChannels,
    float* const* outputChannelData,      int numOutputChannels,
    int numSamples,
    const juce::AudioIODeviceCallbackContext&)
{
    juce::ScopedNoDenormals noDenormals;

    // ── 再生スナップショットを取得 (lock-free) ──
    // ブロック先頭で shared_ptr を 1 回コピーするだけ (極小 SpinLock 下)。以降の全処理
    // (プレビュー drain / 停止 branch / 再生 branch) はこの snap を使い、snapshotLock は保持しない。
    // 構築・解放は UI thread が行うため、この audio thread はメモリ確保/解放をしない。snap は常に非 null。
    std::shared_ptr<PlaybackSnapshot> snap;
    { const juce::SpinLock::ScopedLockType l(snapshotLock); snap = activeSnapshot; }

    // 録音設定スナップショットも同様にブロック先頭で 1 回取得し、録音書き込みとループラップ
    // (liveBuffer reset) の両方で同じ recCfg を使う (旧 recLock の lock-free 化)。常に非 null。
    std::shared_ptr<const RecordingConfig> recCfg;
    { const juce::SpinLock::ScopedLockType l(recConfigLock); recCfg = activeRecConfig; }

    // アプリ設定スナップショット (メトロノーム区間が bpmChanges/meterChanges の vector を読むため、
    // setAppSettings の構造体まるごとコピーとの data race を避ける)。常に非 null。
    std::shared_ptr<const AppSettings> appCfg;
    { const juce::SpinLock::ScopedLockType l(appSettingsLock); appCfg = activeAppSettings; }

    // モニター FX チェーン config もブロック先頭で 1 回 grab する。monCfg を握っている間は
    // チェーン (= Track) が破棄されない (clearPlayback の drain がこの shared_ptr 解放を待つ)。
    std::shared_ptr<const MonitorConfig> monCfg;
    { const juce::SpinLock::ScopedLockType l(monConfigLock); monCfg = activeMonConfig; }

    // 配信ミラー出力リング (非 null なら停止/再生ブランチの末尾で最終出力を複製する)。
    // shared_ptr を grab している間はリングが破棄されない (解除後も退役リストが回収まで保持)。
    std::shared_ptr<StreamMirrorRing> mirror;
    { const juce::SpinLock::ScopedLockType l(mirrorRingLock); mirror = activeMirrorRing; }

    // アプリ音声取り込みリング (非 null ならモニタ返し合算後・ミラー tap 前に出力へ加算する)。
    std::shared_ptr<StreamMirrorRing> appCap;
    { const juce::SpinLock::ScopedLockType l(appCaptureRingLock); appCap = activeAppCaptureRing; }
    PluginChain* const monChain    = (monCfg ? monCfg->chain : nullptr);
    const int          monInputCh  = (monCfg ? monCfg->inputCh : 0);
    const bool         monStereo   = (monCfg ? monCfg->stereo  : false);
    const float        monPan      = (monCfg ? monCfg->pan     : 0.0f);
    const float        monGain     = (monCfg ? monCfg->gain    : 1.0f);
    const bool         monActive   = inputMonitoringActive.load();

    // VU メータ平滑化係数: SR とブロック長から算出し、buffer size を変えても応答を一定に保つ。
    // numSamples (と SR) が変わらない限り std::exp を再計算しないようキャッシュする。
    if (numSamples != vuCoefForSamples)
    {
        vuCoefCached     = computeVuCoef(currentSampleRate, numSamples);
        vuOneMinusCached = 1.0f - vuCoefCached;
        vuCoefForSamples = numSamples;
    }
    const float vuCoef     = vuCoefCached;
    const float vuOneMinus = vuOneMinusCached;

    // clear output
    for (int ch = 0; ch < numOutputChannels; ++ch)
        juce::FloatVectorOperations::clear(outputChannelData[ch], numSamples);

    // 入力レベル計測ポリシー:
    //   - 再生中 (playing): 常に計測
    //   - 停止中: いずれかのトラックが Rec アーム中 か Input Monitor 中のときのみ計測
    //             それ以外は -96 へ徐々に減衰させて静かに落とす
    const bool inputMetersActive = playing.load()
                                  || anyTrackRecArmed.load()
                                  || inputMonitoringActive.load();

    if (inputMetersActive && numInputChannels > 0 && inputChannelData != nullptr)
    {
        const int n = juce::jmin(numInputChannels, kMaxInputChannels);
        for (int ch = 0; ch < n; ++ch)
        {
            const float* d = inputChannelData[ch];
            if (d == nullptr) continue;
            const juce::Range<float> r = juce::FloatVectorOperations::findMinAndMax(d, numSamples);
            const float mag = juce::jmax(std::abs(r.getStart()), std::abs(r.getEnd()));
            const float db = juce::Decibels::gainToDecibels(mag, -96.0f);
            inputPeak[ch].store(db);
            inputVUSmooth[ch] = inputVUSmooth[ch] * vuCoef + mag * vuOneMinus;
            inputVU[ch].store(juce::Decibels::gainToDecibels(inputVUSmooth[ch], -96.0f));
        }
        for (int ch = n; ch < kMaxInputChannels; ++ch)
        {
            inputPeak[ch].store(-96.0f);
            inputVU[ch].store(-96.0f);
        }
    }
    else
    {
        // 停止 + Rec/Mon どちらも無効 → 徐々に減衰
        for (int ch = 0; ch < kMaxInputChannels; ++ch)
        {
            float db = inputPeak[ch].load();
            if (db > -96.0f)
            {
                float g = juce::Decibels::decibelsToGain(db, -96.0f) * 0.80f;
                inputPeak[ch].store(juce::Decibels::gainToDecibels(g, -96.0f));
            }
            inputVUSmooth[ch] *= vuCoef;
            inputVU[ch].store(juce::Decibels::gainToDecibels(inputVUSmooth[ch], -96.0f));
        }
    }

    // 停止中でもピアノロール等からのプレビュー MIDI を処理できるよう、
    // ここでプレビューキューを先に drain しておく (スナップショットの synths に適用)。
    // 特殊値: note < 0 は「allNotesOff」。trackIdx < 0 は「全 synth 対象」(stop/rewind/clear)。
    {
        const juce::ScopedTryLock sl(previewMidiLock);
        if (sl.isLocked() && !pendingPreviewMidi.empty())
        {
            for (auto& p : pendingPreviewMidi)
            {
                if (p.note < 0)
                {
                    if (p.trackIdx < 0)
                    {
                        for (auto& s : snap->synths) if (s) s->allNotesOff();
                    }
                    else if (p.trackIdx < (int) snap->synths.size()
                             && snap->synths[(size_t) p.trackIdx])
                    {
                        snap->synths[(size_t) p.trackIdx]->allNotesOff();
                    }
                }
                else if (p.trackIdx >= 0 && p.trackIdx < (int) snap->synths.size()
                         && snap->synths[(size_t) p.trackIdx])
                {
                    if (p.isOn) snap->synths[(size_t) p.trackIdx]->noteOn(p.note, p.velocity);
                    else        snap->synths[(size_t) p.trackIdx]->noteOff(p.note);
                }
            }
            pendingPreviewMidi.clear();
        }
    }

    // ── MIDI キーボードのライブ入力を drain (停止/再生ブランチ共用) ──
    // 対象トラックの内蔵シンセへはここで note-on/off を直接適用する (プレビューと同じ経路)。
    // INS チェーン (VSTi) へは liveMidiScratch を各ブランチが processBlock の MIDI に合流させる
    // (再生ブランチでは synth 処理の「後」に mb へ足す = 内蔵シンセの二重発音を防ぐ)。
    const int liveMidiTarget = liveMidiTargetTrack.load();
    liveMidiScratch.clear();
    {
        const juce::ScopedTryLock sl(liveMidiLock);
        if (sl.isLocked() && !pendingLiveMidi.empty())
        {
            for (const auto& m : pendingLiveMidi)
                liveMidiScratch.addEvent(m, 0);
            pendingLiveMidi.clear();
        }
    }
    if (liveMidiLastTarget != liveMidiTarget)
    {
        // ターゲット変更 (トラック選択の切替等): 旧トラックで押しっぱなしのノートを止める。
        // synth は即時 allNotesOff、chain (VSTi) へは再生ループで all-notes-off を送る予約
        if (liveMidiLastTarget >= 0 && liveMidiLastTarget < (int) snap->synths.size()
            && snap->synths[(size_t) liveMidiLastTarget])
            snap->synths[(size_t) liveMidiLastTarget]->allNotesOff();
        liveMidiChainFlush = liveMidiLastTarget;
        liveMidiLastTarget = liveMidiTarget;
    }
    if (!liveMidiScratch.isEmpty()
        && liveMidiTarget >= 0 && liveMidiTarget < (int) snap->synths.size()
        && snap->synths[(size_t) liveMidiTarget])
    {
        // 内蔵シンセ OFF のトラックには **note-on を**適用しない (停止中は synth プレビューが
        // synthEnabled に依らず全 synth を描画するため、適用すると VSTi (チェーン) と二重に
        // 鳴ってしまう)。note-off / all-notes-off / pitch wheel は常に届ける — 押しっぱなしの
        // まま synth を ON→OFF に切替えた場合に落とすと、鳴動中のボイスを止められず
        // 鳴り続ける (clearPlayback 直後の snap->midi が空の窓でも note-off は失わない)
        bool synthOn = false;
        for (const auto& mp : snap->midi)
            if (mp.trackIdx == liveMidiTarget)
            {
                synthOn = (mp.track != nullptr && mp.track->isSynthEnabled());
                break;
            }
        auto& syn = snap->synths[(size_t) liveMidiTarget];
        for (const auto meta : liveMidiScratch)
        {
            const auto m = meta.getMessage();
            if      (m.isNoteOn())     { if (synthOn) syn->noteOn(m.getNoteNumber(), m.getFloatVelocity()); }
            else if (m.isNoteOff())    syn->noteOff(m.getNoteNumber());
            else if (m.isPitchWheel()) syn->setPitchWheel(m.getPitchWheelValue());
            else if (m.isAllNotesOff() || m.isAllSoundOff()) syn->allNotesOff();
        }
    }

    if (!playing.load())
    {
        // 再生が止まった最初のブロックでリバーブのテールをリセットする。
        // (凍結保持された前回のテールが次の再生開始で漏れ出すのを防ぐ)
        if (reverbBusDirty) { masterReverbBus.reset(); reverbBusDirty = false; }

        // このブロックでメータを測定したトラックの記録 (再生ブランチと同じ trackMeterFed 方式。
        // 停止中もシンセ/チェーンのプレビュー音・ライブ MIDI 演奏をメータへ反映し、
        // 測定しなかったトラックだけを後段で減衰させる)
        juce::zeromem(trackMeterFed, sizeof(trackMeterFed));

        // ── 停止時もシンセに残った音 (プレビュー音含む) は鳴らす ──
        // 各内蔵シンセを軽量に rendering してマスター出力にミックス。
        // トラックの Vol / Pan / Mute は反映する (プレビュー爆音防止)。
        // ※ メンバの previewBuf を使い回す (オーディオスレッドで毎ブロック確保しない)
        const int previewCh = juce::jmax(2, numOutputChannels);
        if (previewBuf.getNumChannels() < previewCh || previewBuf.getNumSamples() < numSamples)
            previewBuf.setSize(previewCh, numSamples, false, false, true);
        juce::MidiBuffer emptyMidi;
        for (size_t ti = 0; ti < snap->synths.size(); ++ti)
        {
            auto& syn = snap->synths[ti];
            if (!syn) continue;

            // 対応 Track を snapshot の midi から探す
            Track* track = nullptr;
            for (auto& mp : snap->midi)
                if (mp.trackIdx == (int) ti) { track = mp.track; break; }

            previewBuf.clear();
            syn->processBlock(previewBuf, emptyMidi);

            // Mute トラックは出力しない
            if (track && track->isMuted()) continue;

            // Vol (dB → linear)
            const float vol = track ? juce::Decibels::decibelsToGain(track->getVolume(), -60.0f) : 1.0f;
            // Pan: -1..+1 を左右ゲイン (sin/cos 法)
            const float pan = track ? juce::jlimit(-1.0f, 1.0f, track->getPan()) : 0.0f;
            const float panL = std::cos((pan * 0.5f + 0.5f) * juce::MathConstants<float>::halfPi);
            const float panR = std::sin((pan * 0.5f + 0.5f) * juce::MathConstants<float>::halfPi);

            for (int ch = 0; ch < numOutputChannels; ++ch)
            {
                const float chGain = vol * (ch == 0 ? panL : panR);
                const float* src = previewBuf.getReadPointer(juce::jmin(ch, previewBuf.getNumChannels() - 1));
                juce::FloatVectorOperations::addWithMultiply(outputChannelData[ch], src, chGain, numSamples);
            }

            // トラック出力メータへ反映 (ライブ MIDI 演奏 / プレビューで Peak・VU が動くように)
            if ((int) ti < kMaxTracksMeters)
            {
                trackMeterFed[ti] = 1;
                measureStereoBuf(previewBuf, numSamples,
                                 trackOutPeakL[ti], trackOutPeakR[ti],
                                 trackOutVUSmoothL[ti], trackOutVUSmoothR[ti],
                                 trackOutVUL[ti], trackOutVUR[ti],
                                 vuCoef, vol * panL, vol * panR);
            }
        }

        // ── 停止中もトラックのプラグインチェーンを処理する (Melodyne 等の編集プレビュー音を鳴らす) ──
        // 停止中はクリップを読まないので入力は無音。プラグインが自前で生成する音 (Melodyne の
        // ノート編集/スクラブのプレビュー等) だけがチェーンから出てくるので、それを拾って出力へ混ぜる。
        // プラグインの無いトラックは getActivePluginCountAtomic で早期スキップ = 無負荷。入力モニタ
        // 対象トラックは mixInputMonitoring が別途チェーンを叩くので、二重処理を避けてスキップする。
        // オフライン書き出し中は、書き出しスレッドが同じチェーンを processBlock しているため、
        // ここでプレビュー処理すると processBlock が交互に呼ばれてプラグイン状態が壊れ書き出しが
        // 破損する。書き出し中はプレビューを丸ごとスキップする (offlineRenderActive)。
        if (!offlineRenderActive.load())
        {
            // Melodyne 等が「停止中プレビュー」モードで動くよう、停止状態の playhead を供給する。
            fillPlayHead(playHead, currentPosition.load(), currentSampleRate, *appCfg,
                         /*playing*/ false, /*recording*/ false,
                         /*looping*/ false, 0.0, 0.0);
            for (auto& [tidx, trk] : snap->clipTracks)
            {
                if (trk == nullptr || trk->isMuted()) continue;
                if (tidx < 0 || tidx >= (int) snap->trackBuffers.size()) continue;
                auto& chain = trk->getPluginChain();
                if (chain.getActivePluginCountAtomic() == 0) continue;   // プラグイン無し = 触らない
                if (monActive && &chain == monChain) continue;           // モニタ経路で処理済み (二重処理回避)
                if (!chain.isPreparedFor(currentSampleRate, currentBufferSize)) continue;  // 未 prepare は安全にスキップ

                auto& buf = snap->trackBuffers[(size_t) tidx];   // 停止中は未使用のスクラッチを流用
                buf.setSize(2, numSamples, false, false, true);
                buf.clear();                                     // 無音入力 (プラグインの自前生成音のみ拾う)
                emptyMidi.clear();
                chain.processBlock(buf, emptyMidi, &playHead);

                // トラックの Vol/Pan を反映して出力へ (synth プレビューと同じ扱い・マスターは通さない)
                const float vol  = juce::Decibels::decibelsToGain(trk->getVolume(), -60.0f);
                const float pan  = juce::jlimit(-1.0f, 1.0f, trk->getPan());
                const float panL = std::cos((pan * 0.5f + 0.5f) * juce::MathConstants<float>::halfPi);
                const float panR = std::sin((pan * 0.5f + 0.5f) * juce::MathConstants<float>::halfPi);
                for (int ch = 0; ch < numOutputChannels; ++ch)
                {
                    const float chGain = vol * (ch == 0 ? panL : panR);
                    const float* src = buf.getReadPointer(juce::jmin(ch, buf.getNumChannels() - 1));
                    juce::FloatVectorOperations::addWithMultiply(outputChannelData[ch], src, chGain, numSamples);
                }
                if (tidx < kMaxTracksMeters)
                {
                    trackMeterFed[tidx] = 1;
                    measureStereoBuf(buf, numSamples,
                                     trackOutPeakL[tidx], trackOutPeakR[tidx],
                                     trackOutVUSmoothL[tidx], trackOutVUSmoothR[tidx],
                                     trackOutVUL[tidx], trackOutVUR[tidx],
                                     vuCoef, vol * panL, vol * panR);
                }
            }

            // ── ライブ MIDI (MIDI キーボード) 対象トラックの INS チェーン (VSTi) ──
            // MIDI トラックは clipTracks (音声クリップのトラック一覧) に出ないため上のループでは
            // 処理されない。停止中でも VSTi 音源で弾けるよう、対象トラックのチェーンだけ毎ブロック
            // 処理する (発音イベントの無いブロックも回す = 押しっぱなし/テールの継続レンダリング)。
            // trackIdx → MidiPlayback の解決ヘルパ (snap->midi 内で trackIdx は一意)
            auto findMidiPlayback = [&snap](int tidx) -> const MidiPlayback*
            {
                if (tidx < 0) return nullptr;
                for (auto& mp : snap->midi)
                    if (mp.trackIdx == tidx) return &mp;
                return nullptr;
            };
            auto chainProcessable = [&](const MidiPlayback* mp) -> PluginChain*
            {
                if (mp == nullptr || mp->track == nullptr) return nullptr;
                if (mp->trackIdx >= (int) snap->trackBuffers.size()) return nullptr;
                auto& chain = mp->track->getPluginChain();
                if (chain.getActivePluginCountAtomic() == 0) return nullptr;
                if (monActive && &chain == monChain) return nullptr;   // モニタ経路と二重処理しない
                if (!chain.isPreparedFor(currentSampleRate, currentBufferSize)) return nullptr;
                return &chain;
            };

            // ターゲット変更で残った旧トラックのチェーンへ all-notes-off を届ける (再生ループの
            // liveMidiChainFlush 消費と対)。これをしないと、停止中にトラック選択を切替えた時
            // 旧 VSTi の押しっぱなしノートがプラグイン内部に残り、再ターゲット時に鳴り出す
            if (liveMidiChainFlush >= 0)
            {
                if (auto* mp = findMidiPlayback(liveMidiChainFlush))
                    if (auto* chain = chainProcessable(mp))
                    {
                        auto& buf = snap->trackBuffers[(size_t) mp->trackIdx];
                        buf.setSize(2, numSamples, false, false, true);
                        buf.clear();
                        chainMidiScratch.clear();
                        for (int ch = 1; ch <= 16; ++ch)
                            chainMidiScratch.addEvent(juce::MidiMessage::allNotesOff(ch), 0);
                        chain->processBlock(buf, chainMidiScratch, &playHead);
                        // 出力は混ぜない (旧ターゲットの停止処理なので無音で良い)
                    }
                liveMidiChainFlush = -1;
            }

            if (auto* mp = findMidiPlayback(liveMidiTarget))
            {
                if (auto* chain = chainProcessable(mp))
                {
                    auto& buf = snap->trackBuffers[(size_t) mp->trackIdx];
                    buf.setSize(2, numSamples, false, false, true);
                    buf.clear();
                    chainMidiScratch.clear();
                    chainMidiScratch.addEvents(liveMidiScratch, 0, numSamples, 0);
                    chain->processBlock(buf, chainMidiScratch, &playHead);

                    // ミュート中もイベントは届けて (上で processBlock 済み) 出力だけ混ぜない。
                    // ミュートでチェーン処理ごと止めると、ミュート中に離した鍵の note-off が
                    // プラグインに届かず、ミュート解除後に鳴りっぱなしになる
                    if (!mp->track->isMuted())
                    {
                        const float vol  = juce::Decibels::decibelsToGain(mp->track->getVolume(), -60.0f);
                        const float pan  = juce::jlimit(-1.0f, 1.0f, mp->track->getPan());
                        const float panL = std::cos((pan * 0.5f + 0.5f) * juce::MathConstants<float>::halfPi);
                        const float panR = std::sin((pan * 0.5f + 0.5f) * juce::MathConstants<float>::halfPi);
                        for (int ch = 0; ch < numOutputChannels; ++ch)
                        {
                            const float chGain = vol * (ch == 0 ? panL : panR);
                            const float* src = buf.getReadPointer(juce::jmin(ch, buf.getNumChannels() - 1));
                            juce::FloatVectorOperations::addWithMultiply(outputChannelData[ch], src, chGain, numSamples);
                        }
                        if (mp->trackIdx < kMaxTracksMeters)
                        {
                            trackMeterFed[mp->trackIdx] = 1;
                            measureStereoBuf(buf, numSamples,
                                             trackOutPeakL[mp->trackIdx], trackOutPeakR[mp->trackIdx],
                                             trackOutVUSmoothL[mp->trackIdx], trackOutVUSmoothR[mp->trackIdx],
                                             trackOutVUL[mp->trackIdx], trackOutVUR[mp->trackIdx],
                                             vuCoef, vol * panL, vol * panR);
                        }
                    }
                }
            }
        }

        // ── 停止時のマスターメータ ──
        // プレビュー / ライブ MIDI の音が出ているブロックはその出力を測定して反映し
        // (弾いた音で Peak・VU が動くように)、無音なら従来どおり徐々に減衰させて 0 に落とす。
        // 測定点はモニタ返し (mixInputMonitoring) の前 = ここまでの出力はプレビュー音のみ
        {
            float magL = 0.0f, magR = 0.0f;
            if (numOutputChannels >= 1 && outputChannelData[0] != nullptr)
            {
                const auto r = juce::FloatVectorOperations::findMinAndMax(outputChannelData[0], numSamples);
                magL = juce::jmax(std::abs(r.getStart()), std::abs(r.getEnd()));
            }
            if (numOutputChannels >= 2 && outputChannelData[1] != nullptr)
            {
                const auto r = juce::FloatVectorOperations::findMinAndMax(outputChannelData[1], numSamples);
                magR = juce::jmax(std::abs(r.getStart()), std::abs(r.getEnd()));
            }
            else
                magR = magL;

            if (magL > 1.0e-6f || magR > 1.0e-6f)
            {
                peakL.store(juce::Decibels::gainToDecibels(magL, -96.0f));
                peakR.store(juce::Decibels::gainToDecibels(magR, -96.0f));
                const float oneMinus = 1.0f - vuCoef;
                vuSmoothL = vuSmoothL * vuCoef + magL * oneMinus;
                vuSmoothR = vuSmoothR * vuCoef + magR * oneMinus;
                vuL.store(juce::Decibels::gainToDecibels(vuSmoothL, -96.0f));
                vuR.store(juce::Decibels::gainToDecibels(vuSmoothR, -96.0f));
            }
            else
            {
                // ピークは linear gain で乗算減衰（block ≈ 10ms 想定で約 0.4〜0.5 秒で -96 へ）
                // VU は既存のスムージング係数 (0.97) を流用
                const float peakDecay = 0.80f;
                auto decayPeakDb = [peakDecay](std::atomic<float>& vDb)
                {
                    float db = vDb.load();
                    if (db <= -96.0f) return;
                    float g = juce::Decibels::decibelsToGain(db, -96.0f) * peakDecay;
                    vDb.store(juce::Decibels::gainToDecibels(g, -96.0f));
                };
                decayPeakDb(peakL);
                decayPeakDb(peakR);
                vuSmoothL *= vuCoef;
                vuSmoothR *= vuCoef;
                vuL.store(juce::Decibels::gainToDecibels(vuSmoothL, -96.0f));
                vuR.store(juce::Decibels::gainToDecibels(vuSmoothR, -96.0f));
            }
        }

        // 実トラック数までに制限。このブロックで測定した (プレビュー/ライブ MIDI の) トラックは
        // 減衰させない。完全無音 (peak/VU とも底) のスキップと減衰の実体は
        // decayTrackMeter (再生中の「未測定トラックの減衰」と共通ヘルパ) が行う。
        const int meterN = juce::jmin(meterTrackCount.load(), kMaxTracksMeters);
        for (int t = 0; t < meterN; ++t)
            if (!trackMeterFed[t])
                decayTrackMeter(trackOutPeakL[t], trackOutPeakR[t],
                                trackOutVUSmoothL[t], trackOutVUSmoothR[t],
                                trackOutVUL[t], trackOutVUR[t], vuCoef);

        // 停止中も入力モニタリングは通す (ドライ返し + モニターリバーブ、INS があれば FX も)
        mixInputMonitoring(inputChannelData, numInputChannels,
                           outputChannelData, numOutputChannels, numSamples, monChain,
                           monInputCh, monStereo, monPan, monGain);

        // アプリ音声取り込み (ブラウザ等): モニタ返し合算後・ミラー tap 前に加算する
        // (ヘッドホンと配信ミラーの両方に乗り、録音の生入力経路には乗らない)
        if (appCap != nullptr)
            mixAppCapture(*appCap, outputChannelData, numOutputChannels, numSamples);

        // 配信ミラー出力: 停止中の最終出力 (モニタ返し込み = 配信で喋っている声) も複製する
        if (mirror != nullptr && numOutputChannels > 0 && outputChannelData[0] != nullptr)
            mirror->push(outputChannelData[0],
                         (numOutputChannels > 1) ? outputChannelData[1] : nullptr,
                         numSamples);

        // 停止中は再生デクリックの直前出力値を 0 に戻す。次の再生開始で再構築 (preparePlayback)
        // が走って playbackGen が増える場合は 0 起点のクロスフェード (= 短いフェードイン) になり、
        // 再生開始時のクリックも防げる (再構築が走らない場合の開始はクリップ自身の fade-in に委ねる)。
        declickLast[0] = declickLast[1] = 0.0f;
        declickXfadeRemain = 0;
        return;
    }

    // ワークバッファに全クリップをミックス
    workBuffer.setSize(juce::jmax(2, numOutputChannels), numSamples, false, false, true);
    workBuffer.clear();

    // このブロックでメータを測定したトラックの記録をリセット (未測定分はブロック末尾で減衰)
    juce::zeromem(trackMeterFed, sizeof(trackMeterFed));

    double posStart = currentPosition.load();

    // プラグインへ供給する再生位置情報をブロック先頭で 1 回更新する (Melodyne 等の transport
    // 同期用)。トラック/MIDI/マスターの全チェーンがこの playHead を共有する。
    fillPlayHead(playHead, posStart, currentSampleRate, *appCfg,
                 /*playing*/ true, isRecordingActive.load(),
                 loopActive.load(), loopStartSecs.load(), loopEndSecs.load());

    // 簡易リバーブ送りバスのフラグ
    bool anyReverbSend = false;
    // ソロ中は CLICK (メトロノーム) も他トラック同様に黙らせる (クリック自身のソロは除く)。
    // 判定は下の anySolo のスコープで行い、後段のメトロノーム合成ブロックが参照する
    bool clickSoloBlocked = false;

    {
        // ライブ Solo 判定（オーディオクリップだけでなく MIDI トラックも対象に含める）。
        // clipTracks は dedup 済みトラック一覧なので O(トラック数) で済む (全 clips 走査しない)。
        bool anySolo = false;
        for (auto& [tidx, trk] : snap->clipTracks)
            if (trk && trk->isSoloed()) { anySolo = true; break; }
        if (!anySolo)
            for (auto& mp : snap->midi)
                if (mp.track && mp.track->isSoloed()) { anySolo = true; break; }
        // CLICK トラックのソロも anySolo に含める (clipTracks は Click 除外のため明示チェック。
        // これが無いと CLICK をソロにしても他トラックが鳴り続け「クリックだけ」にならない)
        if (!anySolo && snap->clickTrack != nullptr && snap->clickTrack->isSoloed())
            anySolo = true;
        // フォルダトラックのソロも含める (フォルダはクリップを持たず clipTracks に出ないため
        // 明示チェック。フォルダをソロにすると配下の子だけが鳴る)
        if (!anySolo)
            for (auto& fb : snap->folderBuses)
                if (fb.track != nullptr && fb.track->isSoloed()) { anySolo = true; break; }

        clickSoloBlocked = anySolo
                           && (snap->clickTrack == nullptr
                               || !snap->clickTrack->isSoloed());

        // 所属フォルダの解決 (非所属は nullptr)。Mute/Solo の継承判定に使う
        auto folderOf = [&snap](int tidx) -> Track*
        {
            return (tidx >= 0 && tidx < (int)snap->folderOfTrack.size())
                       ? snap->folderOfTrack[(size_t)tidx] : nullptr;
        };

        // 各トラックの「有効性」を判定し、使うトラックのリストを作る。
        // スクラッチを再利用 (clear で長さ 0 に戻すと容量は保たれヒープ確保が起きない)。
        // フォルダの Mute は子を黙らせ、フォルダの Solo は子を鳴らす (継承)。
        auto& activeTrackIdx = activeTrackIdxScratch;
        auto& activeTracks   = activeTracksScratch;
        activeTrackIdx.clear();
        activeTracks.clear();
        for (auto& [tidx, trk] : snap->clipTracks)
        {
            if (trk == nullptr) continue;          // clipTracks は Click 除外・dedup 済み
            Track* fld = folderOf(tidx);
            if (trk->isMuted() || (fld != nullptr && fld->isMuted())) continue;
            if (anySolo && !trk->isSoloed() && !(fld != nullptr && fld->isSoloed())) continue;
            activeTrackIdx.push_back(tidx);
            activeTracks.push_back(trk);
        }

        // 簡易リバーブ送りバスの clear は遅延実行: send > 0 のトラックが現れた最初の時だけ。
        // anyReverbSend が false の間は reverbSendBuf を READ しない (processStereo はガード済み) ため安全。
        reverbSendBuf.setSize(2, numSamples, false, false, true);
        bool reverbBufCleared = false;

        // ── 描画フェーズ (マルチコア対応) ──
        // 各トラックの「クリップ描画 + プラグインチェーン + PDC」はトラックごとに独立
        // (自分の trackBuffers[tidx] / trackDelays[tidx] のみ触る) なので、ワーカープールへ分散する。
        // vol/pan・マスター加算・リバーブ送り・メータは共有 (workBuffer/reverbSendBuf/メータ配列) を
        // 書くため、この後の **直列フェーズでトラック index 昇順** に行う (= スレッド数に依らず
        // 加算順が固定 = 出力ビット同一)。
        if (!activeTrackIdx.empty() && (int)snap->trackBuffers.size() > 0)
        {
            const int nActive = (int) activeTrackIdx.size();
            ProduceCtx pctx { this, snap.get(), &activeTrackIdx, &activeTracks,
                              posStart, numSamples, monActive, monChain };

            if (multicoreEnabled.load(std::memory_order_relaxed)
                && workerPool.getNumWorkers() > 0
                && nActive >= kMinTracksForThreads)
            {
                workerPool.parallelFor(nActive, &AudioEngine::produceTrackJob, &pctx);
            }
            else
            {
                for (int ai = 0; ai < nActive; ++ai)
                    produceTrackJob(&pctx, ai);
            }

            // 直列フェーズ: 固定順 (activeTrackIdx 昇順) に vol/pan・メータ・マスター加算・リバーブ送り。
            for (size_t ai = 0; ai < activeTrackIdx.size(); ++ai)
            {
                const int tidx = activeTrackIdx[ai];
                if (tidx < 0 || tidx >= (int)snap->trackBuffers.size()) continue;
                auto& trackBuf = snap->trackBuffers[(size_t)tidx];
                auto* track    = activeTracks[ai];

                // トラック Vol / Pan を算出（メータをポストフェーダーにするため）
                const float vol  = track ? juce::Decibels::decibelsToGain(track->getVolume()) : 1.0f;
                const float pan  = track ? track->getPan() : 0.0f;
                const float panL = (pan <= 0.0f) ? 1.0f : (1.0f - pan);
                const float panR = (pan >= 0.0f) ? 1.0f : (1.0f + pan);
                const float gL = vol * panL;
                const float gR = vol * panR;

                // トラック出力メータ（Vol/Pan 適用後 = ポストフェーダー。フェーダーに追従する）
                if (tidx >= 0 && tidx < kMaxTracksMeters)
                {
                    trackMeterFed[tidx] = 1;
                    measureStereoBuf(trackBuf, numSamples,
                                     trackOutPeakL[tidx], trackOutPeakR[tidx],
                                     trackOutVUSmoothL[tidx], trackOutVUSmoothR[tidx],
                                     trackOutVUL[tidx], trackOutVUR[tidx],
                                     vuCoef, gL, gR);
                }

                // master (フォルダ配下ならフォルダバス) に加算
                juce::AudioBuffer<float>* dest = &workBuffer;
                const int busIdx = (tidx < (int)snap->folderBusOfTrack.size())
                                       ? snap->folderBusOfTrack[(size_t)tidx] : -1;
                if (busIdx >= 0)
                {
                    auto& fb = snap->folderBuses[(size_t)busIdx];
                    if (!fb.fed)
                    {
                        fb.buf.setSize(2, numSamples, false, false, true);
                        fb.buf.clear();
                        fb.fed = true;
                    }
                    dest = &fb.buf;
                }
                dest->addFrom(0, 0, trackBuf, 0, 0, numSamples, gL);
                if (dest->getNumChannels() >= 2 && trackBuf.getNumChannels() >= 2)
                    dest->addFrom(1, 0, trackBuf, 1, 0, numSamples, gR);

                // 簡易リバーブ送り (post-fader / post-pan)。スライダー値は二乗テーパーで
                // 送りゲイン化する (低位置での効き過ぎ防止・Track::reverbSendGain に一元化)
                const float rs = track ? Track::reverbSendGain(track->getReverbSend()) : 0.0f;
                if (rs > 0.0001f)
                {
                    if (!reverbBufCleared) { reverbSendBuf.clear(); reverbBufCleared = true; }
                    anyReverbSend = true;
                    reverbSendBuf.addFrom(0, 0, trackBuf, 0, 0, numSamples, gL * rs);
                    if (trackBuf.getNumChannels() >= 2)
                        reverbSendBuf.addFrom(1, 0, trackBuf, 1, 0, numSamples, gR * rs);
                }
            }
        }

        // ── MIDI トラック: 内蔵シンセでレンダリング ──
        // 不連続 (seek/loop wrap) または初回再生を検出。検出時は all-notes-off と
        // 「posStart までの Program Change / Control Change / Pitch Bend」を再送する。
        // これをしないと、再生開始位置より前にあった音色指定（Program Change 等）が
        // VST 音源に届かず、GM シンセ等がデフォルト音色（ピアノ）のままになる。
        const double blockDur = (double)numSamples / currentSampleRate;
        bool needsStateRefresh = false;
        if (lastBlockPosStart < 0.0)
        {
            needsStateRefresh = true;   // 初回再生 or stop 後再生
        }
        else
        {
            const double expected = lastBlockPosStart + blockDur;
            if (std::abs(posStart - expected) > 0.001)
            {
                needsStateRefresh = true;
                for (auto& s : snap->synths) if (s) s->allNotesOff();
            }
        }
        lastBlockPosStart = posStart;

        const double posEnd = posStart + blockDur;
        for (auto& mp : snap->midi)
        {
            if (mp.track == nullptr) continue;
            // クリップ 0 個で何も鳴らさない MIDI トラック (ライブ対象でも flush 対象でもなく、
            // synth OFF・チェーン空) はバッファ確保/メータ/マスター加算のコストを払わない
            // (空 MIDI トラックを snap に含めるようにした分の節約)
            if (mp.events.empty()
                && mp.trackIdx != liveMidiTarget && mp.trackIdx != liveMidiChainFlush
                && !mp.track->isSynthEnabled()
                && mp.track->getPluginChain().getActivePluginCountAtomic() == 0)
                continue;
            Track* midiFld = folderOf(mp.trackIdx);
            if (mp.track->isMuted() || (midiFld != nullptr && midiFld->isMuted())) continue;
            if (anySolo && !mp.track->isSoloed()
                && !(midiFld != nullptr && midiFld->isSoloed())) continue;
            if (mp.trackIdx < 0 || mp.trackIdx >= (int)snap->trackBuffers.size()) continue;
            if (mp.trackIdx >= (int)snap->synths.size()) continue;
            auto& syn = snap->synths[(size_t)mp.trackIdx];
            if (!syn) continue;

            auto& trackBuf = snap->trackBuffers[(size_t)mp.trackIdx];
            trackBuf.setSize(2, numSamples, false, false, true);
            trackBuf.clear();

            // 現在のトラック移調量（Octave + Semitone）。実演奏中も即時反映
            const int transpose = mp.track->getTotalTransposeSemitones();
            // シンセ波形もリアルタイム反映
            syn->setWaveform(mp.track->getSynthWaveform());

            // note/state イベント収集用の専用スクラッチ (chainMidiScratch とは別物・aliasing 回避)。
            // clear で長さ 0 に戻すと容量は保たれヒープ確保が起きない。
            auto& mb = synthMidiScratch;
            mb.clear();

            // ── 移調量変更検知: 旧 NoteOn と新 NoteOff のミスマッチで残る音を止める ──
            // 旧シフトで鳴っていた音を all-notes-off で打ち切る。保持中ノートの「鳴らし直し」は
            // PC/CC 再送 (needsStateRefresh) の後で行う (プラグイン音源の音色が確定してから鳴らす)。
            bool transposeChanged = false;
            if (transpose != mp.lastTranspose)
            {
                syn->allNotesOff();
                for (int ch = 1; ch <= 16; ++ch)
                    mb.addEvent(juce::MidiMessage::allNotesOff(ch), 0);
                mp.lastTranspose = transpose;
                transposeChanged = true;
            }

            // 状態再送: posStart より前にあった Program Change / Control Change /
            // Pitch Bend / Channel Pressure をブロック先頭 (sample 0) で再送する
            // (実体は appendMidiStateResend・書き出しの開始ブロックと共用)
            if (needsStateRefresh)
                appendMidiStateResend(mb, mp.events, posStart);

            // 移調量が変わった場合は、posStart 時点で保持中のノート (note-on 済み・未 note-off) を
            // 新シフトで sample 0 に鳴らし直す ("再度鳴らす")。これで Octave/Semitone をリアルタイムに
            // 切り替えても、伸ばしている音が途切れず新しいピッチへ即移行する。allNotesOff / PC/CC 再送の
            // 後に入れる (mb の同一 sample は挿入順を保つので「止める→音色確定→鳴らす」になる)。
            // transpose 変更は手動操作なので低頻度で、線形スキャンでも実時間負荷は無視できる。
            if (transposeChanged)
            {
                std::array<juce::uint8, 16 * 128> heldVel {};   // (channel*128+note) → velocity, 0 = off
                for (const auto& m : mp.events)
                {
                    if (m.getTimeStamp() >= posStart) break;
                    if (!m.isNoteOnOrOff()) continue;
                    const int ch   = m.getChannel() - 1;
                    const int note = m.getNoteNumber();
                    if (ch < 0 || ch >= 16 || note < 0 || note > 127) continue;
                    if (m.isNoteOn() && m.getVelocity() > 0)
                        heldVel[(size_t)(ch * 128 + note)] = (juce::uint8) m.getVelocity();
                    else
                        heldVel[(size_t)(ch * 128 + note)] = 0;
                }
                for (int ch = 0; ch < 16; ++ch)
                    for (int note = 0; note < 128; ++note)
                    {
                        const auto vel = heldVel[(size_t)(ch * 128 + note)];
                        if (vel == 0) continue;
                        mb.addEvent(juce::MidiMessage::noteOn(ch + 1,
                                        juce::jlimit(0, 127, note + transpose), (juce::uint8) vel), 0);
                    }
            }

            // posStart 以上の最初のイベントへ二分探索でジャンプ（線形スキャン回避）
            auto it = std::lower_bound(mp.events.begin(), mp.events.end(), posStart,
                [](const juce::MidiMessage& m, double v) { return m.getTimeStamp() < v; });
            for (; it != mp.events.end(); ++it)
            {
                const double t = it->getTimeStamp();
                if (t >= posEnd) break;
                const int sp = (int)((t - posStart) * currentSampleRate);

                juce::MidiMessage out = *it;
                if (transpose != 0 && out.isNoteOnOrOff())
                    out.setNoteNumber(juce::jlimit(0, 127, out.getNoteNumber() + transpose));
                mb.addEvent(out, juce::jlimit(0, numSamples - 1, sp));
            }

            // 内蔵シンセ ON のときは trackBuf に音を書き込み、MIDI イベントは消費しない。
            // OFF のときは trackBuf は空のまま、MIDI を INS チェーン (VST 音源等) に渡して鳴らす。
            if (mp.track->isSynthEnabled())
                syn->processBlock(trackBuf, mb);

            // ライブ MIDI (MIDI キーボード) を対象トラックの INS チェーン (VSTi) へ渡す。
            // 内蔵シンセへは drain 時に noteOn を直接適用済みなので、synth 処理の「後」に
            // mb へ足す (mb 経由でも synth に届くと二重発音になる)。ターゲットが切り替わった
            // 直後は旧トラックの chain へ all-notes-off を送り、押しっぱなしノートを止める
            if (mp.trackIdx == liveMidiTarget && !liveMidiScratch.isEmpty())
                mb.addEvents(liveMidiScratch, 0, numSamples, 0);
            if (mp.trackIdx == liveMidiChainFlush)
            {
                for (int ch = 1; ch <= 16; ++ch)
                    mb.addEvent(juce::MidiMessage::allNotesOff(ch), 0);
                liveMidiChainFlush = -1;
            }

            // プラグインチェーン（音源プラグインなら MIDI から音を生成、エフェクトなら trackBuf を加工）。
            // ロックを取らずに処理対象有無を判定する。mb は本トラックの note/state 入力なのでそのまま渡す。
            if (mp.track->getPluginChain().getActivePluginCountAtomic() > 0)
                mp.track->getPluginChain().processBlock(trackBuf, mb, &playHead);

            applyTrackDelay(snap->trackDelays, mp.trackIdx, trackBuf, numSamples);

            // トラック Vol / Pan を先に算出（メータをポストフェーダーにするため）
            const float vol  = juce::Decibels::decibelsToGain(mp.track->getVolume());
            const float pan  = mp.track->getPan();
            const float panL = (pan <= 0.0f) ? 1.0f : (1.0f - pan);
            const float panR = (pan >= 0.0f) ? 1.0f : (1.0f + pan);
            const float gL = vol * panL;
            const float gR = vol * panR;

            // トラック出力メータ（Vol/Pan 適用後 = ポストフェーダー）
            if (mp.trackIdx >= 0 && mp.trackIdx < kMaxTracksMeters)
            {
                trackMeterFed[mp.trackIdx] = 1;
                measureStereoBuf(trackBuf, numSamples,
                                 trackOutPeakL[mp.trackIdx], trackOutPeakR[mp.trackIdx],
                                 trackOutVUSmoothL[mp.trackIdx], trackOutVUSmoothR[mp.trackIdx],
                                 trackOutVUL[mp.trackIdx], trackOutVUR[mp.trackIdx],
                                 vuCoef, gL, gR);
            }

            // master (フォルダ配下ならフォルダバス) に加算
            {
                juce::AudioBuffer<float>* dest = &workBuffer;
                const int busIdx = (mp.trackIdx < (int)snap->folderBusOfTrack.size())
                                       ? snap->folderBusOfTrack[(size_t)mp.trackIdx] : -1;
                if (busIdx >= 0)
                {
                    auto& fb = snap->folderBuses[(size_t)busIdx];
                    if (!fb.fed)
                    {
                        fb.buf.setSize(2, numSamples, false, false, true);
                        fb.buf.clear();
                        fb.fed = true;
                    }
                    dest = &fb.buf;
                }
                dest->addFrom(0, 0, trackBuf, 0, 0, numSamples, gL);
                if (dest->getNumChannels() >= 2 && trackBuf.getNumChannels() >= 2)
                    dest->addFrom(1, 0, trackBuf, 1, 0, numSamples, gR);
            }

            const float rs = Track::reverbSendGain(mp.track->getReverbSend());
            if (rs > 0.0001f)
            {
                if (!reverbBufCleared) { reverbSendBuf.clear(); reverbBufCleared = true; }
                anyReverbSend = true;
                reverbSendBuf.addFrom(0, 0, trackBuf, 0, 0, numSamples, gL * rs);
                if (trackBuf.getNumChannels() >= 2)
                    reverbSendBuf.addFrom(1, 0, trackBuf, 1, 0, numSamples, gR * rs);
            }
        }

        // ── フォルダバス: 子の合算に INS チェーン → フォルダ Vol を掛けてマスターへ ──
        // trackIdx 昇順 (folderBuses 構築順) の直列処理なので加算順は決定論。
        // 子が 1 つも鳴っていないブロックはチェーンを叩かない (非アクティブトラックの
        // チェーンを処理しない既存のトラック挙動と同じ = テールはそこで切れる)。
        for (auto& fb : snap->folderBuses)
        {
            if (!fb.fed) continue;
            fb.fed = false;
            if (fb.track == nullptr) continue;

            if (fb.track->getPluginChain().getActivePluginCountAtomic() > 0)
            {
                chainMidiScratch.clear();
                fb.track->getPluginChain().processBlock(fb.buf, chainMidiScratch, &playHead);
            }

            // フォルダの Vol / Pan (トラックと同じリニアバランス則。pan=0 は L/R 等倍 = 従来挙動)
            const float fVol  = juce::Decibels::decibelsToGain(fb.track->getVolume());
            const float fPan  = fb.track->getPan();
            const float fPanL = (fPan <= 0.0f) ? 1.0f : (1.0f - fPan);
            const float fPanR = (fPan >= 0.0f) ? 1.0f : (1.0f + fPan);
            const float fGL = fVol * fPanL;
            const float fGR = fVol * fPanR;

            // フォルダ出力メータ (Vol/Pan 適用後 = ポストフェーダー)
            if (fb.trackIdx >= 0 && fb.trackIdx < kMaxTracksMeters)
            {
                trackMeterFed[fb.trackIdx] = 1;
                measureStereoBuf(fb.buf, numSamples,
                                 trackOutPeakL[fb.trackIdx], trackOutPeakR[fb.trackIdx],
                                 trackOutVUSmoothL[fb.trackIdx], trackOutVUSmoothR[fb.trackIdx],
                                 trackOutVUL[fb.trackIdx], trackOutVUR[fb.trackIdx],
                                 vuCoef, fGL, fGR);
            }

            workBuffer.addFrom(0, 0, fb.buf, 0, 0, numSamples, fGL);
            if (workBuffer.getNumChannels() >= 2 && fb.buf.getNumChannels() >= 2)
                workBuffer.addFrom(1, 0, fb.buf, 1, 0, numSamples, fGR);

            // フォルダの簡易リバーブ送り (post-fader / post-pan・トラックと同じ二乗テーパー)
            const float frs = fb.track != nullptr
                                  ? Track::reverbSendGain(fb.track->getReverbSend()) : 0.0f;
            if (frs > 0.0001f)
            {
                if (!reverbBufCleared) { reverbSendBuf.clear(); reverbBufCleared = true; }
                anyReverbSend = true;
                reverbSendBuf.addFrom(0, 0, fb.buf, 0, 0, numSamples, fGL * frs);
                if (fb.buf.getNumChannels() >= 2)
                    reverbSendBuf.addFrom(1, 0, fb.buf, 1, 0, numSamples, fGR * frs);
            }
        }

        // ── このブロックで測定されなかったトラックの出力メータを減衰させる ──
        // ミュート / ソロ外 / フォルダごとミュートで非アクティブになったトラックと、
        // 子が鳴らず fed されなかったフォルダバスは measureStereoBuf を通らないため、
        // 減衰させないとメータが最後の値のまま凍結する (停止時ブランチと同じバリスティクス)。
        {
            const int meterN = juce::jmin(meterTrackCount.load(), kMaxTracksMeters);
            for (int t = 0; t < meterN; ++t)
            {
                if (trackMeterFed[t]) continue;
                decayTrackMeter(trackOutPeakL[t], trackOutPeakR[t],
                                trackOutVUSmoothL[t], trackOutVUSmoothR[t],
                                trackOutVUL[t], trackOutVUR[t], vuCoef);
            }
        }
    }

    // リバーブ送りバスをウェットだけ処理してマスターへ加算
    if (anyReverbSend && reverbSendBuf.getNumChannels() >= 2)
    {
        masterReverbBus.processStereo(reverbSendBuf.getWritePointer(0),
                                       reverbSendBuf.getWritePointer(1),
                                       numSamples);
        reverbBusDirty = true;   // 停止時にテールをリセットするための印
        if (workBuffer.getNumChannels() >= 2)
        {
            workBuffer.addFrom(0, 0, reverbSendBuf, 0, 0, numSamples, 1.0f);
            workBuffer.addFrom(1, 0, reverbSendBuf, 1, 0, numSamples, 1.0f);
        }
    }

    // ── 再生コンテンツ切替のデクリック (マスターチェーン前のドライ信号に適用) ──
    // 再生中の Undo 等で playbackGen が変わったら、直前出力 declickLast から現在の workBuffer へ
    // kDeclickLen サンプルでクロスフェードして不連続点を橋渡しする。信号が連続な箇所
    // (送り/マスターリバーブのテール等) では declickHold≈先頭サンプルとなりほぼ無改変、
    // 再生クリップが消える/差し替わる瞬間だけが滑らかになる。マスターチェーンより前に
    // 適用するので、マスター FX にクリックが入力されない。
    {
        const juce::uint32 gen = playbackGen.load(std::memory_order_relaxed);
        if (gen != declickSeenGen)
        {
            declickSeenGen = gen;
            declickXfadeRemain = kDeclickLen;
            for (int ch = 0; ch < 2; ++ch) declickHold[ch] = declickLast[ch];
        }
        const int nCh = juce::jmin(2, workBuffer.getNumChannels());
        if (declickXfadeRemain > 0)
        {
            for (int ch = 0; ch < nCh; ++ch)
            {
                float* d = workBuffer.getWritePointer(ch);
                int rem = declickXfadeRemain;
                for (int i = 0; i < numSamples && rem > 0; ++i, --rem)
                {
                    const float t = (float) rem / (float) kDeclickLen;   // 1→0
                    d[i] = declickHold[ch] * t + d[i] * (1.0f - t);
                }
            }
            declickXfadeRemain = juce::jmax(0, declickXfadeRemain - numSamples);
        }
        for (int ch = 0; ch < nCh; ++ch)
            declickLast[ch] = workBuffer.getReadPointer(ch)[numSamples - 1];
    }

    // マスターインサート → マスターゲイン。ロックを取らずに処理対象有無を判定する。
    if (masterChain && masterChain->getActivePluginCountAtomic() > 0)
    {
        chainMidiScratch.clear();
        masterChain->processBlock(workBuffer, chainMidiScratch, &playHead);
    }
    workBuffer.applyGain(masterGain.load());

    // 出力へコピー
    const int outCh = juce::jmin(numOutputChannels, workBuffer.getNumChannels());
    for (int ch = 0; ch < outCh; ++ch)
        juce::FloatVectorOperations::copy(outputChannelData[ch],
                                          workBuffer.getReadPointer(ch),
                                          numSamples);

    // 実時間キャプチャ（マスターゲイン後・メトロノーム前の信号）
    if (captureActive.load())
    {
        const int total  = captureTotalSamples.load();
        const int pos    = captureWritePos.load();
        if (pos < total)
        {
            const int toWrite = juce::jmin(numSamples, total - pos);
            const juce::ScopedLock sl(captureLock);
            for (int ch = 0; ch < juce::jmin(2, captureBuffer.getNumChannels()); ++ch)
                captureBuffer.copyFrom(ch, pos, workBuffer, ch, 0, toWrite);
            captureWritePos.store(pos + toWrite);
        }
    }

    // メトロノーム合成（再生中のみ）。鳴らすのは CLICK トラックが有効なときだけで、
    // カウントイン中も自動では鳴らさない（クリックが欲しいユーザーは CLICK トラックを作る）。
    if (metronomeEnabled.load() || clickEnvelope > 0.001)
    {
        // 現在位置のBPM（途中変更を考慮、ブロック単位で評価）。appCfg はブロック先頭で取得済み。
        double bpmHere = appCfg->bpmChanges.empty()
                         ? metronomeBpm.load()
                         : appCfg->bpmAtTime(posStart);
        double bps   = bpmHere / 60.0;
        int    beatsPerBar = juce::jmax(1, metronomeBeatsPerBar.load());
        float  vol   = metronomeVolume.load();
        float  pan   = metronomePan.load();
        int    sound = metronomeSound.load();
        bool   accent = metronomeAccent.load();
        double rateMul = juce::jmax(0.01f, metronomeRateMul.load());

        // 曲頭からの累積拍数（BPM変更があれば積分、無ければ単純積）
        double beatsAtBlockStart = appCfg->bpmChanges.empty()
                                   ? posStart * bps
                                   : appCfg->beatsAtTime(posStart);

        // レート変化 or BPM変更で累積ビートが巻き戻ったら再同期
        int subBeatIntAtStart = (int)std::floor(beatsAtBlockStart * rateMul);
        if (rateMul != clickLastRateMul || subBeatIntAtStart < clickLastBeatInt)
        {
            clickLastRateMul = rateMul;
            clickLastBeatInt = subBeatIntAtStart;
        }

        // パン: -1=左、0=中央、+1=右
        float lGain = (pan <= 0.0f) ? 1.0f : (1.0f - pan);
        float rGain = (pan >= 0.0f) ? 1.0f : (1.0f + pan);

        // 合成音はまずドライ (vol/pan 前) でスクラッチ ch0 に書き、CLICK トラックの INS
        // チェーン (EQ 等) を通してから vol/pan を掛けて出力へ加算する。これにより
        // 「メトロノームを EQ で削る」等が効く。チェーンは preparePlayback / aboutToStart で
        // 全トラック分 prepare 済み (Click も含む)。プラグインが無ければ従来どおりの直加算。
        clickSynthBuf.setSize(2, numSamples, false, false, true);
        clickSynthBuf.clear();
        float* cs = clickSynthBuf.getWritePointer(0);

        for (int i = 0; i < numSamples; ++i)
        {
            double pos = posStart + (double)i / currentSampleRate;
            double localBeats = beatsAtBlockStart + bps * (double)i / currentSampleRate;
            int beatInt = (int)std::floor(localBeats * rateMul);

            if (metronomeEnabled.load() && !clickSoloBlocked
                && beatInt > clickLastBeatInt && pos >= 0.0)
            {
                clickEnvelope    = 1.0;
                clickPhase       = 0.0;
                // このサブクリックが小節頭（実拍境界 + 小節頭）か判定
                double realBeatF = (double)beatInt / rateMul;
                int    realBeatI = (int)std::round(realBeatF);
                bool   onRealBeat = std::abs(realBeatF - (double)realBeatI) < 0.01;
                clickIsDownbeat  = onRealBeat &&
                                   (appCfg->meterChanges.empty()
                                    ? (realBeatI % beatsPerBar == 0)
                                    : appCfg->isDownbeatAtBeat(realBeatI));
                bool downHi      = accent && clickIsDownbeat;
                // 音色ごとの基本周波数
                switch (sound)
                {
                    case 0: clickFreq = downHi ? 1500.0 : 1000.0; break;  // Beep
                    case 1: clickFreq = downHi ? 2000.0 : 1500.0; break;  // Stick (ノイズ用に高め)
                    case 2: clickFreq = downHi ? 800.0  : 600.0;  break;  // Cowbell
                    case 3: clickFreq = downHi ? 600.0  : 400.0;  break;  // Wood
                    case 4: clickFreq = downHi ? 2200.0 : 1700.0; break;  // Tick
                    case 5: clickFreq = downHi ? 1200.0 : 900.0;  break;  // Bell
                    default: clickFreq = 1000.0;
                }
                clickLastBeatInt = beatInt;
            }

            if (clickEnvelope > 0.001)
            {
                float s = 0.0f;
                clickPhase += 2.0 * juce::MathConstants<double>::pi * clickFreq / currentSampleRate;
                static juce::Random rng;

                switch (sound)
                {
                    case 0: // Beep: サイン波
                        s = (float)(std::sin(clickPhase) * clickEnvelope);
                        break;
                    case 1: // Stick: ノイズ + 速い減衰
                        s = (rng.nextFloat() * 2.0f - 1.0f) * (float)clickEnvelope * 0.7f;
                        break;
                    case 2: // Cowbell: 矩形波
                        s = (std::sin(clickPhase) > 0 ? 1.0f : -1.0f) * (float)clickEnvelope * 0.5f;
                        break;
                    case 3: // Wood: 三角波 + ノイズ
                        s = (float)((std::asin(std::sin(clickPhase)) / juce::MathConstants<double>::halfPi)
                                    * clickEnvelope) * 0.5f
                            + (rng.nextFloat() * 2.0f - 1.0f) * (float)clickEnvelope * 0.2f;
                        break;
                    case 4: // Tick: 高めのサイン + 急峻な減衰
                        s = (float)(std::sin(clickPhase) * clickEnvelope * clickEnvelope);
                        break;
                    case 5: // Bell: サイン + 倍音
                    {
                        double s1 = std::sin(clickPhase);
                        double s2 = std::sin(clickPhase * 2.756);  // 鐘らしい不協倍音
                        s = (float)((s1 + s2 * 0.5) * clickEnvelope * 0.6);
                        break;
                    }
                    default:
                        s = (float)(std::sin(clickPhase) * clickEnvelope);
                }

                float strengthMul = accent ? (clickIsDownbeat ? 1.3f : 0.85f) : 1.0f;
                s *= strengthMul;            // vol / pan はチェーン通過後に適用する
                cs[i] = s;                   // ドライ (モノ) でスクラッチへ

                // エンベロープ減衰（音色ごとに異なる）
                double decay = (sound == 1 || sound == 4) ? 0.997 : 0.9985;  // Stick/Tick は速い減衰
                clickEnvelope *= decay;
                if (clickEnvelope < 0.001) clickEnvelope = 0.0;
            }
        }

        // CLICK トラックの INS チェーンを通す (EQ 等)。プラグインがあるときだけステレオ化して
        // 処理する (空チェーンは getActivePluginCountAtomic()==0 で素通り = 従来と同コスト)。
        // ライブモニタ同様 PDC はかけない (ルックアヘッド系は遅延が聞こえるが EQ は無遅延)。
        Track* clickTr = snap ? snap->clickTrack : nullptr;
        bool   chainStereo = false;
        if (clickTr != nullptr && clickTr->getPluginChain().getActivePluginCountAtomic() > 0)
        {
            clickSynthBuf.copyFrom(1, 0, clickSynthBuf, 0, 0, numSamples);  // dual-mono 化
            chainMidiScratch.clear();
            clickTr->getPluginChain().processBlock(clickSynthBuf, chainMidiScratch, &playHead);
            chainStereo = true;
        }

        // vol / pan を掛けて出力へ加算 (通常トラックと同じく fader/pan はチェーン後段)
        const float* outL = clickSynthBuf.getReadPointer(0);
        const float* outR = chainStereo ? clickSynthBuf.getReadPointer(1) : outL;
        if (numOutputChannels >= 2)
        {
            for (int i = 0; i < numSamples; ++i)
            {
                outputChannelData[0][i] += outL[i] * vol * lGain;
                outputChannelData[1][i] += outR[i] * vol * rGain;
            }
        }
        else if (numOutputChannels >= 1)
        {
            for (int i = 0; i < numSamples; ++i)
                outputChannelData[0][i] += outL[i] * vol;
        }
    }

    // input monitoring (mix input to output before recording check)
    // ドライ返し + モニターリバーブ (INS があれば返し音に FX も通す)。録音は下の
    // recCfg->targets で生入力を書く (FX を通さない別経路) ため、FX は焼き込まれない。
    mixInputMonitoring(inputChannelData, numInputChannels,
                       outputChannelData, numOutputChannels, numSamples, monChain,
                       monInputCh, monStereo, monPan, monGain);

    // アプリ音声取り込み (ブラウザ等): モニタ返し合算後・ミラー tap 前に加算する
    // (ヘッドホンと配信ミラーの両方に乗り、録音の生入力経路には乗らない)
    if (appCap != nullptr)
        mixAppCapture(*appCap, outputChannelData, numOutputChannels, numSamples);

    // 配信ミラー出力: 最終出力が確定した時点 (モニタ返し合算後・以降は録音/計測のみ) で複製する
    if (mirror != nullptr && numOutputChannels > 0 && outputChannelData[0] != nullptr)
        mirror->push(outputChannelData[0],
                     (numOutputChannels > 1) ? outputChannelData[1] : nullptr,
                     numSamples);

    // recording from input (録音設定はブロック先頭で取得した recCfg を使う)
    {
        // 通常録音: posStart >= recordingWriteFromSecs に達してから書き込む。
        // カウントイン/プリロール時は writeFrom が再生開始位置になっており、その区間も
        // 遡及的に録っておく (クリップは fileOffset 付きで配置され、左端を伸ばすと復元できる)
        if (!recCfg->targets.empty() && playing.load()
            && numInputChannels > 0
            && inputChannelData != nullptr
            && posStart >= recordingWriteFromSecs.load() - 1e-6)
        {
            // 最初に書き込んだブロックの位置 = ファイルのサンプル 0 のタイムライン位置。
            // クリップ配置がこれを真のファイル先頭に使う (writeFrom 仮定の登録遅れズレ対策)
            if (recFirstWritePos.load(std::memory_order_relaxed) < 0.0)
                recFirstWritePos.store(posStart);

            // 各ターゲットに自分の input ch から書き込む (複数マイク同時録音対応)
            for (auto& tgt : recCfg->targets)
            {
                if (tgt.writer == nullptr) continue;
                int chL = juce::jlimit(0, numInputChannels - 1, tgt.inputCh);
                if (inputChannelData[chL] == nullptr) chL = 0;

                // writer のチャンネル数 (= tgt.stereo ? 2 : 1) と同じ数の配列を必ず渡す。
                // stereo writer にモノ (1 要素) 配列を渡すと ThreadedWriter が data[1] を
                // 境界外参照してクラッシュする (ステレオトラックをモノ入力デバイス=
                // numInputChannels<2 で録るケース)。入力が 1ch しか無い時は L を R に複製する。
                if (tgt.stereo)
                {
                    int chR = chL;
                    if (numInputChannels >= 2)
                    {
                        chR = juce::jlimit(0, numInputChannels - 1, tgt.inputCh + 1);
                        if (inputChannelData[chR] == nullptr) chR = chL;
                    }
                    const float* stereoData[2] = { inputChannelData[chL], inputChannelData[chR] };
                    tgt.writer->write(stereoData, numSamples);
                }
                else
                {
                    const float* monoData[1] = { inputChannelData[chL] };
                    tgt.writer->write(monoData, numSamples);
                }

                if (tgt.liveBuffer != nullptr)
                    tgt.liveBuffer->pushSamples(inputChannelData[chL], numSamples);
            }
            // 全ターゲット同じ numSamples なので 1 度だけ加算 (ループ録音尺算出用)
            recordedSamples.fetch_add(numSamples);
        }

        // 遡及録音: 再生中ずっと裏で記録（recordingTarget の有無に関わらず）
        if (recCfg->retro != nullptr && playing.load()
            && numInputChannels > 0 && inputChannelData != nullptr)
        {
            // 遡及ファイルのサンプル 0 のタイムライン位置 (targets 側と同じマーカー)
            if (retroFirstWritePos.load(std::memory_order_relaxed) < 0.0)
                retroFirstWritePos.store(posStart);

            int chL = juce::jlimit(0, numInputChannels - 1, recCfg->retroInputCh);
            if (inputChannelData[chL] == nullptr) chL = 0;

            // stereo writer には常に 2ch 配列を渡す (targets と同じ理由・モノ入力は L を複製)
            if (recCfg->retroStereo)
            {
                int chR = chL;
                if (numInputChannels >= 2)
                {
                    chR = juce::jlimit(0, numInputChannels - 1, recCfg->retroInputCh + 1);
                    if (inputChannelData[chR] == nullptr) chR = chL;
                }
                const float* sd[2] = { inputChannelData[chL], inputChannelData[chR] };
                recCfg->retro->write(sd, numSamples);
            }
            else
            {
                const float* md[1] = { inputChannelData[chL] };
                recCfg->retro->write(md, numSamples);
            }

            // ライブ波形バッファにも積む（録音中の視覚フィードバック）
            if (recCfg->retroLiveBuf != nullptr)
                recCfg->retroLiveBuf->pushSamples(inputChannelData[chL], numSamples);
        }
    }

    // metering
    if (workBuffer.getNumChannels() >= 1)
        measureLevel(workBuffer.getReadPointer(0), numSamples,
                     peakL, peakHoldL, vuSmoothL, vuL, vuCoef);

    if (workBuffer.getNumChannels() >= 2)
        measureLevel(workBuffer.getReadPointer(1), numSamples,
                     peakR, peakHoldR, vuSmoothR, vuR, vuCoef);
    else
    {
        peakR.store(peakL.load());
        peakHoldR.store(peakHoldL.load());
        vuR.store(vuL.load());
    }

    // update position（ループ範囲でラップ）
    const double oldPos = currentPosition.load();
    double newPos = oldPos + (double)numSamples / currentSampleRate;
    if (loopActive.load())
    {
        double ls = loopStartSecs.load();
        double le = loopEndSecs.load();
        // ループ末尾を「下から跨いだ」時だけラップする。再生開始位置がループ末尾より
        // 後ろ (oldPos >= le) のときにラップすると、ループ外から再生したのにループ内へ
        // 引き戻されてしまう (再生位置が全然違う所から鳴るバグ)。oldPos < le を条件に
        // 加えることで、ループ外 (末尾より後ろ) から再生したら通常どおりその位置から鳴る。
        if (le > ls && oldPos < le && newPos >= le)
        {
            newPos = ls + std::fmod(newPos - ls, le - ls);
            // ループ継ぎ目のクリック防止: 次ブロック (loopStart の内容) へ向けて、
            // 直前出力 (loopEnd 付近) からデクリック・クロスフェードを張る。これをしないと
            // loopEnd の波形が非ゼロのとき毎周ループ点でクリックが出る (#M5)。
            declickHold[0] = declickLast[0];
            declickHold[1] = declickLast[1];
            declickXfadeRemain = kDeclickLen;
            // ループ録音中は各ターゲットのライブ波形バッファを巻き戻してループ毎に新規描画
            // (ブロック先頭で取得済みの recCfg を使う)
            for (auto& tgt : recCfg->targets)
                if (tgt.liveBuffer != nullptr)
                    tgt.liveBuffer->reset();
            // ループ内の途中から録音開始した場合、2 周目以降はループ頭から録る。
            // 書き込みゲート (posStart >= recordingWriteFromSecs) を開始位置のままにすると、
            // ラップ後に開始位置へ再到達するまで書き込みが止まり、2 周目以降のファイル
            // 内容が毎周欠けてテイクのスライスが累積的にずれる + ライブ波形 (録音バー)
            // が再生バーに追従しなくなる。パンチインミュート位置 (recordingStartSecs) も
            // ループ頭へ進め、歌い直す周回では既存クリップをループ全体でミュートする
            // (いずれも atomic store のみ・確保なし)
            if (isRecordingActive.load())
            {
                if (recordingStartSecs.load() > ls)     recordingStartSecs.store(ls);
                if (recordingWriteFromSecs.load() > ls) recordingWriteFromSecs.store(ls);
            }
            loopWrapCount.fetch_add(1);
        }
    }
    currentPosition.store(newPos);
}

void AudioEngine::measureLevel(const float* data, int numSamples,
                                std::atomic<float>& peak,
                                std::atomic<float>& peakHold,
                                float& vuSmooth,
                                std::atomic<float>& vu,
                                float vuCoef)
{
    float mag = 0.0f;
    for (int i = 0; i < numSamples; ++i)
        mag = juce::jmax(mag, std::abs(data[i]));

    const float db = juce::Decibels::gainToDecibels(mag, -96.0f);
    peak.store(db);

    if (db > peakHold.load())
        peakHold.store(db);

    // VU 1次LPF: 係数は呼び出し側で SR/blockSize から算出 (時定数 300ms 一定)
    vuSmooth = vuSmooth * vuCoef + mag * (1.0f - vuCoef);
    vu.store(juce::Decibels::gainToDecibels(vuSmooth, -96.0f));
}
