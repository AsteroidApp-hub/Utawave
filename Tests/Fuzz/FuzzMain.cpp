// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

// Utawave — ファジング・ドライバ (UtawaveFuzz)
//
// 使い方:
//   UtawaveFuzz --list                                 ターゲット一覧
//   UtawaveFuzz <target|all> [--seconds N] [--seed S]  N 秒間ファジング (all は N 秒ずつ順に)
//               [--timeout T] [--out DIR]
//   UtawaveFuzz <target> --repro <file>                保存された入力で 1 回だけ再現
//
// 正しい入力 (シード) をランダムに壊して流し込む「変異ベース」のファザー。ASan/UBSan ビルドで
// 動かすとメモリ破壊・未定義動作がその場で止まる。止まった/固まった入力は --out (既定:
// 一時フォルダ/UtawaveFuzz) に crash-<target>.bin / timeout-<target>.bin として保存し、
// --repro で再現できる。乱数は --seed で固定でき、同じ seed なら同じ入力列になる。
//
// UTAWAVE_LIBFUZZER を定義してビルドすると (clang の -fsanitize=fuzzer)、このドライバの代わりに
// libFuzzer のカバレッジ誘導エンジンで回せる (ターゲットは環境変数 UTAWAVE_FUZZ_TARGET で選ぶ)。

#include "FuzzTargets.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <iostream>
#include <random>
#include <thread>

#if defined(__has_include)
 #if __has_include(<sanitizer/common_interface_defs.h>)
  #include <sanitizer/common_interface_defs.h>
  #define UTAWAVE_HAS_SANITIZER_API 1
 #endif
#endif

namespace utafuzz
{
namespace
{
juce::File gWorkDir;
juce::File gOutDir;

// クラッシュ時に保存する「いま処理中の入力」。シグナルハンドラ/サニタイザの死亡コールバックから
// 読むため、確保済みバッファ + 長さで持つ (ハンドラ内では確保しない)
std::vector<uint8_t> gCurrent;
std::atomic<size_t>  gCurrentLen { 0 };
char gCrashPath[4096]   = {};
char gTimeoutPath[4096] = {};
std::atomic<bool> gDumped { false };

void dumpCurrent(const char* path, const char* why)
{
    if (gDumped.exchange(true)) return;
    if (FILE* f = std::fopen(path, "wb"))
    {
        std::fwrite(gCurrent.data(), 1, gCurrentLen.load(), f);
        std::fclose(f);
    }
    std::fprintf(stderr, "\n==== UtawaveFuzz: %s. input saved to: %s ====\n", why, path);
    std::fflush(stderr);
}

void onDeath() { dumpCurrent(gCrashPath, "crash detected"); }

void onSignal(int sig)
{
    dumpCurrent(gCrashPath, "fatal signal");
    std::signal(sig, SIG_DFL);
    std::raise(sig);
}

void installCrashHandlers()
{
   #if UTAWAVE_HAS_SANITIZER_API
    __sanitizer_set_death_callback(onDeath);
   #endif
    for (int sig : { SIGSEGV, SIGABRT, SIGFPE, SIGILL
                    #ifdef SIGBUS
                     , SIGBUS
                    #endif
                   })
        std::signal(sig, onSignal);
}

void setPaths(const char* target)
{
    std::snprintf(gCrashPath, sizeof(gCrashPath), "%s",
                  gOutDir.getChildFile(juce::String("crash-") + target + ".bin").getFullPathName().toRawUTF8());
    std::snprintf(gTimeoutPath, sizeof(gTimeoutPath), "%s",
                  gOutDir.getChildFile(juce::String("timeout-") + target + ".bin").getFullPathName().toRawUTF8());
}

void runOne(const Target& t, const Bytes& input)
{
    if (gCurrent.size() < input.size()) gCurrent.resize(input.size());
    if (! input.empty()) std::memcpy(gCurrent.data(), input.data(), input.size());
    gCurrentLen.store(input.size());
    t.run(input.data(), input.size());
}

// ───────────────────────── 変異 ─────────────────────────

struct Mutator
{
    std::mt19937_64 rng;
    explicit Mutator(uint64_t seed) : rng(seed) {}

