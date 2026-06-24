// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

#include "AdPanel.h"
#include "../Localisation.h"

AdPanel::AdPanel()
{
    setOpaque(false);
    setWantsKeyboardFocus(false);
}

AdPanel::~AdPanel()
{
    // 取得中なら worker に以降の処理 (画像 DL / キャッシュ保存 / 結果配信) を打ち切らせる
    if (cancelFlag != nullptr)
        cancelFlag->store(true);
    stopTimer();
}

void AdPanel::load()
{
    if (fetching)   // 多重取得を防止 (連打 / 再読み込み中の再要求を無視)
        return;

    // 開発用 (UTAWAVE_ADS_DEBUG): サーバー無しで組み込みサンプル広告を表示し、通信はしない
    if (AdService::debugSampleMode())
    {
        setAds(AdService::sampleAds());
        return;
    }

    const auto lang = AdService::languageCode();   // "ja" / "en"

    // まず「最新取得」を待つ (キャッシュは即出ししない)。取得できなければ onFetchDone で
    // キャッシュ (前回の広告) にフォールバックする。
    setState(State::Loading);

    fetching   = true;
    cancelFlag = std::make_shared<std::atomic<bool>>(false);
    AdService::fetch(AdService::defaultFeedUrl(), lang, AdService::cacheDir(), cancelFlag,
        [safe = juce::Component::SafePointer<AdPanel>(this)]
        (std::vector<Ad> fetched, AdService::FetchResult result) mutable
        {
            if (auto* p = safe.getComponent())
                p->onFetchDone(std::move(fetched), result);
        });
}

void AdPanel::onFetchDone(std::vector<Ad> fetched, AdService::FetchResult result)
{
    fetching = false;

    // 最新が取れたらそれを表示 (言語フィルタ後に空なら setAds が NoAds へ)
    if (result == AdService::FetchResult::HasAds)
    {
        setAds(std::move(fetched));
        return;
    }

    // サーバーに到達できたが広告 0 件 = 今は掲載なし。エラーではなく中立表示にする
    // (キャッシュは fetch 側で破棄済みなのでフォールバックしない)。
    if (result == AdService::FetchResult::Empty)
    {
        setState(State::NoAds);
        return;
    }

    // Failed: 取得に失敗した時だけキャッシュ (前回の広告) にフォールバック
    auto cached = AdService::loadCache(AdService::cacheDir(), AdService::languageCode());
    if (! cached.empty())
        setAds(std::move(cached));
    else
        setState(State::NoData);   // 接続エラー文言 + 再読み込み
}

void AdPanel::setState(State s)
{
    state = s;
    if (s != State::ShowingAds)
        stopTimer();
    repaint();
}

void AdPanel::setAds(std::vector<Ad> newAds)
{
    // 現在のアプリ言語に合う広告だけ残す (混在フィード / サンプルにも対応)。
    // ja は日本語向け + 全言語向け、en (日本語以外) は英語圏向け + 全言語向けを表示。
    auto filtered = AdService::selectAdsForLanguage(std::move(newAds),
                                                    AdService::languageCode(), AdService::kMaxAds);
    // データはあったが現在言語向けが 0 件 = 掲載なしと同じ中立表示 (接続エラーではない)
    if (filtered.empty()) { setState(State::NoAds); return; }

    ads = std::move(filtered);

    // pinned を先頭固定、残りをランダム化して最大 kMaxVisible(15) 件へ抜粋する
    // (30 件取得できても毎回違う並び。固定バナーだけは常にトップ)。高分解能ティックで毎回違う並び。
    {
        juce::Random rng (juce::Time::getHighResolutionTicks());
        AdService::arrangeForDisplay(ads, kMaxVisible, rng);
    }

    currentIndex  = 0;
    transitioning = false;
    phase         = 0.0;
    autoCountdown = kAutoIntervalSec;
    state         = State::ShowingAds;

    if (scrollable()) startTimerHz(kRefreshHz);
    else              stopTimer();
    repaint();
}

//==============================================================================
// スクロール
//==============================================================================
bool AdPanel::scrollable() const { return (int) ads.size() >= 3; }

int AdPanel::wrapIndex(int i) const
{
    const int n = (int) ads.size();
    if (n <= 0) return 0;
    return ((i % n) + n) % n;
}

