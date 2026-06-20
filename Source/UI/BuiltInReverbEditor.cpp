// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

#include "ModernEffectEditor.h"
#include "../AppColours.h"
#include "../Localisation.h"
#include "../Audio/builtin/BuiltInReverb.h"
#include <cmath>

// リバーブのモダン UI: 残響の**ディケイ (時間 vs レベル) カーブ**をリッチに描く。
// メインの減衰 + ダンピングで早く減衰する高域の 2 本、時間/dB 目盛り、推定残響時間。
// 右に**ライブ出力メーター** (実際にテールが鳴っているのが分かる)。
class BuiltInReverbEditor : public ModernEffectEditor
{
public:
    explicit BuiltInReverbEditor(BuiltInReverb& r)
        : ModernEffectEditor(r, { BuiltInReverb::MixPct, BuiltInReverb::Size,
                                  BuiltInReverb::Damping, BuiltInReverb::Width }, 220),
          rev(r)
    {
        setSize(juce::jmax(560, getWidth()), getHeight());
        resized();
    }

private:
    static constexpr float kMeterW = 28.0f, kScaleW = 26.0f, kTimeH = 14.0f;
    static constexpr float kMinDb = -60.0f, kMaxDb = 0.0f;
    const juce::Colour kBlue { 0xff2f7fb0 };
    const juce::Colour kCyan { 0xff37c8d4 };

    BuiltInReverb& rev;
    float smOut { -100.0f };

    float rt60() const { return juce::jmap(juce::jlimit(0.0f, 1.0f, rev.getP(BuiltInReverb::Size)), 0.0f, 1.0f, 0.25f, 5.0f); }
    float maxTime() const { return juce::jlimit(0.5f, 6.0f, rt60() * 1.2f); }

    juce::Rectangle<float> plotArea(juce::Rectangle<float> a) const
    {
        return a.reduced(8.0f).withTrimmedLeft((int) kScaleW).withTrimmedRight((int) kMeterW + 4).withTrimmedBottom((int) kTimeH);
    }

    static float smoothMeter(float prev, float target) { return target > prev ? target : prev + (target - prev) * 0.2f; }
    void onTick() override { smOut = smoothMeter(smOut, rev.getOutputDb()); }

    void paintGraph(juce::Graphics& g, juce::Rectangle<float> area) override
    {
        const auto p = plotArea(area);
        const float mix  = juce::jlimit(0.0f, 1.0f, rev.getP(BuiltInReverb::MixPct) * 0.01f);
        const float damp = juce::jlimit(0.0f, 1.0f, rev.getP(BuiltInReverb::Damping));
        const float rt   = rt60();
        const float tMax = maxTime();
        const float wetTopDb = 20.0f * std::log10(juce::jmax(mix, 0.001f));   // 濃いほどテールが高い

        auto timeToX = [&](float t) { return p.getX() + (t / tMax) * p.getWidth(); };
        auto dbToY   = [&](float db) { return p.getBottom() - (juce::jlimit(kMinDb, kMaxDb, db) - kMinDb) / (kMaxDb - kMinDb) * p.getHeight(); };

        // dB 目盛り
        g.setFont(9.5f);
        for (int db = 0; db >= -60; db -= 12)
        {
            const float y = dbToY((float) db);
            g.setColour(AppColours::rulerLine.withAlpha(0.20f));
            g.drawHorizontalLine((int) y, p.getX(), p.getRight());
            g.setColour(AppColours::textDim);
            g.drawText(juce::String(db), (int) area.getX() + 2, (int) y - 6, (int) kScaleW - 2, 12, juce::Justification::right);
        }
        // 時間目盛り (tMax に応じた間隔)
        const float step = tMax <= 1.0f ? 0.2f : tMax <= 3.0f ? 0.5f : 1.0f;
        for (float t = 0.0f; t <= tMax + 1.0e-3f; t += step)
        {
            const float x = timeToX(t);
            g.setColour(AppColours::rulerLine.withAlpha(0.20f));
            g.drawVerticalLine((int) x, p.getY(), p.getBottom());
            g.setColour(AppColours::textDim);
            g.drawText(juce::String(t, t < 1.0f ? 1 : (step < 1.0f ? 1 : 0)) + "s",
                       (int) x - 16, (int) p.getBottom() + 1, 32, 13, juce::Justification::centred);
        }

        // ディケイカーブ: level(t) = wetTop - 60*(t/RT60)。メイン + ダンピングで早く減衰する高域。
        auto decayPath = [&](float rt60s, bool fillToBottom)
        {
            juce::Path path;
            const int W = (int) p.getWidth();
            if (fillToBottom) path.startNewSubPath(p.getX(), p.getBottom());
            for (int px = 0; px <= W; ++px)
            {
                const float t  = (float) px / juce::jmax(1, W) * tMax;
                const float db = wetTopDb - 60.0f * (t / juce::jmax(0.05f, rt60s));
                const float x = p.getX() + (float) px, y = dbToY(db);
                if (! fillToBottom && px == 0) path.startNewSubPath(x, y);
                else                           path.lineTo(x, y);
            }
            if (fillToBottom) { path.lineTo(p.getRight(), p.getBottom()); path.closeSubPath(); }
            return path;
        };

        juce::ColourGradient grad(kBlue.withAlpha(0.55f), p.getX(), p.getY(), kBlue.withAlpha(0.08f), p.getX(), p.getBottom(), false);
        g.setGradientFill(grad);
        g.fillPath(decayPath(rt, true));

        const float rtHi = rt * (1.0f - damp * 0.7f);   // 高域はダンピングで早く減衰
        g.setColour(juce::Colours::white.withAlpha(0.30f));
        g.strokePath(decayPath(rtHi, false), juce::PathStrokeType(1.2f));   // 高域 (薄)
        g.setColour(kCyan);
        g.strokePath(decayPath(rt, false), juce::PathStrokeType(2.0f));     // メイン

        // 推定残響時間の表示
        g.setColour(AppColours::textDim);
        g.setFont(11.0f);
        g.drawText(tr(u8"残響") + " " + juce::String(rt, 1) + "s",
                   (int) p.getRight() - 84, (int) p.getY() + 4, 80, 14, juce::Justification::right);

        // ライブ出力メーター (右)
        auto m = juce::Rectangle<float>(area.getRight() - kMeterW + 4, area.getY() + 6, kMeterW - 8, juce::jmax(1.0f, area.getHeight() - 24));
        g.setColour(AppColours::meterBg);
        g.fillRoundedRectangle(m, 2.0f);
        const float frac = juce::jlimit(0.0f, 1.0f, (smOut - kMinDb) / (kMaxDb - kMinDb));
        if (frac > 0.001f)
        {
            auto fillR = m.withTop(m.getBottom() - m.getHeight() * frac);
            juce::ColourGradient mg(kCyan, m.getX(), m.getBottom(), kCyan.brighter(0.4f), m.getX(), m.getY(), false);
            g.setGradientFill(mg);
            g.fillRoundedRectangle(fillR, 2.0f);
        }
        g.setColour(AppColours::textDim);
        g.setFont(10.0f);
        g.drawText("OUT", (int) (area.getRight() - kMeterW + 2), (int) (area.getBottom() - 16), (int) kMeterW, 14, juce::Justification::centred);
    }
};

juce::AudioProcessorEditor* BuiltInReverb::createEditor()
{
    return new BuiltInReverbEditor(*this);
}
