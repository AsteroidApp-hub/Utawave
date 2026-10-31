// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

// Utawave — PitchEngine (自社製オフラインピッチシフト) のユニットテスト
//
// shiftBuffer (フェーズボコーダ + r8brain リサンプル) をデバイス無し・決定論的に検証する。
// 周波数は正方向ゼロ交差の「最初と最後の交差間の周期数」から推定する (窓端に依存しない)。
// 純音はフェーズボコーダで正確に追従するため許容は ±3 Hz。振幅は OLA 正規化と
// リサンプラのユニティゲインが効いていれば ±0.08 に収まる。
// expect メッセージは ASCII。
//
// processFile は実 WAV の往復 (書き出し→変換→読み戻し) で尺と周波数を確認する。

#include <JuceHeader.h>
#include "../Source/Audio/PitchEngine.h"
#include "../Source/Audio/pitchcore/RealFFT.h"
#include <cmath>
#include <cstring>
#include <vector>

namespace
{
class PitchEngineTests : public juce::UnitTest
{
public:
    PitchEngineTests() : juce::UnitTest("PitchEngine") {}

    static juce::AudioBuffer<float> makeSine(int numCh, double sr, double secs,
                                             const std::vector<double>& freqPerCh,
                                             float amp = 0.5f)
    {
        const int len = (int) std::llround(sr * secs);
        juce::AudioBuffer<float> buf(numCh, len);
        for (int ch = 0; ch < numCh; ++ch)
        {
            const double f = freqPerCh[(size_t) juce::jmin(ch, (int) freqPerCh.size() - 1)];
            float* d = buf.getWritePointer(ch);
            for (int i = 0; i < len; ++i)
                d[i] = amp * (float) std::sin(2.0 * juce::MathConstants<double>::pi
                                              * f * (double) i / sr);
        }
        return buf;
    }

    // 正方向ゼロ交差から周波数推定 (最初と最後の交差間に crossings-1 周期)
    static double measureFreq(const juce::AudioBuffer<float>& buf, int ch,
                              double sr, int start, int end)
    {
        const float* d = buf.getReadPointer(ch);
        int crossings = 0, first = -1, last = -1;
        for (int i = start + 1; i < end; ++i)
        {
            if (d[i - 1] <= 0.0f && d[i] > 0.0f)
            {
                ++crossings;
                if (first < 0) first = i;
                last = i;
            }
        }
        if (crossings < 2 || last <= first) return 0.0;
        return (double) (crossings - 1) * sr / (double) (last - first);
    }

    static bool allFinite(const juce::AudioBuffer<float>& buf)
    {
        for (int ch = 0; ch < buf.getNumChannels(); ++ch)
        {
            const float* d = buf.getReadPointer(ch);
            for (int i = 0; i < buf.getNumSamples(); ++i)
                if (! std::isfinite(d[i])) return false;
        }
        return true;
    }

    void runTest() override
    {
        testRealFFT();
        testSineUp();
        testSineDown();
        testStereoChannelsKeepApart();
        testZeroSemitonesIsCopy();
        testSilenceStaysSilent();
        testDurationExact();
        testCancelAndProgress();
        testShortBuffer();
        testGuards();
        testProcessFileRoundTrip();
        testTimeStretch();
        testRenderRegion();
    }

    // 無音の後の最初のオンセット (|x| > thr) の位置
    static int firstOnset(const juce::AudioBuffer<float>& b, float thr)
    {
        const float* d = b.getReadPointer(0);
        for (int i = 0; i < b.getNumSamples(); ++i)
            if (std::abs(d[i]) > thr) return i;
        return -1;
    }

