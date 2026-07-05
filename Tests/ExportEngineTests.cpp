// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

// Utawave — オーディオ書き出し (ExportEngine) の WAV ユニットテスト
//
// オフライン書き出し経路 (ExportEngine::render → AudioEngine::renderOfflineRange) を
// オーディオデバイス無しで検証する。各テストは:
//   1. 既知内容のソース WAV を一時フォルダに生成
//   2. TrackManager / Track / AudioClip でシーンを構築し AudioEngine::preparePlayback
//   3. ExportEngine::render で WAV (または AIFF) に書き出し
//   4. 書き出したファイルを読み直してヘッダ / 尺 / サンプル内容を検証
//
// 内容一致系は量子化・ディザを避けるため 32bit float で書き出す。整数フォーマット /
// ディザ / ピーク超過保護は専用テストで構造的に検証する。
//
// 実行: juce::UnitTestRunner で全テストを走らせ、失敗があれば exit code 1 を返す。

#include <JuceHeader.h>
#include <iostream>
#include <cmath>
#include <set>

#include "../Source/Audio/AudioEngine.h"
#include "../Source/Tracks/TrackManager.h"
#include "../Source/Tracks/Track.h"
#include "../Source/Tracks/AudioClip.h"
#include "../Source/Tracks/MidiClip.h"
#include "../Source/Export/ExportEngine.h"

namespace
{
constexpr double kSR        = 48000.0;     // AudioEngine の既定サンプルレート (デバイス無しでも有効)
constexpr float  kLsb16     = 1.0f / 32768.0f;   // 16bit の 1 LSB
constexpr float  kCeiling   = 0.989f;            // ExportEngine のピーク超過保護の上限

// ───────────────────────── WAV 書き込み / 読み出しヘルパ ─────────────────────────

// 32bit float の WAV をディスクへ書き出す (ソース生成用)。ソースを float にすることで
// 0.4 / 0.30001 等の値が量子化されずに書き出し経路へ届く。
bool writeFloatWav(const juce::File& f, double sr, const juce::AudioBuffer<float>& buf)
{
    juce::WavAudioFormat waf;
    using SF = juce::AudioFormatWriterOptions::SampleFormat;
    auto wopts = juce::AudioFormatWriterOptions{}
                     .withSampleRate(sr)
                     .withNumChannels(buf.getNumChannels())
                     .withBitsPerSample(32)
                     .withSampleFormat(SF::floatingPoint);
    f.getParentDirectory().createDirectory();
    f.deleteFile();
    auto fos = std::make_unique<juce::FileOutputStream>(f);
    if (!fos->openedOk()) return false;
    std::unique_ptr<juce::OutputStream> os = std::move(fos);
    std::unique_ptr<juce::AudioFormatWriter> w(waf.createWriterFor(os, wopts));
    if (!w) return false;
    return w->writeFromAudioSampleBuffer(buf, 0, buf.getNumSamples());
}

// 定数値のモノラル float WAV を生成
bool writeMonoConst(const juce::File& f, int numSamples, float value)
{
    juce::AudioBuffer<float> b(1, numSamples);
    juce::FloatVectorOperations::fill(b.getWritePointer(0), value, numSamples);
    return writeFloatWav(f, kSR, b);
}

// 定数値のステレオ float WAV を生成 (L/R に別の値)
bool writeStereoConst(const juce::File& f, int numSamples, float l, float r)
{
    juce::AudioBuffer<float> b(2, numSamples);
    juce::FloatVectorOperations::fill(b.getWritePointer(0), l, numSamples);
    juce::FloatVectorOperations::fill(b.getWritePointer(1), r, numSamples);
    return writeFloatWav(f, kSR, b);
}

// 読み出した WAV/AIFF の情報
struct AudioFileData
{
    bool        ok        { false };
    double      sampleRate{ 0.0 };
    int         bits      { 0 };
    int         numCh     { 0 };
    bool        isFloat   { false };
    juce::int64 len       { 0 };
    juce::AudioBuffer<float> samples;

    float maxAbs() const
    {
        float m = 0.0f;
        for (int ch = 0; ch < samples.getNumChannels(); ++ch)
            m = juce::jmax(m, samples.getMagnitude(ch, 0, samples.getNumSamples()));
        return m;
    }
};

AudioFileData readAudio(const juce::File& f, bool aiff = false)
{
    AudioFileData d;
    std::unique_ptr<juce::AudioFormat> fmt;
    if (aiff) fmt = std::make_unique<juce::AiffAudioFormat>();
    else      fmt = std::make_unique<juce::WavAudioFormat>();

    std::unique_ptr<juce::AudioFormatReader> r(
        fmt->createReaderFor(new juce::FileInputStream(f), true));
    if (!r) return d;

    d.ok         = true;
    d.sampleRate = r->sampleRate;
    d.bits       = (int) r->bitsPerSample;
    d.numCh      = (int) r->numChannels;
    d.isFloat    = r->usesFloatingPointData;
    d.len        = r->lengthInSamples;
    d.samples.setSize((int) r->numChannels, (int) r->lengthInSamples);
    d.samples.clear();
    r->read(&d.samples, 0, (int) r->lengthInSamples, 0, true, true);
    return d;
}

// 既定の Options (WAV / 全体 / ステレオ)。テスト毎に書き換える。
ExportEngine::Options baseOpts(const juce::File& out, int bitDepth, double endSec)
{
    ExportEngine::Options o;
    o.file        = out;
    o.format      = ExportEngine::Format::WAV;
    o.bitDepth    = bitDepth;
    o.sampleRate  = 0.0;       // エンジン SR をそのまま
    o.startSec    = 0.0;
    o.endSec      = endSec;
    o.numChannels = 2;
    o.dither      = true;
    o.stems       = false;
    o.preFader    = false;
    o.peakGuard   = true;
    return o;
}
} // namespace

//==============================================================================
class ExportEngineTests : public juce::UnitTest
{
public:
    ExportEngineTests() : juce::UnitTest("ExportEngine WAV") {}

    juce::File outDir;

    // [from,to) の全サンプルが value に tol 以内で一致するか
    void expectAllClose(const juce::AudioBuffer<float>& b, int ch, float value, float tol,
                        const juce::String& msg, int from = 0, int to = -1)
    {
        if (to < 0) to = b.getNumSamples();
        float maxErr = 0.0f; int badIdx = -1;
        const float* p = b.getReadPointer(ch);
        for (int i = from; i < to; ++i)
        {
            float e = std::abs(p[i] - value);
            if (e > maxErr) { maxErr = e; badIdx = i; }
        }
        expect(maxErr <= tol,
               msg + " : maxErr=" + juce::String(maxErr, 8)
               + " at idx=" + juce::String(badIdx)
               + " (expected " + juce::String(value, 6) + " +/- " + juce::String(tol, 8) + ")");
    }

