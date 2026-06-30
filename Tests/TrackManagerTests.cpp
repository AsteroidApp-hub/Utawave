// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

// Utawave — TrackManager のユニットテスト (色サイクル / 自動命名 / 複製 / フォルダ署名)
//
//   ・色サイクル: nextColourIndex が mod 8 で巡回し、9 本目が 1 本目と同色 (負数対応は paletteColour)
//   ・自動命名: 空名は既存 "Track N" の最大 + 1。明示名は尊重
//   ・hasClickTrack / addClickTrack (1 本のみ) / hasMidiTrack
//   ・duplicateTrack: 直後に挿入・基本プロパティ/クリップ/レーン solo/MIDI を深くコピー・
//                     recArm/solo は引き継がない・Click/範囲外は nullptr・名前は一意化
//   ・audioFolderSignature: 拡張子フィルタ・内容変化で署名変化・非ディレクトリは空・決定論的
// クリップはダミー File (デコードしない)。複製名は tr() を介すのでロケール非依存に検証する。
// AudioFormatManager は runTest ローカル (静的だと終了時 leak assertion)。expect は ASCII。

#include <JuceHeader.h>
#include "../Source/Tracks/TrackManager.h"
#include "../Source/Edit/TrackActions.h"
#include "../Source/Edit/EditActions.h"
#include "../Source/Localisation.h"

namespace
{
class TrackManagerTests : public juce::UnitTest
{
public:
    TrackManagerTests() : juce::UnitTest("TrackManager") {}

    void runTest() override
    {
        testColourCycle();
        testAutoNaming();
        testAddTrackInsertAfter();
        testClickAndMidiQueries();
        testDuplicateBasic();
        testDuplicateMidiDeepCopy();
        testDuplicateExcludeTakeLanes();
        testDuplicateGuardsAndUniqueName();
        testAudioFolderSignature();
        testExtractInsertIndexOf();
        testTrackAddAction();
        testTrackDeleteAction();
        testReorderTo();
        testTrackReorderAction();
        testMoveClipToNewTrackUndo();
    }

    // ── 色サイクル: track i は paletteColour(i)、9 本目 (idx 8) は 1 本目と同色 ──
    void testColourCycle()
    {
        beginTest("addTrack assigns palette colours cyclically (mod 8)");
        juce::AudioFormatManager fmt; fmt.registerBasicFormats();
        TrackManager tm(fmt);
        for (int i = 0; i < 9; ++i) tm.addTrack();
        for (int i = 0; i < 9; ++i)
            expect(tm.getTrack(i)->getColour() == Track::paletteColour(i),
                   ("track " + juce::String(i) + " uses paletteColour(i)").toRawUTF8());
        expect(tm.getTrack(8)->getColour() == tm.getTrack(0)->getColour(),
               "9th track wraps to the 1st palette colour");
        // paletteColour は負数も巡回する
        expect(Track::paletteColour(-1) == Track::paletteColour(7), "paletteColour(-1) == (7)");
        expect(Track::paletteColour(-8) == Track::paletteColour(0), "paletteColour(-8) == (0)");
    }

    // ── 自動命名: 空名は max("Track N") + 1、明示名は尊重 ──
    void testAutoNaming()
    {
        beginTest("addTrack auto-numbers empty names as Track N (max + 1)");
        juce::AudioFormatManager fmt; fmt.registerBasicFormats();
        TrackManager tm(fmt);
        expect(tm.addTrack()->getName() == "Track 1", "first empty -> Track 1");
        expect(tm.addTrack()->getName() == "Track 2", "second empty -> Track 2");
        expect(tm.addTrack("Track 5")->getName() == "Track 5", "explicit name is respected");
        expect(tm.addTrack()->getName() == "Track 6", "next empty -> max(5) + 1 = Track 6");
        expect(tm.addTrack("Vocals")->getName() == "Vocals", "non-Track name is respected");
    }

