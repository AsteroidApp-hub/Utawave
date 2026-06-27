// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

#pragma once
#include <JuceHeader.h>

// 起動画面に表示する広告 1 件。
// JSON フィード ( { "ads": [ { ... } ] } ) の各要素に対応する。
struct Ad
{
    juce::String id;
    juce::String title;
    juce::String body;
    juce::String imageUrl;
    juce::String linkUrl;
    juce::String lang;    // "ja" / "en" / "zh-Hans" / "zh-Hant" / "ko" / 省略・"all" (=全言語向け)。言語フィルタに使う
    bool         pinned { false };  // true なら常に先頭固定 (シャッフル対象外・月 1 件の固定バナー)
    juce::Image  image;   // imageUrl から取得した画像 (取得失敗時は invalid)
};

// 広告フィード (JSON) を取得し、画像をダウンロードして結果を返すサービス。
// ネットワーク I/O はすべてバックグラウンドスレッドで行い、結果はメッセージスレッドへ戻す。
// インスタンスは持たず、すべて static 関数で完結する (呼び出し元の寿命管理を不要にするため)。
class AdService
{
public:
    // 表示する広告の最大件数。フィードがこれを超えても先頭からこの数だけ採用する。
    static constexpr int kMaxAds = 30;

    // 取得をキャンセルするための共有フラグ。呼び出し元 (AdPanel) と worker スレッドの双方が
    // 所有 (shared_ptr) し、true になると worker は以降の画像 DL / キャッシュ保存 / 結果配信を
    // 打ち切る。呼び出し元が取得完了前に破棄される時に立てることで、無駄な処理を早めに止める。
    using CancelFlag = std::shared_ptr<std::atomic<bool>>;

    // fetch() の結果区分。「広告 0 件の空フィード」と「取得失敗」を区別するための列挙
    // (両者を同じ扱いにすると、サーバーが意図的に空配信した時に接続エラー文言が出てしまう)。
    enum class FetchResult
    {
        HasAds,   // フィードを取得でき、表示できる広告が 1 件以上あった
        Empty,    // サーバーには到達できたが広告が 0 件 (= 今は掲載なし。エラーではない)
        Failed    // 接続 / 取得に失敗 (ネットワーク不通・応答なし・本文空・取得中のキャンセル)
    };

    // 非同期でフィードを取得する。完了時 (またはキャンセルされなかった時) に cb を
    // 「メッセージスレッド」で 1 回呼ぶ。lang ("ja"/"en"/"zh-Hans"/"zh-Hant"/"ko") をフィード URL に
    // 反映し、取得後もその言語で絞り込んでから画像 DL / キャッシュする。
    //   ads   : 取得できた広告 (HasAds 以外は空)
    //   result: 取得結果 (HasAds / Empty / Failed)。Empty と Failed を呼び出し側で出し分ける
    // バックグラウンドスレッドは feedUrl / lang / cacheDir / cancel / cb を値コピーして保持するので、
    // 呼び出し元が取得完了前に破棄されても安全 (cb 側で自身の生存を SafePointer 等で確認すること)。
    static void fetch(juce::String feedUrl, juce::String lang, juce::File cacheDir, CancelFlag cancel,
                      std::function<void(std::vector<Ad>, FetchResult)> cb);

    // ── ネットワーク非依存のテスト可能な純関数 ──
    // JSON 文字列を広告リストへパースする ( { "ads": [...] } もしくはトップレベル配列を受理)。
    static std::vector<Ad> parseAds(const juce::String& jsonText);

    // 表示用に順序をランダム化し (Fisher-Yates)、最大 count 件へ間引く。
    // 30 件取得できても起動ごとに違う 15 件を抜粋する、といった用途。
    // rng を引数で受けるのでテストで決定論的に検証できる (count < 0 で間引きなし)。
    static void shuffleAndCap(std::vector<Ad>& ads, int count, juce::Random& rng);

    // 表示用の並び替え: pinned 広告を配信順のまま先頭へ固定し、残りだけをシャッフルして
    // 全体を最大 maxCount 件に間引く (月 1 件の固定バナーを常にトップに出すため)。
    static void arrangeForDisplay(std::vector<Ad>& ads, int maxCount, juce::Random& rng);

    // ── 言語対応 (純関数) ──
    // 現在のアプリ言語コード (BCP-47: "ja" / "en" / "zh-Hans" / "zh-Hant" / "ko")。
    // Localisation の保存設定から決まる。公式サイト worker のロケールコードと一致させる。
    static juce::String languageCode();
    // フィード URL に言語を反映する。"{lang}" トークンがあれば置換、無ければ ?lang= を付与。
    static juce::String feedUrlForLanguage(const juce::String& baseUrl, const juce::String& lang);
    // ad.lang が lang に一致 (または空 / "all") のものだけ残し、最大 maxCount 件へ間引く。
    static std::vector<Ad> selectAdsForLanguage(std::vector<Ad> ads, const juce::String& lang, int maxCount);

    // ── ディスクキャッシュ (オフライン時に直近の広告を表示するため) ──
    // 言語別ファイル (ads_<lang>.json) に rawJson を、各画像を img_<hash>.img (PNG) に保存する。
    // loadCache は ads_<lang>.json をパースし、対応するキャッシュ画像があれば読み込む。
    static std::vector<Ad> loadCache(const juce::File& cacheDir, const juce::String& lang);
    static void            saveCache(const juce::File& cacheDir, const juce::String& lang,
                                     const juce::String& rawJson,
                                     const std::vector<Ad>& ads);

    // 広告キャッシュの保存先 ( ~/Library/Application Support/Utawave/AdCache )。
    static juce::File cacheDir();

    // ── 開発用 (デバッグサンプルモード) ──
    // UTAWAVE_ADS_DEBUG ビルドか (組み込みサンプル広告を表示するモード)。
    static bool debugSampleMode();
    // サーバー不要で見た目を確認するための組み込みサンプル広告 (画像は動的生成)。
    static std::vector<Ad> sampleAds();

    // 既定の広告フィード URL。本番の配信サーバーが用意できたらここを差し替える。
    // (サーバーが無い間は到達できず、起動画面には「未接続」案内が表示される)
    static juce::String defaultFeedUrl();

private:
    static juce::Image downloadImage(const juce::String& url, int timeoutMs);
    static juce::File  imageCacheFile(const juce::File& cacheDir, const juce::String& url);
};
