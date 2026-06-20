// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

#include "ModernEffectEditor.h"
#include "SpectrumScope.h"
#include "../AppColours.h"
#include "../Localisation.h"
#include "../Audio/builtin/BuiltInDeEsser.h"
#include <cmath>

static_assert(SpectrumScope::N == BuiltInDeEsser::kAnalyzerSize, "SpectrumScope/analyzer size mismatch");

// ディエッサーのモダン UI: 青塗りのスペクトラムアナライザー全域に、処理対象の高域 (周波数以上) を
// ハイライト + 周波数マーカー。dB/周波数の目盛り、右に GR メータ。
class BuiltInDeEsserEditor : public ModernEffectEditor
{
public:
    explicit BuiltInDeEsserEditor(BuiltInDeEsser& d)
        : ModernEffectEditor(d, { BuiltInDeEsser::FreqHz, BuiltInDeEsser::ThresholdDb, BuiltInDeEsser::Ratio }, 230),
          de(d)
    {
        de.setAnalyzerActive(true);
        setSize(juce::jmax(600, getWidth()), getHeight());
        resized();
    }
    ~BuiltInDeEsserEditor() override { de.setAnalyzerActive(false); }

private:
    static constexpr float kFMin = 20.0f, kFMax = 20000.0f, kMeterW = 28.0f;
    static constexpr float kScaleW = 26.0f, kFreqH = 14.0f;
    static constexpr float kAnMin = -72.0f, kAnMax = 0.0f;
    const juce::Colour kBlue { AppColours::fxBlue };
    const juce::Colour kCyan { AppColours::fxCyan };

    BuiltInDeEsser& de;
    SpectrumScope scope;
    bool dragging { false };

    // スペクトラム描画域 (目盛り/メータを除いた内側)
    juce::Rectangle<float> plotArea(juce::Rectangle<float> a) const
    {
        return a.reduced(8.0f).withTrimmedLeft((int) kScaleW).withTrimmedRight((int) kMeterW + 4).withTrimmedBottom((int) kFreqH);
    }
    float freqToX(juce::Rectangle<float> p, double hz) const
    {
        hz = juce::jlimit((double) kFMin, (double) kFMax, hz);
        return p.getX() + (float) (std::log(hz / kFMin) / std::log((double) kFMax / kFMin)) * p.getWidth();
    }
    double xToFreq(juce::Rectangle<float> p, float x) const
    {
        const double t = juce::jlimit(0.0, 1.0, (double) (x - p.getX()) / juce::jmax(1.0f, p.getWidth()));
        return kFMin * std::pow((double) kFMax / kFMin, t);
    }

    void onTick() override
    {
        scope.update([this](float* dst, int n) { return de.readAnalyzerSamples(dst, n); });
    }

    void paintGraph(juce::Graphics& g, juce::Rectangle<float> area) override
    {
        const auto p = plotArea(area);
        const double sr = de.getSampleRateForUi();

        // dB 目盛り
        g.setFont(9.5f);
        for (int db = 0; db >= -72; db -= 12)
        {
            const float t = (float) (db - kAnMin) / (kAnMax - kAnMin);
            const float y = p.getBottom() - t * p.getHeight();
            g.setColour(AppColours::rulerLine.withAlpha(0.20f));
            g.drawHorizontalLine((int) y, p.getX(), p.getRight());
            g.setColour(AppColours::textDim);
            g.drawText(juce::String(db), (int) area.getX() + 2, (int) y - 6, (int) kScaleW - 2, 12, juce::Justification::right);
        }
        // 周波数目盛り
        static const double fl[] = { 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000 };
        for (double f : fl)
        {
            const float x = freqToX(p, f);
            g.setColour(AppColours::rulerLine.withAlpha(0.20f));
            g.drawVerticalLine((int) x, p.getY(), p.getBottom());
            g.setColour(AppColours::textDim);
            g.drawText(f >= 1000 ? (juce::String((int) (f / 1000)) + "k") : juce::String((int) f),
                       (int) x - 16, (int) p.getBottom() + 1, 32, 13, juce::Justification::centred);
        }

        // スペクトラム (青塗り)
        scope.draw(g, p, sr, kFMin, kFMax, kAnMin, kAnMax, kBlue, kCyan.withAlpha(0.8f));

        // 処理対象の高域 (周波数以上) をハイライト
        const float fx2 = freqToX(p, de.getP(BuiltInDeEsser::FreqHz));
        g.setColour(AppColours::accent.withAlpha(0.14f));
        g.fillRect(juce::Rectangle<float>(fx2, p.getY(), p.getRight() - fx2, p.getHeight()));

        // 周波数マーカー
        g.setColour(AppColours::accent);
        g.drawLine(fx2, p.getY(), fx2, p.getBottom(), 2.0f);
        g.fillEllipse(fx2 - 6, p.getY() + 6, 12, 12);
        g.setColour(juce::Colours::white.withAlpha(0.9f));
        g.drawEllipse(fx2 - 6, p.getY() + 6, 12, 12, 1.5f);
        g.setColour(AppColours::textDim);
        g.setFont(11.0f);
        g.drawText(tr(u8"サ行"), (int) fx2 + 8, (int) p.getY() + 4, 60, 16, juce::Justification::left);

        drawReductionMeter(g, area, 18.0f);
    }

    void onGraphMouseDown(const juce::MouseEvent& e) override
    {
        dragging = plotArea(graph.toFloat()).contains(e.position);
        if (dragging) onGraphMouseDrag(e);
    }
    void onGraphMouseUp(const juce::MouseEvent&) override { dragging = false; }
    void onGraphMouseDrag(const juce::MouseEvent& e) override
    {
        if (! dragging) return;
        const auto p = plotArea(graph.toFloat());
        const auto& pi = de.getParamInfo(BuiltInDeEsser::FreqHz);
        de.setP(BuiltInDeEsser::FreqHz, juce::jlimit(pi.minV, pi.maxV, (float) xToFreq(p, e.position.x)));
        clearPresetSelection();
        repaint(graph);
    }
};

juce::AudioProcessorEditor* BuiltInDeEsser::createEditor()
{
    return new BuiltInDeEsserEditor(*this);
}
