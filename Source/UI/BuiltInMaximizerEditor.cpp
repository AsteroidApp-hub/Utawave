// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

#include "ModernEffectEditor.h"
#include "../AppColours.h"
#include "../Localisation.h"
#include "../Audio/builtin/BuiltInMaximizer.h"
#include <cmath>
#include <vector>

// マキシマイザーのイケイケ UI: 左に IN/GR/OUT メーター、右に「入出力レベルの流れる
// スクロール・スコープ」をネオン発光で描く。出力 (シアン) がシーリングのオレンジ線を
// 絶対に越えない様子が一目で分かる (ブリックウォールの可視化)。ドライブを上げるほど
// 出力がシーリングに張り付き、左下に光る LOUD バッジと GR 値が出る。
// シーリング線は縦ドラッグで直接調整できる。
class BuiltInMaximizerEditor : public ModernEffectEditor
{
public:
    explicit BuiltInMaximizerEditor(BuiltInMaximizer& m)
        : ModernEffectEditor(m, { BuiltInMaximizer::DriveDb, BuiltInMaximizer::CeilingDb }, 224),
          maxr(m)
    {
        inHist.assign((size_t) kHistN, kMinDb);
        outHist.assign((size_t) kHistN, kMinDb);
        setSize(juce::jmax(600, getWidth()), getHeight());
        resized();
    }

private:
    static constexpr int   kHistN  = 200;     // スコープの履歴点数 (30Hz → 約 6.6 秒)
    static constexpr float kMetersW = 132.0f;
    static constexpr float kMinDb = -48.0f, kMaxDb = 6.0f;   // 縦軸 (メーター/スコープ共通)
    const juce::Colour kCyan { AppColours::fxCyan };
    const juce::Colour kBlue { AppColours::fxBlue };
    const juce::Colour kAmber { AppColours::accent };

    BuiltInMaximizer& maxr;
    std::vector<float> inHist, outHist;
    int   histPos { 0 };
    float smIn { -100.0f }, smOut { -100.0f };
    float loudGlow { 0.0f };
    bool  dragCeiling { false };

    juce::Rectangle<float> scopeArea(juce::Rectangle<float> a) const { return a.withTrimmedLeft((int) kMetersW).reduced(8.0f); }
    float yFromDb(juce::Rectangle<float> c, float db) const
    {
        return c.getBottom() - (juce::jlimit(kMinDb, kMaxDb, db) - kMinDb) / (kMaxDb - kMinDb) * c.getHeight();
    }
    float dbFromY(juce::Rectangle<float> c, float y) const
    {
        return juce::jlimit(kMinDb, kMaxDb, kMinDb + (c.getBottom() - y) / juce::jmax(1.0f, c.getHeight()) * (kMaxDb - kMinDb));
    }

    static float smoothMeter(float prev, float target) { return target > prev ? target : prev + (target - prev) * 0.25f; }
    void onTick() override
    {
        smIn  = smoothMeter(smIn,  maxr.getInputDb());
        smOut = smoothMeter(smOut, maxr.getOutputDb());
        // 履歴へ追記 (リング)。生のブロックピークを流して「波打つ」見た目にする。
        inHist [(size_t) histPos] = maxr.getInputDb();
        outHist[(size_t) histPos] = maxr.getOutputDb();
        if (++histPos >= kHistN) histPos = 0;
        // GR が大きいほど LOUD バッジが光る
        const float t = juce::jlimit(0.0f, 1.0f, smoothedReductionDb() / 6.0f);
        loudGlow = loudGlow + (t - loudGlow) * 0.25f;
    }

    // 履歴をなめらかな塗り + 上辺ネオンラインで描く (発光は重ね描きで表現)
    void drawHistory(juce::Graphics& g, juce::Rectangle<float> c, const std::vector<float>& hist,
                     juce::Colour col, float fillAlpha, bool glow) const
    {
        juce::Path line, fill;
        fill.startNewSubPath(c.getX(), c.getBottom());
        for (int i = 0; i < kHistN; ++i)
        {
            // 最新を右端に置く: histPos が次の書込み位置 = 最古
            const int idx = (histPos + i) % kHistN;
            const float x = c.getX() + (float) i / (float) (kHistN - 1) * c.getWidth();
            const float y = yFromDb(c, hist[(size_t) idx]);
            if (i == 0) line.startNewSubPath(x, y); else line.lineTo(x, y);
            fill.lineTo(x, y);
        }
        fill.lineTo(c.getRight(), c.getBottom());
        fill.closeSubPath();

        juce::ColourGradient grad(col.withAlpha(fillAlpha), c.getX(), c.getY(),
                                  col.withAlpha(fillAlpha * 0.12f), c.getX(), c.getBottom(), false);
        g.setGradientFill(grad);
        g.fillPath(fill);

        if (glow)
        {
            g.setColour(col.withAlpha(0.25f));
            g.strokePath(line, juce::PathStrokeType(5.0f));
            g.setColour(col.withAlpha(0.5f));
            g.strokePath(line, juce::PathStrokeType(2.6f));
        }
        g.setColour(col);
        g.strokePath(line, juce::PathStrokeType(glow ? 1.6f : 1.1f));
    }

