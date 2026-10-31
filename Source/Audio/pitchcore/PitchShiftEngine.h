// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave
//
// PitchCore — オフライン ピッチシフトエンジン (コア層)
//
// フレームワーク非依存 (C++17 標準ライブラリのみ)。単体ライブラリとして
// 切り出して配布できる。依存はリサンプラの r8brain-free-src (MIT, ヘッダオンリー) のみ。
// アルゴリズムの詳細・設計判断・検証方法は同ディレクトリの README.md を参照。

#pragma once
#include <functional>
#include <vector>

namespace pitchcore
{
    // 尺を維持したままピッチを semitones 半音シフトする (オフライン処理)。
    //   input:      planar float (numChannels 本のポインタ、各 numSamples サンプル)
    //   output:     numChannels × numSamples に確保して書き込む (入力と同尺・同 SR)
    //   semitones:  ±12 程度まで実用品質 (推奨 ±6)。0 は完全コピー
    //   onProgress: 0..1 を通知。false を返すと中断し、戻り値も false
    // 失敗 (不正引数 / キャンセル) で false。スレッドセーフ (内部状態はローカルのみ)。
    bool shiftPitch(const float* const* input, int numChannels, int numSamples,
                    double sampleRate, double semitones,
                    std::vector<std::vector<float>>& output,
                    const std::function<bool(double)>& onProgress = {});

    // ピッチ (semitones 半音) と尺 (timeRatio 倍) を 1 回の処理で同時に変える (オフライン処理)。
    // 内部のストレッチを (timeRatio × ピッチ比) 倍で 1 度だけ行うので、キー変更とタイム
    // ストレッチを別々に掛けるより劣化しない。
    //   output:    numChannels × round(numSamples × timeRatio) に確保して書き込む (同 SR)
    //   timeRatio: 0.25〜4 の範囲 (実用は 0.5〜2)。1.0 は shiftPitch と同一結果
    //   semitones=0 かつ timeRatio=1 は完全コピー
    bool shiftPitchAndTime(const float* const* input, int numChannels, int numSamples,
                           double sampleRate, double semitones, double timeRatio,
                           std::vector<std::vector<float>>& output,
                           const std::function<bool(double)>& onProgress = {});
}
