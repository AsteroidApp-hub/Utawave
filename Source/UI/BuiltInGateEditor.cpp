// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

#include "ModernEffectEditor.h"
#include "../AppColours.h"
#include "../Localisation.h"
#include "../Audio/builtin/BuiltInGate.h"
#include <cmath>

// ノイズゲートのモダン UI: 左に IN/GR メーター (IN にしきい値ティック)、右にゲートの
// 入出力トランスファーカーブ (しきい値で段差状に落ちる)。しきい値ノード (横ドラッグ) と
// レンジハンドル (縦ドラッグ) でカーブを直接調整。入力レベルの「いま位置」をライブ針で示し、
// ゲートの開閉を光る OPEN/CLOSED ピルで表示する。
class BuiltInGateEditor : public ModernEffectEditor
{
public:
    explicit BuiltInGateEditor(BuiltInGate& g)
        : ModernEffectEditor(g, { BuiltInGate::ThresholdDb, BuiltInGate::RangeDb,
                                  BuiltInGate::AttackMs, BuiltInGate::HoldMs,
                                  BuiltInGate::ReleaseMs }, 224),
          gate(g)
    {
        setSize(juce::jmax(580, getWidth()), getHeight());   // メーター + カーブのため幅を確保
        resized();
    }

private:
    static constexpr float kDMin = -80.0f, kDMax = 0.0f;   // トランスファーカーブの軸
    static constexpr float kMetersW = 124.0f;
    static constexpr float kMinDb = -80.0f, kMaxDb = 0.0f; // レベルメーターの軸
    const juce::Colour kCyan  { AppColours::fxCyan };
    const juce::Colour kBlue  { AppColours::fxBlue };
    const juce::Colour kGreen { AppColours::playGreen };

    BuiltInGate& gate;
    int   dragMode { -1 };       // 0 = threshold, 1 = range
    float smIn { -100.0f };
    float openGlow { 0.0f };     // 0..1 の開放グロー (平滑化)

    juce::Rectangle<float> curveArea(juce::Rectangle<float> a) const { return a.withTrimmedLeft((int) kMetersW).reduced(8.0f); }
    float xFromIn (juce::Rectangle<float> c, float db) const { return c.getX() + (juce::jlimit(kDMin, kDMax, db) - kDMin) / (kDMax - kDMin) * c.getWidth(); }
    float yFromOut(juce::Rectangle<float> c, float db) const { return c.getBottom() - (juce::jlimit(kDMin, kDMax, db) - kDMin) / (kDMax - kDMin) * c.getHeight(); }
    float inFromX (juce::Rectangle<float> c, float x)  const { return juce::jlimit(kDMin, kDMax, kDMin + (x - c.getX()) / juce::jmax(1.0f, c.getWidth()) * (kDMax - kDMin)); }
    float outFromY(juce::Rectangle<float> c, float y)  const { return juce::jlimit(kDMin, kDMax, kDMin + (c.getBottom() - y) / juce::jmax(1.0f, c.getHeight()) * (kDMax - kDMin)); }

    // ゲートの出力: しきい値以上は 1:1、未満は レンジ分だけ下げる (段差カーブ)。
    float outAt(float inDb) const
    {
        const float thr   = gate.getP(BuiltInGate::ThresholdDb);
        const float range = gate.getP(BuiltInGate::RangeDb);
        return inDb >= thr ? inDb : juce::jmax(kDMin, inDb - range);
    }

    static float smoothMeter(float prev, float target) { return target > prev ? target : prev + (target - prev) * 0.25f; }
    void onTick() override
    {
        smIn = smoothMeter(smIn, gate.getInputDb());
        const float t = gate.isOpen() ? 1.0f : 0.0f;
        openGlow = openGlow + (t - openGlow) * 0.3f;
    }

