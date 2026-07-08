// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

#pragma once
#include <JuceHeader.h>
#include "../Tracks/TrackManager.h"

class AudioEngine;

class RecordingManager
{
public:
    RecordingManager(AudioEngine& engine, TrackManager& tracks,
                     juce::AudioFormatManager& fmt);
    ~RecordingManager();

    // recStartSec: 実際に録音を開始するタイムライン位置
    // playFromSec: 再生（カウントイン/プレロール込み）を開始する位置（recStart 以下）
    // loopRecording: ループ録音モード（停止時にループ区間ごとに Take レーンへスライス配置）
    // loopStart/loopEnd: ループ範囲（loopRecording=true 時のみ使用）
    bool startRecording(double recStartSec, double playFromSec = 0.0,
                        bool loopRecording = false,
                        double loopStart = 0.0, double loopEnd = 0.0);
    // takesOnly=true (Q リテイクの「テイクを残す」設定) は確定先をテイクレーンだけにする:
    // Lane 0 への配置 / トリム / クロスフェードを行わない (直後に同じ位置から録り直すため。
    // 残したテイクは Shift+↑ で採用できる)。ループ録音も最後のテイクの Lane 0 配置だけを省く
    void stopRecording(double endPositionSeconds, bool takesOnly = false);
    bool isRecording() const { return recording; }

    // リテイク用: 進行中の録音を破棄する。writer を閉じ、リアルタイム配置済みの
    // テイククリップをレーンから外し (遅延破棄)、録音ファイルも削除する。
    // クリップ配置・テイクバックアップは一切行わない (stopRecording の破棄版)
    void discardRecording();

    // アクティブ録音の R 押下位置 (リテイクの戻り先)。録音中でなければ fallback を返す
    double getActiveRecordStartPos(double fallback) const
    {
        if (punchFromRetro)             return punchInRecStart;
        if (!activeRecordings.empty())  return activeRecordings.front().startPosition;
        return fallback;
    }

    // 直前の startRecording でファイル/writer 作成に失敗したトラック名
    // (空でなければ呼び出し側がユーザーへ通知する。silent failure 防止)
    const juce::StringArray& getLastStartFailures() const { return lastStartFailures; }

    // ループラップ通知（録音中に呼ぶ。完了したループ周回をリアルタイムで Take レーンへ配置）
    void onLoopWrap();

    // 遡及録音
    bool startRetrospective(Track* targetTrack, double playStartSec);
    void stopRetrospective(bool commit, double playEndSec);
    bool hasRetrospective() const { return retroActive; }

    // Recordings folder（コールバック未設定時のフォールバック）
    juce::File getRecordingsFolder() const;
    // プロジェクト連携用: 設定すると録音先がここから取得される
    std::function<juce::File()> getAudioFolder;

    // 録音 WAV のビット深度 (16/24 = PCM, 32 = float)
    void setBitDepth(int bits)
    {
        recordingBitDepth = (bits == 16 || bits == 24 || bits == 32) ? bits : 24;
    }
    int  getBitDepth() const { return recordingBitDepth; }

    // 録音レイテンシ補正: クリップ配置 (start, dur, fileOffset) を compSecs だけ手前へ
    // ずらす純関数。開始が floorStart (既定 0 = タイムライン先頭) を割る場合はそこへ
    // クランプし、不足分はファイル先頭を fileOffset で読み飛ばして内容の整合を保つ
    // (尺はその分縮む)。録音の配置では floorStart に「音楽的なアンカー」(R 押下位置 /
    // ループ頭) を渡し、クリップ左端が補正量ぶんアンカーより前へ飛び出すのを防ぐ
    // (トリム分はファイルに残っているので左端を伸ばせば復元できる)。
    // ヘッダインライン定義 (RecordingTests がリンク無しで検証できるように)
    struct ClipPlacement { double start; double dur; double fileOffset; };
    static ClipPlacement compensateLatency(double start, double dur,
                                           double fileOffset, double compSecs,
                                           double floorStart = 0.0)
    {
        ClipPlacement p { start - compSecs, dur, fileOffset };
        const double floorPos = juce::jmax(0.0, floorStart);
        if (p.start < floorPos)
        {
            // アンカーより前には置けない分はファイル先頭を読み飛ばして整合させる
            const double residual = floorPos - p.start;
            p.start = floorPos;
            p.fileOffset += residual;
            p.dur = juce::jmax(0.0, p.dur - residual);
        }
        return p;
    }

