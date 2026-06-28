// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

#pragma once
#include <JuceHeader.h>
#include <atomic>
#include <vector>
#include <memory>

/**
    オーディオコールバックから使う **リアルタイムセーフな固定ワーカープール**。

    トラック単位の独立処理 (クリップ描画 + プラグインチェーン + PDC) を複数コアへ分散するための
    最小プリミティブ。`parallelFor(count, fn, ctx)` は [0,count) の各 index に対し fn を **ちょうど 1 回**
    呼び、全完了までブロックする。呼び出し元 (audio スレッド) 自身も 1 ワーカーとして処理に参加するため、
    ワーカーが起きるのが遅くても audio スレッドだけで完遂できる (= 取りこぼし無し)。

    RT 安全性:
      - ホットパス (parallelFor / workLoop) で **メモリ確保・mutex を一切行わない**。
        ディスパッチは atomic (nextJob / remaining) + ワーカー起床用の WaitableEvent のみ。
      - ジョブは `void(*)(void*, int)` の生関数ポインタ + void* ctx で渡す (std::function は確保しうるため不可)。
      - ワーカーはブロック間 WaitableEvent で待機 (スピンせず CPU を食わない)。完了待ちは軽い yield スピン。
      - 1 ブロックの全ジョブが drain (remaining==0) してから parallelFor が返るので、次ブロックの
        セットアップとワーカーのジョブ実行が重ならない (ABA・取りこぼし無し)。

    注意: ワーカースレッドからプラグインの processBlock を呼ぶため、呼び出し側はトラックごとに
    **独立した状態** (別バッファ・別 reader/voice・別チェーンインスタンス) だけを触ること。
*/
class AudioWorkerPool
{
public:
    using JobFn = void (*)(void* ctx, int index);

    AudioWorkerPool() = default;
    ~AudioWorkerPool() { stop(); }

    // numWorkers 本のワーカーを起動する (audio スレッド以外で・通常 audioDeviceAboutToStart)。
    // 既に起動済みなら一旦 stop してから作り直す。numWorkers<=0 なら何も起動しない (= 常に直列)。
    void start(int numWorkers);
    void stop();

    int getNumWorkers() const noexcept { return (int) workers.size(); }

    // [0,count) を fn(ctx, i) で処理し、全完了までブロックする (audio スレッドも参加)。
    // count<=1 もしくはワーカー無しなら、その場で直列実行する (起床コストを避ける)。
    void parallelFor(int count, JobFn fn, void* ctx);

private:
    struct Worker : public juce::Thread
    {
        explicit Worker(AudioWorkerPool& p, int idx)
            : juce::Thread("uta-audio-worker-" + juce::String(idx)), pool(p) {}
        void run() override;
        AudioWorkerPool&    pool;
        juce::WaitableEvent wake;   // 1 ブロック 1 起床 (auto-reset)
    };

    void workLoop();   // nextJob を fetch_add しながらジョブを消化

    std::vector<std::unique_ptr<Worker>> workers;

    // 現ブロックのジョブ (parallelFor が signal する前にセット → ワーカーは wake/acquire 後に読む)
    JobFn curFn   { nullptr };
    void* curCtx  { nullptr };
    int   curCount { 0 };
    std::atomic<int> nextJob   { 0 };
    std::atomic<int> remaining { 0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AudioWorkerPool)
};
