// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

#include "BuiltInEQEditor.h"
#include "../Localisation.h"
#include "../AppColours.h"
#include <cmath>

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
        { BuiltInEQ::HpfHz,   -1,                 kHpfCol   },
        { BuiltInEQ::LowHz,   BuiltInEQ::LowDb,   kLowCol   },
        { BuiltInEQ::LoMidHz, BuiltInEQ::LoMidDb, kLoMidCol },
        { BuiltInEQ::MidHz,   BuiltInEQ::MidDb,   kMidCol   },
        { BuiltInEQ::HiMidHz, BuiltInEQ::HiMidDb, kHiMidCol },
        { BuiltInEQ::AirHz,   BuiltInEQ::AirDb,   kAirCol   },
    };

    // Hann 窓
    for (int i = 0; i < BuiltInEQ::kAnalyzerSize; ++i)
        window[(size_t) i] = 0.5f - 0.5f * std::cos(2.0f * juce::MathConstants<float>::pi
                                                    * (float) i / (float) (BuiltInEQ::kAnalyzerSize - 1));
    binMag.assign((size_t) (BuiltInEQ::kAnalyzerSize / 2 + 1), 0.0f);
    binMagPrefix.assign((size_t) (BuiltInEQ::kAnalyzerSize / 2 + 2), 0.0f);

    // プリセット
    const auto& presets = eq.getPresets();
    for (int i = 0; i < (int) presets.size(); ++i)
        presetBox.addItem(tr(presets[(size_t) i].name), 100 + i);
    presetBox.setTextWhenNothingSelected(tr(u8"プリセット"));
    presetBox.onChange = [this]
    {
        const int idx = presetBox.getSelectedId() - 100;
        if (idx >= 0) { eq.applyPreset(idx); repaint(); }
    };
    addAndMakeVisible(presetBox);

    eq.setAnalyzerActive(true);
    setSize(620, 380);
    startTimerHz(30);
}

BuiltInEQEditor::~BuiltInEQEditor()
{
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
    auto top = area.removeFromTop(kTopH);
    presetBox.setBounds(top.removeFromRight(180).reduced(kMargin, 5));
    graph = area.reduced(kMargin).withTrimmedTop(0);
}

void BuiltInEQEditor::timerCallback()
{
    updateAnalyzer();
    repaint(graph);
}

void BuiltInEQEditor::updateAnalyzer()
{
    const int N = BuiltInEQ::kAnalyzerSize;
    eq.readAnalyzerSamples(samples.data(), N);
    for (int i = 0; i < N; ++i)
        samples[(size_t) i] *= window[(size_t) i];
    fft.forward(samples.data(), spec.data());

    const float scale = 4.0f / (float) N;   // 0 dBFS 正弦 ≈ 振幅 1 になる正規化
    for (int k = 0; k <= N / 2; ++k)
    {
        const float re = spec[(size_t) (2 * k)];
        const float im = spec[(size_t) (2 * k + 1)];
        const float mag = std::sqrt(re * re + im * im) * scale;   // 線形振幅
        float& s = binMag[(size_t) k];
        s = mag > s ? mag : s * 0.82f + mag * 0.18f;   // 立ち上がり即・立ち下がりゆっくり (線形で平滑)
    }
    // オクターブ平滑の範囲平均を O(1) で引くための累積和
    binMagPrefix[0] = 0.0f;
    for (int k = 0; k <= N / 2; ++k)
        binMagPrefix[(size_t) (k + 1)] = binMagPrefix[(size_t) k] + binMag[(size_t) k];
}

