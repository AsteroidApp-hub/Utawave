// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

#pragma once
#include <JuceHeader.h>
#include "BuiltInEffect.h"

/**
    内蔵エフェクト (EQ / コンプ / リバーブ) の生成ファクトリ。

    - INS スロット追加メニューへの「Utawave」サブメニュー注入
    - メニュー選択 ID → 生成
    - .uta ロード時の identifier 文字列 → 生成 (ProjectManager から呼ぶ)
    すべてここに集約し、エフェクトを増やすときはこの 1 ファイルの kEntries に足すだけにする。
*/
namespace BuiltInFactory
{
    // KnownPluginList::addToMenu の項目 ID (index ベースの小さい値) と衝突しない予約域
    static constexpr int kMenuIdBase = 0x5000000;

    inline const char* formatName() noexcept { return "Utawave"; }
    inline bool isBuiltInFormat(const juce::String& f) { return f == formatName(); }

    // identifier (言語非依存・固定) で生成する。未知なら nullptr。
    std::unique_ptr<BuiltInEffect> create(const juce::String& identifier);

    // .uta の "id" 属性 (PluginDescription::createIdentifierString()) から生成する。
    std::unique_ptr<juce::AudioPluginInstance> createFromIdentifierString(const juce::String& idStr);

    // 追加メニューの選択 ID から生成する。内蔵域でなければ nullptr (呼び出し側が VST 経路へ続行)。
    std::unique_ptr<juce::AudioPluginInstance> createFromMenuId(int menuId);

    // 追加メニューに内蔵エフェクト項目を並べる (各表示名は tr() 済み)
    void appendMenu(juce::PopupMenu& builtinSubMenu);
}
