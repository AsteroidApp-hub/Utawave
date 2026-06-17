// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

// Utawave — AudioFileImporter のユニットテスト (メタデータ除去 / 自動リサンプル)
//
// 他 DAW が埋め込んだテンポ・ループ・各種メタデータがプロジェクトに流入するのを防ぐ中核保証。
//   copyStrippingMetadata:
//     ・bext (bwav*) を除去 / iXML・ASWG (tempo/timeSig/inKey/IXML_VERSION 等) を除去 /
//       smpl ループ (Loop*) を除去 / 無害な LIST-INFO (IART/ICMT 等) は残す
//     ・サンプルはビット完全保持 / ビット深度・ch・SR・float-int を保持
//     ・非 WAV / 欠損ファイルはエラー
//   importFile:
//     ・SR 一致は元ファイルへ短絡 (コピーなし) / 44.1k→48k は wasResampled=true・尺/SR/ch を保つ
//     ・欠損ファイルは success=false
//
// 注意: iXML/ASWG のメタデータキーは JUCE では "aswg" プレフィックスではなく実タグ名
// ("tempo" 等) で入る (juce_WavAudioFormat.cpp の IXMLChunk)。本テストはその実キーが
// 確実に除去されることを契約として固定する。
// AudioFormatManager は runTest ローカル。expect は ASCII。

#include <JuceHeader.h>
#include <cmath>
#include <cstring>
#include "../Source/Audio/AudioFileImporter.h"

namespace
{
class AudioFileImporterTests : public juce::UnitTest
{
public:
    AudioFileImporterTests() : juce::UnitTest("AudioFileImporter") {}

    juce::File dir;

    // 既知サンプル + メタデータの整数 PCM WAV を書く (bits=16/24)。
    juce::File writeWavInt(const juce::String& name, double sr, int ch, int bits,
                           double secs, const juce::StringPairArray& meta)
    {
        auto f = dir.getChildFile(name);
        f.deleteFile();
        auto* os = f.createOutputStream().release();
        if (os == nullptr) return {};
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::AudioFormatWriter> w(
            wav.createWriterFor(os, sr, (unsigned int) ch, bits, meta, 0));
        if (w == nullptr) { delete os; return {}; }
        const int n = (int) (sr * secs);
        juce::AudioBuffer<float> buf(ch, n);
        for (int c = 0; c < ch; ++c)
            for (int i = 0; i < n; ++i)
                buf.setSample(c, i,
                    (float) (0.3 * std::sin(2.0 * juce::MathConstants<double>::pi * 440.0 * i / sr)));
        w->writeFromAudioSampleBuffer(buf, 0, n);
        return f;
    }

    // 64bit WAV (Float64 / 64bit PCM) を手書きで書く。JUCE の WavAudioFormat は 64bit を
    // 書けないため (writer も 8/16/24/32 のみ)、RIFF を直接組む。サンプルは LE で並べる
    // (juce::OutputStream の writeInt/Short/Int64 は LE)。
    juce::File writeWav64(const juce::String& name, double sr, int ch, double secs, bool asInt64)
    {
        auto f = dir.getChildFile(name);
        f.deleteFile();
        juce::FileOutputStream os(f);
        if (!os.openedOk()) return {};

        const int bits        = 64;
        const int blockAlign  = ch * bits / 8;
        const int byteRate    = (int) (sr * blockAlign);
        const int n           = (int) (sr * secs);
        const int dataBytes   = n * blockAlign;

        auto w4 = [&os](const char* s) { os.write(s, 4); };
        w4("RIFF"); os.writeInt(36 + dataBytes); w4("WAVE");
        w4("fmt "); os.writeInt(16);
        os.writeShort((short) (asInt64 ? 1 : 3));   // 1=PCM / 3=IEEE float
        os.writeShort((short) ch);
        os.writeInt((int) sr);
        os.writeInt(byteRate);
        os.writeShort((short) blockAlign);
        os.writeShort((short) bits);
        w4("data"); os.writeInt(dataBytes);

        for (int i = 0; i < n; ++i)
            for (int c = 0; c < ch; ++c)
            {
                const double s = 0.3 * std::sin(2.0 * juce::MathConstants<double>::pi * 440.0 * i / sr)
                                     * (c == 0 ? 1.0 : 0.5);   // R は半分にして ch 独立性も確認
                if (asInt64)
                    os.writeInt64((juce::int64) std::llround(s * 9223372036854775807.0));
                else
                {
                    juce::uint64 bitsv;
                    std::memcpy(&bitsv, &s, sizeof(bitsv));   // double のビットパターンを LE で書く
                    os.writeInt64((juce::int64) bitsv);
                }
            }
        os.flush();
        return f;
    }

