// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

#pragma once
#include <JuceHeader.h>
#include <unordered_map>
#include "../Recording/LiveRecordingBuffer.h"
#include "../AppSettings.h"
#include "../Tracks/AudioClip.h"
#include "EnginePlayHead.h"
#include "AudioWorkerPool.h"

class TrackManager;  // 前方宣言
class Track;
class PluginChain;
class InternalSynth;
class FileStreamVoice;

class AudioEngine : public juce::AudioIODeviceCallback
{
public:
    AudioEngine();
    ~AudioEngine() override;

    void initialise();
    void shutdown();

    juce::AudioDeviceManager& getDeviceManager() { return deviceManager; }
    juce::AudioFormatManager& getFormatManager()  { return formatManager; }
    juce::MixerAudioSource&   getMixer()          { return mixer; }

    // Transport
    void play();
    void stop();
    void rewind();
    void setPosition(double seconds) { currentPosition.store(seconds); }

    // クリップ再生準備（play() の前に呼ぶ）
    void preparePlayback(TrackManager& tm);
    void clearPlayback();
    // appSettings は UI スレッド読み出し用メンバ (preparePlayback) と、audio スレッドがメトロノーム
    // 区間で読む snapshot (bpmChanges/meterChanges の vector を含むため) の両方を更新する。実装は .cpp。
    void setAppSettings(const AppSettings& s);

    // PlaybackClip の生ポインタが指す AudioClip/Track を破棄する編集を行った後に呼ぶ。
    // 再生中なら即座に rebuild、停止中なら次の play() まで遅延 (音切れ防止)。
    void invalidatePlayback();

    // 破棄系編集が取り除いた AudioClip の遅延破棄を依頼する (message thread)。すぐには破棄せず、
    // 次に公開するスナップショットへ載せ、それを参照していた旧スナップショットが audio に手放され
    // 回収される時に解放する。これにより再生中編集でも他トラックを止めずに済む (UAF も防ぐ)。
    void deferClipDestruction(std::vector<std::unique_ptr<AudioClip>>&& clips);

    // オフライン書き出し: [startSec, endSec) を outBuffer (2ch) に描画する
    // ・呼び出し前に preparePlayback でクリップが用意されている必要がある
    // ・現在のエンジン SR で描画される（出力 SR を変えたい場合は呼び出し側でリサンプル）
    // ・進捗 0..1 を progress に通知
    // ・includeTracks が空でない場合、その index に含まれるトラックのみ描画
    //   （Solo/Mute は無視。書き出しダイアログで明示的に選んだ前提）
    // ・includeTracks が空の場合、Solo/Mute を考慮した通常のミックスダウン
    // ・CLICK トラック (メトロノーム合成): includeClick=true なら混ぜる (呼び出し側が
    //   「鳴っている状態か = 非ミュート + ソロ規則」を判定して渡す。2 ミックスは明示
    //   トラックリストを渡すため、この明示フラグが唯一の経路)。includeTracks が空の
    //   通常ミックスダウンでは非ミュート + ソロ規則で自動判定する
    void renderOfflineRange(double startSec, double endSec,
                            juce::AudioBuffer<float>& outBuffer,
                            std::function<void(double)> progress = {},
                            const std::vector<int>& includeTracks = {},
                            bool preFader = false,
                            bool includeClick = false);

    // ── 実時間レンダリング (Realtime capture) ──
    // オーディオデバイスのコールバック経由で実時間にミックス出力を録る。
    // 通常の再生と並行してキャプチャされ、書き出し完了までは音もスピーカに流れる。
    // VST プラグイン等オフライン処理に対応しないエフェクト向け。
    void beginRealtimeCapture(int totalSamples);
    void endRealtimeCapture();
    int  getRealtimeCaptureWritePos() const { return captureWritePos.load(); }
    int  getRealtimeCaptureTotal()    const { return captureTotalSamples.load(); }
    void copyRealtimeCaptureTo(juce::AudioBuffer<float>& dst) const;

    // パンチイン録音中: 録音開始位置以降の古いクリップをミュート
    // startSecs     = パンチイン位置 (既存 Lane 0 クリップをミュートし始める位置)
    // writeFromSecs = ファイルへの書き込み開始位置。カウントイン/プリロール区間を遡及的に
    //                 録っておくため startSecs より手前を渡せる (負値 = startSecs と同じ)。
    //                 ブレスが切れた時にクリップ左端を伸ばして復元できるようにする
    void setRecordingActive(bool active, double startSecs = 0.0, double writeFromSecs = -1.0)
    {
        isRecordingActive.store(active);
        recordingStartSecs.store(startSecs);
        recordingWriteFromSecs.store(writeFromSecs >= 0.0 ? writeFromSecs : startSecs);
        if (active) recordedSamples.store(0);
    }

    // 直前に setRecordingActive(true) されてから recordingTarget に
    // 書き込まれたサンプル数。ループ録音の尺をサンプルベースで算出するために使う。
    juce::int64 getRecordedSampleCount() const { return recordedSamples.load(); }

    // ── 録音ファイルの実書き込み開始位置 (ファイルのサンプル 0 のタイムライン位置) ──
    // 書き込みは「ターゲット登録済み + posStart >= writeFrom」で始まるが、登録は play() の
    // 後に message thread が行うため、再生開始〜登録完了の 0〜数ブロックは書き込まれない。
    // 配置側が「ファイル先頭 = writeFrom」と仮定すると、この取りこぼし分だけ fileOffset の
    // トリムが過剰になり内容が手前へずれる (テイクごとにランダムなブロック粒度のズレ)。
    // audio thread が新しい録音 config で最初に書き込んだブロックの posStart を記録するので、
    // クリップ配置はこちらを真のファイル先頭として使う (一度も書き込みが無ければ fallback)。
    double getRecordingFirstWritePosSecs(double fallback) const
    {
        const double v = recFirstWritePos.load();
        return v >= 0.0 ? v : fallback;
    }
    // 遡及録音側の同マーカー (retro writer はゲート無しで登録直後から書く)
    double getRetroFirstWritePosSecs(double fallback) const
    {
        const double v = retroFirstWritePos.load();
        return v >= 0.0 ? v : fallback;
    }