    size_t below(size_t n) { return n == 0 ? 0 : (size_t) (rng() % n); }
    bool   chance(int pct) { return (int) below(100) < pct; }

    static constexpr int64_t kInteresting[] = { 0, 1, -1, 2, 16, 32, 64, 127, 128, 255, 256, 1024, 4096,
                                                32767, 32768, 65535, 65536, 0x7fffffff, (int64_t) 0x80000000LL,
                                                (int64_t) 0xffffffffLL, 0x7fffffffffffffffLL };

    void mutate(Bytes& b, const Target& t, const std::vector<Bytes>& pool)
    {
        const int n = 1 + (int) below(4);
        for (int k = 0; k < n; ++k)
            mutateOnce(b, t, pool);
        if (b.size() > t.maxLen) b.resize(t.maxLen);
    }

    void mutateOnce(Bytes& b, const Target& t, const std::vector<Bytes>& pool)
    {
        switch (below(t.textual ? 11 : 10))
        {
            case 0: if (! b.empty()) b[below(b.size())] ^= (uint8_t) (1u << below(8)); break;              // ビット反転
            case 1: if (! b.empty()) b[below(b.size())] = (uint8_t) rng(); break;                           // ランダムバイト
            case 2: if (! b.empty()) b[below(b.size())] = (uint8_t) kInteresting[below(std::size(kInteresting))]; break;
            case 3:                                                                                         // 整数の上書き (長さ/オフセット欄を狙う)
            {
                const size_t w = (size_t) 1 << below(4);   // 1/2/4/8 バイト
                if (b.size() < w) break;
                const size_t at = below(b.size() - w + 1);
                const int64_t v = kInteresting[below(std::size(kInteresting))];
                for (size_t i = 0; i < w; ++i)
                    b[at + i] = (uint8_t) (chance(50) ? (v >> (8 * i)) : (v >> (8 * (w - 1 - i))));   // LE / BE
                break;
            }
            case 4:                                                                                         // ランダムバイト挿入
            {
                const size_t len = 1 + below(16), at = below(b.size() + 1);
                Bytes ins(len); for (auto& x : ins) x = (uint8_t) rng();
                b.insert(b.begin() + (long) at, ins.begin(), ins.end());
                break;
            }
            case 5:                                                                                         // 範囲削除
                if (b.size() > 1) { const size_t at = below(b.size()); const size_t len = 1 + below(juce::jmin<size_t>(64, b.size() - at));
                                    b.erase(b.begin() + (long) at, b.begin() + (long) (at + len)); }
                break;
            case 6:                                                                                         // 範囲の複製
                if (! b.empty()) { const size_t at = below(b.size()); const size_t len = 1 + below(juce::jmin<size_t>(256, b.size() - at));
                                   Bytes chunk(b.begin() + (long) at, b.begin() + (long) (at + len));
                                   b.insert(b.begin() + (long) below(b.size() + 1), chunk.begin(), chunk.end()); }
                break;
            case 7:                                                                                         // 辞書トークン挿入/上書き
            {
                if (t.dictionary.empty()) break;
                const auto& tok = t.dictionary[below(t.dictionary.size())];
                const size_t at = below(b.size() + 1);
                if (chance(50) || at + tok.size() > b.size()) b.insert(b.begin() + (long) at, tok.begin(), tok.end());
                else std::copy(tok.begin(), tok.end(), b.begin() + (long) at);
                break;
            }
            case 8:                                                                                         // 別シードとの継ぎはぎ
            {
                const auto& other = pool[below(pool.size())];
                if (other.empty() || b.empty()) break;
                const size_t cut = below(b.size()), from = below(other.size());
                b.resize(cut);
                b.insert(b.end(), other.begin() + (long) from, other.end());
                break;
            }
            case 9: if (! b.empty()) b.resize(below(b.size())); break;                                     // 切り詰め
            case 10:                                                                                        // "..." の中身を辞書トークンへ差し替え (XML/JSON)
            {
                if (b.size() < 2 || t.dictionary.empty()) break;
                size_t start = below(b.size());
                while (start < b.size() && b[start] != '"') ++start;
                if (start >= b.size()) break;
                size_t end = start + 1;
                while (end < b.size() && b[end] != '"') ++end;
                if (end >= b.size()) break;
                const auto& tok = t.dictionary[below(t.dictionary.size())];
                b.erase(b.begin() + (long) start + 1, b.begin() + (long) end);
                b.insert(b.begin() + (long) start + 1, tok.begin(), tok.end());
                break;
            }
            default: break;
        }
    }
};

// ───────────────────────── タイムアウト監視 ─────────────────────────

struct Watchdog
{
    std::atomic<uint64_t> iteration { 0 };
    std::atomic<bool>     stop { false };
    int timeoutSec;
    std::thread th;

