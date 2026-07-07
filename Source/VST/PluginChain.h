// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

#pragma once
#include <JuceHeader.h>

/**
    トラック単位のインサートエフェクトチェーン。
    複数のプラグインを直列に通し、入力バッファをそのまま加工する。
*/
class PluginChain
{
public:
    PluginChain();
    ~PluginChain();

    // ─── プラグイン管理（メッセージスレッドから呼び出す） ───
    void   addPlugin(std::unique_ptr<juce::AudioPluginInstance> plugin);
    // 指定スロット index にプラグインを挿入。
    // slotIdx までの隙間は空スロットで埋まる（空スロットは音声処理ではスキップされる）
    void   insertPluginAt(int slotIdx, std::unique_ptr<juce::AudioPluginInstance> plugin);
    void   removePlugin(int index);
    // プラグインインスタンスを破棄せず取り出す（D&D 移動など、別チェーンに移すために使う）。
    // スロットは空になり末尾の空スロットは掃除される。返り値が空（nullptr）の場合は何もしない。
    std::unique_ptr<juce::AudioPluginInstance> extractPlugin(int index);
    void   movePlugin(int from, int to);
    void   swapSlots(int a, int b);
    int    getNumPlugins() const;
    // ロックを取らずにスロット数を読む (audio thread のガード用)。値は getNumPlugins() と一致する。
    int    getActivePluginCountAtomic() const noexcept
    {
        return activePluginCount.load(std::memory_order_acquire);
    }
    juce::AudioPluginInstance* getPlugin(int index) const;

    // ─── オーディオ処理 ───
    void prepareToPlay(double sampleRate, int blockSize);
    void releaseResources();
    /** buffer をプラグインチェーン全体に通す（in-place）。
        @param playHead 各プラグインへ供給する再生位置情報 (Melodyne 等の transport 同期用)。
                        nullptr なら設定しない（書き出し以外の経路や transport 不要なテスト用）。 */
    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi,
                      juce::AudioPlayHead* playHead = nullptr);

    // バイパス（個別プラグイン）
    void setBypassed(int index, bool bypassed);
    bool isBypassed(int index) const;

    // 合計遅延サンプル（PDC 用）。現状は呼び出し側で参照のみ
    int getTotalLatencySamples() const;

    // 指定 SR / blockSize で既に prepareToPlay 済みか。モニター経路 (setMonitorChain) と
    // 書き出し経路 (renderOfflineRange・別スレッド) が、同設定での二重 prepare
    // (プラグイン状態リセット = グリッチ) を避けるための軽量判定。prepared* は書き出しの
    // バックグラウンドスレッドからも読まれる (prepareToPlay の message thread 書き込みと競合し
    // うる) ため atomic 化してある (torn read = UB を回避)。
    bool isPreparedFor(double sr, int bs) const noexcept
    {
        return preparedSampleRate.load() == sr && preparedBlockSize.load() == bs;
    }

    // 変更通知
    std::function<void()> onChainChanged;

private:
    struct Slot
    {
        std::unique_ptr<juce::AudioPluginInstance> plugin;
        bool bypassed { false };
    };

    juce::OwnedArray<Slot>      slots;
    // processBlock() (audio thread) でも取得するブロッキングロック。プラグインの
    // 追加/削除/並べ替えは UI 側の低頻度操作のため実用上問題ないが、原則としては妥協
    // (#R-10)。lock-free 化は Phase 2 の課題 (AudioEngine の playbackLock と同様)。
    mutable juce::CriticalSection chainLock;
    // chainLock 下でスロット構成を変えるたびに slots.size() を release-store する。
    // audio thread はこれを acquire-load してロック無しで「処理対象あり」を判定する。
    std::atomic<int> activePluginCount { 0 };
    // 書き出しの別スレッドからも isPreparedFor 経由で読まれるため atomic (torn read 回避)。
    std::atomic<double> preparedSampleRate { 0.0 };
    std::atomic<int>    preparedBlockSize  { 0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PluginChain)
};
