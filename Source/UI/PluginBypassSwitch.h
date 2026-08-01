#pragma once

#include <JuceHeader.h>
#include "../AppColours.h"

// INS チップ左のワンクリック・バイパススイッチ (LED ランプ)。
// チップ (TextButton, ConnectedOnLeft で左角を平らに) の左に隙間ゼロで置き、
// 自身は左だけ角丸で描くことで「1 つの枠の左セグメント」に見せる (Cubase の
// インサートスロット風)。背景色・角丸・アウトラインは LookAndFeel_V4 の
// drawButtonBackground と同じ式でチップと揃える。
// オン = 明るく点灯 / バイパス中 = 消灯 (dim)。クリックで onClick が発火する
// (トグルの実処理は所有側が onPluginBypassRequest 経由の Undo 対応パスで行う)。
// TrackHeaderView / MasterPanel で共用。
class PluginBypassSwitch : public juce::Button
{
public:
    PluginBypassSwitch() : juce::Button({}) { setWantsKeyboardFocus(false); }

    void setBypassed(bool b) { bypassed = b; repaint(); }

    void paintButton(juce::Graphics& g, bool highlighted, bool down) override
    {
        // チップ側 (TextButton の buttonColourId) と同じ色定数 + V4 と同じ補正
        auto base = (bypassed ? juce::Colour(0xff2a2d31) : juce::Colour(0xff2e4d7a))
                        .withMultipliedSaturation(0.9f);
        if (down || highlighted)
            base = base.contrasting(down ? 0.2f : 0.05f);

        auto r = getLocalBounds().toFloat().reduced(0.5f, 0.5f);
        juce::Path body;
        body.addRoundedRectangle(r.getX(), r.getY(), r.getWidth(), r.getHeight(),
                                 6.0f, 6.0f,
                                 true, false,   // 左上だけ角丸 (右はチップへ連結)
                                 true, false);  // 左下だけ角丸
        g.setColour(base);
        g.fillPath(body);
        g.setColour(findColour(juce::ComboBox::outlineColourId));
        g.strokePath(body, juce::PathStrokeType(1.0f));

        // LED ランプ: 点灯 = オン / 消灯 (輪郭のみ) = バイパス中。
        // 特定 DAW の電源記号と被らない汎用のインジケータ表現にする (商標方針)。
        // 作業の邪魔にならないよう控えめに: グロー無し・小さめ・淡い色 (要望 2026-08)
        const auto c    = r.getCentre();
        const float rad = juce::jmin(r.getWidth(), r.getHeight()) * 0.16f;
        if (!bypassed)
        {
            g.setColour(juce::Colour(0xffaec8de).withAlpha(0.85f));
            g.fillEllipse(c.x - rad, c.y - rad, rad * 2.0f, rad * 2.0f);
        }
        else
        {
            g.setColour(AppColours::textDim.withAlpha(0.4f));
            g.drawEllipse(c.x - rad, c.y - rad, rad * 2.0f, rad * 2.0f, 1.1f);
        }
    }

private:
    bool bypassed { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PluginBypassSwitch)
};
