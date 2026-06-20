// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

#include "BuiltInEffectEditor.h"
#include "../Localisation.h"
#include "../AppColours.h"

namespace
{
    constexpr int kKnobW   = 92;
    constexpr int kKnobH   = 96;   // ノブ + 値テキスト + ラベル
    constexpr int kTopH    = 40;   // プリセット行
    constexpr int kMargin  = 14;
    constexpr int kMeterW  = 54;

    double intervalForUnit(juce::String unit)
    {
        if (unit == "Hz") return 1.0;
        if (unit == "dB") return 0.1;
        if (unit == "ms") return 0.1;
        if (unit == ":1") return 0.1;
        if (unit == "%")  return 1.0;
        return 0.01;   // size / damping / width
    }
}

BuiltInEffectEditor::BuiltInEffectEditor(BuiltInEffect& effect)
    : juce::AudioProcessorEditor(effect), fx(effect)
{
    setLookAndFeel(&laf);
    hasMeter = fx.hasReductionMeter();

    const int count = fx.getParamCount();
    cols = juce::jmin(4, juce::jmax(1, count));
    rows = (count + cols - 1) / cols;

    // プリセット
    presetLabel.setText(tr(u8"プリセット"), juce::dontSendNotification);
    presetLabel.setColour(juce::Label::textColourId, AppColours::textDim);
    presetLabel.setJustificationType(juce::Justification::centredLeft);
    addAndMakeVisible(presetLabel);

    const auto& presets = fx.getPresets();
    for (int i = 0; i < (int) presets.size(); ++i)
        presetBox.addItem(tr(presets[(size_t) i].name), 100 + i);
    presetBox.setTextWhenNothingSelected(tr(u8"選択..."));
    presetBox.onChange = [this]
    {
        const int idx = presetBox.getSelectedId() - 100;
        if (idx >= 0) { fx.applyPreset(idx); syncKnobsFromEffect(); }
    };
    addAndMakeVisible(presetBox);

    // ノブ
    for (int i = 0; i < count; ++i)
    {
        const auto& pi = fx.getParamInfo(i);

        auto* s = new juce::Slider();
        s->setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
        s->setRange((double) pi.minV, (double) pi.maxV, intervalForUnit(pi.unit));
        if (pi.skew > 0.0f && pi.skew != 1.0f)
            s->setSkewFactor((double) pi.skew);
        s->setTextBoxStyle(juce::Slider::TextBoxBelow, false, 76, 18);
        const bool hasUnit = pi.unit != nullptr && pi.unit[0] != '\0';
        s->setTextValueSuffix(hasUnit ? (juce::String(" ") + pi.unit) : juce::String());
        s->setColour(juce::Slider::rotarySliderFillColourId,   AppColours::accent);
        s->setColour(juce::Slider::rotarySliderOutlineColourId, AppColours::separatorLight);
        s->setColour(juce::Slider::textBoxTextColourId,        AppColours::text);
        s->setColour(juce::Slider::textBoxOutlineColourId,     juce::Colours::transparentBlack);
        s->setValue((double) fx.getP(i), juce::dontSendNotification);
        s->onValueChange = [this, i, s]
        {
            fx.setP(i, (float) s->getValue());
            presetBox.setSelectedId(0, juce::dontSendNotification);   // 手動操作 = プリセット非選択
        };
        addAndMakeVisible(s);
        knobs.add(s);

        auto* lbl = new juce::Label();
        lbl->setText(tr(pi.label), juce::dontSendNotification);
        lbl->setJustificationType(juce::Justification::centred);
        lbl->setColour(juce::Label::textColourId, AppColours::text);
        lbl->setFont(juce::Font(12.0f));
        addAndMakeVisible(lbl);
        knobLabels.add(lbl);
    }

    const int gridW = cols * kKnobW;
    const int w = kMargin * 2 + gridW + (hasMeter ? kMeterW : 0);
    const int h = kTopH + rows * kKnobH + kMargin * 2;
    setSize(juce::jmax(300, w), h);

    // 30Hz で GR メータ更新 + 外部からのパラメータ変更 (プロジェクト再読込/プリセット等) への
    // ノブ追従を行う (ドラッグ中のノブは触らない)。
    startTimerHz(30);
}

BuiltInEffectEditor::~BuiltInEffectEditor()
{
    setLookAndFeel(nullptr);
}

