// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

#include "RecordingManager.h"
#include "../Audio/AudioEngine.h"

RecordingManager::RecordingManager(AudioEngine& eng, TrackManager& tracks,
                                   juce::AudioFormatManager& fmt)
    : audioEngine(eng), trackManager(tracks), formatManager(fmt)
{
    backgroundThread.startThread();
    getRecordingsFolder().createDirectory();
}

RecordingManager::~RecordingManager()
{
    stopRecording(0.0);
    stopRetrospective(false, 0.0);
    backgroundThread.stopThread(2000);
}

juce::File RecordingManager::getRecordingsFolder() const
{
    if (getAudioFolder) return getAudioFolder();
    return juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
               .getChildFile("Utawave").getChildFile("Recordings");
}

juce::File RecordingManager::createRecordingFile(const juce::String& trackName) const
{
    auto folder = getRecordingsFolder();
    folder.createDirectory();
    auto ts   = juce::Time::getCurrentTime();
    auto name = juce::File::createLegalFileName(
        trackName + "_" + ts.formatted("%Y%m%d_%H%M%S") + ".wav");
    // タイムスタンプは秒精度なので、同じ秒内に録音を始め直す (Q リテイクの即時再録音等) と
    // 同一パスになる。既存ファイルは前のテイクが参照している可能性があり、FileOutputStream で
    // 開くと追記されて壊れるため、存在する場合は連番を足して必ず新規ファイルにする
    return folder.getChildFile(name).getNonexistentSibling(false);
}