double AdPanel::scrollPos() const
{
    return (double) currentIndex + (transitioning ? phase * (double) slideDir : 0.0);
}

void AdPanel::startScroll(int dir)
{
    if (! scrollable() || transitioning) return;
    slideDir = dir;
    phase = 0.0;
    transitioning = true;
    if (! isTimerRunning()) startTimerHz(kRefreshHz);
    repaint();
}

void AdPanel::timerCallback()
{
    const double dt = 1.0 / (double) kRefreshHz;

    if (transitioning)
    {
        phase += dt / kTransitionSec;
        if (phase >= 1.0)
        {
            currentIndex  = wrapIndex(currentIndex + slideDir);
            transitioning = false;
            phase         = 0.0;
            autoCountdown = kAutoIntervalSec;
        }
        repaint();
        return;
    }

    if (scrollable())
    {
        if (! isMouseOver)   // ホバー中は自動送りを止めて読みやすく
        {
            autoCountdown -= dt;
            if (autoCountdown <= 0.0)
                startScroll(+1);
        }
    }
    else
    {
        stopTimer();
    }
}

//==============================================================================
// レイアウト
//==============================================================================
juce::Rectangle<int> AdPanel::cardView() const
{
    // ドット帯は件数に依らず常に下部へ確保する。こうするとカード領域の高さ (= カードサイズ) が
    // 件数で変わらず、1 件→複数件でカードがリサイズして驚くことがない。
    return getLocalBounds().reduced(kPad).withTrimmedBottom(kDotBarH);
}

juce::Rectangle<int> AdPanel::dotBar() const
{
    return getLocalBounds().reduced(kPad).removeFromBottom(kDotBarH);
}

AdPanel::Layout AdPanel::layout() const
{
    Layout L;
    L.view = cardView();
    const int vh = L.view.getHeight();

    // カード高さは常に「2 枚フル + 上下に kPeek の覗き」の幾何で算出する。件数が増えても 1 枚
    // あたりの大きさが一定になり、1 件→複数件でカードがリサイズして驚かない。
    //   vh = peek + gap + cardH + gap + cardH + gap + peek
    L.cardH = juce::jmax(40, (vh - 3 * kCardGap - 2 * kPeek) / 2);
    L.unit  = L.cardH + kCardGap;

    // 先頭カードは常に上寄せ (少数でも中央に浮かせず最上部へ詰める)。スクロールする 3 件以上
    // のときだけ、上端に前カードの覗き (peek) 分を空ける。2 件以下はスクロールしないので 0。
    L.y0 = scrollable() ? (kPeek + kCardGap) : 0;
    return L;
}

juce::Rectangle<int> AdPanel::cardRect(int slot, double sp, const Layout& L) const
{
    const int y = L.view.getY() + L.y0 + juce::roundToInt(((double) slot - sp) * L.unit);
    return { L.view.getX(), y, L.view.getWidth(), L.cardH };
}

juce::Rectangle<int> AdPanel::upChevron() const
{
    auto v = cardView();
    return { v.getCentreX() - kChevron / 2, v.getY() + 2, kChevron, kChevron };
}

juce::Rectangle<int> AdPanel::downChevron() const
{
    auto v = cardView();
    return { v.getCentreX() - kChevron / 2, v.getBottom() - kChevron - 2, kChevron, kChevron };
}

std::vector<juce::Rectangle<int>> AdPanel::dotRects() const
{
    std::vector<juce::Rectangle<int>> out;
    const int n = (int) ads.size();
    if (n < 2) return out;

    auto bar = dotBar();
    int pitch = kDot + 5;
    const int maxW = juce::jmax(kDot, bar.getWidth() - 16);
    if ((n - 1) * pitch + kDot > maxW)                    // 件数が多い時は間隔を詰める
        pitch = juce::jmax(kDot + 2, (maxW - kDot) / juce::jmax(1, n - 1));

    const int totalW = (n - 1) * pitch + kDot;
    int x = bar.getCentreX() - totalW / 2;
    const int y = bar.getCentreY() - kDot / 2;
    for (int i = 0; i < n; ++i) { out.push_back({ x, y, kDot, kDot }); x += pitch; }
    return out;
}

