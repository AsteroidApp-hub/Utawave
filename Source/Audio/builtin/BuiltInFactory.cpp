// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

#include "BuiltInFactory.h"
#include "BuiltInEQ.h"
#include "BuiltInCompressor.h"
#include "BuiltInDeEsser.h"
#include "BuiltInGate.h"
#include "BuiltInMaximizer.h"
#include "BuiltInDelay.h"
#include "BuiltInReverb.h"
#include "../../Localisation.h"

namespace BuiltInFactory
{

namespace
{
    struct Entry
    {
        const char* identifier;
        const char* nameKey;   // メニュー表示名の tr() キー (各エフェクトの getName() と一致させる)
        std::unique_ptr<BuiltInEffect> (*make)();
    };

    // 並び順 = メニューの並び順。エフェクトを増やすときはここに 1 行足す。
    const Entry kEntries[] =
    {
        { "utawave.eq",      u8"EQ",         [] { return std::unique_ptr<BuiltInEffect>(new BuiltInEQ()); } },
        { "utawave.comp",    u8"コンプ",     [] { return std::unique_ptr<BuiltInEffect>(new BuiltInCompressor()); } },
        { "utawave.deesser", u8"ディエッサー", [] { return std::unique_ptr<BuiltInEffect>(new BuiltInDeEsser()); } },
        { "utawave.gate",    u8"ゲート",     [] { return std::unique_ptr<BuiltInEffect>(new BuiltInGate()); } },
        { "utawave.maximizer", u8"マキシマイザー", [] { return std::unique_ptr<BuiltInEffect>(new BuiltInMaximizer()); } },
        { "utawave.delay",   u8"ディレイ",   [] { return std::unique_ptr<BuiltInEffect>(new BuiltInDelay()); } },
        { "utawave.reverb",  u8"リバーブ",   [] { return std::unique_ptr<BuiltInEffect>(new BuiltInReverb()); } },
    };
    constexpr int kNumEntries = (int) (sizeof(kEntries) / sizeof(kEntries[0]));
}

std::unique_ptr<BuiltInEffect> create(const juce::String& identifier)
{
    for (const auto& e : kEntries)
        if (identifier == e.identifier)
            return e.make();
    return nullptr;
}

std::unique_ptr<juce::AudioPluginInstance> createFromIdentifierString(const juce::String& idStr)
{
    // 各内蔵エフェクトの PluginDescription を組み立て、createIdentifierString() の一致で判定する
    // (uniqueId / fileOrIdentifier は固定なので識別子は安定)。
    for (const auto& e : kEntries)
    {
        auto fx = e.make();
        if (fx && fx->getPluginDescription().createIdentifierString() == idStr)
            return fx;
    }
    return nullptr;
}

std::unique_ptr<juce::AudioPluginInstance> createFromMenuId(int menuId)
{
    const int idx = menuId - kMenuIdBase - 1;
    if (idx < 0 || idx >= kNumEntries) return nullptr;
    return kEntries[idx].make();
}

void appendMenu(juce::PopupMenu& builtinSubMenu)
{
    // 表示名は nameKey を tr() するだけ (DSP 実体を作らない)。
    for (int i = 0; i < kNumEntries; ++i)
        builtinSubMenu.addItem(kMenuIdBase + 1 + i, tr(kEntries[i].nameKey));
}

}  // namespace BuiltInFactory
