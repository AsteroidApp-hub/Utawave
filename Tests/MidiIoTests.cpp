// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

// Utawave — MIDI 入出力 (MidiImporter / MidiExporter) のユニットテスト
//
// オーディオデバイス不要・決定論的に SMF の読み書きを検証する。中心は「往復 (round-trip)」:
//   1. TrackManager / Track / MidiClip で MIDI シーンを構築
//   2. MidiExporter::save で一時 .mid に書き出し
//   3. MidiImporter::load で読み戻し
//   4. ノートのピッチ / 位置 / 尺 / ベロシティ / チャンネル、テンポ / 拍子が保たれるか検証
// これに加え、エクスポータとは独立に「手書き SMF を import」して importer 単体も検証する
// (両者に相殺するバグがあっても往復だけでは見逃すため)。
//
// 既存の UtawaveTests (ExportEngineTests.cpp) が main() を持ち UnitTestRunner で全テストを
// 走らせる。juce::UnitTest は構築時に自身を登録するので、ここでは静的インスタンスを置くだけ。
//
// 注意 (CLAUDE.md のテスト定石): expect の文字列は ASCII で書く
// (juce::String(const char*) に非 ASCII を渡すと juce_String.cpp が assert する)。

#include <JuceHeader.h>
#include <vector>

#include "../Source/Tracks/TrackManager.h"
#include "../Source/Tracks/Track.h"
#include "../Source/Tracks/MidiClip.h"
#include "../Source/AppSettings.h"
#include "../Source/MIDI/MidiExporter.h"
#include "../Source/MIDI/MidiImporter.h"

namespace
{
constexpr int kPPQ = 960;   // MidiExporter と同じ書き出し解像度 (期待値計算用)

// クリップへノート (NoteOn + NoteOff のペア) を追加する。時刻はクリップ先頭からの秒。
void addNote(MidiClip& clip, int pitch, double onSec, double offSec, int vel = 100)
{
    auto& seq = clip.getSequence();
    const int ch = juce::jlimit(1, 16, clip.getChannel() + 1);
    seq.addEvent(juce::MidiMessage::noteOn (ch, pitch, (juce::uint8) vel), onSec);
    seq.addEvent(juce::MidiMessage::noteOff(ch, pitch),                    offSec);
    seq.updateMatchedPairs();
}

struct NoteInfo { int pitch; double start; double dur; int vel; int channel; };

// ImportedTrack から (絶対秒の) ノート列を抽出する。
std::vector<NoteInfo> extractNotes(const MidiImporter::ImportedTrack& t)
{
    std::vector<NoteInfo> out;
    for (int i = 0; i < t.sequence.getNumEvents(); ++i)
    {
        auto* e = t.sequence.getEventPointer(i);
        if (!e->message.isNoteOn()) continue;
        NoteInfo n;
        n.pitch   = e->message.getNoteNumber();
        n.start   = e->message.getTimeStamp();
        n.vel     = e->message.getVelocity();
        n.channel = e->message.getChannel() - 1;
        n.dur     = (e->noteOffObject != nullptr)
                        ? (e->noteOffObject->message.getTimeStamp() - n.start) : 0.0;
        out.push_back(n);
    }
    return out;
}

const MidiImporter::ImportedTrack* findTrack(const MidiImporter::ImportResult& r,
                                             const juce::String& name)
{
    for (auto& t : r.tracks)
        if (t.name == name) return &t;
    return nullptr;
}
} // namespace

//==============================================================================
class MidiIoTests : public juce::UnitTest
{
public:
    MidiIoTests() : juce::UnitTest("MIDI Import/Export") {}

    juce::File outDir;

    juce::File outFile(const juce::String& name)
    {
        return outDir.getChildFile(name);
    }

    void runTest() override
    {
        outDir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                     .getChildFile("UtawaveMidiIoTests");
        outDir.deleteRecursively();
        outDir.createDirectory();

        testRoundTripBasicNotes();
        testClipStartOffset();
        testMultipleClipsMergeInTrack();
        testTempoPreserved();
        testMultipleTracksAndNames();
        testChannelDrumPreserved();
        testTrackTransposeApplied();
        testEmptyMidiTrackAndAudioTrackSkipped();
        testNoNotesReturnsError();
        testMeterRoundTrip();
        testMeterChangeRoundTrip();
        testMeterChangeTempoIndependent();
        testApplyTempoMeterToSettings();
        testTempoChangeRoundTrip();
        testOverwriteExistingFile();
        testImporterDirectSmf();
        testImporterTrailingChunks();
        testImporterChannelSplit();
        testImporterMissingFile();
        testHasMidiTrackPredicate();
    }