    // ── addTrack(insertAfter): 指定インデックスの直後に挿入・-1/範囲外は末尾 ──
    void testAddTrackInsertAfter()
    {
        beginTest("addTrack inserts after a given index; -1/out-of-range appends");
        juce::AudioFormatManager fmt; fmt.registerBasicFormats();
        TrackManager tm(fmt);
        auto* a = tm.addTrack("A");
        auto* b = tm.addTrack("B");
        auto* c = tm.addTrack("C");                      // [A, B, C]
        juce::ignoreUnused(a, c);

        // index 1 (B) の直後へ挿入 → [A, B, X, C]
        auto* x = tm.addTrack("X", false, /*insertAfter=*/1);
        expect(tm.indexOf(x) == 2, "inserted right after index 1");
        expect(tm.indexOf(b) == 1 && tm.indexOf(c) == 3, "following tracks shift down");

        // -1 は末尾 → [A, B, X, C, Y]
        auto* y = tm.addTrack("Y", false, /*insertAfter=*/-1);
        expect(tm.indexOf(y) == 4, "insertAfter -1 appends at the end");

        // 範囲外も末尾 → [..., Z]
        auto* z = tm.addTrack("Z", false, /*insertAfter=*/99);
        expect(tm.indexOf(z) == 5, "out-of-range insertAfter appends at the end");
    }

    // ── hasClickTrack / addClickTrack (1 本のみ) / hasMidiTrack ──
    void testClickAndMidiQueries()
    {
        beginTest("click track is unique; hasMidiTrack tracks MIDI tracks");
        juce::AudioFormatManager fmt; fmt.registerBasicFormats();
        TrackManager tm(fmt);
        expect(!tm.hasClickTrack(), "no click track initially");
        expect(!tm.hasMidiTrack(),  "no MIDI track initially");

        auto* click = tm.addClickTrack();
        expect(click != nullptr && click->isClickTrack(), "addClickTrack returns a click track");
        expect(tm.hasClickTrack(), "hasClickTrack true after adding");
        expect(tm.addClickTrack() == nullptr, "second addClickTrack returns nullptr (unique)");

        auto* midi = tm.addTrack("Synth");
        expect(!tm.hasMidiTrack(), "audio track does not count as MIDI");
        midi->setMidiTrack(true);
        expect(tm.hasMidiTrack(), "hasMidiTrack true once a track is MIDI");
    }

    // ── duplicateTrack: 直後挿入・基本プロパティ/クリップ/レーン solo をコピー・recArm/solo 非継承 ──
    void testDuplicateBasic()
    {
        beginTest("duplicateTrack copies properties/clips, inserts after, drops recArm/solo");
        juce::AudioFormatManager fmt; fmt.registerBasicFormats();
        TrackManager tm(fmt);
        auto* src = tm.addTrack("Vocals");
        src->setVolume(-6.0f); src->setPan(0.3f); src->setReverbSend(0.2f);
        src->setMuted(true); src->setRecArmed(true); src->setSoloed(true);
        src->setInputMonitor(true); src->setInputChannel(1); src->setStereo(true);
        src->setCustomHeight(140);

        juce::File dummy("/tmp/utawave_dummy_clip.wav");
        auto* clip = src->addClip(dummy, 1.0, 2.0);
        expect(clip != nullptr, "source clip created");
        clip->setFileOffset(0.5); clip->setGain(0.7f);
        clip->getGainPointsRW().push_back({ 0.5, -3.0f });
        src->getLane(0)->soloed = true;

        auto* dst = tm.duplicateTrack(0);
        expect(dst != nullptr, "duplicate returns a track");
        expect(tm.getTrackCount() == 2 && tm.getTrack(1) == dst, "inserted right after source");
        expect(dst->getName() == juce::String("Vocals (1)"),
               "name is source + numbered suffix");

        expect(juce::approximatelyEqual(dst->getVolume(), -6.0f), "volume copied");
        expect(juce::approximatelyEqual(dst->getPan(), 0.3f),     "pan copied");
        expect(juce::approximatelyEqual(dst->getReverbSend(), 0.2f), "reverbSend copied");
        expect(dst->isMuted(), "muted copied");
        expect(dst->isInputMonitor(), "inputMonitor copied");
        expect(dst->getInputChannel() == 1, "inputChannel copied");
        expect(dst->isStereo(), "stereo copied");
        expect(dst->getCustomHeight() == 140, "customHeight copied");
        // 録音アーム・ソロは混乱回避のため引き継がない
        expect(!dst->isRecArmed(), "recArm is NOT inherited");
        expect(!dst->isSoloed(),   "track solo is NOT inherited");

        // クリップの深いコピー
        auto* dl = dst->getLane(0);
        expect(dl != nullptr && (int) dl->clips.size() == 1, "one clip copied to lane 0");
        if (dl && dl->clips.size() == 1)
        {
            auto* dc = dl->clips[0].get();
            expect(dc->getFile() == dummy, "clip file copied");
            expect(juce::approximatelyEqual(dc->getStartPosition(), 1.0), "clip start copied");
            expect(juce::approximatelyEqual(dc->getDuration(), 2.0),      "clip duration copied");
            expect(juce::approximatelyEqual(dc->getFileOffset(), 0.5),    "clip fileOffset copied");
            expect(juce::approximatelyEqual(dc->getGain(), 0.7f),         "clip gain copied");
            expect((int) dc->getGainPoints().size() == 1, "gain points deep-copied (count)");
        }
        expect(dst->getLane(0)->soloed.load(), "lane soloed copied");
    }

