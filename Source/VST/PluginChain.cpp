// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

#include "PluginChain.h"
#include <cmath>

// プラグイン出力の修復 (kMaxPluginSample 超 / NaN / Inf を含むブロックのみ呼ばれる)。
// NaN は 0 へ、有限の過大値は ±limit へクランプする。audio thread から呼ばれるため確保無し。
static void repairPluginBlock(float* d, int n, float limit) noexcept
{
    for (int i = 0; i < n; ++i)
    {
        const float v = d[i];
        d[i] = std::isfinite(v) ? juce::jlimit(-limit, limit, v) : 0.0f;
    }
}

// 不正サンプルの検出。NaN はあらゆる比較が false になるため !(abs(v) <= limit) で確実に拾う
// (SIMD の min/max は NaN を取りこぼすことがあり findMinAndMax では検出に使えない)。
static bool pluginBlockNeedsRepair(const float* d, int n, float limit) noexcept
{
    bool bad = false;
    for (int i = 0; i < n; ++i)
        bad |= !(std::abs(d[i]) <= limit);
    return bad;
}

PluginChain::PluginChain() = default;

PluginChain::~PluginChain()
{
    cancelPendingUpdate();
    {
        const juce::ScopedLock sl(chainLock);
        for (auto* s : slots)
            if (s && s->plugin)
                s->plugin->removeListener(this);
    }
    releaseResources();
}

void PluginChain::addPlugin(std::unique_ptr<juce::AudioPluginInstance> plugin)
{
    if (plugin == nullptr) return;

    // サイドチェイン等の非メインバスを無効化してから 2ch I/O に固定。
    // これをしないとコンソール系/ダイナミクス系プラグインがメイン以外のバッファを
    // 期待して null ポインタアクセスでクラッシュすることがある (bx_console 等)。
    // また、createPluginInstance 内部で既に preparedToPlay されているプラグインを
    // 再 prepareToPlay する際は releaseResources してから行う（Nectar 4 等で必須）。
    plugin->releaseResources();
    plugin->disableNonMainBuses();
    const auto [psr, pbs] = getPrepared();
    const double sr = psr > 0.0 ? psr : 48000.0;
    const int    bs = pbs > 0   ? pbs : 512;
    plugin->setPlayConfigDetails(2, 2, sr, bs);
    if (psr > 0.0 && pbs > 0)
        plugin->prepareToPlay(psr, pbs);
    plugin->addListener(this);   // レイテンシ変更 (latencyChanged) を購読

    auto* slot = new Slot();
    slot->plugin = std::move(plugin);
    {
        const juce::ScopedLock sl(chainLock);
        slots.add(slot);
        activePluginCount.store(slots.size(), std::memory_order_release);
    }
    if (onChainChanged) onChainChanged();
}

void PluginChain::insertPluginAt(int slotIdx, std::unique_ptr<juce::AudioPluginInstance> plugin)
{
    if (plugin == nullptr || slotIdx < 0) return;

    plugin->releaseResources();
    plugin->disableNonMainBuses();
    const auto [psr, pbs] = getPrepared();
    const double sr = psr > 0.0 ? psr : 48000.0;
    const int    bs = pbs > 0   ? pbs : 512;
    plugin->setPlayConfigDetails(2, 2, sr, bs);
    if (psr > 0.0 && pbs > 0)
        plugin->prepareToPlay(psr, pbs);
    plugin->addListener(this);   // レイテンシ変更 (latencyChanged) を購読

    {
        const juce::ScopedLock sl(chainLock);
        // slotIdx までの隙間を空スロットで埋める
        while (slots.size() <= slotIdx)
            slots.add(new Slot());

        auto* slot = slots[slotIdx];
        if (slot->plugin)
        {
            slot->plugin->removeListener(this);
            slot->plugin->releaseResources();   // 既存があれば差し替え
        }
        slot->plugin   = std::move(plugin);
        slot->bypassed = false;
        activePluginCount.store(slots.size(), std::memory_order_release);
    }
    if (onChainChanged) onChainChanged();
}

void PluginChain::removePlugin(int index)
{
    std::unique_ptr<juce::AudioPluginInstance> removedPlugin;
    {
        const juce::ScopedLock sl(chainLock);
        if (index < 0 || index >= slots.size()) return;
        auto* slot = slots[index];
        if (slot)
        {
            removedPlugin = std::move(slot->plugin);
            slot->bypassed = false;
        }
        // 末尾の空スロットを掃除
        while (slots.size() > 0
               && (slots.getLast()->plugin == nullptr))
            slots.removeLast();
        activePluginCount.store(slots.size(), std::memory_order_release);
    }
    if (removedPlugin)
    {
        removedPlugin->removeListener(this);
        removedPlugin->releaseResources();
    }
    if (onChainChanged) onChainChanged();
}

std::unique_ptr<juce::AudioPluginInstance> PluginChain::extractPlugin(int index)
{
    std::unique_ptr<juce::AudioPluginInstance> taken;
    {
        const juce::ScopedLock sl(chainLock);
        if (index < 0 || index >= slots.size()) return nullptr;
        if (auto* slot = slots[index])
        {
            taken = std::move(slot->plugin);
            slot->bypassed = false;
        }
        while (slots.size() > 0
               && (slots.getLast()->plugin == nullptr))
            slots.removeLast();
        activePluginCount.store(slots.size(), std::memory_order_release);
    }
    if (taken)
    {
        taken->removeListener(this);   // 移動先チェーンが自分のリスナーを付け直す
        taken->releaseResources();
    }
    if (onChainChanged) onChainChanged();
    return taken;
}