    // ── 録音レイテンシ補正 ──
    // 録音は「出力レイテンシ分遅れて聞いた再生」に合わせて歌った声が「入力レイテンシ分
    // 遅れて」コールバックに届くため、入出力レイテンシの合計 (round trip) だけ遅れて
    // ファイルに書かれる。録音クリップ配置時にこの量だけ手前へずらして補正する。
    // デバイス報告の入出力レイテンシ合計 (秒)。audioDeviceAboutToStart で更新。
    double getDeviceRoundTripLatencySecs() const { return deviceRoundTripSecs.load(); }
    // 自動補正 ON/OFF + 手動追加オフセット (ms, +で手前へ)。アプリ全体設定から反映。
    void setRecordingLatencyComp(bool autoComp, double manualMs)
    {
        recLatencyAutoComp.store(autoComp);
        recLatencyManualMs.store(manualMs);
    }
    // 録音クリップを手前へずらす補正量 (秒) = 自動分 (デバイス報告値) + 手動分
    double getRecordingLatencyCompSecs() const
    {
        const double dev = recLatencyAutoComp.load() ? deviceRoundTripSecs.load() : 0.0;
        return dev + recLatencyManualMs.load() * 0.001;
    }

    // 遡及録音: 再生中の入力を専用ライターに書き出す (実装は .cpp / 録音 config スナップショット)。
    void setRetrospectiveTarget(juce::AudioFormatWriter::ThreadedWriter* writer,
                                LiveRecordingBuffer* liveBuf = nullptr,
                                int inputCh = 0, bool stereo = false);
    // 遡及録音中、ライブ波形のバッファだけを後から差し替え／クリア
    void setRetrospectiveLiveBuffer(LiveRecordingBuffer* liveBuf);

    // ループ範囲（再生中、loopEnd に到達したら loopStart に戻る）
    void setLoopRange(double startSecs, double endSecs, bool active)
    {
        loopStartSecs.store(startSecs);
        loopEndSecs.store(endSecs);
        loopActive.store(active);
    }

    // メトロノーム
    void setMetronomeEnabled(bool b)        { metronomeEnabled.store(b); }
    void setMetronomeBpm(double b)          { metronomeBpm.store(b); }
    void setMetronomeVolume(float v)        { metronomeVolume.store(v); }
    void setMetronomePan(float p)           { metronomePan.store(p); }
    void setMetronomeSound(int s)           { metronomeSound.store(s); }
    void setMetronomeBeatsPerBar(int n)     { metronomeBeatsPerBar.store(n); }
    void setMetronomeAccent(bool b)         { metronomeAccent.store(b); }
    void setMetronomeRateMul(float mul)     { metronomeRateMul.store(mul); }
    bool isMetronomeEnabled() const         { return metronomeEnabled.load(); }
    float getMetronomeVolume() const        { return metronomeVolume.load(); }
    float getMetronomePan()    const        { return metronomePan.load(); }
    int  getMetronomeSound() const          { return metronomeSound.load(); }
    bool getMetronomeAccent() const         { return metronomeAccent.load(); }
    float getMetronomeRateMul() const       { return metronomeRateMul.load(); }
    bool isCurrentlyPlaying() const { return playing.load(); }
    double getCurrentPositionSeconds() const { return currentPosition.load(); }
    double getSampleRate() const { return currentSampleRate; }
    // ループラップ回数（録音中の Take 自動積み上げ用）
    int    getLoopWrapCount() const { return loopWrapCount.load(); }
    void   resetLoopWrapCount()      { loopWrapCount.store(0); }

    // Levels (call from UI thread only)
    float getPeakL()     const { return peakL.load(); }
    float getPeakR()     const { return peakR.load(); }
    float getVUL()       const { return vuL.load(); }
    float getVUR()       const { return vuR.load(); }
    void  resetPeakHold()     { peakHoldL.store(-96.0f); peakHoldR.store(-96.0f); }
    float getPeakHoldL() const { return peakHoldL.load(); }
    float getPeakHoldR() const { return peakHoldR.load(); }

    // 入力レベル（UI スレッドから呼ぶ。チャンネル番号 0-based、範囲外は -96dB）
    static constexpr int kMaxInputChannels = 32;
    float getInputPeak(int ch) const
    {
        if (ch < 0 || ch >= kMaxInputChannels) return -96.0f;
        return inputPeak[ch].load();
    }
    float getInputVU(int ch) const
    {
        if (ch < 0 || ch >= kMaxInputChannels) return -96.0f;
        return inputVU[ch].load();
    }

    // トラック出力レベル（プラグインチェーン通過後・vol/pan 適用前のステレオ L/R）
    // テイクをレーンではなくトラックで管理する運用 (200+ トラック) に対応するため
    // 十分大きく確保する。固定配列なのでオーディオスレッドとの再確保競合が無い。
    // 停止中の減衰ループは実トラック数 (meterTrackCount) までに制限し、かつ無音トラックは
    // 早期スキップするため、容量を増やしてもアイドル時の実コストは低い。
    static constexpr int kMaxTracksMeters = 1024;
    float getTrackOutputPeakL(int trackIdx) const
    {
        if (trackIdx < 0 || trackIdx >= kMaxTracksMeters) return -96.0f;
        return trackOutPeakL[trackIdx].load();
    }
    float getTrackOutputPeakR(int trackIdx) const
    {
        if (trackIdx < 0 || trackIdx >= kMaxTracksMeters) return -96.0f;
        return trackOutPeakR[trackIdx].load();
    }
    float getTrackOutputVUL(int trackIdx) const
    {
        if (trackIdx < 0 || trackIdx >= kMaxTracksMeters) return -96.0f;
        return trackOutVUL[trackIdx].load();
    }
    float getTrackOutputVUR(int trackIdx) const
    {
        if (trackIdx < 0 || trackIdx >= kMaxTracksMeters) return -96.0f;
        return trackOutVUR[trackIdx].load();
    }

