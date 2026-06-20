// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

#include "BuiltInEQEditor.h"
#include "../Localisation.h"
#include "../AppColours.h"
#include <cmath>

static_assert(SpectrumScope::N == BuiltInEQ::kAnalyzerSize, "SpectrumScope/analyzer size mismatch");

namespace
{
    constexpr int   kTopH   = 34;
    constexpr int   kMargin = 12;
    const juce::Colour kBg      { 0xff141414 };
    const juce::Colour kGraphBg { 0xff1c1c1c };

    const juce::Colour kHpfCol   { 0xff4aa3ff };
    const juce::Colour kLowCol   { 0xff3da85a };
    const juce::Colour kLoMidCol { 0xff2fb0a0 };
    const juce::Colour kMidCol   { 0xffe06422 };  // accent
    const juce::Colour kHiMidCol { 0xffe05a8a };
    const juce::Colour kAirCol   { 0xffc060ff };
}

BuiltInEQEditor::BuiltInEQEditor(BuiltInEQ& e)
    : juce::AudioProcessorEditor(e), eq(e)
{
    setLookAndFeel(&laf);

    nodes = {
        { BuiltInEQ::HpfHz,   -1,                 -1,                kHpfCol   },
        { BuiltInEQ::LowHz,   BuiltInEQ::LowDb,   BuiltInEQ::LowQ,   kLowCol   },
        { BuiltInEQ::LoMidHz, BuiltInEQ::LoMidDb, BuiltInEQ::LoMidQ, kLoMidCol },
        { BuiltInEQ::MidHz,   BuiltInEQ::MidDb,   BuiltInEQ::MidQ,   kMidCol   },
        { BuiltInEQ::HiMidHz, BuiltInEQ::HiMidDb, BuiltInEQ::HiMidQ, kHiMidCol },
        { BuiltInEQ::AirHz,   BuiltInEQ::AirDb,   -1,                kAirCol   },
    };

    // プリセット
    const auto& presets = eq.getPresets();
    for (int i = 0; i < (int) presets.size(); ++i)
        presetBox.addItem(tr(presets[(size_t) i].name), 100 + i);
    presetBox.setTextWhenNothingSelected(tr(u8"プリセット"));
    presetBox.onChange = [this]
    {
        const int idx = presetBox.getSelectedId() - 100;
        if (idx >= 0) { eq.applyPreset(idx); curveDirty = true; repaint(); }
    };
    addAndMakeVisible(presetBox);

    // RESET (全バンドをフラットへ)
    resetBtn.setButtonText(tr(u8"リセット"));
    resetBtn.setColour(juce::TextButton::buttonColourId,  AppColours::buttonBg);
    resetBtn.setColour(juce::TextButton::textColourOffId, AppColours::text);
    resetBtn.onClick = [this]
    {
        eq.applyPreset(0);   // フラット (preset 0)
        presetBox.setSelectedId(100, juce::dontSendNotification);
        curveDirty = true;
        repaint();
    };
    addAndMakeVisible(resetBtn);

    // SPECTRUM 表示 ON/OFF
    spectrumBtn.setButtonText(tr(u8"スペクトラム"));
    spectrumBtn.setClickingTogglesState(true);
    spectrumBtn.setToggleState(true, juce::dontSendNotification);
    spectrumBtn.setColour(juce::TextButton::buttonColourId,   AppColours::buttonBg);
    spectrumBtn.setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xff2f9e44));
    spectrumBtn.setColour(juce::TextButton::textColourOffId,  AppColours::text);
    spectrumBtn.setColour(juce::TextButton::textColourOnId,   juce::Colours::white);
    spectrumBtn.onClick = [this]
    {
        spectrumOn = spectrumBtn.getToggleState();
        eq.setAnalyzerActive(spectrumOn);
        repaint();
    };
    addAndMakeVisible(spectrumBtn);

    eq.setAnalyzerActive(true);
    setSize(720, 420);
    startTimerHz(30);
}