    void runTest() override
    {
        outDir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                     .getChildFile("UtawaveExportTests");
        outDir.deleteRecursively();
        outDir.createDirectory();

        testInvalidRange();
        testHeader16and24();
        testFloat32HeaderAndExactContent();
        testAiffContrast();
        testSampleRateConversion();
        testMonoDownmix();
        testStereoChannelMapping();
        testPostFaderVolume();
        testPostFaderPanLinearLaw();
        testClipGainBothFaderModes();
        testPreVsPostFaderDiffer();
        testMultiTrackSummation();
        testPeakGuardOnMultiTrackSum();
        testPeakGuardAttenuatesOverCeiling();
        testPeakGuardLeavesUnderCeilingUntouched();
        testPeakGuardOffPreservesOverUnity();
        testRangeOffsetMarkerPlacement();
        testSelectedTrackIndicesIgnoreMuteSolo();
        testEmptySelectionHonorsSoloMute();
        testClickTrackExcluded();
        testMidiTrackRendersInExport();
        testReverbSendInExport();
        testDither16PerturbsSteadyLevel();
        testDitherSilenceBounded();
        testOverwritesExistingFile();

        outDir.deleteRecursively();
    }

    //==========================================================================
    // 1. 範囲が無効 (endSec <= startSec) なら false を返しファイルを書かない
    void testInvalidRange()
    {
        beginTest("invalid/inverted range returns false, no file written");

        juce::AudioFormatManager fmt; fmt.registerBasicFormats();
        TrackManager tm(fmt);
        auto src = outDir.getChildFile("range_src.wav");
        expect(writeMonoConst(src, (int)(0.5 * kSR), 0.5f), "source write");
        auto* t = tm.addTrack("a", false);
        auto* c = t->addClip(src, 0.0, 0.5);
        c->setFadeInSecs(0.0); c->setFadeOutSecs(0.0);

        AudioEngine engine;
        engine.preparePlayback(tm);

        auto out = outDir.getChildFile("range_out.wav");

        // (a) endSec == startSec
        {
            auto o = baseOpts(out, 24, 0.0); o.startSec = 1.0; o.endSec = 1.0;
            juce::String err;
            bool ok = ExportEngine::render(engine, o, {}, {}, &err);
            expect(!ok, "equal range should fail");
            expect(err.isNotEmpty(), "error string set on equal range");
            expect(!out.existsAsFile(), "no file written on equal range");
        }
        // (b) endSec < startSec
        {
            auto o = baseOpts(out, 24, 0.0); o.startSec = 2.0; o.endSec = 1.0;
            juce::String err;
            bool ok = ExportEngine::render(engine, o, {}, {}, &err);
            expect(!ok, "inverted range should fail");
            expect(err.isNotEmpty(), "error string set on inverted range");
            expect(!out.existsAsFile(), "no file written on inverted range");
        }
    }

    //==========================================================================
    // 2. 16bit / 24bit ヘッダ (整数フォーマット) と尺
    void testHeader16and24()
    {
        for (int bits : { 16, 24 })
        {
            beginTest("WAV " + juce::String(bits) + "bit integral header + length");

            juce::AudioFormatManager fmt; fmt.registerBasicFormats();
            TrackManager tm(fmt);
            auto src = outDir.getChildFile("hdr_src.wav");
            expect(writeMonoConst(src, (int)(2.0 * kSR), 0.5f), "source write");
            auto* t = tm.addTrack("a", false);
            auto* c = t->addClip(src, 0.0, 2.0);
            c->setFadeInSecs(0.0); c->setFadeOutSecs(0.0);
            t->setVolume(0.0f); t->setPan(0.0f);

            AudioEngine engine; engine.preparePlayback(tm);

            auto out = outDir.getChildFile("hdr_" + juce::String(bits) + ".wav");
            auto o = baseOpts(out, bits, 2.0);
            juce::String err;
            expect(ExportEngine::render(engine, o, {}, {}, &err), "render: " + err);

            auto d = readAudio(out);
            expect(d.ok, "readable WAV");
            expectEquals(d.sampleRate, 48000.0);
            expectEquals(d.bits, bits);
            expect(!d.isFloat, "integral sample format");
            expectEquals(d.numCh, 2);
            expectEquals(d.len, (juce::int64) 96000); // round(2.0 * 48000)
        }
    }

    //==========================================================================
    // 3. 32bit float ヘッダ + ほぼ完全な内容一致 (モノラル→L/R 複製)
    void testFloat32HeaderAndExactContent()
    {
        beginTest("WAV 32bit float header + exact mono content");

        juce::AudioFormatManager fmt; fmt.registerBasicFormats();
        TrackManager tm(fmt);
        auto src = outDir.getChildFile("f32_src.wav");
        expect(writeMonoConst(src, (int)(0.1 * kSR), 0.5f), "source write");
        auto* t = tm.addTrack("a", false);
        auto* c = t->addClip(src, 0.0, 0.1);
        c->setFadeInSecs(0.0); c->setFadeOutSecs(0.0);
        t->setVolume(0.0f); t->setPan(0.0f);

        AudioEngine engine; engine.preparePlayback(tm);

        auto out = outDir.getChildFile("f32.wav");
        auto o = baseOpts(out, 32, 0.1);
        juce::String err;
        expect(ExportEngine::render(engine, o, {}, {}, &err), "render: " + err);

        auto d = readAudio(out);
        expect(d.ok, "readable WAV");
        expectEquals(d.sampleRate, 48000.0);
        expectEquals(d.bits, 32);
        expect(d.isFloat, "floating point sample format");
        expectEquals(d.numCh, 2);
        expectEquals(d.len, (juce::int64) 4800);
        // 32f なのでディザ無し・量子化無し・ゲイン1.0・ピーク0.5<上限 → 入力と一致
        expectAllClose(d.samples, 0, 0.5f, 1.0e-6f, "L == source");
        expectAllClose(d.samples, 1, 0.5f, 1.0e-6f, "R == source (mono duplicated)");
    }

    //==========================================================================
    // 4. AIFF コントラスト (ヘッダ + 尺)
    void testAiffContrast()
    {
        beginTest("AIFF 24bit header + length contrast");

        juce::AudioFormatManager fmt; fmt.registerBasicFormats();
        TrackManager tm(fmt);
        auto src = outDir.getChildFile("aiff_src.wav");
        expect(writeMonoConst(src, (int)(2.0 * kSR), 0.5f), "source write");
        auto* t = tm.addTrack("a", false);
        auto* c = t->addClip(src, 0.0, 2.0);
        c->setFadeInSecs(0.0); c->setFadeOutSecs(0.0);

        AudioEngine engine; engine.preparePlayback(tm);

        auto out = outDir.getChildFile("contrast.aiff");
        auto o = baseOpts(out, 24, 2.0);
        o.format = ExportEngine::Format::AIFF;
        juce::String err;
        expect(ExportEngine::render(engine, o, {}, {}, &err), "render: " + err);

        auto d = readAudio(out, /*aiff*/ true);
        expect(d.ok, "readable AIFF");
        expectEquals(d.sampleRate, 48000.0);
        expectEquals(d.bits, 24);
        expect(!d.isFloat, "integral");
        expectEquals(d.numCh, 2);
        expectEquals(d.len, (juce::int64) 96000);
    }