void BuiltInEffectEditor::syncKnobsFromEffect()
{
    for (int i = 0; i < knobs.size(); ++i)
        knobs[i]->setValue((double) fx.getP(i), juce::dontSendNotification);
}

void BuiltInEffectEditor::paint(juce::Graphics& g)
{
    g.fillAll(AppColours::panelBg);

    if (hasMeter && ! meterArea.isEmpty())
    {
        // GR (ゲインリダクション) メータは「減衰量 0..18dB を下から積む」専用表示で、
        // UI/Meter.h の MeterDraw (信号レベル -60..0dB を緑/黄/赤で示す) とは意味が異なるため
        // 共用せず自前で描く (色は減衰の深さで accent→黄→赤)。meterArea (メンバ) は破壊せず複製して使う。
        auto area = meterArea;
        auto labelArea = area.removeFromTop(16);

        g.setColour(AppColours::textDim);
        g.setFont(11.0f);
        g.drawText("GR", labelArea, juce::Justification::centred);

        auto m = area.reduced(2);
        g.setColour(AppColours::meterBg);
        g.fillRoundedRectangle(m.toFloat(), 3.0f);

        // 0..18dB の減衰を下から塗る
        const float maxDb = 18.0f;
        const float frac = juce::jlimit(0.0f, 1.0f, meterShownDb / maxDb);
        const int fillH = (int) (m.getHeight() * frac);
        if (fillH > 0)
        {
            auto fill = m.removeFromBottom(fillH).toFloat();
            g.setColour(frac > 0.8f ? AppColours::meterRed
                       : frac > 0.4f ? AppColours::meterYellow
                                     : AppColours::accent);
            g.fillRoundedRectangle(fill, 3.0f);
        }
    }
}

void BuiltInEffectEditor::resized()
{
    auto area = getLocalBounds().reduced(kMargin);

    auto top = area.removeFromTop(kTopH - 6);
    presetLabel.setBounds(top.removeFromLeft(72));
    presetBox.setBounds(top.removeFromLeft(juce::jmin(220, top.getWidth())).reduced(0, 4));

    if (hasMeter)
        meterArea = area.removeFromRight(kMeterW).reduced(8, 6);

    auto grid = area;
    for (int i = 0; i < knobs.size(); ++i)
    {
        const int rIdx = i / cols;
        const int cIdx = i % cols;
        juce::Rectangle<int> cell(grid.getX() + cIdx * kKnobW,
                                  grid.getY() + rIdx * kKnobH,
                                  kKnobW, kKnobH);
        auto labelRow = cell.removeFromBottom(16);
        knobLabels[i]->setBounds(labelRow);
        knobs[i]->setBounds(cell.reduced(4, 2));
    }
}

void BuiltInEffectEditor::timerCallback()
{
    // ノブを fx の現在値へ追従させる (外部変更に対応)。**ドラッグ中**(isMouseButtonDown) と
    // **値テキスト編集中**(自身か子=テキストエディタにフォーカス) のノブだけ触らない。単なるホバーでは
    // 同期を止めない (止めるとホバー中のノブだけプリセット適用に追従しなくなる)。
    for (int i = 0; i < knobs.size(); ++i)
    {
        auto* s = knobs[i];
        if (s->isMouseButtonDown() || s->hasKeyboardFocus(true)) continue;
        const double v = (double) fx.getP(i);
        if (std::abs(s->getValue() - v) > 1.0e-4)
            s->setValue(v, juce::dontSendNotification);
    }

    if (! hasMeter) return;

    // GR メータのバリスティクス (立ち上がり即・立ち下がりゆっくり) は**ここ 1 箇所**で掛ける
    // (コンプ側は生の減衰量のみ公開)。30Hz 固定なのでブロック長に依らず一定の落下速度になる。
    const float target = fx.getReductionDb();
    const float next = target > meterShownDb ? target
                                             : meterShownDb * 0.8f + target * 0.2f;
    if (std::abs(next - meterShownDb) > 0.01f)
    {
        meterShownDb = next;
        if (! meterArea.isEmpty()) repaint(meterArea);
    }
}

// BuiltInEffect の純粋仮想 createEditor をここで実装 (UI 依存を 1 箇所に閉じ込める)
juce::AudioProcessorEditor* BuiltInEffect::createEditor()
{
    return new BuiltInEffectEditor(*this);
}