    // Master gain (0.0 - 1.0+)
    void setMasterGain(float gain) { masterGain.store(gain); }

    // マスターインサート用プラグインチェーン
    PluginChain& getMasterChain() { return *masterChain; }

    // Recording (message thread → audio thread via 録音 config スナップショット)
    // stereo=true の時は inputCh と inputCh+1 を 2ch として書き込む。実装は .cpp。
    // 後方互換: 単一ターゲットを設定する (既存ターゲットは全て破棄される)。writer==nullptr は
    // 全クリア + teardown バリア (直後に writer 破棄しても安全)。
    void setRecordingTarget(juce::AudioFormatWriter::ThreadedWriter* writer,
                            LiveRecordingBuffer* liveBuffer = nullptr,
                            int inputCh = 0,
                            bool stereo = false);

    // 複数ターゲット対応: 1 つ追加する (別マイク等で複数トラック同時録音)。
    // 同じ AudioEngine コールバックで全ターゲットに同じ numSamples 分を書き込む。
    void addRecordingTarget(juce::AudioFormatWriter::ThreadedWriter* writer,
                            LiveRecordingBuffer* liveBuffer,
                            int inputCh,
                            bool stereo);

    void clearRecordingTargets();

    // ディスクストリーミング ON/OFF (既定 ON)。OFF で再生時の音声読み込みを従来の同期読みに戻す
    // (切り分け/万一の不具合時のロールバック)。実行時に変えても renderClip が次ブロックから従う
    // (OFF→既存ボイスは無視して reader 直読み / ON→次の preparePlayback でボイス生成)。
    void setDiskStreamingEnabled(bool b) { diskStreamingEnabled.store(b); }
    bool isDiskStreamingEnabled() const  { return diskStreamingEnabled.load(); }

    // オーディオのマルチコア処理 ON/OFF (既定 ON)。ON で再生時のトラック描画 (クリップ + プラグイン
    // チェーン + PDC) を複数コアへ分散する。マスター加算はトラック index 昇順の直列なので、ON/OFF で
    // 出力はビット同一 (決定論)。稀にスレッド安全でないプラグイン向けに OFF で従来の単一スレッド経路へ。
    void setMulticoreAudioEnabled(bool b) { multicoreEnabled.store(b); }
    bool isMulticoreAudioEnabled() const  { return multicoreEnabled.load(); }
    int  getAudioWorkerCount() const      { return workerPool.getNumWorkers(); }
    // テスト用: 次回 audioDeviceAboutToStart で起動するワーカー数を固定する (-1 = 自動 = コア数-2)。
    void setForcedAudioWorkerCountForTests(int n) { forcedWorkerCount = n; }
    // テスト用: 書き出し中フラグを直接切り替える (書き出し中はモニタ返し/停止時プレビューが
    // 休止することの検証用。本番は renderOfflineRange が RAII で set/reset する)。
    void setOfflineRenderActiveForTests(bool v) { offlineRenderActive.store(v); }

    // Input monitoring
    void setInputMonitoringActive(bool b) { inputMonitoringActive.store(b); }
    // いずれかのトラックが Rec アーム中（停止中でも入力メーターを表示するかの判定用）
    void setAnyTrackRecArmed(bool b)      { anyTrackRecArmed.store(b); }
    // モニター返しに掛けるリバーブ量 (0..1)。モニター中トラックの Rev を反映。録音には焼かない。
    void setMonitorReverbSend(float v)    { monitorReverbSend.store(juce::jlimit(0.0f, 1.0f, v)); }
    // 入力モニターの返し音に通す INS チェーン (主モニタトラックのチェーン)。nullptr / 空チェーン
    // なら従来のドライ返しのまま (空チェーンは audio thread でスキップ)。チェーンの所有は Track 側で、
    // Track 破棄 (= clearPlayback) 時に空 config を drain 公開して audio が手放すまで待つ
    // (チェーンのダングリング防止バリア)。停止中モニタでも叩けるよう現 SR/blockSize で prepare する。
    // inputCh / stereo = 主モニタトラックの入力選択。返しの L/R マッピングをこれに合わせる
    // (mono はセンター、stereo は inputCh/inputCh+1)。pan = 返しに反映するパン (-1..+1)。
    // volumeDb = 返しに反映するフェーダー音量 (dB、内部で linear へ変換)。
    // chain が nullptr (ドライ返し) でも渡す。
    void setMonitorChain(class PluginChain* chain, int inputCh, bool stereo, float pan, float volumeDb);

    // 現在デバイスの入力チャンネル数
    int getNumInputChannels() const
    {
        if (auto* dev = deviceManager.getCurrentAudioDevice())
            return dev->getActiveInputChannels().countNumberOfSetBits();
        return 0;
    }

    // AudioIODeviceCallback
    void audioDeviceIOCallbackWithContext(
        const float* const* inputChannelData, int numInputChannels,
        float* const* outputChannelData,      int numOutputChannels,
        int numSamples,
        const juce::AudioIODeviceCallbackContext&) override;

