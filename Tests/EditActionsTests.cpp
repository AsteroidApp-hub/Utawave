// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

// Utawave — EditActions の Undo/Redo 往復ユニットテスト
//
// 「Undo でユーザー編集を黙って失う」事故を防ぐ安全網。各 UndoableAction の
// perform()/undo()/redo() を直接叩いて (UndoManager 不要)、状態の往復を検証する:
//   ClipsPropertyAction (全フィールド往復) / ClipAddAction / ClipDeleteAction (同一インスタンス復帰) /
//   ClipSplitAction (start/dur/fileOffset 再計算・フェード曲線継承・ゲインエンベロープ分割+境界補間) /
//   StripSilenceAction (keep セグメント生成・境界フェード・エンベロープ破棄) /
//   LaneSnapshotAction (全フィールド+gainPoints+カスタム色の往復・deferClips が clear 前に発火) /
//   範囲選択削除レシピ (TimelineView::deleteSelectionRange と同一作法の合成 Undo)
//
// 依存はリンク済み (Track.cpp / AudioClip.cpp)。EditActions.h はヘッダオンリー。
// 各アクションは位置/フェード/エンベロープ操作のみでデコードしないためダミー File で可。
// AudioFormatManager は各テストのローカル。expect は ASCII。

#include <JuceHeader.h>
#include <cmath>
#include "../Source/Edit/EditActions.h"
#include "../Source/Edit/ClipInsertGeometry.h"
#include "../Source/Tracks/Track.h"
#include "../Source/Tracks/AudioClip.h"

namespace
{
static bool approxEq(double a, double b, double eps) { return std::abs(a - b) < eps; }

// MIDI テスト用ヘルパ
static void addNoteToSeq(juce::MidiMessageSequence& seq, int note,
                         double onSec, double offSec, int ch = 0)
{
    auto nOn  = juce::MidiMessage::noteOn (ch + 1, note, (juce::uint8) 100);
    auto nOff = juce::MidiMessage::noteOff(ch + 1, note);
    nOn.setTimeStamp(onSec); nOff.setTimeStamp(offSec);
    seq.addEvent(nOn); seq.addEvent(nOff);
    seq.updateMatchedPairs();
}
static void addMidiNote(MidiClip& clip, int note, double onSec, double offSec, int ch = 0)
{
    addNoteToSeq(clip.getSequence(), note, onSec, offSec, ch);
}
static int countNoteOns(const MidiClip& clip)
{
    int n = 0;
    const auto& seq = clip.getSequence();
    for (int i = 0; i < seq.getNumEvents(); ++i)
        if (seq.getEventPointer(i)->message.isNoteOn()) ++n;
    return n;
}

class EditActionsTests : public juce::UnitTest
{
public:
    EditActionsTests() : juce::UnitTest("EditActions (undo/redo)") {}

    void runTest() override
    {
        testClipsProperty();
        testClipsPropertyValidator();
        testRestoreOrderClamp();
        testClipAdd();
        testClipDelete();
        testClipSplit();
        testStripSilence();
        testLaneSnapshot();
        testMidiClipProperty();
        testMidiClipReplaceDelete();
        testMidiClipReplaceCreate();
        testMidiClipReplaceSplit();
        testMidiUndoStalePointerSafety();
        testClipNameUndo();
        testSnapshotAction();
        testRangeDeleteRecipe();
    }