    struct Loaded
    {
        bool ok { false };
        juce::StringPairArray meta;
        int bits { 0 }, channels { 0 };
        double sampleRate { 0.0 };
        bool isFloat { false };
        juce::int64 length { 0 };
        juce::AudioBuffer<float> samples;
    };

    Loaded readBack(juce::AudioFormatManager& fmt, const juce::File& f)
    {
        Loaded L;
        std::unique_ptr<juce::AudioFormatReader> r(fmt.createReaderFor(f));
        if (!r) return L;
        L.ok = true;
        L.meta = r->metadataValues;
        L.bits = (int) r->bitsPerSample;
        L.channels = (int) r->numChannels;
        L.sampleRate = r->sampleRate;
        L.isFloat = r->usesFloatingPointData;
        L.length = r->lengthInSamples;
        L.samples.setSize((int) r->numChannels, (int) r->lengthInSamples);
        r->read(&L.samples, 0, (int) r->lengthInSamples, 0, true, true);
        return L;
    }

    void runTest() override
    {
        dir = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("UtawaveImporterTests");
        dir.deleteRecursively(); dir.createDirectory();
        juce::AudioFormatManager fmt; fmt.registerBasicFormats();

        testStripRemovesRiskyKeepsHarmless(fmt);
        testStripPreservesSamplesAndFormat(fmt);
        testStripGuards(fmt);
        testImportSampleRateMatchShortCircuits(fmt);
        testImportResamples(fmt);
        testImportProgressAndCancel(fmt);
        testImportMissingFile(fmt);
        testImport64BitWav(fmt);

        dir.deleteRecursively();
    }

    // ── bext + iXML/ASWG + smpl ループを除去し、無害な LIST-INFO は残す ──
    void testStripRemovesRiskyKeepsHarmless(juce::AudioFormatManager& fmt)
    {
        beginTest("copyStrippingMetadata removes bext/iXML/smpl metadata, keeps LIST-INFO");
        juce::StringPairArray meta;
        meta.set("bwav description", "Recorded in another DAW");   // bext
        meta.set("bwav originator",  "OtherDAW");                  // bext
        meta.set("tempo",   "128");                                // iXML / ASWG (テンポ流入の本命)
        meta.set("timeSig", "4/4");                                // iXML / ASWG
        meta.set("inKey",   "Am");                                 // iXML / ASWG
        meta.set("NumSampleLoops", "1");                           // smpl
        meta.set("Loop0Start", "1000");                            // smpl ループ点
        meta.set("Loop0End",   "2000");                            // smpl ループ点
        meta.set("IART", "TestArtist");                            // LIST-INFO (無害・残す)
        meta.set("ICMT", "a friendly comment");                    // LIST-INFO (無害・残す)

        auto src = writeWavInt("meta_src.wav", 48000.0, 1, 24, 1.0, meta);
        expect(src.existsAsFile(), "source written");

        // セットアップ健全性: ソースが実際に risky キーを保持していること (でないと偽の合格になる)
        auto before = readBack(fmt, src);
        expect(before.ok, "source readable");
        expect(before.meta.containsKey("bwav description"), "source carries bext");
        expect(before.meta.containsKey("tempo"), "source carries iXML/ASWG tempo");
        expect(before.meta.containsKey("IART"), "source carries LIST-INFO artist");

        AudioFileImporter importer(fmt);
        juce::String err;
        auto dst = dir.getChildFile("meta_dst.wav");
        const bool okStrip = importer.copyStrippingMetadata(src, dst, err);
        expect(okStrip, ("copyStrippingMetadata succeeds: " + err).toRawUTF8());

        auto after = readBack(fmt, dst);
        expect(after.ok, "stripped file readable");

        // risky メタデータは全て消えていること
        bool anyBext = false, anyLoop = false;
        for (const auto& k : after.meta.getAllKeys())
        {
            if (k.startsWithIgnoreCase("bwav")) anyBext = true;
            if (k.startsWithIgnoreCase("loop")) anyLoop = true;
        }
        expect(!anyBext, "no bext (bwav*) keys remain");
        expect(!anyLoop, "no smpl loop (Loop*) keys remain");
        expect(!after.meta.containsKey("tempo"),        "iXML/ASWG tempo removed");
        expect(!after.meta.containsKey("timeSig"),      "iXML/ASWG timeSig removed");
        expect(!after.meta.containsKey("inKey"),        "iXML/ASWG inKey removed");
        expect(!after.meta.containsKey("IXML_VERSION"), "iXML version sentinel removed");

        // 無害な LIST-INFO は残す
        expect(after.meta.getValue("IART", "") == "TestArtist", "LIST-INFO artist preserved");
        expect(after.meta.getValue("ICMT", "") == "a friendly comment", "LIST-INFO comment preserved");
    }