    void audioDeviceAboutToStart(juce::AudioIODevice* device) override;
    void audioDeviceStopped() override;

private:
    struct PlaybackSnapshot;   // 定義は MidiPlayback / TrackDelay の後 (lock-free 再生スナップショット)
    // 再生クリップのスナップショット（audio thread が使用）
    struct PlaybackClip
    {
        int          trackIdx       { 0 };
        juce::File   file;
        AudioClip*   sourceClip     { nullptr };  // ライブ参照（gain/fade のリアルタイム反映）
        Track*       sourceTrack    { nullptr };  // ライブ参照（vol/pan のリアルタイム反映）
        double       clipStart      { 0.0 };
        double       clipEnd        { 0.0 };
        double       fileOffset     { 0.0 };
        float        trackGain      { 1.0f };  // トラックゲイン（dB→linear）
        float        gain           { 1.0f };  // 旧フィールド（互換のため残す）
        float        fadeInSecs     { 0.0f };
        float        fadeOutSecs    { 0.0f };
        double       fileSampleRate { 48000.0 };
        // 同一ファイルを参照する複数クリップで reader を共有する (ファイルハンドル節約)。
        // オーディオスレッドのみが触り、read() は内部で seek するため逐次呼び出しなら安全。
        // ディスクストリーミング OFF 時 / ボイス未生成時のフォールバック読みにも使う。
        std::shared_ptr<juce::AudioFormatReader> reader;
        // ディスクストリーミングの先読みボイス (ファイル単位・preparePlayback で割当)。非 null なら
        // renderClip はこれ経由で読み (ヒット時 I/O ゼロ)、ミス時はボイス内部で reader 相当の同期読み。
        // null (= 生成失敗 / ストリーミング OFF) なら上の reader を直接使う。
        std::shared_ptr<FileStreamVoice> voice;

        // 「同一の連続した音声」(= Alt+Click 分割など) か。同一ファイルかつ
        // タイムライン↔ファイル の対応 (fileOffset - clipStart) が一致する場合のみ true。
        // これだけクロスフェード対象から除外する (#I2)。テイク (別リージョン) は許可。
        bool sameContinuousAs(const PlaybackClip& o) const
        {
            return file == o.file
                && std::abs((fileOffset - clipStart) - (o.fileOffset - o.clipStart)) < 0.010;
        }
    };

    // allowStreaming: 先読みボイスを使ってよいか。audio コールバックのみ true。オフライン書き出し
    // (renderOfflineRange・別スレッド) は false にして reader を直読みする。ボイスのリングは SPSC
    // (consumer = audio thread 1 本) 前提なので、書き出しスレッドから触ると二重 consumer になるため。
    // scratch: クリップ読み出し用の一時バッファ。**呼び出し側が用意する** (マルチスレッド描画では
    // トラックごとに別インスタンスを渡す = 共有メンバだと並列で競合するため)。容量は blockSize 分。
    void renderClip(PlaybackClip& pc, juce::AudioBuffer<float>& output,
                    juce::AudioBuffer<float>& scratch,
                    double posStart, int numSamples, bool preFader = false,
                    bool allowStreaming = true);

    // 1 アクティブトラック (ai = activeTrackIdx 内の位置) のドライ描画 + プラグインチェーン + PDC を
    // snap.trackBuffers[tidx] に書く。トラックごとに独立な状態のみ触る (= ワーカースレッド並列実行可)。
    // vol/pan・マスター加算・リバーブ送り・メータは呼び出し側が直列フェーズで行う (共有書込のため)。
    void renderActiveTrack(PlaybackSnapshot& snap, int ai,
                           const std::vector<int>& activeTrackIdx,
                           const std::vector<Track*>& activeTracks,
                           double posStart, int numSamples,
                           bool monActive, PluginChain* monChain);
    // AudioWorkerPool 用ジョブ (ctx = ProduceCtx*)。renderActiveTrack を呼ぶだけの薄いブリッジ。
    static void produceTrackJob(void* ctx, int ai);

    void measureLevel(const float* data, int numSamples,
                      std::atomic<float>& peak, std::atomic<float>& peakHold,
                      float& vuSmooth, std::atomic<float>& vu,
                      float vuCoef);

    juce::AudioDeviceManager deviceManager;
    juce::AudioFormatManager formatManager;
    juce::MixerAudioSource   mixer;

    // ── ディスクストリーミング (先読み) ──
    // 全 FileStreamVoice が共有する先読みスレッド。コンストラクタで起動し shutdown() で停止する。
    // voicePool / スナップショットより前に宣言する = 破棄順 (逆順) でボイスより後に壊れる
    // = ボイス dtor の removeTimeSliceClient が生きたスレッドに対して走る (UAF 防止)。
    juce::TimeSliceThread streamThread { "uta-disk-stream" };
    std::atomic<bool>     diskStreamingEnabled { true };

    // ── オーディオのマルチコア処理 (Part B) ──
    // トラック描画を分散するワーカープール。audioDeviceAboutToStart で起動 / Stopped で停止。
    // parallelFor は audio コールバック内で同期完結する (ジョブは drain してから返る)。
    AudioWorkerPool   workerPool;
    std::atomic<bool> multicoreEnabled { true };
    int               forcedWorkerCount { -1 };   // テスト用の固定ワーカー数 (-1=自動)
    // これ未満のアクティブトラック数では並列化しない (起床コストが勝つため直列実行)。
    static constexpr int kMinTracksForThreads = 4;