    //==========================================================================
    // 5. サンプルレート変換書き出し (48k → 44.1k) のヘッダ + 尺
    void testSampleRateConversion()
    {
        beginTest("sample-rate conversion 48k -> 44100 header + length");

        juce::AudioFormatManager fmt; fmt.registerBasicFormats();
        TrackManager tm(fmt);
        auto src = outDir.getChildFile("sr_src.wav");
        expect(writeMonoConst(src, (int)(2.0 * kSR), 0.5f), "source write");
        auto* t = tm.addTrack("a", false);
        auto* c = t->addClip(src, 0.0, 2.0);
        c->setFadeInSecs(0.0); c->setFadeOutSecs(0.0);

        AudioEngine engine; engine.preparePlayback(tm);

        auto out = outDir.getChildFile("sr44100.wav");
        auto o = baseOpts(out, 32, 2.0);
        o.sampleRate = 44100.0;
        juce::String err;
        expect(ExportEngine::render(engine, o, {}, {}, &err), "render: " + err);

        auto d = readAudio(out);
        expect(d.ok, "readable WAV");
        expectEquals(d.sampleRate, 44100.0);
        expectEquals(d.bits, 32);
        expect(d.isFloat, "float");
        expectEquals(d.numCh, 2);
        // 96000 * 44100 / 48000 = 88200 (整数なので厳密一致)
        expectEquals(d.len, (juce::int64) 88200);
    }

    //==========================================================================
    // 6. モノラルダウンミックス: ステレオ(0.4 / -0.2) → 1ch = 0.5*(L+R) = 0.1
    void testMonoDownmix()
    {
        beginTest("mono downmix = 0.5*(L+R)");

        juce::AudioFormatManager fmt; fmt.registerBasicFormats();
        TrackManager tm(fmt);
        auto src = outDir.getChildFile("dmx_src.wav");
        expect(writeStereoConst(src, (int)(0.05 * kSR), 0.4f, -0.2f), "source write");
        auto* t = tm.addTrack("s", true);
        auto* c = t->addClip(src, 0.0, 0.05);
        c->setFadeInSecs(0.0); c->setFadeOutSecs(0.0);
        t->setVolume(0.0f); t->setPan(0.0f);

        AudioEngine engine; engine.preparePlayback(tm);

        auto out = outDir.getChildFile("downmix.wav");
        auto o = baseOpts(out, 32, 0.05);
        o.numChannels = 1;          // モノラル書き出し
        juce::String err;
        expect(ExportEngine::render(engine, o, {}, {}, &err), "render: " + err);

        auto d = readAudio(out);
        expect(d.ok, "readable");
        expectEquals(d.numCh, 1);
        expectEquals(d.len, (juce::int64) 2400);
        expectAllClose(d.samples, 0, 0.1f, 1.0e-6f, "downmix == 0.5*(0.4-0.2)");
    }

    //==========================================================================
    // 7. ステレオソースのチャンネルマッピング (ch0→L, ch1→R)
    void testStereoChannelMapping()
    {
        beginTest("stereo source channel mapping ch0->L ch1->R");

        juce::AudioFormatManager fmt; fmt.registerBasicFormats();
        TrackManager tm(fmt);
        auto src = outDir.getChildFile("map_src.wav");
        expect(writeStereoConst(src, (int)(0.05 * kSR), 0.4f, -0.2f), "source write");
        auto* t = tm.addTrack("s", true);
        auto* c = t->addClip(src, 0.0, 0.05);
        c->setFadeInSecs(0.0); c->setFadeOutSecs(0.0);
        t->setVolume(0.0f); t->setPan(0.0f);

        AudioEngine engine; engine.preparePlayback(tm);

        auto out = outDir.getChildFile("map.wav");
        auto o = baseOpts(out, 32, 0.05);   // 2ch のまま
        juce::String err;
        expect(ExportEngine::render(engine, o, {}, {}, &err), "render: " + err);

        auto d = readAudio(out);
        expect(d.ok, "readable");
        expectEquals(d.numCh, 2);
        expectAllClose(d.samples, 0,  0.4f, 1.0e-6f, "L == 0.4");
        expectAllClose(d.samples, 1, -0.2f, 1.0e-6f, "R == -0.2");
    }

    //==========================================================================
    // 8. ポストフェーダーのトラックボリューム (dB) が反映される
    void testPostFaderVolume()
    {
        beginTest("post-fader track volume (dB) applied");

        juce::AudioFormatManager fmt; fmt.registerBasicFormats();
        TrackManager tm(fmt);
        auto src = outDir.getChildFile("vol_src.wav");
        expect(writeMonoConst(src, (int)(0.1 * kSR), 0.5f), "source write");
        auto* t = tm.addTrack("v", false);
        auto* c = t->addClip(src, 0.0, 0.1);
        c->setFadeInSecs(0.0); c->setFadeOutSecs(0.0);
        t->setPan(0.0f);
        t->setVolume(-6.0206f);     // ≒ gain 0.5

        AudioEngine engine; engine.preparePlayback(tm);

        auto out = outDir.getChildFile("vol.wav");
        auto o = baseOpts(out, 32, 0.1);
        juce::String err;
        expect(ExportEngine::render(engine, o, {}, {}, &err), "render: " + err);

        const float expected = 0.5f * juce::Decibels::decibelsToGain(-6.0206f); // ≒0.25
        auto d = readAudio(out);
        expect(d.ok, "readable");
        expectAllClose(d.samples, 0, expected, 1.0e-4f, "L scaled by track gain");
        expectAllClose(d.samples, 1, expected, 1.0e-4f, "R scaled by track gain");
    }

    //==========================================================================
    // 9. ポストフェーダーのパン (リニアバランス則、center で減衰なし)
    //    pan=-0.5 → panL=1.0, panR=0.5。ソース0.5 → L=0.5, R=0.25 (peak<上限)。
    void testPostFaderPanLinearLaw()
    {
        beginTest("post-fader pan uses linear balance law");

        juce::AudioFormatManager fmt; fmt.registerBasicFormats();
        TrackManager tm(fmt);
        auto src = outDir.getChildFile("pan_src.wav");
        expect(writeMonoConst(src, (int)(0.1 * kSR), 0.5f), "source write");
        auto* t = tm.addTrack("p", false);
        auto* c = t->addClip(src, 0.0, 0.1);
        c->setFadeInSecs(0.0); c->setFadeOutSecs(0.0);
        t->setVolume(0.0f);
        t->setPan(-0.5f);           // 左寄せ: panL=1.0, panR=1+(-0.5)=0.5

        AudioEngine engine; engine.preparePlayback(tm);

        auto out = outDir.getChildFile("pan.wav");
        auto o = baseOpts(out, 32, 0.1);
        juce::String err;
        expect(ExportEngine::render(engine, o, {}, {}, &err), "render: " + err);

        auto d = readAudio(out);
        expect(d.ok, "readable");
        expectAllClose(d.samples, 0, 0.5f,  1.0e-6f, "L unattenuated (panL=1.0)");
        expectAllClose(d.samples, 1, 0.25f, 1.0e-6f, "R = 0.5 * panR(0.5)");
    }

