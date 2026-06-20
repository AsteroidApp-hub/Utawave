// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

#pragma once
#include "BuiltInEffect.h"
#include "Biquad.h"
#include <array>
#include <atomic>

/**
    内蔵 EQ。ローカット (HPF) + 低域シェルフ + 中域ピーク (プレゼンス) + 高域シェルフ (エア)。
    すべて IIR 双二次 (ゼロレイテンシー)。歌の整音に必要十分なバンドに絞る。

    UI (BuiltInEQEditor) 向けに「入力スペクトラムのタップ」と「現在の周波数特性 (dB)」を提供する。
*/
class BuiltInEQ : public BuiltInEffect
{
public:
    // 周波数順に並べる。低/低中/中/高中はピーク (ベル) で **Q を持つ**。両端 (HPF/高シェルフ) は
    // Q を持たない。保存は id 文字列ベースなので enum 値が変わっても旧プロジェクトは読める。
    enum Param { HpfHz = 0,
                 LowHz, LowDb, LowQ,
                 LoMidHz, LoMidDb, LoMidQ,
                 MidHz, MidDb, MidQ,
                 HiMidHz, HiMidDb, HiMidQ,
                 AirHz, AirDb, NumParams };

    BuiltInEQ();

    const juce::String getName() const override;
    juce::String getIdentifier() const override { return "utawave.eq"; }
    juce::int32  getUid()        const override { return 0x55744551; } // 'UtEQ'

    void prepareToPlay(double sampleRate, int blockSize) override;
    void releaseResources() override {}
    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;   // BuiltInEQEditor.cpp で定義

    // ─── アナライザー (UI 用・スペクトラム表示) ───
    static constexpr int kAnalyzerSize = 2048;   // 2 の冪
    double getSampleRateForUi() const noexcept { return sr; }   // message thread から読む (prepare で確定)
    void   setAnalyzerActive(bool on) noexcept { analyzerActive.store(on, std::memory_order_relaxed); }
    // 最新 maxN サンプル (入力 pre-EQ のモノ和) を時系列順に dest へコピー。コピー数を返す。
    int    readAnalyzerSamples(float* dest, int maxN) const;

    // 現在のパラメータでの周波数特性 (dB) を freqHz[i] について outDb[i] へ書く。
    // ライブの biquad とは別のローカル biquad で計算するので audio thread と競合しない。
    void getMagnitudeResponse(const double* freqHz, double* outDb, int count) const;

private:
    double sr { 48000.0 };
    Biquad hpf, low, loMid, mid, hiMid, air;

    // アナライザー用リングバッファ (audio thread が write、message thread が read)。
    std::array<float, kAnalyzerSize> analyzerRing {};
    std::atomic<int>  analyzerWritePos { 0 };
    std::atomic<bool> analyzerActive   { false };
};