bool RecordingManager::startRecording(double recStartSec, double playFromSec,
                                       bool loopRecording,
                                       double loopStart, double loopEnd)
{
    if (recording) return false;
    lastStartFailures.clear();

    // 録音開始時点の補正量を確定 (停止時のクリップ配置で使う)
    activeLatencyComp = audioEngine.getRecordingLatencyCompSecs();

    // 遡及録音アクティブ + そのトラックがアーム中なら、Punch From Retro モードへ。
    // 既存の retro writer をそのまま使い続け、stop 時に 1 つのクリップ（offset付き）として
    // 配置する。他にアーム中のトラックがあれば下の通常ループで R 位置からの writer を作り
    // 同時録音する (旧実装は retroTrack だけで early return しており、再生中のパンチインでは
    // 2 本目以降のアームトラックが無警告で録音されない不具合があった)
    if (retroActive && retroTrack != nullptr && retroTrack->isRecArmed()
        && !loopRecording)
    {
        punchFromRetro = true;
        punchInRecStart = recStartSec;
        // R 押下時点からライブ波形オーバーレイを表示開始。lead に補正量を含めることで
        // 確定クリップ (fileOffset に retroLatencyComp が乗る) と同じ位置に描かれ、
        // 確定した瞬間に波形が左へずれて見えない
        retroTrack->startLiveRecording(recStartSec, retroLatencyComp);
        audioEngine.setRetrospectiveLiveBuffer(&retroTrack->getLiveBuffer());
    }

    // カウントイン/プリロール区間も遡及的に録る: 書き込みは再生開始位置 (playFromSec) から
    // 始め、クリップは recStartSec に fileOffset 付きで置く (左端を伸ばすとブレスを復元できる)
    const double writeFrom = juce::jmin(recStartSec, playFromSec);

    const double sampleRate = audioEngine.getSampleRate();
    juce::WavAudioFormat wavFormat;
    const int bits = recordingBitDepth;
    const auto sampleFormat = (bits >= 32)
        ? juce::AudioFormatWriterOptions::SampleFormat::floatingPoint
        : juce::AudioFormatWriterOptions::SampleFormat::integral;

    for (int i = 0; i < trackManager.getTrackCount(); ++i)
    {
        auto* track = trackManager.getTrack(i);
        if (!track->isRecArmed()) continue;
        // MIDI トラックの録音は音声 writer でなく MIDI キャプチャ
        // (MainComponent::beginMidiCapture) が担当する
        if (track->isMidiTrack()) continue;
        if (punchFromRetro && track == retroTrack) continue;   // retro writer が担当

        auto file   = createRecordingFile(track->getName());
        auto stream = std::make_unique<juce::FileOutputStream>(file);
        if (!stream->openedOk())
        {
            // ディスク満杯/権限などで開けない → 黙ってスキップせず呼び出し側へ報告
            stream.reset();
            file.deleteFile();
            lastStartFailures.add(track->getName());
            continue;
        }

        const int numCh = track->isStereo() ? 2 : 1;
        auto opts = juce::AudioFormatWriterOptions{}
                        .withSampleRate(sampleRate)
                        .withNumChannels((juce::uint32) numCh)
                        .withBitsPerSample(bits)
                        .withSampleFormat(sampleFormat);
        std::unique_ptr<juce::OutputStream> outStream = std::move(stream);
        auto writer = wavFormat.createWriterFor(outStream, opts);
        if (writer == nullptr)
        {
            outStream.reset();
            file.deleteFile();
            lastStartFailures.add(track->getName());
            continue;
        }

        // オーバーレイ表示は R 押下位置から。カウントイン/プリロールの先行録音分 +
        // レイテンシ補正量をリード (非表示) としてバッファ先頭から読み飛ばす。
        // 確定クリップの fileOffset (= preRecDur + comp) と同じスキップ量なので、
        // 録音中の表示と確定後の波形位置が一致する (確定時の左ズレ見えを防ぐ)
        track->startLiveRecording(recStartSec,
                                  recStartSec - writeFrom + activeLatencyComp);

        auto tw = std::make_unique<juce::AudioFormatWriter::ThreadedWriter>(
            writer.release(), backgroundThread, 65536);
        // ループ録音時はラップ毎にクリップを作成・読み出すため定期 flush を有効化
        if (loopRecording)
            tw->setFlushInterval((int)(sampleRate * 0.1));  // 100ms 毎にディスクへ反映

        // 複数トラック同時録音: 1 トラック目は setRecordingTarget で「単一設定」、
        // 2 トラック目以降は addRecordingTarget で追加する。
        // (1 トラック目で clear が走ることで、前回の録音設定が残らないように)
        if (activeRecordings.empty())
            audioEngine.setRecordingTarget(tw.get(), &track->getLiveBuffer(),
                                           track->getInputChannel(),
                                           track->isStereo());
        else
            audioEngine.addRecordingTarget(tw.get(), &track->getLiveBuffer(),
                                            track->getInputChannel(),
                                            track->isStereo());

        ActiveRecording ar;
        ar.track         = track;
        ar.file          = file;
        ar.startPosition = recStartSec;
        ar.fileStartPos  = writeFrom;
        ar.writer        = std::move(tw);
        ar.loopRec       = loopRecording;
        ar.loopStart     = loopStart;
        ar.loopEnd       = loopEnd;
        ar.wallStartMs   = juce::Time::currentTimeMillis()
                           + (juce::int64)((recStartSec - playFromSec) * 1000.0);
        // テイクのレーンは配置時に findFreeTakeLaneIndex で決める (通常録音の
        // backupToTakeLane と同じく、空いている既存テイクレーンを再利用してから新規作成)
        ar.takesAddedRealtime = 0;
        activeRecordings.push_back(std::move(ar));
        // 全 Rec アーム済みトラックを順に登録 (break なし)
    }

    // パンチイン録音開始時刻 (ミュート位置) と書き込み開始位置を AudioEngine に通知
    if (!activeRecordings.empty() || punchFromRetro)
        audioEngine.setRecordingActive(true, recStartSec, writeFrom);

    recording = punchFromRetro || !activeRecordings.empty();
    return recording;
}

