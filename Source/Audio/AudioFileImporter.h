// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

#pragma once
#include <JuceHeader.h>

class AudioFileImporter
{
public:
    AudioFileImporter(juce::AudioFormatManager& fmt) : formatManager(fmt) {}

    // 取り込み結果
    struct Result
    {
        bool        success    { false };
        juce::File  file;          // 実際に使用するファイル（リサンプル時はキャッシュ）
        double      durationSec { 0.0 };
        double      sampleRate { 0.0 };
        int         numChannels { 0 };
        bool        wasResampled { false };
        bool        cancelled   { false };  // onProgress が false を返して中断された
        juce::String errorMessage;
    };

    // 入力ファイルをプロジェクトSRと比較し、必要なら r8brain でリサンプルしたキャッシュを生成
    // 元ファイルは変更しない。SR一致時は元ファイルへの参照のみ返す。
    // outputBits: 32 = 32bit float（既定・ディザ不要）、24 = 24bit PCM + TPDFディザ
    // onProgress: リサンプル進捗 (0..1) を報告する任意コールバック。false を返すと中断する
    //             (success=false, cancelled=true で返る)。SR一致 (リサンプル無し) では呼ばれない。
    Result importFile(const juce::File& src, double projectSampleRate, int outputBits = 32,
                      std::function<bool(double)> onProgress = {});

    // プロジェクト連携用: 設定するとリサンプル出力先がここから取得される
    std::function<juce::File()> getCacheFolderCb;

    static juce::File getDefaultCacheFolder();
    juce::File getCacheFolder() const;

    // WAV ファイルを iXML / bext チャンクを取り除いて dst にコピーする。
    // (他 DAW で埋め込まれたテンポ情報・各種メタデータがプロジェクトに流入するのを防ぐ)
    // 戻り値 false = 失敗 (src が WAV でない場合や I/O エラー)。
    bool copyStrippingMetadata(const juce::File& src, const juce::File& dst,
                               juce::String& errorOut);

    // ── 64bit (>32bit) WAV のサポート ──
    // JUCE の WavAudioFormat は 8/16/24/32bit のみデコード/エンコードでき、64bit (Float64 /
    // 64bit PCM) は読み書きできない (createReaderFor が bitsPerSample>32 で nullptr を返す)。
    // macOS は CoreAudio フォールバックで偶然読めるが Windows には無く、メタデータ除去経路は
    // WavAudioFormat を直接使うので macOS でも 64bit では失敗する。そこで取り込み時に
    // OS 非依存で 32bit float の一時 WAV へ自前変換してから既存パイプラインに乗せる。
    struct WavFormatInfo
    {
        bool        ok          { false };
        int         formatTag   { 0 };       // 1=PCM, 3=IEEE float, 0xFFFE=EXTENSIBLE
        int         channels    { 0 };
        double      sampleRate  { 0.0 };
        int         bits        { 0 };       // bitsPerSample
        bool        isFloat     { false };
        juce::int64 dataOffset  { 0 };       // data チャンクのサンプル先頭オフセット
        juce::int64 dataBytes   { 0 };
    };

    // .wav の RIFF ヘッダを覗いて fmt / data チャンク情報を取り出す純関数 (デコードはしない)。
    // 非 WAV・パース不能・RF64(>4GB) は ok=false。
    static WavFormatInfo peekWavFormat(const juce::File& src);

    // この WAV が JUCE で読めない 64bit (>32bit) のため変換が必要か。
    static bool needsHighBitTranscode(const juce::File& src);

    // 64bit WAV (Float64 / 64bit PCM) を 32bit float の WAV (dst) へ変換する。
    // SR / チャンネル数は保持し、サンプル値はそのまま (クランプしない)。
    static bool transcodeHighBitWavToFloat(const juce::File& src, const juce::File& dst,
                                           juce::String& errorOut);

private:
    bool resampleToFile(const juce::File& src, const juce::File& dst,
                        double srcSr, double dstSr, int numChannels,
                        juce::AudioFormatReader& reader,
                        int outputBits,
                        juce::String& errorOut,
                        const std::function<bool(double)>& onProgress = {});

    juce::AudioFormatManager& formatManager;
};