    // ── 1. 基本ノートの往復 (ピッチ / 位置 / 尺 / ベロシティ) ──
    void testRoundTripBasicNotes()
    {
        beginTest("round-trip: basic notes preserve pitch/time/dur/velocity");

        AppSettings s;                       // 120 BPM / 4/4 (既定)
        juce::AudioFormatManager fmt;
        fmt.registerBasicFormats();
        TrackManager tm(fmt);
        auto* track = tm.addTrack("Lead");
        track->setMidiTrack(true);
        auto* clip = track->addMidiClip(0.0, 2.0);
        // 120BPM では 0.25s=0.5拍(480tick), 0.5s=1拍(960tick) など整数ティックに乗る
        addNote(*clip, 60, 0.0, 0.5, 100);
        addNote(*clip, 64, 0.5, 1.0, 80);
        addNote(*clip, 67, 1.0, 1.5, 64);

        auto f = outFile("basic.mid");
        auto res = MidiExporter::save(f, tm, s);
        expect(res.ok, "export ok");
        expectEquals(res.noteCount, 3, "exported note count");
        expect(f.existsAsFile(), "file written");

        auto imp = MidiImporter::load(f);
        expect(imp.ok, "import ok");
        expectEquals((int) imp.tracks.size(), 1, "one note track (conductor skipped)");
        if (imp.tracks.empty()) return;

        auto notes = extractNotes(imp.tracks[0]);
        expectEquals((int) notes.size(), 3, "imported note count");
        if (notes.size() != 3) return;

        const int    expPitch[] = { 60, 64, 67 };
        const double expStart[] = { 0.0, 0.5, 1.0 };
        const int    expVel[]   = { 100, 80, 64 };
        for (int i = 0; i < 3; ++i)
        {
            expectEquals(notes[(size_t)i].pitch, expPitch[i], "pitch " + juce::String(i));
            expectWithinAbsoluteError(notes[(size_t)i].start, expStart[i], 0.003,
                                      "start " + juce::String(i));
            expectWithinAbsoluteError(notes[(size_t)i].dur, 0.5, 0.003,
                                      "duration " + juce::String(i));
            expectEquals(notes[(size_t)i].vel, expVel[i], "velocity " + juce::String(i));
        }
    }

    // ── 2. クリップ開始位置のオフセットが絶対時刻に反映される ──
    void testClipStartOffset()
    {
        beginTest("round-trip: clip start position offsets absolute note time");

        AppSettings s;
        juce::AudioFormatManager fmt;
        fmt.registerBasicFormats();
        TrackManager tm(fmt);
        auto* track = tm.addTrack("Lead");
        track->setMidiTrack(true);
        auto* clip = track->addMidiClip(1.0, 2.0);   // 1.0s から始まるクリップ
        addNote(*clip, 72, 0.0, 0.5);                 // クリップ先頭 → 絶対 1.0s

        auto f = outFile("offset.mid");
        expect(MidiExporter::save(f, tm, s).ok, "export ok");

        auto imp = MidiImporter::load(f);
        expect(imp.ok && imp.tracks.size() == 1, "import ok with one track");
        if (imp.tracks.size() != 1) return;
        auto notes = extractNotes(imp.tracks[0]);
        expectEquals((int) notes.size(), 1, "one note");
        if (notes.empty()) return;
        expectWithinAbsoluteError(notes[0].start, 1.0, 0.003, "absolute start = clipStart + 0");
    }

    // ── 3. 1 トラック内の複数クリップが 1 つの書き出しトラックへ統合される ──
    void testMultipleClipsMergeInTrack()
    {
        beginTest("round-trip: multiple clips in one track merge into one exported track");

        AppSettings s;
        juce::AudioFormatManager fmt;
        fmt.registerBasicFormats();
        TrackManager tm(fmt);
        auto* track = tm.addTrack("Lead");
        track->setMidiTrack(true);
        auto* c1 = track->addMidiClip(0.0, 1.0);
        addNote(*c1, 60, 0.0, 0.5);
        auto* c2 = track->addMidiClip(2.0, 1.0);      // 別クリップ (絶対 2.0s 始まり)
        addNote(*c2, 72, 0.0, 0.5);                    // → 絶対 2.0s

        auto f = outFile("merge.mid");
        auto res = MidiExporter::save(f, tm, s);
        expect(res.ok, "export ok");
        expectEquals(res.trackCount, 1, "single exported MIDI track");
        expectEquals(res.noteCount, 2, "two notes total");

        auto imp = MidiImporter::load(f);
        expect(imp.ok && imp.tracks.size() == 1, "one imported track");
        if (imp.tracks.size() != 1) return;
        auto notes = extractNotes(imp.tracks[0]);
        expectEquals((int) notes.size(), 2, "two notes imported");
        if (notes.size() != 2) return;
        expectWithinAbsoluteError(notes[0].start, 0.0, 0.003, "first note at 0");
        expectWithinAbsoluteError(notes[1].start, 2.0, 0.003, "second note at 2.0 (clip2 offset)");
    }