void RecordingManager::stopRecording(double endPositionSeconds, bool takesOnly)
{
    if (!recording) return;

    audioEngine.setRecordingActive(false);

    // ── Punch From Retro: retro ライターをそのまま終了し、offset 付きクリップで配置 ──
    // (他のアームトラックが併走録音していれば activeRecordings 側にあり、下の通常処理で配置)
    if (punchFromRetro)
    {
        audioEngine.setRetrospectiveTarget(nullptr);
        retroWriter.reset();  // フラッシュ・クローズ

        // 遡及ファイルの先頭はエンジンの実書き込み開始位置 (writer 登録は play() 後の
        // message thread なので retroPlayStart より 0〜数ブロック遅れ得る。登録遅れ対策)
        const double fileStart = audioEngine.getRetroFirstWritePosSecs(retroPlayStart);
        const double recStart  = punchInRecStart;
        const double stopPos   = endPositionSeconds;

        if (retroTrack && retroFile.existsAsFile())
        {
            retroTrack->cancelLiveRecording();  // ライブ波形オーバーレイを消す
            const double dur = stopPos - recStart;
            if (dur > 0.05)
            {
                // レイテンシ補正: retro ファイル基準の fileOffset を保ったまま手前へ
                const auto p = compensateLatency(recStart, dur,
                                                 juce::jmax(0.0, recStart - fileStart),
                                                 retroLatencyComp, recStart);
                auto* lane = retroTrack->getLane(0);
                if (!takesOnly && lane && p.dur > 0.01)
                {
                    auto* clip = lane->addClip(retroFile, p.start, p.dur,
                                                retroTrack->getFormatManager(),
                                                retroTrack->getThumbnailCache());
                    if (clip)
                    {
                        clip->setFileOffset(p.fileOffset);
                        // 録音直後のファイルはキャッシュが古い/未完なので必ず再読込
                        clip->refreshThumbnail();
                        // パンチイン境界に最小クロスフェード作成
                        retroTrack->trimAndCrossfadeOnLane0(clip, p.start, p.dur);
                    }
                }
                // Take レーンにもバックアップ (録音履歴を残す)
                if (p.dur > 0.01)
                    if (auto* bk = retroTrack->backupToTakeLane(retroFile, p.start, p.dur,
                                                                p.fileOffset))
                        bk->refreshThumbnail();
            }
        }

        retroTrack    = nullptr;
        retroFile     = juce::File();
        retroActive   = false;
        punchFromRetro = false;
        // return しない: 併走した通常録音 (activeRecordings) を下で配置する
    }

    audioEngine.setRecordingTarget(nullptr, nullptr);

    for (auto& ar : activeRecordings)
    {
        ar.writer.reset(); // フラッシュ・クローズ（ファイルが完全に書き終わる）

        // リアルタイムに作成されたクリップのサムネイルを再生成
        // （録音中はファイル末尾が伸び続けていて全体が読めていないため）
        for (auto* clip : ar.realtimeClips)
            if (clip) clip->refreshThumbnail();

        // 通常録音は再生位置ベースで尺を出す（ループ無し前提）
        // ループ録音時は AudioEngine が書き込んだサンプル数で実書き出し時間を出す
        // (currentPosition はループで巻き戻るため使えない。wall clock は OS 時計補正・
        //  スリープでズレるためサンプル数ベースの方が確実)
        double dur = 0.0;
        if (ar.loopRec)
        {
            const double sr = audioEngine.getSampleRate() > 0.0
                                ? audioEngine.getSampleRate() : 48000.0;
            dur = (double) audioEngine.getRecordedSampleCount() / sr;
        }
        else
        {
            dur = endPositionSeconds - ar.startPosition;
        }

        // ファイル先頭の実タイムライン位置。ターゲット登録は play() 後の message thread で
        // 行われるため、書き込みはゲート位置 (ar.fileStartPos = writeFrom) より 0〜数ブロック
        // 遅れて始まることがある。writeFrom 仮定のまま配置すると、その取りこぼし分だけ
        // fileOffset のトリムが過剰になり内容が手前へずれる (テイクごとにランダムな
        // ブロック粒度のズレ。Q リテイクの検証で顕在化)。エンジンが記録した
        // 「最初に書き込んだブロックの位置」を真のファイル先頭として使う
        const double fileStart = audioEngine.getRecordingFirstWritePosSecs(ar.fileStartPos);

        // カウントイン/プリロールの先行録音分 (ファイル先頭の読み飛ばし量)。
        // クリップ左端を伸ばすとこの区間 (ブレス等) を復元できる
        const double preRecDur = juce::jmax(0.0, ar.startPosition - fileStart);

        if (!ar.loopRec)
        {
            // 書き込みが R 位置より後から始まった場合 (カウントイン無し + 登録遅れ) は
            // 左端をファイル先頭へ寄せる (fileOffset は負にできないため)
            const double placeStart = juce::jmax(ar.startPosition, fileStart);
            const auto p = compensateLatency(placeStart, endPositionSeconds - placeStart,
                                             juce::jmax(0.0, placeStart - fileStart),
                                             activeLatencyComp, placeStart);
            if (p.dur > 0.01 && ar.file.existsAsFile())
            {
                if (takesOnly)
                {
                    // テイクレーンだけに確定 (Lane 0 は触らない = 録り直し前提)
                    if (auto* bk = ar.track->backupToTakeLane(ar.file, p.start, p.dur,
                                                              p.fileOffset))
                        bk->refreshThumbnail();
                    ar.track->cancelLiveRecording();
                }
                else
                    ar.track->finishLiveRecording(ar.file, p.start, p.dur, p.fileOffset);
            }
            else
                ar.track->cancelLiveRecording();
            continue;
        }

        // ── ループ録音: ファイルをループ区間ごとにスライスして Take レーンへ ──
        ar.track->cancelLiveRecording();

        const double loopDur = ar.loopEnd - ar.loopStart;
        if (loopDur < 0.05 || !ar.file.existsAsFile()) continue;

        // 最後に配置したテイクのジオメトリ。停止後に Lane 0 へも同内容を置く
        // (歌い終わったらすぐ聴ける状態にする)。リアルタイム配置分を初期値にし、
        // 停止時スライスで更新する
        double lastStart = 0.0, lastDur = 0.0, lastFO = 0.0;
        bool   lastValid = false;
        if (!ar.realtimeClips.empty())
            if (auto* rc = ar.realtimeClips.back())
            {
                lastStart = rc->getStartPosition();
                lastDur   = rc->getDuration();
                lastFO    = rc->getFileOffset();
                lastValid = true;
            }

        if (dur >= 0.05)
        {
            // テイクの位置/フル尺/fileOffset は純関数 loopTakeSlice (ヘッダ・onLoopWrap と
            // 共通) で求め、ここでは「録り切れた分」への尺クランプだけを行う。
            // dur (書き込みサンプル数) はカウントイン/プリロールの先行録音分 (preRecDur) を含む
            const double firstPassDur  = juce::jmax(0.0, ar.loopEnd - ar.startPosition);
            const double durFromStart  = juce::jmax(0.0, dur - preRecDur);

            // リアルタイム配置済みの take は再追加しない
            const int alreadyAdded = ar.takesAddedRealtime;

            // Take 1（まだ配置されていなければ）= 録音開始位置から 1 周目末尾まで
            // (1 周目の途中で停止した場合は録音できた所まで)
            if (alreadyAdded == 0)
            {
                const auto s0 = loopTakeSlice(0, ar.startPosition, fileStart,
                                              ar.loopStart, ar.loopEnd);
                const double take1Dur = juce::jmin(durFromStart, s0.dur);
                const auto p = compensateLatency(s0.pos, take1Dur, s0.fileOffset,
                                                 activeLatencyComp, s0.pos);
                if (p.dur > 0.01)
                {
                    const int laneIdx = ar.track->findFreeTakeLaneIndex(
                        p.start, p.start + p.dur, ar.lastTakeLaneIdx + 1);
                    auto* lane = ar.track->getLane(laneIdx);
                    auto* clip = lane ? lane->addClip(ar.file, p.start, p.dur,
                                                      ar.track->getFormatManager(),
                                                      ar.track->getThumbnailCache())
                                      : nullptr;
                    if (clip)
                    {
                        clip->setFileOffset(p.fileOffset);
                        clip->refreshThumbnail();
                        ar.lastTakeLaneIdx = laneIdx;
                        lastStart = p.start; lastDur = p.dur; lastFO = p.fileOffset;
                        lastValid = true;
                    }
                }
            }

            // 2周目以降の周回（まだ配置されていないものだけ追加）。位置/fileOffset は
            // loopTakeSlice、尺は録り切れた分 (rest) にクランプ
            const double rest = juce::jmax(0.0, durFromStart - firstPassDur);
            const int numRestPasses = (rest > 0.0) ? (int)std::ceil(rest / loopDur) : 0;
            for (int it = juce::jmax(1, alreadyAdded); it <= numRestPasses; ++it)
            {
                const auto sit = loopTakeSlice(it, ar.startPosition, fileStart,
                                               ar.loopStart, ar.loopEnd);
                const double inRestOffset = (double)(it - 1) * loopDur;
                const double sliceDur = juce::jmin(sit.dur, rest - inRestOffset);
                if (sliceDur < 0.05) continue;

                const auto p = compensateLatency(sit.pos, sliceDur, sit.fileOffset,
                                                 activeLatencyComp, sit.pos);
                if (p.dur < 0.01) continue;
                const int laneIdx = ar.track->findFreeTakeLaneIndex(
                    p.start, p.start + p.dur, ar.lastTakeLaneIdx + 1);
                auto* lane = ar.track->getLane(laneIdx);
                if (!lane) continue;
                auto* clip = lane->addClip(ar.file, p.start, p.dur,
                                           ar.track->getFormatManager(),
                                           ar.track->getThumbnailCache());
                if (clip)
                {
                    clip->setFileOffset(p.fileOffset);
                    clip->refreshThumbnail();
                    ar.lastTakeLaneIdx = laneIdx;
                    lastStart = p.start; lastDur = p.dur; lastFO = p.fileOffset;
                    lastValid = true;
                }
            }
        }

        // ── 最後のテイクを Lane 0 にも配置 (要望 2026-07) ──
        // テイクリストに入るだけだと Lane 0 が空のままで、停止後すぐに聴けない。
        // パンチインと同じ作法 (trimAndCrossfadeOnLane0) で既存クリップをトリムして置く。
        // 同じテイクはテイクレーンにも残っているので、別テイクの採用は Shift+↑ で可能
        // (takesOnly = Q リテイクの「テイクを残す」ではこの Lane 0 配置だけを省く)
        if (!takesOnly && lastValid && lastDur > 0.01)
        {
            if (auto* lane0 = ar.track->getLane(0))
            {
                auto* c = lane0->addClip(ar.file, lastStart, lastDur,
                                         ar.track->getFormatManager(),
                                         ar.track->getThumbnailCache());
                if (c)
                {
                    c->setFileOffset(lastFO);
                    c->refreshThumbnail();
                    ar.track->trimAndCrossfadeOnLane0(c, lastStart, lastDur);
                }
            }
        }
    }

    activeRecordings.clear();
    recording = false;
}

