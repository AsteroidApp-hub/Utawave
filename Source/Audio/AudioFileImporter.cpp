// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

#include "AudioFileImporter.h"
#include "CDSPResampler.h"  // r8brain (header-only)
#include <cstring>          // std::memcpy (64bit サンプルのビット解釈)

juce::File AudioFileImporter::getDefaultCacheFolder()
{
    auto f = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
                .getChildFile("Utawave").getChildFile("Cache");
    f.createDirectory();
    return f;
}

juce::File AudioFileImporter::getCacheFolder() const
{
    if (getCacheFolderCb)
    {
        auto f = getCacheFolderCb();
        f.createDirectory();
        return f;
    }
    return getDefaultCacheFolder();
}

AudioFileImporter::Result
AudioFileImporter::importFile(const juce::File& src, double projectSampleRate, int outputBits,
                              std::function<bool(double)> onProgress)
{
    // outputBits は 24 (PCM + TPDF ディザ) または 32 (float) のみサポート。
    // それ以外の値が来た場合は無音で 32 にフォールバックさせず、明示的に DBG ログを残す
    // (16/64 等の指定は呼び出し側の設定ミスである可能性が高い)。
    if (outputBits != 24 && outputBits != 32)
    {
        DBG("AudioFileImporter: unsupported outputBits=" << outputBits
             << ", falling back to 32 (float)");
        jassertfalse;  // デバッグビルドで気付けるよう assert
        outputBits = 32;
    }
    Result r;
    if (!src.existsAsFile()) { r.errorMessage = "File not found"; return r; }

    // 64bit (>32bit) WAV は JUCE がデコードできないため、まず 32bit float の一時 WAV へ変換し、
    // 以降はその変換済みファイルを処理対象 (working) とする。変換済み一時ファイルは処理後に削除する
    // (SR一致時は呼び出し側が Audio/ へ移してから削除する = リサンプルキャッシュと同じ扱い)。
    juce::File working = src;
    bool didTranscode = false;
    std::function<bool(double)> resampleProgress = onProgress;
    if (needsHighBitTranscode(src))
    {
        auto cache = getCacheFolder();
        // クリップ名がファイル名由来になるため、変換物も元の名前を保つ (Audio/ へは元名でコピーされる)。
        working = cache.getChildFile(juce::File::createLegalFileName(src.getFileName()))
                       .getNonexistentSibling();
        juce::String terr;
        if (!transcodeHighBitWavToFloat(src, working, terr))
        {
            working.deleteFile();
            r.errorMessage = terr.isNotEmpty() ? terr : juce::String("Unsupported 64-bit WAV");
            return r;
        }
        didTranscode = true;
    }
    // 圧縮フォーマット (MP3/M4A 等) は SR 一致でも 32bit float WAV へデコード変換する。
    // OS デコーダのシークが sample-accurate でないため、圧縮のまま置くと再生開始位置ごとに
    // 数十 ms 単位でズレて聞こえる (詳細はヘッダのコメント)。64bit 変換と同じく変換物を
    // working とし、SR 一致でも wasResampled=true (Audio/ へ移して一時ファイルを消す扱い)。
    else if (needsDecodeTranscode(src))
    {
        // 進捗の割り当てを先に決めるため SR だけ覗く (デコーダ生成はヘッダ解析のみで軽い)。
        // リサンプルが続くならデコード 0..0.5 / リサンプル 0.5..1、続かないならデコードが 0..1。
        bool willResample = false;
        {
            std::unique_ptr<juce::AudioFormatReader> probe(formatManager.createReaderFor(src));
            if (probe == nullptr) { r.errorMessage = "Unsupported format"; return r; }
            willResample = std::abs(probe->sampleRate - projectSampleRate) >= 0.01;
        }
        std::function<bool(double)> decodeProgress;
        if (onProgress)
        {
            if (willResample)
            {
                decodeProgress   = [&onProgress](double p) { return onProgress(p * 0.5); };
                resampleProgress = [&onProgress](double p) { return onProgress(0.5 + p * 0.5); };
            }
            else
                decodeProgress = onProgress;
        }
        auto cache = getCacheFolder();
        // 変換後は WAV なので拡張子だけ .wav に差し替え、元の名前 (stem) は保つ
        // (Audio/ へは <元名>.wav でコピーされ、クリップ名も元名由来になる)。
        working = cache.getChildFile(juce::File::createLegalFileName(
                            src.getFileNameWithoutExtension() + ".wav"))
                       .getNonexistentSibling();
        juce::String terr;
        bool tcancelled = false;
        if (!transcodeToWavFloat(src, working, terr, tcancelled, decodeProgress))
        {
            working.deleteFile();
            r.cancelled = tcancelled;
            if (!tcancelled)
                r.errorMessage = terr.isNotEmpty() ? terr : juce::String("Unsupported format");
            return r;
        }
        didTranscode = true;
    }

    std::unique_ptr<juce::AudioFormatReader> reader(
        formatManager.createReaderFor(working));
    if (!reader)
    {
        if (didTranscode) working.deleteFile();
        r.errorMessage = "Unsupported format";
        return r;
    }

    r.sampleRate  = reader->sampleRate;
    r.numChannels = (int)reader->numChannels;
    r.durationSec = (double)reader->lengthInSamples / juce::jmax(1.0, reader->sampleRate);

    // SR一致なら変換不要 (元ファイルをそのまま使う)。64bit を変換した場合は変換物を返し、
    // リサンプルと同じく「Audio/ へコピー後に一時ファイルを消す」扱い (wasResampled=true) にする。
    if (std::abs(reader->sampleRate - projectSampleRate) < 0.01)
    {
        reader.reset();   // working を開いたままにしない (Windows では削除/移動が阻まれる)
        r.success = true;
        if (didTranscode) { r.file = working; r.wasResampled = true; }
        else              { r.file = src; }
        return r;
    }

    // リサンプル: キャッシュフォルダにユニーク名でWAV出力 (変換済みなら working を読む)
    auto cache = getCacheFolder();
    auto stem  = src.getFileNameWithoutExtension()
                 + juce::String::formatted("_%dHz_%lld.wav",
                                           (int)projectSampleRate,
                                           (long long)juce::Time::currentTimeMillis());
    auto dst   = cache.getChildFile(juce::File::createLegalFileName(stem));

    juce::String err;
    if (!resampleToFile(working, dst, reader->sampleRate, projectSampleRate,
                        (int)reader->numChannels, *reader, outputBits, err, resampleProgress))
    {
        r.errorMessage = err;
        r.cancelled    = (err == "cancelled");
        reader.reset();
        dst.deleteFile();   // 中断/失敗時の部分ファイルを片付ける (破損キャッシュを残さない)
        if (didTranscode) working.deleteFile();
        return r;
    }

    reader.reset();
    if (didTranscode) working.deleteFile();   // 中間変換ファイルはもう不要

    r.success      = true;
    r.file         = dst;
    r.wasResampled = true;
    r.sampleRate   = projectSampleRate;
    return r;
}