    // ── 範囲選択削除のレシピ (TimelineView::deleteSelectionRange と同じ作法) ──
    // planInsertOverlap (kXf=0) の結果を ClipsPropertyAction / ClipDeleteAction /
    // ClipAddAction (分割の右側末尾) へ写して 1 トランザクションに積む。契約:
    // 範囲 [t1,t2] が空く / 残る内容が時間整合 (fileOffset がタイムライン前進量だけ進み
    // 音が動かない) / Undo 1 回で全復元。TrackManagerTests の Move-to-new-track と同じ
    // 「同一作法」回帰テスト (本体は GUI 側のため直接リンクできない)。
    void testRangeDeleteRecipe()
    {
        beginTest("Range delete recipe: trim/covered leaves gap, undo round-trip");
        juce::AudioFormatManager fmt; fmt.registerBasicFormats();
        juce::AudioThumbnailCache cache(8);
        juce::UndoManager um;
        using namespace ClipInsertGeometry;
        constexpr double kEps = 1e-4;

        Lane lane;
        auto* a = lane.addClip(juce::File("/tmp/rdA.wav"), 0.0, 2.0, fmt, cache);  // LeftCut
        auto* b = lane.addClip(juce::File("/tmp/rdB.wav"), 2.5, 3.0, fmt, cache);  // Covered
        auto* c = lane.addClip(juce::File("/tmp/rdC.wav"), 5.5, 2.5, fmt, cache);  // RightCut
        c->setFileOffset(0.25);
        const double t1 = 1.0, t2 = 6.0;

        um.beginNewTransaction();
        std::vector<AudioClip*> snap;
        for (auto& cp : lane.clips) snap.push_back(cp.get());
        for (auto* clip : snap)
        {
            const Plan plan = planInsertOverlap(clip->getStartPosition(), clip->getEndPosition(),
                                                t1, t2, t2 - t1, 0.0, kEps);
            if (plan.kind == OverlapKind::None) continue;
            if (plan.kind == OverlapKind::Covered)
            {
                um.perform(new EditActions::ClipDeleteAction(&lane, clip, nullptr));
                continue;
            }
            EditActions::ClipState before; before.capture(clip);
            EditActions::ClipState after = before;
            if (plan.kind == OverlapKind::LeftCut)
            {
                after.duration = plan.leftDuration;
                after.fadeOut  = juce::jmin(0.010, plan.leftDuration * 0.5);
            }
            else  // RightCut
            {
                after.startPos   = plan.rightStart;
                after.fileOffset = before.fileOffset + plan.rightFileOffsetDelta;
                after.duration   = plan.rightDuration;
                after.fadeIn     = juce::jmin(0.010, plan.rightDuration * 0.5);
            }
            um.perform(new EditActions::ClipsPropertyAction({ before }, { after }, nullptr, nullptr));
        }

        expect((int) lane.clips.size() == 2, "covered clip removed");
        expect(approxEq(a->getStartPosition(), 0.0, 1e-9) && approxEq(a->getDuration(), 1.0, 1e-9),
               "left clip trimmed to range start");
        expect(approxEq(c->getStartPosition(), 6.0, 1e-9) && approxEq(c->getDuration(), 2.0, 1e-9),
               "right clip starts at range end and keeps tail length");
        expect(approxEq(c->getFileOffset(), 0.75, 1e-9),
               "fileOffset advanced by trim amount (content stays in place)");
        for (auto& cp : lane.clips)
            expect(cp->getEndPosition() <= t1 + 1e-6 || cp->getStartPosition() >= t2 - 1e-6,
                   "range is empty after delete");

        um.undo();
        expect((int) lane.clips.size() == 3, "undo restores covered clip");
        bool bBack = false;
        for (auto& cp : lane.clips) if (cp.get() == b) bBack = true;
        expect(bBack, "covered clip restored as same instance");
        expect(approxEq(a->getDuration(), 2.0, 1e-9)
               && approxEq(c->getStartPosition(), 5.5, 1e-9)
               && approxEq(c->getFileOffset(), 0.25, 1e-9),
               "undo restores trim geometry");

        um.redo();
        expect((int) lane.clips.size() == 2
               && approxEq(a->getDuration(), 1.0, 1e-9)
               && approxEq(c->getStartPosition(), 6.0, 1e-9),
               "redo re-applies range delete");
        um.undo();

        // ── 内包 (Split): 左トリム + 右側末尾の新クリップ ──
        beginTest("Range delete recipe: split inside a clip keeps tail aligned, undo round-trip");
        Lane lane2;
        auto* d = lane2.addClip(juce::File("/tmp/rdD.wav"), 0.0, 10.0, fmt, cache);
        d->setFileOffset(0.25);
        const double s1 = 3.0, s2 = 5.0;
        const Plan plan = planInsertOverlap(d->getStartPosition(), d->getEndPosition(),
                                            s1, s2, s2 - s1, 0.0, kEps);
        expect(plan.kind == OverlapKind::Split, "range inside clip -> Split");

        um.beginNewTransaction();
        EditActions::ClipState before; before.capture(d);
        EditActions::ClipState after = before;
        after.duration = plan.leftDuration;
        after.fadeOut  = juce::jmin(0.010, plan.leftDuration * 0.5);
        um.perform(new EditActions::ClipsPropertyAction({ before }, { after }, nullptr, nullptr));
        EditActions::ClipParams rp;
        rp.file       = d->getFile();
        rp.startPos   = plan.rightStart;
        rp.duration   = plan.rightDuration;
        rp.fileOffset = before.fileOffset + plan.rightFileOffsetDelta;
        um.perform(new EditActions::ClipAddAction(&lane2, rp, fmt, cache, nullptr));

        expect((int) lane2.clips.size() == 2, "split -> left part + tail");
        AudioClip* tail = nullptr;
        for (auto& cp : lane2.clips) if (cp.get() != d) tail = cp.get();
        expect(tail != nullptr, "tail clip created");
        expect(approxEq(d->getDuration(), 3.0, 1e-9), "left part ends at range start");
        expect(approxEq(tail->getStartPosition(), 5.0, 1e-9)
               && approxEq(tail->getStartPosition() + tail->getDuration(), 10.0, 1e-9),
               "tail spans range end to original clip end");
        // アンカー (fileOffset - startPos) が左部分と一致 = 同一連続音声のまま (音が動かない)
        expect(approxEq(tail->getFileOffset() - tail->getStartPosition(),
                        d->getFileOffset() - d->getStartPosition(), 1e-9),
               "tail anchor matches original (same continuous audio)");

        um.undo();
        expect((int) lane2.clips.size() == 1, "undo removes tail");
        expect(approxEq(d->getDuration(), 10.0, 1e-9)
               && approxEq(d->getFileOffset(), 0.25, 1e-9),
               "undo restores original clip");
    }

    // ── ClipState: name の往復 (クリップ名変更の Undo) ──
    void testClipNameUndo()
    {
        beginTest("ClipState: name captured/restored (clip rename undo)");
        juce::AudioFormatManager fmt; fmt.registerBasicFormats();
        juce::AudioThumbnailCache cache(8);
        Lane lane;
        auto* c = lane.addClip(juce::File("/tmp/name.wav"), 0.0, 2.0, fmt, cache);
        c->setName("Before");
        EditActions::ClipState oldS; oldS.capture(c);
        c->setName("After");
        EditActions::ClipState newS; newS.capture(c);
        expect(oldS.differsFrom(newS), "name-only change -> differsFrom true");

        EditActions::ClipsPropertyAction act({ oldS }, { newS }, nullptr, nullptr);
        act.perform();
        expect(c->getName() == juce::String("After"), "perform -> new name");
        act.undo();
        expect(c->getName() == juce::String("Before"), "undo -> old name");
        act.perform();
        expect(c->getName() == juce::String("After"), "redo -> new name");
    }