void RecordingManager::discardRecording()
{
    if (!recording) return;

    audioEngine.setRecordingActive(false);

    // Punch From Retro: retro キャプチャごと破棄 (retro ファイルは R 押下前の
    // 遡及区間も含むが、リテイクでは丸ごと録り直すので残さない)
    if (punchFromRetro)
    {
        audioEngine.setRetrospectiveTarget(nullptr);
        retroWriter.reset();
        if (retroTrack) retroTrack->cancelLiveRecording();
        if (retroFile.existsAsFile()) retroFile.deleteFile();
        retroTrack     = nullptr;
        retroFile      = juce::File();
        retroActive    = false;
        punchFromRetro = false;
    }

    // teardown バリア (audio が旧 config を手放すまで待つ) を通してから writer を破棄する
    audioEngine.setRecordingTarget(nullptr, nullptr);

    std::vector<std::unique_ptr<AudioClip>> discarded;
    for (auto& ar : activeRecordings)
    {
        ar.writer.reset();   // フラッシュ・クローズ
        if (!ar.track) continue;

        ar.track->cancelLiveRecording();

        // ループ録音でリアルタイム配置済みのテイクをレーンから所有権ごと外す
        for (auto* clip : ar.realtimeClips)
        {
            if (!clip) continue;
            bool found = false;
            for (int li = 0; li < ar.track->getLaneCount() && !found; ++li)
            {
                auto* lane = ar.track->getLane(li);
                if (!lane) continue;
                for (auto it = lane->clips.begin(); it != lane->clips.end(); ++it)
                    if (it->get() == clip)
                    {
                        discarded.push_back(std::move(*it));
                        lane->clips.erase(it);
                        found = true;
                        break;
                    }
            }
        }

        // 録音ファイルは残さない (best-effort: サムネイルが開いたままの環境では
        // 削除に失敗し得るが、クリップ参照は無いので孤児 WAV が残るだけで無害)
        if (ar.file.existsAsFile()) ar.file.deleteFile();
    }

    // 再生スナップショットが生参照している可能性があるため即破棄せず遅延破棄へ
    if (!discarded.empty())
        audioEngine.deferClipDestruction(std::move(discarded));

    activeRecordings.clear();
    recording = false;
}