bool AudioFileImporter::resampleToFile(const juce::File& src, const juce::File& dst,
                                       double srcSr, double dstSr, int numChannels,
                                       juce::AudioFormatReader& reader,
                                       int outputBits,
                                       juce::String& errorOut,
                                       const std::function<bool(double)>& onProgress)
{
    juce::ignoreUnused(src);
    if (numChannels < 1) { errorOut = "No channels"; return false; }

    const int blockSize = 4096;
    // チャンネルごとに r8brain リサンプラを用意
    std::vector<std::unique_ptr<r8b::CDSPResampler24>> resamps;
    resamps.reserve((size_t)numChannels);
    for (int ch = 0; ch < numChannels; ++ch)
        resamps.emplace_back(std::make_unique<r8b::CDSPResampler24>(
            srcSr, dstSr, blockSize));

    juce::WavAudioFormat wav;
    auto outStreamUP = std::make_unique<juce::FileOutputStream>(dst);
    if (!outStreamUP->openedOk()) { errorOut = "Cannot open cache file"; return false; }
    outStreamUP->setPosition(0);
    outStreamUP->truncate();

    using SF = juce::AudioFormatWriterOptions::SampleFormat;
    auto opts = juce::AudioFormatWriterOptions{}
                    .withSampleRate(dstSr)
                    .withNumChannels(numChannels)
                    .withBitsPerSample(outputBits)
                    .withSampleFormat(outputBits == 32 ? SF::floatingPoint : SF::integral);

    std::unique_ptr<juce::OutputStream> outStream = std::move(outStreamUP);
    auto writer = wav.createWriterFor(outStream, opts);
    if (!writer) { errorOut = "Cannot create WAV writer"; return false; }

    juce::AudioBuffer<float>  inBuf((int)numChannels, blockSize);
    std::vector<std::vector<double>> inDouble((size_t)numChannels);
    for (auto& v : inDouble) v.resize((size_t)blockSize);

    juce::int64 totalIn = reader.lengthInSamples;
    juce::int64 expectedOut = (juce::int64)((double)totalIn * dstSr / srcSr);
    juce::int64 readPos = 0;
    juce::int64 writtenOut = 0;

    while (writtenOut < expectedOut)
    {
        if (onProgress && expectedOut > 0
            && ! onProgress((double) writtenOut / (double) expectedOut))
        {
            errorOut = "cancelled";
            return false;
        }

        int toRead = (int)juce::jmin((juce::int64)blockSize, totalIn - readPos);
        bool flushing = (toRead <= 0);

        if (!flushing)
        {
            inBuf.clear();
            reader.read(&inBuf, 0, toRead, readPos, true, true);
            readPos += toRead;
        }
        else
        {
            // ファイル末尾後はゼロを送ってリサンプラの遅延分を取り出す
            toRead = blockSize;
            inBuf.clear();
        }

        // float → double（チャンネル別）
        for (int ch = 0; ch < numChannels; ++ch)
        {
            auto* src = inBuf.getReadPointer(ch);
            for (int i = 0; i < toRead; ++i)
                inDouble[(size_t)ch][(size_t)i] = (double)src[i];
        }

        // チャンネル別にリサンプラ通過 → 出力ポインタを取得
        std::vector<double*> outPtrs((size_t)numChannels, nullptr);
        int writeCount = 0;
        for (int ch = 0; ch < numChannels; ++ch)
        {
            double* opp = nullptr;
            int wc = resamps[(size_t)ch]->process(
                inDouble[(size_t)ch].data(), toRead, opp);
            outPtrs[(size_t)ch] = opp;
            writeCount = wc;  // 全チャンネル同じ
        }

        if (writeCount <= 0)
        {
            if (flushing) break;  // これ以上出ない
            continue;
        }

        // ファイル尺を超えないようクリップ
        if (writtenOut + writeCount > expectedOut)
            writeCount = (int)(expectedOut - writtenOut);
        if (writeCount <= 0) break;

        // double → float に変換しつつ writer に渡す（インターリーブ無し: writer はチャンネル分離 API を取る）
        juce::AudioBuffer<float> outBuf((int)numChannels, writeCount);
        for (int ch = 0; ch < numChannels; ++ch)
        {
            auto* dstP = outBuf.getWritePointer(ch);
            for (int i = 0; i < writeCount; ++i)
                dstP[i] = (float)outPtrs[(size_t)ch][i];
        }

        // 24bit 出力時のみ TPDF ディザを足す（量子化雑音の信号相関を除去）
        if (outputBits == 24)
        {
            static thread_local juce::Random rng;
            const float lsb = 1.0f / (float)(1 << 23);  // 24bit の 1LSB（[-1,1] スケール）
            for (int ch = 0; ch < numChannels; ++ch)
            {
                auto* p = outBuf.getWritePointer(ch);
                for (int i = 0; i < writeCount; ++i)
                {
                    float n1 = rng.nextFloat() - 0.5f;
                    float n2 = rng.nextFloat() - 0.5f;
                    p[i] += (n1 + n2) * lsb;  // 三角分布（ピーク ±1 LSB）
                }
            }
        }

        if (!writer->writeFromAudioSampleBuffer(outBuf, 0, writeCount))
        {
            errorOut = "Failed to write samples (disk full?)";
            return false;
        }
        writtenOut += writeCount;

        if (flushing && writeCount == 0) break;
    }

    writer->flush();
    return true;
}