juce::String BuiltInEQEditor::formatFreq(double hz)
{
    if (hz >= 10000.0) return juce::String(hz / 1000.0, 0) + " kHz";
    if (hz >= 1000.0)  return juce::String(hz / 1000.0, 2) + " kHz";
    return juce::String((int) std::round(hz)) + " Hz";
}

BuiltInEQEditor::~BuiltInEQEditor()
{
    stopTimer();   // 破棄中に timerCallback が走らないよう先に止める
    eq.setAnalyzerActive(false);
    setLookAndFeel(nullptr);
}

// ─── 座標変換 ───
float BuiltInEQEditor::freqToX(double hz) const
{
    hz = juce::jlimit((double) kFMin, (double) kFMax, hz);
    const double t = std::log(hz / kFMin) / std::log((double) kFMax / kFMin);
    return (float) (graph.getX() + t * graph.getWidth());
}
double BuiltInEQEditor::xToFreq(float x) const
{
    const double t = juce::jlimit(0.0, 1.0, (double) (x - graph.getX()) / juce::jmax(1, graph.getWidth()));
    return kFMin * std::pow((double) kFMax / kFMin, t);
}
float BuiltInEQEditor::gainToY(double db) const
{
    const double t = juce::jlimit(0.0, 1.0, (db + kGMax) / (2.0 * kGMax));
    return (float) (graph.getBottom() - t * graph.getHeight());
}
double BuiltInEQEditor::yToGain(float y) const
{
    const double t = juce::jlimit(0.0, 1.0, (double) (graph.getBottom() - y) / juce::jmax(1, graph.getHeight()));
    return -kGMax + t * 2.0 * kGMax;
}

juce::Point<float> BuiltInEQEditor::nodePos(const Node& nd) const
{
    const float x = freqToX(eq.getP(nd.freqParam));
    const float y = gainToY(nd.gainParam >= 0 ? eq.getP(nd.gainParam) : 0.0);
    return { x, y };
}

int BuiltInEQEditor::hitTestNode(juce::Point<float> p) const
{
    int best = -1; float bestD = 18.0f;
    for (int i = 0; i < (int) nodes.size(); ++i)
    {
        const float d = p.getDistanceFrom(nodePos(nodes[(size_t) i]));
        if (d < bestD) { bestD = d; best = i; }
    }
    return best;
}

void BuiltInEQEditor::resized()
{
    auto area = getLocalBounds();
    auto top = area.removeFromTop(kTopH).reduced(kMargin, 5);
    resetBtn.setBounds(top.removeFromLeft(60));
    top.removeFromLeft(6);
    spectrumBtn.setBounds(top.removeFromLeft(104));
    presetBox.setBounds(top.removeFromRight(170));
    graph = area.reduced(kMargin).withTrimmedTop(0);
    curveDirty = true;   // graph 座標が変わったのでカーブ再計算
}

void BuiltInEQEditor::timerCallback()
{
    if (spectrumOn) scope.update([this](float* d, int n) { return eq.readAnalyzerSamples(d, n); });
    repaint(graph);
}

void BuiltInEQEditor::rebuildCurveIfNeeded()
{
    if (! curveDirty || graph.isEmpty()) return;
    curveDirty = false;

    const auto gf = graph.toFloat();
    const int w = juce::jmax(2, graph.getWidth());
    std::vector<double> freqs((size_t) w), db((size_t) w);
    for (int i = 0; i < w; ++i)
        freqs[(size_t) i] = xToFreq(gf.getX() + (float) i);
    eq.getMagnitudeResponse(freqs.data(), db.data(), w);

    const float y0 = gainToY(0.0);
    cachedCurve.clear();
    cachedCurveFill.clear();
    for (int i = 0; i < w; ++i)
    {
        const float x = gf.getX() + (float) i;
        const float y = gainToY(db[(size_t) i]);
        if (i == 0) { cachedCurve.startNewSubPath(x, y); cachedCurveFill.startNewSubPath(x, y0); cachedCurveFill.lineTo(x, y); }
        else        { cachedCurve.lineTo(x, y); cachedCurveFill.lineTo(x, y); }
    }
    cachedCurveFill.lineTo(gf.getRight() - 1.0f, y0);
    cachedCurveFill.closeSubPath();
}