    // ── 4. テンポが保たれ、非 120BPM でもノート位置が正しい ──
    void testTempoPreserved()
    {
        beginTest("round-trip: tempo preserved; note time correct at 90 BPM");

        AppSettings s;
        s.initialBpm = 90.0;
        juce::AudioFormatManager fmt;
        fmt.registerBasicFormats();
        TrackManager tm(fmt);
        auto* track = tm.addTrack("Lead");
        track->setMidiTrack(true);
        auto* clip = track->addMidiClip(0.0, 2.0);
        addNote(*clip, 60, 0.5, 1.0);   // 90BPM では 0.5s=0.75拍=720tick (整数)

        auto f = outFile("tempo90.mid");
        expect(MidiExporter::save(f, tm, s).ok, "export ok");

        auto imp = MidiImporter::load(f);
        expect(imp.ok, "import ok");
        expectWithinAbsoluteError(imp.initialBpm, 90.0, 0.05, "initial BPM ~ 90");
        if (imp.tracks.size() != 1) { expect(false, "one track expected"); return; }
        auto notes = extractNotes(imp.tracks[0]);
        expectEquals((int) notes.size(), 1, "one note");
        if (notes.empty()) return;
        expectWithinAbsoluteError(notes[0].start, 0.5, 0.003, "note start at 0.5s");
    }

    // ── 5. 複数 MIDI トラック + トラック名の往復 ──
    void testMultipleTracksAndNames()
    {
        beginTest("round-trip: multiple tracks keep names and routing");

        AppSettings s;
        juce::AudioFormatManager fmt;
        fmt.registerBasicFormats();
        TrackManager tm(fmt);
        auto* t1 = tm.addTrack("Melody");
        t1->setMidiTrack(true);
        addNote(*t1->addMidiClip(0.0, 1.0), 60, 0.0, 0.5);

        auto* t2 = tm.addTrack("Harmony");
        t2->setMidiTrack(true);
        addNote(*t2->addMidiClip(0.0, 1.0), 64, 0.0, 0.5);

        auto f = outFile("multi.mid");
        auto res = MidiExporter::save(f, tm, s);
        expect(res.ok, "export ok");
        expectEquals(res.trackCount, 2, "two exported tracks");

        auto imp = MidiImporter::load(f);
        expect(imp.ok, "import ok");
        expectEquals((int) imp.tracks.size(), 2, "two imported tracks");

        auto* m = findTrack(imp, "Melody");
        auto* h = findTrack(imp, "Harmony");
        expect(m != nullptr, "Melody track present by name");
        expect(h != nullptr, "Harmony track present by name");
        if (m) { auto n = extractNotes(*m); expect(n.size() == 1 && n[0].pitch == 60, "Melody note 60"); }
        if (h) { auto n = extractNotes(*h); expect(n.size() == 1 && n[0].pitch == 64, "Harmony note 64"); }
    }

    // ── 6. ドラムチャンネル (ch10) が保持される ──
    void testChannelDrumPreserved()
    {
        beginTest("round-trip: drum channel (ch10) preserved");

        AppSettings s;
        juce::AudioFormatManager fmt;
        fmt.registerBasicFormats();
        TrackManager tm(fmt);
        auto* track = tm.addTrack("Drums");
        track->setMidiTrack(true);
        auto* clip = track->addMidiClip(0.0, 1.0);
        clip->setChannel(9);              // 0-based 9 = MIDI ch10
        addNote(*clip, 38, 0.0, 0.25);    // スネア相当

        auto f = outFile("drum.mid");
        expect(MidiExporter::save(f, tm, s).ok, "export ok");

        auto imp = MidiImporter::load(f);
        expect(imp.ok && imp.tracks.size() == 1, "one imported track");
        if (imp.tracks.size() != 1) return;
        expectEquals(imp.tracks[0].primaryChannel, 9, "primary channel 9 (ch10)");
        expect(imp.tracks[0].isDrum, "flagged as drum");
        auto notes = extractNotes(imp.tracks[0]);
        expect(notes.size() == 1 && notes[0].channel == 9, "note on channel 9");
    }

    // ── 7. トラックの移調 (半音) が書き出しに反映される ──
    void testTrackTransposeApplied()
    {
        beginTest("export applies track transpose to note pitch");

        AppSettings s;
        juce::AudioFormatManager fmt;
        fmt.registerBasicFormats();
        TrackManager tm(fmt);
        auto* track = tm.addTrack("Lead");
        track->setMidiTrack(true);
        track->setSemitoneTranspose(2);   // +2 半音
        auto* clip = track->addMidiClip(0.0, 1.0);
        addNote(*clip, 60, 0.0, 0.5);      // 書き出し後は 62 のはず

        auto f = outFile("transpose.mid");
        expect(MidiExporter::save(f, tm, s).ok, "export ok");

        auto imp = MidiImporter::load(f);
        expect(imp.ok && imp.tracks.size() == 1, "one imported track");
        if (imp.tracks.size() != 1) return;
        auto notes = extractNotes(imp.tracks[0]);
        expect(notes.size() == 1 && notes[0].pitch == 62, "pitch transposed 60 -> 62");
    }