    // GR メーター (上から下へ・絞った量)。fullScale = レンジ (全閉でバー満杯)。
    void drawGrBar(juce::Graphics& g, juce::Rectangle<float> r, float redDb, float fullScale)
    {
        g.setColour(AppColours::meterBg);
        g.fillRoundedRectangle(r, 2.0f);
        const float frac = juce::jlimit(0.0f, 1.0f, redDb / juce::jmax(1.0f, fullScale));
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
        // ── メーター域 (左): IN レベル + GR ──
        auto m = area.withWidth(kMetersW).reduced(8.0f).withTrimmedBottom(14);
        const float scaleW = 26.0f;
        const float barW   = 24.0f;
        const float x0  = m.getX() + scaleW;
        const float gap = juce::jmax(8.0f, m.getWidth() - scaleW - barW * 2.0f);

        g.setFont(9.5f);
        for (int db = -80; db <= 0; db += 10)
        {
            const float t = (float) (db - kMinDb) / (kMaxDb - kMinDb);
            const float y = m.getBottom() - t * m.getHeight();
            g.setColour(AppColours::rulerLine.withAlpha(0.20f));
            g.drawHorizontalLine((int) y, m.getX() + scaleW - 2, m.getRight());
            g.setColour(AppColours::textDim);
            g.drawText(juce::String(db), (int) m.getX() - 2, (int) y - 6, (int) scaleW, 12, juce::Justification::right);
        }
        const juce::Rectangle<float> inR(x0,             m.getY(), barW, m.getHeight());
        const juce::Rectangle<float> grR(x0 + barW + gap, m.getY(), barW, m.getHeight());
        // IN メーターは開いていれば緑、閉じていればシアンで「鳴っている/ゲートが切っている」を直感化
        const juce::Colour inCol = kCyan.interpolatedWith(kGreen, juce::jlimit(0.0f, 1.0f, openGlow));
        drawLevelBar(g, inR, smIn, kMinDb, kMaxDb, inCol, "IN");
        drawGrBar(g, grR, smoothedReductionDb(), gate.getP(BuiltInGate::RangeDb));

        // IN メーター上のしきい値ティック
        const float thr = gate.getP(BuiltInGate::ThresholdDb);
        const float tThr = juce::jlimit(0.0f, 1.0f, (thr - kMinDb) / (kMaxDb - kMinDb));
        const float yThr = inR.getBottom() - tThr * inR.getHeight();
        g.setColour(AppColours::accent);
        g.drawHorizontalLine((int) yThr, inR.getX() - 3, inR.getRight() + 3);

        // ── トランスファーカーブ域 (右) ──
        const auto c = curveArea(area);
        g.setColour(AppColours::rulerLine.withAlpha(0.25f));
        for (int db = -80; db <= 0; db += 20)
        {
            g.drawVerticalLine  ((int) xFromIn (c, (float) db), c.getY(), c.getBottom());
            g.drawHorizontalLine((int) yFromOut(c, (float) db), c.getX(), c.getRight());
        }
        g.setColour(AppColours::rulerLine.withAlpha(0.45f));
        g.drawLine(c.getX(), c.getBottom(), c.getRight(), c.getY(), 1.0f);   // 1:1 基準線

        // ゲートで通る領域 (しきい値以上の入力) を緑で薄く塗ってハイライト
        const float xThr = xFromIn(c, thr);
        g.setColour(kGreen.withAlpha(0.10f));
        g.fillRect(juce::Rectangle<float>(xThr, c.getY(), c.getRight() - xThr, c.getHeight()));

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

        // ライブ入力針 (いまの入力レベル位置)。開いていれば緑・閉じていれば淡色。
        const float nx = xFromIn(c, smIn);
        g.setColour(kGreen.withAlpha(0.25f + 0.55f * openGlow));
        g.drawLine(nx, c.getY(), nx, c.getBottom(), 1.5f);
        const float ny = yFromOut(c, outAt(smIn));
        g.setColour(kGreen.brighter(0.2f).withAlpha(0.4f + 0.6f * openGlow));
        g.fillEllipse(nx - 4, ny - 4, 8, 8);

        // しきい値ノード (横ドラッグ) とレンジハンドル (縦ドラッグ) を段差の上下端に置く
        const float ax = xThr, ayTop = yFromOut(c, thr);
        const float range = gate.getP(BuiltInGate::RangeDb);
        const float ayBot = yFromOut(c, juce::jmax(kDMin, thr - range));
        g.setColour(AppColours::accent.withAlpha(0.6f));
        g.drawLine(ax, ayTop, ax, ayBot, 2.0f);                       // 段差 (= ゲートの絞り量)
        g.setColour(juce::Colours::white); g.fillEllipse(ax - 6, ayTop - 6, 12, 12);
        g.setColour(AppColours::accent);   g.drawEllipse(ax - 6, ayTop - 6, 12, 12, 2.0f);
        g.setColour(AppColours::accent);   g.fillEllipse(ax - 6, ayBot - 6, 12, 12);
        g.setColour(juce::Colours::white); g.drawEllipse(ax - 6, ayBot - 6, 12, 12, 2.0f);

        // 開閉ステータスのピル (グロー付き)
        const bool isOpen = openGlow > 0.5f;
        const juce::String label = isOpen ? juce::String("OPEN") : juce::String("CLOSED");
        const juce::Colour pill = isOpen ? kGreen : AppColours::stopGrey;
        auto pr = juce::Rectangle<float>(c.getRight() - 86, c.getY() + 6, 80, 22);
        g.setColour(pill.withAlpha(0.18f + 0.5f * openGlow));
        g.fillRoundedRectangle(pr.expanded(3.0f * openGlow), 11.0f);
        g.setColour(pill);
        g.drawRoundedRectangle(pr, 11.0f, 1.5f);
        g.fillEllipse(pr.getX() + 8, pr.getCentreY() - 4, 8, 8);
        g.setColour(juce::Colours::white.withAlpha(0.92f));
        g.setFont(juce::Font(12.0f, juce::Font::bold));
        g.drawText(label, pr.withTrimmedLeft(20), juce::Justification::centredLeft);
    }