    //==========================================================================
    // 10. クリップゲインは pre/post 両モードで適用される
    void testClipGainBothFaderModes()
    {
        beginTest("clip gain applied in both pre/post fader modes");

        for (bool preFader : { true, false })
        {
            juce::AudioFormatManager fmt; fmt.registerBasicFormats();
            TrackManager tm(fmt);
            auto src = outDir.getChildFile("cg_src.wav");
            expect(writeMonoConst(src, (int)(0.1 * kSR), 0.5f), "source write");
            auto* t = tm.addTrack("c", false);
            auto* c = t->addClip(src, 0.0, 0.1);
            c->setFadeInSecs(0.0); c->setFadeOutSecs(0.0);
            c->setGain(0.5f);
            t->setVolume(0.0f); t->setPan(0.0f);

            AudioEngine engine; engine.preparePlayback(tm);

            auto out = outDir.getChildFile(juce::String(preFader ? "cg_pre" : "cg_post") + ".wav");
            auto o = baseOpts(out, 32, 0.1);
            o.preFader = preFader;
            juce::String err;
            expect(ExportEngine::render(engine, o, {}, {}, &err), "render: " + err);

            auto d = readAudio(out);
            expect(d.ok, "readable");
            // 0.5(source) * 0.5(clip gain) = 0.25 (track vol 0dB なので post でも同じ)
            expectAllClose(d.samples, 0, 0.25f, 1.0e-6f,
                           juce::String(preFader ? "preFader" : "postFader") + " clip gain");
        }
    }

    //==========================================================================
    // 11. preFader と postFader で結果が異なる (track vol を無視するか否か)
    void testPreVsPostFaderDiffer()
    {
        beginTest("pre-fader vs post-fader differ by track volume");

        auto runOnce = [&](bool preFader) -> float
        {
            juce::AudioFormatManager fmt; fmt.registerBasicFormats();
            TrackManager tm(fmt);
            auto src = outDir.getChildFile("pp_src.wav");
            writeMonoConst(src, (int)(0.1 * kSR), 0.4f);
            auto* t = tm.addTrack("x", false);
            auto* c = t->addClip(src, 0.0, 0.1);
            c->setFadeInSecs(0.0); c->setFadeOutSecs(0.0);
            t->setVolume(-6.0206f); t->setPan(0.0f);

            AudioEngine engine; engine.preparePlayback(tm);
            auto out = outDir.getChildFile(juce::String(preFader ? "pp_pre" : "pp_post") + ".wav");
            auto o = baseOpts(out, 32, 0.1); o.preFader = preFader;
            juce::String err;
            ExportEngine::render(engine, o, {}, {}, &err);
            return readAudio(out).samples.getSample(0, 100);
        };

        const float pre  = runOnce(true);
        const float post = runOnce(false);
        expectWithinAbsoluteError(pre, 0.4f, 1.0e-5f);   // track vol 無視
        expectWithinAbsoluteError(post, 0.4f * juce::Decibels::decibelsToGain(-6.0206f), 1.0e-4f);
        expect(std::abs(pre - post) > 0.1f, "modes diverge");
    }

    //==========================================================================
    // 12. 複数トラックの加算ミックス
    void testMultiTrackSummation()
    {
        beginTest("two tracks sum into the mix");

        juce::AudioFormatManager fmt; fmt.registerBasicFormats();
        TrackManager tm(fmt);
        auto s1 = outDir.getChildFile("sum1.wav");
        auto s2 = outDir.getChildFile("sum2.wav");
        writeMonoConst(s1, (int)(0.1 * kSR), 0.3f);
        writeMonoConst(s2, (int)(0.1 * kSR), 0.3f);
        for (auto* src : { &s1, &s2 })
        {
            auto* t = tm.addTrack("t", false);
            auto* c = t->addClip(*src, 0.0, 0.1);
            c->setFadeInSecs(0.0); c->setFadeOutSecs(0.0);
            t->setVolume(0.0f); t->setPan(0.0f);
        }

        AudioEngine engine; engine.preparePlayback(tm);

        auto out = outDir.getChildFile("sum.wav");
        auto o = baseOpts(out, 32, 0.1);
        juce::String err;
        expect(ExportEngine::render(engine, o, {}, {}, &err), "render: " + err);

        auto d = readAudio(out);
        expect(d.ok, "readable");
        expectAllClose(d.samples, 0, 0.6f, 1.0e-6f, "0.3 + 0.3 = 0.6 (peak < ceiling)");
    }

    //==========================================================================
    // 13. 合計が上限を超えるとピーク超過保護で 0.989 に減衰
    void testPeakGuardOnMultiTrackSum()
    {
        beginTest("peak guard scales over-ceiling sum to 0.989");

        juce::AudioFormatManager fmt; fmt.registerBasicFormats();
        TrackManager tm(fmt);
        auto s1 = outDir.getChildFile("pg1.wav");
        auto s2 = outDir.getChildFile("pg2.wav");
        writeMonoConst(s1, (int)(0.1 * kSR), 0.6f);
        writeMonoConst(s2, (int)(0.1 * kSR), 0.6f);   // 合計 1.2 > 0.989
        for (auto* src : { &s1, &s2 })
        {
            auto* t = tm.addTrack("t", false);
            auto* c = t->addClip(*src, 0.0, 0.1);
            c->setFadeInSecs(0.0); c->setFadeOutSecs(0.0);
            t->setVolume(0.0f); t->setPan(0.0f);
        }

        AudioEngine engine; engine.preparePlayback(tm);

        auto out = outDir.getChildFile("pgsum.wav");
        auto o = baseOpts(out, 32, 0.1);   // peakGuard=true (既定)
        juce::String err;
        expect(ExportEngine::render(engine, o, {}, {}, &err), "render: " + err);

        auto d = readAudio(out);
        expect(d.ok, "readable");
        expectAllClose(d.samples, 0, kCeiling, 1.0e-4f, "1.2 * (0.989/1.2) = 0.989");
    }

    //==========================================================================
    // 14. ピーク超過信号 (1.0) → ピーク超過保護で 0.989
    void testPeakGuardAttenuatesOverCeiling()
    {
        beginTest("peak guard attenuates a full-scale signal to ceiling");

        juce::AudioFormatManager fmt; fmt.registerBasicFormats();
        TrackManager tm(fmt);
        auto src = outDir.getChildFile("ceil_src.wav");
        writeMonoConst(src, (int)(0.2 * kSR), 1.0f);
        auto* t = tm.addTrack("c", false);
        auto* c = t->addClip(src, 0.0, 0.2);
        c->setFadeInSecs(0.0); c->setFadeOutSecs(0.0);
        t->setVolume(0.0f); t->setPan(0.0f);

        AudioEngine engine; engine.preparePlayback(tm);

        auto out = outDir.getChildFile("ceil.wav");
        auto o = baseOpts(out, 32, 0.2);
        juce::String err;
        expect(ExportEngine::render(engine, o, {}, {}, &err), "render: " + err);

        auto d = readAudio(out);
        expect(d.ok, "readable");
        expectWithinAbsoluteError(d.maxAbs(), kCeiling, 1.0e-3f);
        expectAllClose(d.samples, 0, kCeiling, 1.0e-3f, "L at ceiling");
        expectAllClose(d.samples, 1, kCeiling, 1.0e-3f, "R at ceiling (mono dup)");
    }