bool AudioFileImporter::copyStrippingMetadata(const juce::File& src, const juce::File& dst,
                                              juce::String& errorOut)
{
    if (!src.existsAsFile()) { errorOut = "Source not found"; return false; }
    // WAV のみ対応。他のフォーマットは iXML/bext を含まないため呼び出し側で plain copy を使う。
    if (!src.hasFileExtension("wav")) { errorOut = "Not a WAV file"; return false; }

    juce::WavAudioFormat wav;
    auto inputStream = src.createInputStream();
    if (inputStream == nullptr) { errorOut = "Cannot open source"; return false; }
    std::unique_ptr<juce::AudioFormatReader> reader(
        wav.createReaderFor(inputStream.get(), false));
    if (!reader) { errorOut = "Unsupported WAV"; return false; }
    inputStream.release();  // reader が所有

    // インポート時は LIST-INFO (RIFF INFO) の説明用タグだけを残し、それ以外のチャンク由来
    // メタデータ (bext / iXML・ASWG / smpl ループ / acid / tracktion 等のテンポ・ループ・
    // 各種制作情報) はすべて除去する。これにより他 DAW の埋め込みメタデータがプロジェクトへ
    // 流入するのを防ぐ。
    //
    // 判定は許可リスト方式: LIST-INFO のキーは RIFF 仕様上つねに 4 文字の FourCC
    // (例: IART=Artist, ICMT=Comment, ICRD=DateCreated)。一方、除去対象のキーはいずれも
    // FourCC にならない — "bwav description" (空白入り) / "tempo" "timeSig" "inKey" (iXML の
    // 小文字タグ名) / "Loop0Start" "NumSampleLoops" "Manufacturer" (smpl, 5 文字以上) /
    // "IXML_VERSION" (下線入り) など。そこで「4 文字の英大文字/数字」のキーのみ残す。
    //
    // (旧実装は "aswg"/"bwav"/"loop"/"sample" プレフィックス一致で除去していたが、JUCE が
    //  返す実キーは iXML が "tempo" 等の生タグ名・smpl が "Loop0Start"/"NumSampleLoops" で、
    //  "aswg"/"loop"/"sample" プレフィックスを持たない。さらに非プレフィックスの smpl キー
    //  (NumSampleLoops 等) が残ると writer が smpl チャンクを再生成するため、結果として iXML
    //  テンポ・smpl ループが素通りしていた。許可リスト方式はこの取りこぼしを構造的に防ぐ)
    auto isHarmlessInfoTag = [](const juce::String& key)
    {
        if (key.length() != 4) return false;
        for (auto ch : key)
            if (! ((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9')))
                return false;
        return true;
    };

    std::unordered_map<juce::String, juce::String> cleanMeta;
    const auto& meta = reader->metadataValues;
    const auto keys   = meta.getAllKeys();
    const auto values = meta.getAllValues();
    for (int i = 0; i < keys.size(); ++i)
        if (isHarmlessInfoTag(keys[i]))
            cleanMeta[keys[i]] = values[i];

    auto outStreamUP = std::make_unique<juce::FileOutputStream>(dst);
    if (!outStreamUP->openedOk()) { errorOut = "Cannot open destination"; return false; }
    outStreamUP->setPosition(0);
    outStreamUP->truncate();

    using SF = juce::AudioFormatWriterOptions::SampleFormat;
    const int bits = (int) reader->bitsPerSample;
    auto opts = juce::AudioFormatWriterOptions{}
                    .withSampleRate(reader->sampleRate)
                    .withNumChannels((int) reader->numChannels)
                    .withBitsPerSample(bits > 0 ? bits : 24)
                    .withSampleFormat(reader->usesFloatingPointData
                                        ? SF::floatingPoint : SF::integral)
                    .withMetadataValues(cleanMeta);

    std::unique_ptr<juce::OutputStream> outStream = std::move(outStreamUP);
    std::unique_ptr<juce::AudioFormatWriter> writer(wav.createWriterFor(outStream, opts));
    if (!writer) { errorOut = "Cannot create WAV writer"; return false; }

    if (!writer->writeFromAudioReader(*reader, 0, reader->lengthInSamples))
    {
        writer.reset();    // ストリームを閉じてから
        dst.deleteFile();  // 部分書き込みの破損ファイルを残さない
        errorOut = "Failed to copy samples";
        return false;
    }
    writer->flush();
    return true;
}

//==============================================================================
// 64bit WAV サポート: RIFF ヘッダの覗き見 + 32bit float への自前変換
//==============================================================================
AudioFileImporter::WavFormatInfo AudioFileImporter::peekWavFormat(const juce::File& src)
{
    WavFormatInfo info;
    if (!src.hasFileExtension("wav")) return info;

    auto in = src.createInputStream();
    if (in == nullptr) return info;

    auto readTag = [&in](char tag[4]) { return in->read(tag, 4) == 4; };

    char riff[4];
    if (!readTag(riff)) return info;
    // RF64 (>4GB) は data サイズが ds64 チャンク側にあり扱いが異なるため非対応 (ok=false)。
    if (std::memcmp(riff, "RIFF", 4) != 0) return info;
    in->skipNextBytes(4);                       // RIFF サイズ
    char wave[4];
    if (!readTag(wave) || std::memcmp(wave, "WAVE", 4) != 0) return info;

    bool haveFmt = false, haveData = false;
    while (!in->isExhausted() && !(haveFmt && haveData))
    {
        char id[4];
        if (!readTag(id)) break;
        const juce::uint32 sz = (juce::uint32) in->readInt();   // little-endian
        const juce::int64 chunkStart = in->getPosition();

        if (std::memcmp(id, "fmt ", 4) == 0)
        {
            info.formatTag  = (int) (juce::uint16) in->readShort();
            info.channels   = (int) (juce::uint16) in->readShort();
            info.sampleRate = (double) (juce::uint32) in->readInt();
            in->skipNextBytes(4);                               // byteRate
            in->skipNextBytes(2);                               // blockAlign
            info.bits       = (int) (juce::uint16) in->readShort();

            if (info.formatTag == 3)        info.isFloat = true;     // WAVE_FORMAT_IEEE_FLOAT
            else if (info.formatTag == 1)   info.isFloat = false;    // WAVE_FORMAT_PCM
            else if (info.formatTag == 0xFFFE && sz >= 40)           // WAVE_FORMAT_EXTENSIBLE
            {
                in->skipNextBytes(2);                           // cbSize
                in->skipNextBytes(2);                           // validBitsPerSample
                in->skipNextBytes(4);                           // channelMask
                const int sub = (int) (juce::uint16) in->readShort();   // SubFormat の先頭 = 実フォーマット
                info.isFloat = (sub == 3);
            }
            haveFmt = true;
        }
        else if (std::memcmp(id, "data", 4) == 0)
        {
            if (sz == 0xFFFFFFFFu) return WavFormatInfo{};       // RF64 マーカー → 非対応
            info.dataOffset = chunkStart;
            info.dataBytes  = (juce::int64) sz;
            haveData = true;
        }

        // 次のチャンクへ (チャンクは 2 バイト境界にパディングされる)
        const juce::int64 next = chunkStart + (juce::int64) sz + ((sz & 1u) ? 1 : 0);
        in->setPosition(next);
        if (in->getPosition() != next) break;
    }

    info.ok = haveFmt && haveData
              && info.channels > 0 && info.bits > 0 && info.sampleRate > 0.0;
    return info;
}

bool AudioFileImporter::needsHighBitTranscode(const juce::File& src)
{
    const auto info = peekWavFormat(src);
    return info.ok && info.bits > 32;
}

//==============================================================================
// 圧縮フォーマットのデコード変換: WAV/AIFF 以外を逐次デコードして 32bit float WAV へ
//==============================================================================
bool AudioFileImporter::needsDecodeTranscode(const juce::File& src)
{
    // WAV / AIFF は非圧縮でシークが sample-accurate なのでそのまま扱える。
    // それ以外 (MP3 / M4A / AAC / WMA / Ogg / FLAC / CAF 等) はすべて対象にする除外方式
    // (許可リストだと OS デコーダが読める形式が増えた時に取りこぼす)。
    return ! src.hasFileExtension("wav;aif;aiff");
}

bool AudioFileImporter::transcodeToWavFloat(const juce::File& src, const juce::File& dst,
                                            juce::String& errorOut, bool& cancelledOut,
                                            const std::function<bool(double)>& onProgress)
{
    cancelledOut = false;
    std::unique_ptr<juce::AudioFormatReader> reader(formatManager.createReaderFor(src));
    if (reader == nullptr)             { errorOut = "Unsupported format";  return false; }
    const int ch = (int) reader->numChannels;
    if (ch < 1 || reader->lengthInSamples <= 0) { errorOut = "Empty audio file"; return false; }

    juce::WavAudioFormat wav;
    auto outStreamUP = std::make_unique<juce::FileOutputStream>(dst);
    if (!outStreamUP->openedOk()) { errorOut = "Cannot open cache file"; return false; }
    outStreamUP->setPosition(0);
    outStreamUP->truncate();

    using SF = juce::AudioFormatWriterOptions::SampleFormat;
    auto opts = juce::AudioFormatWriterOptions{}
                    .withSampleRate(reader->sampleRate)
                    .withNumChannels(ch)
                    .withBitsPerSample(32)
                    .withSampleFormat(SF::floatingPoint);

    std::unique_ptr<juce::OutputStream> outStream = std::move(outStreamUP);
    std::unique_ptr<juce::AudioFormatWriter> writer(wav.createWriterFor(outStream, opts));
    if (!writer) { errorOut = "Cannot create WAV writer"; return false; }

    // 先頭から単調増加の位置でのみ read する (後方シーク無し)。圧縮デコーダは逐次読みなら
    // 決定論なので、シーク精度に依存しない正確なサンプル列が得られる。
    const int blockFrames = 4096;
    juce::AudioBuffer<float> buf(ch, blockFrames);
    const juce::int64 total = reader->lengthInSamples;
    juce::int64 pos = 0;
    while (pos < total)
    {
        if (onProgress && ! onProgress((double) pos / (double) total))
        {
            cancelledOut = true;
            writer.reset();     // ストリームを閉じてから
            dst.deleteFile();   // 部分ファイルを残さない
            return false;
        }

        const int n = (int) juce::jmin((juce::int64) blockFrames, total - pos);
        buf.clear();
        if (! reader->read(&buf, 0, n, pos, true, true))
            break;   // 尺の報告が過大なデコーダ対策: 読めた分だけ書いて終了

        if (! writer->writeFromAudioSampleBuffer(buf, 0, n))
        {
            errorOut = "Failed to write samples (disk full?)";
            writer.reset();
            dst.deleteFile();
            return false;
        }
        pos += n;
    }

    // 1 サンプルも読めずに終わった (最初の read で失敗) = デコード不能。空 WAV を成功として
    // 返すと無音の空クリップが無警告で取り込まれてしまう (破損 / 途中切れの MP3 や、途中で
    // エラーを返す OS デコーダで起きうる)。ここで失敗にして呼び出し側に案内させる。
    if (pos <= 0)
    {
        errorOut = "Could not decode audio";
        writer.reset();
        dst.deleteFile();
        return false;
    }

    writer->flush();
    return true;
}

bool AudioFileImporter::transcodeHighBitWavToFloat(const juce::File& src, const juce::File& dst,
                                                   juce::String& errorOut)
{
    const auto info = peekWavFormat(src);
    if (!info.ok)         { errorOut = "Cannot parse WAV header"; return false; }
    if (info.bits != 64)  { errorOut = "Unsupported bit depth";  return false; }   // 現状 64bit のみ

    const int ch             = info.channels;
    const int bytesPerSample = info.bits / 8;          // 8
    const int frameBytes     = ch * bytesPerSample;
    if (frameBytes <= 0)  { errorOut = "Bad frame size"; return false; }
    const juce::int64 totalFrames = info.dataBytes / frameBytes;

    auto in = src.createInputStream();
    if (in == nullptr)    { errorOut = "Cannot open source"; return false; }
    in->setPosition(info.dataOffset);

    juce::WavAudioFormat wav;
    auto outStreamUP = std::make_unique<juce::FileOutputStream>(dst);
    if (!outStreamUP->openedOk()) { errorOut = "Cannot open destination"; return false; }
    outStreamUP->setPosition(0);
    outStreamUP->truncate();

    using SF = juce::AudioFormatWriterOptions::SampleFormat;
    auto opts = juce::AudioFormatWriterOptions{}
                    .withSampleRate(info.sampleRate)
                    .withNumChannels(ch)
                    .withBitsPerSample(32)
                    .withSampleFormat(SF::floatingPoint);

    std::unique_ptr<juce::OutputStream> outStream = std::move(outStreamUP);
    std::unique_ptr<juce::AudioFormatWriter> writer(wav.createWriterFor(outStream, opts));
    if (!writer) { errorOut = "Cannot create WAV writer"; return false; }

    const int blockFrames = 4096;
    juce::AudioBuffer<float> buf(ch, blockFrames);
    std::vector<char> raw((size_t) blockFrames * (size_t) frameBytes);

    juce::int64 done = 0;
    while (done < totalFrames)
    {
        const int n         = (int) juce::jmin((juce::int64) blockFrames, totalFrames - done);
        const int wantBytes = n * frameBytes;
        const int gotBytes  = in->read(raw.data(), wantBytes);
        const int framesGot = (frameBytes > 0) ? (gotBytes / frameBytes) : 0;
        if (framesGot <= 0) break;   // EOF / 途中切れ

        for (int f = 0; f < framesGot; ++f)
            for (int c = 0; c < ch; ++c)
            {
                const void* p = raw.data() + (size_t) (f * frameBytes + c * bytesPerSample);
                // WAV は LE。littleEndianInt64 は数値として正しい値を返す (PCM 経路で利用)。
                // float 経路はビットパターンを double として解釈するが、対象プラットフォーム
                // (macOS / Windows) は LE なので le の格納バイト列 = p のバイト列で一致する。
                const juce::uint64 le = juce::ByteOrder::littleEndianInt64(p);
                float v;
                if (info.isFloat)
                {
                    double d;
                    std::memcpy(&d, &le, sizeof(d));
                    v = (float) d;
                }
                else
                {
                    // 64bit 符号付き PCM → [-1, 1)
                    v = (float) ((double) (juce::int64) le / 9223372036854775808.0);
                }
                buf.setSample(c, f, v);
            }

        if (!writer->writeFromAudioSampleBuffer(buf, 0, framesGot))
        {
            errorOut = "Failed to write samples (disk full?)";
            writer.reset();
            dst.deleteFile();
            return false;
        }
        done += framesGot;
        if (framesGot < n) break;    // 途中切れ: 読めた分だけ書いて終了
    }

    writer->flush();
    return true;
}