juce::Rectangle<int> AdPanel::dotsBacking() const
{
    auto dots = dotRects();
    if (dots.empty()) return {};
    const auto first = dots.front();
    const auto last  = dots.back();
    return juce::Rectangle<int>(first.getX(), first.getY(),
                                last.getRight() - first.getX(), first.getHeight())
               .expanded(6, 4);
}

juce::Rectangle<int> AdPanel::retryHit() const
{
    auto c = getLocalBounds().getCentre();
    return juce::Rectangle<int>(0, 0, 140, 30).withCentre({ c.x, c.y + 30 });
}

int AdPanel::adIndexAt(juce::Point<int> p) const
{
    const auto L = layout();
    if (! L.view.contains(p)) return -1;

    const int n = (int) ads.size();
    if (n <= 0) return -1;
    if (n == 1) return cardRect(0, 0.0, L).contains(p) ? 0 : -1;
    if (n == 2)
    {
        if (cardRect(0, 0.0, L).contains(p)) return 0;
        if (cardRect(1, 0.0, L).contains(p)) return 1;
        return -1;
    }

    const double sp = scrollPos();
    for (int i = currentIndex - 2; i <= currentIndex + 3; ++i)
        if (cardRect(i, sp, L).contains(p)) return wrapIndex(i);
    return -1;
}

// マウス位置が、ある広告カードの「見出し帯」(下端 kTitleH・drawCard と同じ) 上にあれば、その広告
// index を返す。枠内では見出しが … で省略されるため、ここで全文ツールチップを出す判定に使う。
int AdPanel::titleAdIndexAt(juce::Point<int> p) const
{
    if (! cardView().contains(p)) return -1;
    const auto L = layout();
    const int n = (int) ads.size();

    auto overTitle = [&](int slot, double sp, int adIdx) -> bool
    {
        if (adIdx < 0 || adIdx >= n || ads[(size_t) adIdx].title.isEmpty()) return false;
        auto card = cardRect(slot, sp, L);
        return card.removeFromBottom(kTitleH).contains(p);   // 見出し帯のみ
    };

    if (n <= 0) return -1;
    if (n <= 2)
    {
        for (int s = 0; s < n; ++s)
            if (overTitle(s, 0.0, s)) return s;
        return -1;
    }
    const double sp = scrollPos();
    for (int i = currentIndex - 2; i <= currentIndex + 3; ++i)
    {
        const int idx = wrapIndex(i);
        if (overTitle(i, sp, idx)) return idx;
    }
    return -1;
}

juce::String AdPanel::getTooltip()
{
    if (state != State::ShowingAds) return {};
    const int idx = titleAdIndexAt(getMouseXYRelative());
    return (idx >= 0 && idx < (int) ads.size()) ? ads[(size_t) idx].title : juce::String();
}

//==============================================================================
// 描画
//==============================================================================
void AdPanel::drawCard(juce::Graphics& g, juce::Rectangle<int> rect, int adIndex)
{
    if (adIndex < 0 || adIndex >= (int) ads.size()) return;
    const auto& ad = ads[(size_t) adIndex];

    auto imgArea   = rect;
    const int titleH = ad.title.isNotEmpty() ? kTitleH : 0;
    auto titleArea = (titleH > 0) ? imgArea.removeFromBottom(titleH) : juce::Rectangle<int>();

    // バナー画像: 枠内に収める contain (アスペクト維持・見切れさせない)。枠と画像の比率が
    // 違うと余白が出るので、先に黒で塗ってレターボックス背景にする (端が浮かない / 暗い UI に馴染む)。
    if (ad.image.isValid() && imgArea.getHeight() > 0)
    {
        const int iw = ad.image.getWidth();
        const int ih = ad.image.getHeight();
        if (iw > 0 && ih > 0)
        {
            juce::Graphics::ScopedSaveState s(g);
            g.reduceClipRegion(imgArea);
            g.setColour(juce::Colours::black);
            g.fillRect(imgArea);
            const double scale = juce::jmin((double) imgArea.getWidth()  / (double) iw,
                                            (double) imgArea.getHeight() / (double) ih);
            const int dw = juce::jmax(1, juce::roundToInt(iw * scale));
            const int dh = juce::jmax(1, juce::roundToInt(ih * scale));
            auto dest = juce::Rectangle<int>(dw, dh).withCentre(imgArea.getCentre());
            g.drawImage(ad.image, dest.toFloat());
        }
    }
    else if (imgArea.getHeight() > 0)
    {
        g.setColour(juce::Colour(0xff303236));   // 画像なしの控えめな背景
        g.fillRect(imgArea);
    }

    // 見出し (1 行・はみ出しは省略)
    if (titleH > 0)
    {
        g.setColour(juce::Colours::white);
        g.setFont(juce::FontOptions(13.5f, juce::Font::bold));
        g.drawText(ad.title, titleArea.reduced(3, 0), juce::Justification::centredLeft, true);
    }
}

