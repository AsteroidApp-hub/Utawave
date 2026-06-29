// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

#include "ModernEffectEditor.h"
#include "../AppColours.h"
#include "../Localisation.h"
#include "../Audio/builtin/BuiltInDelay.h"
#include <cmath>

// ディレイのイケイケ UI: 中央に**エコータップのビジュアライズ** (ドライの白タップから、
// ディレイ間隔ごとにフィードバックで減衰するネオンのタップが続く。ピンポン時は L=上 / R=下 に
// 交互に跳ねる)。上部に SYNC / PING-PONG トグルと、同期時の音価ボタン (1/4〜1/16)。右に出力メータ。
// 下部に フィードバック / トーン / ミックス のノブ (+ 非同期時のみ タイム ノブ)。
class BuiltInDelayEditor : public ModernEffectEditor
{
public:
    explicit BuiltInDelayEditor(BuiltInDelay& d)
        : ModernEffectEditor(d, { BuiltInDelay::TimeMs, BuiltInDelay::Feedback,
                                  BuiltInDelay::Tone, BuiltInDelay::Mix }, 224),
          dly(d)
    {
        auto styleToggle = [this] (juce::TextButton& b, const juce::String& txt)
        {
            b.setButtonText(txt);
            b.setClickingTogglesState(true);
            b.setColour(juce::TextButton::buttonColourId,   AppColours::buttonBg);
            b.setColour(juce::TextButton::buttonOnColourId, AppColours::accent);
            b.setColour(juce::TextButton::textColourOffId,  AppColours::text);
            b.setColour(juce::TextButton::textColourOnId,   juce::Colours::white);
            addAndMakeVisible(b);
        };
        styleToggle(syncBtn, "SYNC");
        styleToggle(pingBtn, "PING-PONG");
        syncBtn.onClick = [this] { dly.setP(BuiltInDelay::Sync,     syncBtn.getToggleState() ? 1.0f : 0.0f); clearPresetSelection(); refreshControls(); repaint(); };
        pingBtn.onClick = [this] { dly.setP(BuiltInDelay::PingPong, pingBtn.getToggleState() ? 1.0f : 0.0f); clearPresetSelection(); repaint(); };

        for (int i = 0; i < BuiltInDelay::numDivisions(); ++i)
        {
            auto* b = new juce::TextButton(BuiltInDelay::divisionLabel(i));
            b->setColour(juce::TextButton::buttonColourId,   AppColours::buttonBg);
            b->setColour(juce::TextButton::buttonOnColourId, AppColours::fxCyan);
            b->setColour(juce::TextButton::textColourOffId,  AppColours::textDim);
            b->setColour(juce::TextButton::textColourOnId,   juce::Colours::black);
            const int idx = i;
            b->onClick = [this, idx] { dly.setP(BuiltInDelay::Division, (float) idx); clearPresetSelection(); refreshControls(); repaint(); };
            addAndMakeVisible(b);
            divBtns.add(b);
        }

        setSize(juce::jmax(620, getWidth()), getHeight());
        resized();
        refreshControls();
    }

private:
    static constexpr float kMeterW = 44.0f;
    static constexpr float kBtnAreaH = 56.0f;
    const juce::Colour kCyan { AppColours::fxCyan };
    const juce::Colour kAmber { AppColours::accent };

    BuiltInDelay& dly;
    juce::TextButton syncBtn, pingBtn;
    juce::OwnedArray<juce::TextButton> divBtns;
    float smOut { -100.0f };

    void refreshControls()
    {
        const bool sync = dly.getP(BuiltInDelay::Sync) > 0.5f;
        syncBtn.setToggleState(sync, juce::dontSendNotification);
        pingBtn.setToggleState(dly.getP(BuiltInDelay::PingPong) > 0.5f, juce::dontSendNotification);
        if (auto* k = knobForParam(BuiltInDelay::TimeMs)) k->setEnabled(! sync);
        const int sel = juce::jlimit(0, BuiltInDelay::numDivisions() - 1, (int) std::lround(dly.getP(BuiltInDelay::Division)));
        for (int i = 0; i < divBtns.size(); ++i)
        {
            divBtns[i]->setVisible(sync);
            divBtns[i]->setToggleState(i == sel, juce::dontSendNotification);
        }
    }

    static float smoothMeter(float prev, float target) { return target > prev ? target : prev + (target - prev) * 0.25f; }
    void onTick() override
    {
        smOut = smoothMeter(smOut, dly.getOutputDb());
        refreshControls();   // プリセット適用・外部変更でトグル/音価を追従
    }