    explicit Watchdog(int timeout) : timeoutSec(timeout)
    {
        th = std::thread([this] {
            uint64_t last = iteration.load();
            auto since = std::chrono::steady_clock::now();
            while (! stop.load())
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                const uint64_t now = iteration.load();
                if (now != last) { last = now; since = std::chrono::steady_clock::now(); continue; }
                if (std::chrono::steady_clock::now() - since > std::chrono::seconds(timeoutSec))
                {
                    dumpCurrent(gTimeoutPath, "input took too long (hang?)");
                    std::_Exit(3);
                }
            }
        });
    }
    ~Watchdog() { stop = true; th.join(); }
};

int fuzzTarget(const Target& t, double seconds, uint64_t seed, int timeoutSec)
{
    setPaths(t.name);
    gDumped = false;
    auto seeds = t.seeds();
    if (seeds.empty()) { std::cerr << t.name << ": no seeds could be generated\n"; return 2; }

    // まずシードそのものが通ること (ここで落ちるならファジング以前の問題)
    for (auto& s : seeds) runOne(t, s);

    std::vector<Bytes> pool = seeds;
    Mutator mut(seed ^ std::hash<std::string>{}(t.name));
    Watchdog wd(timeoutSec);
    const auto deadline = std::chrono::steady_clock::now()
                        + std::chrono::milliseconds((int64_t) (seconds * 1000.0));
    uint64_t iters = 0;
    while (std::chrono::steady_clock::now() < deadline)
    {
        Bytes input = pool[mut.below(pool.size())];
        mut.mutate(input, t, pool);
        runOne(t, input);
        wd.iteration.fetch_add(1);
        ++iters;
        // 変異済み入力の一部をプールに残して、より深い (複数回壊れた) 入力も作る
        if (mut.chance(5))
        {
            if (pool.size() < seeds.size() + 64) pool.push_back(std::move(input));
            else pool[seeds.size() + mut.below(64)] = std::move(input);
        }
    }
    std::cout << "  " << t.name << ": " << iters << " inputs in " << seconds << "s, no crash\n" << std::flush;
    return 0;
}

const Target* findTarget(const juce::String& name)
{
    for (auto& t : allTargets())
        if (name == t.name) return &t;
    return nullptr;
}

void prepareDirs(const juce::String& outArg)
{
    auto tmp = juce::File::getSpecialLocation(juce::File::tempDirectory);
    gWorkDir = tmp.getChildFile("UtawaveFuzzWork");
    gWorkDir.deleteRecursively();
    gWorkDir.createDirectory();
    gOutDir = outArg.isNotEmpty() ? juce::File(outArg) : tmp.getChildFile("UtawaveFuzz");
    gOutDir.createDirectory();
}
} // namespace

