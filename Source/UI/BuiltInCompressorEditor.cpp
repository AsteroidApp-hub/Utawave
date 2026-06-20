// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

#include "ModernEffectEditor.h"
#include "../AppColours.h"
#include "../Localisation.h"
#include "../Audio/builtin/BuiltInCompressor.h"
#include "../Audio/builtin/GainDynamics.h"
#include <cmath>

// コンプレッサーのモダン UI: 入出力トランスファーカーブ + GR メータ。
// しきい値ノード (横ドラッグ) と右端のレシオハンドル (縦ドラッグ) でカーブを直接調整。
// オートゲイン (静的メイクアップ・遅延無し) のトグル付き。
class BuiltInCompressorEditor : public ModernEffectEditor
{
public:
    explicit BuiltInCompressorEditor(BuiltInCompressor& c)
        : ModernEffectEditor(c, { BuiltInCompressor::ThresholdDb, BuiltInCompressor::Ratio,
                                  BuiltInCompressor::AttackMs, BuiltInCompressor::ReleaseMs,
                                  BuiltInCompressor::MakeupDb }),
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
        // 基底 ctor の resized() は派生 vtable 確定前に走り layoutOverlay が効かないため、
        // ここで再レイアウトしてオーバーレイ (オートゲイン トグル) を配置する。
        resized();
    }

private:
    static constexpr float kDMin = -60.0f, kDMax = 0.0f;
    static constexpr float kMeterW = 26.0f;
    BuiltInCompressor& comp;
    juce::TextButton autoBtn;
    int dragMode { -1 };   // -1=なし, 0=threshold, 1=ratio

    juce::Rectangle<float> curveArea(juce::Rectangle<float> a) const { return a.reduced(8.0f).withTrimmedRight(kMeterW); }
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
        if (auto* k = knobForParam(BuiltInCompressor::MakeupDb)) k->setEnabled(! on);   // 自動時は手動 Makeup を無効
    }

    void onTick() override { updateAutoState(); }

    void layoutOverlay(juce::Rectangle<int> g) override
    {
        // グラフ外 (上部バーの左) に配置する
        autoBtn.setBounds(g.getX(), juce::jmax(6, g.getY() - 27), 116, 22);
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

        // トランスファーカーブ (メイクアップ込み)
        juce::Path curve;
        for (int px = 0; px <= (int) c.getWidth(); ++px)
        {
            const float inDb = inFromX(c, c.getX() + (float) px);
            const float x = c.getX() + (float) px;
            const float y = yFromOut(c, outAt(inDb));
            if (px == 0) curve.startNewSubPath(x, y); else curve.lineTo(x, y);
        }
        g.setColour(AppColours::accent);
        g.strokePath(curve, juce::PathStrokeType(2.0f));

        // しきい値ノード (白丸)
        const float thr = comp.getP(BuiltInCompressor::ThresholdDb);
        const float ax = xFromIn(c, thr), ay = yFromOut(c, outAt(thr));
        g.setColour(juce::Colours::white);
        g.fillEllipse(ax - 6, ay - 6, 12, 12);
        g.setColour(AppColours::accent);
        g.drawEllipse(ax - 6, ay - 6, 12, 12, 2.0f);

        // レシオハンドル (右端・input 0dB の出力位置。アクセント丸)
        const float bx = xFromIn(c, 0.0f), by = yFromOut(c, outAt(0.0f));
        g.setColour(AppColours::accent);
        g.fillEllipse(bx - 6, by - 6, 12, 12);
        g.setColour(juce::Colours::white);
        g.drawEllipse(bx - 6, by - 6, 12, 12, 2.0f);

        // GR メータ
        auto m = juce::Rectangle<float>(area.getRight() - kMeterW + 4, area.getY() + 6, kMeterW - 8, area.getHeight() - 24);
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
        g.drawText("GR", (int) (area.getRight() - kMeterW + 2), (int) (area.getBottom() - 16), (int) kMeterW, 14, juce::Justification::centred);
    }

    void onGraphMouseDown(const juce::MouseEvent& e) override
    {
        const auto c = curveArea(graph.toFloat());
        const float thr = comp.getP(BuiltInCompressor::ThresholdDb);
        const juce::Point<float> a(xFromIn(c, thr),  yFromOut(c, outAt(thr)));
        const juce::Point<float> b(xFromIn(c, 0.0f), yFromOut(c, outAt(0.0f)));
        const float dA = e.position.getDistanceFrom(a);
        const float dB = e.position.getDistanceFrom(b);
        // ノードを実際に掴んだ時だけ動かす (メータや余白のクリックでは値を変えない)
        if (juce::jmin(dA, dB) > 22.0f) { dragMode = -1; return; }
        dragMode = (dB < dA) ? 1 : 0;
        onGraphMouseDrag(e);
    }

    void onGraphMouseUp(const juce::MouseEvent&) override { dragMode = -1; }

    void onGraphMouseDrag(const juce::MouseEvent& e) override
    {
        if (dragMode < 0) return;
        const auto c = curveArea(graph.toFloat());
        if (dragMode == 1)   // レシオ: input 0dB の出力位置から傾きを逆算
        {
            const float thr = comp.getP(BuiltInCompressor::ThresholdDb);
            const float over = juce::jmax(1.0f, -thr);
            const float yOut = outFromY(c, e.position.y);
            const float slope = juce::jlimit(0.0f, 0.95f, (comp.currentMakeupDb() - yOut) / over);
            const float ratio = 1.0f / (1.0f - slope);
            const auto& pi = comp.getParamInfo(BuiltInCompressor::Ratio);
            comp.setP(BuiltInCompressor::Ratio, juce::jlimit(pi.minV, pi.maxV, ratio));
        }
        else                 // しきい値: 横位置
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
