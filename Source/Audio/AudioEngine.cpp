// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

#include "AudioEngine.h"
#include "../Tracks/TrackManager.h"
#include "../Tracks/MidiClip.h"
#include "../VST/PluginChain.h"
#include "../MIDI/InternalSynth.h"
#include "AudioDeviceSettings.h"
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

void AudioEngine::previewMidiNote(int trackIdx, int note, float velocity, bool isOn)
{
    const juce::ScopedLock sl(previewMidiLock);
    pendingPreviewMidi.push_back({ trackIdx, note, velocity, isOn });
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

void AudioEngine::setMonitorChain(PluginChain* chain, int inputCh, bool stereo)
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
        next->targets.push_back({ writer, liveBuffer, inputCh, stereo });
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
    deviceManager.removeAudioCallback(this);
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
            const auto key = clip->getFile().getFullPathName();
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
    readerPool.clear();
    for (auto& pc : snap->clips)
        if (pc.reader) readerPool.emplace(pc.file.getFullPathName(), pc.reader);

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
    if (currentSampleRate > 0.0 && currentBufferSize > 0)
    {
        // マスターチェーンは常に準備（トラック 0 個でも使う可能性がある）
        masterChain->prepareToPlay(currentSampleRate, currentBufferSize);

        // ── MIDI 再生キャッシュ構築 ──
        for (int ti = 0; ti < tm.getTrackCount(); ++ti)
        {
            auto* tr = tm.getTrack(ti);
            if (!tr || !tr->isMidiTrack() || tr->getMidiClipCount() == 0) continue;
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
                tr->getPluginChain().prepareToPlay(currentSampleRate, currentBufferSize);
                maxIdx = juce::jmax(maxIdx, ti);
            }

            // トラック単位のドライバッファ (index = trackIdx)
            snap->trackBuffers.resize((size_t)(maxIdx + 1));
            for (auto& tb : snap->trackBuffers)
                tb.setSize(2, currentBufferSize, false, false, true);

            // ── PDC: 全トラックの最大プラグイン遅延を求めて各トラックの補正量を確定 ──
            int newMaxLat = 0;
            std::vector<int> trackLats((size_t)(maxIdx + 1), 0);
            for (int ti = 0; ti <= maxIdx; ++ti)
            {
                auto* tr = (ti < nTracks) ? tm.getTrack(ti) : nullptr;
                if (!tr) continue;
                const int lat = tr->getPluginChain().getTotalLatencySamples();
                trackLats[(size_t)ti] = lat;
                newMaxLat = juce::jmax(newMaxLat, lat);
            }
            maxPluginLatency = newMaxLat;

            snap->trackDelays.resize((size_t)(maxIdx + 1));
            // バッファサイズ: 最大遅延 + 1ブロック分の余裕、最低でも 1
            const int delayBufLen = juce::jmax(1, newMaxLat + currentBufferSize);
            for (int ti = 0; ti <= maxIdx; ++ti)
            {
                auto& d = snap->trackDelays[(size_t)ti];
                d.delaySamples = newMaxLat - trackLats[(size_t)ti];
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
    auto& d = delays[(size_t)trackIdx];
    if (d.delaySamples == 0) return;            // 遅延不要 = 最遅トラック自身
    const int bufLen = d.buf.getNumSamples();
    if (bufLen <= 0) return;

    // Step 1: trackBuf → 循環バッファへ書き込み（ラップ分割）
    int wp = d.writePos;
    int firstChunk  = juce::jmin(numSamples, bufLen - wp);
    int secondChunk = numSamples - firstChunk;
    for (int ch = 0; ch < juce::jmin(2, trackBuf.getNumChannels()); ++ch)
    {
        d.buf.copyFrom(ch, wp, trackBuf, ch, 0, firstChunk);
        if (secondChunk > 0)
            d.buf.copyFrom(ch, 0,  trackBuf, ch, firstChunk, secondChunk);
    }

    // Step 2: 循環バッファから delaySamples 遅れた位置を読み出して trackBuf 上書き
    int rp = (wp - d.delaySamples + bufLen) % bufLen;
    firstChunk  = juce::jmin(numSamples, bufLen - rp);
    secondChunk = numSamples - firstChunk;
    for (int ch = 0; ch < juce::jmin(2, trackBuf.getNumChannels()); ++ch)
    {
        trackBuf.copyFrom(ch, 0,          d.buf, ch, rp, firstChunk);
        if (secondChunk > 0)
            trackBuf.copyFrom(ch, firstChunk, d.buf, ch, 0, secondChunk);
    }

    d.writePos = (wp + numSamples) % bufLen;
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
                              double posStart, int numSamples, bool preFader)
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
    clipBuffer.setSize(juce::jmin(2, (int)pc.reader->numChannels), nRead, false, false, true);
    clipBuffer.clear();
    pc.reader->read(&clipBuffer, 0, nRead, fileSample, true, true);

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
        const int numCh   = clipBuffer.getNumChannels();
        const double invSR = 1.0 / currentSampleRate;

        // チャンネル毎の生ポインタ（setSample/getSample の bounds check を回避）
        float* writePtrs[8] = {};
        const int chCount = juce::jmin(numCh, 8);
        for (int ch = 0; ch < chCount; ++ch)
            writePtrs[ch] = clipBuffer.getWritePointer(ch);

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

        // clipBuffer 自身にゲインランプを適用してミックス
        clipBuffer.applyGainRamp(0, nRead, baseGain * envGainStart, baseGain * envGainEnd);

        const int numOutCh = output.getNumChannels();
        for (int ch = 0; ch < numOutCh; ++ch)
        {
            int srcCh = juce::jmin(ch, clipBuffer.getNumChannels() - 1);
            float chPan = (numOutCh >= 2) ? (ch == 0 ? panL : panR) : 1.0f;
            output.addFrom(ch, bufOffset, clipBuffer, srcCh, 0, nRead, chPan);
        }
    }
    else
    {
        const int numOutCh = output.getNumChannels();
        for (int ch = 0; ch < numOutCh; ++ch)
        {
            int srcCh = juce::jmin(ch, clipBuffer.getNumChannels() - 1);
            float chPan = (numOutCh >= 2) ? (ch == 0 ? panL : panR) : 1.0f;
            output.addFrom(ch, bufOffset, clipBuffer, srcCh, 0, nRead, baseGain * chPan);
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
        deviceRoundTripSecs.store((device->getInputLatencyInSamples()
                                   + device->getOutputLatencyInSamples())
                                  / currentSampleRate);
    mixer.prepareToPlay(currentBufferSize, currentSampleRate);
    workBuffer.setSize(2, currentBufferSize);
    // 停止中シンセプレビュー用に十分なサイズで先に確保 (audio thread での realloc を回避)
    const int previewCh = juce::jmax(2, device->getActiveOutputChannels().countNumberOfSetBits());
    previewBuf.setSize(previewCh, currentBufferSize);
    // クリップ読み出し用バッファも先に確保する。これをしないと mono トラック群の後に
    // 最初の stereo クリップを再生する瞬間などに renderClip 内の setSize が
    // オーディオスレッドでヒープ確保し、一度きりのドロップアウトを招きうる (#M3)。
    // 以後の per-block setSize は avoidReallocating=true なので容量内に収まり再確保しない。
    clipBuffer.setSize(2, currentBufferSize, false, false, true);

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

    // audio callback のアクティブトラック収集スクラッチを事前確保 (毎ブロックの再確保回避)。
    // clear() で長さ 0 に戻しても容量は保たれるため、以後 push_back で再確保が起きない。
    activeTrackIdxScratch.reserve(64);
    activeTracksScratch.reserve(64);

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
    }
}

void AudioEngine::audioDeviceStopped()
{
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

void AudioEngine::renderOfflineRange(double startSec, double endSec,
                                      juce::AudioBuffer<float>& outBuffer,
                                      std::function<void(double)> progress,
                                      const std::vector<int>& includeTracks,
                                      bool preFader)
{
    if (endSec <= startSec || currentSampleRate <= 0.0) return;

    const double sr        = currentSampleRate;
    const int    totalSamp = (int)std::round((endSec - startSec) * sr);
    const int    blockSize = 1024;

    // 書き出し中もプラグインへ再生位置を供給する (テンポ同期プラグイン等が正しく動くように)。
    // audio thread と競合しないよう、メンバ playHead ではなくローカルインスタンスを使う。
    EnginePlayHead exportHead;
    std::shared_ptr<const AppSettings> exportCfg;
    { const juce::SpinLock::ScopedLockType l(appSettingsLock); exportCfg = activeAppSettings; }

    if (outBuffer.getNumSamples() < totalSamp || outBuffer.getNumChannels() < 2)
        outBuffer.setSize(2, totalSamp, false, true, true);
    outBuffer.clear();

    juce::AudioBuffer<float> blockBuf(2, blockSize);

    int written = 0;
    while (written < totalSamp)
    {
        const int n = juce::jmin(blockSize, totalSamp - written);
        blockBuf.setSize(2, n, false, false, true);
        blockBuf.clear();

        const double posStart = startSec + (double)written / sr;
        if (exportCfg) fillPlayHead(exportHead, posStart, sr, *exportCfg,
                                    /*playing*/ true, /*recording*/ false,
                                    /*looping*/ false, 0.0, 0.0);

        {
            // 再生スナップショットを per-block で取得する (旧 playbackLock 相当)。per-block にする
            // ことで clearPlayback() の drain がこのレンダリングを 1 ブロック分だけ待てば解放される。
            std::shared_ptr<PlaybackSnapshot> snap;
            { const juce::SpinLock::ScopedLockType l(snapshotLock); snap = activeSnapshot; }

            // 明示的にトラックを指定された場合は Solo/Mute を無視
            const bool explicitFilter = !includeTracks.empty();

            // Solo 判定（明示フィルタ無しのときのみ、MIDI トラックも含める）。
            // clipTracks は dedup 済みトラック一覧 (Click 除外)
            bool anySolo = false;
            if (!explicitFilter)
            {
                for (auto& [ti, trk] : snap->clipTracks)
                    if (trk && trk->isSoloed()) { anySolo = true; break; }
                if (!anySolo)
                    for (auto& mp : snap->midi)
                        if (mp.track && mp.track->isSoloed()) { anySolo = true; break; }
            }

            // アクティブトラック収集 (clipTracks ベースで O(トラック数)。全 clips 走査しない)
            const std::unordered_set<int> includeSet(includeTracks.begin(), includeTracks.end());
            std::vector<int>    activeIdx;
            std::vector<Track*> activeTracks;
            for (auto& [ti, trk] : snap->clipTracks)
            {
                if (trk == nullptr) continue;
                if (explicitFilter)
                {
                    if (includeSet.find(ti) == includeSet.end()) continue;
                }
                else
                {
                    if (trk->isMuted()) continue;
                    if (anySolo && !trk->isSoloed()) continue;
                }
                activeIdx.push_back(ti);
                activeTracks.push_back(trk);
            }

            juce::AudioBuffer<float> trackBuf(2, n);
            for (size_t ai = 0; ai < activeIdx.size(); ++ai)
            {
                trackBuf.clear();
                const int tidx = activeIdx[ai];
                if (tidx < (int)snap->clipsByTrack.size())
                    for (int ci : snap->clipsByTrack[(size_t)tidx])
                        renderClip(snap->clips[(size_t)ci], trackBuf, posStart, n, /*preFader*/ true);

                auto* track = activeTracks[ai];
                if (track && track->getPluginChain().getNumPlugins() > 0)
                {
                    juce::MidiBuffer midi;
                    track->getPluginChain().processBlock(trackBuf, midi, &exportHead);
                }

                if (preFader)
                {
                    // Pre-Fader: トラック Vol/Pan/マスター無視
                    blockBuf.addFrom(0, 0, trackBuf, 0, 0, n, 1.0f);
                    blockBuf.addFrom(1, 0, trackBuf, 1, 0, n, 1.0f);
                }
                else
                {
                    const float vol  = track ? juce::Decibels::decibelsToGain(track->getVolume()) : 1.0f;
                    const float pan  = track ? track->getPan() : 0.0f;
                    const float panL = (pan <= 0.0f) ? 1.0f : (1.0f - pan);
                    const float panR = (pan >= 0.0f) ? 1.0f : (1.0f + pan);
                    blockBuf.addFrom(0, 0, trackBuf, 0, 0, n, vol * panL);
                    blockBuf.addFrom(1, 0, trackBuf, 1, 0, n, vol * panR);
                }
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

        for (int ch = 0; ch < 2; ++ch)
            outBuffer.copyFrom(ch, written, blockBuf, ch, 0, n);

        written += n;
        if (progress) progress((double)written / (double)totalSamp);
    }
}

void AudioEngine::mixInputMonitoring(const float* const* inputChannelData, int numInputChannels,
                                     float* const* outputChannelData, int numOutputChannels,
                                     int numSamples, PluginChain* monChain,
                                     int monInputCh, bool monStereo)
{
    const bool monitoring = inputMonitoringActive.load()
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

        // 処理済み (FX 後) の返しを出力へ加算 (= ドライ返しの置き換え)。
        for (int ch = 0; ch < numOutputChannels; ++ch)
            juce::FloatVectorOperations::add(outputChannelData[ch],
                                             monitorChainBuf.getReadPointer(juce::jmin(ch, 1)),
                                             numSamples);

        // リバーブ送りは FX 処理後の信号から (再生時の post-fader/post-FX 送りと同じセマンティクス)。
        if (rs > 0.0001f)
        {
            monitorReverbBuf.setSize(2, numSamples, false, false, true);
            monitorReverbBuf.copyFrom(0, 0, monitorChainBuf.getReadPointer(0), numSamples, rs);
            monitorReverbBuf.copyFrom(1, 0, monitorChainBuf.getReadPointer(1), numSamples, rs);
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
    // mono は srcL を L/R 両方へ (センター)、stereo は L=srcL / R=srcR。
    if (numOutputChannels >= 1 && inL != nullptr)
        juce::FloatVectorOperations::add(outputChannelData[0], inL, numSamples);
    if (numOutputChannels >= 2 && inR != nullptr)
        juce::FloatVectorOperations::add(outputChannelData[1], inR, numSamples);

    // ── モニターリバーブ: 返し音にだけウェットを足す (録音ファイルには焼き込まない) ──
    // ドライ返しと同じ入力 (srcL / srcR) を送り、Rev 量でスケールしてプレートで処理。
    // 出力にだけ加算するので、下流の録音 (生入力書き込み) には一切影響しない。
    if (rs > 0.0001f)
    {
        monitorReverbBuf.setSize(2, numSamples, false, false, true);
        if (inL != nullptr) monitorReverbBuf.copyFrom(0, 0, inL, numSamples, rs);
        else                monitorReverbBuf.clear(0, 0, numSamples);
        if (inR != nullptr) monitorReverbBuf.copyFrom(1, 0, inR, numSamples, rs);
        else                monitorReverbBuf.clear(1, 0, numSamples);

        monitorReverbBus.processStereo(monitorReverbBuf.getWritePointer(0),
                                       monitorReverbBuf.getWritePointer(1), numSamples);
        monitorReverbDirty = true;

        for (int ch = 0; ch < juce::jmin(2, numOutputChannels); ++ch)
            juce::FloatVectorOperations::add(outputChannelData[ch],
                                             monitorReverbBuf.getReadPointer(ch), numSamples);
    }
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
    PluginChain* const monChain    = (monCfg ? monCfg->chain : nullptr);
    const int          monInputCh  = (monCfg ? monCfg->inputCh : 0);
    const bool         monStereo   = (monCfg ? monCfg->stereo  : false);
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

    if (!playing.load())
    {
        // 再生が止まった最初のブロックでリバーブのテールをリセットする。
        // (凍結保持された前回のテールが次の再生開始で漏れ出すのを防ぐ)
        if (reverbBusDirty) { masterReverbBus.reset(); reverbBusDirty = false; }

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
        }

        // ── 停止時はメータを徐々に減衰させて 0 に落とす ──
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

        // 実トラック数までに制限し、かつ完全無音 (peak/VU とも底) のトラックは log10 をスキップ。
        // これによりアイドル時の不要な log10 呼び出しを実トラック数相当まで減らす。
        const int meterN = juce::jmin(meterTrackCount.load(), kMaxTracksMeters);
        for (int t = 0; t < meterN; ++t)
        {
            const bool peakSilent = (trackOutPeakL[t].load() <= -96.0f)
                                 && (trackOutPeakR[t].load() <= -96.0f);
            const bool vuSilent   = (trackOutVUSmoothL[t] < 1.0e-7f)
                                 && (trackOutVUSmoothR[t] < 1.0e-7f);
            if (peakSilent && vuSilent) continue;   // 完全無音は減衰処理不要

            decayPeakDb(trackOutPeakL[t]);
            decayPeakDb(trackOutPeakR[t]);
            trackOutVUSmoothL[t] *= vuCoef;
            trackOutVUSmoothR[t] *= vuCoef;
            trackOutVUL[t].store(juce::Decibels::gainToDecibels(trackOutVUSmoothL[t], -96.0f));
            trackOutVUR[t].store(juce::Decibels::gainToDecibels(trackOutVUSmoothR[t], -96.0f));
        }

        // 停止中も入力モニタリングは通す (ドライ返し + モニターリバーブ、INS があれば FX も)
        mixInputMonitoring(inputChannelData, numInputChannels,
                           outputChannelData, numOutputChannels, numSamples, monChain,
                           monInputCh, monStereo);

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

    double posStart = currentPosition.load();

    // プラグインへ供給する再生位置情報をブロック先頭で 1 回更新する (Melodyne 等の transport
    // 同期用)。トラック/MIDI/マスターの全チェーンがこの playHead を共有する。
    fillPlayHead(playHead, posStart, currentSampleRate, *appCfg,
                 /*playing*/ true, isRecordingActive.load(),
                 loopActive.load(), loopStartSecs.load(), loopEndSecs.load());

    // 簡易リバーブ送りバスのフラグ
    bool anyReverbSend = false;

    {
        // ライブ Solo 判定（オーディオクリップだけでなく MIDI トラックも対象に含める）。
        // clipTracks は dedup 済みトラック一覧なので O(トラック数) で済む (全 clips 走査しない)。
        bool anySolo = false;
        for (auto& [tidx, trk] : snap->clipTracks)
            if (trk && trk->isSoloed()) { anySolo = true; break; }
        if (!anySolo)
            for (auto& mp : snap->midi)
                if (mp.track && mp.track->isSoloed()) { anySolo = true; break; }

        // 各トラックの「有効性」を判定し、使うトラックのリストを作る。
        // スクラッチを再利用 (clear で長さ 0 に戻すと容量は保たれヒープ確保が起きない)。
        auto& activeTrackIdx = activeTrackIdxScratch;
        auto& activeTracks   = activeTracksScratch;
        activeTrackIdx.clear();
        activeTracks.clear();
        for (auto& [tidx, trk] : snap->clipTracks)
        {
            if (trk == nullptr) continue;          // clipTracks は Click 除外・dedup 済み
            if (trk->isMuted()) continue;
            if (anySolo && !trk->isSoloed()) continue;
            activeTrackIdx.push_back(tidx);
            activeTracks.push_back(trk);
        }

        // 簡易リバーブ送りバスの clear は遅延実行: send > 0 のトラックが現れた最初の時だけ。
        // anyReverbSend が false の間は reverbSendBuf を READ しない (processStereo はガード済み) ため安全。
        reverbSendBuf.setSize(2, numSamples, false, false, true);
        bool reverbBufCleared = false;

        // トラック単位でドライ描画 → プラグインチェーン通過 → フェーダー/パン → マスターへ加算
        if (!activeTrackIdx.empty() && (int)snap->trackBuffers.size() > 0)
        {
            for (size_t ai = 0; ai < activeTrackIdx.size(); ++ai)
            {
                const int tidx = activeTrackIdx[ai];
                if (tidx < 0 || tidx >= (int)snap->trackBuffers.size()) continue;
                auto& trackBuf = snap->trackBuffers[(size_t)tidx];
                // 重要: プラグインが見るバッファサイズと実際の numSamples を合わせる。
                // スナップショット構築時に blockSize 容量を確保済みなので allocation は起きない。
                // サイズ不一致だとプラグイン内部状態と齟齬して AudioUnitRender が落ちる事例あり。
                trackBuf.setSize(2, numSamples, false, false, true);
                trackBuf.clear();

                // ドライ描画（Pre-Fader 相当: トラック Vol/Pan は後で適用）。
                // Mute/Solo/Click はトラック単位で判定済み (activeTrackIdx 構築時) なので
                // クリップ毎の再判定は不要。clipsByTrack で自トラックのクリップだけを直接引く。
                if (tidx < (int)snap->clipsByTrack.size())
                    for (int ci : snap->clipsByTrack[(size_t)tidx])
                        renderClip(snap->clips[(size_t)ci], trackBuf, posStart, numSamples, /*preFader*/ true);

                // プラグインチェーン。ロックを取らずに処理対象有無を判定する。
                auto* track = activeTracks[ai];
                // 二重処理ガード: このトラックが入力モニターの FX 対象なら、同じチェーン
                // インスタンスを mixInputMonitoring が生入力で叩く。再生クリップでも叩くと
                // 1 ブロックで 2 回処理してプラグイン内部状態 (ディレイ/包絡) が壊れるため、
                // 再生側ではチェーンをスキップする (このトラックの旧クリップは dry で鳴る)。
                const bool isMonTarget = monActive && track != nullptr
                                         && &track->getPluginChain() == monChain;
                if (track && !isMonTarget && track->getPluginChain().getActivePluginCountAtomic() > 0)
                {
                    chainMidiScratch.clear();
                    track->getPluginChain().processBlock(trackBuf, chainMidiScratch, &playHead);
                }

                // PDC: 自分より遅いトラックに合わせて trackBuf を遅延させる
                applyTrackDelay(snap->trackDelays, tidx, trackBuf, numSamples);

                // トラック Vol / Pan を先に算出（メータをポストフェーダーにするため）
                const float vol  = track ? juce::Decibels::decibelsToGain(track->getVolume()) : 1.0f;
                const float pan  = track ? track->getPan() : 0.0f;
                const float panL = (pan <= 0.0f) ? 1.0f : (1.0f - pan);
                const float panR = (pan >= 0.0f) ? 1.0f : (1.0f + pan);
                const float gL = vol * panL;
                const float gR = vol * panR;

                // トラック出力メータ（Vol/Pan 適用後 = ポストフェーダー。フェーダーに追従する）
                if (tidx >= 0 && tidx < kMaxTracksMeters)
                    measureStereoBuf(trackBuf, numSamples,
                                     trackOutPeakL[tidx], trackOutPeakR[tidx],
                                     trackOutVUSmoothL[tidx], trackOutVUSmoothR[tidx],
                                     trackOutVUL[tidx], trackOutVUR[tidx],
                                     vuCoef, gL, gR);

                // master に加算
                workBuffer.addFrom(0, 0, trackBuf, 0, 0, numSamples, gL);
                if (workBuffer.getNumChannels() >= 2 && trackBuf.getNumChannels() >= 2)
                    workBuffer.addFrom(1, 0, trackBuf, 1, 0, numSamples, gR);

                // 簡易リバーブ送り (post-fader / post-pan)
                const float rs = track ? track->getReverbSend() : 0.0f;
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
            if (mp.track->isMuted()) continue;
            if (anySolo && !mp.track->isSoloed()) continue;
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
            // Pitch Bend / Channel Pressure をブロック先頭 (sample 0) で再送する。
            // 同種類イベントが複数あれば「最後の値」だけを採用するため、いったん集めて重複排除する。
            if (needsStateRefresh)
            {
                // channel ごとに: 最後の PC、各 CC の最終値、最後の Pitch Bend、最後の Channel Pressure
                std::array<int, 16> lastPC;            lastPC.fill(-1);
                std::array<int, 16> lastPitchBend;    lastPitchBend.fill(-1);
                std::array<int, 16> lastChanPressure; lastChanPressure.fill(-1);
                // CC は (channel * 128 + ccNum) でキー化
                std::array<int, 16 * 128> lastCC;     lastCC.fill(-1);

                for (const auto& m : mp.events)
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

                // sample 0 に注入: CC → PC の順だと PC で音色が変わってから CC が効く
                // 通常順: Bank Select (CC0/32) → PC → 他の CC → PB
                // ここでは安全のため CC 全部 → PC → PB の順に送る
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
                measureStereoBuf(trackBuf, numSamples,
                                 trackOutPeakL[mp.trackIdx], trackOutPeakR[mp.trackIdx],
                                 trackOutVUSmoothL[mp.trackIdx], trackOutVUSmoothR[mp.trackIdx],
                                 trackOutVUL[mp.trackIdx], trackOutVUR[mp.trackIdx],
                                 vuCoef, gL, gR);

            workBuffer.addFrom(0, 0, trackBuf, 0, 0, numSamples, gL);
            if (workBuffer.getNumChannels() >= 2 && trackBuf.getNumChannels() >= 2)
                workBuffer.addFrom(1, 0, trackBuf, 1, 0, numSamples, gR);

            const float rs = mp.track->getReverbSend();
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

            if (metronomeEnabled.load() && beatInt > clickLastBeatInt && pos >= 0.0)
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
                       monInputCh, monStereo);

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
            // 各ターゲットに自分の input ch から書き込む (複数マイク同時録音対応)
            for (auto& tgt : recCfg->targets)
            {
                if (tgt.writer == nullptr) continue;
                int chL = juce::jlimit(0, numInputChannels - 1, tgt.inputCh);
                if (inputChannelData[chL] == nullptr) chL = 0;

                if (tgt.stereo && numInputChannels >= 2)
                {
                    int chR = juce::jlimit(0, numInputChannels - 1, tgt.inputCh + 1);
                    if (inputChannelData[chR] == nullptr) chR = chL;
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
            int chL = juce::jlimit(0, numInputChannels - 1, recCfg->retroInputCh);
            if (inputChannelData[chL] == nullptr) chL = 0;

            if (recCfg->retroStereo && numInputChannels >= 2)
            {
                int chR = juce::jlimit(0, numInputChannels - 1, recCfg->retroInputCh + 1);
                if (inputChannelData[chR] == nullptr) chR = chL;
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
    double newPos = currentPosition.load() + (double)numSamples / currentSampleRate;
    if (loopActive.load())
    {
        double ls = loopStartSecs.load();
        double le = loopEndSecs.load();
        if (le > ls && newPos >= le)
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
