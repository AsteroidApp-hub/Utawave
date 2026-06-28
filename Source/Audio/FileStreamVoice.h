// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

#pragma once
#include <JuceHeader.h>
#include <atomic>
#include <memory>

/**
    ディスクストリーミングの「先読みボイス」(1 ファイルに 1 つ)。

    バックグラウンドの共有 TimeSliceThread が、audio スレッドの読み出し位置 (wantFrom) を追って
    SPSC リングへ先読みする。audio スレッドはリングがウォームなら **ロックフリーにコピー** し、
    まだ読めていない領域 (シーク直後 / ループラップ / クロスフェードの 2 本目同時読み等) は
    **呼び出し元が渡す同期 reader へフォールバック** する。これにより:
      - 通常の順次再生は audio スレッドのディスク I/O がゼロになる (先読みヒット)
      - ミス時も「今までと同じ同期読み」= プチノイズ無し・出力は常にビット正確

    スレッドモデル (SPSC):
      - producer = bg スレッド (useTimeSlice): ring へ書き込み、residentStart/End を release 公開
      - consumer = audio スレッド (read): residentStart/End を acquire 読み、wantFrom を release 公開
    producer は consumer が読む領域 [wantFrom, wantFrom+block) のスロットを書かない (lookahead < capacity
    を保証し、再利用は capacity 先のスロットだけ) ため、ロック無しで安全。所有 reader は bg 専用、
    フォールバック reader は audio 専用 (互いに別インスタンス) なので reader の seek 競合も無い。
*/
class FileStreamVoice : private juce::TimeSliceClient
{
public:
    // bgReader        : 先読み用 (このクラスが所有・bg スレッド専用)
    // fallbackReader  : ミス時の同期読み用 (所有しない・audio スレッド専用に呼ばれる前提)
    // thread          : 開始済みの共有先読みスレッド
    // capacitySamples : リング容量。lookaheadSamples の 2 倍以上にすること
    FileStreamVoice(std::unique_ptr<juce::AudioFormatReader> bgReader,
                    juce::AudioFormatReader* fallbackReader,
                    juce::TimeSliceThread& thread,
                    int capacitySamples  = 1 << 18,   // 262144 ≈ 5.4s @48k
                    int lookaheadSamples = 1 << 16);  // 65536  ≈ 1.4s @48k
    ~FileStreamVoice() override;

    int getNumChannels() const noexcept { return chans; }

    // audio スレッド: dest の [destOffset, destOffset+num) に file [startSample, …) を満たす。
    // reader->read(&dest, destOffset, num, startSample, true, hasRight) と同じ結果を返す
    // (ヒット時はリングから、ミス時はフォールバック reader から)。常に成功する。
    void read(juce::AudioBuffer<float>& dest, int destOffset, int num, juce::int64 startSample);

    // 観測用 (テスト/デバッグ): リングから供給できた read 回数 / 全 read 回数。
    juce::int64 getRingHits()  const noexcept { return ringHits.load(std::memory_order_relaxed); }
    juce::int64 getReadCalls() const noexcept { return readCalls.load(std::memory_order_relaxed); }

private:
    int useTimeSlice() override;  // bg 先読み

    std::unique_ptr<juce::AudioFormatReader> bg;       // bg 専用 reader (所有)
    juce::AudioFormatReader*                 fallback; // audio 専用 reader (非所有)
    juce::TimeSliceThread&                   thread;

    const int          capacity;
    const int          lookahead;
    const int          chans;
    const juce::int64  fileLen;

    juce::AudioBuffer<float> ring;      // chans × capacity (循環: file sample F → 列 F % capacity)
    juce::AudioBuffer<float> fillTmp;   // bg の read 用一時 (chans × fillChunk)
    static constexpr int kFillChunk = 1 << 14;  // 16384

    // SPSC で共有する位置 (file sample 単位)。
    std::atomic<juce::int64> residentStart { 0 };  // producer 公開: リング有効域の先頭
    std::atomic<juce::int64> residentEnd   { 0 };  // producer 公開: リング有効域の末尾 (排他)
    std::atomic<juce::int64> wantFrom      { -1 }; // consumer 公開: いま欲しい先頭位置

    std::atomic<juce::int64> ringHits  { 0 };
    std::atomic<juce::int64> readCalls { 0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(FileStreamVoice)
};