    //==========================================================================
    // 15. 上限以下はピーク超過保護で変化せず、ON/OFF で同一 (32f)
    void testPeakGuardLeavesUnderCeilingUntouched()
    {
        beginTest("peak guard is a no-op below ceiling (on == off)");

        auto render = [&](bool guard) -> AudioFileData
        {
            juce::AudioFormatManager fmt; fmt.registerBasicFormats();
            TrackManager tm(fmt);
            auto src = outDir.getChildFile("under_src.wav");
            writeMonoConst(src, (int)(0.1 * kSR), 0.5f);
            auto* t = tm.addTrack("u", false);
            auto* c = t->addClip(src, 0.0, 0.1);
            c->setFadeInSecs(0.0); c->setFadeOutSecs(0.0);
            t->setVolume(0.0f); t->setPan(0.0f);

            AudioEngine engine; engine.preparePlayback(tm);
            auto out = outDir.getChildFile(juce::String(guard ? "under_on" : "under_off") + ".wav");
            auto o = baseOpts(out, 32, 0.1); o.peakGuard = guard;
            juce::String err;
            ExportEngine::render(engine, o, {}, {}, &err);
            return readAudio(out);
        };

        auto on  = render(true);
        auto off = render(false);
        expectAllClose(on.samples,  0, 0.5f, 1.0e-6f, "guard on leaves 0.5");
        expectAllClose(off.samples, 0, 0.5f, 1.0e-6f, "guard off leaves 0.5");
        // 32f + ディザ無し → ON/OFF はビット一致
        float maxDiff = 0.0f;
        for (int i = 0; i < on.samples.getNumSamples(); ++i)
            maxDiff = juce::jmax(maxDiff, std::abs(on.samples.getSample(0, i) - off.samples.getSample(0, i)));
        expect(maxDiff <= 1.0e-9f, "guard on/off identical at 32f, maxDiff=" + juce::String(maxDiff, 10));
    }

    //==========================================================================
    // 16. peakGuard OFF なら 32f で 1.0 超のサンプルが保持される
    void testPeakGuardOffPreservesOverUnity()
    {
        beginTest("peak guard off preserves >1.0 samples in 32f");

        const float expected = 0.8f * juce::Decibels::decibelsToGain(6.0f); // ≒1.5962

        juce::AudioFormatManager fmt; fmt.registerBasicFormats();
        TrackManager tm(fmt);
        auto src = outDir.getChildFile("ou_src.wav");
        writeMonoConst(src, (int)(0.1 * kSR), 0.8f);
        auto* t = tm.addTrack("o", false);
        auto* c = t->addClip(src, 0.0, 0.1);
        c->setFadeInSecs(0.0); c->setFadeOutSecs(0.0);
        t->setVolume(6.0f); t->setPan(0.0f);     // +6 dB ≒ x1.995

        AudioEngine engine; engine.preparePlayback(tm);

        auto out = outDir.getChildFile("overunity.wav");
        auto o = baseOpts(out, 32, 0.1);
        o.peakGuard = false;
        juce::String err;
        expect(ExportEngine::render(engine, o, {}, {}, &err), "render: " + err);

        auto d = readAudio(out);
        expect(d.ok, "readable");
        expectAllClose(d.samples, 0, expected, 5.0e-3f, "0.8 * +6dB ~= 1.5962 preserved");

        // 同条件で peakGuard ON → 0.989 に減衰
        auto out2 = outDir.getChildFile("overunity_guard.wav");
        auto o2 = baseOpts(out2, 32, 0.1); o2.peakGuard = true;
        ExportEngine::render(engine, o2, {}, {}, &err);
        auto d2 = readAudio(out2);
        expectWithinAbsoluteError(d2.maxAbs(), kCeiling, 1.0e-3f);
    }

    //==========================================================================
    // 17. クリップ開始位置 / ファイルオフセットに応じてマーカーが正しいサンプルに来る
    void testRangeOffsetMarkerPlacement()
    {
        beginTest("clip start offset places marker at correct sample");

        juce::AudioFormatManager fmt; fmt.registerBasicFormats();
        TrackManager tm(fmt);
        // 0.2s モノラル、全 0 で file sample 240 (=0.005s) のみ 0.5
        const int n = (int)(0.2 * kSR);
        juce::AudioBuffer<float> b(1, n); b.clear();
        b.setSample(0, 240, 0.5f);
        auto src = outDir.getChildFile("marker_src.wav");
        expect(writeFloatWav(src, kSR, b), "source write");

        auto* t = tm.addTrack("m", false);
        auto* c = t->addClip(src, /*startPos*/ 0.1, /*dur*/ 0.2);
        c->setFileOffset(0.0);
        c->setFadeInSecs(0.0); c->setFadeOutSecs(0.0);
        t->setVolume(0.0f); t->setPan(0.0f);

        AudioEngine engine; engine.preparePlayback(tm);

        auto out = outDir.getChildFile("marker.wav");
        auto o = baseOpts(out, 32, 0.35);   // [0.0, 0.35) → 16800 サンプル
        juce::String err;
        expect(ExportEngine::render(engine, o, {}, {}, &err), "render: " + err);

        auto d = readAudio(out);
        expect(d.ok, "readable");
        expectEquals(d.len, (juce::int64) 16800);
        // クリップは 0.1s (=4800) から開始、マーカーは +0.005s → export index 5040
        expectWithinAbsoluteError(d.samples.getSample(0, 5040), 0.5f, 1.0e-6f);
        expectWithinAbsoluteError(d.samples.getSample(1, 5040), 0.5f, 1.0e-6f);
        expectWithinAbsoluteError(d.samples.getSample(0, 5039), 0.0f, 1.0e-6f);
        expectWithinAbsoluteError(d.samples.getSample(0, 5041), 0.0f, 1.0e-6f);
        // クリップ開始前 (index < 4800) は無音
        expectWithinAbsoluteError(d.samples.getSample(0, 100), 0.0f, 1.0e-6f);
        expectWithinAbsoluteError(d.samples.getSample(0, 4799), 0.0f, 1.0e-6f);
    }

    //==========================================================================
    // 18. selectedTrackIndices 指定時は Mute/Solo を無視し指定トラックのみ
    void testSelectedTrackIndicesIgnoreMuteSolo()
    {
        beginTest("selectedTrackIndices ignores mute/solo");

        juce::AudioFormatManager fmt; fmt.registerBasicFormats();
        TrackManager tm(fmt);
        const float vals[3] = { 0.10f, 0.20f, 0.40f };
        for (int i = 0; i < 3; ++i)
        {
            auto src = outDir.getChildFile("sel" + juce::String(i) + ".wav");
            writeMonoConst(src, (int)(0.1 * kSR), vals[i]);
            auto* t = tm.addTrack("t" + juce::String(i), false);
            auto* c = t->addClip(src, 0.0, 0.1);
            c->setFadeInSecs(0.0); c->setFadeOutSecs(0.0);
            t->setVolume(0.0f); t->setPan(0.0f);
        }
        tm.getTrack(0)->setMuted(true);     // 通常なら除外されるトラック
        tm.getTrack(2)->setSoloed(true);    // 通常なら唯一鳴るトラック

        AudioEngine engine; engine.preparePlayback(tm);

        // 明示選択 {0,1}: mute/solo を無視 → 0.10 + 0.20 = 0.30
        auto out = outDir.getChildFile("sel.wav");
        auto o = baseOpts(out, 32, 0.1);
        o.peakGuard = false;
        o.selectedTrackIndices = { 0, 1 };
        juce::String err;
        expect(ExportEngine::render(engine, o, {}, {}, &err), "render: " + err);

        auto d = readAudio(out);
        expect(d.ok, "readable");
        expectAllClose(d.samples, 0, 0.30f, 1.0e-5f, "{0,1} ignoring mute/solo = 0.30");
    }