    // ── includeTakeLanes=false (Option 押下複製): Lane 0 のみコピーしテイクレーンは複製しない ──
    void testDuplicateExcludeTakeLanes()
    {
        beginTest("duplicateTrack(includeTakeLanes=false) copies only Lane 0, drops take lanes");
        juce::AudioFormatManager fmt; fmt.registerBasicFormats();
        TrackManager tm(fmt);
        auto* src = tm.addTrack("Vocals");
        juce::File dummy("/tmp/utawave_dummy_clip.wav");
        src->addClip(dummy, 0.0, 1.0);                 // Lane 0
        src->ensureLane(1)->addClip(dummy, 0.0, 1.0,   // Take lane (Lane 1)
                                    fmt, tm.getThumbnailCache());
        src->ensureLane(2)->addClip(dummy, 0.0, 1.0,   // Take lane (Lane 2)
                                    fmt, tm.getThumbnailCache());
        expect(src->getLaneCount() == 3, "source has lane 0 + 2 take lanes");

        // includeTakeLanes=false → テイクレーンを複製しない
        auto* d0 = tm.duplicateTrack(0, false);
        expect(d0 != nullptr, "duplicate (exclude takes) returns a track");
        expect(d0->getLaneCount() == 1, "only Lane 0 is present (take lanes dropped)");
        expect(d0->getLane(0) != nullptr && (int) d0->getLane(0)->clips.size() == 1,
               "Lane 0 clip copied");

        // 既定 (includeTakeLanes=true) は従来どおりテイクレーンも複製
        auto* d1 = tm.duplicateTrack(0, true);
        expect(d1 != nullptr && d1->getLaneCount() == 3, "default keeps take lanes");
    }

    // ── MIDI トラックの深いコピー: synth/移調・MIDI クリップ ch・シーケンス ──
    void testDuplicateMidiDeepCopy()
    {
        beginTest("duplicateTrack deep-copies MIDI track (synth/transpose/channel/sequence)");
        juce::AudioFormatManager fmt; fmt.registerBasicFormats();
        TrackManager tm(fmt);
        auto* src = tm.addTrack("Harmony");
        src->setMidiTrack(true);
        src->setSynthWaveform(2); src->setOctaveShift(1); src->setSemitoneTranspose(3);
        auto* mc = src->addMidiClip(0.0, 2.0);
        mc->setChannel(9);  // ch10 ドラム
        mc->getSequence().addEvent(juce::MidiMessage::noteOn(10, 60, (juce::uint8) 100), 0.0);
        mc->getSequence().addEvent(juce::MidiMessage::noteOff(10, 60), 0.5);
        mc->getSequence().updateMatchedPairs();
        const int srcEvents = mc->getSequence().getNumEvents();

        auto* dst = tm.duplicateTrack(0);
        expect(dst != nullptr && dst->isMidiTrack(), "duplicate is a MIDI track");
        expect(dst->getSynthWaveform() == 2,      "synthWaveform copied");
        expect(dst->getOctaveShift() == 1,        "octaveShift copied");
        expect(dst->getSemitoneTranspose() == 3,  "semitoneTranspose copied");
        expect(dst->getMidiClipCount() == 1, "one MIDI clip copied");
        if (dst->getMidiClipCount() == 1)
        {
            auto* dmc = dst->getMidiClip(0);
            expect(dmc->getChannel() == 9, "MIDI channel (ch10 drum) preserved");
            expect(dmc->getSequence().getNumEvents() == srcEvents, "sequence events copied");
        }
    }