void AdPanel::paint(juce::Graphics& g)
{
    const auto full = getLocalBounds();

    if (state == State::Loading)
    {
        g.setColour(juce::Colour(0xff8a8f95));
        g.setFont(juce::FontOptions(13.0f));
        g.drawText(tr(u8"広告を読み込み中..."), full, juce::Justification::centred, true);
        return;
    }

    if (state == State::NoAds)
    {
        // サーバーには到達できたが広告 0 件。エラーではないので中立的な案内のみ
        // (赤系の警告色・再読み込みボタンは出さない)。
        g.setColour(juce::Colour(0xff8a8f95));
        g.setFont(juce::FontOptions(13.0f));
        g.drawFittedText(tr(u8"現在表示できる広告はありません"),
                         full.reduced(14), juce::Justification::centred, 2);
        return;
    }

    if (state == State::NoData)
    {
        auto area = full.reduced(14);
        const int cy = area.getCentreY();

        g.setColour(juce::Colour(0xffc9ced3));
        g.setFont(juce::FontOptions(13.5f, juce::Font::bold));
        g.drawFittedText(tr(u8"広告を読み込めませんでした"),
                         area.getX(), cy - 50, area.getWidth(), 22,
                         juce::Justification::centred, 1);

        g.setColour(juce::Colour(0xff8a8f95));
        g.setFont(juce::FontOptions(12.0f));
        g.drawFittedText(tr(u8"インターネット接続を確認してください"),
                         area.getX(), cy - 26, area.getWidth(), 34,
                         juce::Justification::centredTop, 2);

        auto rb = retryHit();
        g.setColour(juce::Colour(0xff2a78ff).withAlpha(hoverRetry ? 1.0f : 0.85f));
        g.fillRoundedRectangle(rb.toFloat(), 6.0f);
        g.setColour(juce::Colours::white);
        g.setFont(juce::FontOptions(12.5f, juce::Font::bold));
        g.drawText(tr(u8"再読み込み"), rb, juce::Justification::centred, false);
        return;
    }

    // ── ShowingAds ──
    const auto L = layout();
    const int n = (int) ads.size();
    {
        juce::Graphics::ScopedSaveState s(g);
        g.reduceClipRegion(L.view);

        if (n == 1)
        {
            drawCard(g, cardRect(0, 0.0, L), 0);
        }
        else if (n == 2)
        {
            drawCard(g, cardRect(0, 0.0, L), 0);
            drawCard(g, cardRect(1, 0.0, L), 1);
        }
        else
        {
            const double sp = scrollPos();
            for (int i = currentIndex - 2; i <= currentIndex + 3; ++i)
                drawCard(g, cardRect(i, sp, L), wrapIndex(i));
        }
    }

    // ページ位置ドット (下部・ホバー時のみ表示。件数と「いま見えている 2 件」を示す)
    if ((int) ads.size() >= 2 && isMouseOver)
    {
        const auto dots = dotRects();
        g.setColour(juce::Colours::black.withAlpha(0.28f));
        g.fillRoundedRectangle(dotsBacking().toFloat(), 5.0f);

        const int a0 = currentIndex;
        const int a1 = wrapIndex(currentIndex + 1);
        for (int i = 0; i < (int) dots.size(); ++i)
        {
            const bool active = (i == a0 || i == a1);   // 表示中の 2 件をハイライト
            g.setColour(active ? juce::Colour(0xff2a78ff)
                               : juce::Colours::white.withAlpha(hoverDot == i ? 0.85f : 0.40f));
            g.fillEllipse(dots[(size_t) i].toFloat());
        }
    }

    // 上下シェブロン (3 件以上 & ホバー時のみ)
    if (scrollable() && isMouseOver)
    {
        auto drawChev = [&g](juce::Rectangle<int> z, bool up, bool hot)
        {
            g.setColour(juce::Colours::black.withAlpha(hot ? 0.55f : 0.32f));
            g.fillEllipse(z.toFloat());
            auto c = z.toFloat().reduced(7.0f);
            juce::Path p;
            if (up) p.addTriangle(c.getCentreX(), c.getY(), c.getRight(), c.getBottom(), c.getX(), c.getBottom());
            else    p.addTriangle(c.getX(), c.getY(), c.getRight(), c.getY(), c.getCentreX(), c.getBottom());
            g.setColour(juce::Colours::white.withAlpha(hot ? 1.0f : 0.85f));
            g.fillPath(p);
        };
        drawChev(upChevron(),   true,  hoverUp);
        drawChev(downChevron(), false, hoverDown);
    }
}

