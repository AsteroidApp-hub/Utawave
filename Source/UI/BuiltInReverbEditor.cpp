// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

#include "ModernEffectEditor.h"
#include "../AppColours.h"
#include "../Audio/builtin/BuiltInReverb.h"
#include <cmath>

// リバーブのモダン UI: 残響テールの「ディケイ形状」を可視化する。
// 横=時間、縦=広がり (中心から上下対称)。Size で長さ、Damping で高域の早い減衰、
// Width で縦の広がり、Mix で全体の濃さが変わる。
class BuiltInReverbEditor : public ModernEffectEditor
{
public:
    explicit BuiltInReverbEditor(BuiltInReverb& r)
        : ModernEffectEditor(r, { BuiltInReverb::MixPct, BuiltInReverb::Size,
                                  BuiltInReverb::Damping, BuiltInReverb::Width }),
          rev(r) {}

private:
    BuiltInReverb& rev;

    void paintGraph(juce::Graphics& g, juce::Rectangle<float> area) override
    {
        const auto a = area.reduced(10.0f);
        const float mix   = juce::jlimit(0.0f, 1.0f, rev.getP(BuiltInReverb::MixPct) * 0.01f);
        const float size  = juce::jlimit(0.0f, 1.0f, rev.getP(BuiltInReverb::Size));
        const float damp  = juce::jlimit(0.0f, 1.0f, rev.getP(BuiltInReverb::Damping));
        const float width = juce::jlimit(0.0f, 1.0f, rev.getP(BuiltInReverb::Width));

        const float cy = a.getCentreY();
        const float k  = juce::jmap(size, 0.0f, 1.0f, 7.5f, 1.6f);          // 大きいほど長い尾
        const float k2 = k * (1.0f + damp * 2.5f);                          // 高域はもっと早く減衰
        const float ampMain = (0.30f + 0.70f * mix);
        const float halfMax = a.getHeight() * 0.5f * (0.30f + 0.70f * width);

        auto envShape = [&](float decay, float amp) -> juce::Path
        {
            juce::Path top, full;
            const int W = (int) a.getWidth();
            for (int px = 0; px <= W; ++px)
            {
                const float t = (float) px / juce::jmax(1, W);
                const float h = std::exp(-t * decay) * amp * halfMax;
                const float x = a.getX() + (float) px;
                if (px == 0) top.startNewSubPath(x, cy - h); else top.lineTo(x, cy - h);
            }
            full = top;
            for (int px = W; px >= 0; --px)
            {
                const float t = (float) px / juce::jmax(1, W);
                const float h = std::exp(-t * decay) * amp * halfMax;
                full.lineTo(a.getX() + (float) px, cy + h);
            }
            full.closeSubPath();
            return full;
        };

        // 中心線
        g.setColour(AppColours::rulerLine.withAlpha(0.35f));
        g.drawHorizontalLine((int) cy, a.getX(), a.getRight());

        // メインのテール (広がり)
        g.setColour(AppColours::accent.withAlpha(0.22f));
        g.fillPath(envShape(k, ampMain));
        // 高域 (ダンピングで早く減衰する内側)
        g.setColour(AppColours::accent.withAlpha(0.40f));
        g.fillPath(envShape(k2, ampMain * 0.9f));

        // 立ち上がりインパルス
        g.setColour(juce::Colours::white.withAlpha(0.85f));
        g.drawLine(a.getX(), cy - halfMax * ampMain, a.getX(), cy + halfMax * ampMain, 2.0f);

        // 輪郭ライン
        juce::Path outline;
        const int W = (int) a.getWidth();
        for (int px = 0; px <= W; ++px)
        {
            const float t = (float) px / juce::jmax(1, W);
            const float h = std::exp(-t * k) * ampMain * halfMax;
            const float x = a.getX() + (float) px;
            if (px == 0) outline.startNewSubPath(x, cy - h); else outline.lineTo(x, cy - h);
        }
        g.setColour(AppColours::accent.withAlpha(0.7f));
        g.strokePath(outline, juce::PathStrokeType(1.2f));
    }
};

juce::AudioProcessorEditor* BuiltInReverb::createEditor()
{
    return new BuiltInReverbEditor(*this);
}