    // ── 範囲外 / Click は nullptr、連続複製で名前が一意化される ──
    void testDuplicateGuardsAndUniqueName()
    {
        beginTest("duplicateTrack guards (click/out-of-range) and unique naming");
        juce::AudioFormatManager fmt; fmt.registerBasicFormats();
        TrackManager tm(fmt);
        expect(tm.duplicateTrack(-1) == nullptr, "negative index -> nullptr");
        expect(tm.duplicateTrack(99) == nullptr, "out-of-range index -> nullptr");

        auto* click = tm.addClickTrack();
        juce::ignoreUnused(click);
        expect(tm.duplicateTrack(0) == nullptr, "click track cannot be duplicated");

        auto* v = tm.addTrack("Vocals");          // index 1
        juce::ignoreUnused(v);
        auto* d1 = tm.duplicateTrack(1);
        auto* d2 = tm.duplicateTrack(1);
        expect(d1 != nullptr && d2 != nullptr, "two duplicates created");
        expect(d1->getName() == juce::String("Vocals (1)"), "first copy name");
        expect(d2->getName() == juce::String("Vocals (2)"), "second copy gets next number");
        expect(d2->getName() != d1->getName(), "second copy gets a distinct (numbered) name");
    }

    // ── audioFolderSignature: 拡張子フィルタ・内容変化で変化・非ディレクトリは空・決定論的 ──
    void testAudioFolderSignature()
    {
        beginTest("audioFolderSignature: extension filter / content-change / non-dir / deterministic");
        auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                       .getChildFile("UtawaveTMSigTest");
        dir.deleteRecursively(); dir.createDirectory();

        expect(TrackManager::audioFolderSignature(dir.getChildFile("nope")) == juce::String(),
               "non-existent / non-directory -> empty signature");

        auto a = dir.getChildFile("a.wav");
        auto b = dir.getChildFile("b.wav");
        a.replaceWithText("AAAA");
        b.replaceWithText("BBBBBB");
        const auto sig1 = TrackManager::audioFolderSignature(dir);
        expect(sig1.isNotEmpty(), "signature of a folder with audio is non-empty");
        expect(TrackManager::audioFolderSignature(dir) == sig1, "signature is deterministic");

        // 非オーディオ拡張子は無視される (署名は変わらない)
        dir.getChildFile("notes.txt").replaceWithText("ignore me");
        expect(TrackManager::audioFolderSignature(dir) == sig1,
               "non-audio file does not affect the signature");

        // 内容 (サイズ) が変われば署名が変わる
        a.replaceWithText("AAAAAAAAAAAAAAAA");
        expect(TrackManager::audioFolderSignature(dir) != sig1,
               "changing an audio file's content changes the signature");

        // .wav 以外のオーディオ拡張子 (.aiff/.mp3/.aif) も算入される (.txt の除外と対照)
        const auto sigBeforeExt = TrackManager::audioFolderSignature(dir);
        dir.getChildFile("c.aiff").replaceWithText("CCCC");
        const auto sigAiff = TrackManager::audioFolderSignature(dir);
        expect(sigAiff != sigBeforeExt, ".aiff file IS counted in the signature");
        dir.getChildFile("d.mp3").replaceWithText("DDDD");
        const auto sigMp3 = TrackManager::audioFolderSignature(dir);
        expect(sigMp3 != sigAiff, ".mp3 file IS counted in the signature");
        dir.getChildFile("e.aif").replaceWithText("EEEE");
        expect(TrackManager::audioFolderSignature(dir) != sigMp3,
               ".aif file IS counted in the signature");

        dir.deleteRecursively();
    }