    void paintGraph(juce::Graphics& g, juce::Rectangle<float> area) override
    {
        // ── メーター域 (左): IN / GR / OUT ──
        auto m = area.withWidth(kMetersW).reduced(8.0f).withTrimmedBottom(14);
        const float scaleW = 24.0f;
        const float barW   = 22.0f;
        const float x0 = m.getX() + scaleW;
        const float gap = (m.getWidth() - scaleW - barW * 3.0f) / 2.0f;

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
        const juce::Rectangle<float> inR (x0,                    m.getY(), barW, m.getHeight());
        const juce::Rectangle<float> grR (x0 + barW + gap,       m.getY(), barW, m.getHeight());
        const juce::Rectangle<float> outR(x0 + (barW + gap) * 2, m.getY(), barW, m.getHeight());
        drawLevelBar(g, inR,  smIn,  kMinDb, kMaxDb, kBlue, "IN");
        // GR は上から下へ (押さえた量)。fullScale = 24dB。
        {
            g.setColour(AppColours::meterBg);
            g.fillRoundedRectangle(grR, 2.0f);
            const float frac = juce::jlimit(0.0f, 1.0f, smoothedReductionDb() / BuiltInMaximizer::kFullScaleGrDb);
            if (frac > 0.001f)
            {
                auto fillR = grR.withHeight(grR.getHeight() * frac);
                g.setColour(frac > 0.8f ? AppColours::meterRed : frac > 0.4f ? AppColours::meterYellow : kAmber);
                g.fillRoundedRectangle(fillR, 2.0f);
            }
            g.setColour(AppColours::textDim);
            g.setFont(10.0f);
            g.drawText("GR", grR.getX() - 4, grR.getBottom() + 2, grR.getWidth() + 8, 12, juce::Justification::centred);
        }
        drawLevelBar(g, outR, smOut, kMinDb, kMaxDb, kCyan, "OUT");

        // ── スコープ域 (右): 流れる入出力レベル + シーリング線 ──
        const auto c = scopeArea(area);
        g.setColour(AppColours::rulerLine.withAlpha(0.20f));
        for (int db = -42; db <= 6; db += 12)
            g.drawHorizontalLine((int) yFromDb(c, (float) db), c.getX(), c.getRight());

        drawHistory(g, c, inHist,  kBlue, 0.20f, false);   // 入力 (背面・控えめ)
        drawHistory(g, c, outHist, kCyan, 0.45f, true);    // 出力 (ネオン発光)

        // シーリング線 (オレンジ・ドラッグ可能)。出力がこの線を越えない
        const float ceilDb = maxr.getP(BuiltInMaximizer::CeilingDb);
        const float cy = yFromDb(c, ceilDb);
        // シーリングより上 (出力が到達できない禁止域) を薄いオレンジで塗ってブリックウォールを示す
        g.setColour(kAmber.withAlpha(0.10f));
        g.fillRect(juce::Rectangle<float>(c.getX(), c.getY(), c.getWidth(), juce::jmax(0.0f, cy - c.getY())));
        g.setColour(kAmber);
        g.drawLine(c.getX(), cy, c.getRight(), cy, dragCeiling ? 2.4f : 1.6f);
        g.setColour(kAmber);
        g.fillRoundedRectangle(c.getRight() - 30.0f, cy - 7.0f, 28.0f, 14.0f, 3.0f);
        g.setColour(juce::Colours::black);
        g.setFont(juce::Font(9.5f, juce::Font::bold));
        g.drawText(juce::String(ceilDb, 1), (int) (c.getRight() - 30.0f), (int) (cy - 7.0f), 28, 14, juce::Justification::centred);

        // LOUD バッジ + GR 値 (左下・GR に応じて発光)
        const float gr = smoothedReductionDb();
        if (loudGlow > 0.02f)
        {
            auto badge = juce::Rectangle<float>(c.getX() + 8.0f, c.getBottom() - 26.0f, 58.0f, 18.0f);
            g.setColour(kAmber.withAlpha(0.25f + 0.55f * loudGlow));
            g.fillRoundedRectangle(badge.expanded(2.0f * loudGlow), 4.0f);
            g.setColour(juce::Colours::white.withAlpha(0.6f + 0.4f * loudGlow));
            g.setFont(juce::Font(11.0f, juce::Font::bold));
            g.drawText("LOUD", badge, juce::Justification::centred);
        }
        g.setColour(AppColours::textDim);
        g.setFont(10.0f);
        g.drawText(tr(u8"押さえ込み") + " " + juce::String(gr, 1) + " dB",
                   (int) (c.getX() + 8), (int) (c.getY() + 4), 180, 14, juce::Justification::left);
    }

    void onGraphMouseDown(const juce::MouseEvent& e) override
    {
        const auto c = scopeArea(graph.toFloat());
        // スコープ域内の縦ドラッグでシーリングを掴む (どこを掴んでも y がシーリングになる)
        dragCeiling = c.expanded(0.0f, 6.0f).contains(e.position);
        if (dragCeiling) onGraphMouseDrag(e);
    }

    void onGraphMouseUp(const juce::MouseEvent&) override { dragCeiling = false; }

    void onGraphMouseDrag(const juce::MouseEvent& e) override
    {
        if (! dragCeiling) return;
        const auto c = scopeArea(graph.toFloat());
        const auto& pi = maxr.getParamInfo(BuiltInMaximizer::CeilingDb);
        maxr.setP(BuiltInMaximizer::CeilingDb, juce::jlimit(pi.minV, pi.maxV, dbFromY(c, e.position.y)));
        clearPresetSelection();
        repaint(graph);
    }
};

juce::AudioProcessorEditor* BuiltInMaximizer::createEditor()
{
    return new BuiltInMaximizerEditor(*this);
}
