// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

#include "ModernEffectEditor.h"
#include "../AppColours.h"
#include "../Localisation.h"
#include "../Audio/builtin/BuiltInCompressor.h"
#include "../Audio/builtin/GainDynamics.h"
#include <cmath>

// コンプレッサーのモダン UI: 左に IN/GR/OUT メーター、右に入出力トランスファーカーブ (青塗り)。
// しきい値ノード (横ドラッグ) と右端のレシオハンドル (縦ドラッグ) でカーブを直接調整。
// オートゲイン (静的メイクアップ・遅延無し) のトグル付き。
class BuiltInCompressorEditor : public ModernEffectEditor
{
public:
    explicit BuiltInCompressorEditor(BuiltInCompressor& c)
        : ModernEffectEditor(c, { BuiltInCompressor::ThresholdDb, BuiltInCompressor::Ratio,
                                  BuiltInCompressor::AttackMs, BuiltInCompressor::ReleaseMs,
                                  BuiltInCompressor::MakeupDb }, 224),
          comp(c)
    {
        autoBtn.setButtonText(tr(u8"オートゲイン"));
        autoBtn.setClickingTogglesState(true);
        autoBtn.setColour(juce::TextButton::buttonColourId,   AppColours::buttonBg);
        autoBtn.setColour(juce::TextButton::buttonOnColourId, AppColours::accent);
        autoBtn.setColour(juce::TextButton::textColourOffId,  AppColours::text);
        autoBtn.setColour(juce::TextButton::textColourOnId,   juce::Colours::white);
        autoBtn.setToggleState(comp.getP(BuiltInCompressor::AutoGain) > 0.5f, juce::dontSendNotification);
        autoBtn.onClick = [this]
        {
            comp.setP(BuiltInCompressor::AutoGain, autoBtn.getToggleState() ? 1.0f : 0.0f);
            clearPresetSelection();
            updateAutoState();
            repaint();
        };
        addAndMakeVisible(autoBtn);
        updateAutoState();

        setSize(juce::jmax(580, getWidth()), getHeight());   // メーター + カーブのため幅を確保
        resized();   // 基底 ctor の resized() は派生 vtable 前に走るのでオーバーレイを再配置
    }

private:
    static constexpr float kDMin = -60.0f, kDMax = 0.0f;   // トランスファーカーブの軸
    static constexpr float kMetersW = 156.0f;
    static constexpr float kMinDb = -48.0f, kMaxDb = 6.0f; // レベルメーターの軸
    const juce::Colour kCyan { AppColours::fxCyan };
    const juce::Colour kBlue { AppColours::fxBlue };

    BuiltInCompressor& comp;
    juce::TextButton autoBtn;
    int   dragMode { -1 };
    float dragMakeup { 0.0f };
    float smIn { -100.0f }, smOut { -100.0f };

    juce::Rectangle<float> curveArea(juce::Rectangle<float> a) const { return a.withTrimmedLeft((int) kMetersW).reduced(8.0f); }
    float xFromIn (juce::Rectangle<float> c, float db) const { return c.getX() + (db - kDMin) / (kDMax - kDMin) * c.getWidth(); }
    float yFromOut(juce::Rectangle<float> c, float db) const { return c.getBottom() - (juce::jlimit(kDMin, kDMax, db) - kDMin) / (kDMax - kDMin) * c.getHeight(); }
    float inFromX (juce::Rectangle<float> c, float x)  const { return juce::jlimit(kDMin, kDMax, kDMin + (x - c.getX()) / juce::jmax(1.0f, c.getWidth()) * (kDMax - kDMin)); }
    float outFromY(juce::Rectangle<float> c, float y)  const { return juce::jlimit(kDMin, kDMax, kDMin + (c.getBottom() - y) / juce::jmax(1.0f, c.getHeight()) * (kDMax - kDMin)); }

    float reductionAt(float inDb) const
    {
        const float thr = comp.getP(BuiltInCompressor::ThresholdDb);
        const float ratio = juce::jmax(1.0f, comp.getP(BuiltInCompressor::Ratio));
        return builtin::softKneeReductionDb(inDb - thr, 1.0f - 1.0f / ratio, 6.0f);
    }
    float outAt(float inDb) const { return inDb - reductionAt(inDb) + comp.currentMakeupDb(); }

