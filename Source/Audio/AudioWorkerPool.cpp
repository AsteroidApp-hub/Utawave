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
    {
        curFn(curCtx, j);
        remaining.fetch_sub(1, std::memory_order_acq_rel);
    }
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

    curFn   = fn;
    curCtx  = ctx;
    curCount = count;
    remaining.store(count, std::memory_order_relaxed);
    nextJob.store(0, std::memory_order_release);   // 最後に公開 (acquire 側が fn/ctx/count を見える)

    for (auto& w : workers) w->wake.signal();

    workLoop();   // 呼び出し元 (audio スレッド) も処理に参加

    // ワーカーが掴んで未完了のジョブが drain するまで待つ (軽い yield スピン)。
    // audio スレッドが自分の分を終えた後の残りだけなので短時間。
    while (remaining.load(std::memory_order_acquire) > 0)
        juce::Thread::yield();
}

void AudioWorkerPool::Worker::run()
{
    while (! threadShouldExit())
    {
        wake.wait();                 // 次ブロックの signal まで休眠 (CPU を食わない)
        if (threadShouldExit()) break;
        pool.workLoop();
    }
}
