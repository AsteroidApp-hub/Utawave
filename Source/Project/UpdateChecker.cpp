// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

#include "UpdateChecker.h"
#include "SafeText.h"

namespace
{
    // 接続タイムアウト。到達不能なサーバーへの接続でスレッドが長く居座らないよう抑えめに。
    constexpr int kConnectTimeoutMs = 5000;

    // version JSON へ GET し、ボディ文字列を返す (失敗時は空)。
    juce::String httpGet(const juce::String& url)
    {
        juce::URL u(url);
        const auto opts = juce::URL::InputStreamOptions(juce::URL::ParameterHandling::inAddress)
                              .withConnectionTimeoutMs(kConnectTimeoutMs)
                              .withExtraHeaders("User-Agent: Utawave\r\n"
                                                "Accept: application/json");
        if (auto in = u.createInputStream(opts))
        {
            // 接続後の read がハングするサーバー対策: 時間とサイズの上限付きで読む
            // (InputStreamOptions のタイムアウトは接続段階のみで read には効かない)
            constexpr int    kReadTimeoutMs = 10000;
            constexpr size_t kMaxBytes      = 64 * 1024;   // version JSON は数百バイト想定
            const auto deadline = juce::Time::getMillisecondCounterHiRes() + kReadTimeoutMs;
            juce::MemoryBlock mb;
            char buf[4096];
            while (mb.getSize() < kMaxBytes
                   && juce::Time::getMillisecondCounterHiRes() < deadline)
            {
                const int n = in->read(buf, (int)sizeof(buf));
                if (n <= 0) break;   // EOF / エラー
                mb.append(buf, (size_t)n);
            }
            return SafeText::fromUtf8OrEmpty(mb);
        }
        return {};
    }

    // バージョン文字列を数値コンポーネントの配列にする ("0.10.0" → {0,10,0})。
    std::vector<int> versionParts(const juce::String& v)
    {
        std::vector<int> parts;
        const auto core = UpdateChecker::normaliseVersion(v);
        if (core.isEmpty()) return parts;

        juce::StringArray toks;
        toks.addTokens(core, ".", "");
        for (auto& t : toks)
            parts.push_back(t.getIntValue());
        return parts;
    }
}

void UpdateChecker::check(juce::String versionUrl, juce::String fallbackPageUrl, CancelFlag cancel,
                          std::function<void(UpdateInfo, bool)> cb)
{
    // 引数はすべて値コピーされ、デタッチスレッドが所有する。
    // run 中に呼び出し元 (StartupComponent) のメンバへは一切触れない。
    juce::Thread::launch([versionUrl, fallbackPageUrl, cancel, cb = std::move(cb)]
    {
        const auto cancelled = [&cancel] { return cancel != nullptr && cancel->load(); };

        UpdateInfo info;
        bool ok = false;

        if (! cancelled())
        {
            info = parseVersionInfo(httpGet(versionUrl));
            if (info.pageUrl.isEmpty())
                info.pageUrl = fallbackPageUrl;   // JSON に url が無ければ既定のダウンロードページへ
            ok = info.version.isNotEmpty() && info.pageUrl.isNotEmpty();
        }

        if (cancelled())
            return;   // 呼び出し元が消えていれば結果は捨てる (callAsync すら投げない)

        // 結果はメッセージスレッドへ。cb は SafePointer で自身の生存を確認する想定。
        juce::MessageManager::callAsync([cb, info = std::move(info), ok]() mutable
        {
            if (cb)
                cb(std::move(info), ok);
        });
    });
}

UpdateInfo UpdateChecker::parseVersionInfo(const juce::String& jsonText)
{
    UpdateInfo info;

    const auto v = juce::JSON::parse(jsonText);
    if (auto* obj = v.getDynamicObject())
    {
        info.tag     = obj->getProperty("version").toString().trim();
        info.pageUrl = obj->getProperty("url").toString().trim();
        info.version = normaliseVersion(info.tag);
    }
    return info;
}

juce::String UpdateChecker::normaliseVersion(const juce::String& raw)
{
    const auto s = raw.trim();

    // 先頭の数字位置を探す ("v0.2.0" / "Utawave-v0.2.0" のような接頭辞を飛ばす)。
    int start = -1;
    for (int i = 0; i < s.length(); ++i)
        if (juce::CharacterFunctions::isDigit(s[i])) { start = i; break; }
    if (start < 0) return {};

    // 数字とドットの連続を取り出す ("0.2.0-beta" → "0.2.0")。
    int end = start;
    while (end < s.length()
           && (juce::CharacterFunctions::isDigit(s[end]) || s[end] == '.'))
        ++end;

    auto core = s.substring(start, end);
    while (core.endsWithChar('.'))           // 末尾ドットを除去 ("1.2." → "1.2")
        core = core.dropLastCharacters(1);
    return core;
}

bool UpdateChecker::isNewerVersion(const juce::String& latest, const juce::String& current)
{
    const auto a = versionParts(latest);
    const auto b = versionParts(current);

    if (a.empty()) return false;   // 最新版が解析不能なら通知しない
    if (b.empty()) return true;    // 現在版が不明なら最新を新しい扱い

    const size_t n = juce::jmax(a.size(), b.size());
    for (size_t i = 0; i < n; ++i)
    {
        const int ai = i < a.size() ? a[i] : 0;
        const int bi = i < b.size() ? b[i] : 0;
        if (ai != bi) return ai > bi;
    }
    return false;   // 同一バージョン
}

juce::String UpdateChecker::defaultVersionUrl()
{
    // 公式ビルドは CMake の UTAWAVE_VERSION_URL でビルド時に本番 URL を埋め込む
    // (AdService の UTAWAVE_AD_FEED_URL と同じ作法)。
   #if defined(UTAWAVE_VERSION_URL)
    return juce::String::fromUTF8(UTAWAVE_VERSION_URL);
   #else
    return "https://utawave.com/version.json";   // プレースホルダ (未稼働なら取得失敗 → 通知なし)
   #endif
}

juce::String UpdateChecker::defaultDownloadPageUrl()
{
   #if defined(UTAWAVE_DOWNLOAD_PAGE_URL)
    return juce::String::fromUTF8(UTAWAVE_DOWNLOAD_PAGE_URL);
   #else
    return "https://utawave.com/";               // プレースホルダ (公式サイトトップ)
   #endif
}

juce::String UpdateChecker::currentAppVersion()
{
    return JUCE_APPLICATION_VERSION_STRING;
}