    void updateAutoState()
    {
        const bool on = comp.getP(BuiltInCompressor::AutoGain) > 0.5f;
        if (autoBtn.getToggleState() != on) autoBtn.setToggleState(on, juce::dontSendNotification);
        if (auto* k = knobForParam(BuiltInCompressor::MakeupDb)) k->setEnabled(! on);
    }

    static float smoothMeter(float prev, float target) { return target > prev ? target : prev + (target - prev) * 0.25f; }
    void onTick() override
    {
        updateAutoState();
        smIn  = smoothMeter(smIn,  comp.getInputDb());
        smOut = smoothMeter(smOut, comp.getOutputDb());
    }

    void layoutOverlay(juce::Rectangle<int> g) override
    {
        autoBtn.setBounds(g.getX(), juce::jmax(6, g.getY() - 27), 116, 22);
    }

    // GR メーター (上から下へ・任意位置)。レベルメーターは基底の drawLevelBar を使う。
    void drawGrBar(juce::Graphics& g, juce::Rectangle<float> r, float redDb)
    {
        g.setColour(AppColours::meterBg);
        g.fillRoundedRectangle(r, 2.0f);
        const float frac = juce::jlimit(0.0f, 1.0f, redDb / 24.0f);
        if (frac > 0.001f)
        {
            auto fillR = r.withHeight(r.getHeight() * frac);
            g.setColour(frac > 0.8f ? AppColours::meterRed : frac > 0.4f ? AppColours::meterYellow : AppColours::accent);
            g.fillRoundedRectangle(fillR, 2.0f);
        }
        g.setColour(AppColours::textDim);
        g.setFont(10.0f);
        g.drawText("GR", r.getX() - 4, r.getBottom() + 2, r.getWidth() + 8, 12, juce::Justification::centred);
    }

    void paintGraph(juce::Graphics& g, juce::Rectangle<float> area) override
    {
        // ── メーター域 (左) ──
        auto m = area.withWidth(kMetersW).reduced(8.0f).withTrimmedBottom(14);
        const float scaleW = 24.0f;
        const float barW   = 22.0f;
        const float x0 = m.getX() + scaleW;
        const float gap = (m.getWidth() - scaleW - barW * 3.0f) / 2.0f;

        // dB 目盛り
        g.setFont(9.5f);
        for (int db = -48; db <= 6; db += 6)
        {
            const float t = (float) (db - kMinDb) / (kMaxDb - kMinDb);
            const float y = m.getBottom() - t * m.getHeight();
            g.setColour(AppColours::rulerLine.withAlpha(0.22f));
            g.drawHorizontalLine((int) y, m.getX() + scaleW - 2, m.getRight());
            g.setColour(AppColours::textDim);
            g.drawText(juce::String(db), (int) m.getX() - 2, (int) y - 6, (int) scaleW, 12, juce::Justification::right);
        }
        const juce::Rectangle<float> inR (x0,                  m.getY(), barW, m.getHeight());
        const juce::Rectangle<float> grR (x0 + barW + gap,     m.getY(), barW, m.getHeight());
        const juce::Rectangle<float> outR(x0 + (barW + gap) * 2, m.getY(), barW, m.getHeight());
        drawLevelBar(g, inR,  smIn,  kMinDb, kMaxDb, kCyan, "IN");
        drawGrBar   (g, grR,  smoothedReductionDb());
        drawLevelBar(g, outR, smOut, kMinDb, kMaxDb, kCyan, "OUT");

        // ── トランスファーカーブ域 (右) ──
        const auto c = curveArea(area);
        g.setColour(AppColours::rulerLine.withAlpha(0.25f));
        for (int db = -48; db <= 0; db += 12)
        {
            g.drawVerticalLine  ((int) xFromIn (c, (float) db), c.getY(), c.getBottom());
            g.drawHorizontalLine((int) yFromOut(c, (float) db), c.getX(), c.getRight());
        }
        g.setColour(AppColours::rulerLine.withAlpha(0.45f));
        g.drawLine(c.getX(), c.getBottom(), c.getRight(), c.getY(), 1.0f);   // 1:1 基準線

        juce::Path curve, fill;
        fill.startNewSubPath(c.getX(), c.getBottom());
        for (int px = 0; px <= (int) c.getWidth(); ++px)
        {
            const float inDb = inFromX(c, c.getX() + (float) px);
            const float x = c.getX() + (float) px;
            const float y = yFromOut(c, outAt(inDb));
            if (px == 0) curve.startNewSubPath(x, y); else curve.lineTo(x, y);
            fill.lineTo(x, y);
        }
        fill.lineTo(c.getRight(), c.getBottom());
        fill.closeSubPath();

        juce::ColourGradient grad(kBlue.withAlpha(0.55f), c.getX(), c.getY(), kBlue.withAlpha(0.10f), c.getX(), c.getBottom(), false);
        g.setGradientFill(grad);
        g.fillPath(fill);
        g.setColour(kCyan);
        g.strokePath(curve, juce::PathStrokeType(2.0f));

        // しきい値ノード / レシオハンドル
        const float thr = comp.getP(BuiltInCompressor::ThresholdDb);
        const float ax = xFromIn(c, thr), ay = yFromOut(c, outAt(thr));
        g.setColour(juce::Colours::white); g.fillEllipse(ax - 6, ay - 6, 12, 12);
        g.setColour(kCyan);                g.drawEllipse(ax - 6, ay - 6, 12, 12, 2.0f);
        const float bx = xFromIn(c, 0.0f), by = yFromOut(c, outAt(0.0f));
        g.setColour(kCyan);                g.fillEllipse(bx - 6, by - 6, 12, 12);
        g.setColour(juce::Colours::white); g.drawEllipse(bx - 6, by - 6, 12, 12, 2.0f);
    }