    // ── タイムストレッチ (shiftPitchAndTime): 尺 / ピッチ維持 / オンセット位置 / キー合成 ──
    void testTimeStretch()
    {
        const double sr = 48000.0;
        beginTest("shiftBufferAndTime: x1.5 keeps pitch, length = round(n*1.5)");
        {
            auto in = makeSine(1, sr, 2.0, { 440.0 });
            juce::AudioBuffer<float> out;
            expect(PitchEngine::shiftBufferAndTime(in, sr, 0.0, 1.5, out), "returns true");
            expectEquals(out.getNumSamples(), (int) std::llround(in.getNumSamples() * 1.5), "length x1.5");
            const double f = measureFreq(out, 0, sr, (int) (sr * 0.5), (int) (sr * 2.5));
            expect(std::abs(f - 440.0) < 3.0, "pitch kept ~440 Hz, got " + juce::String(f, 2));
            const float mag = out.getMagnitude(0, (int) (sr * 0.5), (int) (sr * 2.0));
            expect(mag > 0.4f && mag < 0.6f, "amplitude kept ~0.5");
            expect(allFinite(out), "finite");
        }
        beginTest("shiftBufferAndTime: x0.75 shortens, onset maps to t*0.75");
        {
            // 0.8s 無音 → 440Hz 0.5 振幅。オンセットは 0.8 * 0.75 = 0.6s に来るはず
            const int n = (int) (sr * 2.0), on = (int) (sr * 0.8);
            juce::AudioBuffer<float> in(1, n);
            in.clear();
            for (int i = on; i < n; ++i)
                in.setSample(0, i, 0.5f * (float) std::sin(2.0 * juce::MathConstants<double>::pi * 440.0 * (i - on) / sr));
            juce::AudioBuffer<float> out;
            expect(PitchEngine::shiftBufferAndTime(in, sr, 0.0, 0.75, out), "returns true");
            expectEquals(out.getNumSamples(), (int) std::llround(n * 0.75), "length x0.75");
            const int o = firstOnset(out, 0.2f);
            const double err = std::abs((double) o / sr - 0.6);
            expect(err < 0.015, "onset at ~0.60 s (error " + juce::String(err * 1000.0, 1) + " ms)");
        }
        beginTest("shiftBufferAndTime: key + time in one pass (+3 st, x1.25)");
        {
            auto in = makeSine(1, sr, 2.0, { 440.0 });
            juce::AudioBuffer<float> out;
            expect(PitchEngine::shiftBufferAndTime(in, sr, 3.0, 1.25, out), "returns true");
            expectEquals(out.getNumSamples(), (int) std::llround(in.getNumSamples() * 1.25), "length x1.25");
            const double f = measureFreq(out, 0, sr, (int) (sr * 0.4), (int) (sr * 2.1));
            expect(std::abs(f - 523.25) < 3.0, "pitch ~523.25 Hz, got " + juce::String(f, 2));
        }
        beginTest("shiftBufferAndTime: timeRatio 1 is identical to shiftBuffer; guards");
        {
            auto in = makeSine(2, sr, 1.0, { 440.0, 660.0 });
            juce::AudioBuffer<float> a, b;
            expect(PitchEngine::shiftBuffer(in, sr, 2.0, a), "shiftBuffer ok");
            expect(PitchEngine::shiftBufferAndTime(in, sr, 2.0, 1.0, b), "shiftBufferAndTime ok");
            bool same = a.getNumSamples() == b.getNumSamples();
            for (int ch = 0; same && ch < 2; ++ch)
                same = std::memcmp(a.getReadPointer(ch), b.getReadPointer(ch),
                                   sizeof(float) * (size_t) a.getNumSamples()) == 0;
            expect(same, "bit-identical to shiftBuffer at timeRatio 1");

            juce::AudioBuffer<float> c;
            expect(PitchEngine::shiftBufferAndTime(in, sr, 0.0, 1.0, c), "0 st / x1 ok");
            expect(c.getNumSamples() == in.getNumSamples()
                   && std::memcmp(c.getReadPointer(0), in.getReadPointer(0),
                                  sizeof(float) * (size_t) in.getNumSamples()) == 0,
                   "0 st / x1 is an exact copy");
            expect(! PitchEngine::shiftBufferAndTime(in, sr, 0.0, 0.1, c), "ratio 0.1 rejected");
            expect(! PitchEngine::shiftBufferAndTime(in, sr, 0.0, 5.0, c), "ratio 5 rejected");
        }
    }