    // ── 再生スナップショット (lock-free 公開) ──
    // 再生に必要な不変データ (clips/midi) と、audio thread が中身を書く scratch (trackBuffers/
    // trackDelays) と内蔵シンセを 1 つの PlaybackSnapshot にまとめ、shared_ptr を snapshotLock
    // (極小 SpinLock) 下で原子的に差し替える。audio thread はブロック先頭で shared_ptr を 1 回
    // コピーするだけで、以降の処理はロックを取らない (旧 playbackLock の lock-free 化)。
    // 旧 captureLock / previewMidiLock は別経路 (前者は realtime capture 専用、後者は ScopedTryLock)。
    std::shared_ptr<PlaybackSnapshot> activeSnapshot;
    juce::SpinLock                    snapshotLock;
    // 退役スナップショット (非 audio スレッドが保持し、audio が手放した = use_count()==1 で解放する)。
    std::vector<std::shared_ptr<PlaybackSnapshot>> retiredSnapshots;
    // retiredSnapshots / retiredRecConfigs は非 audio スレッド (message + 書き出しバックグラウンド) の
    // 両方から触られうる (realtime 書き出しは bg スレッドから play()→preparePlayback を呼びうる) ため、
    // 構造変更 (push_back / erase) を reclaimLock で直列化する。audio thread はこれらに一切触れないので、
    // この lock が audio thread をブロックすることはない。
    juce::CriticalSection reclaimLock;
    // 新スナップショットを公開して旧を退役へ送り、解放可能な退役を回収する (非 audio スレッド)。
    void publishSnapshot(std::shared_ptr<PlaybackSnapshot> next);
    void sweepRetiredSnapshots();                                       // use_count()==1 の退役を解放 (reclaimLock)
    void drainOldSnapshot(const std::shared_ptr<PlaybackSnapshot>& old);// audio が手放すまで待つ (UAF バリア)
    // synth 再 prepareToPlay 判定用 (SR/blockSize 不変なら持ち回しシンセを触らない)。
    double snapshotPreparedSr    { 0.0 };
    int    snapshotPreparedBlock { 0 };
    // preparePlayback 間で開いた AudioFormatReader を保持し流用する (再生中編集のたびに WAV を
    // 開き直して再構築が長引く = 一瞬の停止感、を避ける)。message thread (preparePlayback) 専用。
    // reader 自体は単一 audio thread が逐次 read/seek するためスナップショット間で共有して安全。
    std::unordered_map<juce::String, std::shared_ptr<juce::AudioFormatReader>> readerPool;
    // ディスクストリーミングの先読みボイス (ファイルパス → ボイス)。readerPool と同じく
    // preparePlayback 間で流用し、再生中編集のたびにボイス (= reader 2 本) を作り直さない。
    // message thread 専用。各ボイスは streamThread を共有する。
    std::unordered_map<juce::String, std::shared_ptr<FileStreamVoice>> voicePool;
    // 遅延破棄待ちの AudioClip (deferClipDestruction で積まれ、次の preparePlayback で公開する
    // スナップショットの graveyard へ移される)。message thread 専用。
    std::vector<std::unique_ptr<AudioClip>> pendingGraveyard;
    // 停止中の編集で立てる: 次の play() で preparePlayback を呼び直す。
    std::atomic<bool>         playbackDirty    { false };
    TrackManager*             lastTrackManager { nullptr };

    // ── 再生コンテンツ切替のデクリック ──
    // 再生中の Undo / 編集で invalidatePlayback() が走ると、UAF 防止のため一旦 clearPlayback()
    // してから rebuild するため、再生コンテンツが不連続になり「プチッ」というクリックになる。
    // これを防ぐため、playbackGen の変化を検知したブロックで「直前出力値 declickLast から現在の
    // workBuffer へ」kDeclickLen サンプルかけてクロスフェードし、不連続点を橋渡しする。
    // 信号が連続な箇所 (リバーブのテール等) では declickHold≈先頭サンプルとなりほぼ無改変。
    // playbackGen は clearPlayback/preparePlayback で増やす (UI/audio 間は atomic)。
    static constexpr int      kDeclickLen = 256;        // ~5.3ms @48k
    std::atomic<juce::uint32> playbackGen { 0 };
    juce::uint32              declickSeenGen     { 0 };  // audio thread 専用
    int                       declickXfadeRemain { 0 };  // audio thread 専用 (クロスフェード残サンプル)
    float                     declickLast[2] { 0.0f, 0.0f };// audio thread 専用 (直前出力値)
    float                     declickHold[2] { 0.0f, 0.0f };// audio thread 専用 (クロスフェード元値)
    // トラック単位のドライバッファ（プラグインチェーン処理用、index = trackIdx）は
    // PlaybackSnapshot::trackBuffers に集約 (公開後は構造不変、audio thread が中身のみ書く)。

    // ── プラグイン遅延補正 (PDC) ──
    // 各トラックの「最大遅延 - 自身の遅延」分を後段で遅延させてミックスに揃える
    struct TrackDelay
    {
        juce::AudioBuffer<float> buf;
        int writePos      { 0 };
        int delaySamples  { 0 };
    };
    int  maxPluginLatency { 0 };  // preparePlayback 内の PDC 計算用 (audio thread では未使用)
    // PDC: スナップショットの trackDelays を渡して trackBuf を遅延させる。
    void applyTrackDelay(std::vector<TrackDelay>& delays, int trackIdx,
                         juce::AudioBuffer<float>& trackBuf, int numSamples);
    // 単一の遅延ライン (循環バッファ) を buf に適用する。applyTrackDelay と書き出しの
    // クリック遅延で共用 (delaySamples==0 は no-op)。
    static void applyDelayLine(TrackDelay& d, juce::AudioBuffer<float>& buf, int numSamples);
    // 入力モニターの返しを出力へミックス (ドライ + モニターリバーブ)。録音には焼かない。
    // monChain が非 null かつ非空なら、返し音をそのチェーンに通す (INS をライブに掛ける)。
    void mixInputMonitoring(const float* const* inputChannelData, int numInputChannels,
                            float* const* outputChannelData, int numOutputChannels,
                            int numSamples, class PluginChain* monChain,
                            int monInputCh, bool monStereo, float monPan, float monGain);
    // UI スレッド読み出し用 (preparePlayback の autoCrossfade / zeroCrossingFade)。
    AppSettings               appSettings;
    // audio スレッド読み出し用スナップショット (メトロノーム区間が bpmChanges/meterChanges の vector を
    // 読む)。setAppSettings (message thread) が shared_ptr を appSettingsLock 下で差し替え、audio は
    // ブロック先頭で 1 回コピーするだけ。旧スナップショットは reclaimLock 配下の retiredAppSettings へ
    // 退役させ、audio が手放した (use_count()==1) ら非 audio スレッドで解放する (audio は解放しない)。
    std::shared_ptr<const AppSettings>              activeAppSettings;
    juce::SpinLock                                  appSettingsLock;
    std::vector<std::shared_ptr<const AppSettings>> retiredAppSettings;
    void sweepRetiredAppSettings();

