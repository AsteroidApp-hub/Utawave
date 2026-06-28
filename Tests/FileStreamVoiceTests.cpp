// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave
//
// Utawave — ディスクストリーミング先読みボイス (FileStreamVoice) のユニットテスト。
// 核心は「リング経由でもフォールバックでも、直接 reader->read と完全一致する」こと
// (= 先読みの有無/タイミングに依らず出力がビット正確)。順次再生・シーク・モノ/ステレオ・
// EOF 跨ぎを検証し、ウォーム後に実際にリングから供給されている (ringHits>0) ことも確認する。

#include <JuceHeader.h>
#include "../Source/Audio/FileStreamVoice.h"

namespace
{
    constexpr double kSr = 48000.0;

    // L/R が区別できる既知サンプルで 24bit WAV を temp に作る (i 番目: L=fL(i), R=fR(i))。
    juce::File writeKnownWav(int numCh, juce::int64 n)
    {
        auto f = juce::File::createTempFile(".wav");
        juce::AudioBuffer<float> buf(numCh, (int) n);
        for (juce::int64 i = 0; i < n; ++i)
        {
            const float l = std::sin((float) i * 0.01f) * 0.5f;
            buf.setSample(0, (int) i, l);
            if (numCh > 1) buf.setSample(1, (int) i, -std::cos((float) i * 0.013f) * 0.5f);
        }
        juce::WavAudioFormat wav;
        auto os = std::make_unique<juce::FileOutputStream>(f);
        os->setPosition(0); os->truncate();
        std::unique_ptr<juce::AudioFormatWriter> w(
            wav.createWriterFor(os.get(), kSr, (unsigned) numCh, 24, {}, 0));
        if (w != nullptr) { os.release(); w->writeFromAudioSampleBuffer(buf, 0, (int) n); }
        return f;
    }
}

class FileStreamVoiceTests : public juce::UnitTest
{
public:
    FileStreamVoiceTests() : juce::UnitTest("FileStreamVoice") {}

    // voice と参照 reader を同じ params で読み、全チャンネル一致を確認する。
    bool sameAsReference(FileStreamVoice& v, juce::AudioFormatReader& ref,
                         int numCh, int num, juce::int64 startSample)
    {
        juce::AudioBuffer<float> a(numCh, num), b(numCh, num);
        a.clear(); b.clear();
        v.read(a, 0, num, startSample);
        ref.read(&b, 0, num, startSample, true, numCh > 1);
        for (int c = 0; c < numCh; ++c)
            for (int i = 0; i < num; ++i)
                if (std::abs(a.getSample(c, i) - b.getSample(c, i)) > 1.0e-6f)
                    return false;
        return true;
    }

    void runTest() override
    {
        juce::AudioFormatManager fmt;     // ローカル所有 (静的だと終了時 leak assert)
        fmt.registerBasicFormats();

        beginTest("Sequential read matches direct reader (stereo) + warms ring");
        {
            const juce::int64 n = 120000;
            auto file = writeKnownWav(2, n);
            juce::TimeSliceThread th("uta-stream-test"); th.startThread();
            {
                std::unique_ptr<juce::AudioFormatReader> bgR(fmt.createReaderFor(file));
                std::unique_ptr<juce::AudioFormatReader> fbR(fmt.createReaderFor(file));
                std::unique_ptr<juce::AudioFormatReader> refR(fmt.createReaderFor(file));
                expect(bgR && fbR && refR, "readers open");
                FileStreamVoice v(std::move(bgR), std::move(fbR), th);

                const int block = 512;
                bool ok = true;
                // 先頭ブロックでウォーム開始 → 少し待ってから順次読み (ヒットを発生させる)
                ok &= sameAsReference(v, *refR, 2, block, 0);
                juce::Thread::sleep(200);
                for (juce::int64 s = block; s + block <= n; s += block)
                    ok &= sameAsReference(v, *refR, 2, block, s);
                expect(ok, "every sequential block matches the direct reader");
                expect(v.getRingHits() > 0, "ring should serve some reads after warming");
            }
            th.stopThread(2000);
            file.deleteFile();
        }

        beginTest("Random seeks match direct reader");
        {
            const juce::int64 n = 120000;
            auto file = writeKnownWav(2, n);
            juce::TimeSliceThread th("uta-stream-test2"); th.startThread();
            {
                std::unique_ptr<juce::AudioFormatReader> bgR(fmt.createReaderFor(file));
                std::unique_ptr<juce::AudioFormatReader> fbR(fmt.createReaderFor(file));
                std::unique_ptr<juce::AudioFormatReader> refR(fmt.createReaderFor(file));
                FileStreamVoice v(std::move(bgR), std::move(fbR), th);

                juce::Random rng(1234);
                bool ok = true;
                for (int k = 0; k < 200; ++k)
                {
                    const int num = 64 + rng.nextInt(900);
                    const juce::int64 start = rng.nextInt((int) (n - 1000));
                    ok &= sameAsReference(v, *refR, 2, num, start);
                    if ((k % 40) == 0) juce::Thread::sleep(15);  // 時々ウォームさせる
                }
                expect(ok, "all random-seek reads match the direct reader");
            }
            th.stopThread(2000);
            file.deleteFile();
        }

        beginTest("Reads spanning EOF match direct reader (zero-filled tail)");
        {
            const juce::int64 n = 5000;
            auto file = writeKnownWav(2, n);
            juce::TimeSliceThread th("uta-stream-test3"); th.startThread();
            {
                std::unique_ptr<juce::AudioFormatReader> bgR(fmt.createReaderFor(file));
                std::unique_ptr<juce::AudioFormatReader> fbR(fmt.createReaderFor(file));
                std::unique_ptr<juce::AudioFormatReader> refR(fmt.createReaderFor(file));
                FileStreamVoice v(std::move(bgR), std::move(fbR), th);
                juce::Thread::sleep(80);
                // 末尾跨ぎ (n-200 から 512) と完全に EOF 後 (n+100 から 256)
                bool ok = sameAsReference(v, *refR, 2, 512, n - 200)
                        & sameAsReference(v, *refR, 2, 256, n + 100);
                expect(ok, "EOF-spanning and past-EOF reads match (both zero-fill tail)");
            }
            th.stopThread(2000);
            file.deleteFile();
        }

        beginTest("Mono file matches direct reader");
        {
            const juce::int64 n = 40000;
            auto file = writeKnownWav(1, n);
            juce::TimeSliceThread th("uta-stream-test4"); th.startThread();
            {
                std::unique_ptr<juce::AudioFormatReader> bgR(fmt.createReaderFor(file));
                std::unique_ptr<juce::AudioFormatReader> fbR(fmt.createReaderFor(file));
                std::unique_ptr<juce::AudioFormatReader> refR(fmt.createReaderFor(file));
                FileStreamVoice v(std::move(bgR), std::move(fbR), th);
                expect(v.getNumChannels() == 1, "mono voice reports 1 channel");
                juce::Thread::sleep(120);
                bool ok = true;
                for (juce::int64 s = 0; s + 512 <= n; s += 512)
                    ok &= sameAsReference(v, *refR, 1, 512, s);
                expect(ok, "mono sequential reads match the direct reader");
            }
            th.stopThread(2000);
            file.deleteFile();
        }
    }
};

static FileStreamVoiceTests fileStreamVoiceTests;
