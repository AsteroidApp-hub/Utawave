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
    return folder.getChildFile(name);
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
        // R 押下時点からライブ波形オーバーレイを表示開始
        retroTrack->startLiveRecording(recStartSec);
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

        // オーバーレイ表示は R 押下位置から。カウントイン/プリロールの先行録音分は
        // リード (非表示) としてバッファ先頭を読み飛ばす
        track->startLiveRecording(recStartSec, recStartSec - writeFrom);

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
        // テイクの開始レーン = 「クリップを持つ最後のレーン」の次。末尾の空レーン
        // (テイクを削除した跡など) は再利用し、録り直すたび下へ際限なく増えないようにする
        {
            int takeStart = 1;
            for (int li = track->getLaneCount() - 1; li >= 1; --li)
                if (auto* l = track->getLane(li); l != nullptr && !l->clips.empty())
                {
                    takeStart = li + 1;
                    break;
                }
            ar.takeStartLaneIdx = takeStart;
        }
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

void RecordingManager::stopRecording(double endPositionSeconds)
{
    if (!recording) return;

    audioEngine.setRecordingActive(false);

    // ── Punch From Retro: retro ライターをそのまま終了し、offset 付きクリップで配置 ──
    // (他のアームトラックが併走録音していれば activeRecordings 側にあり、下の通常処理で配置)
    if (punchFromRetro)
    {
        audioEngine.setRetrospectiveTarget(nullptr);
        retroWriter.reset();  // フラッシュ・クローズ

        const double fileStart = retroPlayStart;
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
                if (lane && p.dur > 0.01)
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

        // カウントイン/プリロールの先行録音分 (ファイル先頭の読み飛ばし量)。
        // クリップ左端を伸ばすとこの区間 (ブレス等) を復元できる
        const double preRecDur = juce::jmax(0.0, ar.startPosition - ar.fileStartPos);

        if (!ar.loopRec)
        {
            const auto p = compensateLatency(ar.startPosition, dur, preRecDur,
                                             activeLatencyComp, ar.startPosition);
            if (p.dur > 0.01 && ar.file.existsAsFile())
                ar.track->finishLiveRecording(ar.file, p.start, p.dur, p.fileOffset);
            else
                ar.track->cancelLiveRecording();
            continue;
        }

        // ── ループ録音: ファイルをループ区間ごとにスライスして Take レーンへ ──
        ar.track->cancelLiveRecording();

        const double loopDur = ar.loopEnd - ar.loopStart;
        if (loopDur < 0.05 || dur < 0.05 || !ar.file.existsAsFile()) continue;

        // テイクの位置/フル尺/fileOffset は純関数 loopTakeSlice (ヘッダ・onLoopWrap と共通) で
        // 求め、ここでは「録り切れた分」への尺クランプだけを行う。
        // dur (書き込みサンプル数) はカウントイン/プリロールの先行録音分 (preRecDur) を含む
        const double firstPassDur  = juce::jmax(0.0, ar.loopEnd - ar.startPosition);
        const double durFromStart  = juce::jmax(0.0, dur - preRecDur);

        // リアルタイム配置済みの take は再追加しない
        const int alreadyAdded = ar.takesAddedRealtime;

        // Take 1（まだ配置されていなければ）= 録音開始位置から 1 周目末尾まで
        // (1 周目の途中で停止した場合は録音できた所まで)
        if (alreadyAdded == 0)
        {
            const auto s0 = loopTakeSlice(0, ar.startPosition, ar.fileStartPos,
                                          ar.loopStart, ar.loopEnd);
            const double take1Dur = juce::jmin(durFromStart, s0.dur);
            auto* lane = ar.track->ensureLane(ar.takeStartLaneIdx);
            const auto p = compensateLatency(s0.pos, take1Dur, s0.fileOffset,
                                             activeLatencyComp, s0.pos);
            if (lane && p.dur > 0.01)
            {
                auto* clip = lane->addClip(ar.file, p.start, p.dur,
                                            ar.track->getFormatManager(),
                                            ar.track->getThumbnailCache());
                if (clip)
                {
                    clip->setFileOffset(p.fileOffset);
                    clip->refreshThumbnail();
                }
            }
        }

        // 2周目以降の周回（まだ配置されていないものだけ追加）。位置/fileOffset は
        // loopTakeSlice、尺は録り切れた分 (rest) にクランプ
        const double rest = juce::jmax(0.0, durFromStart - firstPassDur);
        const int numRestPasses = (rest > 0.0) ? (int)std::ceil(rest / loopDur) : 0;
        for (int it = juce::jmax(1, alreadyAdded); it <= numRestPasses; ++it)
        {
            const auto sit = loopTakeSlice(it, ar.startPosition, ar.fileStartPos,
                                           ar.loopStart, ar.loopEnd);
            const double inRestOffset = (double)(it - 1) * loopDur;
            const double sliceDur = juce::jmin(sit.dur, rest - inRestOffset);
            if (sliceDur < 0.05) continue;

            auto* lane = ar.track->ensureLane(ar.takeStartLaneIdx + it);
            if (!lane) continue;
            const auto p = compensateLatency(sit.pos, sliceDur, sit.fileOffset,
                                             activeLatencyComp, sit.pos);
            if (p.dur < 0.01) continue;
            auto* clip = lane->addClip(ar.file, p.start, p.dur,
                                       ar.track->getFormatManager(),
                                       ar.track->getThumbnailCache());
            if (clip)
            {
                clip->setFileOffset(p.fileOffset);
                clip->refreshThumbnail();
            }
        }
    }

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
        // 停止時スライス (stopRecording) と RecordingTests も同じ式を使う
        const int it = ar.takesAddedRealtime;
        const auto slice = loopTakeSlice(it, ar.startPosition, ar.fileStartPos,
                                         ar.loopStart, ar.loopEnd);

        int laneIdx = ar.takeStartLaneIdx + it;
        auto* lane  = ar.track->ensureLane(laneIdx);
        if (!lane) continue;
        const auto p = compensateLatency(slice.pos, slice.dur, slice.fileOffset,
                                         activeLatencyComp, slice.pos);
        if (p.dur > 0.01)
        {
            auto* clip = lane->addClip(ar.file, p.start, p.dur,
                                       ar.track->getFormatManager(),
                                       ar.track->getThumbnailCache());
            if (clip)
            {
                clip->setFileOffset(p.fileOffset);
                ar.realtimeClips.push_back(clip);
            }
        }

        ++ar.takesAddedRealtime;

        // 2 周目以降のライブ波形オーバーレイ (録音バー) はループ頭から表示する。
        // liveBuffer は AudioEngine がラップ時に reset 済みなので、表示開始位置だけ進める。
        // カウントインのリード (非表示先行録音分) は 1 周目だけのものなので 0 に戻す
        ar.track->setRecordingStartPos(ar.loopStart);
        ar.track->setLiveBufferLeadSecs(0.0);
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
        const double dur = playEndSec - retroPlayStart;
        const auto p = compensateLatency(retroPlayStart, dur, 0.0, retroLatencyComp,
                                         retroPlayStart);
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