    // ── サンプルはビット完全保持・フォーマット (bits/ch/SR/float-int) を保持 ──
    void testStripPreservesSamplesAndFormat(juce::AudioFormatManager& fmt)
    {
        beginTest("copyStrippingMetadata preserves samples bit-for-bit and the format");
        juce::StringPairArray meta;
        meta.set("bwav description", "strip me");
        // 24bit モノ
        auto src = writeWavInt("fmt24_src.wav", 44100.0, 1, 24, 0.5, meta);
        AudioFileImporter importer(fmt);
        juce::String err;
        auto dst = dir.getChildFile("fmt24_dst.wav");
        expect(importer.copyStrippingMetadata(src, dst, err), "strip ok (24bit mono)");

        auto a = readBack(fmt, src);
        auto b = readBack(fmt, dst);
        expect(a.ok && b.ok, "both readable");
        expect(b.bits == 24, "bit depth preserved (24)");
        expect(b.channels == 1, "channel count preserved (mono)");
        expect(std::abs(b.sampleRate - 44100.0) < 0.01, "sample rate preserved (44100)");
        expect(b.isFloat == a.isFloat, "float/int flag preserved");
        expect(b.length == a.length, "sample count preserved");

        // サンプル一致 (同一 24bit エンコードなので完全一致)
        bool identical = (a.length == b.length && a.channels == b.channels);
        if (identical)
            for (int c = 0; c < a.channels && identical; ++c)
                for (int i = 0; i < (int) a.length; ++i)
                    if (a.samples.getSample(c, i) != b.samples.getSample(c, i)) { identical = false; break; }
        expect(identical, "samples are bit-identical after stripping");

        // ステレオ 16bit でも format 保持
        juce::StringPairArray meta2;
        meta2.set("tempo", "90");
        auto srcS = writeWavInt("fmt16_src.wav", 48000.0, 2, 16, 0.3, meta2);
        auto dstS = dir.getChildFile("fmt16_dst.wav");
        expect(importer.copyStrippingMetadata(srcS, dstS, err), "strip ok (16bit stereo)");
        auto bs = readBack(fmt, dstS);
        expect(bs.ok && bs.bits == 16 && bs.channels == 2,
               "16bit stereo format preserved");
    }

    // ── 非 WAV / 欠損はエラーで false ──
    void testStripGuards(juce::AudioFormatManager& fmt)
    {
        beginTest("copyStrippingMetadata: non-WAV and missing source return false");
        AudioFileImporter importer(fmt);
        juce::String err;

        auto notWav = dir.getChildFile("x.aiff");
        notWav.replaceWithText("not really aiff");   // existsAsFile, but extension != wav
        expect(!importer.copyStrippingMetadata(notWav, dir.getChildFile("o1.wav"), err),
               "non-WAV source returns false");
        expect(err.isNotEmpty(), "error message set for non-WAV");

        expect(!importer.copyStrippingMetadata(dir.getChildFile("nope.wav"),
                                               dir.getChildFile("o2.wav"), err),
               "missing source returns false");
    }

    // ── importFile: SR 一致なら元ファイルへ短絡 (コピーしない) ──
    void testImportSampleRateMatchShortCircuits(juce::AudioFormatManager& fmt)
    {
        beginTest("importFile: matching sample rate short-circuits to the source file");
        juce::StringPairArray meta;
        auto src = writeWavInt("sr48.wav", 48000.0, 1, 24, 0.5, meta);
        AudioFileImporter importer(fmt);
        auto r = importer.importFile(src, 48000.0);
        expect(r.success, "import succeeds");
        expect(!r.wasResampled, "no resample when SR matches");
        expect(r.file == src, "returns the source file unchanged");
        expect(std::abs(r.sampleRate - 48000.0) < 0.01, "reports source sample rate");
        expect(r.numChannels == 1, "reports channel count");
    }

