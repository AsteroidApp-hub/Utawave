// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

// Utawave — ピアノロール下部コントロールレーンのユニットテスト
//
// PianoRollEditor を実サイズでヘッドレス構築し、合成 MouseEvent で mouseDown/Drag/Up を
// 直接駆動して「レーンの操作体系」を固定する:
//   ・素のドラッグ = 範囲選択 (値は書かない・誤クリック防止)
//   ・Cmd+ドラッグ = 描画 / ペンツール (D) 中は Cmd 不要で素のドラッグでも描画
//   ・描いた値は mouseUp で MidiClip のシーケンスへ書き戻される (ピッチベンド等)
// レーン切替 / ペン切替はテスト用シーム (sendNotificationSync) で本物のコンボ/ボタン経路を通す。
// expect メッセージは ASCII。

#include <JuceHeader.h>
#include "../Source/UI/PianoRollEditor.h"
#include "../Source/Tracks/MidiClip.h"

namespace
{

class PianoRollLaneTests : public juce::UnitTest
{
public:
    PianoRollLaneTests() : juce::UnitTest("PianoRoll control lanes") {}

    static juce::MouseEvent makeMouse(juce::Component& c, juce::Point<float> pos,
                                      juce::ModifierKeys mods)
    {
        return juce::MouseEvent(juce::Desktop::getInstance().getMainMouseSource(),
                                pos, mods,
                                juce::MouseInputSource::defaultPressure,
                                juce::MouseInputSource::defaultOrientation,
                                juce::MouseInputSource::defaultRotation,
                                juce::MouseInputSource::defaultTiltX,
                                juce::MouseInputSource::defaultTiltY,
                                &c, &c, juce::Time::getCurrentTime(), pos,
                                juce::Time::getCurrentTime(), 1, false);
    }

    static int countPitchWheelEvents(const MidiClip& clip)
    {
        int n = 0;
        const auto& seq = clip.getSequence();
        for (int i = 0; i < seq.getNumEvents(); ++i)
            if (seq.getEventPointer(i)->message.isPitchWheel()) ++n;
        return n;
    }

    void runTest() override
    {
        beginTest("control lane: plain drag selects, Cmd/pen drags draw");

        MidiClip clip(0.0, 8.0);
        PianoRollEditor ed(clip, 120.0);
        ed.setSize(900, 520);
        ed.setCtrlLaneForTests(1);   // ピッチベンド (CtrlLane::PitchBend)

        // レーン内の点 (velocityH 既定 80 → レーン上端 440。境界 ±4px を避けて中程)
        const juce::Point<float> inLane { 400.0f, 480.0f };
        const auto plain = juce::ModifierKeys(juce::ModifierKeys::leftButtonModifier);
        const auto cmd   = juce::ModifierKeys(juce::ModifierKeys::leftButtonModifier
                                              | juce::ModifierKeys::commandModifier);

        // (1) 素のドラッグ: 値は書かず範囲選択になる
        {
            auto ev = makeMouse(ed, inLane, plain);
            ed.mouseDown(ev);
            ed.mouseUp(ev);
            expect(countPitchWheelEvents(clip) == 0, "plain click writes nothing");
        }

        // (2) Cmd+ドラッグ: 描画される (mouseUp でクリップへ書き戻し)
        {
            auto ev = makeMouse(ed, inLane, cmd);
            ed.mouseDown(ev);
            ed.mouseUp(ev);
            expect(countPitchWheelEvents(clip) > 0, "Cmd+click draws a pitch bend event");
        }

        // (3) ペンツール ON: Cmd 無しの素のドラッグでも描画される
        {
            // 前のイベントを消してから (Option+ドラッグ相当の直接クリアではなくシーケンス初期化)
            clip.getSequence().clear();
            ed.reloadNotesFromClip();

            ed.setPenModeForTests(true);
            expect(ed.getPenModeForTests(), "pen toggle reaches penMode via button path");

            auto ev = makeMouse(ed, inLane, plain);
            ed.mouseDown(ev);
            ed.mouseUp(ev);
            expect(countPitchWheelEvents(clip) > 0, "pen ON: plain drag draws without Cmd");
        }

        // (4) ペンツール ON + Shift: 範囲選択に回る (描画しない)
        {
            clip.getSequence().clear();
            ed.reloadNotesFromClip();
            const auto shift = juce::ModifierKeys(juce::ModifierKeys::leftButtonModifier
                                                  | juce::ModifierKeys::shiftModifier);
            auto down = makeMouse(ed, inLane, shift);
            ed.mouseDown(down);
            auto drag = makeMouse(ed, { 500.0f, 480.0f }, shift);
            ed.mouseDrag(drag);
            ed.mouseUp(drag);
            expect(countPitchWheelEvents(clip) == 0, "pen ON + Shift: selects instead of drawing");
            expect(ed.hasCtrlSelectionForTests(), "pen ON + Shift: range selection active");
        }
    }
};

static PianoRollLaneTests pianoRollLaneTests;

} // namespace