    // パンチイン状態（録音中は録音開始位置以降の古いクリップをミュート）
    std::atomic<bool>   isRecordingActive { false };
    std::atomic<double> recordingStartSecs { 0.0 };
    // 書き込み開始位置 (ゲート)。カウントイン/プリロールの遡及録音のため
    // recordingStartSecs (ミュート位置) より手前になり得る
    std::atomic<double> recordingWriteFromSecs { 0.0 };
    // 実書き込み開始位置マーカー (-1 = 未書き込み)。リセットは setRecordingTarget /
    // setRetrospectiveTarget (新 writer 登録時・config 公開前) が行い、audio thread は
    // 新 config で最初に書いたブロックの posStart を store する (公開順で race しない)。
    // 詳細は getRecordingFirstWritePosSecs のコメント
    std::atomic<double> recFirstWritePos   { -1.0 };
    std::atomic<double> retroFirstWritePos { -1.0 };

    // ループ範囲
    std::atomic<bool>   loopActive    { false };
    std::atomic<double> loopStartSecs { 0.0 };
    std::atomic<double> loopEndSecs   { 0.0 };
    std::atomic<int>    loopWrapCount { 0 };

    // メトロノーム
    std::atomic<bool>   metronomeEnabled    { false };
    std::atomic<double> metronomeBpm        { 120.0 };
    // 既定は -7 dB 相当 (decibelsToGain(-7)*0.5)。トラック同期式 volume = decibelsToGain(dB)*0.5 の
    // 逆写像で設定ダイアログには -7.0 dB と表示される。旧既定 0.5 (= 0 dB) はクリック音が大きすぎた
    std::atomic<float>  metronomeVolume     { 0.223342f };
    std::atomic<float>  metronomePan        { 0.0f };
    std::atomic<int>    metronomeSound      { 0 };
    std::atomic<int>    metronomeBeatsPerBar { 4 };
    std::atomic<bool>   metronomeAccent     { true };
    std::atomic<float>  metronomeRateMul    { 1.0f };  // 1.0=Normal, 0.5=Half, 2.0=Double
    // 内部状態（オーディオスレッドのみ）
    int    clickLastBeatInt { -1 };
    double clickLastRateMul { 1.0 };  // 直前ブロックで使った rateMul（変化検出用）
    double clickEnvelope    { 0.0 };
    double clickPhase       { 0.0 };
    double clickFreq        { 1000.0 };
    bool   clickIsDownbeat  { false };

    std::atomic<bool>  playing         { false };
    // オフライン書き出し (renderOfflineRange) の実行中フラグ。書き出しは別スレッドで同じ
    // PluginChain の processBlock を叩くため、停止時ブランチのプラグインプレビューと
    // 入力モニタ返し (mixInputMonitoring) が同じチェーンを並行処理して書き出し音声を
    // 壊さないよう、書き出し中はプレビューとモニタ返しの両方をスキップする。
    std::atomic<bool>  offlineRenderActive { false };
    std::atomic<double> currentPosition { 0.0 };
    std::atomic<float> masterGain      { 1.0f };

    // ホストの再生位置情報をプラグインへ供給する (Melodyne 等の transport 同期用)。
    // 全チェーン (トラック/MIDI/マスター) が共有し、ブロック先頭で 1 回更新する。
    // audio thread からのみ触る (update / getPosition とも audio thread・逐次)。
    // 書き出し (renderOfflineRange・別スレッド) はこれを使わずローカルインスタンスを使う。
    EnginePlayHead playHead;
    // ph に posSecs の位置情報を書き込む (cfg からテンポ/拍子を解決)。静的純関数的ヘルパ。
    static void fillPlayHead(EnginePlayHead& ph, double posSecs, double sr,
                             const AppSettings& cfg, bool isPlaying, bool isRecording,
                             bool looping, double loopStartSec, double loopEndSec);

    // 簡易リバーブ送りバス (各トラックの reverbSend を合算し、ウェットだけマスターへ加算)
    juce::Reverb              masterReverbBus;
    juce::AudioBuffer<float>  reverbSendBuf;
    // メトロノーム合成音を CLICK トラックの INS チェーンへ通すためのスクラッチ (audio thread 専用)。
    juce::AudioBuffer<float>  clickSynthBuf;
    double                    reverbPreparedSr { 0.0 };
    // リバーブのテールが内部に残っているか (audio thread 専用)。停止に入った最初の
    // ブロックで一度だけ reset するためのフラグ。これが無いと前回のテールが凍結保持され、
    // 次の再生で送りが発生した瞬間に古い残響が混入する。
    bool                      reverbBusDirty { false };

