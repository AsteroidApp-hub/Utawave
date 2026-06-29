// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

#pragma once
#include "BuiltInEffect.h"
#include <atomic>
#include <vector>

/**
    内蔵マキシマイザー。ラフに「音圧を上げて聴く」ための先読み (ルックアヘッド) 付き
    ブリックウォール・リミッター。ドライブで全体を押し上げ、シーリングを超えないように
    先読みでピークを予測して動的に抑える (歌枠/配信ミックスのラフ確認用)。

    - **先読みでレイテンシーが出る** (lookahead 分)。マキシマイザーは重い/遅い前提なので許容
      (再生中は PluginChain の PDC で他トラックと整合する。ライブモニターは前倒し補正できず
      lookahead 分の遅れが聞こえる = 他のルックアヘッド系と同じ)。
    - ステレオはリンク (両 ch の最大で 1 つのゲインを共有 = 定位が崩れない)。
    - 先読みの平滑化に加え、最終段でシーリングへハードクリップする安全網を入れ、残留オーバー
      シュートも確実にブリックウォールにする (聴感上は無視できる微小量)。
    - GR メータ (押さえた量) + 入出力ピークを公開する (コンプと同じ仕組み)。
*/
class BuiltInMaximizer : public BuiltInEffect
{
public:
    enum Param { DriveDb = 0, CeilingDb, ReleaseMs, NumParams };

    BuiltInMaximizer();

    const juce::String getName() const override;
    juce::String getIdentifier() const override { return "utawave.maximizer"; }
    juce::int32  getUid()        const override { return 0x55744D58; } // 'UtMX'

    void prepareToPlay(double sampleRate, int blockSize) override;
    void releaseResources() override {}
    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;   // BuiltInMaximizerEditor.cpp で定義

    bool  hasReductionMeter() const override { return true; }
    float getReductionDb()    const override { return meterReductionDb.load(std::memory_order_relaxed); }
    // UI メーター用の入出力ピーク (dBFS, ブロック毎の生値。平滑化は UI 側)
    float getInputDb()  const { return meterInputDb.load(std::memory_order_relaxed); }
    float getOutputDb() const { return meterOutputDb.load(std::memory_order_relaxed); }

    static constexpr float kFullScaleGrDb { 24.0f }; // GR メータ上限

private:
    double sr { 48000.0 };
    int    lookahead { 144 };       // 先読みサンプル数 (= 遅延 = setLatencySamples)
    int    writePos { 0 };
    int    holdCounter { 0 };       // 減衰を lookahead サンプル保持する残数 (リリースが先読みを潰さないように)
    float  gain { 1.0f };           // 現在の平滑化済みリニアゲイン (<=1)
    std::vector<float> delayL, delayR;  // 先読み遅延ライン (prepareToPlay で確保・audio では再確保しない)

    std::atomic<float> meterReductionDb { 0.0f };
    std::atomic<float> meterInputDb  { -100.0f };
    std::atomic<float> meterOutputDb { -100.0f };
};
