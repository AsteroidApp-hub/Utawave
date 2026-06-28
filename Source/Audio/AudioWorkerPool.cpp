// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

#include "AudioWorkerPool.h"

void AudioWorkerPool::start(int numWorkers)
{
    stop();
    if (numWorkers <= 0) return;

    workers.reserve((size_t) numWorkers);
    for (int i = 0; i < numWorkers; ++i)
    {
        auto w = std::make_unique<Worker>(*this, i);
        // オーディオ補助スレッドなので最優先に近い優先度で動かす (メッセージスレッド等に
        // 押しのけられて完了待ちが伸びるのを防ぐ)。realtime までは要求しない (移植性優先)。
        w->startThread(juce::Thread::Priority::highest);
        workers.push_back(std::move(w));
    }
}

void AudioWorkerPool::stop()
{
    for (auto& w : workers) { w->signalThreadShouldExit(); w->wake.signal(); }
    for (auto& w : workers) w->stopThread(2000);
    workers.clear();
}

void AudioWorkerPool::workLoop()
{
    int j;
    while ((j = nextJob.fetch_add(1, std::memory_order_acq_rel)) < curCount)
        curFn(curCtx, j);
}

void AudioWorkerPool::parallelFor(int count, JobFn fn, void* ctx)
{
    if (count <= 0) return;

    // 1 件のみ / ワーカー無し → その場で直列 (起床コストを払わない)
    if (count == 1 || workers.empty())
    {
        for (int i = 0; i < count; ++i) fn(ctx, i);
        return;
    }

    // 今回起こすワーカー数 = min(ワーカー数, count-1)。audio スレッド自身も 1 参加するので
    // count-1 本あれば足り、不要なワーカーを起こして待たずに済む。
    const int toSignal = juce::jmin((int) workers.size(), count - 1);

    curFn   = fn;
    curCtx  = ctx;
    curCount = count;
    workersDone.store(0, std::memory_order_relaxed);
    nextJob.store(0, std::memory_order_release);   // 最後に公開 (acquire 側が fn/ctx/count を見える)

    for (int i = 0; i < toSignal; ++i) workers[(size_t) i]->wake.signal();

    workLoop();   // 呼び出し元 (audio スレッド) も処理に参加

    // 起こした全ワーカーが workLoop を抜ける (= workersDone に達する) まで待つ。返った時点で
    // どのワーカーも workLoop の中におらず、次の dispatch の setup と重ならない (cross-block レース排除)。
    while (workersDone.load(std::memory_order_acquire) < toSignal)
        juce::Thread::yield();
}

void AudioWorkerPool::Worker::run()
{
    while (! threadShouldExit())
    {
        wake.wait();                 // 次ブロックの signal まで休眠 (CPU を食わない)
        if (threadShouldExit()) break;
        pool.workLoop();
        pool.workersDone.fetch_add(1, std::memory_order_release);  // 完了を親へ通知
    }
}
