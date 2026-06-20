// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

#include "ModernEffectEditor.h"
#include "../Localisation.h"
#include "../AppColours.h"

namespace
{
    constexpr int kTopH    = 32;
    constexpr int kKnobW   = 84;
    constexpr int kKnobRowH = 80;
    constexpr int kMargin  = 12;

    double intervalForUnit(const char* unit)
    {
        const juce::String u (unit);
        if (u == "Hz") return 1.0;
        if (u == "dB") return 0.1;
        if (u == "ms") return 0.1;
        if (u == ":1") return 0.1;
        if (u == "%")  return 1.0;
        return 0.01;
    }
}

ModernEffectEditor::ModernEffectEditor(BuiltInEffect& f, std::vector<int> knobParams, int graphHeight)
    : juce::AudioProcessorEditor(f), fx(f), knobParamIdx(std::move(knobParams)), graphH(graphHeight)
{
    setLookAndFeel(&laf);

    presetLabel.setText(tr(u8"プリセット"), juce::dontSendNotification);
    presetLabel.setColour(juce::Label::textColourId, AppColours::textDim);
    presetLabel.setJustificationType(juce::Justification::centredRight);
    addAndMakeVisible(presetLabel);

    const auto& presets = fx.getPresets();
    for (int i = 0; i < (int) presets.size(); ++i)
        presetBox.addItem(tr(presets[(size_t) i].name), 100 + i);
    presetBox.setTextWhenNothingSelected(tr(u8"選択..."));
    presetBox.onChange = [this]
    {
        const int idx = presetBox.getSelectedId() - 100;
        if (idx < 0) return;
        fx.applyPreset(idx);
        for (int i = 0; i < knobs.size(); ++i)
            knobs[i]->setValue((double) fx.getP(knobParamIdx[(size_t) i]), juce::dontSendNotification);
        repaint();
    };
    addAndMakeVisible(presetBox);

    for (int i = 0; i < (int) knobParamIdx.size(); ++i)
    {
        const auto& pi = fx.getParamInfo(knobParamIdx[(size_t) i]);
        auto* s = new juce::Slider();
        s->setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
        s->setRange((double) pi.minV, (double) pi.maxV, intervalForUnit(pi.unit));
        if (pi.skew > 0.0f && pi.skew != 1.0f) s->setSkewFactor((double) pi.skew);
        s->setTextBoxStyle(juce::Slider::TextBoxBelow, false, 72, 16);
        const bool hasUnit = pi.unit != nullptr && pi.unit[0] != '\0';
        s->setTextValueSuffix(hasUnit ? (juce::String(" ") + pi.unit) : juce::String());
        s->setColour(juce::Slider::rotarySliderFillColourId,    AppColours::accent);
        s->setColour(juce::Slider::rotarySliderOutlineColourId, AppColours::separatorLight);
        s->setColour(juce::Slider::textBoxTextColourId,         AppColours::text);
        s->setColour(juce::Slider::textBoxOutlineColourId,      juce::Colours::transparentBlack);
        s->setValue((double) fx.getP(knobParamIdx[(size_t) i]), juce::dontSendNotification);
        const int param = knobParamIdx[(size_t) i];
        s->onValueChange = [this, param, s] { fx.setP(param, (float) s->getValue()); clearPresetSelection(); };
        addAndMakeVisible(s);
        knobs.add(s);

        auto* lbl = new juce::Label();
        lbl->setText(tr(pi.label), juce::dontSendNotification);
        lbl->setJustificationType(juce::Justification::centred);
        lbl->setColour(juce::Label::textColourId, AppColours::text);
        lbl->setFont(juce::Font(11.5f));
        addAndMakeVisible(lbl);
        knobLabels.add(lbl);
    }

    const int cols = juce::jmax(1, (int) knobParamIdx.size());
    const int w = juce::jmax(440, kMargin * 2 + cols * kKnobW);
    setSize(w, kTopH + graphH + kKnobRowH + kMargin * 2);
    startTimerHz(30);
}

ModernEffectEditor::~ModernEffectEditor() { setLookAndFeel(nullptr); }

void ModernEffectEditor::resized()
{
    auto area = getLocalBounds().reduced(kMargin, kMargin);
    auto top = area.removeFromTop(kTopH);
    presetBox.setBounds(top.removeFromRight(170).reduced(0, 4));
    presetLabel.setBounds(top.removeFromRight(76).reduced(0, 4));

    auto knobRow = area.removeFromBottom(kKnobRowH);
    const int cols = juce::jmax(1, knobs.size());
    const int cw = knobRow.getWidth() / cols;
    for (int i = 0; i < knobs.size(); ++i)
    {
        juce::Rectangle<int> cell(knobRow.getX() + i * cw, knobRow.getY(), cw, knobRow.getHeight());
        knobLabels[i]->setBounds(cell.removeFromBottom(15));
        knobs[i]->setBounds(cell.reduced(6, 2));
    }

    graph = area.withTrimmedBottom(6);
    layoutOverlay(graph);
}

juce::Slider* ModernEffectEditor::knobForParam(int param) const
{
    for (int i = 0; i < (int) knobParamIdx.size(); ++i)
        if (knobParamIdx[(size_t) i] == param) return knobs[i];
    return nullptr;
}

void ModernEffectEditor::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xff141414));
    if (graph.isEmpty()) return;
    const auto gf = graph.toFloat();
    g.setColour(juce::Colour(0xff1c1c1c));
    g.fillRoundedRectangle(gf, 4.0f);

    juce::Graphics::ScopedSaveState ss(g);
    g.reduceClipRegion(graph);
    paintGraph(g, gf);
}

void ModernEffectEditor::mouseDown(const juce::MouseEvent& e)
{
    if (graph.contains(e.getPosition())) onGraphMouseDown(e);
}
void ModernEffectEditor::mouseDrag(const juce::MouseEvent& e) { onGraphMouseDrag(e); }
void ModernEffectEditor::mouseUp(const juce::MouseEvent& e)   { onGraphMouseUp(e); }

void ModernEffectEditor::timerCallback()
{
    // ノブを fx の値へ追従 (ドラッグ/テキスト入力中は触らない)
    for (int i = 0; i < knobs.size(); ++i)
    {
        auto* s = knobs[i];
        if (s->isMouseButtonDown() || s->hasKeyboardFocus(true)) continue;
        const double v = (double) fx.getP(knobParamIdx[(size_t) i]);
        if (std::abs(s->getValue() - v) > 1.0e-4) s->setValue(v, juce::dontSendNotification);
    }

    if (fx.hasReductionMeter())
    {
        const float t = fx.getReductionDb();
        smReductionDb = t > smReductionDb ? t : smReductionDb * 0.8f + t * 0.2f;
    }
    onTick();
    repaint(graph);
}
