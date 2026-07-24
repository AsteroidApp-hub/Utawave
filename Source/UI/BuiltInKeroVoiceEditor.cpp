// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

#include "ModernEffectEditor.h"
#include "../AppColours.h"
#include "../Localisation.h"
#include "../Audio/builtin/BuiltInKeroVoice.h"
#include <cmath>

// ケロケロボイスのモダン UI: 中央に**スクロールする音程トレース** (灰 = 入力ピッチ /
// シアン = スナップ後のノート段差)。背景に半音グリッド (C はラベル + 強調)。
// 右端に現在のターゲットノートを大きく表示。下部に スピード / 補正量 / トランスポーズ /
// 感度 / ミックス のノブ。
// **スケール / キーの UI は出さない (常に半音階)**: メジャー/マイナー等の音楽用語は初心者に
// 分かりにくいため非表示にした (要望 2026-07)。DSP 側の Scale/Key パラメータとスケールスナップ
// 機能は温存してあり (テストも行使)、将来「上級者向け」で再表示する場合はボタンを戻すだけ。
class BuiltInKeroVoiceEditor : public ModernEffectEditor
{
public:
    explicit BuiltInKeroVoiceEditor(BuiltInKeroVoice& k)
        : ModernEffectEditor(k, { BuiltInKeroVoice::Speed, BuiltInKeroVoice::Amount,
                                  BuiltInKeroVoice::Transpose, BuiltInKeroVoice::Sens,
                                  BuiltInKeroVoice::Mix }, 236),
          kero(k)
    {
        for (int i = 0; i < kHist; ++i) { histIn[i] = -1.0f; histOut[i] = -1.0f; }

        setSize(juce::jmax(640, getWidth()), getHeight());
        resized();
    }

private:
    static constexpr int   kHist = 220;
    static constexpr float kNoteW = 88.0f;

    BuiltInKeroVoice& kero;
    float histIn[kHist] {}, histOut[kHist] {};
    int   histPos { 0 };
    float viewCenter { 60.0f };

    static juce::String noteNameFor(float midi)
    {
        const int m = juce::roundToInt(midi);
        int pc = m % 12; if (pc < 0) pc += 12;
        return juce::String(BuiltInKeroVoice::keyLabel(pc)) + juce::String(m / 12 - 1);
    }

    void onTick() override
    {
        histIn[histPos]  = kero.getUiInputMidi();
        histOut[histPos] = kero.getUiTargetMidi();
        histPos = (histPos + 1) % kHist;
        const float out = kero.getUiTargetMidi();
        if (out >= 0.0f) viewCenter += (out - viewCenter) * 0.10f;
    }

    void paintGraph(juce::Graphics& g, juce::Rectangle<float> area) override
    {
        const juce::Colour cyan { AppColours::fxCyan };

        auto noteCol = area.removeFromRight(kNoteW);
        auto trace = area.reduced(8.0f, 6.0f);

        const float semiSpan = 8.0f;   // 表示レンジ ±8 半音
        const float pxPerSemi = trace.getHeight() / (semiSpan * 2.0f);
        auto yFor = [&] (float midi) { return trace.getCentreY() - (midi - viewCenter) * pxPerSemi; };

        // ── 半音グリッド (C はラベル + 強調) ──
        g.setFont(10.0f);
        for (int m = (int) std::ceil(viewCenter - semiSpan); m <= (int) std::floor(viewCenter + semiSpan); ++m)
        {
            int pc = m % 12; if (pc < 0) pc += 12;
            const float y = yFor((float) m);
            g.setColour(AppColours::rulerLine.withAlpha(pc == 0 ? 0.45f : 0.16f));
            g.drawHorizontalLine((int) y, trace.getX(), trace.getRight());
            if (pc == 0)
            {
                g.setColour(AppColours::textDim);
                g.drawText(noteNameFor((float) m), (int) trace.getX() + 2, (int) y - 12, 34, 11,
                           juce::Justification::left);
            }
        }

        // ── トレース (古い → 新しい を左 → 右へ) ──
        const float dx = trace.getWidth() / (float) (kHist - 1);
        auto drawTrace = [&] (const float* hist, juce::Colour col, float thickness)
        {
            juce::Path p;
            bool pen = false;
            for (int j = 0; j < kHist; ++j)
            {
                const float v = hist[(histPos + j) % kHist];
                if (v < 0.0f) { pen = false; continue; }
                const float x = trace.getX() + dx * (float) j;
                const float y = juce::jlimit(trace.getY(), trace.getBottom(), yFor(v));
                if (! pen) { p.startNewSubPath(x, y); pen = true; }
                else         p.lineTo(x, y);
            }
            g.setColour(col);
            g.strokePath(p, juce::PathStrokeType(thickness, juce::PathStrokeType::curved,
                                                 juce::PathStrokeType::rounded));
        };
        drawTrace(histIn, juce::Colours::white.withAlpha(0.35f), 1.2f);
        // 出力はネオン風に 2 度描き (グロー + 本線)
        drawTrace(histOut, cyan.withAlpha(0.22f), 5.0f);
        drawTrace(histOut, cyan, 2.2f);

        // ── 現在のターゲットノート (右端・大きく) ──
        const float outMidi = kero.getUiTargetMidi();
        const float inHz    = kero.getUiInputHz();
        auto nc = juce::Rectangle<float>(noteCol.getX(), trace.getY(), noteCol.getWidth() - 6.0f, trace.getHeight());
        if (outMidi >= 0.0f)
        {
            g.setColour(cyan.withAlpha(0.14f));
            g.fillRoundedRectangle(nc.getX(), nc.getCentreY() - 26.0f, nc.getWidth(), 52.0f, 6.0f);
            g.setColour(cyan);
            g.setFont(juce::Font(26.0f, juce::Font::bold));
            g.drawText(noteNameFor(outMidi), nc.toNearestInt().withHeight((int) nc.getHeight() - 20),
                       juce::Justification::centred);
            g.setColour(AppColours::textDim);
            g.setFont(11.0f);
            g.drawText(juce::String((int) std::lround(inHz)) + " Hz",
                       (int) nc.getX(), (int) (nc.getCentreY() + 28.0f), (int) nc.getWidth(), 14,
                       juce::Justification::centred);
        }
        else
        {
            g.setColour(AppColours::textDim.withAlpha(0.6f));
            g.setFont(juce::Font(22.0f, juce::Font::bold));
            g.drawText("--", nc.toNearestInt(), juce::Justification::centred);
        }
    }
};

juce::AudioProcessorEditor* BuiltInKeroVoice::createEditor()
{
    return new BuiltInKeroVoiceEditor(*this);
}
