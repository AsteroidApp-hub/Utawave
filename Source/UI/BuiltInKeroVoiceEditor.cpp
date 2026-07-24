// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

#include "ModernEffectEditor.h"
#include "../AppColours.h"
#include "../Localisation.h"
#include "../Audio/builtin/BuiltInKeroVoice.h"
#include <cmath>

// ケロケロボイスのモダン UI: 中央に**スクロールする音程トレース** (灰 = 入力ピッチ /
// シアン = スナップ後のノート段差)。背景にスケール構成音のグリッド線 (C にはオクターブ表記)。
// 右端に現在のターゲットノートを大きく表示。上部に スケール (半音階/メジャー/マイナー) と
// キー (C..B・スケール選択時のみ) のボタン。下部に スピード / ミックス のノブ。
class BuiltInKeroVoiceEditor : public ModernEffectEditor
{
public:
    explicit BuiltInKeroVoiceEditor(BuiltInKeroVoice& k)
        : ModernEffectEditor(k, { BuiltInKeroVoice::Speed, BuiltInKeroVoice::Mix }, 236),
          kero(k)
    {
        auto styleBtn = [this] (juce::TextButton* b, juce::Colour onCol, juce::Colour onText)
        {
            b->setClickingTogglesState(false);
            b->setColour(juce::TextButton::buttonColourId,   AppColours::buttonBg);
            b->setColour(juce::TextButton::buttonOnColourId, onCol);
            b->setColour(juce::TextButton::textColourOffId,  AppColours::textDim);
            b->setColour(juce::TextButton::textColourOnId,   onText);
            addAndMakeVisible(b);
        };

        const char* scaleKeys[3] = { u8"半音階", u8"メジャー", u8"マイナー" };
        for (int i = 0; i < 3; ++i)
        {
            auto* b = new juce::TextButton(tr(scaleKeys[i]));
            styleBtn(b, AppColours::accent, juce::Colours::white);
            const int idx = i;
            b->onClick = [this, idx] { kero.setP(BuiltInKeroVoice::Scale, (float) idx); clearPresetSelection(); refreshControls(); repaint(); };
            scaleBtns.add(b);
        }
        for (int i = 0; i < 12; ++i)
        {
            auto* b = new juce::TextButton(BuiltInKeroVoice::keyLabel(i));
            styleBtn(b, AppColours::fxCyan, juce::Colours::black);
            const int idx = i;
            b->onClick = [this, idx] { kero.setP(BuiltInKeroVoice::Key, (float) idx); clearPresetSelection(); refreshControls(); repaint(); };
            keyBtns.add(b);
        }

        for (int i = 0; i < kHist; ++i) { histIn[i] = -1.0f; histOut[i] = -1.0f; }

        setSize(juce::jmax(640, getWidth()), getHeight());
        resized();
        refreshControls();
    }

private:
    static constexpr int   kHist = 220;
    static constexpr float kBtnAreaH = 56.0f;
    static constexpr float kNoteW = 88.0f;

    BuiltInKeroVoice& kero;
    juce::OwnedArray<juce::TextButton> scaleBtns, keyBtns;
    float histIn[kHist] {}, histOut[kHist] {};
    int   histPos { 0 };
    float viewCenter { 60.0f };

    static juce::String noteNameFor(float midi)
    {
        const int m = juce::roundToInt(midi);
        int pc = m % 12; if (pc < 0) pc += 12;
        return juce::String(BuiltInKeroVoice::keyLabel(pc)) + juce::String(m / 12 - 1);
    }

    void refreshControls()
    {
        const int scale = juce::jlimit(0, 2,  (int) std::lround(kero.getP(BuiltInKeroVoice::Scale)));
        const int key   = juce::jlimit(0, 11, (int) std::lround(kero.getP(BuiltInKeroVoice::Key)));
        for (int i = 0; i < scaleBtns.size(); ++i)
            scaleBtns[i]->setToggleState(i == scale, juce::dontSendNotification);
        for (int i = 0; i < keyBtns.size(); ++i)
        {
            keyBtns[i]->setVisible(scale != BuiltInKeroVoice::Chromatic);
            keyBtns[i]->setToggleState(i == key, juce::dontSendNotification);
        }
    }

    void onTick() override
    {
        histIn[histPos]  = kero.getUiInputMidi();
        histOut[histPos] = kero.getUiTargetMidi();
        histPos = (histPos + 1) % kHist;
        const float out = kero.getUiTargetMidi();
        if (out >= 0.0f) viewCenter += (out - viewCenter) * 0.10f;
        refreshControls();
    }

    void layoutOverlay(juce::Rectangle<int> g) override
    {
        const int bx = g.getX() + 8, by = g.getY() + 6;
        int sx = bx;
        for (auto* b : scaleBtns) { b->setBounds(sx, by, 78, 20); sx += 82; }
        int kx = bx, ky = by + 26;
        for (auto* b : keyBtns) { b->setBounds(kx, ky, 28, 20); kx += 30; }
    }

    void paintGraph(juce::Graphics& g, juce::Rectangle<float> area) override
    {
        const int scale = juce::jlimit(0, 2,  (int) std::lround(kero.getP(BuiltInKeroVoice::Scale)));
        const int key   = juce::jlimit(0, 11, (int) std::lround(kero.getP(BuiltInKeroVoice::Key)));
        const juce::Colour cyan { AppColours::fxCyan };

        auto noteCol = area.removeFromRight(kNoteW);
        auto trace = area.withTrimmedTop(kBtnAreaH).reduced(8.0f, 6.0f);

        const float semiSpan = 8.0f;   // 表示レンジ ±8 半音
        const float pxPerSemi = trace.getHeight() / (semiSpan * 2.0f);
        auto yFor = [&] (float midi) { return trace.getCentreY() - (midi - viewCenter) * pxPerSemi; };

        // ── ノートグリッド (スケール構成音を強調・C はラベル) ──
        g.setFont(10.0f);
        for (int m = (int) std::ceil(viewCenter - semiSpan); m <= (int) std::floor(viewCenter + semiSpan); ++m)
        {
            int pc = m % 12; if (pc < 0) pc += 12;
            const bool inScale = BuiltInKeroVoice::isPitchClassInScale(pc, scale, key);
            const float y = yFor((float) m);
            g.setColour(inScale ? AppColours::rulerLine.withAlpha(0.45f)
                                : AppColours::rulerLine.withAlpha(0.12f));
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