    // ── SnapshotAction<T>: 値スナップショットの往復 (マーカー/テンポ/トラックプロパティ等の土台) ──
    void testSnapshotAction()
    {
        beginTest("SnapshotAction<T>: value snapshot round-trip (perform/undo/redo)");
        std::vector<int> live   = { 1, 2, 3 };
        std::vector<int> before = live;
        std::vector<int> after  = { 9, 8 };
        int changes = 0;
        EditActions::SnapshotAction<std::vector<int>> act(before, after,
            [&](const std::vector<int>& v) { live = v; ++changes; });
        act.perform();
        expect(live == after,  "perform -> after value applied");
        act.undo();
        expect(live == before, "undo -> before value applied");
        act.perform();
        expect(live == after,  "redo -> after value applied");
        expect(changes == 3,   "apply fired on perform/undo/redo");
    }

    // ── ClipsPropertyAction: 全フィールド往復 ──
    void testClipsProperty()
    {
        beginTest("ClipsPropertyAction: round-trip all fields (perform/undo/redo)");
        juce::AudioFormatManager fmt; fmt.registerBasicFormats();
        juce::AudioThumbnailCache cache(8);
        Lane lane;
        auto* c = lane.addClip(juce::File("/tmp/prop.wav"), 1.0, 4.0, fmt, cache);

        // old 状態
        c->setFileOffset(0.5); c->setFadeInSecs(0.1); c->setFadeOutSecs(0.2);
        c->setGain(0.5f); c->setFadeInCurve(FadeCurve::EqualPower);
        c->getGainPointsRW() = { { 0.0, 0.0f }, { 2.0, -6.0f } };
        EditActions::ClipState oldS; oldS.capture(c);

        // new へ変更
        c->setStartPosition(2.0); c->setDuration(3.0); c->setFileOffset(1.0);
        c->setFadeInSecs(0.3); c->setFadeOutSecs(0.4); c->setGain(0.8f);
        c->setFadeOutCurve(FadeCurve::SCurve);
        c->getGainPointsRW() = { { 0.0, -3.0f } };
        EditActions::ClipState newS; newS.capture(c);

        int changes = 0;
        EditActions::ClipsPropertyAction act({ oldS }, { newS }, [&] { ++changes; }, nullptr);

        act.perform();
        expect(approxEq(c->getStartPosition(), 2.0, 1e-9) && approxEq(c->getDuration(), 3.0, 1e-9),
               "perform -> new geometry");
        expect(approxEq(c->getGain(), 0.8, 1e-6), "perform -> new gain");
        expect(c->getFadeOutCurve() == FadeCurve::SCurve, "perform -> new fade-out curve");
        expect(c->getGainPoints().size() == 1, "perform -> new gainPoints");

        act.undo();
        expect(approxEq(c->getStartPosition(), 1.0, 1e-9) && approxEq(c->getDuration(), 4.0, 1e-9),
               "undo -> old geometry");
        expect(approxEq(c->getFileOffset(), 0.5, 1e-9), "undo -> old fileOffset");
        expect(approxEq(c->getFadeInSecs(), 0.1, 1e-9) && approxEq(c->getFadeOutSecs(), 0.2, 1e-9),
               "undo -> old fades");
        expect(approxEq(c->getGain(), 0.5, 1e-6), "undo -> old gain");
        expect(c->getFadeInCurve() == FadeCurve::EqualPower, "undo -> old fade-in curve");
        expect(c->getGainPoints().size() == 2, "undo -> old gainPoints restored");

        act.perform();   // redo
        expect(approxEq(c->getStartPosition(), 2.0, 1e-9), "redo -> new geometry");
        expect(changes == 3, "onChange fired on perform/undo/redo");
    }