void BuiltInEQEditor::paint(juce::Graphics& g)
{
    g.fillAll(kBg);
    if (graph.isEmpty()) return;

    const auto gf = graph.toFloat();
    g.setColour(kGraphBg);
    g.fillRoundedRectangle(gf, 4.0f);

    const double sr = eq.getSampleRateForUi();

    // ── グリッド ──
    g.saveState();
    g.reduceClipRegion(graph);

    static const double freqLines[] = { 20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000 };
    g.setFont(10.0f);
    for (double f : freqLines)
    {
        const float x = freqToX(f);
        g.setColour(AppColours::rulerLine.withAlpha(0.30f));
        g.drawVerticalLine((int) x, gf.getY(), gf.getBottom());
        g.setColour(AppColours::textDim);
        const juce::String lbl = f >= 1000 ? (juce::String((int) (f / 1000)) + "k") : juce::String((int) f);
        g.drawText(lbl, (int) x - 16, graph.getBottom() - 14, 32, 13, juce::Justification::centred);
    }
    for (int db = -12; db <= 12; db += 6)
    {
        const float y = gainToY(db);
        g.setColour(db == 0 ? AppColours::rulerLine.withAlpha(0.6f) : AppColours::rulerLine.withAlpha(0.28f));
        g.drawHorizontalLine((int) y, gf.getX(), gf.getRight());
        if (db != 0)
        {
            g.setColour(AppColours::textDim);
            g.drawText(juce::String(db > 0 ? "+" : "") + juce::String(db),
                       graph.getX() + 3, (int) y - 13, 30, 13, juce::Justification::left);
        }
    }

    // ── アナライザー (背面・緑) ── 共通ヘルパで対数軸の滑らかな塗り + 上辺ライン
    if (spectrumOn)
    {
        const juce::Colour green { 0xff8fdf6a };
        scope.draw(g, gf, sr, kFMin, kFMax, kAnMinDb, kAnMaxDb, green, green.withAlpha(0.75f));
    }

    // ── EQ 周波数特性カーブ (0dB 線との間を塗り + 白ライン) ──
    // カーブはパラメータ/サイズ変化時のみ再計算 (アナライザーの 30Hz アニメで毎回再評価しない)。
    rebuildCurveIfNeeded();
    g.setColour(juce::Colours::white.withAlpha(0.10f));
    g.fillPath(cachedCurveFill);
    g.setColour(juce::Colours::white.withAlpha(0.95f));
    g.strokePath(cachedCurve, juce::PathStrokeType(2.0f));

    g.restoreState();

    // ── バンドノード (番号付き・バンド色) ──
    g.setFont(juce::Font(11.0f, juce::Font::bold));
    for (int i = 0; i < (int) nodes.size(); ++i)
    {
        const auto p = nodePos(nodes[(size_t) i]);
        const bool hot = (i == dragNode || i == hoverNode);
        const float r = hot ? 10.0f : 8.0f;
        g.setColour(nodes[(size_t) i].colour.withAlpha(hot ? 1.0f : 0.9f));
        g.fillEllipse(p.x - r, p.y - r, r * 2, r * 2);
        g.setColour(juce::Colours::white.withAlpha(hot ? 1.0f : 0.7f));
        g.drawEllipse(p.x - r, p.y - r, r * 2, r * 2, 1.5f);
        g.setColour(juce::Colours::white);
        g.drawText(juce::String(i + 1), juce::Rectangle<float>(p.x - r, p.y - r, r * 2, r * 2),
                   juce::Justification::centred);
    }

    // ── ドラッグ/ホバー中のノードの数値表示 ──
    const int show = dragNode >= 0 ? dragNode : hoverNode;
    if (show >= 0 && show < (int) nodes.size())
    {
        const auto& nd = nodes[(size_t) show];
        juce::String txt = formatFreq(eq.getP(nd.freqParam));
        if (nd.gainParam >= 0)
            txt += "   " + juce::String(eq.getP(nd.gainParam), 1) + " dB";
        if (nd.qParam >= 0)
            txt += "   Q " + juce::String(eq.getP(nd.qParam), 2);

        g.setFont(12.0f);
        const int tw = juce::jmax(72, g.getCurrentFont().getStringWidth(txt) + 16);
        const auto p = nodePos(nd);
        // lo<=hi を保証 (狭ウィンドウで getRight()-tw が getX() を下回っても jlimit を壊さない)
        const float bxHi = juce::jmax((float) graph.getX(), (float) graph.getRight() - tw);
        const float byHi = juce::jmax((float) graph.getY(), (float) graph.getBottom() - 22);
        float bx = juce::jlimit((float) graph.getX(), bxHi, p.x - tw * 0.5f);
        float by = juce::jlimit((float) graph.getY(), byHi, p.y - 30.0f);
        juce::Rectangle<float> box(bx, by, (float) tw, 20.0f);
        g.setColour(juce::Colour(0xee0a0a0a));
        g.fillRoundedRectangle(box, 4.0f);
        g.setColour(nd.colour);
        g.drawRoundedRectangle(box, 4.0f, 1.0f);
        g.setColour(juce::Colours::white);
        g.drawText(txt, box, juce::Justification::centred);
    }
}