    // ── extractTrack / insertTrack / indexOf (トラック追加 Undo の土台) ──
    void testExtractInsertIndexOf()
    {
        beginTest("extractTrack / insertTrack / indexOf round-trip keeps the instance");
        juce::AudioFormatManager fmt; fmt.registerBasicFormats();
        TrackManager tm(fmt);
        auto* a = tm.addTrack("A", false);
        auto* b = tm.addTrack("B", false);
        auto* c = tm.addTrack("C", false);

        expect(tm.indexOf(b) == 1, "indexOf finds the middle track");
        expect(tm.indexOf(nullptr) == -1, "indexOf of null is -1");

        // 真ん中を取り外す → 残りが詰まり、インスタンスは生きている
        auto held = tm.extractTrack(1);
        expect(held.get() == b, "extract returns the same instance");
        expect(tm.getTrackCount() == 2 && tm.getTrack(0) == a && tm.getTrack(1) == c,
               "remaining tracks shift up");
        expect(tm.indexOf(b) == -1, "extracted track is no longer found");
        expect(held->getName() == "B", "extracted instance is alive and intact");

        // 同じ位置へ戻す → 同一インスタンス・元の順序
        tm.insertTrack(1, std::move(held));
        expect(tm.getTrackCount() == 3 && tm.getTrack(1) == b, "reinsert restores order");

        // ガード: 範囲外 extract は null / 範囲外 index の insert はクランプ / null insert は no-op
        expect(tm.extractTrack(99) == nullptr && tm.extractTrack(-1) == nullptr,
               "out-of-range extract returns null");
        auto held2 = tm.extractTrack(2);   // c
        tm.insertTrack(99, std::move(held2));
        expect(tm.getTrack(2) == c, "insert index clamps to the end");
        tm.insertTrack(0, nullptr);
        expect(tm.getTrackCount() == 3, "null insert is a no-op");
    }

