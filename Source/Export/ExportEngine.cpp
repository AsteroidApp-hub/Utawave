// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

#include "ExportEngine.h"
#include "../Localisation.h"
#include "../Audio/AudioEngine.h"
#include "Mp3EncoderWriter.h"
#include "CDSPResampler.h"  // r8brain (header-only)

namespace {

// HP-TPDF ディザ + 心理音響ノイズシェイピング (9-tap, 改良 E-weighted)
//
// 2 段階で量子化ノイズを高域へ追い出す:
//   1. HP-TPDF: 連続する TPDF サンプルの差分を取りディザ自体を高域寄りに
//      (Lipshitz/Vanderkooy/Wannamaker, "Quantization and Dither: A Theoretical Survey")
//   2. ノイズシェイピング: 9-tap IIR フィードバックで残った量子化誤差を 20kHz 付近へ
//      (Wannamaker 1992, "Psychoacoustically Optimal Noise Shaping")
void applyDither(juce::AudioBuffer<float>& buf, int bits)
{
    if (bits >= 32) return;
    const float lsb = 1.0f / (float)(1 << (bits - 1));
    juce::Random rng;

    static constexpr int   kNTaps = 9;
    static constexpr float shaperCoefs[kNTaps] = {
        2.412f, -3.370f,  3.937f, -4.174f,  3.353f,
       -2.205f,  1.281f, -0.569f,  0.0847f
    };

    for (int ch = 0; ch < buf.getNumChannels(); ++ch)
    {
        auto* p = buf.getWritePointer(ch);
        float err[kNTaps] = { 0 };   // 過去 9 サンプルの量子化誤差 (LSB 単位)
        float prevTpdf    = 0.0f;    // HP-TPDF 用、前サンプルの TPDF 値

        for (int i = 0; i < buf.getNumSamples(); ++i)
        {
            // 1. ノイズフィードバック (過去エラーを高域へシフト)
            float feedback = 0.0f;
            for (int k = 0; k < kNTaps; ++k) feedback += shaperCoefs[k] * err[k];

            // 2. HP-TPDF ディザ: 連続 TPDF の差分 → 1 次ハイパスでディザ自体も高域へ
            //    振幅補正のため √2 で割って TPDF 同等の RMS に揃える
            const float n1 = rng.nextFloat() - 0.5f;
            const float n2 = rng.nextFloat() - 0.5f;
            const float currTpdf = n1 + n2;
            const float dither = (currTpdf - prevTpdf) * 0.7071067f * lsb;  // 1/√2
            prevTpdf = currTpdf;

            // 3. 量子化 (LSB グリッドへスナップ)
            const float in       = p[i] - feedback * lsb;
            const float withDth  = in + dither;
            const float quant    = std::round(withDth / lsb) * lsb;

            // 4. 量子化誤差を LSB 単位で記録し、履歴をシフト
            const float newErr   = (quant - withDth) / lsb;
            for (int k = kNTaps - 1; k > 0; --k) err[k] = err[k - 1];
            err[0] = newErr;

            p[i] = quant;
        }
    }
}

bool resampleBuffer(juce::AudioBuffer<float>& inOut, double srcSr, double dstSr,
                    juce::String& errOut)
{
    if (std::abs(srcSr - dstSr) < 0.01) return true;

    const int nIn      = inOut.getNumSamples();
    const int channels = inOut.getNumChannels();
    if (nIn <= 0 || channels < 1) return true;

    const int blockSize = 4096;
    std::vector<std::unique_ptr<r8b::CDSPResampler24>> rs;
    rs.reserve((size_t)channels);
    for (int c = 0; c < channels; ++c)
        rs.emplace_back(std::make_unique<r8b::CDSPResampler24>(srcSr, dstSr, blockSize));

    const juce::int64 expectedOut = (juce::int64)((double)nIn * dstSr / srcSr);

    juce::AudioBuffer<float> out((int)channels, (int)expectedOut + blockSize);
    out.clear();

    std::vector<std::vector<double>> inDouble((size_t)channels);
    for (auto& v : inDouble) v.resize((size_t)blockSize);

    int readPos = 0;
    juce::int64 writtenOut = 0;

    while (writtenOut < expectedOut)
    {
        int toRead = juce::jmin(blockSize, nIn - readPos);
        bool flushing = (toRead <= 0);
        if (flushing) toRead = blockSize;

        for (int c = 0; c < channels; ++c)
        {
            const float* src = inOut.getReadPointer(c);
            for (int i = 0; i < toRead; ++i)
                inDouble[(size_t)c][(size_t)i] = flushing ? 0.0
                                                          : (double)src[readPos + i];
        }
        if (!flushing) readPos += toRead;

        int writeCount = 0;
        std::vector<double*> outPtrs((size_t)channels, nullptr);
        for (int c = 0; c < channels; ++c)
        {
            double* opp = nullptr;
            int wc = rs[(size_t)c]->process(inDouble[(size_t)c].data(), toRead, opp);
            outPtrs[(size_t)c] = opp;
            writeCount = wc;
        }

        if (writeCount <= 0)
        {
            if (flushing) break;
            continue;
        }

        if (writtenOut + writeCount > expectedOut)
            writeCount = (int)(expectedOut - writtenOut);
        if (writeCount <= 0) break;

        for (int c = 0; c < channels; ++c)
        {
            auto* dst = out.getWritePointer(c);
            for (int i = 0; i < writeCount; ++i)
                dst[(int)writtenOut + i] = (float)outPtrs[(size_t)c][i];
        }
        writtenOut += writeCount;
    }

    inOut.setSize(channels, (int)expectedOut, false, false, true);
    for (int c = 0; c < channels; ++c)
        inOut.copyFrom(c, 0, out, c, 0, (int)expectedOut);

    juce::ignoreUnused(errOut);
    return true;
}

} // namespace