    // ── renderRegion: 区間だけを読み、x 倍の尺で 32f WAV を書く ──
    void testRenderRegion()
    {
        beginTest("renderRegion: renders only the requested region at timeRatio");
        juce::AudioFormatManager fmt;
        fmt.registerBasicFormats();
        auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                       .getChildFile("UtawavePitchRegionTest");
        dir.deleteRecursively();
        dir.createDirectory();
        auto inFile  = dir.getChildFile("in.wav");
        auto outFile = dir.getChildFile("out.wav");

        const double sr = 48000.0;
        auto buf = makeSine(1, sr, 3.0, { 440.0 });
        {
            juce::WavAudioFormat wav;
            std::unique_ptr<juce::OutputStream> os = inFile.createOutputStream();
            auto w = wav.createWriterFor(os, juce::AudioFormatWriterOptions{}
                         .withSampleRate(sr).withNumChannels(1).withBitsPerSample(32)
                         .withSampleFormat(juce::AudioFormatWriterOptions::SampleFormat::floatingPoint));
            expect(w != nullptr, "source writer");
            if (w) w->writeFromAudioSampleBuffer(buf, 0, buf.getNumSamples());
        }

        const juce::int64 start = (juce::int64) (sr * 1.0), len = (juce::int64) (sr * 1.0);
        expect(PitchEngine::renderRegion(inFile, outFile, fmt, start, len, 0.0, 1.5), "render ok");
        std::unique_ptr<juce::AudioFormatReader> r(fmt.createReaderFor(outFile));
        expect(r != nullptr, "output readable");
        if (r)
        {
            expectEquals((int) r->lengthInSamples, (int) std::llround((double) len * 1.5), "region x1.5 length");
            expect(r->bitsPerSample == 32 && r->usesFloatingPointData, "32-bit float output");
            juce::AudioBuffer<float> rd(1, (int) r->lengthInSamples);
            r->read(&rd, 0, rd.getNumSamples(), 0, true, true);
            const double f = measureFreq(rd, 0, sr, (int) (sr * 0.3), (int) (sr * 1.2));
            expect(std::abs(f - 440.0) < 3.0, "pitch kept, got " + juce::String(f, 2));
        }

        auto cancelled = dir.getChildFile("cancel.wav");
        expect(! PitchEngine::renderRegion(inFile, cancelled, fmt, start, len, 0.0, 1.5,
                                           [](double) { return false; }),
               "cancel returns false");
        expect(! cancelled.existsAsFile(), "cancel leaves no output");
        expect(! PitchEngine::renderRegion(dir.getChildFile("missing.wav"), outFile, fmt, 0, 100, 0.0, 1.5),
               "missing input -> false");
        r.reset();
        dir.deleteRecursively();
    }

