// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

#pragma once
#include <JuceHeader.h>

/**
    描画済みのテキスト Image を (text, font, colour, size, justify) でキーに持つキャッシュ。
    JUCE 8 の drawText は内部で HarfBuzz シェーピング + フォントキャッシュ参照を毎回行うため、
    固定ラベル（バー番号、Peak、VU、Tempo、トラック名、クリップ名等）が頻繁に再描画されると
    支配的なコストになる。一度 Image に焼いて blit すれば次回からほぼゼロコスト。

    ⚠ 将来の JUCE バージョンアップ時 (9 / 10 等) には、HarfBuzz 統合の改善で
    この workaround 自体が不要になっている可能性があります。アップグレード時には
    本キャッシュを通す/通さないでフレームレート計測を行い、効果を再検証してください。
    効果が確認できない場合は本キャッシュを撤去して直接 drawText に戻すのが望ましい。
*/
class TextImageCache
{
public:
    static TextImageCache& getInstance()
    {
        static TextImageCache instance;
        return instance;
    }

    /// 指定領域にテキストを描画。最初の呼び出しでレンダリングして Image に焼き、以降は blit。
    ///
    /// ⚠ Image は bounds 幅ではなく「テキストの実幅」までに縮めて焼く (キーの w も同様)。
    /// クリップ名は bounds = クリップのピクセル幅で渡ってくるため、素直に bounds 幅で確保すると
    /// ズームインで 1 エントリ数 MB〜数十 MB の Image になり、さらにズーム中は幅が毎イベント
    /// 変わる = 毎回新キーで積もる。件数上限 (2048) はバイト無制限なので、トラックが多い
    /// プロジェクトで拡大縮小を繰り返すとメモリが際限なく増える (iPad 実機で jetsam 落ちを確認)。
    /// テキスト実幅に縮めれば、(1) エントリは高々テキストサイズ (数十 KB)、(2) bounds 幅が
    /// テキストより広い限りズームで幅が変わってもキーが変わらない (キャッシュヒットし続ける)。
    void drawText(juce::Graphics& g,
                  const juce::String& text,
                  juce::Rectangle<int> bounds,
                  const juce::Font& font,
                  juce::Colour colour,
                  juce::Justification justify)
    {
        if (text.isEmpty() || bounds.getWidth() <= 0 || bounds.getHeight() <= 0)
            return;

        // テキスト実幅 (シェーピングが重いので (text, font) 単位でキャッシュ)
        const int textW = measureTextWidth(text, font);
        const int effW  = juce::jmin(bounds.getWidth(), textW + 4);   // +4: AA/丸めの安全余白
        if (effW <= 0) return;

        Key k;
        k.text       = text;
        k.fontHeight = font.getHeight();
        k.fontStyle  = font.getStyleFlags();
        k.colourArgb = colour.getARGB();
        k.w          = effW;
        k.h          = bounds.getHeight();
        k.justify    = justify.getFlags();

        juce::Image img;
        {
            const juce::ScopedLock sl(lock);
            auto it = cache.find(k);
            if (it != cache.end())
                img = it->second;
        }

        if (!img.isValid())
        {
            img = juce::Image(juce::Image::ARGB, k.w, k.h, true);
            juce::Graphics ig(img);
            ig.setFont(font);
            ig.setColour(colour);
            // 横は Image 幅にちょうど入れる (bounds が広い場合の横位置は blit 側で合わせる)。
            // bounds がテキストより狭い時は従来どおり ellipsis
            ig.drawText(text, 0, 0, k.w, k.h, justify, true);

            const juce::ScopedLock sl(lock);
            // 上限管理: 件数 + 総バイト数の両方 (幅広テキストの安全網)
            if (auto it = cache.find(k); it != cache.end())
                cacheBytes -= imageBytes(it->second);   // 同キーを並行生成した場合の二重計上防止
            cacheBytes += imageBytes(img);
            while (!cache.empty() && (cache.size() >= maxEntries || cacheBytes > maxBytes))
            {
                cacheBytes -= imageBytes(cache.begin()->second);
                cache.erase(cache.begin());
            }
            cache[k] = img;
        }

        // Image はテキスト実幅までしか無いので、bounds 内の横位置を justification で合わせる
        int drawX = bounds.getX();
        if ((k.justify & juce::Justification::horizontallyCentred) != 0)
            drawX = bounds.getX() + (bounds.getWidth() - effW) / 2;
        else if ((k.justify & juce::Justification::right) != 0)
            drawX = bounds.getRight() - effW;
        g.drawImageAt(img, drawX, bounds.getY());
    }

    /// 上記オーバーロード (x,y,w,h 直接指定)
    void drawText(juce::Graphics& g,
                  const juce::String& text,
                  int x, int y, int w, int h,
                  const juce::Font& font,
                  juce::Colour colour,
                  juce::Justification justify)
    {
        drawText(g, text, { x, y, w, h }, font, colour, justify);
    }

    void clear()
    {
        const juce::ScopedLock sl(lock);
        cache.clear();
        widthCache.clear();
        cacheBytes = 0;
    }

private:
    static size_t imageBytes(const juce::Image& img)
    {
        return (size_t) juce::jmax(0, img.getWidth()) * (size_t) juce::jmax(0, img.getHeight()) * 4;
    }

    // テキスト実幅の測定 ((text, font) 単位でキャッシュ。measure 自体がシェーピングを伴い
    // 重いため、Image キャッシュと同じ理由で毎 paint 実行しない)
    int measureTextWidth(const juce::String& text, const juce::Font& font)
    {
        WidthKey wk { text, font.getHeight(), font.getStyleFlags() };
        {
            const juce::ScopedLock sl(lock);
            auto it = widthCache.find(wk);
            if (it != widthCache.end()) return it->second;
        }
        const int w = juce::GlyphArrangement::getStringWidthInt(font, text);
        const juce::ScopedLock sl(lock);
        if (widthCache.size() >= maxEntries)
            widthCache.erase(widthCache.begin());
        widthCache[wk] = w;
        return w;
    }

    struct WidthKey
    {
        juce::String text;
        float        fontHeight { 0 };
        int          fontStyle  { 0 };
        bool operator<(const WidthKey& o) const
        {
            if (text       != o.text)       return text       < o.text;
            if (fontHeight != o.fontHeight) return fontHeight < o.fontHeight;
            return fontStyle < o.fontStyle;
        }
    };
    struct Key
    {
        juce::String text;
        float        fontHeight   { 0 };
        int          fontStyle    { 0 };
        juce::uint32 colourArgb   { 0 };
        int          w            { 0 };
        int          h            { 0 };
        int          justify      { 0 };
        bool operator<(const Key& o) const
        {
            if (text       != o.text)       return text       < o.text;
            if (fontHeight != o.fontHeight) return fontHeight < o.fontHeight;
            if (fontStyle  != o.fontStyle)  return fontStyle  < o.fontStyle;
            if (colourArgb != o.colourArgb) return colourArgb < o.colourArgb;
            if (w          != o.w)          return w          < o.w;
            if (h          != o.h)          return h          < o.h;
            return justify < o.justify;
        }
    };

    std::map<Key, juce::Image> cache;
    std::map<WidthKey, int>    widthCache;
    size_t                     cacheBytes { 0 };
    juce::CriticalSection      lock;
    static constexpr size_t    maxEntries { 2048 };
    static constexpr size_t    maxBytes   { 32 * 1024 * 1024 };   // 総バイト上限 (安全網)
};