    //==========================================================================
    // 19. 空選択は Solo / Mute を尊重する
    void testEmptySelectionHonorsSoloMute()
    {
        beginTest("empty selection honors solo and mute");

        auto buildAndRender = [&](bool soloT2, bool muteT0) -> float
        {
            juce::AudioFormatManager fmt; fmt.registerBasicFormats();
            TrackManager tm(fmt);
            const float vals[3] = { 0.10f, 0.20f, 0.40f };
            for (int i = 0; i < 3; ++i)
            {
                auto src = outDir.getChildFile("es" + juce::String(i) + ".wav");
                writeMonoConst(src, (int)(0.1 * kSR), vals[i]);
                auto* t = tm.addTrack("t" + juce::String(i), false);
                auto* c = t->addClip(src, 0.0, 0.1);
                c->setFadeInSecs(0.0); c->setFadeOutSecs(0.0);
                t->setVolume(0.0f); t->setPan(0.0f);
            }
            if (muteT0) tm.getTrack(0)->setMuted(true);
            if (soloT2) tm.getTrack(2)->setSoloed(true);

            AudioEngine engine; engine.preparePlayback(tm);
            auto out = outDir.getChildFile(juce::String(soloT2 ? "es_solo" : "es_mute") + ".wav");
            auto o = baseOpts(out, 32, 0.1);
            o.peakGuard = false;          // selectedTrackIndices は空
            juce::String err;
            ExportEngine::render(engine, o, {}, {}, &err);
            return readAudio(out).samples.getSample(0, 100);
        };

        // (A) track2 を solo → 0.40 のみ
        expectWithinAbsoluteError(buildAndRender(/*solo*/ true,  /*mute*/ false), 0.40f, 1.0e-5f);
        // (B) solo 無し、track0 を mute → 0.20 + 0.40 = 0.60
        expectWithinAbsoluteError(buildAndRender(/*solo*/ false, /*mute*/ true),  0.60f, 1.0e-5f);
    }

    //==========================================================================
    // 20. クリックトラック: 通常ミックスには鳴っていれば混ざる (要望 2026-07)。
    //     クリップは再生されず (合成メトロノームのみ)、ミュートで除外、明示選択 (stems) は従来どおり除外
    void testClickTrackExcluded()
    {
        beginTest("click track: mixed into 2-mix when audible, excluded from stems");

        juce::AudioFormatManager fmt; fmt.registerBasicFormats();
        TrackManager tm(fmt);
        auto sNorm = outDir.getChildFile("clk_norm.wav");
        writeMonoConst(sNorm, (int)(0.1 * kSR), 0.30f);
        auto* tn = tm.addTrack("normal", false);
        auto* cn = tn->addClip(sNorm, 0.0, 0.1);
        cn->setFadeInSecs(0.0); cn->setFadeOutSecs(0.0);
        tn->setVolume(0.0f); tn->setPan(0.0f);

        auto* click = tm.addClickTrack();
        click->setVolume(-20.0f);   // クリック振幅を抑えて閾値検証を安定させる
        auto sClk = outDir.getChildFile("clk_click.wav");
        writeMonoConst(sClk, (int)(0.1 * kSR), 0.50f);
        auto* cc = click->addClip(sClk, 0.0, 0.1);
        cc->setFadeInSecs(0.0); cc->setFadeOutSecs(0.0);

        // クリックトラックの index を取得
        int clickIdx = -1;
        for (int i = 0; i < tm.getTrackCount(); ++i)
            if (tm.getTrack(i)->isClickTrack()) clickIdx = i;
        expect(clickIdx >= 0, "click track present");

        AudioEngine engine; engine.preparePlayback(tm);

        // (A1) クリックをミュート: 通常ミックスに混ざらず 0.30 のみ
        {
            click->setMuted(true);
            auto out = outDir.getChildFile("clk_mix_muted.wav");
            auto o = baseOpts(out, 32, 0.1); o.peakGuard = false;
            juce::String err;
            expect(ExportEngine::render(engine, o, {}, {}, &err), "render: " + err);
            auto d = readAudio(out);
            expectAllClose(d.samples, 0, 0.30f, 1.0e-5f, "muted click absent from mix");
            click->setMuted(false);
        }
        // (A2) クリック非ミュート: 合成メトロノームが混ざる。ただしクリップ (0.5) は再生されない
        {
            auto out = outDir.getChildFile("clk_mix.wav");
            auto o = baseOpts(out, 32, 0.1); o.peakGuard = false;
            juce::String err;
            expect(ExportEngine::render(engine, o, {}, {}, &err), "render: " + err);
            auto d = readAudio(out);
            float maxDev = 0.0f;
            const float* p = d.samples.getReadPointer(0);
            for (int i = 0; i < d.samples.getNumSamples(); ++i)
                maxDev = juce::jmax(maxDev, std::abs(p[i] - 0.30f));
            expect(maxDev > 0.005f, "synthesized click mixed into 2-mix");
            expect(d.maxAbs() < 0.45f, "click track clip (0.5) is NOT played");
        }
        // (A3) 実アプリの 2 ミックス経路: 明示トラックリスト + includeClick=true → 混ざる
        //      (2 ミックスは常に selectedTrackIndices を渡すため、このフラグが唯一の経路。
        //       フラグ無しで明示リストだけだと混ざらなかった実機バグの回帰テスト)
        {
            auto out = outDir.getChildFile("clk_mix_explicit.wav");
            auto o = baseOpts(out, 32, 0.1); o.peakGuard = false;
            o.selectedTrackIndices = { 0 };   // normal トラックのみ明示 (実アプリと同じ形)
            o.includeClick = true;
            juce::String err;
            expect(ExportEngine::render(engine, o, {}, {}, &err), "render: " + err);
            auto d = readAudio(out);
            float maxDev = 0.0f;
            const float* p = d.samples.getReadPointer(0);
            for (int i = 0; i < d.samples.getNumSamples(); ++i)
                maxDev = juce::jmax(maxDev, std::abs(p[i] - 0.30f));
            expect(maxDev > 0.005f, "includeClick mixes click with explicit track list");
        }
        // (B) クリックを明示選択 (stems・includeClick 無し) → 従来どおり除外され無音
        {
            auto out = outDir.getChildFile("clk_only.wav");
            auto o = baseOpts(out, 32, 0.1); o.peakGuard = false;
            o.selectedTrackIndices = { clickIdx };
            juce::String err;
            expect(ExportEngine::render(engine, o, {}, {}, &err), "render: " + err);
            auto d = readAudio(out);
            expectWithinAbsoluteError(d.maxAbs(), 0.0f, 1.0e-4f);
        }
    }