    void onGraphMouseDown(const juce::MouseEvent& e) override
    {
        const auto c = curveArea(graph.toFloat());
        const float thr   = gate.getP(BuiltInGate::ThresholdDb);
        const float range = gate.getP(BuiltInGate::RangeDb);
        const juce::Point<float> top(xFromIn(c, thr), yFromOut(c, thr));
        const juce::Point<float> bot(xFromIn(c, thr), yFromOut(c, juce::jmax(kDMin, thr - range)));
        const float dTop = e.position.getDistanceFrom(top);
        const float dBot = e.position.getDistanceFrom(bot);
        if (juce::jmin(dTop, dBot) > 22.0f) { dragMode = -1; return; }
        dragMode = (dBot < dTop) ? 1 : 0;
        onGraphMouseDrag(e);
    }

    void onGraphMouseUp(const juce::MouseEvent&) override { dragMode = -1; }

    void onGraphMouseDrag(const juce::MouseEvent& e) override
    {
        if (dragMode < 0) return;
        const auto c = curveArea(graph.toFloat());
        if (dragMode == 1)
        {
            const float thr = gate.getP(BuiltInGate::ThresholdDb);
            const float out = outFromY(c, e.position.y);
            const auto& pi = gate.getParamInfo(BuiltInGate::RangeDb);
            gate.setP(BuiltInGate::RangeDb, juce::jlimit(pi.minV, pi.maxV, thr - out));
        }
        else
        {
            const auto& pi = gate.getParamInfo(BuiltInGate::ThresholdDb);
            gate.setP(BuiltInGate::ThresholdDb, juce::jlimit(pi.minV, pi.maxV, inFromX(c, e.position.x)));
        }
        clearPresetSelection();
        repaint(graph);
    }
};

juce::AudioProcessorEditor* BuiltInGate::createEditor()
{
    return new BuiltInGateEditor(*this);
}