    // ── ClipsPropertyAction: 生存バリデータが破棄済みクリップの restore をスキップ ──
    // v0.5.6 Windows 実クラッシュ (redo → 破棄済み AudioClip::setName の String UAF) の回帰テスト。
    // 履歴中の後続アクション (LaneSnapshotAction 等) がインスタンスを再生成すると、
    // ClipsPropertyAction の生ポインタは undo/redo 時に破棄済みになり得る。
    void testClipsPropertyValidator()
    {
        beginTest("ClipsPropertyAction: validator skips destroyed clips (redo UAF regression)");
        juce::AudioFormatManager fmt; fmt.registerBasicFormats();
        juce::AudioThumbnailCache cache(8);
        Lane lane;
        auto* a = lane.addClip(juce::File("/tmp/val_a.wav"), 0.0, 2.0, fmt, cache);
        auto* b = lane.addClip(juce::File("/tmp/val_b.wav"), 3.0, 2.0, fmt, cache);

        a->setName("A-old"); b->setName("B-old");
        EditActions::ClipState oldA; oldA.capture(a);
        EditActions::ClipState oldB; oldB.capture(b);
        a->setName("A-new"); a->setGain(0.7f);
        b->setName("B-new"); b->setGain(0.6f);
        EditActions::ClipState newA; newA.capture(a);
        EditActions::ClipState newB; newB.capture(b);

        // clipStillExists と同じ作法のレーン走査バリデータ (呼ばれた回数も数える)
        int queries = 0;
        auto validator = [&lane, &queries](AudioClip* c)
        {
            ++queries;
            for (auto& cp : lane.clips)
                if (cp.get() == c) return true;
            return false;
        };
        EditActions::ClipsPropertyAction act({ oldA, oldB }, { newA, newB },
                                             nullptr, validator);

        // クリップ A を破棄 (LaneSnapshotAction による再生成を模す)。act は dangling a を保持
        lane.clips.erase(lane.clips.begin());
        expect(lane.clips.size() == 1 && lane.clips[0].get() == b, "clip A destroyed, B remains");

        act.perform();   // redo 相当: A はスキップ (UAF しない)、B だけ restore
        expect(queries == 2, "validator queried for both clips");
        expect(b->getName() == juce::String("B-new"), "perform -> live clip restored");

        act.undo();      // undo も同様に A をスキップ
        expect(b->getName() == juce::String("B-old"), "undo -> live clip restored to old");
        expect(approxEq(b->getGain(), oldB.gain, 1e-6), "undo -> live clip gain restored");
        expect(queries == 4, "validator queried on undo too");
    }

    // ── ClipState::restore の順序 (duration を fade より先に復元) を区別するケース ──
    // 旧 fadeIn が「復元後の duration では valid だが、途中の (小さい) duration ではクリップされる」
    // 値を使うことで、duration→fade の順序でないと正しく復元できないことを検証する。
    void testRestoreOrderClamp()
    {
        beginTest("ClipState.restore: duration restored before fades (old fade survives shrink->grow undo)");
        juce::AudioFormatManager fmt; fmt.registerBasicFormats();
        juce::AudioThumbnailCache cache(8);
        Lane lane;
        auto* c = lane.addClip(juce::File("/tmp/order.wav"), 0.0, 4.0, fmt, cache);

        // old: duration 4.0, fadeIn 1.8 (valid: <= 2.0)
        c->setFadeInSecs(1.8);
        EditActions::ClipState oldS; oldS.capture(c);
        expect(approxEq(oldS.fadeIn, 1.8, 1e-9), "captured old fadeIn = 1.8");

        // new: duration 1.0 -> 既存 fadeIn は setDuration で 0.5 に再クランプされる
        c->setDuration(1.0);
        EditActions::ClipState newS; newS.capture(c);

        EditActions::ClipsPropertyAction act({ oldS }, { newS }, nullptr, nullptr);
        act.perform();   // new (dur 1.0, fadeIn 0.5)
        expect(approxEq(c->getDuration(), 1.0, 1e-9) && approxEq(c->getFadeInSecs(), 0.5, 1e-9),
               "new state: dur 1.0, fadeIn clamped to 0.5");

        act.undo();      // old: setDuration(4.0) THEN setFadeInSecs(1.8) -> 1.8 survives (<= 2.0)
        expect(approxEq(c->getDuration(), 4.0, 1e-9), "undo restores duration 4.0");
        expect(approxEq(c->getFadeInSecs(), 1.8, 1e-9),
               "undo restores fadeIn 1.8 (only correct if duration is restored before fades)");
    }

    // ── ClipAddAction ──
    void testClipAdd()
    {
        beginTest("ClipAddAction: applies params; undo removes; redo restores same instance");
        juce::AudioFormatManager fmt; fmt.registerBasicFormats();
        juce::AudioThumbnailCache cache(8);
        Lane lane;
        EditActions::ClipParams p;
        p.file = juce::File("/tmp/add.wav"); p.startPos = 2.0; p.duration = 3.0;
        p.fileOffset = 0.5; p.fadeIn = 0.05; p.fadeOut = 0.06; p.gain = 0.6f;
        p.name = "Added"; p.colour = juce::Colour(0xff778899); p.customColour = true;

        int changes = 0;
        EditActions::ClipAddAction act(&lane, p, fmt, cache, [&] { ++changes; });

        act.perform();
        expect(lane.clips.size() == 1, "perform -> 1 clip added");
        auto* added = act.getAddedClip();
        expect(added != nullptr && added == lane.clips[0].get(), "getAddedClip matches the lane clip");
        if (added)
        {
            expect(approxEq(added->getStartPosition(), 2.0, 1e-9) && approxEq(added->getDuration(), 3.0, 1e-9),
                   "geometry from params");
            expect(approxEq(added->getFileOffset(), 0.5, 1e-9), "fileOffset from params");
            expect(approxEq(added->getFadeInSecs(), 0.05, 1e-9) && approxEq(added->getFadeOutSecs(), 0.06, 1e-9),
                   "fades from params");
            expect(approxEq(added->getGain(), 0.6, 1e-6), "gain from params");
            expect(added->getName() == "Added", "name from params");
            expect(added->hasCustomColour(), "colour set");
        }

        AudioClip* firstInstance = added;
        act.undo();
        expect(lane.clips.size() == 0, "undo -> removed");
        act.perform();   // redo
        expect(lane.clips.size() == 1, "redo -> restored");
        expect(lane.clips[0].get() == firstInstance, "redo restores the SAME instance (stored unique_ptr)");
        expect(changes == 3, "onChange on perform/undo/redo");

        // 既定 (customColour=false) はトラック色追従のまま。旧実装は colour を無条件に
        // 焼き付けていたため、トラック色追従クリップの移動/コピーで既定の青に変わっていた
        // (「移動すると色が変わる」回帰テスト)。
        EditActions::ClipParams p2;
        p2.file = juce::File("/tmp/add2.wav"); p2.startPos = 6.0; p2.duration = 1.0;
        EditActions::ClipAddAction act2(&lane, p2, fmt, cache, nullptr);
        act2.perform();
        expect(act2.getAddedClip() != nullptr
               && ! act2.getAddedClip()->hasCustomColour(),
               "default params keep track-follow colour (no custom colour burned)");
    }