    // ── 自前 FFT: ラウンドトリップ恒等 + 正弦波の bin 位置/振幅/実信号対称性 ──
    void testRealFFT()
    {
        beginTest("RealFFT: round-trip identity, sine bin magnitude A*N/2");
        const int n = 1024;
        pitchcore::RealFFT fft(n);
        std::vector<float> x((size_t) n), spec((size_t)(n + 2)), y((size_t) n);

        // 決定論的擬似ランダム信号 (LCG) で往復誤差を検証
        juce::uint32 seed = 12345;
        for (int i = 0; i < n; ++i)
        {
            seed = seed * 1664525u + 1013904223u;
            x[(size_t) i] = (float)((seed >> 8) & 0xFFFF) / 32768.0f - 1.0f;
        }
        fft.forward(x.data(), spec.data());
        fft.inverse(spec.data(), y.data());
        float maxErr = 0.0f;
        for (int i = 0; i < n; ++i)
            maxErr = juce::jmax(maxErr, std::abs(x[(size_t) i] - y[(size_t) i]));
        expect(maxErr < 1.0e-4f,
               "round-trip max error < 1e-4, got " + juce::String(maxErr, 8));

        // 正弦波 (整数 bin): ピークが bin k0、振幅 A*N/2
        const int k0 = 37;
        const float amp = 0.6f;
        for (int i = 0; i < n; ++i)
            x[(size_t) i] = amp * (float) std::sin(
                2.0 * juce::MathConstants<double>::pi * k0 * i / n);
        fft.forward(x.data(), spec.data());
        int bestBin = 0;
        float bestMag = 0.0f;
        for (int k = 0; k <= n / 2; ++k)
        {
            const float mg = std::hypot(spec[(size_t)(2 * k)], spec[(size_t)(2 * k + 1)]);
            if (mg > bestMag) { bestMag = mg; bestBin = k; }
        }
        expectEquals(bestBin, k0, "sine lands on its bin");
        const float expected = amp * (float) n / 2.0f;
        expect(std::abs(bestMag - expected) < expected * 0.01f,
               "bin magnitude ~A*N/2, got " + juce::String(bestMag, 2));
        // 実信号の対称性: DC / ナイキストの虚部は 0
        expect(std::abs(spec[1]) < 1.0e-2f && std::abs(spec[(size_t)(n + 1)]) < 1.0e-2f,
               "DC and Nyquist bins are real");
    }

    // ── +6 半音: 440 Hz → 622.25 Hz、尺・振幅維持 ──
    void testSineUp()
    {
        beginTest("shiftBuffer: 440 Hz +6 semitones -> ~622.25 Hz, length kept");
        const double sr = 48000.0;
        auto in = makeSine(1, sr, 2.0, { 440.0 });
        juce::AudioBuffer<float> out;
        expect(PitchEngine::shiftBuffer(in, sr, 6.0, out), "shiftBuffer returns true");
        expectEquals(out.getNumSamples(), in.getNumSamples(), "output length == input length");
        expect(allFinite(out), "no NaN/Inf in output");

        const int a = (int) (sr * 0.25), b = (int) (sr * 1.75);
        const double f = measureFreq(out, 0, sr, a, b);
        expect(std::abs(f - 622.25) < 3.0,
               "frequency ~622.25 Hz, got " + juce::String(f, 2));
        const float m = out.getMagnitude(0, a, b - a);
        expect(std::abs(m - 0.5f) < 0.08f,
               "amplitude ~0.5 preserved, got " + juce::String(m, 3));
    }

    // ── -6 半音: 440 Hz → 311.13 Hz ──
    void testSineDown()
    {
        beginTest("shiftBuffer: 440 Hz -6 semitones -> ~311.13 Hz");
        const double sr = 48000.0;
        auto in = makeSine(1, sr, 2.0, { 440.0 });
        juce::AudioBuffer<float> out;
        expect(PitchEngine::shiftBuffer(in, sr, -6.0, out), "shiftBuffer returns true");
        expectEquals(out.getNumSamples(), in.getNumSamples(), "output length == input length");

        const int a = (int) (sr * 0.25), b = (int) (sr * 1.75);
        const double f = measureFreq(out, 0, sr, a, b);
        expect(std::abs(f - 311.13) < 3.0,
               "frequency ~311.13 Hz, got " + juce::String(f, 2));
    }

