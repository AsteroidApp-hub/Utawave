// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

#include "ModernEffectEditor.h"
#include "../AppColours.h"
#include "../Localisation.h"
#include "../Audio/builtin/BuiltInDeEsser.h"
#include <cmath>

// ディエッサーのモダン UI: 対数周波数の帯に「処理対象の高域 (周波数以上)」をハイライトし、
// 周波数マーカーを横ドラッグで動かす。右に GR メータ。
class BuiltInDeEsserEditor : public ModernEffectEditor
{
public:
    explicit BuiltInDeEsserEditor(BuiltInDeEsser& d)
        : ModernEffectEditor(d, { BuiltInDeEsser::FreqHz, BuiltInDeEsser::ThresholdDb, BuiltInDeEsser::Ratio }),
          de(d) {}

private:
    static constexpr float kFMin = 20.0f, kFMax = 20000.0f, kMeterW = 26.0f;
    BuiltInDeEsser& de;

    juce::Rectangle<float> bandArea(juce::Rectangle<float> a) const { return a.reduced(8.0f).withTrimmedRight(kMeterW); }
    float freqToX(juce::Rectangle<float> b, double hz) const
    {
        hz = juce::jlimit((double) kFMin, (double) kFMax, hz);
        return b.getX() + (float) (std::log(hz / kFMin) / std::log((double) kFMax / kFMin)) * b.getWidth();
    }
    double xToFreq(juce::Rectangle<float> b, float x) const
    {
        const double t = juce::jlimit(0.0, 1.0, (double) (x - b.getX()) / juce::jmax(1.0f, b.getWidth()));
        return kFMin * std::pow((double) kFMax / kFMin, t);
    }

    void paintGraph(juce::Graphics& g, juce::Rectangle<float> area) override
    {
        const auto b = bandArea(area);
        const float fx2 = freqToX(b, de.getP(BuiltInDeEsser::FreqHz));

        // 処理対象の高域 (周波数以上) をハイライト
        g.setColour(AppColours::accent.withAlpha(0.16f));
        g.fillRect(juce::Rectangle<float>(fx2, b.getY(), b.getRight() - fx2, b.getHeight()));

        // 周波数グリッド + ラベル
        static const double lines[] = { 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000 };
        g.setFont(11.0f);
        for (double f : lines)
        {
            const float x = freqToX(b, f);
            g.setColour(AppColours::rulerLine.withAlpha(0.3f));
            g.drawVerticalLine((int) x, b.getY(), b.getBottom());
            if (f == 100 || f == 1000 || f == 10000)
            {
                g.setColour(AppColours::textDim);
                g.drawText(f >= 1000 ? (juce::String((int) (f / 1000)) + "k") : juce::String((int) f),
                           (int) x - 16, (int) b.getBottom() - 15, 32, 14, juce::Justification::centred);
            }
        }

        // 周波数マーカー
        g.setColour(AppColours::accent);
        g.drawLine(fx2, b.getY(), fx2, b.getBottom(), 2.0f);
        g.fillEllipse(fx2 - 6, b.getY() + 8, 12, 12);
        g.setColour(juce::Colours::white.withAlpha(0.9f));
        g.drawEllipse(fx2 - 6, b.getY() + 8, 12, 12, 1.5f);
        g.setColour(AppColours::textDim);
        g.setFont(11.0f);
        g.drawText(tr(u8"サ行"), (int) fx2 + 8, (int) b.getY() + 6, 60, 16, juce::Justification::left);

        drawReductionMeter(g, area, 18.0f);
    }

    bool dragging { false };

    void onGraphMouseDown(const juce::MouseEvent& e) override
    {
        // 帯域エリア内 (GR メータ・余白を除く) のクリックだけ周波数操作にする
        dragging = bandArea(graph.toFloat()).contains(e.position);
        if (dragging) onGraphMouseDrag(e);
    }
    void onGraphMouseUp(const juce::MouseEvent&) override { dragging = false; }
    void onGraphMouseDrag(const juce::MouseEvent& e) override
    {
        if (! dragging) return;
        const auto b = bandArea(graph.toFloat());
        const auto& pi = de.getParamInfo(BuiltInDeEsser::FreqHz);
        de.setP(BuiltInDeEsser::FreqHz, juce::jlimit(pi.minV, pi.maxV, (float) xToFreq(b, (float) e.position.x)));
        clearPresetSelection();
        repaint(graph);
    }
};

juce::AudioProcessorEditor* BuiltInDeEsser::createEditor()
{
    return new BuiltInDeEsserEditor(*this);
}
