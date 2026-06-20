// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

#pragma once
#include "BuiltInEffect.h"
#include <atomic>

/**
    内蔵コンプレッサー。先読み (ルックアヘッド) 無しのフィードフォワード型なので
    ゼロレイテンシー。ステレオはリンク (両 ch の最大値で 1 つのゲインを共有)。
    ソフトニー固定。GR メータ用に減衰量を公開する。
*/
class BuiltInCompressor : public BuiltInEffect
{
public:
    enum Param { ThresholdDb = 0, Ratio, AttackMs, ReleaseMs, MakeupDb, AutoGain, NumParams };

    BuiltInCompressor();

    // 実効メイクアップゲイン (dB)。オートゲイン ON ならしきい値/レシオから自動算出 (静的ゲイン=
    // レイテンシー無し)、OFF なら Makeup ノブの値。DSP と UI のカーブで共用する単一の真実源。
    float currentMakeupDb() const;

    const juce::String getName() const override;
    juce::String getIdentifier() const override { return "utawave.comp"; }
    juce::int32  getUid()        const override { return 0x55744350; } // 'UtCP'

    void prepareToPlay(double sampleRate, int blockSize) override;
    void releaseResources() override {}
    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;   // BuiltInCompressorEditor.cpp で定義

    bool  hasReductionMeter() const override { return true; }
    float getReductionDb()    const override { return meterReductionDb.load(std::memory_order_relaxed); }
    // UI メーター用の入出力ピーク (dBFS, ブロック毎の生値。平滑化は UI 側)
    float getInputDb()  const { return meterInputDb.load(std::memory_order_relaxed); }
    float getOutputDb() const { return meterOutputDb.load(std::memory_order_relaxed); }

private:
    double sr { 48000.0 };
    float  gainDb { 0.0f };               // 現在の平滑化済みゲイン (dB, 通常 <= 0)
    std::atomic<float> meterReductionDb { 0.0f };  // UI 表示用 (正値 = 減衰量)
    std::atomic<float> meterInputDb  { -100.0f };
    std::atomic<float> meterOutputDb { -100.0f };
    static constexpr float kKneeDb { 6.0f };
};
