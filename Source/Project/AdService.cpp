// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

#include "AdService.h"
#include "../Localisation.h"

namespace
{
    // 接続タイムアウト。長すぎると到達不能なサーバーへの接続でスレッドが長く居座るため抑えめに。
    constexpr int kConnectTimeoutMs = 5000;

    juce::URL::InputStreamOptions streamOptions()
    {
        return juce::URL::InputStreamOptions(juce::URL::ParameterHandling::inAddress)
                   .withConnectionTimeoutMs(kConnectTimeoutMs);
    }

    // 接続後の read がハングするサーバー対策: 時間とサイズの上限付きで読む
    // (InputStreamOptions のタイムアウトは接続段階のみで read には効かない)
    juce::MemoryBlock readBounded(juce::InputStream& in, size_t maxBytes, int maxMs)
    {
        const auto deadline = juce::Time::getMillisecondCounterHiRes() + maxMs;
        juce::MemoryBlock mb;
        char buf[8192];
        while (mb.getSize() < maxBytes
               && juce::Time::getMillisecondCounterHiRes() < deadline)
        {
            const int n = in.read(buf, (int)sizeof(buf));
            if (n <= 0) break;   // EOF / エラー
            mb.append(buf, (size_t)n);
        }
        return mb;
    }
}

juce::String AdService::defaultFeedUrl()
{
    // 公式ビルドは CMake の UTAWAVE_AD_FEED_URL でビルド時に本番 URL を埋め込む
    // (GitHub Actions で差し替える)。未指定 (公開ソースの通常ビルド) はプレースホルダ。
   #if defined(UTAWAVE_AD_FEED_URL)
    return juce::String::fromUTF8(UTAWAVE_AD_FEED_URL);
   #else
    return "https://utawave.com/ads/feed.json";
   #endif
}

juce::File AdService::cacheDir()
{
    auto root = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                    .getChildFile("Utawave")
                    .getChildFile("AdCache");
    root.createDirectory();
    return root;
}

bool AdService::debugSampleMode()
{
   #if defined(UTAWAVE_ADS_DEBUG) && (UTAWAVE_ADS_DEBUG)
    return true;
   #else
    return false;
   #endif
}

std::vector<Ad> AdService::sampleAds()
{
    // バナー画像を動的生成 (アセット同梱不要)。グラデーション + ラベルだけのダミー。
    auto banner = [](juce::Colour a, juce::Colour b, const juce::String& tag)
    {
        juce::Image img(juce::Image::ARGB, 480, 220, true);
        juce::Graphics g(img);
        g.setGradientFill(juce::ColourGradient(a, 0.0f, 0.0f, b, 480.0f, 220.0f, false));
        g.fillAll();
        g.setColour(juce::Colours::white.withAlpha(0.92f));
        g.setFont(juce::FontOptions(30.0f, juce::Font::bold));
        g.drawText(tag, img.getBounds(), juce::Justification::centred, false);
        return img;
    };

    // バナー + 見出しのカードを上限いっぱい (kMaxAds 件) 生成し、30 件時の見え方
    // (ドット・スクロール) を確認できるようにする。色相をずらしてバナーを色違いにする。
    std::vector<Ad> out;
    const int n = kMaxAds;
    for (int i = 0; i < n; ++i)
    {
        Ad ad;
        ad.id    = "sample-" + juce::String(i + 1);
        // 先頭 1 件は固定バナー (pinned) のサンプル。全言語向け (lang 空) にして両言語で確認できるように。
        if (i == 0)
        {
            ad.pinned = true;
            ad.lang   = "";
            ad.title  = juce::String::fromUTF8(u8"サンプル広告 1 (固定 pinned)");
        }
        else
        {
            // 言語を ja / en / 全言語 で循環させ、言語フィルタの確認に使えるようにする
            const char* langs[] = { "ja", "en", "" };
            ad.lang  = langs[i % 3];
            const juce::String tag = ad.lang.isEmpty() ? "all" : ad.lang;
            ad.title = juce::String::fromUTF8(u8"サンプル広告 ") + juce::String(i + 1) + " (" + tag + ")";
        }

        if (i < n - 1)   // 最後の 1 件だけ画像なし・リンクなし (レイアウト確認用)
        {
            const float hue1 = (float) ((i * 47) % 360)        / 360.0f;
            const float hue2 = (float) ((i * 47 + 28) % 360)   / 360.0f;
            ad.image   = banner(juce::Colour::fromHSV(hue1, 0.62f, 0.85f, 1.0f),
                                juce::Colour::fromHSV(hue2, 0.70f, 0.55f, 1.0f),
                                "AD " + juce::String(i + 1));
            ad.linkUrl = "https://example.com/" + juce::String(i + 1);
        }
        out.push_back(std::move(ad));
    }

    return out;
}