    // ── ステレオ: L/R が混ざらず各チャンネル独立にシフトされる ──
    void testStereoChannelsKeepApart()
    {
        beginTest("shiftBuffer: stereo L=440/R=880 +3 semitones stay separated");
        const double sr = 48000.0;
        auto in = makeSine(2, sr, 2.0, { 440.0, 880.0 });
        juce::AudioBuffer<float> out;
        expect(PitchEngine::shiftBuffer(in, sr, 3.0, out), "shiftBuffer returns true");
        expectEquals(out.getNumChannels(), 2, "stereo preserved");

        const int a = (int) (sr * 0.25), b = (int) (sr * 1.75);
        const double fL = measureFreq(out, 0, sr, a, b);
        const double fR = measureFreq(out, 1, sr, a, b);
        expect(std::abs(fL - 523.25) < 3.0, "L ~523.25 Hz, got " + juce::String(fL, 2));
        expect(std::abs(fR - 1046.50) < 5.0, "R ~1046.50 Hz, got " + juce::String(fR, 2));
    }

    // ── 0 半音は完全コピー ──
    void testZeroSemitonesIsCopy()
    {
        beginTest("shiftBuffer: 0 semitones is an exact copy");
        const double sr = 44100.0;
        auto in = makeSine(1, sr, 0.5, { 440.0 });
        juce::AudioBuffer<float> out;
        expect(PitchEngine::shiftBuffer(in, sr, 0.0, out), "shiftBuffer returns true");
        expectEquals(out.getNumSamples(), in.getNumSamples(), "length identical");
        bool same = true;
        for (int i = 0; i < in.getNumSamples(); ++i)
            if (in.getSample(0, i) != out.getSample(0, i)) { same = false; break; }
        expect(same, "samples bit-identical for 0 semitones");
    }

    // ── 無音入力は無音出力 ──
    void testSilenceStaysSilent()
    {
        beginTest("shiftBuffer: silence in -> silence out");
        const double sr = 48000.0;
        juce::AudioBuffer<float> in(1, (int) sr);
        in.clear();
        juce::AudioBuffer<float> out;
        expect(PitchEngine::shiftBuffer(in, sr, 4.0, out), "shiftBuffer returns true");
        expect(out.getMagnitude(0, 0, out.getNumSamples()) < 1.0e-4f,
               "output magnitude stays ~0");
    }

    // ── 端数の長さでも尺が厳密に一致 ──
    void testDurationExact()
    {
        beginTest("shiftBuffer: odd input length kept exactly");
        const double sr = 44100.0;
        const int len = 44100 + 1234;
        juce::AudioBuffer<float> in(2, len);
        for (int ch = 0; ch < 2; ++ch)
        {
            float* d = in.getWritePointer(ch);
            for (int i = 0; i < len; ++i)
                d[i] = 0.4f * (float) std::sin(2.0 * juce::MathConstants<double>::pi
                                               * 330.0 * i / sr);
        }
        juce::AudioBuffer<float> out;
        expect(PitchEngine::shiftBuffer(in, sr, 4.0, out), "shiftBuffer returns true");
        expectEquals(out.getNumSamples(), len, "output length == odd input length");
        expect(allFinite(out), "no NaN/Inf in output");
    }

    // ── キャンセルで false / 進捗は [0,1] で単調非減少 ──
    void testCancelAndProgress()
    {
        beginTest("shiftBuffer: cancel aborts, progress is monotonic in [0,1]");
        const double sr = 48000.0;
        auto in = makeSine(1, sr, 1.0, { 440.0 });
        juce::AudioBuffer<float> out;

        int calls = 0;
        const bool cancelled = PitchEngine::shiftBuffer(in, sr, 3.0, out,
            [&calls](double) { return ++calls < 3; });
        expect(! cancelled, "returns false when onProgress requests cancel");

        std::vector<double> progress;
        expect(PitchEngine::shiftBuffer(in, sr, 3.0, out,
            [&progress](double p) { progress.push_back(p); return true; }),
            "completes with progress callback");
        bool ok = ! progress.empty();
        for (size_t i = 0; i < progress.size() && ok; ++i)
        {
            if (progress[i] < 0.0 || progress[i] > 1.0) ok = false;
            if (i > 0 && progress[i] < progress[i - 1]) ok = false;
        }
        expect(ok, "progress values stay in [0,1] and never decrease");
        expect(! progress.empty() && std::abs(progress.back() - 1.0) < 1.0e-9,
               "final progress is 1.0");
    }