void RecordingManager::onLoopWrap()
{
    if (!recording) return;
    for (auto& ar : activeRecordings)
    {
        if (!ar.loopRec || !ar.writer || !ar.track) continue;

        const double loopDur = ar.loopEnd - ar.loopStart;
        if (loopDur < 0.05) continue;

        // テイクの位置/尺/fileOffset は純関数 loopTakeSlice (ヘッダ) に一本化。
        // 停止時スライス (stopRecording) と RecordingTests も同じ式を使う。
        // ファイル先頭はエンジンの実書き込み開始位置 (登録遅れ対策・stopRecording と同じ)
        const int it = ar.takesAddedRealtime;
        const double fileStart = audioEngine.getRecordingFirstWritePosSecs(ar.fileStartPos);
        const auto slice = loopTakeSlice(it, ar.startPosition, fileStart,
                                         ar.loopStart, ar.loopEnd);

        const auto p = compensateLatency(slice.pos, slice.dur, slice.fileOffset,
                                         activeLatencyComp, slice.pos);
        if (p.dur > 0.01)
        {
            // 空いている既存テイクレーンを再利用 (通常録音の backupToTakeLane と同じ)。
            // lastTakeLaneIdx + 1 から探すので、セッション内では必ず下方向へ積まれる
            const int laneIdx = ar.track->findFreeTakeLaneIndex(
                p.start, p.start + p.dur, ar.lastTakeLaneIdx + 1);
            auto* lane = ar.track->getLane(laneIdx);
            auto* clip = lane ? lane->addClip(ar.file, p.start, p.dur,
                                              ar.track->getFormatManager(),
                                              ar.track->getThumbnailCache())
                              : nullptr;
            if (clip)
            {
                clip->setFileOffset(p.fileOffset);
                ar.realtimeClips.push_back(clip);
                ar.lastTakeLaneIdx = laneIdx;
            }
        }

        ++ar.takesAddedRealtime;

        // 2 周目以降のライブ波形オーバーレイ (録音バー) はループ頭から表示する。
        // liveBuffer は AudioEngine がラップ時に reset 済みなので、表示開始位置だけ進める。
        // カウントインのリードは 1 周目だけのものなので落とすが、レイテンシ補正分は残す
        // (ラップ直後のバッファ先頭 comp 秒は前の周回の歌い終わりで、確定テイクの
        //  fileOffset にも comp が乗るため、隠すのが確定後の波形と一致する表示)
        ar.track->setRecordingStartPos(ar.loopStart);
        ar.track->setLiveBufferLeadSecs(activeLatencyComp);
    }
}