    // ── ClipDeleteAction ──
    void testClipDelete()
    {
        beginTest("ClipDeleteAction: perform removes; undo restores same instance");
        juce::AudioFormatManager fmt; fmt.registerBasicFormats();
        juce::AudioThumbnailCache cache(8);
        Lane lane;
        auto* c = lane.addClip(juce::File("/tmp/del.wav"), 0.0, 2.0, fmt, cache);
        AudioClip* inst = c;

        int changes = 0;
        EditActions::ClipDeleteAction act(&lane, c, [&] { ++changes; });

        act.perform();
        expect(lane.clips.size() == 0, "perform -> removed");
        act.undo();
        expect(lane.clips.size() == 1 && lane.clips[0].get() == inst, "undo restores the same instance");
        act.perform();   // redo
        expect(lane.clips.size() == 0, "redo -> removed again");
        expect(changes == 3, "onChange on perform/undo/redo");
    }

    // ── ClipSplitAction (最重要: fileOffset 再計算・フェード曲線継承・エンベロープ分割) ──
    void testClipSplit()
    {
        beginTest("ClipSplitAction: geometry, fileOffset recalc, fade-curve + gain-envelope inheritance, undo");
        juce::AudioFormatManager fmt; fmt.registerBasicFormats();
        juce::AudioThumbnailCache cache(8);
        Lane lane;
        auto* orig = lane.addClip(juce::File("/tmp/split.wav"), 1.0, 4.0, fmt, cache);
        orig->setFileOffset(0.5);
        orig->setFadeInSecs(0.1);
        orig->setFadeOutSecs(0.2);
        orig->setFadeInCurve(FadeCurve::EqualPower);
        orig->setFadeOutCurve(FadeCurve::SCurve);
        orig->setColour(juce::Colour(0xff112233));   // カスタム色
        orig->getGainPointsRW() = { { 0.0, 0.0f }, { 2.0, -6.0f }, { 4.0, 0.0f } };

        int deferred = 0, changes = 0;
        EditActions::ClipSink sink = [&](std::vector<std::unique_ptr<AudioClip>>&& v) { deferred += (int) v.size(); };
        EditActions::ClipSplitAction act(&lane, orig, /*splitPos=*/3.0, fmt, cache, [&] { ++changes; }, sink);

        act.perform();
        expect(lane.clips.size() == 2, "perform -> 2 clips (left + right)");
        AudioClip* L = nullptr; AudioClip* R = nullptr;
        for (auto& c : lane.clips)
        {
            if (approxEq(c->getStartPosition(), 1.0, 1e-6)) L = c.get();
            if (approxEq(c->getStartPosition(), 3.0, 1e-6)) R = c.get();
        }
        expect(L != nullptr && R != nullptr, "left at 1.0 and right at 3.0 exist");
        if (L && R)
        {
            expect(approxEq(L->getDuration(), 2.0, 1e-9), "left dur = split - start");
            expect(approxEq(L->getFileOffset(), 0.5, 1e-9), "left keeps original fileOffset");
            expect(approxEq(L->getFadeInSecs(), 0.1, 1e-9), "left inherits fade-in");
            expect(L->getFadeInCurve() == FadeCurve::EqualPower, "left inherits fade-in curve");
            expect(approxEq(R->getDuration(), 2.0, 1e-9), "right dur = end - split");
            expect(approxEq(R->getFileOffset(), 0.5 + 2.0, 1e-9), "right fileOffset recalculated (+ split offset)");
            expect(approxEq(R->getFadeOutSecs(), 0.2, 1e-9), "right inherits fade-out");
            expect(R->getFadeOutCurve() == FadeCurve::SCurve, "right inherits fade-out curve");
            expect(L->hasCustomColour() && R->hasCustomColour(), "custom colour inherited by both");

            // ゲインエンベロープ分割 + 境界補間 (split @ clip-time 2.0, dB@split = -6)
            const auto& lp = L->getGainPoints();
            expect(lp.size() == 2
                   && approxEq(lp[0].time, 0.0, 1e-9) && approxEq(lp[0].dB, 0.0, 1e-6)
                   && approxEq(lp[1].time, 2.0, 1e-9) && approxEq(lp[1].dB, -6.0, 1e-6),
                   "left env = pre-split point (0,0) + boundary (2,-6)");
            const auto& rp = R->getGainPoints();
            expect(rp.size() == 2
                   && approxEq(rp[0].time, 0.0, 1e-9) && approxEq(rp[0].dB, -6.0, 1e-6)
                   && approxEq(rp[1].time, 2.0, 1e-9) && approxEq(rp[1].dB, 0.0, 1e-6),
                   "right env = boundary (0,-6) + shifted post-split (2,0)");
        }

        act.undo();
        expect(deferred == 2, "deferClips received left+right (2 clips) on undo");
        expect(lane.clips.size() == 1, "undo -> original restored (1 clip)");
        if (lane.clips.size() == 1)
        {
            auto* restored = lane.clips[0].get();
            expect(restored == orig, "restored is the SAME original instance");
            expect(approxEq(restored->getStartPosition(), 1.0, 1e-9) && approxEq(restored->getDuration(), 4.0, 1e-9),
                   "original geometry restored");
            expect(approxEq(restored->getFileOffset(), 0.5, 1e-9), "original fileOffset restored");
            expect(approxEq(restored->getFadeInSecs(), 0.1, 1e-9) && approxEq(restored->getFadeOutSecs(), 0.2, 1e-9),
                   "original fades restored");
            expect(restored->getFadeInCurve() == FadeCurve::EqualPower, "original fade-in curve restored");
            expect(restored->getGainPoints().size() == 3, "original 3 gain points restored");
        }
        expect(changes >= 2, "onChange fired on perform and undo");
    }