    // ── importFile: 44.1k→48k リサンプルでキャッシュ生成・尺/SR/ch 保持 ──
    void testImportResamples(juce::AudioFormatManager& fmt)
    {
        beginTest("importFile: 44.1k -> 48k resamples into cache, preserves duration/SR/channels");
        auto cache = dir.getChildFile("cache");
        cache.deleteRecursively(); cache.createDirectory();

        juce::StringPairArray meta;
        auto src = writeWavInt("sr44.wav", 44100.0, 1, 24, 1.0, meta);
        AudioFileImporter importer(fmt);
        importer.getCacheFolderCb = [cache] { return cache; };

        auto r = importer.importFile(src, 48000.0);
        expect(r.success, "resample import succeeds");
        expect(r.wasResampled, "wasResampled is true");
        expect(r.file != src, "returns a new cache file, not the source");
        expect(r.file.getParentDirectory() == cache, "cache file lives in the cache folder");
        expect(std::abs(r.sampleRate - 48000.0) < 0.01, "reports project sample rate (48000)");
        expect(r.numChannels == 1, "channel count preserved");
        expect(std::abs(r.durationSec - 1.0) < 0.02, "duration in seconds preserved (~1.0s)");

        // 生成ファイルが実際に 48k で約 1 秒であること
        auto out = readBack(fmt, r.file);
        expect(out.ok, "cache file readable");
        expect(std::abs(out.sampleRate - 48000.0) < 0.01, "cache file is 48000 Hz");
        expect(std::abs((double) out.length - 48000.0) < 600.0,
               "cache file is ~48000 samples (~1.0s, allowing resampler latency)");
    }

    // ── importFile: onProgress がリサンプル中に報告される / false で中断 ──
    void testImportProgressAndCancel(juce::AudioFormatManager& fmt)
    {
        beginTest("importFile: onProgress is reported during resample and returning false cancels");
        auto cache = dir.getChildFile("cache_prog");
        cache.deleteRecursively(); cache.createDirectory();
        juce::StringPairArray meta;
        auto src = writeWavInt("prog44.wav", 44100.0, 1, 24, 1.0, meta);
        AudioFileImporter importer(fmt);
        importer.getCacheFolderCb = [cache] { return cache; };

        // 進捗が報告され、[0,1] 内で非減少
        std::vector<double> seen;
        auto r = importer.importFile(src, 48000.0, 32,
                                     [&seen](double p) { seen.push_back(p); return true; });
        expect(r.success && r.wasResampled, "resample succeeds with a progress callback");
        expect(!seen.empty(), "onProgress was called at least once during resample");
        bool inRange = true, monotonic = true;
        for (size_t i = 0; i < seen.size(); ++i)
        {
            if (seen[i] < 0.0 || seen[i] > 1.0) inRange = false;
            if (i > 0 && seen[i] < seen[i - 1] - 1.0e-9) monotonic = false;
        }
        expect(inRange, "progress values stay within [0,1]");
        expect(monotonic, "progress is non-decreasing");

        // false を返すと中断 (success=false / cancelled=true / 出力ファイル無し)
        auto before = cache.getNumberOfChildFiles(juce::File::findFiles);
        auto r2 = importer.importFile(src, 48000.0, 32, [](double) { return false; });
        expect(!r2.success && r2.cancelled, "returning false cancels (success=false, cancelled=true)");
        expect(cache.getNumberOfChildFiles(juce::File::findFiles) == before,
               "cancelled import leaves no new cache file");
    }

