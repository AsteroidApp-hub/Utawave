// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

#pragma once
#include <JuceHeader.h>

namespace Localisation
{
    enum class Language { Japanese, English, SimplifiedChinese, TraditionalChinese, Korean };

    // 指定言語の LocalisedStrings をインストールする。
    //  - Japanese: JUCE 標準 UI の英語文字列を日本語化 (アプリ文言は元の日本語のまま)
    //  - English : アプリの日本語キーを英語へ翻訳 (JUCE 標準文言は英語のまま)
    //  - 簡体/繁体/韓国: アプリの日本語キー + JUCE 標準英語キーを各言語へ翻訳
    void install(Language lang);

    // 後方互換 (= install(Language::Japanese))
    void installJapanese();

    // アプリ全体の言語設定の永続化 (~/Library/Utawave/language.txt)
    Language getSavedLanguage();
    void     saveLanguage(Language lang);

    // 言語表示名 (設定 UI 用)
    juce::String displayName(Language lang);
}

// アプリ UI 文言の翻訳ヘルパ。日本語をキーにして juce::translate に通す。
//   例: tr(u8"保存")  → 日本語モードでは「保存」、英語モードでは "Save"
inline juce::String tr(const char* utf8Japanese)
{
    return juce::translate(juce::String::fromUTF8(utf8Japanese));
}

// ショートカットを含む文言の修飾キー表記をプラットフォームに合わせる (表示直前に通す)。
// 文言キー・翻訳テーブルは Mac 表記 (Cmd / Option) のまま統一し、非 Mac では Ctrl / Alt
// に置換して表示する。キー名以外に Cmd/Option/Ctrl/Alt の語を含む文字列には使わないこと。
//   例: platformShortcutText(tr(u8"環境設定 (Cmd+,)"))  → Windows では「環境設定 (Ctrl+,)」
inline juce::String platformShortcutText(juce::String text)
{
   #if JUCE_MAC
    return text.replace("Ctrl", "Cmd").replace("Alt", "Option");
   #else
    return text.replace("Cmd", "Ctrl").replace("Option", "Alt");
   #endif
}
