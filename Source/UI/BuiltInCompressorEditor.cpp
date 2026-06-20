// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

#include "ModernEffectEditor.h"
#include "../AppColours.h"
#include "../Audio/builtin/BuiltInCompressor.h"
#include "../Audio/builtin/GainDynamics.h"
#include <cmath>

// コンプレッサーのモダン UI: 入出力トランスファーカーブ + GR メータ + しきい値ノードのドラッグ。
class BuiltInCompressorEditor : public ModernEffectEditor
{
public:
    explicit BuiltInCompressorEditor(BuiltInCompressor& c)
        : ModernEffectEditor(c, { BuiltInCompressor::ThresholdDb, BuiltInCompressor::Ratio,
                                  BuiltInCompressor::AttackMs, BuiltInCompressor::ReleaseMs,
                                  BuiltInCompressor::MakeupDb }),
          comp(c) {}

private:
    static constexpr float kDMin = -60.0f, kDMax = 0.0f;
    static constexpr float kMeterW = 26.0f;
    BuiltInCompressor& comp;

    juce::Rectangle<float> curveArea(juce::Rectangle<float> a) const { return a.reduced(8.0f).withTrimmedRight(kMeterW); }
    float xFromIn (juce::Rectangle<float> c, float db) const { return c.getX() + (db - kDMin) / (kDMax - kDMin) * c.getWidth(); }
    float yFromOut(juce::Rectangle<float> c, float db) const { return c.getBottom() - (db - kDMin) / (kDMax - kDMin) * c.getHeight(); }
    float inFromX (juce::Rectangle<float> c, float x)  const { return juce::jlimit(kDMin, kDMax, kDMin + (x - c.getX()) / juce::jmax(1.0f, c.getWidth()) * (kDMax - kDMin)); }

    float gainReductionAt(float inDb) const
    {
        const float thr = comp.getP(BuiltInCompressor::ThresholdDb);
        const float ratio = juce::jmax(1.0f, comp.getP(BuiltInCompressor::Ratio));
        return builtin::softKneeReductionDb(inDb - thr, 1.0f - 1.0f / ratio, 6.0f);
    }

    void paintGraph(juce::Graphics& g, juce::Rectangle<float> area) override
    {
        const auto c = curveArea(area);

        // グリッド + 1:1 基準線
        g.setColour(AppColours::rulerLine.withAlpha(0.28f));
        for (int db = -48; db <= 0; db += 12)
        {
            g.drawVerticalLine  ((int) xFromIn (c, (float) db), c.getY(), c.getBottom());
            g.drawHorizontalLine((int) yFromOut(c, (float) db), c.getX(), c.getRight());
        }
        g.setColour(AppColours::rulerLine.withAlpha(0.5f));
        g.drawLine(c.getX(), c.getBottom(), c.getRight(), c.getY(), 1.0f);

        // トランスファーカーブ
        juce::Path curve;
        for (int px = 0; px <= (int) c.getWidth(); ++px)
        {
            const float inDb = inFromX(c, c.getX() + (float) px);
            const float outDb = inDb - gainReductionAt(inDb);
            const float x = c.getX() + (float) px;
            const float y = yFromOut(c, outDb);
            if (px == 0) curve.startNewSubPath(x, y); else curve.lineTo(x, y);
        }
        g.setColour(AppColours::accent);
        g.strokePath(curve, juce::PathStrokeType(2.0f));

        // しきい値ノード
        const float thr = comp.getP(BuiltInCompressor::ThresholdDb);
        const float nx = xFromIn(c, thr);
        const float ny = yFromOut(c, thr - gainReductionAt(thr));
        g.setColour(juce::Colours::white);
        g.fillEllipse(nx - 6, ny - 6, 12, 12);
        g.setColour(AppColours::accent);
        g.drawEllipse(nx - 6, ny - 6, 12, 12, 2.0f);

        // GR メータ (右端・上から下へ減衰量)
        auto m = juce::Rectangle<float>(area.getRight() - kMeterW + 4, area.getY() + 6,
                                        kMeterW - 8, area.getHeight() - 24);
        g.setColour(AppColours::meterBg);
        g.fillRoundedRectangle(m, 2.0f);
        const float frac = juce::jlimit(0.0f, 1.0f, smoothedReductionDb() / 24.0f);
        if (frac > 0.001f)
        {
            auto fill = m.removeFromTop(m.getHeight() * frac);
            g.setColour(frac > 0.8f ? AppColours::meterRed : frac > 0.4f ? AppColours::meterYellow : AppColours::accent);
            g.fillRoundedRectangle(fill, 2.0f);
        }
        g.setColour(AppColours::textDim);
        g.setFont(10.0f);
        g.drawText("GR", (int) (area.getRight() - kMeterW + 2), (int) (area.getBottom() - 16),
                   (int) kMeterW, 14, juce::Justification::centred);
    }

    void onGraphMouseDown(const juce::MouseEvent& e) override { onGraphMouseDrag(e); }
    void onGraphMouseDrag(const juce::MouseEvent& e) override
    {
        const auto c = curveArea(graph.toFloat());
        const auto& pi = comp.getParamInfo(BuiltInCompressor::ThresholdDb);
        comp.setP(BuiltInCompressor::ThresholdDb, juce::jlimit(pi.minV, pi.maxV, inFromX(c, (float) e.position.x)));
        clearPresetSelection();
        repaint(graph);
    }
};

juce::AudioProcessorEditor* BuiltInCompressor::createEditor()
{
    return new BuiltInCompressorEditor(*this);
}
