// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

#pragma once
#include "BuiltInEffect.h"
#include <atomic>
#include <cstdint>
#include <vector>

/**
    内蔵ケロケロボイス (リアルタイム・ハードピッチ補正)。歌枠/配信で「ケロる」ための軽量エフェクト。

    - **検出**: 入力のモノ和を 1/4 デシメーション + YIN (CMNDF) で基音を推定 (約 5ms 間隔)。
      60〜1000 Hz のボーカル帯域のみ。無声音/無音は補正しない (子音・ブレスは素通り)。
    - **スナップ**: 検出ピッチを半音階 (クロマチック) またはキー指定のメジャー/マイナー
      スケールの最寄りノートへ。境界のばたつきはヒステリシスで抑える。
    - **シフト**: ピッチ同期スプライスのディレイライン方式 (検出周期ぶんだけ戻る/進める +
      短いクロスフェード)。FFT も先読みも使わないので **報告レイテンシー 0** (PDC 不要)、
      実効遅延もディレイラインの数 ms のみ。ライブモニタ経由でそのまま「ケロった声」を返せる。
    - **スピード** 0 で即座にノートへ張り付く (ケロケロ)。上げるほど滑らかな補正になる。
    - バッファは prepareToPlay で確保し audio スレッドでは再確保しない。ステレオは検出・
      ポインタ制御を共有 (定位維持)、リングだけ独立。
*/
class BuiltInKeroVoice : public BuiltInEffect
{
public:
    enum Param { Scale = 0, Key, Speed, Mix, NumParams };
    enum ScaleType { Chromatic = 0, Major = 1, Minor = 2 };

    BuiltInKeroVoice();

    const juce::String getName() const override;
    juce::String getIdentifier() const override { return "utawave.kerovoice"; }
    juce::int32  getUid()        const override { return 0x55744B56; } // 'UtKV'

    void prepareToPlay(double sampleRate, int blockSize) override;
    void releaseResources() override {}
    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;   // BuiltInKeroVoiceEditor.cpp で定義

    // ─── UI 用の読み出し (audio が書き UI が読む・可視化なのでテアリング許容) ───
    float getUiInputMidi()  const { return uiInMidi.load(std::memory_order_relaxed);  }  // <0 = 無声
    float getUiTargetMidi() const { return uiOutMidi.load(std::memory_order_relaxed); }  // <0 = 無声
    float getUiInputHz()    const { return uiInHz.load(std::memory_order_relaxed);    }

    // キー名 (ASCII・UI のキーボタンと表示で共用)
    static const char* keyLabel(int idx);
    // pc (0..11) が scale/key に含まれるか (UI のグリッド強調と共用)
    static bool isPitchClassInScale(int pc, int scaleType, int key);

    static constexpr float kMinHz = 60.0f;
    static constexpr float kMaxHz = 1000.0f;

private:
    void runDetection(int scaleType, int key);
    float snapToScale(float midiF, int scaleType, int key);
    void setUnvoiced();

    inline float readCubic(const std::vector<float>& buf, double pos) const noexcept
    {
        const auto i1 = (std::int64_t) std::floor(pos);
        const float frac = (float) (pos - (double) i1);
        const auto m = (std::int64_t) bufMask;
        const float y0 = buf[(size_t) ((i1 - 1) & m)];
        const float y1 = buf[(size_t) (i1 & m)];
        const float y2 = buf[(size_t) ((i1 + 1) & m)];
        const float y3 = buf[(size_t) ((i1 + 2) & m)];
        const float c1 = 0.5f * (y2 - y0);
        const float c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
        const float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
        return ((c3 * frac + c2) * frac + c1) * frac + y1;
    }

    double sr { 48000.0 };

    // ─── シフタ (ピッチ同期スプライスのディレイライン) ───
    static constexpr double kMinDelay = 4.0;   // 3 次補間が未書き込みサンプルを読まない下限
    int bufMask { 0 };
    std::vector<float> ringL, ringR;
    std::int64_t writeCount { 0 };
    double readPos { 0.0 };
    double curRatio { 1.0 }, targetRatio { 1.0 };
    float  curPeriod { 218.0f };     // 入力周期 (サンプル)。無声中は直近値を保持
    int    fadeRemain { 0 }, fadeLen { 1 };
    double fadeOldPos { 0.0 };

    // ─── 検出 (1/4 デシメーション + YIN) ───
    static constexpr int kDecim = 4;
    float detLp { 0.0f };            // デシメーション前の一極ローパス状態
    float detLpCoef { 0.5f };
    int   decimPhase { 0 };
    int   decMask { 0 };
    std::vector<float> decRing;
    std::int64_t decCount { 0 };
    std::vector<float> detScratch;   // 直近 (winDec+maxLagDec) サンプルの線形コピー
    std::vector<float> cmndf;
    int minLagDec { 12 }, maxLagDec { 200 }, winDec { 200 };
    int detCounter { 0 }, detInterval { 240 };
    float lvl { 0.0f };              // 短時間平均二乗 (レベルゲート)
    bool  voiced { false };
    float lastTargetMidi { -1.0f };  // ノート境界のヒステリシス用

    std::atomic<float> uiInMidi { -1.0f }, uiOutMidi { -1.0f }, uiInHz { 0.0f };
};