bool RecordingManager::startRetrospective(Track* targetTrack, double playStartSec)
{
    if (retroActive) return false;
    if (!targetTrack) return false;

    const double sampleRate = audioEngine.getSampleRate();
    juce::WavAudioFormat wavFormat;
    const int bits = recordingBitDepth;
    const auto sampleFormat = (bits >= 32)
        ? juce::AudioFormatWriterOptions::SampleFormat::floatingPoint
        : juce::AudioFormatWriterOptions::SampleFormat::integral;

    auto file   = createRecordingFile(targetTrack->getName() + "_retro");
    auto stream = std::make_unique<juce::FileOutputStream>(file);
    if (!stream->openedOk()) return false;

    const int numCh = targetTrack->isStereo() ? 2 : 1;
    auto opts = juce::AudioFormatWriterOptions{}
                    .withSampleRate(sampleRate)
                    .withNumChannels((juce::uint32) numCh)
                    .withBitsPerSample(bits)
                    .withSampleFormat(sampleFormat);
    std::unique_ptr<juce::OutputStream> outStream = std::move(stream);
    auto writer = wavFormat.createWriterFor(outStream, opts);
    if (writer == nullptr) return false;

    auto tw = std::make_unique<juce::AudioFormatWriter::ThreadedWriter>(
        writer.release(), backgroundThread, 65536);

    // R 押下までは波形を表示しない（liveBuffer 無し）。書き出しのみ裏で行う。
    audioEngine.setRetrospectiveTarget(tw.get(),
                                       /*liveBuf*/ nullptr,
                                       targetTrack->getInputChannel(),
                                       targetTrack->isStereo());

    retroTrack       = targetTrack;
    retroFile        = file;
    retroPlayStart   = playStartSec;
    retroStereo      = targetTrack->isStereo();
    retroLatencyComp = audioEngine.getRecordingLatencyCompSecs();
    retroWriter      = std::move(tw);
    retroActive      = true;
    return true;
}

