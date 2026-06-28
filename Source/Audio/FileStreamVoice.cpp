// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

#include "FileStreamVoice.h"
#include <algorithm>

FileStreamVoice::FileStreamVoice(std::unique_ptr<juce::AudioFormatReader> bgReader,
                                 std::unique_ptr<juce::AudioFormatReader> fallbackReader,
                                 juce::TimeSliceThread& sharedThread,
                                 int capacitySamples,
                                 int lookaheadSamples)
    : bg(std::move(bgReader)),
      fallback(std::move(fallbackReader)),
      thread(sharedThread),
      // 容量は「lookahead + 充填チャンク」を必ず上回らせる (producer が consumer の読む
      // スロットを上書きしない不変条件のため)。下限を kFillChunk の数倍にして、小さい
      // capacitySamples を渡されても下の jlimit が lo>hi にならないようにする。
      capacity(juce::jmax(kFillChunk * 4, capacitySamples)),
      lookahead(juce::jlimit(1 << 10, juce::jmax(1 << 10, capacity - kFillChunk - 1), lookaheadSamples)),
      chans(bg ? juce::jmin(2, (int) bg->numChannels) : 1),
      fileLen(bg ? bg->lengthInSamples : 0)
{
    ring.setSize(chans, capacity);
    ring.clear();
    fillTmp.setSize(chans, kFillChunk);
    thread.addTimeSliceClient(this);
}

FileStreamVoice::~FileStreamVoice()
{
    // bg スレッドがこのインスタンスの useTimeSlice を実行中なら戻るまで待つ (メンバ破棄前のバリア)。
    thread.removeTimeSliceClient(this);
}

void FileStreamVoice::read(juce::AudioBuffer<float>& dest, int destOffset, int num, juce::int64 startSample)
{
    if (num <= 0) return;
    readCalls.fetch_add(1, std::memory_order_relaxed);

    // いま欲しい位置を producer へ通知 (先読みの追従先)。
    wantFrom.store(startSample, std::memory_order_release);

    const juce::int64 re = residentEnd.load(std::memory_order_acquire);
    const juce::int64 rs = residentStart.load(std::memory_order_acquire);

    const int destCh = dest.getNumChannels();
    const int nCopy  = juce::jmin(destCh, chans);

    if (startSample >= rs && startSample + num <= re)
    {
        // ── リングヒット: ロックフリーにコピー (producer は [re, …) しか書かないので競合無し) ──
        const int idx   = (int) (startSample % capacity);
        const int first = juce::jmin(num, capacity - idx);
        for (int c = 0; c < nCopy; ++c)
        {
            dest.copyFrom(c, destOffset, ring, c, idx, first);
            if (num > first)
                dest.copyFrom(c, destOffset + first, ring, c, 0, num - first);
        }
        for (int c = nCopy; c < destCh; ++c)
            dest.clear(c, destOffset, num);   // reader->read と同じく余剰チャンネルは 0
        ringHits.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    // ── ミス (シーク / ループラップ / 未ウォーム): 同期フォールバック (= 従来の挙動・正確) ──
    if (fallback != nullptr)
        fallback->read(&dest, destOffset, num, startSample, true, destCh > 1);
    else
        for (int c = 0; c < destCh; ++c) dest.clear(c, destOffset, num);
}

int FileStreamVoice::useTimeSlice()
{
    const juce::int64 want = wantFrom.load(std::memory_order_acquire);
    if (want < 0 || want >= fileLen) return 20;   // 未開始 / EOF: のんびり

    juce::int64 rs = residentStart.load(std::memory_order_relaxed);
    juce::int64 re = residentEnd.load(std::memory_order_relaxed);

    if (want < rs || want > re)
    {
        // シーク: まず有効域を want へ畳む (consumer は acquire で見てフォールバックへ落ちる)。
        rs = re = want;
        residentStart.store(rs, std::memory_order_release);
        residentEnd.store(re, std::memory_order_release);
    }
    else if (want > rs)
    {
        rs = want;                                         // 消費済みを捨てる
        residentStart.store(rs, std::memory_order_release);
    }

    const juce::int64 target = std::min<juce::int64>(want + lookahead, fileLen);
    if (re >= target) return 10;                           // 充分先読み済み

    const int chunk = (int) std::min<juce::int64>({ (juce::int64) kFillChunk,
                                                    target - re, fileLen - re });
    if (chunk <= 0) return 10;

    // [re, re+chunk) を bg reader で読み、ring の循環スロットへ散らす。
    bg->read(&fillTmp, 0, chunk, re, true, chans > 1);
    const int idx   = (int) (re % capacity);
    const int first = juce::jmin(chunk, capacity - idx);
    for (int c = 0; c < chans; ++c)
    {
        ring.copyFrom(c, idx, fillTmp, c, 0, first);
        if (chunk > first)
            ring.copyFrom(c, 0, fillTmp, c, first, chunk - first);
    }
    re += chunk;
    residentEnd.store(re, std::memory_order_release);      // データ書込後に公開

    return (re < target) ? 1 : 10;                         // まだ余地があれば即再呼び出し
}