    // ── 8. 空の MIDI トラック / オーディオトラックは書き出されない ──
    void testEmptyMidiTrackAndAudioTrackSkipped()
    {
        beginTest("export skips empty MIDI tracks and non-MIDI tracks");

        AppSettings s;
        juce::AudioFormatManager fmt;
        fmt.registerBasicFormats();
        TrackManager tm(fmt);

        auto* withNotes = tm.addTrack("HasNotes");
        withNotes->setMidiTrack(true);
        addNote(*withNotes->addMidiClip(0.0, 1.0), 60, 0.0, 0.5);

        auto* emptyMidi = tm.addTrack("EmptyMidi");
        emptyMidi->setMidiTrack(true);
        emptyMidi->addMidiClip(0.0, 1.0);   // クリップはあるがノート無し

        tm.addTrack("AudioTrack");           // MIDI ではない (isMidiTrack=false)

        auto f = outFile("skip.mid");
        auto res = MidiExporter::save(f, tm, s);
        expect(res.ok, "export ok");
        expectEquals(res.trackCount, 1, "only the track with notes is exported");

        auto imp = MidiImporter::load(f);
        expect(imp.ok, "import ok");
        expectEquals((int) imp.tracks.size(), 1, "one imported track");
        if (!imp.tracks.empty())
            expect(imp.tracks[0].name == "HasNotes", "exported track is HasNotes");
    }

    // ── 9. ノートが 1 つも無ければ ok=false でファイルを作らない ──
    void testNoNotesReturnsError()
    {
        beginTest("export with no notes returns failure and writes no file");

        AppSettings s;
        juce::AudioFormatManager fmt;
        fmt.registerBasicFormats();
        TrackManager tm(fmt);
        auto* track = tm.addTrack("EmptyMidi");
        track->setMidiTrack(true);
        track->addMidiClip(0.0, 1.0);   // ノート無し

        auto f = outFile("empty.mid");
        f.deleteFile();
        auto res = MidiExporter::save(f, tm, s);
        expect(!res.ok, "export reports failure");
        expectEquals(res.noteCount, 0, "zero notes");
        expect(res.error.isNotEmpty(), "error message present");
        expect(!f.existsAsFile(), "no file created");
    }

    // ── 10. 拍子 (3/4) の往復 ──
    void testMeterRoundTrip()
    {
        beginTest("round-trip: time signature 3/4 preserved");

        AppSettings s;
        s.meterNumerator   = 3;
        s.meterDenominator = 4;
        juce::AudioFormatManager fmt;
        fmt.registerBasicFormats();
        TrackManager tm(fmt);
        auto* track = tm.addTrack("Lead");
        track->setMidiTrack(true);
        addNote(*track->addMidiClip(0.0, 2.0), 60, 0.0, 0.5);

        auto f = outFile("meter34.mid");
        expect(MidiExporter::save(f, tm, s).ok, "export ok");

        auto imp = MidiImporter::load(f);
        expect(imp.ok, "import ok");
        expectEquals(imp.meterNumerator, 3, "numerator 3");
        expectEquals(imp.meterDenominator, 4, "denominator 4");
    }

    // ── 10b. 拍子の途中変化 (meterChanges) の往復 ──
    void testMeterChangeRoundTrip()
    {
        beginTest("round-trip: mid-song meter change preserved");

        AppSettings s;
        s.meterNumerator   = 4;
        s.meterDenominator = 4;
        s.meterChanges     = { { 4, 3, 4 } };   // bar 5 (index 4) から 3/4 へ
        juce::AudioFormatManager fmt;
        fmt.registerBasicFormats();
        TrackManager tm(fmt);
        auto* track = tm.addTrack("Lead");
        track->setMidiTrack(true);
        addNote(*track->addMidiClip(0.0, 2.0), 60, 0.0, 0.5);

        auto f = outFile("meterchg.mid");
        expect(MidiExporter::save(f, tm, s).ok, "export ok");

        auto imp = MidiImporter::load(f);
        expect(imp.ok, "import ok");
        expectEquals(imp.meterNumerator, 4, "initial numerator 4");
        expectEquals(imp.meterDenominator, 4, "initial denominator 4");
        expect(imp.meterChanges.size() == 1, "one meter change imported");
        if (! imp.meterChanges.empty())
        {
            expectEquals(imp.meterChanges[0].first, 4, "change at bar index 4 (bar 5)");
            expectEquals(imp.meterChanges[0].second.first, 3, "changed numerator 3");
            expectEquals(imp.meterChanges[0].second.second, 4, "changed denominator 4");
        }
    }