void PluginChain::swapSlots(int a, int b)
{
    {
        const juce::ScopedLock sl(chainLock);
        if (a == b) return;
        // 必要なら index まで slots を伸ばす
        const int maxIdx = juce::jmax(a, b);
        while (slots.size() <= maxIdx)
            slots.add(new Slot());
        auto* sa = slots[a];
        auto* sb = slots[b];
        if (sa && sb)
        {
            std::swap(sa->plugin,   sb->plugin);
            std::swap(sa->bypassed, sb->bypassed);
        }
        while (slots.size() > 0
               && (slots.getLast()->plugin == nullptr))
            slots.removeLast();
        activePluginCount.store(slots.size(), std::memory_order_release);
    }
    if (onChainChanged) onChainChanged();
}

void PluginChain::movePlugin(int from, int to)
{
    {
        const juce::ScopedLock sl(chainLock);
        if (from < 0 || from >= slots.size()) return;
        to = juce::jlimit(0, slots.size() - 1, to);
        slots.move(from, to);
        activePluginCount.store(slots.size(), std::memory_order_release);
    }
    if (onChainChanged) onChainChanged();
}

int PluginChain::getNumPlugins() const
{
    const juce::ScopedLock sl(chainLock);
    return slots.size();
}

juce::AudioPluginInstance* PluginChain::getPlugin(int index) const
{
    const juce::ScopedLock sl(chainLock);
    if (index < 0 || index >= slots.size()) return nullptr;
    return slots[index]->plugin.get();
}

// chainLock 保持下で呼ぶ 1 プラグインの prepare シーケンス (prepareToPlay / setOfflineRenderMode 共用)。
void PluginChain::prepareSlot(Slot* s, double sampleRate, int blockSize)
{
    if (s == nullptr || s->plugin == nullptr) return;
    s->plugin->releaseResources();      // 既存リソース解放
    s->plugin->disableNonMainBuses();   // サイドチェイン等を切る
    s->plugin->setPlayConfigDetails(2, 2, sampleRate, blockSize);
    s->plugin->prepareToPlay(sampleRate, blockSize);
}

void PluginChain::prepareToPlay(double sampleRate, int blockSize)
{
    setPrepared(sampleRate, blockSize);
    const juce::ScopedLock sl(chainLock);
    for (auto* s : slots)
        prepareSlot(s, sampleRate, blockSize);
}

void PluginChain::releaseResources()
{
    const juce::ScopedLock sl(chainLock);
    for (auto* s : slots)
        if (s && s->plugin)
            s->plugin->releaseResources();
    // prepared 済みフラグを落とす。これをしないと releaseResources 後も isPreparedFor が
    // stale な true を返し、次の processBlock が未 prepare のプラグインを叩いてクラッシュしうる。
    setPrepared(0.0, 0);
}

void PluginChain::setOfflineRenderMode(bool offline, double sampleRate, int blockSize)
{
    if (sampleRate <= 0.0 || blockSize <= 0) return;
    const juce::ScopedLock sl(chainLock);
    if (slots.isEmpty()) return;
    setPrepared(sampleRate, blockSize);
    for (auto* s : slots)
    {
        if (s == nullptr || s->plugin == nullptr) continue;
        s->plugin->setNonRealtime(offline);   // prepareToPlay より前に設定する (契約)
        prepareSlot(s, sampleRate, blockSize);
    }
}

void PluginChain::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi,
                               juce::AudioPlayHead* playHead)
{
    const juce::ScopedLock sl(chainLock);
    if (slots.isEmpty()) return;

    for (auto* s : slots)
    {
        if (s == nullptr || s->plugin == nullptr || s->bypassed) continue;

        // 再生位置情報を供給する。setPlayHead はポインタ代入のみ (audio thread から安全)。
        // 毎ブロック設定するのは再生中にプラグインが追加されても確実に行き渡らせるため。
        s->plugin->setPlayHead(playHead);

        // プラグインが要求するチャンネル数を確認 (disableNonMainBuses + setPlayConfigDetails で
        // 通常は 2ch だが、念のため安全策として最大値で確保)
        const int needed = juce::jmax(2, s->plugin->getTotalNumInputChannels(),
                                          s->plugin->getTotalNumOutputChannels());
        if (buffer.getNumChannels() < needed)
        {
            // ※ audio thread での setSize はアロケーションが起きうる。
            //   通常 needed == 2 で発生しない想定だが、保険として残す。
            const int oldCh = buffer.getNumChannels();
            buffer.setSize(needed, buffer.getNumSamples(), true, false, true);
            for (int c = oldCh; c < needed; ++c)
                buffer.clear(c, 0, buffer.getNumSamples());
        }
        s->plugin->processBlock(buffer, midi);

        // 出力ガード: プラグインが内部再構成中 (UI のモード切替等) に吐く未初期化バッファ由来の
        // 巨大値 / NaN / Inf を修復し、爆音の出力直行と後段の状態汚染を防ぐ。正常時は検出のみ。
        const int n = buffer.getNumSamples();
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        {
            float* d = buffer.getWritePointer(ch);
            if (pluginBlockNeedsRepair(d, n, kMaxPluginSample))
                repairPluginBlock(d, n, kMaxPluginSample);
        }
    }
}

void PluginChain::setBypassed(int index, bool bypassed)
{
    const juce::ScopedLock sl(chainLock);
    if (index < 0 || index >= slots.size()) return;
    slots[index]->bypassed = bypassed;
}

bool PluginChain::isBypassed(int index) const
{
    const juce::ScopedLock sl(chainLock);
    if (index < 0 || index >= slots.size()) return false;
    return slots[index]->bypassed;
}

int PluginChain::getTotalLatencySamples() const
{
    const juce::ScopedLock sl(chainLock);
    int total = 0;
    for (auto* s : slots)
        if (s && s->plugin && !s->bypassed)
            total += s->plugin->getLatencySamples();
    return total;
}