juce::File workDir() { return gWorkDir; }

void fuzzFail(const char* what)
{
    std::fprintf(stderr, "\n==== UtawaveFuzz: check failed: %s ====\n", what);
    dumpCurrent(gCrashPath, "check failed");
    std::_Exit(4);
}
} // namespace utafuzz

#ifdef UTAWAVE_LIBFUZZER
// ── libFuzzer エンジン (clang -fsanitize=fuzzer でビルドした時) ──
static const utafuzz::Target* gLibFuzzerTarget = nullptr;

extern "C" int LLVMFuzzerInitialize(int*, char***)
{
    static juce::ScopedJuceInitialiser_GUI gui;
    utafuzz::prepareDirs({});
    auto name = juce::SystemStats::getEnvironmentVariable("UTAWAVE_FUZZ_TARGET", "uta");
    gLibFuzzerTarget = utafuzz::findTarget(name);
    if (gLibFuzzerTarget == nullptr) { std::cerr << "unknown UTAWAVE_FUZZ_TARGET: " << name << "\n"; std::exit(2); }
    utafuzz::setPaths(gLibFuzzerTarget->name);
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    if (size > gLibFuzzerTarget->maxLen) return 0;
    gLibFuzzerTarget->run(data, size);
    return 0;
}
#else
int main(int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI gui;
    using namespace utafuzz;

    juce::StringArray args;
    for (int i = 1; i < argc; ++i) args.add(juce::String::fromUTF8(argv[i]));
    auto opt = [&] (const char* key, const juce::String& def) {
        const int i = args.indexOf(key);
        return (i >= 0 && i + 1 < args.size()) ? args[i + 1] : def;
    };

    if (args.isEmpty() || args[0] == "--list" || args[0] == "--help")
    {
        std::cout << "usage: UtawaveFuzz <target|all> [--seconds N] [--seed S] [--timeout T] [--out DIR]\n"
                     "       UtawaveFuzz <target> --repro <file>\n\ntargets:\n";
        for (auto& t : allTargets())
            std::cout << "  " << t.name << " - " << t.description << "\n";
        return args.isEmpty() ? 2 : 0;
    }

    prepareDirs(opt("--out", {}));
    installCrashHandlers();

    const auto targetName = args[0];
    const auto repro = opt("--repro", {});
    if (repro.isNotEmpty())
    {
        auto* t = findTarget(targetName);
        if (t == nullptr) { std::cerr << "unknown target: " << targetName << "\n"; return 2; }
        juce::MemoryBlock mb;
        if (! juce::File(repro).loadFileAsData(mb)) { std::cerr << "cannot read " << repro << "\n"; return 2; }
        setPaths(t->name);
        auto* p = static_cast<const uint8_t*>(mb.getData());
        runOne(*t, Bytes(p, p + mb.getSize()));
        std::cout << "repro: no crash\n";
        return 0;
    }

    const double seconds = opt("--seconds", "30").getDoubleValue();
    const auto seed = (uint64_t) opt("--seed", juce::String((juce::int64) juce::Time::currentTimeMillis())).getLargeIntValue();
    const int timeout = juce::jmax(1, opt("--timeout", "20").getIntValue());
    std::cout << "UtawaveFuzz seed=" << seed << " (reproduce the same sequence with --seed " << seed << ")\n";

    std::vector<const Target*> list;
    if (targetName == "all") for (auto& t : allTargets()) list.push_back(&t);
    else if (auto* t = findTarget(targetName)) list.push_back(t);
    else { std::cerr << "unknown target: " << targetName << " (see --list)\n"; return 2; }

    for (auto* t : list)
        if (int rc = fuzzTarget(*t, seconds, seed, timeout); rc != 0)
            return rc;
    std::cout << "==== UtawaveFuzz: all targets finished without crashes ====\n";
    gWorkDir.deleteRecursively();
    return 0;
}
#endif