    // ── 10c. テンポ変化があっても拍子変化の小節番号がずれない (tick ベース算出の回帰テスト) ──
    void testMeterChangeTempoIndependent()
    {
        beginTest("import: meter-change bar index is tempo-independent");

        // bar 5 (index 4) で 3/4 へ。さらに 1.0s でテンポを半分に落とす。
        // 旧実装 (秒 × initialBpm で拍換算) では小節番号がずれていた (bar index 8 になる) が、
        // tick ベース算出ならテンポに依らず bar index 4 になる。
        AppSettings s;
        s.initialBpm       = 120.0;
        s.bpmChanges       = { { 1.0, 60.0 } };  // 1.0s でテンポ半減
        s.meterNumerator   = 4;
        s.meterDenominator = 4;
        s.meterChanges     = { { 4, 3, 4 } };
        juce::AudioFormatManager fmt;
        fmt.registerBasicFormats();
        TrackManager tm(fmt);
        auto* track = tm.addTrack("Lead");
        track->setMidiTrack(true);
        addNote(*track->addMidiClip(0.0, 2.0), 60, 0.0, 0.5);

        auto f = outFile("meterchg_tempo.mid");
        expect(MidiExporter::save(f, tm, s).ok, "export ok");

        auto imp = MidiImporter::load(f);
        expect(imp.ok, "import ok");
        expect(imp.meterChanges.size() == 1, "one meter change imported");
        if (! imp.meterChanges.empty())
            expectEquals(imp.meterChanges[0].first, 4,
                         "bar index 4 despite a tempo change before it");
    }

    // ── 10d. インポート結果 → AppSettings への反映 (applyTempoMeterToSettings 純関数) ──
    // MIDI インポート確定時の変換と、その結果がルーラー/グリッドの計算
    // (bpmAtTime / getMeterAtBar) に効くことを SMF からの全経路で固定する。
    // (途中の拍子/テンポ変化が表示に反映されなかったバグの回帰テスト)
    void testApplyTempoMeterToSettings()
    {
        beginTest("applyTempoMeterToSettings: imported map drives ruler math");

        // 元プロジェクト: 120 BPM 4/4、2.0s で 60 BPM、bar index 4 (bar 5) から 3/4
        AppSettings src;
        src.initialBpm       = 120.0;
        src.bpmChanges       = { { 2.0, 60.0 } };
        src.meterNumerator   = 4;
        src.meterDenominator = 4;
        src.meterChanges     = { { 4, 3, 4 } };
        juce::AudioFormatManager fmt;
        fmt.registerBasicFormats();
        TrackManager tm(fmt);
        auto* track = tm.addTrack("Lead");
        track->setMidiTrack(true);
        addNote(*track->addMidiClip(0.0, 2.0), 60, 0.0, 0.5);

        auto f = outFile("apply_tempometer.mid");
        expect(MidiExporter::save(f, tm, src).ok, "export ok");
        auto imp = MidiImporter::load(f);
        expect(imp.ok, "import ok");

        // 既定値 (新規プロジェクト相当) へ反映
        AppSettings dst;
        MidiImporter::applyTempoMeterToSettings(imp, dst);

        expectWithinAbsoluteError(dst.initialBpm, 120.0, 0.05, "initial bpm applied");
        expectEquals(dst.meterNumerator, 4, "initial numerator applied");
        expectEquals(dst.meterDenominator, 4, "initial denominator applied");
        expect(dst.bpmChanges.size() == 1 && dst.meterChanges.size() == 1,
               "mid-song changes applied");

        // ルーラー/グリッドが読む計算に効いていること (= 表示に反映される実体)
        expectWithinAbsoluteError(dst.bpmAtTime(0.5), 120.0, 0.5, "bpm before the change");
        expectWithinAbsoluteError(dst.bpmAtTime(3.0), 60.0, 0.5, "bpm after the change");
        int n = 0, d = 0;
        dst.getMeterAtBar(4, n, d);   // 1 始まり bar 4 = 変化前
        expect(n == 4 && d == 4, "meter before the change bar is 4/4");
        dst.getMeterAtBar(5, n, d);   // bar 5 (barIndex 4) から 3/4
        expect(n == 3 && d == 4, "meter from bar 5 is 3/4");
    }

