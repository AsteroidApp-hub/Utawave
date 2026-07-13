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
//   圧縮フォーマットのデコード変換 (needsDecodeTranscode / transcodeToWavFloat):
//     ・MP3/FLAC 等は SR 一致でも 32bit float WAV へ変換して返す (wasResampled=true)。
//       OS デコーダのシークが sample-accurate でなく再生ズレになるため (「オケが滑る」報告)
//     ・FLAC はロスレスなのでサンプル一致 / MP3 は周波数・振幅・尺で検証 (lossy)
//     ・デコード中のキャンセル / デコード→リサンプル連結の進捗単調性
//
// 注意: iXML/ASWG のメタデータキーは JUCE では "aswg" プレフィックスではなく実タグ名
// ("tempo" 等) で入る (juce_WavAudioFormat.cpp の IXMLChunk)。本テストはその実キーが
// 確実に除去されることを契約として固定する。
// AudioFormatManager は runTest ローカル。expect は ASCII。

#include <JuceHeader.h>
#include <cmath>
#include <cstring>
#include "../Source/Audio/AudioFileImporter.h"
#include "../Source/Export/Mp3EncoderWriter.h"

namespace
{
// 常に read 失敗を返す AudioFormatReader (破損 / 途中でエラーを返す OS デコーダの模擬)。
// createReaderFor は成功し lengthInSamples>0 を報告するが read が false = 1 サンプルも
// デコードできない状況を決定論的に作る (transcodeToWavFloat の pos<=0 ガードの回帰テスト。
// 実ファイルの破損はコーデック依存で挙動が不安定なため、専用フォーマットで再現する)。
struct FailingAudioReader : public juce::AudioFormatReader
{
    FailingAudioReader (juce::InputStream* in) : juce::AudioFormatReader (in, "Failing")
    {
        sampleRate            = 48000.0;
        bitsPerSample         = 16;
        lengthInSamples       = 48000;   // 1 秒あると偽って報告する (>0 でガードを通す)
        numChannels           = 1;
        usesFloatingPointData = false;
    }
    bool readSamples (int* const*, int, int, juce::int64, int) override { return false; }
};

struct FailingAudioFormat : public juce::AudioFormat
{
    FailingAudioFormat() : juce::AudioFormat ("Failing", ".fail") {}
    juce::Array<int> getPossibleSampleRates() override { return {}; }
    juce::Array<int> getPossibleBitDepths()  override { return {}; }
    bool canDoStereo() override { return false; }
    bool canDoMono()   override { return true; }
    juce::AudioFormatReader* createReaderFor (juce::InputStream* sourceStream,
                                              bool /*deleteStreamIfOpeningFails*/) override
    {
        return new FailingAudioReader (sourceStream);   // reader が stream を所有
    }
    std::unique_ptr<juce::AudioFormatWriter> createWriterFor (
        std::unique_ptr<juce::OutputStream>&, const juce::AudioFormatWriterOptions&) override
    { return {}; }
};

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

    // 440Hz 正弦波の MP3 を内蔵 LAME で書く (モノ・振幅 0.5)。OS 非依存でテストソースを作れる。
    juce::File writeMp3(const juce::String& name, double sr, double secs)
    {
        auto f = dir.getChildFile(name);
        f.deleteFile();
        const int n = (int) (sr * secs);
        juce::AudioBuffer<float> buf(1, n);
        for (int i = 0; i < n; ++i)
            buf.setSample(0, i,
                (float) (0.5 * std::sin(2.0 * juce::MathConstants<double>::pi * 440.0 * i / sr)));
        juce::String err;
        if (! Mp3EncoderWriter::encodeBuffer(buf, sr, 192, f, &err)) return {};
        return f;
    }