    // ── モニターリバーブ (入力モニターの「返し」専用) ──
    // 録音ファイルには一切焼き込まず、出力に足す返し音にだけリバーブをかける。
    // 再生用 masterReverbBus とは独立 (停止中の monitor のみでも鳴らすため)。
    juce::Reverb              monitorReverbBus;
    juce::AudioBuffer<float>  monitorReverbBuf;
    std::atomic<float>        monitorReverbSend { 0.0f };  // モニター中トラックの Rev 量
    bool                      monitorReverbDirty { false }; // audio thread 専用

    // ── 実時間キャプチャ ──
    std::atomic<bool>   captureActive       { false };
    std::atomic<int>    captureWritePos     { 0 };
    std::atomic<int>    captureTotalSamples { 0 };
    juce::AudioBuffer<float> captureBuffer;
    juce::CriticalSection    captureLock;

    std::atomic<float> peakL    { -96.0f };
    std::atomic<float> peakR    { -96.0f };
    std::atomic<float> peakHoldL { -96.0f };
    std::atomic<float> peakHoldR { -96.0f };
    std::atomic<float> vuL      { -96.0f };
    std::atomic<float> vuR      { -96.0f };

    float vuSmoothL { 0.0f };
    float vuSmoothR { 0.0f };

    // 入力チャンネル毎のメータ
    std::atomic<float> inputPeak[kMaxInputChannels] {};
    std::atomic<float> inputVU  [kMaxInputChannels] {};
    float inputVUSmooth[kMaxInputChannels] {};

    // トラック出力毎のメータ（プラグイン処理後の純粋トラック信号）
    std::atomic<float> trackOutPeakL[kMaxTracksMeters] {};
    std::atomic<float> trackOutPeakR[kMaxTracksMeters] {};
    std::atomic<float> trackOutVUL  [kMaxTracksMeters] {};
    std::atomic<float> trackOutVUR  [kMaxTracksMeters] {};
    float trackOutVUSmoothL[kMaxTracksMeters] {};
    float trackOutVUSmoothR[kMaxTracksMeters] {};
    // 実トラック数。停止中の減衰ループをこの数まで（かつ無音は早期スキップ）に
    // 制限し、アイドル時の不要な log10 を避ける。preparePlayback で更新。
    std::atomic<int> meterTrackCount { 0 };

    // 複数同時録音ターゲット (各 Track 毎の writer / input ch / stereo)。
    // 全ターゲットが同じ AudioEngine コールバックで同期書き込みされる。
    struct RecordingTarget
    {
        juce::AudioFormatWriter::ThreadedWriter* writer     { nullptr };
        LiveRecordingBuffer*                     liveBuffer { nullptr };
        int                                      inputCh    { 0 };
        bool                                     stereo     { false };
    };
    // 録音設定スナップショット (recLock の lock-free 化)。targets + 遡及録音の影ライターを
    // 1 つの不変 config にまとめ、shared_ptr を recConfigLock (極小 SpinLock) 下で差し替える。
    // audio thread はブロック先頭で 1 回コピーするだけ。writer 破棄を伴う teardown setter は
    // 旧 config を audio が手放すまで drain してから返す (writer の UAF 防止バリア)。
    struct RecordingConfig
    {
        std::vector<RecordingTarget>             targets;
        juce::AudioFormatWriter::ThreadedWriter* retro        { nullptr };
        LiveRecordingBuffer*                     retroLiveBuf { nullptr };
        int                                      retroInputCh { 0 };
        bool                                     retroStereo  { false };
    };
    std::shared_ptr<const RecordingConfig>              activeRecConfig;
    juce::SpinLock                                      recConfigLock;
    std::vector<std::shared_ptr<const RecordingConfig>> retiredRecConfigs;
    void publishRecConfig(std::shared_ptr<const RecordingConfig> next, bool drain);
    void sweepRetiredRecConfigs();

    // ── モニター FX チェーン公開 (入力モニターの返しに INS を通す) ──
    // 主モニタトラックの PluginChain* を不変 config にまとめ、shared_ptr を monConfigLock
    // (極小 SpinLock) 下で差し替える (recConfig と同じ作法)。audio thread はブロック先頭で
    // 1 回 grab するだけ。チェーンの所有は Track 側。Track 破棄を伴う clearPlayback() が空
    // config を drain 公開し、audio が旧 config を手放すまで待つ (チェーンの UAF 防止バリア)。
    struct MonitorConfig
    {
        class PluginChain* chain { nullptr };
        // 主モニタトラックの入力チャンネル選択。モニター返しの L/R マッピングをこれに合わせる
        // (録音と同じ入力を聞かせる)。mono は inputCh を L/R 両方へ = センター、stereo は
        // inputCh→L / inputCh+1→R。これが無いと device ch0→L / ch1→R 固定で、mono 入力でも
        // 別チャンネルが R に乗って L/R 分離して聞こえてしまう。
        int  inputCh { 0 };
        bool stereo  { false };
        // 主モニタトラックのパン (-1..+1)。返しにも反映する (再生時と同じく FX 後段で適用)。
        // pan=0 は L/R 等倍 = センター (従来挙動)。
        float pan    { 0.0f };
        // 主モニタトラックのフェーダー音量 (リニアゲイン)。返しにも反映する (再生時と同じく
        // FX 後段で pan と一緒に適用)。1.0 = 0dB (従来挙動)。setMonitorChain で dB→linear 変換。
        float gain   { 1.0f };
    };
    std::shared_ptr<const MonitorConfig>              activeMonConfig;
    juce::SpinLock                                    monConfigLock;
    std::vector<std::shared_ptr<const MonitorConfig>> retiredMonConfigs;
    void publishMonConfig(std::shared_ptr<const MonitorConfig> next, bool drain);
    void sweepRetiredMonConfigs();
    // モニター FX 経由用スクラッチ (audioDeviceAboutToStart で確保・audio thread 専用)。
    juce::AudioBuffer<float> monitorChainBuf;
    juce::MidiBuffer         monitorMidiScratch;
    std::atomic<bool>                             inputMonitoringActive { false };
    std::atomic<bool>                             anyTrackRecArmed      { false };
    // setRecordingActive(true) 以降に recordingTarget へ書き込んだサンプル累計
    std::atomic<juce::int64>                      recordedSamples       { 0 };

