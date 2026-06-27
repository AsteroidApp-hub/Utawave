// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

// Utawave — Localisation (多言語化) のユニットテスト
//
// アプリ文言は日本語をキーに tr(u8"...") → juce::translate で表示する。English モードでは
// englishTranslations テーブルを引く。テーブルのキーは末尾スペース・改行・エスケープまで
// 完全一致が要るため、ここで代表キーを固定して回帰 (キー drift / 訳抜け) を捕捉する。
//   ・install(English) で代表キーが英訳される (末尾/先頭スペース・埋め込み改行を含む)
//   ・未訳キーは日本語キーのままフォールバックする
//   ・install(Japanese) ではアプリキーは素通り (元の日本語)
//   ・displayName
// install は process グローバルな setCurrentMappings を呼ぶため、終了時に Japanese へ戻す。
// 永続化 (language.txt) はユーザ設定ファイルを触るので本テストでは呼ばない。
// expect は ASCII (メッセージのみ。u8 リテラルのキーは日本語可)。

#include <JuceHeader.h>
#include "../Source/Localisation.h"

namespace
{
class LocalisationTests : public juce::UnitTest
{
public:
    LocalisationTests() : juce::UnitTest("Localisation") {}

    void runTest() override
    {
        testEnglishTable();
        testFallback();
        testJapanesePassThrough();
        testCjkTables();
        testDisplayName();

        // グローバル mappings を既定 (Japanese) に戻して後続テストへ影響させない
        Localisation::install(Localisation::Language::Japanese);
    }

    // ── 追加言語 (簡体 / 繁体 / 韓国): 代表キーが各言語へ訳される ──
    // キーは機械生成で englishTranslations と完全一致 (空白/改行込み) を保証済み。
    // ここでは各テーブルが install され代表キーを引けること + 連結断片の末尾スペース保持を固定する。
    void testCjkTables()
    {
        beginTest("install(SimplifiedChinese): representative keys translate");
        Localisation::install(Localisation::Language::SimplifiedChinese);
        expect(tr(u8"保存") == juce::String::fromUTF8(u8"保存"), "zh-Hans save");
        expect(tr(u8"録音") == juce::String::fromUTF8(u8"录音"), "zh-Hans record");
        expect(tr(u8"プラグイン管理") == juce::String::fromUTF8(u8"插件管理"), "zh-Hans menu");
        expect(tr(u8" (コピー)") == juce::String::fromUTF8(u8" (副本)"), "zh-Hans leading-space fragment");

        beginTest("install(TraditionalChinese): representative keys translate");
        Localisation::install(Localisation::Language::TraditionalChinese);
        expect(tr(u8"保存") == juce::String::fromUTF8(u8"儲存"), "zh-Hant save");
        expect(tr(u8"書き出し") == juce::String::fromUTF8(u8"匯出"), "zh-Hant export");
        expect(tr(u8"プラグイン管理") == juce::String::fromUTF8(u8"外掛程式管理"), "zh-Hant menu");

        beginTest("install(Korean): representative keys translate");
        Localisation::install(Localisation::Language::Korean);
        expect(tr(u8"保存") == juce::String::fromUTF8(u8"저장"), "ko save");
        expect(tr(u8"録音") == juce::String::fromUTF8(u8"녹음"), "ko record");
        expect(tr(u8"書き出し") == juce::String::fromUTF8(u8"내보내기"), "ko export");
        expect(tr(u8"テンポ: ") == juce::String::fromUTF8(u8"템포: "), "ko trailing-space fragment");

        // 未訳キーは日本語キーへフォールバック (英語と同じ契約)
        const char* missing = u8"このキーは翻訳テーブルに存在しない98765";
        expect(tr(missing) == juce::String::fromUTF8(missing), "ko missing key falls back");
    }

    // ── English テーブル: 代表キーが正しく英訳される (空白/改行/エスケープ込み) ──
    void testEnglishTable()
    {
        beginTest("install(English): representative keys translate exactly");
        Localisation::install(Localisation::Language::English);

        expect(tr(u8"保存") == "Save", "plain key");
        expect(tr(u8"プラグイン管理") == "Plugin Manager", "menu key");
        expect(tr(u8"クロスフェードを描く") == "Create Crossfade", "edit menu key");
        expect(tr(u8"トラックを複製") == "Duplicate Track", "track menu key");
        // 末尾スペースまで一致が要る連結断片
        expect(tr(u8"テンポ: ") == "Tempo: ", "key with a trailing space");
        // 先頭スペースの断片 (複製サフィックス)
        expect(tr(u8" (コピー)") == " (copy)", "key with a leading space");
        // 埋め込み改行 (テーブルは \\n、ソースは実 LF) が一致する
        expect(tr(u8"このプロジェクトには未保存の変更があります。\n保存しますか?")
                   == "This project has unsaved changes.\nDo you want to save?",
               "key with an embedded newline");
    }

    // ── 未訳キーは日本語キーのままフォールバック ──
    void testFallback()
    {
        beginTest("untranslated keys fall back to the Japanese key itself");
        Localisation::install(Localisation::Language::English);
        const char* missing = u8"このキーは翻訳テーブルに存在しない12345";
        expect(tr(missing) == juce::String::fromUTF8(missing),
               "missing key returns the key unchanged");
    }

    // ── Japanese モードではアプリキーは素通り (元の日本語) ──
    void testJapanesePassThrough()
    {
        beginTest("install(Japanese): app keys pass through unchanged");
        Localisation::install(Localisation::Language::Japanese);
        expect(tr(u8"保存") == juce::String::fromUTF8(u8"保存"),
               "Japanese mode returns the original Japanese key");
        expect(tr(u8"プラグイン管理") == juce::String::fromUTF8(u8"プラグイン管理"),
               "Japanese mode passes app key through");
    }

    // ── displayName ──
    void testDisplayName()
    {
        beginTest("displayName returns the language label");
        expect(Localisation::displayName(Localisation::Language::English) == "English",
               "English label");
        expect(Localisation::displayName(Localisation::Language::Japanese)
                   == juce::String::fromUTF8(u8"日本語"),
               "Japanese label");
        expect(Localisation::displayName(Localisation::Language::SimplifiedChinese)
                   == juce::String::fromUTF8(u8"简体中文"), "zh-Hans label");
        expect(Localisation::displayName(Localisation::Language::TraditionalChinese)
                   == juce::String::fromUTF8(u8"繁體中文"), "zh-Hant label");
        expect(Localisation::displayName(Localisation::Language::Korean)
                   == juce::String::fromUTF8(u8"한국어"), "ko label");
    }
};

static LocalisationTests localisationTests;
}