    // ── TrackAddAction: 追加の Undo/Redo (同一インスタンス復帰・willRemove 発火) ──
    void testTrackAddAction()
    {
        beginTest("TrackAddAction: undo removes / redo restores the same instance");
        juce::AudioFormatManager fmt; fmt.registerBasicFormats();
        TrackManager tm(fmt);
        auto* a = tm.addTrack("A", false);
        auto* added = tm.addTrack("Added", true);
        juce::ignoreUnused(a);

        int willRemoveCount = 0, changeCount = 0;
        Track* willRemoveArg = nullptr;
        EditActions::TrackAddAction action(tm, added,
            [&](Track* t) { ++willRemoveCount; willRemoveArg = t; },
            [&] { ++changeCount; });

        // 最初の perform は no-op (追加自体は呼び出し側が実施済み)
        expect(action.perform(), "first perform succeeds");
        expect(tm.getTrackCount() == 2 && changeCount == 0,
               "first perform does not change anything");

        // undo → リストから外れるがインスタンスは延命 (willRemove が先に 1 回)
        expect(action.undo(), "undo succeeds");
        expect(tm.getTrackCount() == 1 && tm.indexOf(added) == -1, "track removed by undo");
        expect(willRemoveCount == 1 && willRemoveArg == added,
               "willRemove fired once with the track");
        expect(changeCount == 1, "onChange fired on undo");

        // redo → 同一インスタンスが同じ位置 (index 1) へ復帰
        expect(action.perform(), "redo succeeds");
        expect(tm.getTrackCount() == 2 && tm.getTrack(1) == added,
               "redo restores the same instance at the same index");
        expect(added->getName() == "Added" && added->isStereo(),
               "instance state survives undo/redo");
        expect(changeCount == 2, "onChange fired on redo");

        // undo → (Undo 非対応の削除を模して) トラックを消した後の再 undo は安全に false
        expect(action.undo(), "second undo succeeds");
        expect(action.undo() == false, "undo with the track already gone is a safe no-op");

        // redo して戻し、もう一度 undo してから二重 redo: 2 回目は stored が無いので false
        expect(action.perform(), "redo after failed undo still works");
        expect(action.undo(), "third undo succeeds");
        expect(action.perform(), "third redo succeeds");
        expect(action.perform() == false, "redo without a preceding undo is rejected");
    }
    // ── 波形を空き領域へドロップ → 新規トラックへ移動 の合成 Undo/Redo ──
    // MainComponent::moveClipToNewTrack と同じ作法 (TrackAdd + ClipDelete + ClipAdd を
    // 1 トランザクション) を本物の juce::UndoManager で往復検証する。クリップを掴んで
    // 全トラックより下の空き領域へ落とした時の「元に戻す」が正しく動くことを担保する。
    void testMoveClipToNewTrackUndo()
    {
        beginTest("Move-to-new-track composition: undo restores source clip to its ORIGINAL (pre-drag) position");
        juce::AudioFormatManager fmt; fmt.registerBasicFormats();
        TrackManager tm(fmt);

        // ソーストラック: 先に「既にある波形」(小クリップ) を 0..2s に置き、掴むクリップは
        // 元位置 5..9s に置く (両者は重ならない = スクショの初期状態)。
        auto* src      = tm.addTrack("Vocals", false);
        auto* srcLane  = src->getLane(0);
        auto* existing = srcLane->addClip(juce::File("/tmp/existing.wav"), 0.0, 2.0,
                                          fmt, src->getThumbnailCache());
        auto* srcClip  = srcLane->addClip(juce::File("/tmp/move.wav"), 5.0, 4.0,
                                          fmt, src->getThumbnailCache());
        srcClip->setGain(1.3f);
        srcClip->setName("Vocals");
        const double origStart = srcClip->getStartPosition();   // 5.0 (掴む前の位置)

        // ドラッグで掴むクリップを左 (0.5s) かつ下 (空き領域) へ動かした状態を模す。
        // この時点で srcClip は「ドラッグ後の位置」(既存クリップ 0..2 と重なる) になっている。
        srcClip->setStartPosition(0.5);

        // 新トラックへ置くクリップはドロップ位置 (0.5) のまま取得
        EditActions::ClipParams p;
        p.file = srcClip->getFile();   p.startPos   = srcClip->getStartPosition();   // 0.5
        p.duration = srcClip->getDuration(); p.fileOffset = srcClip->getFileOffset();
        p.gain = srcClip->getGain();   p.name = srcClip->getName();
        p.colour = srcClip->getColour();

        // 修正の肝: 元クリップはドラッグ分を巻き戻してから削除させる
        // (これが無いと undo が 0.5 = 既存クリップに重なる位置へ戻してしまう)。
        srcClip->setStartPosition(origStart);   // 5.0 へ復元

        auto* nt       = tm.addTrack(p.name, false);
        auto* destLane = nt->getLane(0);

        juce::UndoManager um;
        int willRemove = 0;
        auto noChange = [] {};

        // 1 トランザクション: TrackAdd(no-op) → ClipDelete(元から) → ClipAdd(新トラックへ)
        um.beginNewTransaction("Move to New Track");
        um.perform(new EditActions::TrackAddAction(tm, nt,
            [&](Track*) { ++willRemove; }, noChange));
        um.perform(new EditActions::ClipDeleteAction(srcLane, srcClip, noChange));
        um.perform(new EditActions::ClipAddAction(destLane, p, fmt,
                                                  nt->getThumbnailCache(), noChange));

        // 適用後: 2 トラック・ソースは既存クリップのみ・新トラックにドロップ位置 (0.5) のクリップ
        expect(tm.getTrackCount() == 2, "new track added");
        expect((int) srcLane->clips.size() == 1 && srcLane->clips[0].get() == existing,
               "source keeps only the pre-existing clip");
        expect((int) destLane->clips.size() == 1, "clip added to new track");
        {
            auto* moved = destLane->clips[0].get();
            expect(std::abs(moved->getStartPosition() - 0.5) < 1e-9, "new clip lands at drop position");
            expect(std::abs((double) moved->getGain() - 1.3) < 1e-6, "moved clip keeps gain");
        }

        // Undo: ソースへ同一インスタンスが復帰し、位置は ORIGINAL (5.0) = 既存クリップに重ならない
        expect(um.undo(), "undo the whole transaction");
        expect(tm.getTrackCount() == 1 && tm.indexOf(nt) == -1, "new track removed by undo");
        expect((int) srcLane->clips.size() == 2, "both clips back on source");
        expect(srcClip->getStartPosition() > 4.999 && srcClip->getStartPosition() < 5.001,
               "source clip restored to ORIGINAL position (5.0), not the dragged 0.5");
        expect(srcClip->getStartPosition() >= existing->getEndPosition(),
               "restored source clip does NOT overlap the pre-existing clip");
        expect(willRemove == 1, "willRemove fired once on undo (track removed)");

        // Redo: 新トラックインスタンスが元の位置へ復帰し、クリップが再びドロップ位置へ移る
        expect(um.redo(), "redo the whole transaction");
        expect(tm.getTrackCount() == 2 && tm.indexOf(nt) == 1,
               "same new track restored at original index");
        expect((int) srcLane->clips.size() == 1, "source back to only the pre-existing clip");
        expect((int) nt->getLane(0)->clips.size() == 1,
               "clip back on new track (destLane valid across undo/redo)");

        // もう一度 Undo して初期状態へ (べき等・原位置維持)
        expect(um.undo(), "undo again");
        expect(tm.getTrackCount() == 1 && (int) srcLane->clips.size() == 2,
               "back to initial: one track with existing + restored clip");
    }