    // ── 11. テンポチェンジ (bpmChanges) の往復 ──
    void testTempoChangeRoundTrip()
    {
        beginTest("round-trip: mid-song tempo change preserved");

        AppSettings s;
        s.initialBpm = 120.0;
        s.bpmChanges = { { 2.0, 140.0 } };   // 2.0s で 140 BPM へ
        juce::AudioFormatManager fmt;
        fmt.registerBasicFormats();
        TrackManager tm(fmt);
        auto* track = tm.addTrack("Lead");
        track->setMidiTrack(true);
        addNote(*track->addMidiClip(0.0, 4.0), 60, 0.5, 1.0);  // 変化前のノート

        auto f = outFile("tempochg.mid");
        expect(MidiExporter::save(f, tm, s).ok, "export ok");

        auto imp = MidiImporter::load(f);
        expect(imp.ok, "import ok");
        expectWithinAbsoluteError(imp.initialBpm, 120.0, 0.05, "initial 120");
        expect(imp.tempoChanges.size() >= 1, "has a tempo change");
        if (!imp.tempoChanges.empty())
        {
            expectWithinAbsoluteError(imp.tempoChanges[0].first, 2.0, 0.02, "change at ~2.0s");
            expectWithinAbsoluteError(imp.tempoChanges[0].second, 140.0, 0.5, "change to ~140 BPM");
        }
    }

    // ── 12. 既存ファイルへの上書き書き出し ──
    void testOverwriteExistingFile()
    {
        beginTest("export overwrites an existing file cleanly");

        AppSettings s;
        juce::AudioFormatManager fmt;
        fmt.registerBasicFormats();
        TrackManager tm(fmt);
        auto* track = tm.addTrack("Lead");
        track->setMidiTrack(true);
        addNote(*track->addMidiClip(0.0, 1.0), 60, 0.0, 0.5);

        auto f = outFile("overwrite.mid");
        expect(MidiExporter::save(f, tm, s).ok, "first export ok");
        const auto firstSize = f.getSize();
        expect(firstSize > 0, "first file non-empty");

        // 2 回目: 同じパスへ (内容も変えてみる)
        addNote(*track->getMidiClip(0), 64, 0.5, 1.0);
        auto res2 = MidiExporter::save(f, tm, s);
        expect(res2.ok, "second export ok");
        expectEquals(res2.noteCount, 2, "now two notes");

        auto imp = MidiImporter::load(f);
        expect(imp.ok && imp.tracks.size() == 1, "re-import ok");
        if (!imp.tracks.empty())
            expectEquals((int) extractNotes(imp.tracks[0]).size(), 2, "two notes after overwrite");
    }

    // ── 13. importer 単体: 手書き SMF を読む (テンポ / 拍子 / 名前 / チャンネル / マーカー) ──
    void testImporterDirectSmf()
    {
        beginTest("importer: hand-built SMF (tempo/meter/name/channel/marker)");

        juce::MidiFile mf;
        mf.setTicksPerQuarterNote(480);

        juce::MidiMessageSequence cond;
        cond.addEvent(juce::MidiMessage::tempoMetaEvent(600000), 0.0);          // 100 BPM
        cond.addEvent(juce::MidiMessage::timeSignatureMetaEvent(6, 8), 0.0);    // 6/8
        cond.addEvent(juce::MidiMessage::textMetaEvent(6, "Verse"), 480.0);     // marker @ 480tick
        mf.addTrack(cond);

        juce::MidiMessageSequence lead;
        lead.addEvent(juce::MidiMessage::textMetaEvent(3, "Lead"), 0.0);        // track name
        lead.addEvent(juce::MidiMessage::noteOn (3, 67, (juce::uint8) 90), 240.0);
        lead.addEvent(juce::MidiMessage::noteOff(3, 67),                    480.0);
        mf.addTrack(lead);

        auto f = outFile("handbuilt.mid");
        f.deleteFile();
        {
            juce::FileOutputStream os(f);
            expect(os.openedOk(), "open output stream");
            expect(mf.writeTo(os), "writeTo ok");
            os.flush();
        }

        auto imp = MidiImporter::load(f);
        expect(imp.ok, "import ok");
        expectWithinAbsoluteError(imp.initialBpm, 100.0, 0.05, "100 BPM");
        expectEquals(imp.meterNumerator, 6, "numerator 6");
        expectEquals(imp.meterDenominator, 8, "denominator 8");
        expectEquals((int) imp.tracks.size(), 1, "one note track");
        expect(imp.markers.size() >= 1, "marker present");
        if (!imp.markers.empty())
        {
            expect(imp.markers[0].second == "Verse", "marker text 'Verse'");
            // 480tick @ 480ppq = 1 quarter note; 100BPM -> 0.6s
            expectWithinAbsoluteError(imp.markers[0].first, 0.6, 0.01, "marker at 0.6s");
        }
        if (imp.tracks.empty()) return;
        expect(imp.tracks[0].name == "Lead", "track name 'Lead'");
        expectEquals(imp.tracks[0].primaryChannel, 2, "primary channel 2 (ch3)");
        auto notes = extractNotes(imp.tracks[0]);
        expectEquals((int) notes.size(), 1, "one note");
        if (notes.empty()) return;
        expectEquals(notes[0].pitch, 67, "pitch 67");
        expectEquals(notes[0].vel, 90, "velocity 90");
        // 240tick @ 480ppq = 0.5 quarter; 100BPM -> 0.3s, 尺も 0.3s
        expectWithinAbsoluteError(notes[0].start, 0.3, 0.01, "note start 0.3s");
        expectWithinAbsoluteError(notes[0].dur,   0.3, 0.01, "note dur 0.3s");
    }

