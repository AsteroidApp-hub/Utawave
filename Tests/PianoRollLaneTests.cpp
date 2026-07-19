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

        // (5) 既存イベント点の掴み調整: 素のクリックで点を掴み、ドラッグで値/時刻を動かせる
        {
            MidiClip clip(0.0, 8.0);
            PianoRollEditor ed(clip, 120.0);
            ed.setSize(900, 520);
            ed.setCtrlLaneForTests(1);   // ピッチベンド
            // GRID Off にして描画位置をピクセルどおりにする (Shift+0)
            ed.keyPressed(juce::KeyPress('0',
                juce::ModifierKeys(juce::ModifierKeys::shiftModifier), 0));
            const juce::Point<float> at { 400.0f, 480.0f };
            const auto plain = juce::ModifierKeys(juce::ModifierKeys::leftButtonModifier);
            const auto cmd   = juce::ModifierKeys(juce::ModifierKeys::leftButtonModifier
                                                  | juce::ModifierKeys::commandModifier);
            // Cmd+クリックで点を 1 つ描く
            {
                auto ev = makeMouse(ed, at, cmd);
                ed.mouseDown(ev);
                ed.mouseUp(ev);
            }
            double t1 = -1.0; int v1 = -1;
            {
                const auto& seq = clip.getSequence();
                for (int i = 0; i < seq.getNumEvents(); ++i)
                    if (seq.getEventPointer(i)->message.isPitchWheel())
                    {
                        t1 = seq.getEventPointer(i)->message.getTimeStamp();
                        v1 = seq.getEventPointer(i)->message.getPitchWheelValue();
                    }
            }
            expect(t1 >= 0.0, "setup: one pitch bend point drawn");
            // 素のクリックで同じ位置を掴んで右上へドラッグ → 時刻が進み値が上がる
            {
                auto down = makeMouse(ed, at, plain);
                ed.mouseDown(down);
                expect(!ed.hasCtrlSelectionForTests(),
                       "grabbing a point does not start range selection");
                auto drag = makeMouse(ed, { 450.0f, 470.0f }, plain);
                ed.mouseDrag(drag);
                ed.mouseUp(drag);
            }
            int nPB = 0; double t2 = -1.0; int v2 = -1;
            {
                const auto& seq = clip.getSequence();
                for (int i = 0; i < seq.getNumEvents(); ++i)
                    if (seq.getEventPointer(i)->message.isPitchWheel())
                    {
                        ++nPB;
                        t2 = seq.getEventPointer(i)->message.getTimeStamp();
                        v2 = seq.getEventPointer(i)->message.getPitchWheelValue();
                    }
            }
            expect(nPB == 1, "dragged point stays a single event");
            expect(t2 > t1 + 1e-6, "drag right moves the event later in time");
            expect(v2 > v1, "drag up raises the value");
        }

        // (6) ステップ入力中: グリッド域の空きクリックで入力カーソルが移動する (GRID スナップ)
        {
            MidiClip clip(0.0, 8.0);
            PianoRollEditor ed(clip, 120.0);
            ed.setSize(900, 520);
            ed.setStepModeForTests(true);
            expect(ed.isStepInputActive(), "step mode toggles via button path");
            expect(ed.getStepPosForTests() == 0.0, "cursor starts at clip head");
            const auto plain = juce::ModifierKeys(juce::ModifierKeys::leftButtonModifier);
            auto ev = makeMouse(ed, { 400.0f, 300.0f }, plain);   // 空きグリッド域
            ed.mouseDown(ev);
            ed.mouseUp(ev);
            const double p = ed.getStepPosForTests();
            expect(p > 0.0, "grid-area click moves the step cursor");
            // 既定 GRID 1/16 @120bpm = 0.125s 倍数へスナップされている
            const double rem = std::abs(p / 0.125 - std::round(p / 0.125));
            expect(rem < 1e-6, "moved cursor is snapped to the grid");
        }

        // (7) Cmd+Option = GRID を一時解除したフリー描画 (Cmd のみはグリッドへスナップ)
        {
            const auto cmdOnly = juce::ModifierKeys(juce::ModifierKeys::leftButtonModifier
                                                    | juce::ModifierKeys::commandModifier);
            const auto cmdAlt  = juce::ModifierKeys(juce::ModifierKeys::leftButtonModifier
                                                    | juce::ModifierKeys::commandModifier
                                                    | juce::ModifierKeys::altModifier);
            // x=403 → xToTime = (403-48)/200 = 1.775s。GRID 1/16 (0.125s) なら 1.75 へスナップ
            const juce::Point<float> at { 403.0f, 480.0f };

            auto drawnPBTime = [](const MidiClip& c) -> double
            {
                const auto& seq = c.getSequence();
                for (int i = 0; i < seq.getNumEvents(); ++i)
                    if (seq.getEventPointer(i)->message.isPitchWheel())
                        return seq.getEventPointer(i)->message.getTimeStamp();
                return -1.0;
            };

            MidiClip snapClip(0.0, 8.0);
            {
                PianoRollEditor ed(snapClip, 120.0);
                ed.setSize(900, 520);
                ed.setCtrlLaneForTests(1);
                auto ev = makeMouse(ed, at, cmdOnly);
                ed.mouseDown(ev);
                ed.mouseUp(ev);
            }
            expectWithinAbsoluteError(drawnPBTime(snapClip), 1.75, 1e-6,
                                      "Cmd draw snaps to the grid");

            MidiClip freeClip(0.0, 8.0);
            {
                PianoRollEditor ed(freeClip, 120.0);
                ed.setSize(900, 520);
                ed.setCtrlLaneForTests(1);
                auto ev = makeMouse(ed, at, cmdAlt);
                ed.mouseDown(ev);
                ed.mouseUp(ev);
            }
            expectWithinAbsoluteError(drawnPBTime(freeClip), 1.775, 1e-6,
                                      "Cmd+Option draws free of the grid");
        }

        // (8) ノート上の Cmd+Shift+ドラッグ = ベロシティ調整 (和音でも掴んだノートだけ変わる)
        {
            MidiClip clip(0.0, 8.0);
            putNote(clip, 60, 0.5, 1.0, 100);
            putNote(clip, 64, 0.5, 1.0, 100);   // 同時刻の和音 (レーンのバーは重なる)
            PianoRollEditor ed(clip, 120.0);
            ed.setSize(900, 520);
            const auto cmdShift = juce::ModifierKeys(juce::ModifierKeys::leftButtonModifier
                                                     | juce::ModifierKeys::commandModifier
                                                     | juce::ModifierKeys::shiftModifier);
            // ノートの画面 y は内部レイアウト依存なので、グリッド域を走査して掴めた所で確定する
            // (空振りは Cmd+Shift ではラバーバンドになるだけで velocity は変わらない)
            bool adjusted = false;
            for (int y = 60; y < 400 && !adjusted; y += 4)
            {
                auto down = makeMouse(ed, { 200.0f, (float) y }, cmdShift);
                ed.mouseDown(down);
                auto drag = makeMouse(ed, { 200.0f, (float) y - 30.0f }, cmdShift);
                ed.mouseDrag(drag);
                ed.mouseUp(drag);
                for (const auto& n : readNotes(clip))
                    if (n.vel > 110) adjusted = true;
            }
            expect(adjusted, "Cmd+Shift drag on a note raises its velocity");
            // 和音のもう片方は元の値のまま (個別調整できる = 要望の本体)
            int changed = 0, unchanged = 0;
            for (const auto& n : readNotes(clip))
            {
                if (n.vel > 110) ++changed;
                else if (std::abs(n.vel - 100) <= 1) ++unchanged;
            }
            expect(changed == 1 && unchanged == 1,
                   "only the grabbed chord note changes velocity");
        }

        testNoteCommands();
    }

    // ── ノート編集コマンド (複製 / 終端クォンタイズ / 長さ揃え / 重なり解消 / オクターブ移動) ──
    static void putNote(MidiClip& clip, int pitch, double start, double dur,
                        juce::uint8 vel = 100)
    {
        auto on  = juce::MidiMessage::noteOn(1, pitch, vel);
        auto off = juce::MidiMessage::noteOff(1, pitch);
        on.setTimeStamp(start);
        off.setTimeStamp(start + dur);
        clip.getSequence().addEvent(on);
        clip.getSequence().addEvent(off);
        clip.getSequence().updateMatchedPairs();
    }

    struct FoundNote { double start; double dur; int pitch; int vel; };
    static std::vector<FoundNote> readNotes(const MidiClip& clip)
    {
        std::vector<FoundNote> out;
        const auto& seq = clip.getSequence();
        for (int i = 0; i < seq.getNumEvents(); ++i)
        {
            const auto& m = seq.getEventPointer(i)->message;
            if (!m.isNoteOn()) continue;
            const int offI = seq.getIndexOfMatchingKeyUp(i);
            const double offT = (offI >= 0) ? seq.getEventPointer(offI)->message.getTimeStamp()
                                            : m.getTimeStamp();
            out.push_back({ m.getTimeStamp(), offT - m.getTimeStamp(),
                            m.getNoteNumber(), (int) m.getVelocity() });
        }
        return out;
    }

    void testNoteCommands()
    {
        beginTest("note commands: duplicate / end-quantize / match lengths / overlaps / octave");

        const auto cmdA = juce::KeyPress('A', juce::ModifierKeys(juce::ModifierKeys::commandModifier), 0);

        // 複製 (Cmd+D): 選択スパン分だけ後ろへ。GRID 1/16 @120bpm = 0.125s 倍数へ切り上げ
        {
            MidiClip clip(0.0, 8.0);
            putNote(clip, 60, 0.0, 0.5);
            putNote(clip, 64, 1.0, 0.5);   // スパン = 1.5s (0.125 の倍数)
            PianoRollEditor ed(clip, 120.0);
            ed.setSize(900, 520);
            ed.keyPressed(cmdA);
            ed.duplicateSelected();
            auto ns = readNotes(clip);
            expect((int) ns.size() == 4, "duplicate doubles the note count");
            bool foundShifted = false;
            for (const auto& n : ns)
                if (n.pitch == 60 && std::abs(n.start - 1.5) < 1e-6) foundShifted = true;
            expect(foundShifted, "duplicate places copies one span later");
        }

        // 複製 (単一ノート・同一ピッチ隣接): 複製が元と同じ長さを保つ
        // (span == dur で複製の note-on が元の note-off と同時刻になるケース)
        {
            MidiClip clip(0.0, 8.0);
            putNote(clip, 65, 0.5, 0.75);   // 0.125 の倍数 → span = dur = 0.75
            PianoRollEditor ed(clip, 120.0);
            ed.setSize(900, 520);
            ed.keyPressed(cmdA);
            ed.duplicateSelected();
            auto ns = readNotes(clip);
            std::sort(ns.begin(), ns.end(),
                      [](const FoundNote& a, const FoundNote& b) { return a.start < b.start; });
            expect((int) ns.size() == 2, "adjacent duplicate keeps both notes");
            if (ns.size() == 2)
            {
                expect(std::abs(ns[1].start - 1.25) < 1e-6, "copy starts one span later");
                expect(std::abs(ns[1].dur - 0.75) < 1e-6, "copy keeps full duration");
                expect(std::abs(ns[0].dur - 0.75) < 1e-6, "original keeps full duration");
            }
        }

        // 複製 (クリップ末尾を越えるケース): 複製の長さを保ち、クリップを次の小節境界へ
        // 自動延長する (旧実装は末尾クランプで 0.01s の極小ノートになる不具合だった)
        {
            MidiClip clip(0.0, 2.0);          // 1 小節 @120bpm 4/4 = 2.0s
            putNote(clip, 65, 1.0, 0.75);     // 複製 [1.75, 2.5] は末尾 2.0 を越える
            PianoRollEditor ed(clip, 120.0);
            ed.setSize(900, 520);
            ed.keyPressed(cmdA);
            ed.duplicateSelected();
            auto ns = readNotes(clip);
            std::sort(ns.begin(), ns.end(),
                      [](const FoundNote& a, const FoundNote& b) { return a.start < b.start; });
            expect((int) ns.size() == 2, "over-end duplicate keeps both notes");
            if (ns.size() == 2)
                expect(std::abs(ns[1].dur - 0.75) < 1e-6,
                       "over-end duplicate keeps full duration (no clamp)");
            expect(std::abs(clip.getDuration() - 4.0) < 1e-9,
                   "clip auto-extends to next bar boundary (2.0 -> 4.0)");
        }

        // 複製 (実アプリ経路 = 外部 UndoManager + callAsync コミット + reload):
        // コミットが発火しても複製の長さが保たれ、選択も消えないこと
        {
            MidiClip clip(0.0, 2.0);          // 1 小節 → 複製で自動延長されるケース
            putNote(clip, 65, 1.0, 0.75);
            PianoRollEditor ed(clip, 120.0);
            ed.setSize(900, 520);
            juce::UndoManager um;
            ed.setUndoManager(&um);
            juce::Component::SafePointer<PianoRollEditor> safeEd(&ed);
            ed.setExternalReloadCallback([safeEd](MidiClip*)
            {
                if (auto* p = safeEd.getComponent()) p->reloadNotesFromClip();
            });
            ed.keyPressed(cmdA);
            ed.duplicateSelected();
            // callAsync のコミットを発火させる (実アプリのメッセージループ相当)
            juce::MessageManager::getInstance()->runDispatchLoopUntil(80);
            auto ns = readNotes(clip);
            std::sort(ns.begin(), ns.end(),
                      [](const FoundNote& a, const FoundNote& b) { return a.start < b.start; });
            expect((int) ns.size() == 2, "async commit keeps both notes");
            if (ns.size() == 2)
                expect(std::abs(ns[1].dur - 0.75) < 1e-6, "async commit keeps copy duration");
            expect(ed.getSelectedCountForTests() > 0, "selection survives async commit");
            // Undo でノートもクリップ延長も元へ戻る
            um.undo();
            expect((int) readNotes(clip).size() == 1, "undo removes the duplicate");
            expect(std::abs(clip.getDuration() - 2.0) < 1e-9,
                   "undo restores original clip duration");
            um.redo();
            expect((int) readNotes(clip).size() == 2, "redo restores the duplicate");
            expect(std::abs(clip.getDuration() - 4.0) < 1e-9,
                   "redo restores extended clip duration");
        }

        // ナッジ (実アプリ経路): ↑ 移動後もコミットを跨いで選択が残り、連続 Shift+↑ が効く
        {
            MidiClip clip(0.0, 8.0);
            putNote(clip, 60, 0.0, 0.5);
            PianoRollEditor ed(clip, 120.0);
            ed.setSize(900, 520);
            juce::UndoManager um;
            ed.setUndoManager(&um);
            juce::Component::SafePointer<PianoRollEditor> safeEd(&ed);
            ed.setExternalReloadCallback([safeEd](MidiClip*)
            {
                if (auto* p = safeEd.getComponent()) p->reloadNotesFromClip();
            });
            ed.keyPressed(cmdA);
            const auto shiftUp = juce::KeyPress(juce::KeyPress::upKey,
                                                juce::ModifierKeys(juce::ModifierKeys::shiftModifier), 0);
            ed.keyPressed(shiftUp);
            juce::MessageManager::getInstance()->runDispatchLoopUntil(80);
            expect(ed.getSelectedCountForTests() > 0, "selection survives commit after nudge");
            ed.keyPressed(shiftUp);   // 2 オクターブ目 (選択が残っていれば効く)
            juce::MessageManager::getInstance()->runDispatchLoopUntil(80);
            auto ns = readNotes(clip);
            expect((int) ns.size() == 1 && ns[0].pitch == 84,
                   "second Shift+Up still applies (two octaves total)");
        }

        // 終端クォンタイズ: end 0.13 → 0.125 / 極小は次のグリッド線まで確保
        {
            MidiClip clip(0.0, 8.0);
            putNote(clip, 60, 0.0, 0.13);
            PianoRollEditor ed(clip, 120.0);   // 既定 GRID 1/16 = 0.125s
            ed.setSize(900, 520);
            ed.keyPressed(cmdA);
            ed.quantizeNoteEnds();
            auto ns = readNotes(clip);
            expect((int) ns.size() == 1 && std::abs(ns[0].dur - 0.125) < 1e-6,
                   "note end snapped to grid (0.13 -> 0.125)");
        }

        // 長さを揃える: 最も早いノートの長さへ統一
        {
            MidiClip clip(0.0, 8.0);
            putNote(clip, 60, 0.0, 0.5);
            putNote(clip, 62, 1.0, 0.2);
            PianoRollEditor ed(clip, 120.0);
            ed.setSize(900, 520);
            ed.keyPressed(cmdA);
            ed.equalizeLengths();
            auto ns = readNotes(clip);
            bool allHalf = !ns.empty();
            for (const auto& n : ns) allHalf = allHalf && std::abs(n.dur - 0.5) < 1e-6;
            expect(allHalf, "lengths matched to earliest note (0.5)");
        }

        // 重なり解消: コマンド実行後は同一ピッチのノートが重ならない (契約)。
        // 注: JUCE の updateMatchedPairs が書き込み時点で同一ピッチの重なりを切り詰めるため、
        // クリップ経由の重なりは読み込みで既に正規化されうる。resolveOverlaps の削除/トリム
        // 分岐はエディタ内で作った未書き出し状態も対象で、ここでは最終状態の不変条件
        // 「重なりゼロ + ノートが失われない (全域はカバーされたまま)」を固定する
        {
            MidiClip clip(0.0, 8.0);
            putNote(clip, 60, 0.0, 1.0);
            putNote(clip, 60, 0.5, 1.0);   // 元の全域 = [0, 1.5]
            PianoRollEditor ed(clip, 120.0);
            ed.setSize(900, 520);
            ed.resolveOverlaps();          // 無選択 = 全ノート対象
            auto ns = readNotes(clip);
            std::sort(ns.begin(), ns.end(),
                      [](const FoundNote& a, const FoundNote& b) { return a.start < b.start; });
            expect(! ns.empty(), "notes remain after overlap resolve");
            bool noOverlap = true;
            for (size_t i = 0; i + 1 < ns.size(); ++i)
                if (ns[i].pitch == ns[i + 1].pitch
                    && ns[i].start + ns[i].dur > ns[i + 1].start + 1e-6)
                    noOverlap = false;
            expect(noOverlap, "no same-pitch overlap remains");
            expect(std::abs(ns.front().start - 0.0) < 1e-6, "coverage starts at 0");
        }

        // Shift+↑/↓: オクターブ移動
        {
            MidiClip clip(0.0, 8.0);
            putNote(clip, 60, 0.0, 0.5);
            PianoRollEditor ed(clip, 120.0);
            ed.setSize(900, 520);
            ed.keyPressed(cmdA);
            ed.keyPressed(juce::KeyPress(juce::KeyPress::upKey,
                                         juce::ModifierKeys(juce::ModifierKeys::shiftModifier), 0));
            auto ns = readNotes(clip);
            expect((int) ns.size() == 1 && ns[0].pitch == 72, "Shift+Up moves one octave up");
            ed.keyPressed(juce::KeyPress(juce::KeyPress::downKey,
                                         juce::ModifierKeys(juce::ModifierKeys::shiftModifier), 0));
            ns = readNotes(clip);
            expect((int) ns.size() == 1 && ns[0].pitch == 60, "Shift+Down moves back down");
        }
    }
};

static PianoRollLaneTests pianoRollLaneTests;

} // namespace