bool ExportEngine::render(AudioEngine& engine, const Options& opts,
                          std::function<void(double)> progress,
                          std::function<bool()> shouldCancel,
                          juce::String* errorOut)
{
    auto setErr = [errorOut](const juce::String& s) { if (errorOut) *errorOut = s; };

    if (opts.endSec <= opts.startSec)
    {
        setErr(tr(u8"範囲が無効です"));
        return false;
    }

    const double engineSr = engine.getSampleRate();
    if (engineSr <= 0.0)
    {
        setErr(tr(u8"オーディオエンジンが起動していません"));
        return false;
    }

    // Phase 1: 描画（オフライン or 実時間）
    juce::AudioBuffer<float> buffer;
    if (opts.realtime)
    {
        const int totalSamp = (int)std::round((opts.endSec - opts.startSec) * engineSr);
        engine.beginRealtimeCapture(totalSamp);
        engine.setPosition(opts.startSec);
        engine.play();

        // 完了まで待つ
        while (engine.getRealtimeCaptureWritePos() < totalSamp)
        {
            if (shouldCancel && shouldCancel())
            {
                engine.stop();
                engine.endRealtimeCapture();
                return false;
            }
            if (progress) progress(0.85 * (double)engine.getRealtimeCaptureWritePos() / (double)totalSamp);
            juce::Thread::sleep(40);
        }
        engine.stop();
        engine.copyRealtimeCaptureTo(buffer);
        engine.endRealtimeCapture();
    }
    else
    {
        engine.renderOfflineRange(opts.startSec, opts.endSec, buffer,
            [progress, shouldCancel](double p)
            {
                if (progress) progress(p * 0.85);
            },
            opts.selectedTrackIndices,
            opts.preFader,
            opts.includeClick);
    }

    if (shouldCancel && shouldCancel()) return false;

    // Phase 2: SR 変換
    const double dstSr = (opts.sampleRate > 0.0) ? opts.sampleRate : engineSr;
    if (std::abs(dstSr - engineSr) > 0.01)
    {
        juce::String e;
        if (!resampleBuffer(buffer, engineSr, dstSr, e))
        {
            setErr(tr(u8"リサンプル失敗: ") + e);
            return false;
        }
    }
    if (progress) progress(0.92);

    // Phase 2.5: チャンネル数調整（モノラル指定なら L+R を平均で 1ch に）
    int outChannels = juce::jlimit(1, 2, opts.numChannels);
    if (outChannels == 1 && buffer.getNumChannels() >= 2)
    {
        const int n = buffer.getNumSamples();
        juce::AudioBuffer<float> mono(1, n);
        auto* dst = mono.getWritePointer(0);
        const auto* L = buffer.getReadPointer(0);
        const auto* R = buffer.getReadPointer(1);
        for (int i = 0; i < n; ++i)
            dst[i] = 0.5f * (L[i] + R[i]);
        buffer.makeCopyOf(mono);
    }

    // Phase 2.7: ピーク超過保護 (環境設定でオン/オフ、デフォルト ON)
    // 32bit float でも超過分を減衰させて再生機で歪まないようにする。
    if (opts.peakGuard)
    {
        float peak = 0.0f;
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        {
            const float* p = buffer.getReadPointer(ch);
            for (int i = 0; i < buffer.getNumSamples(); ++i)
                peak = juce::jmax(peak, std::abs(p[i]));
        }
        // -0.1 dBFS まで余裕を持たせる (ディザ加算でわずかに山が伸びても 0 dBFS 以下)
        constexpr float ceiling = 0.989f;  // 10^(-0.1/20)
        if (peak > ceiling)
            buffer.applyGain(ceiling / peak);
    }

    // Phase 3: ディザ
    if (opts.dither && opts.bitDepth < 32)
        applyDither(buffer, opts.bitDepth);

    // Phase 4: ファイル書き出し
    // ※ 同名衝突時の連番処理は呼び出し側 (MainComponent) で行う想定
    juce::File outFile = opts.file;
    if (outFile.exists()) outFile.deleteFile();

    if (opts.format == Format::MP3)
    {
        // 内蔵 libmp3lame でエンコード（外部バイナリ不要・全プラットフォーム共通）
        juce::String e;
        if (!Mp3EncoderWriter::encodeBuffer(buffer, dstSr, opts.mp3BitrateKbps, outFile, &e,
                                            shouldCancel))
        {
            outFile.deleteFile();  // 部分書き込みの破損 MP3 を残さない
            if (shouldCancel && shouldCancel()) return false;  // キャンセルはエラー扱いしない
            setErr(tr(u8"MP3 書き出しに失敗しました: ") + e);
            return false;
        }
        if (progress) progress(1.0);
        return true;
    }

    std::unique_ptr<juce::AudioFormat> format;
    if (opts.format == Format::WAV)        format = std::make_unique<juce::WavAudioFormat>();
    else if (opts.format == Format::AIFF)  format = std::make_unique<juce::AiffAudioFormat>();
    else { setErr(tr(u8"未対応フォーマット")); return false; }

    auto streamUP = std::make_unique<juce::FileOutputStream>(outFile);
    if (!streamUP->openedOk())
    {
        setErr(tr(u8"出力ファイルを開けません"));
        return false;
    }
    streamUP->setPosition(0);
    streamUP->truncate();

    using SF = juce::AudioFormatWriterOptions::SampleFormat;
    auto wopts = juce::AudioFormatWriterOptions{}
                     .withSampleRate(dstSr)
                     .withNumChannels(outChannels)
                     .withBitsPerSample(opts.bitDepth)
                     .withSampleFormat(opts.bitDepth == 32 ? SF::floatingPoint : SF::integral);

    std::unique_ptr<juce::OutputStream> stream = std::move(streamUP);
    std::unique_ptr<juce::AudioFormatWriter> writer(format->createWriterFor(stream, wopts));
    if (!writer)
    {
        setErr(tr(u8"ライターを作成できません"));
        return false;
    }

    if (!writer->writeFromAudioSampleBuffer(buffer, 0, buffer.getNumSamples()))
    {
        writer.reset();        // ストリームを閉じてから
        outFile.deleteFile();  // 部分書き込みの破損ファイルを残さない
        setErr(tr(u8"書き出しに失敗しました"));
        return false;
    }
    writer->flush();

    if (progress) progress(1.0);
    return true;
}