    //==========================================================================
    // 20.5. MIDI トラック (内蔵シンセ) が書き出しに乗る
    //       (旧実装は renderOfflineRange が MIDI を描画せず常に無音だった・2026-07 修正の回帰テスト)
    void testMidiTrackRendersInExport()
    {
        beginTest("MIDI track (internal synth) renders in offline export");

        juce::AudioFormatManager fmt; fmt.registerBasicFormats();
        TrackManager tm(fmt);
        auto* t = tm.addTrack("midi", false);
        t->setMidiTrack(true);
        t->setVolume(0.0f); t->setPan(0.0f);
        auto* mc = t->addMidiClip(0.0, 1.0);
        expect(mc != nullptr, "midi clip created");
        // A4 (69) を 0.1s on / 0.8s off (クリップ相対秒 = クリップが 0 開始なので絶対秒と同じ)
        auto& seq = mc->getSequence();
        seq.addEvent(juce::MidiMessage::noteOn(1, 69, (juce::uint8) 100), 0.1);
        seq.addEvent(juce::MidiMessage::noteOff(1, 69), 0.8);
        seq.updateMatchedPairs();

        AudioEngine engine;
        engine.preparePlayback(tm);

        auto out = outDir.getChildFile("midi.wav");
        auto o = baseOpts(out, 32, 1.0);
        o.dither = false; o.peakGuard = false;
        juce::String err;
        expect(ExportEngine::render(engine, o, {}, {}, &err), "render: " + err);

        auto d = readAudio(out);
        expect(d.ok && d.len == (juce::int64) kSR, "readable, 1s output");
        // ノート区間は鳴っている (フル振幅 = velocity(100/127) * 0.25 ~= 0.197)
        const int sOn = (int)(0.3 * kSR), nOn = (int)(0.3 * kSR);
        expect(d.samples.getMagnitude(0, sOn, nOn) > 0.10f, "note region is audible");
        expect(d.samples.getMagnitude(1, sOn, nOn) > 0.10f, "note audible on both channels");
        // ノート前は無音 / ノートオフ + リリース (50ms) 後も無音
        expect(d.samples.getMagnitude(0, 0, (int)(0.08 * kSR)) < 1.0e-4f, "silence before note");
        expect(d.samples.getMagnitude(0, (int)(0.95 * kSR), (int)(0.05 * kSR)) < 1.0e-3f,
               "silence after release");

        // ミュートは通常書き出しから除外 / 明示選択 (stems) はミュートを無視して鳴る
        t->setMuted(true);
        expect(ExportEngine::render(engine, o, {}, {}, &err), "render muted: " + err);
        d = readAudio(out);
        expect(d.maxAbs() < 1.0e-6f, "muted MIDI track exports silence");

        auto o2 = o; o2.selectedTrackIndices = { 0 };
        expect(ExportEngine::render(engine, o2, {}, {}, &err), "render selected: " + err);
        d = readAudio(out);
        expect(d.samples.getMagnitude(0, sOn, nOn) > 0.10f,
               "explicit selection ignores mute for MIDI track");
        t->setMuted(false);
    }

    //==========================================================================
    // 20.6. リバーブ送り (Rev スライダー) が書き出しに乗る
    //       (旧実装は送りバスが無くドライのみだった・2026-07 修正の回帰テスト)
    void testReverbSendInExport()
    {
        beginTest("reverb send (Rev slider) renders in offline export");

        juce::AudioFormatManager fmt; fmt.registerBasicFormats();
        TrackManager tm(fmt);
        auto src = outDir.getChildFile("rev_src.wav");
        writeMonoConst(src, (int)(0.2 * kSR), 0.4f);
        auto* t = tm.addTrack("vo", false);
        auto* c = t->addClip(src, 0.0, 0.2);
        c->setFadeInSecs(0.0); c->setFadeOutSecs(0.0);
        t->setVolume(0.0f); t->setPan(0.0f);

        AudioEngine engine; engine.preparePlayback(tm);
        const int tailFrom = (int)(0.3 * kSR);
        const int tailLen  = (int)(0.5 * kSR);

        // (A) Rev = 0: クリップ終了後は完全無音 (テール無し)
        {
            t->setReverbSend(0.0f);
            auto out = outDir.getChildFile("rev_dry.wav");
            auto o = baseOpts(out, 32, 1.0); o.peakGuard = false; o.dither = false;
            juce::String err;
            expect(ExportEngine::render(engine, o, {}, {}, &err), "render dry: " + err);
            auto d = readAudio(out);
            expect(d.samples.getMagnitude(0, tailFrom, tailLen) < 1.0e-5f,
                   "no tail when send is 0");
        }
        // (B) Rev = 1: クリップ終了後にリバーブテールが残る + ドライは維持
        {
            t->setReverbSend(1.0f);
            auto out = outDir.getChildFile("rev_wet.wav");
            auto o = baseOpts(out, 32, 1.0); o.peakGuard = false; o.dither = false;
            juce::String err;
            expect(ExportEngine::render(engine, o, {}, {}, &err), "render wet: " + err);
            auto d = readAudio(out);
            expect(d.samples.getMagnitude(0, tailFrom, tailLen) > 1.0e-3f,
                   "reverb tail present after clip end");
            expect(d.samples.getMagnitude(0, 0, (int)(0.1 * kSR)) > 0.3f,
                   "dry signal still present during clip");
        }
        // (C) Pre-Fader は素のクリップ音のみ: Rev=1 でもテール無し + ドライは素 (0.4)
        //     (Pre = Vol/Pan/Rev/master を通さない。要望 2026-07 の回帰テスト)
        {
            t->setReverbSend(1.0f);
            auto out = outDir.getChildFile("rev_pre.wav");
            auto o = baseOpts(out, 32, 1.0); o.peakGuard = false; o.dither = false;
            o.preFader = true;
            juce::String err;
            expect(ExportEngine::render(engine, o, {}, {}, &err), "render pre: " + err);
            auto d = readAudio(out);
            expect(d.samples.getMagnitude(0, tailFrom, tailLen) < 1.0e-5f,
                   "no reverb tail in pre-fader export");
            expectAllClose(d.samples, 0, 0.4f, 1.0e-5f,
                           "pre-fader is raw clip (0.4), no reverb summed", 0, (int)(0.15 * kSR));
        }
        t->setReverbSend(0.0f);
    }