void RecordingManager::stopRetrospective(bool commit, double playEndSec)
{
    if (!retroActive) return;

    audioEngine.setRetrospectiveTarget(nullptr);
    retroWriter.reset();  // フラッシュ・クローズ

    // ライブ波形オーバーレイをクリア
    if (retroTrack) retroTrack->cancelLiveRecording();

    if (commit && retroTrack && retroFile.existsAsFile())
    {
        // ファイル先頭 = 実書き込み開始位置 (登録遅れ対策・クリップ左端もそこへ置く)
        const double fileStart = audioEngine.getRetroFirstWritePosSecs(retroPlayStart);
        const double dur = playEndSec - fileStart;
        const auto p = compensateLatency(fileStart, dur, 0.0, retroLatencyComp,
                                         fileStart);
        if (dur > 0.05 && p.dur > 0.01)
        {
            auto* lane = retroTrack->getLane(0);
            if (lane)
            {
                auto* clip = lane->addClip(retroFile, p.start, p.dur,
                              retroTrack->getFormatManager(),
                              retroTrack->getThumbnailCache());
                // 録音直後のファイルはキャッシュが古い/未完なので必ず再読込
                if (clip)
                {
                    clip->setFileOffset(p.fileOffset);
                    clip->refreshThumbnail();
                }
            }
        }
    }
    else
    {
        if (retroFile.existsAsFile()) retroFile.deleteFile();
    }

    retroTrack    = nullptr;
    retroFile     = juce::File();
    retroActive   = false;
}