    // ── FFT 1 枚より短い入力でも落ちず尺維持 ──
    void testShortBuffer()
    {
        beginTest("shiftBuffer: very short input (10 ms) survives");
        const double sr = 48000.0;
        auto in = makeSine(1, sr, 0.01, { 1000.0 });
        juce::AudioBuffer<float> out;
        expect(PitchEngine::shiftBuffer(in, sr, 6.0, out), "shiftBuffer returns true");
        expectEquals(out.getNumSamples(), in.getNumSamples(), "length kept");
        expect(allFinite(out), "no NaN/Inf in output");
    }

    // ── 無効入力ガード ──
    void testGuards()
    {
        beginTest("shiftBuffer: invalid inputs return false");
        juce::AudioBuffer<float> empty;
        juce::AudioBuffer<float> out;
        expect(! PitchEngine::shiftBuffer(empty, 48000.0, 3.0, out),
               "empty buffer -> false");
        auto in = makeSine(1, 48000.0, 0.1, { 440.0 });
        expect(! PitchEngine::shiftBuffer(in, 0.0, 3.0, out),
               "sampleRate 0 -> false");
    }

    // ── processFile: 実 WAV の往復 ──
    void testProcessFileRoundTrip()
    {
        beginTest("processFile: WAV round-trip keeps length, shifts pitch");
        juce::AudioFormatManager fmt;
        fmt.registerBasicFormats();

        auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                       .getChildFile("UtawavePitchEngineTest");
        dir.deleteRecursively();
        dir.createDirectory();
        auto inFile  = dir.getChildFile("in.wav");
        auto outFile = dir.getChildFile("out.wav");

        const double sr = 48000.0;
        auto buf = makeSine(1, sr, 1.5, { 440.0 });
        {
            juce::WavAudioFormat wav;
            auto os = std::make_unique<juce::FileOutputStream>(inFile);
            auto opts = juce::AudioFormatWriterOptions{}
                            .withSampleRate(sr)
                            .withNumChannels(1)
                            .withBitsPerSample(32)
                            .withSampleFormat(
                                juce::AudioFormatWriterOptions::SampleFormat::floatingPoint);
            std::unique_ptr<juce::OutputStream> osBase = std::move(os);
            auto writer = wav.createWriterFor(osBase, opts);
            expect(writer != nullptr, "test WAV writer created");
            if (writer != nullptr)
            {
                writer->writeFromAudioSampleBuffer(buf, 0, buf.getNumSamples());
                writer->flush();
            }
        }

        expect(PitchEngine::processFile(inFile, outFile, fmt, 6.0, 32),
               "processFile returns true");
        std::unique_ptr<juce::AudioFormatReader> reader(fmt.createReaderFor(outFile));
        expect(reader != nullptr, "output readable");
        if (reader != nullptr)
        {
            expectEquals((int) reader->lengthInSamples, buf.getNumSamples(),
                         "output length == input length");
            expectEquals((int) reader->sampleRate, (int) sr, "sample rate kept");
            juce::AudioBuffer<float> rd(1, (int) reader->lengthInSamples);
            reader->read(&rd, 0, rd.getNumSamples(), 0, true, true);
            const int a = (int) (sr * 0.25), b = (int) (sr * 1.25);
            const double f = measureFreq(rd, 0, sr, a, b);
            expect(std::abs(f - 622.25) < 3.0,
                   "file output ~622.25 Hz, got " + juce::String(f, 2));
        }

        expect(! PitchEngine::processFile(dir.getChildFile("missing.wav"),
                                          outFile, fmt, 6.0, 32),
               "missing input -> false");
        dir.deleteRecursively();
    }
};

static PitchEngineTests pitchEngineTests;
}
