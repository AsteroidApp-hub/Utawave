// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

#pragma once
#include <JuceHeader.h>
#include "../Project/AdService.h"

// 起動画面の「最近使ったプロジェクト」横に並ぶ広告枠。
// 広告は **バナー画像 + 見出し** のシンプルなカード。**常に 2 件を表示**し、上下に隣の広告が
// 少し見切れて覗く「ピーク」付きの**連続縦スクロール**で流す (自動送り + 手動: 上下シェブロン /
// マウスホイール)。広告クリックで既定ブラウザを開く。データが無い時は案内文を表示する。
// 描画用の枠 (角丸カード) は親 (StartupComponent) が描くため、本コンポーネントは透過。
class AdPanel : public juce::Component,
                private juce::Timer
{
public:
    AdPanel();
    ~AdPanel() override;

    // ディスクキャッシュを即時表示しつつ、バックグラウンドで最新フィードを取得する。
    void load();

    void paint(juce::Graphics&) override;

    void mouseDown(const juce::MouseEvent&) override;
    void mouseMove(const juce::MouseEvent&) override;
    void mouseEnter(const juce::MouseEvent&) override;
    void mouseExit(const juce::MouseEvent&) override;
    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

private:
    // NoAds : サーバーに到達できたが広告が 0 件 (中立表示。エラーではない)
    // NoData: 取得に失敗しキャッシュも無い (接続エラー文言 + 再読み込みボタン)
    enum class State { Loading, ShowingAds, NoAds, NoData };

    void setState(State);
    void setAds(std::vector<Ad>);
    void onFetchDone(std::vector<Ad>, AdService::FetchResult);

    void timerCallback() override;
    void startScroll(int dir);    // +1: 次へ (上へスクロール) / -1: 前へ (下へスクロール)
    int  wrapIndex(int i) const;
    bool scrollable() const;      // 3 件以上なら連続スクロール (2 件以下は固定表示)
    double scrollPos() const;     // 連続スクロール位置 (currentIndex + phase*dir)

    // レイアウト (パネル座標)
    struct Layout { juce::Rectangle<int> view; int cardH; int unit; int y0; };
    Layout layout() const;
    juce::Rectangle<int> cardView() const;       // カード描画領域 (下部のドット帯を除いた範囲)
    juce::Rectangle<int> dotBar() const;         // 下部のドット用帯
    juce::Rectangle<int> cardRect(int slot, double scrollPos, const Layout&) const;
    juce::Rectangle<int> upChevron() const;
    juce::Rectangle<int> downChevron() const;
    juce::Rectangle<int> retryHit() const;
    int  adIndexAt(juce::Point<int>) const;     // 可視カード上の広告 index (無ければ -1)

    // ページ位置ドット (下部に横並び・件数と現在位置を示す。ホバー時のみ表示)
    std::vector<juce::Rectangle<int>> dotRects() const;
    juce::Rectangle<int> dotsBacking() const;

    void drawCard(juce::Graphics&, juce::Rectangle<int> rect, int adIndex);

    State state { State::Loading };
    std::vector<Ad> ads;
    int currentIndex { 0 };

    // スクロールアニメーション
    bool   transitioning { false };
    int    slideDir { 1 };
    double phase { 0.0 };            // 0..1
    double autoCountdown { 0.0 };    // 自動送りまでの残り秒
    bool   isMouseOver { false };

    // ホバー状態 (見た目とカーソルの切替用)
    bool hoverUp { false }, hoverDown { false }, hoverRetry { false };
    int  hoverAd { -1 };
    int  hoverDot { -1 };

    // 取得の多重起動防止 + 破棄時のキャンセル (いずれもメッセージスレッドで操作)
    bool fetching { false };
    AdService::CancelFlag cancelFlag;

    static constexpr int    kMaxVisible  = 15;   // 同時に表示する最大件数 (多すぎると見づらいので抜粋)
    static constexpr int    kPad         = 6;    // パネル内側の余白
    static constexpr int    kCardGap     = 8;    // カード間の隙間
    static constexpr int    kPeek        = 22;   // 上下に覗かせる隣カードの高さ
    static constexpr int    kChevron     = 22;   // 上下シェブロンのサイズ
    static constexpr int    kDot         = 6;    // ページドットの直径
    static constexpr int    kDotBarH     = 16;   // 下部のドット帯の高さ
    static constexpr int    kTitleH      = 24;   // 見出し帯の高さ
    static constexpr int    kRefreshHz   = 30;
    static constexpr double kTransitionSec   = 0.5;
    static constexpr double kAutoIntervalSec = 4.0;   // 自動送り間隔 (短め)

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AdPanel)
};
