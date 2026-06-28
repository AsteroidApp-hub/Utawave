// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave
//
// Utawave — RT セーフなワーカープール (AudioWorkerPool) のユニットテスト。
// 核心は「parallelFor が [0,count) の各 index をちょうど 1 回呼ぶ」こと (取りこぼし/二重処理が
// 無い)。様々な count・繰り返し (ストレス)・ワーカー 0 本 (直列フォールバック)・再起動を検証する。
// 実スレッドのレースを直接は検めないが、被覆 (各 index ちょうど 1 回 = ジョブ重複/欠落) は検出できる。

#include <JuceHeader.h>
#include "../Source/Audio/AudioWorkerPool.h"
#include <vector>
#include <atomic>

namespace
{
    struct Ctx
    {
        std::vector<int>* hits;   // index ごとの呼ばれた回数
        std::vector<int>* vals;   // index ごとに書いた値 (i*2+1)
        std::atomic<int>* total;  // 全呼び出し回数
    };

    void job(void* vctx, int i)
    {
        auto& c = *static_cast<Ctx*>(vctx);
        (*c.hits)[(size_t) i] += 1;          // 各 index は 1 スレッドのみが触る前提
        (*c.vals)[(size_t) i] = i * 2 + 1;
        c.total->fetch_add(1, std::memory_order_relaxed);
    }
}

class AudioWorkerPoolTests : public juce::UnitTest
{
public:
    AudioWorkerPoolTests() : juce::UnitTest("AudioWorkerPool") {}

    // count 件を 1 回 parallelFor し、被覆 (各 index ちょうど 1 回) と書込値を検証する。
    bool runOnce(AudioWorkerPool& pool, int count)
    {
        std::vector<int> hits((size_t) juce::jmax(0, count), 0);
        std::vector<int> vals((size_t) juce::jmax(0, count), -1);
        std::atomic<int> total { 0 };
        Ctx ctx { &hits, &vals, &total };
        pool.parallelFor(count, job, &ctx);

        if (total.load() != juce::jmax(0, count)) return false;
        for (int i = 0; i < count; ++i)
        {
            if (hits[(size_t) i] != 1)        return false;   // 欠落 / 二重処理
            if (vals[(size_t) i] != i * 2 + 1) return false;  // 書込ミス
        }
        return true;
    }

    void runTest() override
    {
        beginTest("parallelFor covers every index exactly once (various counts)");
        {
            AudioWorkerPool pool;
            pool.start(4);
            expect(pool.getNumWorkers() == 4, "4 workers started");
            for (int count : { 0, 1, 2, 3, 5, 8, 17, 64, 257, 1000, 4096 })
                expect(runOnce(pool, count), "coverage for count=" + juce::String(count));
            pool.stop();
            expect(pool.getNumWorkers() == 0, "workers stopped");
        }

        beginTest("stress: many repeated dispatches stay correct");
        {
            AudioWorkerPool pool;
            pool.start(juce::jmax(2, juce::SystemStats::getNumCpus() - 1));
            bool ok = true;
            for (int iter = 0; iter < 400 && ok; ++iter)
                ok = runOnce(pool, 200 + (iter % 50));   // count を少し変えながら連打
            expect(ok, "400 repeated dispatches all covered correctly");
            pool.stop();
        }

        beginTest("no workers (start(0)) falls back to inline serial");
        {
            AudioWorkerPool pool;
            pool.start(0);
            expect(pool.getNumWorkers() == 0, "no worker threads");
            expect(runOnce(pool, 500), "inline serial still covers all indices");
        }

        beginTest("never started pool runs inline");
        {
            AudioWorkerPool pool;   // start を呼ばない
            expect(runOnce(pool, 123), "inline without start works");
        }

        beginTest("restart works");
        {
            AudioWorkerPool pool;
            pool.start(2);
            expect(runOnce(pool, 300), "first run");
            pool.start(3);   // 再 start (内部で stop → 作り直し)
            expect(pool.getNumWorkers() == 3, "restarted with 3 workers");
            expect(runOnce(pool, 300), "second run after restart");
            pool.stop();
        }

        beginTest("results match a serial computation (sum)");
        {
            AudioWorkerPool pool;
            pool.start(4);
            const int count = 2000;
            std::vector<int> hits((size_t) count, 0), vals((size_t) count, -1);
            std::atomic<int> total { 0 };
            Ctx ctx { &hits, &vals, &total };
            pool.parallelFor(count, job, &ctx);

            long long parallelSum = 0, serialSum = 0;
            for (int i = 0; i < count; ++i) { parallelSum += vals[(size_t) i]; serialSum += i * 2 + 1; }
            expect(parallelSum == serialSum, "parallel-produced values sum to the serial result");
            pool.stop();
        }
    }
};

static AudioWorkerPoolTests audioWorkerPoolTests;
