// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

// Utawave — アプリ全体設定 (AppPreferences) のユニットテスト
//
// 対象は load のバリデーション (特に録音補正の手動オフセット recLatencyManualMs の
// ±300ms クランプ・改修 2026-07) と save→load の往復。
// 実ストア (~/Library/Application Support/Utawave/app_prefs.xml) を読み書きするため、
// AppStorageTests と同じ StoreGuard RAII で退避→終了時復元し、開発者の設定を壊さない。
// expect メッセージは ASCII。

#include <JuceHeader.h>
#include <cmath>
#include "../Source/Project/AppPreferences.h"

namespace
{

// テスト開始時にストアファイルを退避し、終了時に必ず復元する (無ければ削除)
struct StoreGuard
{
    juce::File file;
    bool       existed { false };
    juce::String content;

    explicit StoreGuard(juce::File f) : file(std::move(f))
    {
        existed = file.existsAsFile();
        if (existed) content = file.loadFileAsString();
    }
    ~StoreGuard()
    {
        if (existed) file.replaceWithText(content);
        else         file.deleteFile();
    }
};

class AppPreferencesTests : public juce::UnitTest
{
public:
    AppPreferencesTests() : juce::UnitTest("AppPreferences") {}

    static bool approxEq(double a, double b) { return std::abs(a - b) < 1e-9; }

    // ストアへ属性直書き (save を経由しない値で load の検証をするため)
    static void writeStore(const juce::String& attributes)
    {
        AppPreferences::getStoreFile()
            .replaceWithText("<Preferences " + attributes + "/>");
    }

    void runTest() override
    {
        StoreGuard guard(AppPreferences::getStoreFile());

        beginTest("load clamps recLatencyManualMs to +/-300 ms");
        {
            writeStore("recLatencyManualMs=\"500\"");
            expect(approxEq(AppPreferences::load().recLatencyManualMs, 300.0),
                   "over-range +500 clamps to +300");

            writeStore("recLatencyManualMs=\"-500\"");
            expect(approxEq(AppPreferences::load().recLatencyManualMs, -300.0),
                   "over-range -500 clamps to -300");

            // 旧上限 (±500 時代) に保存された値も次回 load で ±300 へ丸まる
            writeStore("recLatencyManualMs=\"480\"");
            expect(approxEq(AppPreferences::load().recLatencyManualMs, 300.0),
                   "legacy stored value clamps to +300");

            writeStore("recLatencyManualMs=\"120\"");
            expect(approxEq(AppPreferences::load().recLatencyManualMs, 120.0),
                   "in-range value passes through");

            writeStore("recLatencyManualMs=\"-300\"");
            expect(approxEq(AppPreferences::load().recLatencyManualMs, -300.0),
                   "boundary -300 passes through");
        }

        beginTest("load falls back to defaults when attributes are missing");
        {
            const AppPreferences def {};
            writeStore("showAds=\"0\"");   // 無関係な属性のみ
            const auto p = AppPreferences::load();
            expect(approxEq(p.recLatencyManualMs, def.recLatencyManualMs),
                   "missing manual offset -> default 0");
            expect(p.recLatencyAutoComp == def.recLatencyAutoComp,
                   "missing auto comp -> default true");

            // ストア自体が無い場合も既定
            AppPreferences::getStoreFile().deleteFile();
            expect(approxEq(AppPreferences::load().recLatencyManualMs, 0.0),
                   "no store file -> default 0");
        }

        beginTest("save/load round trip for latency comp settings");
        {
            AppPreferences p;
            p.recLatencyAutoComp = false;
            p.recLatencyManualMs = -37.0;
            expect(p.save(), "save succeeds");

            const auto q = AppPreferences::load();
            expect(q.recLatencyAutoComp == false, "auto comp round trips");
            expect(approxEq(q.recLatencyManualMs, -37.0), "manual offset round trips");
        }

        beginTest("save/load round trip for export-complete dialog toggle");
        {
            // 既定 true。false を保存 → load で false が保たれる (書き出し完了ダイアログの表示設定)
            expect(AppPreferences{}.showExportCompleteDialog, "default is true");
            AppPreferences p;
            p.showExportCompleteDialog = false;
            expect(p.save(), "save succeeds");
            expect(AppPreferences::load().showExportCompleteDialog == false,
                   "showExportCompleteDialog round trips false");
        }

        beginTest("save/load round trip for last-used project SR / bit depth");
        {
            // 既定: SR 未設定 (0) / ビット深度 32。作成後の値を保存 → load で引き継がれる
            expect(approxEq(AppPreferences{}.lastProjectSampleRate, 0.0), "default SR unset (0)");
            expect(AppPreferences{}.lastProjectBitDepth == 32, "default bit depth 32");
            AppPreferences p;
            p.lastProjectSampleRate = 48000.0;
            p.lastProjectBitDepth   = 24;
            p.lastProjectLocation   = "/tmp/UtawaveProjects";
            expect(p.save(), "save succeeds");
            const auto q = AppPreferences::load();
            expect(approxEq(q.lastProjectSampleRate, 48000.0), "last SR round trips (48000)");
            expect(q.lastProjectBitDepth == 24, "last bit depth round trips (24)");
            expect(q.lastProjectLocation == juce::String("/tmp/UtawaveProjects"),
                   "last project location round trips");
            // 空 (デフォルトへリセット) も往復する
            AppPreferences r;
            r.lastProjectLocation = {};
            expect(r.save(), "save empty succeeds");
            expect(AppPreferences::load().lastProjectLocation.isEmpty(),
                   "empty location round trips (reset to default)");
        }
    }
};

static AppPreferencesTests appPreferencesTests;

} // namespace