//==============================================================================
// マウス操作
//==============================================================================
void AdPanel::mouseDown(const juce::MouseEvent& e)
{
    const auto p = e.getPosition();

    if (state == State::NoData)
    {
        if (retryHit().contains(p)) load();
        return;
    }
    if (state != State::ShowingAds) return;

    if (scrollable())
    {
        if (upChevron().expanded(3).contains(p))   { startScroll(-1); return; }
        if (downChevron().expanded(3).contains(p)) { startScroll(+1); return; }

        // ドットクリックでその広告へジャンプ
        const auto dots = dotRects();
        for (int i = 0; i < (int) dots.size(); ++i)
            if (dots[(size_t) i].expanded(4).contains(p))
            {
                currentIndex  = wrapIndex(i);
                transitioning = false;
                phase         = 0.0;
                autoCountdown = kAutoIntervalSec;
                repaint();
                return;
            }
    }

    if (transitioning) return;
    const int idx = adIndexAt(p);
    if (idx >= 0 && ads[(size_t) idx].linkUrl.isNotEmpty())
        juce::URL(ads[(size_t) idx].linkUrl).launchInDefaultBrowser();
}

void AdPanel::mouseMove(const juce::MouseEvent& e)
{
    const auto p = e.getPosition();
    bool up = false, dn = false, nr = false;
    int  ad = -1, dot = -1;

    if (state == State::NoData)
    {
        nr = retryHit().contains(p);
    }
    else if (state == State::ShowingAds)
    {
        if (scrollable())
        {
            up = upChevron().expanded(3).contains(p);
            dn = downChevron().expanded(3).contains(p);
            if (! up && ! dn)
            {
                const auto dots = dotRects();
                for (int i = 0; i < (int) dots.size(); ++i)
                    if (dots[(size_t) i].expanded(4).contains(p)) dot = i;
            }
        }
        if (! up && ! dn && dot < 0 && ! transitioning)
        {
            const int idx = adIndexAt(p);
            if (idx >= 0 && ads[(size_t) idx].linkUrl.isNotEmpty())
                ad = idx;
        }
    }

    if (up != hoverUp || dn != hoverDown || nr != hoverRetry || ad != hoverAd || dot != hoverDot)
    {
        hoverUp = up; hoverDown = dn; hoverRetry = nr; hoverAd = ad; hoverDot = dot;
        setMouseCursor((up || dn || nr || ad >= 0 || dot >= 0)
                           ? juce::MouseCursor::PointingHandCursor
                           : juce::MouseCursor::NormalCursor);
        repaint();
    }
}

void AdPanel::mouseEnter(const juce::MouseEvent& e)
{
    isMouseOver = true;
    repaint();          // シェブロンを出す
    mouseMove(e);
}

void AdPanel::mouseExit(const juce::MouseEvent&)
{
    isMouseOver = false;
    hoverUp = hoverDown = hoverRetry = false;
    hoverAd = hoverDot = -1;
    setMouseCursor(juce::MouseCursor::NormalCursor);
    repaint();
}

void AdPanel::mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails& w)
{
    if (state != State::ShowingAds || ! scrollable() || transitioning) return;
    if      (w.deltaY >  0.02f) startScroll(-1);
    else if (w.deltaY < -0.02f) startScroll(+1);
}