    // ── StripSilenceAction ──
    void testStripSilence()
    {
        beginTest("StripSilenceAction: keep segments with boundary fades; envelope dropped; undo restores");
        juce::AudioFormatManager fmt; fmt.registerBasicFormats();
        juce::AudioThumbnailCache cache(8);
        Lane lane;
        auto* orig = lane.addClip(juce::File("/tmp/strip.wav"), 0.0, 10.0, fmt, cache);
        orig->setFileOffset(0.5);   // 非ゼロにして fileOffset = origOffset + segStart の origOffset 項を検証
        orig->setFadeInSecs(0.1);
        orig->setFadeOutSecs(0.2);
        orig->getGainPointsRW() = { { 0.0, 0.0f }, { 5.0, -3.0f } };   // 破棄される想定

        int deferred = 0, changes = 0;
        EditActions::ClipSink sink = [&](std::vector<std::unique_ptr<AudioClip>>&& v) { deferred += (int) v.size(); };
        std::vector<EditActions::StripSilenceAction::Segment> segs = { { 0.0, 3.0 }, { 5.0, 9.0 } };
        EditActions::StripSilenceAction act(&lane, orig, segs, /*fadeSecs=*/0.01, fmt, cache,
                                            [&] { ++changes; }, sink);

        act.perform();
        expect(lane.clips.size() == 2, "perform -> 2 keep segments");
        AudioClip* s0 = nullptr; AudioClip* s1 = nullptr;
        for (auto& c : lane.clips)
        {
            if (approxEq(c->getStartPosition(), 0.0, 1e-6)) s0 = c.get();
            if (approxEq(c->getStartPosition(), 5.0, 1e-6)) s1 = c.get();
        }
        expect(s0 != nullptr && s1 != nullptr, "segments at 0.0 and 5.0 exist");
        if (s0 && s1)
        {
            expect(approxEq(s0->getDuration(), 3.0, 1e-9), "first segment dur = 3");
            expect(approxEq(s0->getFileOffset(), 0.5, 1e-9), "first fileOffset = origOffset (0.5) + segStart (0)");
            expect(approxEq(s0->getFadeInSecs(), 0.1, 1e-9), "first keeps original fade-in");
            expect(approxEq(s0->getFadeOutSecs(), 0.01, 1e-9), "first inner boundary fade-out");
            expect(approxEq(s1->getDuration(), 4.0, 1e-9), "second segment dur = 4");
            expect(approxEq(s1->getFileOffset(), 5.5, 1e-9), "second fileOffset = origOffset (0.5) + segStart (5)");
            expect(approxEq(s1->getFadeInSecs(), 0.01, 1e-9), "second inner boundary fade-in");
            expect(approxEq(s1->getFadeOutSecs(), 0.2, 1e-9), "last keeps original fade-out");
            expect(s0->getGainPoints().empty() && s1->getGainPoints().empty(),
                   "gain envelope dropped on the keep segments");
        }

        act.undo();
        expect(deferred == 2, "deferClips received the 2 created segments");
        expect(lane.clips.size() == 1, "undo -> original restored");
        if (lane.clips.size() == 1)
        {
            auto* restored = lane.clips[0].get();
            expect(approxEq(restored->getDuration(), 10.0, 1e-9), "original duration restored");
            expect(approxEq(restored->getFadeInSecs(), 0.1, 1e-9) && approxEq(restored->getFadeOutSecs(), 0.2, 1e-9),
                   "original fades restored");
            expect(restored->getGainPoints().size() == 2, "original gain envelope restored on undo");
        }
        expect(changes >= 2, "onChange fired on perform and undo");
    }