    // ── 録音レイテンシ補正 (getRecordingLatencyCompSecs 参照) ──
    std::atomic<double> deviceRoundTripSecs { 0.0 };
    std::atomic<bool>   recLatencyAutoComp  { true };
    std::atomic<double> recLatencyManualMs  { 0.0 };

    double currentSampleRate { 48000.0 };
    int    currentBufferSize { 512 };

    juce::AudioBuffer<float> workBuffer;
    // 停止中のシンセプレビューレンダリング用 (オーディオスレッドでヒープ確保しないよう保持)
    juce::AudioBuffer<float> previewBuf;

    // ── realtime callback の再利用スクラッチ (毎ブロックのヒープ確保を避ける) ──
    // preparePlayback で reserve / 容量確保し、callback では clear() で長さ 0 に戻して使う。
    std::vector<int>    activeTrackIdxScratch;
    std::vector<Track*> activeTracksScratch;
    // プラグインチェーン用の空 MIDI バッファ (トラック / マスター / ディザー共用)。
    juce::MidiBuffer    chainMidiScratch;
    // MIDI トラックの note/state イベントを詰める専用バッファ (chainMidiScratch とは別物)。
    juce::MidiBuffer    synthMidiScratch;

    // ── VU 係数キャッシュ (numSamples / SR 不変なら std::exp を再計算しない) ──
    int   vuCoefForSamples { -1 };
    float vuCoefCached     { 0.0f };
    float vuOneMinusCached { 0.0f };

public:
    // ピアノロール等の UI から MIDI ノート単発プレビュー (内蔵シンセ経由)
    void previewMidiNote(int trackIdx, int note, float velocity, bool isOn);
private:
    struct PreviewMidiEvent { int trackIdx; int note; float velocity; bool isOn; };
    juce::CriticalSection previewMidiLock;
    std::vector<PreviewMidiEvent> pendingPreviewMidi;

    // マスターインサートチェーン（マスターゲイン前に適用）
    std::unique_ptr<PluginChain> masterChain;
    // preparePlayback で構築される MIDI 再生キャッシュ。
    // 各 MIDI トラックの全クリップを1本のタイムライン秒ソート済みリストに集約。
    struct MidiPlayback
    {
        int     trackIdx { -1 };
        Track*  track    { nullptr };
        std::vector<juce::MidiMessage> events;   // タイムスタンプ = タイムライン秒、昇順ソート
        int     lastTranspose { 0 };             // 直前のトランスポーズ値（差分検知用）
    };
    // 再生に必要な状態を 1 つにまとめた不変構造 (shared_ptr で lock-free 公開)。
    //  - clips / midi : 公開後は完全に不変 (audio thread は読むだけ)
    //  - trackBuffers / trackDelays : 構造 (要素数/サイズ) は公開後不変。中身は単一 audio thread が
    //    毎ブロック書く scratch (export は自前の局所バッファを使うので競合しない)
    //  - synths : MIDI ボイス状態を rebuild 間で保つため shared_ptr で持ち回す
    struct PlaybackSnapshot
    {
        std::vector<PlaybackClip>                   clips;
        // トラック別インデックス (preparePlayback で構築・公開後不変)。毎ブロックの
        // 「全 clips 走査 ×3 (Solo判定/アクティブ収集/トラック毎の描画)」を排除する。
        //  - clipsByTrack[trackIdx] = その トラックに属する clips 内 index 一覧
        //  - clipTracks = クリップを持つトラックの (trackIdx, Track*) 一覧 (dedup 済み・Click 除外)
        std::vector<std::vector<int>>               clipsByTrack;
        std::vector<std::pair<int, Track*>>         clipTracks;
        std::vector<MidiPlayback>                   midi;
        std::vector<juce::AudioBuffer<float>>       trackBuffers;
        // クリップ読み出し用スクラッチ (index = trackIdx)。renderClip がトラック描画時に使う。
        // トラックごとに別インスタンスなので、マルチコア描画でワーカーが並列に renderClip を
        // 呼んでも競合しない (旧: 単一メンバ clipBuffer を共有していた)。preparePlayback で確保。
        std::vector<juce::AudioBuffer<float>>       clipScratch;
        std::vector<TrackDelay>                      trackDelays;
        std::vector<std::shared_ptr<InternalSynth>> synths;
        // 遅延破棄: 破棄系編集 (テイク操作/分割/無音カット) で取り除いた AudioClip を、まだそれを
        // 参照しているスナップショットが生きている間は破棄せずここで保持する。これにより再生中編集でも
        // 旧スナップショットを空にせず (= 他トラックを止めず) 旧→新へ直接遷移できる。次に公開される
        // スナップショットへ載せ、そのスナップショットが退役・回収される時に解放される (audio thread は
        // 触らない)。AudioClip* を生参照する PlaybackClip より後に解放されるよう、ここで所有を握る。
        std::vector<std::unique_ptr<AudioClip>>     graveyard;
        // CLICK トラック (あれば) の生ポインタ。メトロノーム合成音をこのトラックの INS
        // チェーン (EQ 等) に通すために保持する。clipTracks の Track* と同じく clearPlayback()
        // の drain バリアで保護される (トラック削除時に空スナップショットへ遷移してから破棄)。
        Track*                                      clickTrack { nullptr };
    };
    double lastBlockPosStart { -1.0 };  // 不連続検知用

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AudioEngine)
};