    //==========================================================================
    // 21. 16bit ディザは中間レベルの定常信号を複数量子化レベルに分散させる
    //     (ノイズシェイピングのため偏差は数 LSB に及ぶ。構造的性質で検証)
    void testDither16PerturbsSteadyLevel()
    {
        beginTest("16bit dither perturbs a steady inter-grid level");

        auto render16 = [&](bool dither) -> AudioFileData
        {
            juce::AudioFormatManager fmt; fmt.registerBasicFormats();
            TrackManager tm(fmt);
            auto src = outDir.getChildFile("dth_src.wav");
            writeMonoConst(src, (int)(1.0 * kSR), 0.30001f);   // LSB グリッド間の値
            auto* t = tm.addTrack("d", false);
            auto* c = t->addClip(src, 0.0, 1.0);
            c->setFadeInSecs(0.0); c->setFadeOutSecs(0.0);
            t->setVolume(0.0f); t->setPan(0.0f);

            AudioEngine engine; engine.preparePlayback(tm);
            auto out = outDir.getChildFile(juce::String(dither ? "dth_on" : "dth_off") + ".wav");
            auto o = baseOpts(out, 16, 1.0); o.dither = dither;
            juce::String err;
            ExportEngine::render(engine, o, {}, {}, &err);
            return readAudio(out);
        };

        auto distinctLevels = [](const AudioFileData& d, int from, int to)
        {
            std::set<int> levels;
            const float* p = d.samples.getReadPointer(0);
            for (int i = from; i < to; ++i)
                levels.insert((int) std::lround(p[i] / kLsb16));
            return levels.size();
        };

        auto off = render16(false);
        auto on  = render16(true);
        expect(off.ok && on.ok, "both readable");

        const int from = 100, to = (int) on.len - 100;

        // ディザ無し: 単一レベルに量子化
        expectEquals((int) distinctLevels(off, from, to), 1, "no-dither => single level");

        // ディザ有り: 複数レベルに分散
        expectGreaterOrEqual((int) distinctLevels(on, from, to), 2);

        // ディザ有りは無ディザ出力と相当数のサンプルで差が出る
        int diffCount = 0;
        for (int i = from; i < to; ++i)
            if (std::abs(on.samples.getSample(0, i) - off.samples.getSample(0, i)) > 0.5f * kLsb16)
                ++diffCount;
        expect(diffCount > (to - from) / 20, "dither changes a substantial fraction of samples");

        // ただし偏差は有界 (ノイズシェイパの理論上限 ~12 LSB、安全側で 16 LSB)
        float maxDev = 0.0f;
        for (int i = from; i < to; ++i)
            maxDev = juce::jmax(maxDev, std::abs(on.samples.getSample(0, i) - 0.30001f));
        expect(maxDev <= 16.0f * kLsb16, "dither deviation bounded, maxDev(LSB)="
               + juce::String(maxDev / kLsb16, 2));
    }

    //==========================================================================
    // 22. 無音へのディザは有界 (グリッド量子化 + 数 LSB)。ディザ OFF なら完全な 0。
    void testDitherSilenceBounded()
    {
        beginTest("dithered silence stays grid-quantized and bounded");

        auto renderSilence = [&](bool dither, int bits) -> AudioFileData
        {
            juce::AudioFormatManager fmt; fmt.registerBasicFormats();
            TrackManager tm(fmt);
            auto src = outDir.getChildFile("sil_src.wav");
            writeMonoConst(src, (int)(0.3 * kSR), 0.0f);
            auto* t = tm.addTrack("s", false);
            auto* c = t->addClip(src, 0.0, 0.3);
            c->setFadeInSecs(0.0); c->setFadeOutSecs(0.0);
            t->setVolume(0.0f); t->setPan(0.0f);

            AudioEngine engine; engine.preparePlayback(tm);
            auto out = outDir.getChildFile("sil_" + juce::String(bits)
                                           + (dither ? "_on" : "_off") + ".wav");
            auto o = baseOpts(out, bits, 0.3); o.dither = dither;
            juce::String err;
            ExportEngine::render(engine, o, {}, {}, &err);
            return readAudio(out);
        };

        // 16bit ディザ有り無音: 各サンプルは LSB の整数倍、かつ |x| <= 16 LSB
        auto on = renderSilence(true, 16);
        expect(on.ok, "readable");
        float maxAbs = 0.0f, maxGridErr = 0.0f;
        const float* p = on.samples.getReadPointer(0);
        for (int i = 0; i < on.samples.getNumSamples(); ++i)
        {
            maxAbs = juce::jmax(maxAbs, std::abs(p[i]));
            float grid = std::lround(p[i] / kLsb16) * kLsb16;
            maxGridErr = juce::jmax(maxGridErr, std::abs(p[i] - grid));
        }
        expect(maxGridErr <= 1.0e-6f, "samples are integer multiples of LSB");
        expect(maxAbs <= 16.0f * kLsb16, "silence dither bounded, maxAbs(LSB)="
               + juce::String(maxAbs / kLsb16, 2));

        // 16bit ディザ無し無音: 完全に 0
        auto off = renderSilence(false, 16);
        expectWithinAbsoluteError(off.maxAbs(), 0.0f, 1.0e-9f);

        // 32f は無音そのまま (ディザ非適用) → 完全に 0
        auto f32 = renderSilence(true, 32);
        expectWithinAbsoluteError(f32.maxAbs(), 0.0f, 1.0e-9f);
    }

    //==========================================================================
    // 23. 既存の出力ファイルは上書き (削除→新規書き込み) される
    void testOverwritesExistingFile()
    {
        beginTest("existing output file is overwritten");

        juce::AudioFormatManager fmt; fmt.registerBasicFormats();
        TrackManager tm(fmt);
        auto src = outDir.getChildFile("ow_src.wav");
        writeMonoConst(src, (int)(0.2 * kSR), 0.5f);
        auto* t = tm.addTrack("o", false);
        auto* c = t->addClip(src, 0.0, 0.2);
        c->setFadeInSecs(0.0); c->setFadeOutSecs(0.0);
        t->setVolume(0.0f); t->setPan(0.0f);

        AudioEngine engine; engine.preparePlayback(tm);

        // 事前に別内容 (異なる尺) の WAV を同じパスに作っておく
        auto out = outDir.getChildFile("overwrite.wav");
        writeMonoConst(out, (int)(5.0 * kSR), 0.9f);  // 240000 サンプルの別ファイル
        const auto oldLen = readAudio(out).len;
        expectEquals(oldLen, (juce::int64) 240000);

        auto o = baseOpts(out, 32, 0.2);
        juce::String err;
        expect(ExportEngine::render(engine, o, {}, {}, &err), "render: " + err);

        auto d = readAudio(out);
        expect(d.ok, "still a valid WAV after overwrite");
        // 新しい尺は (endSec-startSec)*SR で決まり、古いファイルとは無関係
        expectEquals(d.len, (juce::int64) 9600);     // round(0.2 * 48000)
        expect(d.len != oldLen, "length replaced, not appended");
        expectAllClose(d.samples, 0, 0.5f, 1.0e-6f, "content is freshly rendered (0.5)");
    }
};

static ExportEngineTests exportEngineTests;

//==============================================================================
namespace
{
struct ConsoleLogger : juce::Logger
{
    void logMessage(const juce::String& m) override { std::cout << m << std::endl; }
};
}

int main()
{
    juce::ScopedJuceInitialiser_GUI gui;   // MessageManager / SharedResourcePointer を起動

    ConsoleLogger logger;
    juce::Logger::setCurrentLogger(&logger);

    juce::UnitTestRunner runner;
    runner.setAssertOnFailure(false);
    runner.runAllTests();

    int passes = 0, failures = 0;
    for (int i = 0; i < runner.getNumResults(); ++i)
    {
        const auto* r = runner.getResult(i);
        passes   += r->passes;
        failures += r->failures;
    }

    std::cout << "\n==== UtawaveTests: " << passes << " checks passed, "
              << failures << " failed ====" << std::endl;

    juce::Logger::setCurrentLogger(nullptr);
    return failures == 0 ? 0 : 1;
}
