// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

#pragma once
#include <JuceHeader.h>
#include "../AppColours.h"

class UtawaveLookAndFeel : public juce::LookAndFeel_V4
{
public:
    UtawaveLookAndFeel()
    {
        setColour(juce::ResizableWindow::backgroundColourId, AppColours::panelBg);
        setColour(juce::AlertWindow::backgroundColourId,     AppColours::panelBg);
        setColour(juce::AlertWindow::textColourId,           AppColours::text);
        setColour(juce::TextButton::buttonColourId,          AppColours::buttonBg);
        setColour(juce::TextButton::textColourOffId,         AppColours::text);
        setColour(juce::ComboBox::backgroundColourId,        AppColours::buttonBg);
        setColour(juce::ComboBox::textColourId,              AppColours::text);
        setColour(juce::PopupMenu::backgroundColourId,       AppColours::panelBg);
        setColour(juce::PopupMenu::textColourId,             AppColours::text);
        setColour(juce::PopupMenu::highlightedBackgroundColourId, AppColours::accent);
        setColour(juce::Label::textColourId,                 AppColours::text);

        // ツールチップ: 暗い UI に埋もれないよう、やや明るい背景 + アクセント枠 + 明るい文字
        setColour(juce::TooltipWindow::backgroundColourId, juce::Colour(0xff333538));
        setColour(juce::TooltipWindow::textColourId,       juce::Colour(0xfff2f2f2));
        setColour(juce::TooltipWindow::outlineColourId,    AppColours::accent);
    }

    // DAW スタイルの垂直フェーダー
    void drawLinearSlider(juce::Graphics& g,
                          int x, int y, int width, int height,
                          float sliderPos,
                          float /*minSliderPos*/, float /*maxSliderPos*/,
                          juce::Slider::SliderStyle style,
                          juce::Slider& /*slider*/) override
    {
        if (style != juce::Slider::LinearVertical)
        {
            LookAndFeel_V4::drawLinearSlider(g, x, y, width, height,
                                             sliderPos, 0, 0, style, *((juce::Slider*)nullptr));
            return;
        }

        // ── Track (groove) ──
        const int trackW = 4;
        const int trackX = x + width / 2 - trackW / 2;
        g.setColour(AppColours::meterBg);
        g.fillRoundedRectangle((float)trackX, (float)y,
                               (float)trackW, (float)height, 2.0f);
        g.setColour(AppColours::separator);
        g.drawRoundedRectangle((float)trackX, (float)y,
                               (float)trackW, (float)height, 2.0f, 1.0f);

        // ── Fader cap ──
        const int capW = width - 6;
        const int capH = 20;
        const int capX = x + 3;
        const int capY = (int)sliderPos - capH / 2;

        // shadow
        g.setColour(juce::Colours::black.withAlpha(0.4f));
        g.fillRoundedRectangle((float)(capX + 1), (float)(capY + 2),
                               (float)capW, (float)capH, 3.0f);

        // cap body
        juce::ColourGradient grad(
            AppColours::buttonHover, (float)capX, (float)capY,
            AppColours::buttonBg,    (float)capX, (float)(capY + capH),
            false);
        g.setGradientFill(grad);
        g.fillRoundedRectangle((float)capX, (float)capY,
                               (float)capW, (float)capH, 3.0f);

        // cap border
        g.setColour(AppColours::separatorLight);
        g.drawRoundedRectangle((float)capX, (float)capY,
                               (float)capW, (float)capH, 3.0f, 1.0f);

        // center notch line
        const int midY = capY + capH / 2;
        g.setColour(AppColours::accent);
        g.drawLine((float)(capX + 4), (float)midY,
                   (float)(capX + capW - 4), (float)midY, 1.5f);
    }

    int getSliderThumbRadius(juce::Slider&) override { return 0; }

    // ── ツールチップ（角丸 + アクセント枠 + 余白 + 左寄せ複数行）──
    // TooltipWindow は setOpaque(true) + ネイティブ影付きなので、全面を塗る
    // (透明マージン/手描き影は黒く出る/二重影になるため使わない)。
    juce::Rectangle<int> getTooltipBounds(const juce::String& tipText,
                                          juce::Point<int> screenPos,
                                          juce::Rectangle<int> parentArea) override
    {
        const auto tl = layoutTooltip(tipText, juce::Colours::white);
        const int w = (int) tl.getWidth()  + kTipPadX * 2;
        const int h = (int) tl.getHeight() + kTipPadY * 2;
        return juce::Rectangle<int>(
                   screenPos.x > parentArea.getCentreX() ? screenPos.x - (w + 10) : screenPos.x + 18,
                   screenPos.y > parentArea.getCentreY() ? screenPos.y - (h + 6)  : screenPos.y + 12,
                   w, h).constrainedWithin(parentArea);
    }

    void drawTooltip(juce::Graphics& g, const juce::String& text, int width, int height) override
    {
        auto bounds = juce::Rectangle<float>((float) width, (float) height);
        const float corner = 5.0f;

        g.setColour(findColour(juce::TooltipWindow::backgroundColourId));
        g.fillRoundedRectangle(bounds, corner);

        g.setColour(findColour(juce::TooltipWindow::outlineColourId).withAlpha(0.95f));
        g.drawRoundedRectangle(bounds.reduced(0.6f), corner, 1.3f);

        layoutTooltip(text, findColour(juce::TooltipWindow::textColourId))
            .draw(g, bounds.reduced((float) kTipPadX, (float) kTipPadY));
    }

private:
    static constexpr int kTipPadX  = 12;
    static constexpr int kTipPadY  = 9;
    static constexpr float kTipMaxWidth = 340.0f;

    static juce::TextLayout layoutTooltip(const juce::String& text, juce::Colour colour)
    {
        juce::AttributedString s;
        s.setJustification(juce::Justification::topLeft);
        s.setLineSpacing(2.0f);
        s.append(text, juce::Font(13.5f, juce::Font::plain), colour);
        juce::TextLayout tl;
        tl.createLayout(s, kTipMaxWidth);
        return tl;
    }
};