    // ── importer: 末尾に独自チャンク (Yamaha XF の XFIH/XFKM 等) を持つ SMF を救済する ──
    // 標準トラックの後に未知チャンクが付くと JUCE の readFrom は余剰バイトを嫌って false を
    // 返す。MidiImporter は MThd + MTrk だけを抽出し直して読み込めること (回帰テスト)。
    void testImporterTrailingChunks()
    {
        beginTest("importer: SMF with trailing XF chunks (XFIH/XFKM) is recovered");

        // 正常な Type 0 SMF を 1 トラックで作る
        juce::MidiFile mf;
        mf.setTicksPerQuarterNote(480);
        juce::MidiMessageSequence seq;
        seq.addEvent(juce::MidiMessage::tempoMetaEvent(500000), 0.0);   // 120 BPM
        seq.addEvent(juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100), 0.0);
        seq.addEvent(juce::MidiMessage::noteOff(1, 60),                    480.0);
        mf.addTrack(seq);

        juce::MemoryBlock smf;
        {
            juce::MemoryOutputStream os(smf, false);
            expect(mf.writeTo(os), "writeTo ok");
        }

        // 末尾に Yamaha XF を模した独自チャンク (XFIH / XFKM) を付ける
        auto appendChunk = [](juce::MemoryBlock& mb, const char* id, int len)
        {
            mb.append(id, 4);
            const juce::uint8 b[4] = { (juce::uint8)(len >> 24), (juce::uint8)(len >> 16),
                                       (juce::uint8)(len >> 8),  (juce::uint8) len };
            mb.append(b, 4);
            std::vector<char> body((size_t) len, 0);
            mb.append(body.data(), (size_t) len);
        };
        appendChunk(smf, "XFIH", 16);
        appendChunk(smf, "XFKM", 32);

        auto f = outFile("trailing_xf.mid");
        f.deleteFile();
        {
            juce::FileOutputStream os(f);
            expect(os.openedOk(), "open output stream");
            os.write(smf.getData(), smf.getSize());
            os.flush();
        }

        // 素の JUCE では末尾チャンクのせいで失敗することを確認 (前提条件)
        {
            juce::FileInputStream is(f);
            juce::MidiFile plain;
            expect(! plain.readFrom(is), "raw JUCE readFrom fails on trailing chunks");
        }

