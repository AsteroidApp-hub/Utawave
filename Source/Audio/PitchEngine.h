// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

#pragma once
#include <JuceHeader.h>
#include <functional>

// 自社製オフライン ピッチシフトエンジン (JUCE アダプタ層)
//
// 実体は `pitchcore/` (フレームワーク非依存・C++17 のみ・切り出して単体配布可能) にあり、
// ここは juce::AudioBuffer / juce::File と pitchcore::shiftPitch の橋渡しだけを行う。
// アルゴリズム (フェーズボコーダ + r8brain リサンプル) の詳細は pitchcore/README.md を参照。
namespace PitchEngine
{
    // バッファ→バッファの中核 (テスト可能・ファイル I/O 無し)。
    // onProgress: 0..1 を通知。false を返すと中断し、戻り値も false。
    bool shiftBuffer(const juce::AudioBuffer<float>& input,
                     double sampleRate,
                     double semitones,
                     juce::AudioBuffer<float>& output,
                     const std::function<bool(double)>& onProgress = {});

    // ファイル変換ラッパ (入力を読み→shiftBuffer→WAV 書き出し)。
    // semitones: ±12 程度まで実用品質 / outputBits: 16 / 24 / 32 (32 = float)
    bool processFile(const juce::File& inputFile,
                     const juce::File& outputFile,
                     juce::AudioFormatManager& fmt,
                     double semitones,
                     int outputBits = 32,
                     std::function<void(double)> onProgress = {});

    // ピッチ (semitones) と尺 (timeRatio 倍) を同時に変える版 (テスト可能・I/O 無し)。
    // output は round(入力長 × timeRatio) サンプルになる。
    bool shiftBufferAndTime(const juce::AudioBuffer<float>& input,
                            double sampleRate, double semitones, double timeRatio,
                            juce::AudioBuffer<float>& output,
                            const std::function<bool(double)>& onProgress = {});

    // 入力ファイルの [startSample, startSample+numSamples) だけを読み、shiftBufferAndTime して
    // 32bit float WAV へ書き出す (タイムストレッチのクリップ区間レンダー用)。
    // onProgress が false を返すと中断し、書きかけの出力は削除する。
    bool renderRegion(const juce::File& inputFile, const juce::File& outputFile,
                      juce::AudioFormatManager& fmt,
                      juce::int64 startSample, juce::int64 numSamples,
                      double semitones, double timeRatio,
                      const std::function<bool(double)>& onProgress = {});
}
