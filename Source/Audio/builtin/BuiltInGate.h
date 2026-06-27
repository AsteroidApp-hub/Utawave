// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

#pragma once
#include "BuiltInEffect.h"
#include <atomic>

/**
    内蔵ノイズゲート。スレッショルドを下回る区間 (発声していない時の「サー」というノイズや
    空調音など) を、レンジで指定した量だけ動的に絞る。歌枠/配信で声を出していない時の
    定常ノイズを目立たなくするのが主目的。

    先読み無しのエンベロープゲートでゼロレイテンシー。ステレオはリンク (両 ch の最大で
    1 つのゲインを共有 = L/R の定位が崩れない)。アタックで開き、ホールドで開いたまま保持、
    リリースで閉じる。レンジ=0 は完全素通り (= バイパス相当)。
*/
class BuiltInGate : public BuiltInEffect
{
public:
    enum Param { ThresholdDb = 0, RangeDb, AttackMs, HoldMs, ReleaseMs, NumParams };

    BuiltInGate();

    const juce::String getName() const override;
    juce::String getIdentifier() const override { return "utawave.gate"; }
    juce::int32  getUid()        const override { return 0x55744754; } // 'UtGT'

    void prepareToPlay(double sampleRate, int blockSize) override;
    void releaseResources() override {}
    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;   // BuiltInGateEditor.cpp で定義

    // GR メータ (絞った量 dB)。コンプ/ディエッサーと同じ仕組みで右端の縦バーに出す。
    bool  hasReductionMeter() const override { return true; }
    float getReductionDb()    const override { return meterReductionDb.load(std::memory_order_relaxed); }

    // UI メータ/ステータス用 (生値・平滑化は UI 側)
    float getInputDb() const noexcept { return meterInputDb.load(std::memory_order_relaxed); }
    bool  isOpen()     const noexcept { return gateOpen.load(std::memory_order_relaxed); }

private:
    double sr { 48000.0 };
    float  gainDb { 0.0f };          // 平滑化済みゲイン (dB, 0=全開 / -range=全閉)
    int    holdCounter { 0 };        // ホールド残りサンプル数
    bool   open { false };           // ゲート開閉状態 (ブロック跨ぎで保持)

    std::atomic<float> meterReductionDb { 0.0f };
    std::atomic<float> meterInputDb     { -100.0f };
    std::atomic<bool>  gateOpen         { false };
};