    // ── LaneSnapshotAction ──
    void testLaneSnapshot()
    {
        beginTest("LaneSnapshotAction: round-trip all fields incl gainPoints/custom colour; deferClips before clear");
        juce::AudioFormatManager fmt; fmt.registerBasicFormats();
        juce::AudioThumbnailCache cache(8);
        Lane lane;
        auto* a = lane.addClip(juce::File("/tmp/a.wav"), 0.0, 2.0, fmt, cache);
        a->setFileOffset(0.25); a->setGain(0.7f); a->setName("Clip A");
        a->setColour(juce::Colour(0xff445566)); a->setFadeInCurve(FadeCurve::EqualPower);
        a->setFadeInSecs(0.05); a->setFadeOutSecs(0.06);
        a->getGainPointsRW() = { { 0.0, -3.0f }, { 1.0, 0.0f } };
        lane.addClip(juce::File("/tmp/b.wav"), 3.0, 1.0, fmt, cache)->setGain(1.2f);

        std::vector<EditActions::LaneSnapshotAction::ClipSnap> before;
        for (auto& c : lane.clips)
            before.push_back(EditActions::LaneSnapshotAction::ClipSnap::capture(c.get()));

        std::vector<EditActions::LaneSnapshotAction::ClipSnap> after;
        {
            EditActions::LaneSnapshotAction::ClipSnap s;
            s.file = juce::File("/tmp/c.wav"); s.startPos = 5.0; s.duration = 2.0; s.gain = 0.4f;
            after.push_back(s);
        }

        int deferred = 0, changes = 0;
        EditActions::ClipSink sink = [&](std::vector<std::unique_ptr<AudioClip>>&& v) { deferred += (int) v.size(); };
        EditActions::LaneSnapshotAction act(&lane, before, after, fmt, cache, [&] { ++changes; }, sink);

        act.perform();
        expect(deferred == 2, "deferClips received the 2 'before' clips (moved out before repopulate)");
        expect(lane.clips.size() == 1, "perform -> 1 clip (after snapshot)");
        if (lane.clips.size() == 1)
            expect(approxEq(lane.clips[0]->getStartPosition(), 5.0, 1e-9)
                   && approxEq(lane.clips[0]->getGain(), 0.4, 1e-6), "after-snapshot props applied");

        act.undo();
        expect(lane.clips.size() == 2, "undo -> 2 clips (before snapshot)");
        AudioClip* ra = nullptr;
        for (auto& c : lane.clips)
            if (approxEq(c->getStartPosition(), 0.0, 1e-9)) ra = c.get();
        expect(ra != nullptr, "before clip A restored at 0.0");
        if (ra)
        {
            expect(approxEq(ra->getFileOffset(), 0.25, 1e-9), "fileOffset restored");
            expect(approxEq(ra->getGain(), 0.7, 1e-6), "gain restored");
            expect(ra->getName() == "Clip A", "name restored");
            expect(ra->hasCustomColour(), "custom colour restored");
            expect(ra->getFadeInCurve() == FadeCurve::EqualPower, "fade-in curve restored");
            expect(ra->getGainPoints().size() == 2, "gainPoints restored");
        }
        expect(changes == 2, "onChange fired on perform + undo");
    }

    // ── MidiClipPropertyAction: 移動 / リサイズ (start/dur 往復・実体は据え置き) ──
    void testMidiClipProperty()
    {
        beginTest("MidiClipPropertyAction: move/resize round-trip, same instance");
        juce::AudioFormatManager fmt; juce::AudioThumbnailCache cache(8);
        Track track("MIDI", fmt, cache);
        track.setMidiTrack(true);
        auto* clip = track.addMidiClip(1.0, 4.0);
        addMidiNote(*clip, 60, 0.0, 1.0);

        int changes = 0;
        EditActions::MidiClipPropertyAction act(clip, 1.0, 4.0, 5.0, 2.0, [&] { ++changes; });

        act.perform();
        expect(approxEq(clip->getStartPosition(), 5.0, 1e-9), "perform -> new start");
        expect(approxEq(clip->getDuration(), 2.0, 1e-9), "perform -> new duration");

        act.undo();
        expect(approxEq(clip->getStartPosition(), 1.0, 1e-9), "undo -> old start");
        expect(approxEq(clip->getDuration(), 4.0, 1e-9), "undo -> old duration");

        act.perform();   // redo
        expect(approxEq(clip->getStartPosition(), 5.0, 1e-9), "redo -> new start");
        expect(changes == 3, "onChange fired per perform/undo/redo");
        // クリップ実体は作り直さないので同一インスタンス (ピアノロールが保持される条件)
        expect(track.getMidiClipCount() == 1 && track.getMidiClip(0) == clip, "same clip instance");
    }

    // ── MidiClipReplaceAction (削除): 元クリップを同一インスタンスで復活 ──
    void testMidiClipReplaceDelete()
    {
        beginTest("MidiClipReplaceAction: delete restores same instance + notes; keeps others");
        juce::AudioFormatManager fmt; juce::AudioThumbnailCache cache(8);
        Track track("MIDI", fmt, cache);
        track.setMidiTrack(true);
        auto* keep = track.addMidiClip(0.0, 2.0);  addMidiNote(*keep, 48, 0.0, 1.0);
        auto* del  = track.addMidiClip(2.0, 2.0);  addMidiNote(*del, 60, 0.0, 0.5); addMidiNote(*del, 64, 0.5, 1.0);

        int willRemove = 0;
        EditActions::MidiClipReplaceAction act(
            &track, { del }, {}, [] {}, [&](MidiClip*) { ++willRemove; });

        act.perform();
        expect(track.getMidiClipCount() == 1, "perform -> one clip remains");
        expect(track.getMidiClip(0) == keep, "perform -> kept clip identity preserved");
        expect(willRemove == 1, "willRemove called once (for deleted clip)");

        act.undo();
        expect(track.getMidiClipCount() == 2, "undo -> two clips");
        bool delBack = false;
        for (int i = 0; i < track.getMidiClipCount(); ++i)
            if (track.getMidiClip(i) == del) delBack = true;
        expect(delBack, "undo -> deleted clip restored as same instance");
        expect(countNoteOns(*del) == 2, "undo -> deleted clip notes preserved");

        act.perform();   // redo
        expect(track.getMidiClipCount() == 1 && track.getMidiClip(0) == keep, "redo -> deleted again");
    }