    // 440Hz 正弦波の FLAC を書く (JUCE 内蔵 writer・24bit・ロスレスなのでサンプル検証に使う)。
    juce::File writeFlac(const juce::String& name, double sr, int ch, double secs)
    {
        auto f = dir.getChildFile(name);
        f.deleteFile();
        auto* os = f.createOutputStream().release();
        if (os == nullptr) return {};
        juce::FlacAudioFormat flac;
        std::unique_ptr<juce::AudioFormatWriter> w(
            flac.createWriterFor(os, sr, (unsigned int) ch, 24, {}, 0));
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

    // 正方向ゼロ交差から支配的周波数を推定 (窓の開始位相に依らない)
    static double estimateFreq(const juce::AudioBuffer<float>& s, int start, int count, double sr)
    {
        int crossings = 0;
        for (int i = start + 1; i < start + count; ++i)
            if (s.getSample(0, i - 1) <= 0.0f && s.getSample(0, i) > 0.0f) ++crossings;
        return (double) crossings / ((double) count / sr);
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
        testDecodeTranscodeMp3(fmt);
        testDecodeTranscodeFlacAndResampleChain(fmt);
        testDecodeTranscodeErrorPaths(fmt);

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

    // ── 圧縮フォーマット (MP3): SR 一致でも 32f WAV へデコード変換して取り込む ──
    // 圧縮ファイルを元のまま置くと OS デコーダのシークが sample-accurate でなく再生ズレになる
    // (「オケが再生のたびに滑る」報告の回帰テスト)。MP3 は lossy なので内容は周波数/振幅/尺で検証。
    void testDecodeTranscodeMp3(juce::AudioFormatManager& fmt)
    {
        beginTest("compressed import: MP3 at matching SR is decoded to a 32f WAV cache file");

        // needsDecodeTranscode の判定 (拡張子ベースの除外方式)
        expect(AudioFileImporter::needsDecodeTranscode(juce::File("/tmp/a.mp3")),  "mp3 needs decode");
        expect(AudioFileImporter::needsDecodeTranscode(juce::File("/tmp/a.m4a")),  "m4a needs decode");
        expect(AudioFileImporter::needsDecodeTranscode(juce::File("/tmp/a.flac")), "flac needs decode");
        expect(!AudioFileImporter::needsDecodeTranscode(juce::File("/tmp/a.wav")),  "wav does not");
        expect(!AudioFileImporter::needsDecodeTranscode(juce::File("/tmp/a.aiff")), "aiff does not");
        expect(!AudioFileImporter::needsDecodeTranscode(juce::File("/tmp/a.aif")),  "aif does not");

        auto mp3 = writeMp3("sine44.mp3", 44100.0, 1.0);
        expect(mp3.existsAsFile(), "mp3 source written (built-in encoder)");

        auto cache = dir.getChildFile("cache_mp3");
        cache.deleteRecursively(); cache.createDirectory();
        AudioFileImporter importer(fmt);
        importer.getCacheFolderCb = [cache] { return cache; };

        // SR 一致でも元ファイルへ短絡せず、デコード変換した WAV を wasResampled=true で返す
        auto r = importer.importFile(mp3, 44100.0);
        expect(r.success, "mp3 import succeeds at matching SR");
        expect(r.wasResampled, "decoded mp3 is treated like a cache file (wasResampled=true)");
        expect(r.file != mp3, "returns the decoded file, not the mp3 source");
        expect(r.file.hasFileExtension("wav"), "decoded file is a .wav");
        expect(r.file.getParentDirectory() == cache, "decoded file lives in the cache folder");

        auto out = readBack(fmt, r.file);
        expect(out.ok, "decoded file readable");
        expect(out.bits == 32 && out.isFloat, "decoded file is 32-bit float");
        expect(out.channels == 1, "channel count preserved");
        expect(std::abs(out.sampleRate - 44100.0) < 0.01, "sample rate preserved (44100)");
        // MP3 はエンコーダ遅延/パディングで数百〜千サンプル前後する。尺はゆるく検証
        expect(std::abs((double) out.length - 44100.0) < 3000.0,
               "decoded length is ~1.0s (allowing codec delay/padding)");

        // 内容: 中央 0.5 秒の支配的周波数が ~440Hz・振幅が ~0.5 (lossy 許容)
        if (out.ok && out.length > 30000)
        {
            const int start = (int) (out.length / 4);
            const int count = juce::jmin((int) out.length / 2, 22050);
            const double freq = estimateFreq(out.samples, start, count, out.sampleRate);
            expect(std::abs(freq - 440.0) < 6.0, "decoded content is a ~440Hz tone");
            const float mag = out.samples.getMagnitude(0, start, count);
            expect(mag > 0.35f && mag < 0.65f, "decoded amplitude is ~0.5 (lossy tolerance)");
        }

        // デコード中のキャンセル: success=false / cancelled=true / 新規キャッシュファイル無し
        const auto before = cache.getNumberOfChildFiles(juce::File::findFiles);
        auto r2 = importer.importFile(mp3, 44100.0, 32, [](double) { return false; });
        expect(!r2.success && r2.cancelled, "cancelling during decode reports cancelled=true");
        expect(cache.getNumberOfChildFiles(juce::File::findFiles) == before,
               "cancelled decode leaves no new cache file");
    }

    // ── 圧縮フォーマット (FLAC = ロスレス) のサンプル一致 + MP3 のデコード→リサンプル連結 ──
    void testDecodeTranscodeFlacAndResampleChain(juce::AudioFormatManager& fmt)
    {
        beginTest("compressed import: FLAC decodes sample-accurately; decode chains into resample");

        auto cache = dir.getChildFile("cache_flac");
        cache.deleteRecursively(); cache.createDirectory();
        AudioFileImporter importer(fmt);
        importer.getCacheFolderCb = [cache] { return cache; };

        // FLAC はロスレスなので、デコード変換後のサンプルが元の正弦波と一致することを厳密に検証
        auto flac = writeFlac("sine44.flac", 44100.0, 1, 0.5);
        expect(flac.existsAsFile(), "flac source written");
        auto r = importer.importFile(flac, 44100.0);
        expect(r.success && r.wasResampled, "flac import decodes at matching SR");
        expect(r.file.hasFileExtension("wav"), "flac decoded to .wav");

        auto out = readBack(fmt, r.file);
        expect(out.ok && out.bits == 32 && out.isFloat, "flac decoded file is 32f");
        const int expN = (int) (44100.0 * 0.5);
        expect((int) out.length == expN, "flac decoded length is exact (lossless)");
        bool samplesMatch = ((int) out.length == expN);
        for (int i = 0; i < expN && samplesMatch; ++i)
        {
            const double ref = 0.3 * std::sin(2.0 * juce::MathConstants<double>::pi * 440.0 * i / 44100.0);
            if (std::abs(out.samples.getSample(0, i) - (float) ref) > 1.0e-4) samplesMatch = false;
        }
        expect(samplesMatch, "flac decoded samples match the source sine (24-bit tolerance)");

        // MP3 44.1k -> 48k プロジェクト: デコード変換とリサンプルが連結して動き、進捗も単調
        auto mp3 = writeMp3("sine44b.mp3", 44100.0, 1.0);
        std::vector<double> seen;
        auto r2 = importer.importFile(mp3, 48000.0, 32,
                                      [&seen](double p) { seen.push_back(p); return true; });
        expect(r2.success && r2.wasResampled, "mp3 decode + resample succeeds");
        auto out2 = readBack(fmt, r2.file);
        expect(out2.ok && std::abs(out2.sampleRate - 48000.0) < 0.01,
               "mp3 resampled output is 48000 Hz");
        expect(std::abs((double) out2.length - 48000.0) < 3300.0,
               "mp3 resampled output is ~1.0s");
        if (out2.ok && out2.length > 30000)
        {
            const double freq = estimateFreq(out2.samples, (int) (out2.length / 4),
                                             juce::jmin((int) out2.length / 2, 24000), out2.sampleRate);
            expect(std::abs(freq - 440.0) < 6.0, "mp3 resampled content is still a ~440Hz tone");
        }
        expect(!seen.empty(), "progress reported across decode + resample");
        bool inRange = true, monotonic = true;
        for (size_t i = 0; i < seen.size(); ++i)
        {
            if (seen[i] < 0.0 || seen[i] > 1.0) inRange = false;
            if (i > 0 && seen[i] < seen[i - 1] - 1.0e-9) monotonic = false;
        }
        expect(inRange && monotonic, "combined progress stays in [0,1] and is non-decreasing");
    }

    // ── 圧縮音源の異常系: 読めない / 空 / デコード 0 サンプルは失敗して後始末する ──
    // 破損音源が無音の空クリップとして無警告で取り込まれないことの回帰テスト
    // (今回の pos<=0 ガードを含む)。エラー時は dst / Cache にゴミを残さない。
    void testDecodeTranscodeErrorPaths(juce::AudioFormatManager& fmt)
    {
        beginTest("compressed import: unreadable / empty / undecodable sources fail cleanly with no leftover files");

        auto cache = dir.getChildFile("cache_err");
        cache.deleteRecursively(); cache.createDirectory();
        AudioFileImporter importer(fmt);
        importer.getCacheFolderCb = [cache] { return cache; };

        // (1) importFile: 中身が不正な .mp3 (デコーダが開けない) → success=false / cancelled=false /
        //     エラー文言あり / Cache にゴミを残さない (compressed 分岐の probe reader が null の経路)
        auto bogus = dir.getChildFile("bogus.mp3");
        bogus.replaceWithText("this is definitely not a valid mp3 stream");
        const auto before = cache.getNumberOfChildFiles(juce::File::findFiles);
        auto r = importer.importFile(bogus, 48000.0);
        expect(!r.success,               "unreadable mp3 import fails");
        expect(!r.cancelled,             "unreadable mp3 is a failure, not a cancel");
        expect(r.errorMessage.isNotEmpty(), "error message set for unreadable mp3");
        expect(cache.getNumberOfChildFiles(juce::File::findFiles) == before,
               "unreadable mp3 leaves no cache file");

        // (2) transcodeToWavFloat 直呼び: 読めないソース → false / cancelled は false へ上書き / dst 未生成
        auto dst1 = dir.getChildFile("err_unreadable.wav");
        juce::String terr; bool tcancel = true;   // 事前に true にして false 上書きを確認
        expect(!importer.transcodeToWavFloat(bogus, dst1, terr, tcancel, {}),
               "transcodeToWavFloat fails on an unreadable source");
        expect(!tcancel,             "unreadable source is not a cancel (cancelledOut=false)");
        expect(terr.isNotEmpty(),    "error set for unreadable source");
        expect(!dst1.existsAsFile(), "no dst written for unreadable source");

        // (3) transcodeToWavFloat 直呼び: 0 サンプルのソース → false ("Empty audio file" ガード) / dst 未生成
        auto empty = writeWavInt("empty0.wav", 48000.0, 1, 24, 0.0, {});
        expect(empty.existsAsFile(), "zero-length source written");
        auto dst2 = dir.getChildFile("err_empty.wav");
        juce::String terr2; bool tcancel2 = false;
        expect(!importer.transcodeToWavFloat(empty, dst2, terr2, tcancel2, {}),
               "transcodeToWavFloat fails on a zero-length source");
        expect(terr2.isNotEmpty(),    "error set for zero-length source");
        expect(!dst2.existsAsFile(),  "no dst written for zero-length source");

        // (4) pos<=0 ガード (今回の修正の回帰テスト): reader は開けて length>0 を報告するが read が
        //     常に失敗する (破損 / 途中エラーの OS デコーダ模擬) → 空 WAV を成功として返さず false /
        //     dst 未生成。決定論的に叩くため専用の failing フォーマットを登録した manager を使う。
        juce::AudioFormatManager failFmt;
        failFmt.registerBasicFormats();
        failFmt.registerFormat(new FailingAudioFormat(), false);
        AudioFileImporter failImporter(failFmt);
        auto failSrc = dir.getChildFile("undecodable.fail");
        failSrc.replaceWithText("header-ok-but-frames-unreadable");
        // 前提: reader が開けて length>0 を報告する (でないと別ガードで弾かれ pos<=0 を通らない)
        {
            std::unique_ptr<juce::AudioFormatReader> chk(failFmt.createReaderFor(failSrc));
            expect(chk != nullptr && chk->lengthInSamples > 0,
                   "failing reader opens and reports a positive length (precondition)");
        }
        auto dst3 = dir.getChildFile("err_undecodable.wav");
        juce::String terr3; bool tcancel3 = false;
        expect(!failImporter.transcodeToWavFloat(failSrc, dst3, terr3, tcancel3, {}),
               "transcodeToWavFloat fails when the decoder yields zero samples");
        expect(!tcancel3,             "zero-sample decode is a failure, not a cancel");
        expect(terr3.isNotEmpty(),    "error set when zero samples decoded");
        expect(!dst3.existsAsFile(),  "no empty WAV left when zero samples decoded (pos<=0 guard)");
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