    // ループ録音のテイクスライス計算 (純関数・ユニットテスト対象)。
    // it 番目 (0-based) のテイクのタイムライン位置 / フル尺 / fileOffset を返す
    // (停止時に録り切れていない分の尺クランプは呼び出し側で行う)。
    //  - startPosition: R 押下位置 (Take 1 の開始。ループ内の途中でも前でも良い)
    //  - fileStartPos : ファイルのサンプル 0 のタイムライン位置 (カウントイン/プリロールの
    //                   遡及録音で startPosition より手前になる。先行分 = fileOffset に乗る)
    //  - it == 0: [startPosition, loopEnd]。it >= 1: ループ頭にフル尺、fileOffset は
    //    1 周目の実録音長 (loopEnd - startPosition) を基準に loopDur ずつ累積
    struct TakeSlice { double pos; double dur; double fileOffset; };
    static TakeSlice loopTakeSlice(int it, double startPosition, double fileStartPos,
                                   double loopStart, double loopEnd)
    {
        const double loopDur      = loopEnd - loopStart;
        const double firstPassDur = juce::jmax(0.0, loopEnd - startPosition);
        const double preRecDur    = juce::jmax(0.0, startPosition - fileStartPos);
        if (it <= 0)
            return { startPosition, firstPassDur, preRecDur };
        return { loopStart, loopDur,
                 preRecDur + firstPassDur + (double)(it - 1) * loopDur };
    }

private:
    juce::File createRecordingFile(const juce::String& trackName) const;

    AudioEngine&                      audioEngine;
    TrackManager&                     trackManager;
    juce::AudioFormatManager&         formatManager;

    juce::TimeSliceThread             backgroundThread { "RecordingThread" };

    struct ActiveRecording
    {
        Track* track { nullptr };
        juce::File file;
        double startPosition { 0.0 };  // recStart (パンチイン位置 = クリップを置く位置)
        // ファイルのサンプル 0 に対応するタイムライン位置。カウントイン/プリロール時は
        // その開始位置 (= 再生開始位置) から遡及的に録るため startPosition より手前になる
        double fileStartPos  { 0.0 };
        std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter> writer;
        // ループ録音
        bool loopRec { false };
        double loopStart { 0.0 };
        double loopEnd   { 0.0 };
        juce::int64 wallStartMs { 0 };
        // リアルタイムに作成済みのテイク数（ループラップ毎に増える）
        int  takesAddedRealtime { 0 };
        // この録音セッションで最後にテイクを置いたレーン index (0 = 未配置)。
        // 各テイクは findFreeTakeLaneIndex(…, lastTakeLaneIdx + 1) で「空いている既存
        // テイクレーンを再利用しつつ、セッション内では必ず下方向へ積む」(テイクの
        // 上から下 = 時系列順を保ち、stopRecording の Undo のレーン昇順前提とも整合)
        int  lastTakeLaneIdx    { 0 };
        // リアルタイムに追加された AudioClip 群（停止時にサムネイルを再読込する）
        std::vector<class AudioClip*> realtimeClips;
    };

    std::vector<ActiveRecording> activeRecordings;
    juce::StringArray lastStartFailures;
    bool recording { false };
    int  recordingBitDepth { 24 };

    // 録音開始時に確定したレイテンシ補正量 (秒)。録音中のデバイス変更に影響されないよう
    // start 時に AudioEngine::getRecordingLatencyCompSecs() をスナップショットする
    double activeLatencyComp { 0.0 };

    // 遡及録音
    Track*     retroTrack    { nullptr };
    juce::File retroFile;
    double     retroPlayStart { 0.0 };
    bool       retroActive   { false };
    bool       retroStereo   { false };
    double     retroLatencyComp { 0.0 };   // startRetrospective 時に確定した補正量 (秒)
    std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter> retroWriter;

    // 遡及録音 → 通常録音 (Punch from Retro)
    bool       punchFromRetro { false };
    double     punchInRecStart { 0.0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(RecordingManager)
};