    void layoutOverlay(juce::Rectangle<int> g) override
    {
        const int bx = g.getX() + 8, by = g.getY() + 6;
        syncBtn.setBounds(bx, by, 60, 20);
        pingBtn.setBounds(bx + 66, by, 96, 20);
        int dx = bx, dy = by + 26;
        for (auto* b : divBtns) { b->setBounds(dx, dy, 44, 20); dx += 47; }
    }

    void paintGraph(juce::Graphics& g, juce::Rectangle<float> area) override
    {
        const bool  sync     = dly.getP(BuiltInDelay::Sync) > 0.5f;
        const bool  ping     = dly.getP(BuiltInDelay::PingPong) > 0.5f;
        const float fb       = juce::jlimit(0.0f, 0.95f, dly.getP(BuiltInDelay::Feedback) * 0.01f);
        const float mix      = juce::jlimit(0.0f, 1.0f, dly.getP(BuiltInDelay::Mix) * 0.01f);
        const float delaySec = juce::jmax(0.01f, dly.getDelaySeconds());

        // 時間/音価の読み出し (右上)
        const int sel = juce::jlimit(0, BuiltInDelay::numDivisions() - 1, (int) std::lround(dly.getP(BuiltInDelay::Division)));
        juce::String readout = (sync ? juce::String(BuiltInDelay::divisionLabel(sel)) + "  ·  " : juce::String())
                             + juce::String(dly.getDelaySeconds() * 1000.0, 0) + " ms";
        g.setColour(kCyan);
        g.setFont(juce::Font(12.5f, juce::Font::bold));
        g.drawText(readout, (int) (area.getRight() - kMeterW - 190), (int) (area.getY() + 8), 184, 18, juce::Justification::right);

        // ── メーター域 (右) ──
        auto meterCol = area.removeFromRight(kMeterW);
        auto taps = area.withTrimmedTop((int) kBtnAreaH).reduced(8.0f, 6.0f);
        {
            auto mr = juce::Rectangle<float>(meterCol.getCentreX() - 11.0f, taps.getY(), 22.0f, taps.getHeight());
            drawLevelBar(g, mr, smOut, -48.0f, 6.0f, kCyan, "OUT");
        }

        // ── エコータップ ──
        const float cy = taps.getCentreY();
        const float halfH = taps.getHeight() * 0.5f - 4.0f;
        g.setColour(AppColours::rulerLine.withAlpha(0.30f));
        g.drawHorizontalLine((int) cy, taps.getX(), taps.getRight());

        const float windowSec = juce::jmax(0.05f, delaySec * 5.5f);
        auto xAt = [&] (float t) { return taps.getX() + (t / windowSec) * taps.getWidth(); };

        // ドライタップ (t=0・白・上下フル)
        {
            const float x = xAt(0.0f);
            g.setColour(juce::Colours::white.withAlpha(0.85f));
            g.fillRect(x - 1.5f, cy - halfH, 3.0f, halfH * 2.0f);
            g.setColour(juce::Colours::white.withAlpha(0.5f));
            g.fillEllipse(x - 3.0f, cy - halfH - 3.0f, 6.0f, 6.0f);
        }

        // エコー (k=1.. ・amp = mix * fb^(k-1)・減衰)
        for (int k = 1; k < 64; ++k)
        {
            const float amp = mix * std::pow(fb, (float) (k - 1));
            if (amp < 0.012f) break;
            const float x = xAt((float) k * delaySec);
            if (x > taps.getRight()) break;

            const float h = juce::jlimit(2.0f, halfH, amp * halfH * 2.2f);
            const bool up = ping ? (k % 2 == 1) : true;     // ピンポン: 交互 / 通常: 上下両方
            const bool dn = ping ? (k % 2 == 0) : true;
            const float glow = juce::jlimit(0.15f, 1.0f, amp * 2.0f);

            auto drawTap = [&] (float topY, float botY)
            {
                g.setColour(kCyan.withAlpha(0.18f * glow));
                g.fillRect(x - 3.0f, topY, 6.0f, botY - topY);
                g.setColour(kCyan.withAlpha(0.9f * glow));
                g.fillRect(x - 1.3f, topY, 2.6f, botY - topY);
                g.setColour(kCyan.withAlpha(glow));
                g.fillEllipse(x - 3.0f, (topY < cy ? topY : botY) - 3.0f, 6.0f, 6.0f);
            };
            if (up) drawTap(cy - h, cy);
            if (dn) drawTap(cy, cy + h);
        }
    }
};

juce::AudioProcessorEditor* BuiltInDelay::createEditor()
{
    return new BuiltInDelayEditor(*this);
}