// ─── マウス操作 (ノードドラッグ) ───
void BuiltInEQEditor::mouseDown(const juce::MouseEvent& e)
{
    dragNode = hitTestNode(e.position);
    if (dragNode >= 0) mouseDrag(e);
}

void BuiltInEQEditor::mouseDrag(const juce::MouseEvent& e)
{
    if (dragNode < 0) return;
    const auto& nd = nodes[(size_t) dragNode];

    const auto& fi = eq.getParamInfo(nd.freqParam);
    eq.setP(nd.freqParam, juce::jlimit(fi.minV, fi.maxV, (float) xToFreq(e.position.x)));

    if (nd.gainParam >= 0)
    {
        const auto& gi = eq.getParamInfo(nd.gainParam);
        eq.setP(nd.gainParam, juce::jlimit(gi.minV, gi.maxV, (float) yToGain(e.position.y)));
    }
    presetBox.setSelectedId(0, juce::dontSendNotification);   // 手動操作 = プリセット非選択
    curveDirty = true;
    repaint();
}

void BuiltInEQEditor::mouseUp(const juce::MouseEvent&)
{
    dragNode = -1;
}

void BuiltInEQEditor::mouseMove(const juce::MouseEvent& e)
{
    const int h = hitTestNode(e.position);
    if (h != hoverNode)
    {
        hoverNode = h;
        setMouseCursor(h >= 0 ? juce::MouseCursor::DraggingHandCursor : juce::MouseCursor::NormalCursor);
        repaint(graph);
    }
}

// ノード上でホイール → そのバンドの Q を調整 (ピークのみ。乗算で対数的な操作感)
void BuiltInEQEditor::mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    const int h = hitTestNode(e.position);
    if (h < 0) return;
    const auto& nd = nodes[(size_t) h];
    if (nd.qParam < 0) return;   // HPF / シェルフは Q なし

    const auto& qi = eq.getParamInfo(nd.qParam);
    const float q = eq.getP(nd.qParam) * std::exp(wheel.deltaY * 1.6f);
    eq.setP(nd.qParam, juce::jlimit(qi.minV, qi.maxV, q));

    hoverNode = h;                 // 数値表示を出す
    curveDirty = true;
    presetBox.setSelectedId(0, juce::dontSendNotification);
    repaint();
}

// 内蔵 EQ の専用エディタを返す (UI 依存をこの TU に閉じ込める)
juce::AudioProcessorEditor* BuiltInEQ::createEditor()
{
    return new BuiltInEQEditor(*this);
}