    // ── TrackDeleteAction: 削除の Undo/Redo (複数・同一インスタンス復帰・元位置再構成) ──
    void testTrackDeleteAction()
    {
        beginTest("TrackDeleteAction: perform deletes (multi), undo restores same instances at original positions");
        juce::AudioFormatManager fmt; fmt.registerBasicFormats();
        TrackManager tm(fmt);
        auto* a = tm.addTrack("A", false);
        auto* b = tm.addTrack("B", true);
        auto* c = tm.addTrack("C", false);
        auto* d = tm.addTrack("D", false);
        juce::ignoreUnused(a, c);

        int willRemoveCount = 0, changeCount = 0;
        std::vector<Track*> removedArgs;
        EditActions::TrackDeleteAction action(tm, { b, d },
            [&](Track* t) { ++willRemoveCount; removedArgs.push_back(t); },
            [&] { ++changeCount; });

        // perform → B,D が消え A,C が残る (降順削除で index ずれなし)
        expect(action.perform(), "perform deletes the tracks");
        expect(tm.getTrackCount() == 2, "two tracks removed");
        expect(tm.indexOf(b) == -1 && tm.indexOf(d) == -1, "B and D are gone");
        expect(tm.getTrack(0) == a && tm.getTrack(1) == c, "A and C remain in order");
        expect(willRemoveCount == 2, "willRemove fired once per deleted track");
        expect(removedArgs.size() == 2
               && std::find(removedArgs.begin(), removedArgs.end(), b) != removedArgs.end()
               && std::find(removedArgs.begin(), removedArgs.end(), d) != removedArgs.end(),
               "willRemove received both deleted tracks");
        expect(changeCount == 1, "onChange fired once for the whole delete");

        // undo → 同一インスタンスが元の位置 (B=1, D=3) へ復帰し並びを再構成
        expect(action.undo(), "undo restores the tracks");
        expect(tm.getTrackCount() == 4, "all four tracks back");
        expect(tm.getTrack(0) == a && tm.getTrack(1) == b
            && tm.getTrack(2) == c && tm.getTrack(3) == d,
            "original order [A,B,C,D] reconstructed with the same instances");
        expect(b->getName() == "B" && b->isStereo(), "restored instance keeps its state");
        expect(changeCount == 2, "onChange fired on undo");

        // redo → 再び B,D を削除
        expect(action.perform(), "redo deletes again");
        expect(tm.getTrackCount() == 2 && tm.indexOf(b) == -1 && tm.indexOf(d) == -1,
               "redo removes B and D again");
        expect(changeCount == 3, "onChange fired on redo");
        expect(action.undo(), "final undo restores for cleanup");
        expect(tm.getTrackCount() == 4, "restored to four tracks");

        // ガード: このマネージャに無いトラックを削除しようとしても安全に no-op
        TrackManager other(fmt);
        auto* alien = other.addTrack("Alien", false);
        EditActions::TrackDeleteAction foreign(tm, { alien }, [](Track*){}, []{});
        expect(foreign.perform() == false, "perform on a track not in this manager is a safe no-op");
        expect(other.getTrackCount() == 1 && tm.getTrackCount() == 4,
               "foreign delete touches neither manager");
    }