std::vector<Ad> AdService::parseAds(const juce::String& jsonText)
{
    std::vector<Ad> out;
    if (jsonText.isEmpty())
        return out;

    const juce::var root = juce::JSON::parse(jsonText);

    // 受理する形: { "ads": [ {...} ] }  または トップレベル配列 [ {...} ]
    juce::var adsVar;
    if (auto* obj = root.getDynamicObject())
        adsVar = obj->getProperty("ads");
    if (! adsVar.isArray())
        adsVar = root;

    auto* arr = adsVar.getArray();
    if (arr == nullptr)
        return out;

    for (const auto& item : *arr)
    {
        if ((int) out.size() >= kMaxAds)   // 上限 (kMaxAds 件) を超えたら打ち切る
            break;

        auto* o = item.getDynamicObject();
        if (o == nullptr)
            continue;

        Ad ad;
        ad.id       = o->getProperty("id").toString();
        ad.title    = o->getProperty("title").toString();
        ad.body     = o->getProperty("body").toString();
        ad.imageUrl = o->getProperty("imageUrl").toString();
        ad.linkUrl  = o->getProperty("linkUrl").toString();
        ad.lang     = o->getProperty("lang").toString();
        ad.pinned   = (bool) o->getProperty("pinned");

        // 表示するものが何も無い項目 (全フィールド空) は捨てる
        if (ad.title.isNotEmpty() || ad.body.isNotEmpty() || ad.imageUrl.isNotEmpty())
            out.push_back(std::move(ad));
    }
    return out;
}

void AdService::shuffleAndCap(std::vector<Ad>& ads, int count, juce::Random& rng)
{
    for (int i = (int) ads.size() - 1; i > 0; --i)
        std::swap(ads[(size_t) i], ads[(size_t) rng.nextInt(i + 1)]);

    if (count >= 0 && (int) ads.size() > count)
        ads.resize((size_t) count);
}

void AdService::arrangeForDisplay(std::vector<Ad>& ads, int maxCount, juce::Random& rng)
{
    std::vector<Ad> pinned, rest;
    pinned.reserve(ads.size());
    rest.reserve(ads.size());
    for (auto& ad : ads)
        (ad.pinned ? pinned : rest).push_back(std::move(ad));

    shuffleAndCap(rest, -1, rng);   // 残りだけシャッフル (間引きは結合後に全体へ)

    ads.clear();
    for (auto& ad : pinned) ads.push_back(std::move(ad));   // 固定は配信順のまま先頭
    for (auto& ad : rest)   ads.push_back(std::move(ad));

    if (maxCount >= 0 && (int) ads.size() > maxCount)
        ads.resize((size_t) maxCount);
}

juce::String AdService::languageCode()
{
    // 日本語のみ "ja"。それ以外 (English 等) は英語圏として "en"。
    return Localisation::getSavedLanguage() == Localisation::Language::Japanese ? "ja" : "en";
}

juce::String AdService::feedUrlForLanguage(const juce::String& baseUrl, const juce::String& lang)
{
    if (baseUrl.contains("{lang}"))
        return baseUrl.replace("{lang}", lang);   // 言語別ファイル: .../ads/{lang}.json

    return juce::URL(baseUrl).withParameter("lang", lang).toString(true);   // ?lang=xx を付与
}

std::vector<Ad> AdService::selectAdsForLanguage(std::vector<Ad> ads, const juce::String& lang, int maxCount)
{
    std::vector<Ad> out;
    for (auto& ad : ads)
    {
        const auto l = ad.lang.trim().toLowerCase();
        if (l.isEmpty() || l == "all" || l == lang)   // 現在言語 or 全言語向け
        {
            out.push_back(std::move(ad));
            if (maxCount >= 0 && (int) out.size() >= maxCount)
                break;
        }
    }
    return out;
}

juce::Image AdService::downloadImage(const juce::String& url, int timeoutMs)
{
    if (url.isEmpty())
        return {};

    juce::URL u(url);
    if (auto in = u.createInputStream(
            juce::URL::InputStreamOptions(juce::URL::ParameterHandling::inAddress)
                .withConnectionTimeoutMs(timeoutMs)))
    {
        // 画像は最大 5MB / 15 秒で打ち切り (バナー想定。巨大ファイル/ストールへの防御)
        const auto mb = readBounded(*in, 5 * 1024 * 1024, 15000);
        if (mb.getSize() > 0)
            return juce::ImageFileFormat::loadFrom(mb.getData(), mb.getSize());
    }
    return {};
}

