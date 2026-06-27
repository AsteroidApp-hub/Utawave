// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

#include <JuceHeader.h>
#include "../Source/Project/AdService.h"

// AdService の JSON パースとディスクキャッシュ往復を、ネットワーク非依存で検証する。
// (fetch() の通信経路はユニットテスト対象外。parseAds / loadCache / saveCache の純ロジックのみ)
class AdServiceTests : public juce::UnitTest
{
public:
    AdServiceTests() : juce::UnitTest("AdService") {}

    void runTest() override
    {
        beginTest("parseAds: well-formed { ads: [...] }");
        {
            const juce::String json = R"({
              "ads": [
                { "id": "a1", "title": "Title A", "body": "Body A",
                  "imageUrl": "https://example.com/a.png", "linkUrl": "https://example.com/a" },
                { "id": "a2", "title": "Title B", "body": "Body B",
                  "linkUrl": "https://example.com/b" }
              ]
            })";
            auto ads = AdService::parseAds(json);
            expectEquals((int) ads.size(), 2, "two ads parsed");
            expectEquals(ads[0].id,       juce::String("a1"));
            expectEquals(ads[0].title,    juce::String("Title A"));
            expectEquals(ads[0].body,     juce::String("Body A"));
            expectEquals(ads[0].imageUrl, juce::String("https://example.com/a.png"));
            expectEquals(ads[0].linkUrl,  juce::String("https://example.com/a"));
            // 2件目は imageUrl 無し → 空文字
            expectEquals(ads[1].imageUrl, juce::String());
            expectEquals(ads[1].linkUrl,  juce::String("https://example.com/b"));
        }

        beginTest("parseAds: top-level array fallback");
        {
            const juce::String json = R"([ { "title": "Only title" } ])";
            auto ads = AdService::parseAds(json);
            expectEquals((int) ads.size(), 1, "top-level array accepted");
            expectEquals(ads[0].title, juce::String("Only title"));
        }

        beginTest("parseAds: order preserved");
        {
            const juce::String json =
                R"({ "ads": [ {"title":"1"}, {"title":"2"}, {"title":"3"} ] })";
            auto ads = AdService::parseAds(json);
            expectEquals((int) ads.size(), 3);
            expectEquals(ads[0].title, juce::String("1"));
            expectEquals(ads[1].title, juce::String("2"));
            expectEquals(ads[2].title, juce::String("3"));
        }

        beginTest("parseAds: drops items with no displayable content");
        {
            // id だけ (title/body/imageUrl すべて空) の項目は捨てる
            const juce::String json =
                R"({ "ads": [ {"id":"x"}, {"title":"keep"}, {"id":"y","imageUrl":"u"} ] })";
            auto ads = AdService::parseAds(json);
            expectEquals((int) ads.size(), 2, "id-only item dropped");
            expectEquals(ads[0].title, juce::String("keep"));
            expectEquals(ads[1].imageUrl, juce::String("u"));
        }

        beginTest("parseAds: empty / invalid / non-ad JSON -> empty");
        {
            expect(AdService::parseAds("").empty(),              "empty string");
            expect(AdService::parseAds("not json {{{").empty(),  "garbage");
            expect(AdService::parseAds(R"({"foo": 1})").empty(), "object without ads array");
            expect(AdService::parseAds(R"({"ads": 5})").empty(), "ads not an array");
        }

        beginTest("cache round-trip preserves fields (no images)");
        {
            auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                           .getChildFile("UtawaveAdCacheTest");
            dir.deleteRecursively();
            dir.createDirectory();

            const juce::String json = R"({
              "ads": [
                { "id": "c1", "title": "Cached", "body": "Stored",
                  "imageUrl": "https://example.com/c.png", "linkUrl": "https://example.com/c" }
              ]
            })";
            auto original = AdService::parseAds(json);
            AdService::saveCache(dir, "ja", json, original);

            expect(dir.getChildFile("ads_ja.json").existsAsFile(), "ads_ja.json written");

            auto loaded = AdService::loadCache(dir, "ja");
            expectEquals((int) loaded.size(), 1, "one ad loaded from cache");
            expectEquals(loaded[0].id,      juce::String("c1"));
            expectEquals(loaded[0].title,   juce::String("Cached"));
            expectEquals(loaded[0].body,    juce::String("Stored"));
            expectEquals(loaded[0].linkUrl, juce::String("https://example.com/c"));
            // 画像はダウンロードしていない → キャッシュ画像も無く invalid のまま
            expect(! loaded[0].image.isValid(), "no cached image present");

            dir.deleteRecursively();
        }

        beginTest("loadCache: missing dir -> empty");
        {
            auto missing = juce::File::getSpecialLocation(juce::File::tempDirectory)
                               .getChildFile("UtawaveAdCacheTest_DoesNotExist");
            missing.deleteRecursively();
            expect(AdService::loadCache(missing, "ja").empty(), "no cache dir -> empty");
        }

        beginTest("selectAdsForLanguage: keeps current language + language-neutral");
        {
            auto mk = [](const char* id, const char* lang)
            { Ad a; a.id = id; a.title = "t"; a.lang = lang; return a; };

            std::vector<Ad> in { mk("a","ja"), mk("b","en"), mk("c",""),
                                 mk("d","ja"), mk("e","all"), mk("f","EN") };

            auto ja = AdService::selectAdsForLanguage(in, "ja", -1);
            // ja: a, d (ja) + c (空) + e (all) → 4 件、b/f (en) は除外
            expectEquals((int) ja.size(), 4, "ja keeps ja + empty + all");
            for (auto& a : ja) expect(a.id != "b" && a.id != "f", "no en ad in ja set");

            auto en = AdService::selectAdsForLanguage(in, "en", -1);
            // en: b, f (en・大文字小文字無視) + c (空) + e (all) → 4 件
            expectEquals((int) en.size(), 4, "en keeps en (case-insensitive) + empty + all");
            bool hasF = false; for (auto& a : en) if (a.id == "f") hasF = true;
            expect(hasF, "EN (uppercase) matched as en");

            // 件数上限
            auto capped = AdService::selectAdsForLanguage(in, "ja", 2);
            expectEquals((int) capped.size(), 2, "capped to maxCount");
        }

        beginTest("selectAdsForLanguage: zh-Hans / zh-Hant / ko (BCP-47, case-insensitive)");
        {
            auto mk = [](const char* id, const char* lang)
            { Ad a; a.id = id; a.title = "t"; a.lang = lang; return a; };

            // 追加言語 + 全言語 + 別言語が混在したフィード
            std::vector<Ad> in { mk("hans","zh-Hans"), mk("hant","zh-Hant"), mk("ko","ko"),
                                 mk("all",""), mk("ja","ja"), mk("hans2","ZH-HANS") };

            // languageCode() が返す BCP-47 コード (大小混在) を渡しても一致する
            auto hans = AdService::selectAdsForLanguage(in, "zh-Hans", -1);
            // zh-Hans 2 件 (大文字違いの hans2 含む) + all → 3 件。hant/ko/ja は除外
            expectEquals((int) hans.size(), 3, "zh-Hans keeps zh-Hans (any case) + all");
            for (auto& a : hans)
                expect(a.id != "hant" && a.id != "ko" && a.id != "ja", "no other-language ad in zh-Hans set");
            bool hasHans2 = false; for (auto& a : hans) if (a.id == "hans2") hasHans2 = true;
            expect(hasHans2, "ZH-HANS (uppercase) matched zh-Hans");

            auto hant = AdService::selectAdsForLanguage(in, "zh-Hant", -1);
            expectEquals((int) hant.size(), 2, "zh-Hant keeps zh-Hant + all");   // hant + all

            auto ko = AdService::selectAdsForLanguage(in, "ko", -1);
            expectEquals((int) ko.size(), 2, "ko keeps ko + all");               // ko + all
            for (auto& a : ko)
                expect(a.id != "hans" && a.id != "hant", "no Chinese ad in ko set");
        }

        beginTest("feedUrlForLanguage: token substitution and query fallback");
        {
            expectEquals(AdService::feedUrlForLanguage("https://x/ads/{lang}.json", "ja"),
                         juce::String("https://x/ads/ja.json"), "{lang} token replaced");

            auto q = AdService::feedUrlForLanguage("https://x/ads/feed.json", "en");
            expect(q.contains("lang=en"), "lang query appended when no token");
        }

        beginTest("sampleAds: built-in debug feed is valid");
        {
            auto s = AdService::sampleAds();
            expectEquals((int) s.size(), AdService::kMaxAds, "sample feed fills up to kMaxAds");
            // 各広告は表示物 (title / image のいずれか) を持つ
            for (auto& ad : s)
                expect(ad.title.isNotEmpty() || ad.image.isValid(),
                       "sample ad has displayable content");
            // 先頭は固定バナー (pinned) + 画像 + リンクつき、末尾はテキストのみ
            expect(s.front().pinned,                "first sample is the pinned banner");
            expect(s.front().image.isValid(),       "first sample has a generated image");
            expect(s.front().linkUrl.isNotEmpty(),  "first sample is clickable");
            expect(! s.back().image.isValid(),      "last sample is text-only");
            expect(s.back().linkUrl.isEmpty(),      "last sample is non-clickable");
        }

        beginTest("shuffleAndCap: random subset of fixed size, no duplicates");
        {
            std::vector<Ad> in;
            for (int i = 0; i < 30; ++i) { Ad a; a.id = juce::String(i); a.title = "t"; in.push_back(a); }

            juce::Random rng (12345);
            auto v = in;
            AdService::shuffleAndCap(v, 15, rng);

            expectEquals((int) v.size(), 15, "capped to 15");
            juce::StringArray seen;
            for (auto& a : v)
            {
                expect(! seen.contains(a.id), "no duplicate in subset");
                seen.add(a.id);
                const int id = a.id.getIntValue();
                expect(id >= 0 && id < 30, "subset element comes from the input set");
            }
        }

        beginTest("shuffleAndCap: fewer than cap keeps all");
        {
            std::vector<Ad> in;
            for (int i = 0; i < 8; ++i) { Ad a; a.id = juce::String(i); in.push_back(a); }
            juce::Random rng (7);
            AdService::shuffleAndCap(in, 15, rng);
            expectEquals((int) in.size(), 8, "kept all when count is below cap");
        }

        beginTest("parseAds: reads the pinned flag");
        {
            auto a = AdService::parseAds(R"({ "ads": [
                { "title": "x", "pinned": true },
                { "title": "y" } ] })");
            expectEquals((int) a.size(), 2);
            expect(a[0].pinned,   "pinned:true parsed");
            expect(! a[1].pinned, "absent pinned defaults to false");
        }

        beginTest("arrangeForDisplay: pinned first (feed order), rest shuffled, capped");
        {
            auto mk = [](juce::String id, bool pin)
            { Ad a; a.id = std::move(id); a.title = "t"; a.pinned = pin; return a; };

            std::vector<Ad> in;
            in.push_back(mk("p1", true));
            in.push_back(mk("p2", true));
            for (int i = 0; i < 20; ++i) in.push_back(mk("r" + juce::String(i), false));

            juce::Random rng (99);
            AdService::arrangeForDisplay(in, 15, rng);

            expectEquals((int) in.size(), 15, "capped to 15");
            // 固定 2 件が配信順のまま先頭
            expectEquals(in[0].id, juce::String("p1"), "first pinned kept at index 0");
            expectEquals(in[1].id, juce::String("p2"), "second pinned kept at index 1");
            expect(in[0].pinned && in[1].pinned, "leading items are the pinned ones");
            // 3 件目以降は非 pinned
            for (int i = 2; i < (int) in.size(); ++i)
                expect(! in[(size_t) i].pinned, "no pinned ad after the leading block");

            // pinned が無い場合は単純なシャッフル + キャップ (shuffleAndCap 相当)
            std::vector<Ad> none;
            for (int i = 0; i < 20; ++i) none.push_back(mk("n" + juce::String(i), false));
            juce::Random rng2 (5);
            AdService::arrangeForDisplay(none, 15, rng2);
            expectEquals((int) none.size(), 15, "no-pinned still caps to 15");
        }

        beginTest("parseAds: caps the feed at kMaxAds");
        {
            juce::String j = "{ \"ads\": [";
            const int over = AdService::kMaxAds + 12;
            for (int i = 0; i < over; ++i)
            {
                if (i > 0) j += ",";
                j += "{\"title\":\"t" + juce::String(i) + "\"}";
            }
            j += "] }";
            expectEquals((int) AdService::parseAds(j).size(), AdService::kMaxAds,
                         "no more than kMaxAds parsed");
        }
    }
};

static AdServiceTests adServiceTests;