    void onGraphMouseDown(const juce::MouseEvent& e) override
    {
        const auto c = curveArea(graph.toFloat());
        const float thr = comp.getP(BuiltInCompressor::ThresholdDb);
        const juce::Point<float> a(xFromIn(c, thr),  yFromOut(c, outAt(thr)));
        const juce::Point<float> b(xFromIn(c, 0.0f), yFromOut(c, outAt(0.0f)));
        const float dA = e.position.getDistanceFrom(a);
        const float dB = e.position.getDistanceFrom(b);
        if (juce::jmin(dA, dB) > 22.0f) { dragMode = -1; return; }
        dragMode = (dB < dA) ? 1 : 0;
        dragMakeup = comp.currentMakeupDb();
        onGraphMouseDrag(e);
    }

    void onGraphMouseUp(const juce::MouseEvent&) override { dragMode = -1; }

    void onGraphMouseDrag(const juce::MouseEvent& e) override
    {
        if (dragMode < 0) return;
        const auto c = curveArea(graph.toFloat());
        if (dragMode == 1)
        {
            const float thr = comp.getP(BuiltInCompressor::ThresholdDb);
            const float over = juce::jmax(1.0f, -thr);
            const float yOut = outFromY(c, e.position.y);
            const float slope = juce::jlimit(0.0f, 0.95f, (dragMakeup - yOut) / over);
            const float ratio = 1.0f / (1.0f - slope);
            const auto& pi = comp.getParamInfo(BuiltInCompressor::Ratio);
            comp.setP(BuiltInCompressor::Ratio, juce::jlimit(pi.minV, pi.maxV, ratio));
        }
        else
        {
            const auto& pi = comp.getParamInfo(BuiltInCompressor::ThresholdDb);
            comp.setP(BuiltInCompressor::ThresholdDb, juce::jlimit(pi.minV, pi.maxV, inFromX(c, e.position.x)));
        }
        clearPresetSelection();
        repaint(graph);
    }
};

juce::AudioProcessorEditor* BuiltInCompressor::createEditor()
{
    return new BuiltInCompressorEditor(*this);
}