juce::File AdService::imageCacheFile(const juce::File& dir, const juce::String& url)
{
    return dir.getChildFile("img_" + juce::String::toHexString(url.hashCode64()) + ".img");
}

namespace
{
    // 言語別キャッシュ JSON ( ads_<lang>.json )。lang を安全な英数字に丸める。
    juce::File adsJsonFile(const juce::File& dir, const juce::String& lang)
    {
        const auto safe = lang.retainCharacters("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789");
        return dir.getChildFile("ads_" + (safe.isEmpty() ? juce::String("en") : safe) + ".json");
    }
}

void AdService::saveCache(const juce::File& dir, const juce::String& lang,
                          const juce::String& rawJson,
                          const std::vector<Ad>& ads)
{
    dir.createDirectory();
    adsJsonFile(dir, lang).replaceWithText(rawJson);

    for (const auto& ad : ads)
    {
        if (ad.image.isValid() && ad.imageUrl.isNotEmpty())
        {
            auto f = imageCacheFile(dir, ad.imageUrl);
            f.deleteFile();
            if (auto os = f.createOutputStream())
            {
                juce::PNGImageFormat png;
                png.writeImageToStream(ad.image, *os);
            }
        }
    }
}

std::vector<Ad> AdService::loadCache(const juce::File& dir, const juce::String& lang)
{
    auto jf = adsJsonFile(dir, lang);
    if (! jf.existsAsFile())
        return {};

    auto ads = parseAds(jf.loadFileAsString());
    for (auto& ad : ads)
    {
        if (ad.imageUrl.isEmpty())
            continue;

        auto imgF = imageCacheFile(dir, ad.imageUrl);
        if (imgF.existsAsFile())
        {
            juce::FileInputStream is(imgF);
            if (is.openedOk())
                ad.image = juce::ImageFileFormat::loadFrom(is);
        }
    }
    return ads;
}

void AdService::fetch(juce::String feedUrl, juce::String lang, juce::File dir, CancelFlag cancel,
                      std::function<void(std::vector<Ad>, FetchResult)> cb)
{
    // feedUrl / lang / dir / cancel / cb は値コピーされ、デタッチスレッドが所有する。
    // run 中に呼び出し元 (StartupComponent / AdPanel) のメンバへは一切触れない。
    juce::Thread::launch([feedUrl, lang, dir, cancel, cb = std::move(cb)]
    {
        const auto cancelled = [&cancel] { return cancel != nullptr && cancel->load(); };

        std::vector<Ad> ads;
        FetchResult result = FetchResult::Failed;

        juce::URL feed(feedUrlForLanguage(feedUrl, lang));
        if (! cancelled())
        {
            if (auto in = feed.createInputStream(streamOptions()))
            {
                // フィード JSON は最大 1MB / 10 秒で打ち切り
                const juce::String json = readBounded(*in, 1024 * 1024, 10000).toString();

                if (json.isNotEmpty())   // 本文を読めた = サーバーに到達できた (Empty/HasAds を判定)
                {
                    // 現在言語で絞り込んでから画像 DL / キャッシュ (不要言語の画像を落とさない)
                    ads    = selectAdsForLanguage(parseAds(json), lang, kMaxAds);
                    result = ads.empty() ? FetchResult::Empty : FetchResult::HasAds;

                    if (result == FetchResult::HasAds && ! cancelled())
                    {
                        for (auto& ad : ads)
                        {
                            if (cancelled()) { result = FetchResult::Failed; break; }   // 破棄されたら画像 DL を打ち切る
                            if (ad.imageUrl.isNotEmpty())
                                ad.image = downloadImage(ad.imageUrl, kConnectTimeoutMs);
                        }

                        if (result == FetchResult::HasAds && ! cancelled())
                            saveCache(dir, lang, json, ads);
                    }
                    else if (result == FetchResult::Empty && ! cancelled())
                    {
                        // サーバーが「今は広告なし」を返したら直近のキャッシュを破棄する。
                        // 最新の配信を権威とし、次回オフライン時に古い広告が蘇らないようにする。
                        adsJsonFile(dir, lang).deleteFile();
                    }
                }
            }
        }

        if (cancelled())
            return;   // 呼び出し元が消えていれば結果は捨てる (callAsync すら投げない)

        // 結果はメッセージスレッドへ。cb は SafePointer で自身の生存を確認する想定。
        juce::MessageManager::callAsync([cb, ads = std::move(ads), result]() mutable
        {
            if (cb)
                cb(std::move(ads), result);
        });
    });
}