void BuiltInEQEditor::paint(juce::Graphics& g)
{
    g.fillAll(kBg);
    if (graph.isEmpty()) return;

    const auto gf = graph.toFloat();
    g.setColour(kGraphBg);
    g.fillRoundedRectangle(gf, 4.0f);

    const double sr = eq.getSampleRateForUi();
    const int N = BuiltInEQ::kAnalyzerSize;

    // ── グリッド ──
    g.saveState();
    g.reduceClipRegion(graph);

    static const double freqLines[] = { 30, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000 };
    g.setFont(11.0f);
    for (double f : freqLines)
    {
        const float x = freqToX(f);
        g.setColour(AppColours::rulerLine.withAlpha(0.35f));
        g.drawVerticalLine((int) x, gf.getY(), gf.getBottom());
        if (f == 100 || f == 1000 || f == 10000)
        {
            g.setColour(AppColours::textDim);
            const juce::String lbl = f >= 1000 ? (juce::String((int) (f / 1000)) + "k") : juce::String((int) f);
            g.drawText(lbl, (int) x - 16, graph.getBottom() - 15, 32, 14, juce::Justification::centred);
        }
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

    // ── アナライザー (背面) ──
    // 低域は対数軸で bin が粗く階段状になるので、1/6 オクターブの範囲平均 (累積和で O(1)) と
    // bin 間のフラクショナル補間でなめらかな線にする。塗り + 上辺ラインでリッチに見せる。
    {
        const int    w       = graph.getWidth();
        const float  span    = kAnMaxDb - kAnMinDb;
        const double binPerHz = N / juce::jmax(1.0, sr);
        const double octHalf = std::pow(2.0, 1.0 / 12.0);   // 1/6 オクターブ (両側 1/12)

        std::vector<float> ys((size_t) (w + 1));
        for (int px = 0; px <= w; ++px)
        {
            const double f    = xToFreq(gf.getX() + (float) px);
            const double fbin = f * binPerHz;

            int lo = (int) std::floor(fbin / octHalf);
            int hi = (int) std::ceil (fbin * octHalf);
            lo = juce::jlimit(0, N / 2, lo);
            hi = juce::jlimit(0, N / 2, hi);

            float mag;
            if (hi > lo)   // 帯域に複数 bin → 範囲平均
                mag = (binMagPrefix[(size_t) (hi + 1)] - binMagPrefix[(size_t) lo]) / (float) (hi - lo + 1);
            else           // 低域: 隣接 bin をフラクショナル補間
            {
                const int b0 = juce::jlimit(0, N / 2, (int) std::floor(fbin));
                const int b1 = juce::jmin(N / 2, b0 + 1);
                const float fr = (float) (fbin - b0);
                mag = binMag[(size_t) b0] * (1.0f - fr) + binMag[(size_t) b1] * fr;
            }

            const float db = juce::jlimit(kAnMinDb, kAnMaxDb, 20.0f * std::log10(mag + 1.0e-9f));
            ys[(size_t) px] = gf.getBottom() - ((db - kAnMinDb) / span) * gf.getHeight();
        }

        juce::Path fill, top;
        fill.startNewSubPath(gf.getX(), gf.getBottom());
        top.startNewSubPath(gf.getX(), ys[0]);
        for (int px = 0; px <= w; ++px)
        {
            const float x = gf.getX() + (float) px;
            fill.lineTo(x, ys[(size_t) px]);
            top.lineTo(x, ys[(size_t) px]);
        }
        fill.lineTo(gf.getRight(), gf.getBottom());
        fill.closeSubPath();

        g.setColour(AppColours::accent.withAlpha(0.16f));
        g.fillPath(fill);
        g.setColour(AppColours::accent.withAlpha(0.55f));
        g.strokePath(top, juce::PathStrokeType(1.2f));
    }

    // ── EQ 周波数特性カーブ ──
    {
        const int w = juce::jmax(2, graph.getWidth());
        std::vector<double> freqs((size_t) w), db((size_t) w);
        for (int i = 0; i < w; ++i)
            freqs[(size_t) i] = xToFreq(gf.getX() + (float) i);
        eq.getMagnitudeResponse(freqs.data(), db.data(), w);

        juce::Path curve;
        for (int i = 0; i < w; ++i)
        {
            const float x = gf.getX() + (float) i;
            const float y = gainToY(db[(size_t) i]);
            if (i == 0) curve.startNewSubPath(x, y);
            else        curve.lineTo(x, y);
        }
        g.setColour(juce::Colours::white.withAlpha(0.92f));
        g.strokePath(curve, juce::PathStrokeType(2.0f));
    }

    g.restoreState();

    // ── バンドノード ──
    for (int i = 0; i < (int) nodes.size(); ++i)
    {
        const auto p = nodePos(nodes[(size_t) i]);
        const bool hot = (i == dragNode || i == hoverNode);
        const float r = hot ? 8.0f : 6.0f;
        g.setColour(nodes[(size_t) i].colour.withAlpha(hot ? 1.0f : 0.85f));
        g.fillEllipse(p.x - r, p.y - r, r * 2, r * 2);
        g.setColour(juce::Colours::white.withAlpha(hot ? 0.95f : 0.6f));
        g.drawEllipse(p.x - r, p.y - r, r * 2, r * 2, 1.5f);
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

// 内蔵 EQ の専用エディタを返す (UI 依存をこの TU に閉じ込める)
juce::AudioProcessorEditor* BuiltInEQ::createEditor()
{
    return new BuiltInEQEditor(*this);
}