        // MidiImporter は救済して読み込めること
        auto imp = MidiImporter::load(f);
        expect(imp.ok, "import recovers despite trailing chunks");
        expectEquals((int) imp.tracks.size(), 1, "one note track");
        expectWithinAbsoluteError(imp.initialBpm, 120.0, 0.05, "120 BPM");
        if (! imp.tracks.empty())
        {
            auto notes = extractNotes(imp.tracks[0]);
            expectEquals((int) notes.size(), 1, "one note");
            if (! notes.empty())
                expectEquals(notes[0].pitch, 60, "pitch 60");
        }
    }

    // ── importer: 1 トラックに複数チャンネルがある SMF (Type 0 等) をチャンネル別に分割する ──
    // Type 0 のカラオケ MIDI は全パートが 1 MTrk に同居する。各 MIDI チャンネルを独立した
    // トラックへ割り当て、正しいチャンネルルーティング + 楽器名で取り込めること (回帰テスト)。
    void testImporterChannelSplit()
    {
        beginTest("importer: multi-channel single track (Type 0) splits per channel");

        juce::MidiFile mf;
        mf.setTicksPerQuarterNote(480);

        // 1 つの MTrk に 3 チャンネル分のノートを入れる (ベース/ドラム/オーボエ)
        juce::MidiMessageSequence seq;
        seq.addEvent(juce::MidiMessage::tempoMetaEvent(500000), 0.0);  // 120 BPM
        // ch1: program 33 (Electric Bass finger)
        seq.addEvent(juce::MidiMessage::programChange(1, 33), 0.0);
        seq.addEvent(juce::MidiMessage::noteOn (1, 40, (juce::uint8) 100), 0.0);
        seq.addEvent(juce::MidiMessage::noteOff(1, 40),                    240.0);
        // ch10: ドラム (program 0)
        seq.addEvent(juce::MidiMessage::noteOn (10, 36, (juce::uint8) 110), 0.0);
        seq.addEvent(juce::MidiMessage::noteOff(10, 36),                    120.0);
        // ch2: program 68 (Oboe)
        seq.addEvent(juce::MidiMessage::programChange(2, 68), 0.0);
        seq.addEvent(juce::MidiMessage::noteOn (2, 72, (juce::uint8) 90), 240.0);
        seq.addEvent(juce::MidiMessage::noteOff(2, 72),                   480.0);
        mf.addTrack(seq);

        auto f = outFile("typed0_multichannel.mid");
        f.deleteFile();
        {
            juce::FileOutputStream os(f);
            expect(os.openedOk(), "open output stream");
            expect(mf.writeTo(os), "writeTo ok");
            os.flush();
        }

        auto imp = MidiImporter::load(f);
        expect(imp.ok, "import ok");
        expectEquals((int) imp.tracks.size(), 3, "split into 3 tracks (one per channel)");
        if (imp.tracks.size() != 3) return;

        // チャンネル順 (0-based) に並ぶ: ch1(idx0), ch2(idx1), ch10(idx9)
        expectEquals(imp.tracks[0].primaryChannel, 0,  "track0 = MIDI ch1");
        expectEquals(imp.tracks[1].primaryChannel, 1,  "track1 = MIDI ch2");
        expectEquals(imp.tracks[2].primaryChannel, 9,  "track2 = MIDI ch10");
        expect(! imp.tracks[0].isDrum, "ch1 not drum");
        expect(  imp.tracks[2].isDrum, "ch10 is drum");

        // 名前: GM 楽器名 (チャンネル番号は付けない) / ドラムは "Drums"
        expect(imp.tracks[0].name == "Electric Bass (finger)", "ch1 named by GM program");
        expect(imp.tracks[1].name == "Oboe",                   "ch2 named by GM program");
        expect(imp.tracks[2].name == "Drums",                  "ch10 named Drums");

        // 各トラックは自分のチャンネルのノートだけを持つ (混ざらない)
        expectEquals((int) extractNotes(imp.tracks[0]).size(), 1, "ch1 has its 1 note");
        expectEquals((int) extractNotes(imp.tracks[1]).size(), 1, "ch2 has its 1 note");
        expectEquals((int) extractNotes(imp.tracks[2]).size(), 1, "ch10 has its 1 note");
        expectEquals(extractNotes(imp.tracks[0])[0].pitch, 40, "ch1 pitch 40");
        expectEquals(extractNotes(imp.tracks[1])[0].pitch, 72, "ch2 pitch 72");
        expectEquals(extractNotes(imp.tracks[2])[0].pitch, 36, "ch10 pitch 36");
    }

    // ── 14. importer: 存在しないファイルは ok=false + エラー文 ──
    void testImporterMissingFile()
    {
        beginTest("importer: missing file returns failure");
        auto imp = MidiImporter::load(outFile("does_not_exist.mid"));
        expect(!imp.ok, "import reports failure");
        expect(imp.error.isNotEmpty(), "error message present");
    }

    // ── 15. MIDI 書き出しメニューの活性判定: TrackManager::hasMidiTrack() ──
    // メニュー (MainComponent) はテストターゲット外だが、活性条件は本メソッドに集約してある。
    // メニューコードと同じ述語を直接検証し、トラック追加/削除に追従することを担保する。
    void testHasMidiTrackPredicate()
    {
        beginTest("hasMidiTrack reflects MIDI track presence (menu enable state)");

        juce::AudioFormatManager fmt;
        fmt.registerBasicFormats();
        TrackManager tm(fmt);

        expect(!tm.hasMidiTrack(), "empty project has no MIDI track");

        tm.addTrack("Audio");                 // 通常 (オーディオ) トラックは数えない
        expect(!tm.hasMidiTrack(), "audio-only track does not count");

        auto* midi = tm.addTrack("Vox MIDI");
        midi->setMidiTrack(true);
        expect(tm.hasMidiTrack(), "a MIDI track is detected");

        tm.addClickTrack();                   // クリック (オーディオ扱い) を足しても変わらない
        expect(tm.hasMidiTrack(), "still true alongside non-MIDI tracks");

        // MIDI トラックを取り除くと false に戻る (メニューが削除に追従する根拠)
        int midiIdx = -1;
        for (int i = 0; i < tm.getTrackCount(); ++i)
            if (tm.getTrack(i) == midi) { midiIdx = i; break; }
        expect(midiIdx >= 0, "found MIDI track index");
        tm.removeTrack(midiIdx);
        expect(!tm.hasMidiTrack(), "no MIDI track after removal");
    }
};

static MidiIoTests midiIoTests;
