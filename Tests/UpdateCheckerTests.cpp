// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

#include <JuceHeader.h>
#include "../Source/Project/UpdateChecker.h"

// UpdateChecker のネットワーク非依存な純関数 (parseVersionInfo / normaliseVersion /
// isNewerVersion) を検証する。check() の通信経路はユニットテスト対象外。
// 取得元は公式サイトの version JSON: { "version": "...", "url": "..." } (url は任意)。
class UpdateCheckerTests : public juce::UnitTest
{
public:
    UpdateCheckerTests() : juce::UnitTest("UpdateChecker") {}

    void runTest() override
    {
        beginTest("parseVersionInfo: well-formed version JSON");
        {
            const juce::String json = R"({
              "version": "v0.2.0",
              "url": "https://utawave.com/download"
            })";
            auto info = UpdateChecker::parseVersionInfo(json);
            expectEquals(info.tag,     juce::String("v0.2.0"));
            expectEquals(info.version, juce::String("0.2.0"));
            expectEquals(info.pageUrl, juce::String("https://utawave.com/download"));
        }

        beginTest("parseVersionInfo: url is optional, unknown fields ignored");
        {
            // url 無し → pageUrl 空 (check() が fallback を入れる)。余分なフィールドは無視。
            auto info = UpdateChecker::parseVersionInfo(
                R"({ "version": "0.3.1", "notes": "bugfix", "minOs": "12" })");
            expectEquals(info.tag,     juce::String("0.3.1"));
            expectEquals(info.version, juce::String("0.3.1"));
            expect(info.pageUrl.isEmpty(), "no url field -> empty pageUrl");
        }

        beginTest("parseVersionInfo: missing version");
        {
            auto info = UpdateChecker::parseVersionInfo(R"({ "url": "https://x.example/" })");
            expect(info.tag.isEmpty(),     "no version field");
            expect(info.version.isEmpty(), "no comparable version");
        }

        beginTest("parseVersionInfo: malformed / non-object");
        {
            expect(UpdateChecker::parseVersionInfo("not json").tag.isEmpty());
            expect(UpdateChecker::parseVersionInfo("").tag.isEmpty());
            expect(UpdateChecker::parseVersionInfo("[ 1, 2, 3 ]").tag.isEmpty());
            expect(UpdateChecker::parseVersionInfo(R"("0.2.0")").tag.isEmpty(),
                   "bare string is not an object");
        }

        beginTest("normaliseVersion: strips prefixes and suffixes");
        {
            expectEquals(UpdateChecker::normaliseVersion("v0.2.0"),          juce::String("0.2.0"));
            expectEquals(UpdateChecker::normaliseVersion("0.2.0"),           juce::String("0.2.0"));
            expectEquals(UpdateChecker::normaliseVersion("V1.0"),            juce::String("1.0"));
            expectEquals(UpdateChecker::normaliseVersion("Utawave-v1.2.3"),  juce::String("1.2.3"));
            expectEquals(UpdateChecker::normaliseVersion("0.2.0-beta"),      juce::String("0.2.0"));
            expectEquals(UpdateChecker::normaliseVersion("1.2."),            juce::String("1.2"));
            expectEquals(UpdateChecker::normaliseVersion("  v3.4.5  "),      juce::String("3.4.5"));
            expectEquals(UpdateChecker::normaliseVersion("beta"),           juce::String());
            expectEquals(UpdateChecker::normaliseVersion(""),               juce::String());
        }

        beginTest("isNewerVersion: strictly-newer semantics");
        {
            expect(  UpdateChecker::isNewerVersion("0.2.0", "0.1.0"), "patch/minor bump newer");
            expect(! UpdateChecker::isNewerVersion("0.1.0", "0.1.0"), "same is not newer");
            expect(! UpdateChecker::isNewerVersion("0.1.0", "0.2.0"), "older is not newer");
            expect(  UpdateChecker::isNewerVersion("1.0.0", "0.9.9"), "major bump newer");
            expect(  UpdateChecker::isNewerVersion("0.1.1", "0.1.0"), "patch bump newer");
        }

        beginTest("isNewerVersion: numeric (not lexical) comparison");
        {
            expect(  UpdateChecker::isNewerVersion("0.10.0", "0.9.0"), "0.10 > 0.9 numerically");
            expect(! UpdateChecker::isNewerVersion("0.9.0", "0.10.0"), "0.9 < 0.10 numerically");
        }

        beginTest("isNewerVersion: prefix tolerance and unequal lengths");
        {
            expect(  UpdateChecker::isNewerVersion("v0.2.0", "0.1.0"), "leading v normalised");
            expect(! UpdateChecker::isNewerVersion("0.1.0", "0.1"),    "0.1.0 == 0.1 (zero pad)");
            expect(  UpdateChecker::isNewerVersion("0.2", "0.1.9"),    "0.2 > 0.1.9");
        }

        beginTest("isNewerVersion: guards on unparsable input");
        {
            expect(! UpdateChecker::isNewerVersion("garbage", "0.1.0"), "unparsable latest -> no notify");
            expect(! UpdateChecker::isNewerVersion("", "0.1.0"),        "empty latest -> no notify");
            expect(  UpdateChecker::isNewerVersion("0.2.0", ""),        "empty current -> treat as newer");
        }
    }
};

static UpdateCheckerTests updateCheckerTests;