    // ── reorderTo: 任意順への並べ替え / 検証ガード (並べ替え Undo の土台) ──
    void testReorderTo()
    {
        beginTest("reorderTo applies a permutation; rejects non-permutations");
        juce::AudioFormatManager fmt; fmt.registerBasicFormats();
        TrackManager tm(fmt);
        auto* a = tm.addTrack("A", false);
        auto* b = tm.addTrack("B", false);
        auto* c = tm.addTrack("C", false);
        auto* d = tm.addTrack("D", false);

        int changeCount = 0;
        tm.onChanged = [&] { ++changeCount; };

        // 並べ替え: [a,b,c,d] → [d,b,a,c]
        expect(tm.reorderTo({ d, b, a, c }), "reorderTo accepts a valid permutation");
        expect(tm.getTrack(0) == d && tm.getTrack(1) == b
            && tm.getTrack(2) == a && tm.getTrack(3) == c, "tracks are in the requested order");
        expect(changeCount == 1, "reorderTo fires onChanged once");

        // 恒等 (現在順そのまま) も成功する
        expect(tm.reorderTo({ d, b, a, c }), "identity reorder succeeds");
        expect(tm.getTrack(0) == d, "identity keeps order");

        // ガード: 数違い → 何もしない (順序不変)
        expect(tm.reorderTo({ d, b, a }) == false, "size mismatch is rejected");
        expect(tm.getTrack(0) == d && tm.getTrackCount() == 4, "rejected reorder leaves tracks intact");

        // ガード: 重複を含む (= permutation でない) → 拒否し順序不変
        expect(tm.reorderTo({ d, d, a, c }) == false, "duplicate entry is rejected");
        expect(tm.getTrack(1) == b, "rejected duplicate leaves order intact");

        // ガード: 集合外のポインタを含む → 拒否 (削除済みトラック参照を模す)
        TrackManager other(fmt);
        auto* alien = other.addTrack("X", false);
        expect(tm.reorderTo({ d, b, a, alien }) == false, "foreign pointer is rejected");
        expect(tm.getTrack(3) == c, "rejected foreign reorder leaves order intact");
    }

    // ── TrackReorderAction: 並べ替えの Undo/Redo 往復 ──
    void testTrackReorderAction()
    {
        beginTest("TrackReorderAction: undo restores before-order, redo restores after-order");
        juce::AudioFormatManager fmt; fmt.registerBasicFormats();
        TrackManager tm(fmt);
        auto* a = tm.addTrack("A", false);
        auto* b = tm.addTrack("B", false);
        auto* c = tm.addTrack("C", false);

        const std::vector<Track*> before { a, b, c };
        // 並べ替え自体は呼び出し側が実施済みという前提 (b を末尾へ): [a,b,c] → [a,c,b]
        tm.reorderTo({ a, c, b });
        const std::vector<Track*> after { a, c, b };

        int changeCount = 0;
        EditActions::TrackReorderAction action(tm, before, after, [&] { ++changeCount; });

        // 最初の perform は no-op (並べ替えは実施済み)
        expect(action.perform(), "first perform succeeds (no-op)");
        expect(tm.getTrack(1) == c && tm.getTrack(2) == b && changeCount == 0,
               "first perform changes nothing");

        // undo → before 順 [a,b,c]
        expect(action.undo(), "undo succeeds");
        expect(tm.getTrack(0) == a && tm.getTrack(1) == b && tm.getTrack(2) == c,
               "undo restores the before-order");
        expect(changeCount == 1, "onChange fired on undo");

        // redo → after 順 [a,c,b]
        expect(action.perform(), "redo succeeds");
        expect(tm.getTrack(1) == c && tm.getTrack(2) == b,
               "redo restores the after-order");
        expect(changeCount == 2, "onChange fired on redo");

        // トラックが消えていれば (Undo 非対応削除を模す) undo は安全に false
        tm.removeTrack(tm.indexOf(c));
        expect(action.undo() == false, "undo with a missing track is a safe no-op");
    }
};

static TrackManagerTests trackManagerTests;
}