    // ── 64bit WAV (Float64 / 64bit PCM) を 32bit float へ変換して取り込む ──
    // JUCE の WavAudioFormat は 64bit を読めない (createReaderFor が bitsPerSample>32 で nullptr)。
    // peek / transcode の純関数と、importFile の統合 (SR一致変換 + リサンプル) を検証する。
    void testImport64BitWav(juce::AudioFormatManager& fmt)
    {
        beginTest("64-bit WAV: peek + transcode to 32f, and importFile integration");

        auto src64 = writeWav64("f64.wav", 48000.0, 2, 0.5, /*asInt64*/ false);
        expect(src64.existsAsFile(), "64-bit float source written");

        // peekWavFormat: ヘッダを正しく読む
        auto info = AudioFileImporter::peekWavFormat(src64);
        expect(info.ok, "peek parses 64-bit WAV header");
        expect(info.bits == 64, "peek reports 64 bits");
        expect(info.isFloat, "peek reports float");
        expect(info.channels == 2, "peek reports stereo");
        expect(std::abs(info.sampleRate - 48000.0) < 0.01, "peek reports 48000 Hz");

        // needsHighBitTranscode: 64bit は true / 通常の 24bit は false
        expect(AudioFileImporter::needsHighBitTranscode(src64), "64-bit needs transcode");
        auto src24 = writeWavInt("not64.wav", 48000.0, 1, 24, 0.2, {});
        expect(!AudioFileImporter::needsHighBitTranscode(src24), "24-bit does not need transcode");

        // transcodeHighBitWavToFloat: 32f になり、サンプルが一致する
        auto conv = dir.getChildFile("f64_to_f32.wav");
        juce::String terr;
        expect(AudioFileImporter::transcodeHighBitWavToFloat(src64, conv, terr),
               ("transcode succeeds: " + terr).toRawUTF8());
        auto out = readBack(fmt, conv);
        expect(out.ok, "transcoded file readable");
        expect(out.bits == 32 && out.isFloat, "transcoded file is 32-bit float");
        expect(out.channels == 2, "channels preserved");
        expect(std::abs(out.sampleRate - 48000.0) < 0.01, "sample rate preserved");
        const int expN = (int) (48000.0 * 0.5);
        expect(std::abs((double) out.length - expN) < 2.0, "sample count preserved");

        bool samplesMatch = (out.length >= expN);
        for (int i = 0; i < expN && samplesMatch; ++i)
        {
            const double ref = 0.3 * std::sin(2.0 * juce::MathConstants<double>::pi * 440.0 * i / 48000.0);
            if (std::abs(out.samples.getSample(0, i) - (float) ref) > 1.0e-4) samplesMatch = false;
            if (std::abs(out.samples.getSample(1, i) - (float) (ref * 0.5)) > 1.0e-4) samplesMatch = false;
        }
        expect(samplesMatch, "transcoded samples match the 64-bit source (both channels)");

        // 64bit 符号付き PCM も変換できる
        auto srcI64 = writeWav64("i64.wav", 48000.0, 1, 0.2, /*asInt64*/ true);
        auto convI  = dir.getChildFile("i64_to_f32.wav");
        expect(AudioFileImporter::transcodeHighBitWavToFloat(srcI64, convI, terr),
               ("64-bit PCM transcode succeeds: " + terr).toRawUTF8());
        auto outI = readBack(fmt, convI);
        expect(outI.ok && outI.bits == 32 && outI.isFloat, "64-bit PCM -> 32f readable");
        expect(outI.length > 0 && outI.samples.getMagnitude(0, 0, (int) outI.length) > 0.05,
               "64-bit PCM transcode is non-silent");

        // importFile 統合 (SR 一致): 変換物を返し wasResampled=true (Audio/ へ移して消す扱い)
        auto cache = dir.getChildFile("cache64");
        cache.deleteRecursively(); cache.createDirectory();
        AudioFileImporter importer(fmt);
        importer.getCacheFolderCb = [cache] { return cache; };

        auto r = importer.importFile(src64, 48000.0);
        expect(r.success, "importFile succeeds for 64-bit at matching SR");
        expect(r.wasResampled, "64-bit transcode is treated like a cache file (wasResampled=true)");
        expect(r.file != src64, "returns the converted file, not the 64-bit source");
        expect(r.numChannels == 2, "importFile reports channel count");
        auto ir = readBack(fmt, r.file);
        expect(ir.ok && ir.bits == 32 && ir.isFloat, "imported 64-bit file is now 32f and readable");

        // importFile 統合 (リサンプル): 48k 64bit -> 44.1k で変換 + リサンプルが連結して動く
        auto r2 = importer.importFile(src64, 44100.0);
        expect(r2.success && r2.wasResampled, "64-bit + resample succeeds");
        auto ir2 = readBack(fmt, r2.file);
        expect(ir2.ok && std::abs(ir2.sampleRate - 44100.0) < 0.01,
               "64-bit resampled output is 44100 Hz");
    }

    // ── importFile: 欠損ファイルは success=false ──
    void testImportMissingFile(juce::AudioFormatManager& fmt)
    {
        beginTest("importFile: missing file returns success=false with an error");
        AudioFileImporter importer(fmt);
        auto r = importer.importFile(dir.getChildFile("ghost.wav"), 48000.0);
        expect(!r.success, "missing file -> success false");
        expect(r.errorMessage.isNotEmpty(), "error message set");
    }
};

static AudioFileImporterTests audioFileImporterTests;
}