    // ── MidiClipReplaceAction (作成): undo で消え redo で再生成 ──
    void testMidiClipReplaceCreate()
    {
        beginTest("MidiClipReplaceAction: create/undo/redo");
        juce::AudioFormatManager fmt; juce::AudioThumbnailCache cache(8);
        Track track("MIDI", fmt, cache);
        track.setMidiTrack(true);

        EditActions::MidiClipReplaceAction::NewMidiClip np;
        np.startPos = 3.0; np.duration = 1.5; np.name = "New"; np.channel = 2;
        addNoteToSeq(np.sequence, 72, 0.0, 0.5);

        EditActions::MidiClipReplaceAction act(&track, {}, { np }, [] {}, {});

        act.perform();
        expect(track.getMidiClipCount() == 1, "perform -> clip created");
        auto* c = track.getMidiClip(0);
        expect(c != nullptr && approxEq(c->getStartPosition(), 3.0, 1e-9), "created start 3.0");
        expect(c != nullptr && c->getChannel() == 2 && c->getName() == "New", "created channel/name");
        expect(c != nullptr && countNoteOns(*c) == 1, "created notes copied");

        act.undo();
        expect(track.getMidiClipCount() == 0, "undo -> clip removed");

        act.perform();   // redo
        expect(track.getMidiClipCount() == 1, "redo -> clip re-created");
        // redo は同一インスタンスを戻す (作り直さない)。これにより、この追加クリップの生ポインタを
        // 掴む後段の MidiClipPropertyAction が undo/redo を跨いでもダングリングしない。
        expect(track.getMidiClip(0) == c, "redo restores the SAME instance (no UAF for stacked actions)");
    }

    // ── 重ねた Undo: 作成→移動→undo×2→redo×2 で生ポインタが UAF しない (回帰テスト) ──
    void testMidiUndoStalePointerSafety()
    {
        beginTest("MIDI undo: create+move survive undo x2 / redo x2 without dangling");
        juce::AudioFormatManager fmt; juce::AudioThumbnailCache cache(8);
        Track track("MIDI", fmt, cache);
        track.setMidiTrack(true);

        EditActions::MidiClipReplaceAction::NewMidiClip np;
        np.startPos = 0.0; np.duration = 2.0;
        EditActions::MidiClipReplaceAction createAct(&track, {}, { np }, [] {}, {});
        createAct.perform();
        auto* a = track.getMidiClip(0);
        expect(a != nullptr, "clip created");

        // 作成したクリップを掴む property アクション (移動)
        EditActions::MidiClipPropertyAction moveAct(a, 0.0, 2.0, 5.0, 2.0, [] {});
        moveAct.perform();
        expect(approxEq(a->getStartPosition(), 5.0, 1e-9), "moved to 5.0");

        moveAct.undo();                          // 移動を戻す
        createAct.undo();                        // 作成を戻す (a は破棄せず退避されるはず)
        expect(track.getMidiClipCount() == 0, "create undone");

        createAct.perform();                     // 作成を redo → 同一インスタンスが戻る
        expect(track.getMidiClipCount() == 1, "create redone");
        expect(track.getMidiClip(0) == a, "redo restores SAME instance (moveAct's pointer stays valid)");

        moveAct.perform();                       // 移動を redo → 退避していた a を安全に参照
        expect(approxEq(a->getStartPosition(), 5.0, 1e-9), "redo move applied to the valid instance");
    }

    // ── MidiClipReplaceAction (分割): 元クリップを undo で同一インスタンス復活 ──
    void testMidiClipReplaceSplit()
    {
        beginTest("MidiClipReplaceAction: split removes original, undo restores it");
        juce::AudioFormatManager fmt; juce::AudioThumbnailCache cache(8);
        Track track("MIDI", fmt, cache);
        track.setMidiTrack(true);
        auto* orig = track.addMidiClip(0.0, 4.0);
        addMidiNote(*orig, 60, 0.0, 1.0);
        addMidiNote(*orig, 64, 3.0, 3.5);

        using NewMidiClip = EditActions::MidiClipReplaceAction::NewMidiClip;
        NewMidiClip left, right;
        left.startPos = 0.0;  left.duration  = 2.0;
        right.startPos = 2.0;  right.duration = 2.0;
        addNoteToSeq(left.sequence, 60, 0.0, 1.0);
        addNoteToSeq(right.sequence, 64, 1.0, 1.5);

        EditActions::MidiClipReplaceAction act(&track, { orig }, { left, right }, [] {}, {});

        act.perform();
        expect(track.getMidiClipCount() == 2, "perform -> two clips");
        bool origPresent = false;
        for (int i = 0; i < track.getMidiClipCount(); ++i)
            if (track.getMidiClip(i) == orig) origPresent = true;
        expect(! origPresent, "perform -> original removed");

        act.undo();
        expect(track.getMidiClipCount() == 1, "undo -> back to one clip");
        expect(track.getMidiClip(0) == orig, "undo -> original restored (same instance)");
        expect(countNoteOns(*orig) == 2, "undo -> original notes intact");

        act.perform();   // redo
        expect(track.getMidiClipCount() == 2, "redo -> split again");
    }
};

static EditActionsTests editActionsTests;
}
