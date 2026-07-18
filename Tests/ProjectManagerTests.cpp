// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

// Utawave — ProjectManager (.uta 保存/読み込み) のユニットテスト
//
// データ永続性の耐久性契約。シリアライズのバグはユーザーのプロジェクトを黙って破壊する
// (テイク消失・ゲイン誤り・MIDI 破損)。save/load は手書き XML の二重写経で、片方だけに
// フィールドを足すとデータ消失するため round-trip で全フィールドを固定する。
//   ・save → load 全フィールド往復 (Track 属性 / AudioClip / Lane / Settings / bpm/meter 変化 /
//     loop / markers / MIDI クリップ)。load は既存トラックを全消去してから読む
//   ・Settings 属性欠落 XML → AppSettings{} 既定値にフォールバック
//   ・相対パス保存 + 欠損ファイル検出 (missingFiles + プレースホルダクリップ)
//   ・アトミック性 (.tmp が残らない) と null ガード / 不正ファイルで false
//
// pluginManager / masterChain は nullptr を渡して VST を回避する。
// AudioFormatManager は各テストのローカル。expect は ASCII。

#include <JuceHeader.h>
#include <cmath>
#include "../Source/Project/ProjectManager.h"   // Marker も含む (UI/TimelineView.h 経由)
#include "../Source/Tracks/TrackManager.h"
#include "../Source/Tracks/Track.h"
#include "../Source/Tracks/AudioClip.h"
#include "../Source/Tracks/MidiClip.h"
#include "../Source/AppSettings.h"
#include "../Source/VST/PluginChain.h"
#include "../Source/Audio/builtin/BuiltInFactory.h"

namespace
{
static bool approxEq(double a, double b, double eps) { return std::abs(a - b) < eps; }

class ProjectManagerTests : public juce::UnitTest
{
public:
    ProjectManagerTests() : juce::UnitTest("ProjectManager (save/load)") {}

    juce::File dir;

    juce::File writeWav(const juce::File& f, double sr, double secs)
    {
        f.getParentDirectory().createDirectory();
        f.deleteFile();
        std::unique_ptr<juce::FileOutputStream> os(f.createOutputStream());
        if (os == nullptr) return {};
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::AudioFormatWriter> w(wav.createWriterFor(os.get(), sr, 1, 16, {}, 0));
        if (w == nullptr) return {};
        os.release();
        const int n = (int) (sr * secs);
        juce::AudioBuffer<float> buf(1, n);
        buf.clear();
        w->writeFromAudioSampleBuffer(buf, 0, n);
        return f;
    }

    void runTest() override
    {
        dir = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("UtawavePMTests");
        dir.deleteRecursively();
        dir.createDirectory();

        testRoundTrip();
        testSettingsFallback();
        testLegacyRootTag();
        testRelativePathAndMissingFile();
        testAtomicAndGuards();
        testLyrics();
        testDeferredPluginRestore();

        dir.deleteRecursively();
    }

    // ── 全フィールド往復 ──
    void testRoundTrip()
    {
        beginTest("save/load: full round-trip of tracks, clips, MIDI, settings, markers, loop, bpm");
        auto projDir = dir.getChildFile("Proj");
        projDir.createDirectory();
        auto wav = writeWav(projDir.getChildFile("Audio").getChildFile("clip.wav"), 48000.0, 1.0);
        auto projFile = projDir.getChildFile("Proj.uta");

        // ── シーン構築 (A) ──
        juce::AudioFormatManager fmtA; fmtA.registerBasicFormats();
        TrackManager tmA(fmtA);

        auto* t0 = tmA.addTrack("Vocals", /*stereo*/ true);
        t0->setVolume(-6.0f); t0->setPan(0.3f); t0->setReverbSend(0.2f);
        t0->setMuted(true); t0->setSoloed(true); t0->setRecArmed(true);
        t0->setInputMonitor(true); t0->setInputChannel(1);
        t0->setColour(juce::Colour(0xff112233));
        t0->setLanesCollapsed(false); t0->setInsertSlotsVisible(true);
        t0->setCustomHeight(140); t0->setCustomLaneHeight(70);
        {
            auto* c = t0->getLane(0)->addClip(wav, 1.0, 3.0, t0->getFormatManager(), t0->getThumbnailCache());
            c->setFileOffset(0.5); c->setGain(0.7f); c->setName("take1");
            c->setColour(juce::Colour(0xffaabbcc));
            c->setFadeInSecs(0.1); c->setFadeOutSecs(0.2);
            c->setFadeInCurve(FadeCurve::EqualPower); c->setFadeOutCurve(FadeCurve::SCurve);
            c->getGainPointsRW() = { { 0.0, 0.0f }, { 1.0, -3.0f } };
            t0->getLane(0)->soloed = true;
        }

        auto* t1 = tmA.addTrack("Synth", false);
        t1->setMidiTrack(true);
        t1->setSynthWaveform(2); t1->setSynthEnabled(true);
        t1->setOctaveShift(1); t1->setSemitoneTranspose(3);
        {
            auto* mc = t1->addMidiClip(0.0, 2.0);
            mc->setChannel(9);
            auto& seq = mc->getSequence();
            seq.addEvent(juce::MidiMessage::noteOn (10, 60, (juce::uint8) 100), 0.0);
            seq.addEvent(juce::MidiMessage::noteOff(10, 60),                    0.5);
            seq.updateMatchedPairs();
        }

        // フォルダトラック + 配下の子 (フォルダ直後 = 連続ランの不変条件を満たす並びで保存)
        auto* t2 = tmA.addFolderTrack();
        t2->setName("Bus");
        t2->setVolume(-3.0f);
        t2->setFolderOpen(false);   // 閉じた状態も往復させる
        auto* t3 = tmA.addTrack("Chorus", false);
        t3->setFolderParent(t2);

        // アプリケーショントラック (アプリ音声取り込み) — フラグ + 対象 exe の往復
        auto* t4 = tmA.addAppCaptureTrack();
        t4->setName("Browser");
        t4->setAppCaptureExe("chrome.exe");
        t4->setVolume(-6.0f);

        AppSettings setA;
        setA.initialBpm = 100.0;
        setA.meterNumerator = 3; setA.meterDenominator = 4;
        setA.bpmChanges   = { { 4.0, 140.0 } };
        setA.meterChanges = { { 2, 5, 8 } };
        setA.snapMode = SnapMode::Eighth;
        setA.maxBackups = 30;
        setA.vuReferenceLevel = -20.0f;
        setA.loudnessTargetLufs = -16.0f;
        setA.autoNormalizeOnImport = false;   // default true -> flip
        setA.exportPeakGuard = false;         // default true -> flip
        // 残りの永続化属性も非既定値にして round-trip を網羅 (片側欠落 = データ消失の捕捉)
        setA.countInBars = 2; setA.preRollSecs = 1.5;
        setA.retrospectiveEnabled = false; setA.playheadFollowsSelection = true;
        setA.autoCrossfade = true; setA.zeroCrossingFade = true; setA.crossfadeDuration = 0.05;
        setA.showClipGain = true; setA.useMarkerColors = false; setA.toolMode = ToolMode::Selection;
        setA.resampleOutputBits = 24; setA.projectSampleRate = 44100.0; setA.projectBitDepth = 24;
        setA.masterInsertSlotsVisible = false; setA.masterPanelCollapsed = false;
        setA.rulerTimeRowVisible = false; setA.rulerBarsRowVisible = false;
        setA.autoSaveIntervalMinutes = 15; setA.stripImportedMetadata = false;
        setA.zoomToMousePosition = false; setA.returnToStartOnStop = false;
        // 歌詞: 複数行 + XML 特殊文字 (< > &) + 日本語で escaping / 改行保持を網羅
        const juce::String lyricsA = juce::String::fromUTF8(
            u8"1行目 <verse>\n2行目 & ロック\n\n最後の行");
        setA.lyrics = lyricsA.toStdString();

        std::vector<Marker> markersA = {
            { 2.0, "Verse",  juce::Colour(0xffcc0000) },
            { 5.5, "Chorus", juce::Colour(0xff0000cc) },
        };
        double bpmA = 128.0, loopSA = 1.0, loopEA = 4.0, headA = 7.0, ppbA = 120.0;
        bool loopActA = true;

        ProjectManager::State sA;
        sA.trackManager = &tmA; sA.appSettings = &setA; sA.markers = &markersA;
        sA.bpm = &bpmA; sA.loopStartSecs = &loopSA; sA.loopEndSecs = &loopEA;
        sA.loopActive = &loopActA; sA.playheadSecs = &headA; sA.pixelsPerBeat = &ppbA;
        sA.pluginManager = nullptr; sA.masterChain = nullptr;

        expect(ProjectManager::save(projFile, sA), "save succeeds");
        expect(projFile.existsAsFile(), "project file written");

        // ── 読み戻し (B): わざと既存トラックを 1 つ入れて、load が消去するか検証 ──
        juce::AudioFormatManager fmtB; fmtB.registerBasicFormats();
        TrackManager tmB(fmtB);
        tmB.addTrack("Stale", false);   // load で消える想定

        AppSettings setB;
        std::vector<Marker> markersB;
        std::vector<juce::String> missingB;
        double bpmB = 0, loopSB = 0, loopEB = 0, headB = 0, ppbB = 0;
        bool loopActB = false;

        ProjectManager::State sB;
        sB.trackManager = &tmB; sB.appSettings = &setB; sB.markers = &markersB;
        sB.bpm = &bpmB; sB.loopStartSecs = &loopSB; sB.loopEndSecs = &loopEB;
        sB.loopActive = &loopActB; sB.playheadSecs = &headB; sB.pixelsPerBeat = &ppbB;
        sB.pluginManager = nullptr; sB.masterChain = nullptr; sB.missingFiles = &missingB;

        expect(ProjectManager::load(projFile, sB), "load succeeds");

        // トラック数 (Stale が消えて 4)
        expect(tmB.getTrackCount() == 5, "load cleared the stale track and restored 5");

        // Track 0 属性
        auto* r0 = tmB.getTrack(0);
        expect(r0 != nullptr, "track 0 restored");
        if (r0)
        {
            expect(r0->getName() == "Vocals", "track 0 name");
            expect(approxEq(r0->getVolume(), -6.0, 1e-5), "track 0 volume");
            expect(approxEq(r0->getPan(), 0.3, 1e-5), "track 0 pan");
            expect(approxEq(r0->getReverbSend(), 0.2, 1e-5), "track 0 reverbSend");
            expect(r0->isMuted() && r0->isSoloed() && r0->isRecArmed(), "track 0 mute/solo/recArm");
            expect(r0->isInputMonitor() && r0->getInputChannel() == 1, "track 0 input monitor/channel");
            expect(r0->isStereo(), "track 0 stereo");
            expect(r0->getColour() == juce::Colour(0xff112233), "track 0 colour (ARGB)");
            expect(! r0->isLanesCollapsed() && r0->isInsertSlotsVisible(), "track 0 lane/insert flags");
            expect(r0->getCustomHeight() == t0->getCustomHeight()
                   && r0->getLaneHeight() == t0->getLaneHeight(), "track 0 custom/lane heights");
            expect(r0->getLane(0)->soloed.load(), "lane 0 soloed restored");

            auto* lane0 = r0->getLane(0);
            expect(! lane0->clips.empty(), "track 0 lane 0 has a clip");
            if (! lane0->clips.empty())
            {
                auto* c = lane0->clips[0].get();
                expect(c->getFile() == wav, "clip file resolved via relative path");
                expect(approxEq(c->getStartPosition(), 1.0, 1e-9), "clip start");
                expect(approxEq(c->getDuration(), 3.0, 1e-9), "clip dur");
                expect(approxEq(c->getFileOffset(), 0.5, 1e-9), "clip fileOffset");
                expect(approxEq(c->getGain(), 0.7, 1e-5), "clip gain");
                expect(c->getName() == "take1", "clip name");
                expect(c->getColour() == juce::Colour(0xffaabbcc), "clip custom colour");
                expect(approxEq(c->getFadeInSecs(), 0.1, 1e-9) && approxEq(c->getFadeOutSecs(), 0.2, 1e-9),
                       "clip fades");
                expect(c->getFadeInCurve() == FadeCurve::EqualPower
                       && c->getFadeOutCurve() == FadeCurve::SCurve, "clip fade curves");
                const auto& gp = c->getGainPoints();
                expect(gp.size() == 2, "clip gain envelope (2 points)");
                if (gp.size() == 2)
                    expect(approxEq(gp[0].time, 0.0, 1e-9) && approxEq(gp[0].dB, 0.0, 1e-4)
                           && approxEq(gp[1].time, 1.0, 1e-9) && approxEq(gp[1].dB, -3.0, 1e-4),
                           "gain envelope point times/dB values preserved");
            }
        }

        // Track 1 (MIDI)
        auto* r1 = tmB.getTrack(1);
        expect(r1 != nullptr && r1->isMidiTrack(), "track 1 restored as MIDI");
        if (r1)
        {
            expect(r1->getSynthWaveform() == 2, "synth waveform");
            expect(r1->isSynthEnabled(), "synth enabled");
            expect(r1->getOctaveShift() == 1 && r1->getSemitoneTranspose() == 3, "octave/semitone");
            expect(r1->getMidiClipCount() == 1, "1 MIDI clip");
            if (r1->getMidiClipCount() == 1)
            {
                auto* mc = r1->getMidiClip(0);
                expect(approxEq(mc->getStartPosition(), 0.0, 1e-9) && approxEq(mc->getDuration(), 2.0, 1e-9),
                       "MIDI clip start/duration");
                expect(mc->getChannel() == 9, "MIDI clip channel 9 (drum)");
                auto& seq = mc->getSequence();
                const juce::MidiMessage* on = nullptr;
                const juce::MidiMessage* off = nullptr;
                int noteOns = 0;
                for (int i = 0; i < seq.getNumEvents(); ++i)
                {
                    const auto& m = seq.getEventPointer(i)->message;
                    if (m.isNoteOn())  { ++noteOns; on = &m; }
                    if (m.isNoteOff()) { off = &m; }
                }
                expect(noteOns == 1, "exactly one note-on preserved");
                expect(on != nullptr && on->getNoteNumber() == 60 && (int) on->getVelocity() == 100
                       && approxEq(on->getTimeStamp(), 0.0, 1e-6),
                       "note-on pitch/velocity/time preserved");
                expect(off != nullptr && approxEq(off->getTimeStamp(), 0.5, 1e-6),
                       "note-off timing preserved");
            }
        }

        // Track 2 (フォルダ) + Track 3 (配下の子)
        auto* r2 = tmB.getTrack(2);
        auto* r3 = tmB.getTrack(3);
        expect(r2 != nullptr && r2->isFolderTrack(), "track 2 restored as folder track");
        expect(r3 != nullptr && !r3->isFolderTrack(), "track 3 restored as normal track");
        if (r2 && r3)
        {
            expect(r2->getName() == "Bus", "folder name");
            expect(approxEq(r2->getVolume(), -3.0, 1e-5), "folder volume");
            expect(! r2->isFolderOpen(), "folder open state (closed) restored");
            expect(r3->getFolderParent() == r2, "child folder membership resolved to instance");
            expect(r2->getFolderParent() == nullptr, "folder itself is top-level");
        }

        // Track 4 (アプリケーショントラック)
        auto* r4 = tmB.getTrack(4);
        expect(r4 != nullptr && r4->isAppCaptureTrack(), "track 4 restored as application track");
        if (r4)
        {
            expect(r4->getAppCaptureExe() == juce::String("chrome.exe"), "app capture exe restored");
            expect(approxEq(r4->getVolume(), -6.0, 1e-5), "app track volume restored");
        }
        expect(r3 != nullptr && !r3->isAppCaptureTrack(), "normal track is not an app track");

        // Settings
        expect(approxEq(setB.initialBpm, 100.0, 1e-9), "initialBpm");
        expect(setB.meterNumerator == 3 && setB.meterDenominator == 4, "meter 3/4");
        expect(setB.bpmChanges.size() == 1 && approxEq(setB.bpmChanges[0].timeSec, 4.0, 1e-9)
               && approxEq(setB.bpmChanges[0].bpm, 140.0, 1e-9), "bpmChanges restored");
        expect(setB.meterChanges.size() == 1 && setB.meterChanges[0].barIndex == 2
               && setB.meterChanges[0].numerator == 5 && setB.meterChanges[0].denominator == 8,
               "meterChanges restored");
        expect(setB.snapMode == SnapMode::Eighth, "snapMode");
        expect(setB.maxBackups == 30, "maxBackups");
        expect(approxEq(setB.vuReferenceLevel, -20.0, 1e-4), "vuReferenceLevel");
        expect(approxEq(setB.loudnessTargetLufs, -16.0, 1e-4), "loudnessTargetLufs");
        expect(! setB.autoNormalizeOnImport && ! setB.exportPeakGuard, "flipped bools (autoNorm/peakGuard)");
        expect(setB.countInBars == 2 && approxEq(setB.preRollSecs, 1.5, 1e-9), "countInBars/preRollSecs");
        expect(! setB.retrospectiveEnabled && setB.playheadFollowsSelection, "retrospective/followSelection");
        expect(setB.autoCrossfade && setB.zeroCrossingFade && approxEq(setB.crossfadeDuration, 0.05, 1e-9),
               "autoCrossfade/zeroCrossingFade/crossfadeDuration");
        expect(setB.showClipGain && ! setB.useMarkerColors && setB.toolMode == ToolMode::Selection,
               "showClipGain/useMarkerColors/toolMode");
        expect(setB.resampleOutputBits == 24 && approxEq(setB.projectSampleRate, 44100.0, 1e-6)
               && setB.projectBitDepth == 24, "resampleOutputBits/projectSampleRate/projectBitDepth");
        expect(! setB.masterInsertSlotsVisible && ! setB.masterPanelCollapsed
               && ! setB.rulerTimeRowVisible && ! setB.rulerBarsRowVisible,
               "master/ruler visibility flags");
        expect(setB.autoSaveIntervalMinutes == 15 && ! setB.stripImportedMetadata
               && ! setB.zoomToMousePosition && ! setB.returnToStartOnStop,
               "autoSaveInterval/stripMetadata/zoomMouse/returnToStart");
        expect(juce::String::fromUTF8(setB.lyrics.c_str()) == lyricsA,
               "lyrics round-trip (multiline + XML special chars)");

        // Transport / loop / markers
        expect(approxEq(bpmB, 128.0, 1e-9), "transport bpm");
        expect(approxEq(headB, 7.0, 1e-9), "playhead");
        expect(approxEq(ppbB, 120.0, 1e-9), "pixelsPerBeat (View)");
        expect(loopActB && approxEq(loopSB, 1.0, 1e-9) && approxEq(loopEB, 4.0, 1e-9), "loop restored");
        expect(markersB.size() == 2, "2 markers restored");
        if (markersB.size() == 2)
        {
            expect(approxEq(markersB[0].time, 2.0, 1e-9) && markersB[0].name == "Verse"
                   && markersB[0].colour == juce::Colour(0xffcc0000), "marker 0 time/name/colour");
            expect(approxEq(markersB[1].time, 5.5, 1e-9) && markersB[1].name == "Chorus", "marker 1");
        }
        expect(missingB.empty(), "no missing files (clip resolved)");
    }

    // ── Settings 属性欠落 → 既定値にフォールバック ──
    void testSettingsFallback()
    {
        beginTest("load: missing Settings attributes fall back to AppSettings{} defaults");
        auto f = dir.getChildFile("fallback.uta");
        {
            juce::XmlElement root("UtawaveProject");
            root.setAttribute("version", "1.0");
            root.createNewChildElement("Settings");   // 空 (属性なし)
            root.writeTo(f);
        }

        juce::AudioFormatManager fmt; fmt.registerBasicFormats();
        TrackManager tm(fmt);
        AppSettings set;
        // わざと既定から外す → load 後に既定へ戻ることを検証
        set.snapMode = SnapMode::Bar;
        set.maxBackups = 999;
        set.autoNormalizeOnImport = false;
        set.exportPeakGuard = false;
        set.returnToStartOnStop = false;

        ProjectManager::State s;
        s.trackManager = &tm; s.appSettings = &set;
        s.pluginManager = nullptr; s.masterChain = nullptr;
        expect(ProjectManager::load(f, s), "load minimal project");

        const AppSettings def {};
        expect(set.snapMode == def.snapMode, "snapMode -> default (Off)");
        expect(set.maxBackups == def.maxBackups, "maxBackups -> default (20)");
        expect(set.autoNormalizeOnImport == def.autoNormalizeOnImport, "autoNormalizeOnImport -> default (true)");
        expect(set.exportPeakGuard == def.exportPeakGuard, "exportPeakGuard -> default (true)");
        expect(set.returnToStartOnStop == def.returnToStartOnStop, "returnToStartOnStop -> default (true)");
    }

    // ── 旧名 (Trakova) 時代のルートタグ互換 ──
    void testLegacyRootTag()
    {
        beginTest("load: legacy TrakovaProject root tag is accepted");
        auto f = dir.getChildFile("legacy.trakova");
        {
            juce::XmlElement root("TrakovaProject");
            root.setAttribute("version", "1.0");
            root.createNewChildElement("Settings");
            root.writeTo(f);
        }

        juce::AudioFormatManager fmt; fmt.registerBasicFormats();
        TrackManager tm(fmt);
        AppSettings set;
        ProjectManager::State s;
        s.trackManager = &tm; s.appSettings = &set;
        s.pluginManager = nullptr; s.masterChain = nullptr;
        expect(ProjectManager::load(f, s), "load legacy-root project");
    }

    // ── 相対パス保存 + 欠損ファイル検出 ──
    void testRelativePathAndMissingFile()
    {
        beginTest("save stores relative path; load of missing audio populates missingFiles + placeholder");
        auto projDir = dir.getChildFile("Proj2");
        projDir.createDirectory();
        auto wav = writeWav(projDir.getChildFile("Audio").getChildFile("a.wav"), 48000.0, 1.0);
        auto projFile = projDir.getChildFile("Proj2.uta");

        juce::AudioFormatManager fmt; fmt.registerBasicFormats();
        TrackManager tm(fmt);
        auto* t = tm.addTrack("T", false);
        t->getLane(0)->addClip(wav, 0.0, 1.0, t->getFormatManager(), t->getThumbnailCache());
        AppSettings set;
        ProjectManager::State s;
        s.trackManager = &tm; s.appSettings = &set;
        s.pluginManager = nullptr; s.masterChain = nullptr;
        expect(ProjectManager::save(projFile, s), "save");

        // 保存された file 属性が相対 (絶対パスでない・"Audio" を含む) であること
        auto xml = juce::XmlDocument::parse(projFile);
        expect(xml != nullptr, "saved project parses");
        juce::String savedFileAttr;
        if (xml)
            if (auto* tracks = xml->getChildByName("Tracks"))
                if (auto* trackEl = tracks->getChildByName("Track"))
                    if (auto* lanes = trackEl->getChildByName("Lanes"))
                        if (auto* lane = lanes->getChildByName("Lane"))
                            if (auto* clip = lane->getChildByName("Clip"))
                                savedFileAttr = clip->getStringAttribute("file");
        expect(savedFileAttr.isNotEmpty(), "clip file attribute present");
        expect(! juce::File::isAbsolutePath(savedFileAttr), "saved clip path is relative (not absolute)");
        expect(savedFileAttr.contains("a.wav"), "relative path points at the audio file");

        // 音声ファイルを消す → load で missingFiles に積まれ、プレースホルダクリップは作られる
        wav.deleteFile();
        TrackManager tm2(fmt);
        AppSettings set2;
        std::vector<juce::String> missing;
        ProjectManager::State s2;
        s2.trackManager = &tm2; s2.appSettings = &set2;
        s2.pluginManager = nullptr; s2.masterChain = nullptr; s2.missingFiles = &missing;
        expect(ProjectManager::load(projFile, s2), "load with missing audio");
        expect(missing.size() == 1, "missing file recorded");
        expect(tm2.getTrack(0) != nullptr && ! tm2.getTrack(0)->getLane(0)->clips.empty(),
               "placeholder clip still created for missing file");
    }

    // ── アトミック性 + null ガード / 不正ファイル ──
    void testAtomicAndGuards()
    {
        beginTest("save: atomic (no .tmp leftover); guards (null / missing / bad XML) return false");
        auto projDir = dir.getChildFile("Proj3");
        projDir.createDirectory();
        auto projFile = projDir.getChildFile("Proj3.uta");

        juce::AudioFormatManager fmt; fmt.registerBasicFormats();
        TrackManager tm(fmt);
        tm.addTrack("T", false);
        AppSettings set;
        ProjectManager::State s;
        s.trackManager = &tm; s.appSettings = &set;
        s.pluginManager = nullptr; s.masterChain = nullptr;

        expect(ProjectManager::save(projFile, s), "save ok");
        expect(! projFile.getSiblingFile(projFile.getFileName() + ".tmp").existsAsFile(),
               "no .tmp leftover after save (atomic)");

        // null ガード: trackManager / appSettings が無ければ false
        ProjectManager::State bad;
        bad.trackManager = nullptr; bad.appSettings = &set;
        expect(! ProjectManager::save(projFile, bad), "save with null trackManager -> false");
        expect(! ProjectManager::load(projFile, bad), "load with null trackManager -> false");

        // load: 存在しないファイル / 不正 XML -> false
        expect(! ProjectManager::load(dir.getChildFile("nope.uta"), s), "load missing file -> false");
        auto badXml = dir.getChildFile("bad.uta");
        badXml.replaceWithText("<NotAUtawaveProject/>");
        expect(! ProjectManager::load(badXml, s), "load non-UtawaveProject XML -> false");
    }

    // ── 歌詞 (歌詞表示窓のテキスト永続化) ──
    void testLyrics()
    {
        beginTest("save/load: lyrics persist; empty writes no <Lyrics>; absent element clears stale");
        auto projDir = dir.getChildFile("ProjLyrics");
        projDir.createDirectory();
        auto projFile = projDir.getChildFile("ProjLyrics.uta");

        juce::AudioFormatManager fmt; fmt.registerBasicFormats();
        TrackManager tm(fmt);
        tm.addTrack("T", false);

        ProjectManager::State s;
        s.trackManager = &tm;
        s.pluginManager = nullptr; s.masterChain = nullptr;

        // (1) 空の歌詞は <Lyrics> 要素を書き出さない (XML を肥大化させない)
        AppSettings setEmpty;   // lyrics は既定の空
        s.appSettings = &setEmpty;
        expect(ProjectManager::save(projFile, s), "save with empty lyrics");
        {
            auto xml = juce::XmlDocument::parse(projFile);
            expect(xml != nullptr, "saved XML parses");
            expect(xml != nullptr && xml->getChildByName("Lyrics") == nullptr,
                   "empty lyrics -> no <Lyrics> element written");
        }

        // (2) 非空の歌詞 (複数行 + 特殊文字) を保存すると <Lyrics> 要素ができ、読み戻せる
        const juce::String lyrics = juce::String::fromUTF8(
            u8"サビ <hook>\n  二行目 & 三行目\n\n空行のあと");
        AppSettings setSave; setSave.lyrics = lyrics.toStdString();
        s.appSettings = &setSave;
        expect(ProjectManager::save(projFile, s), "save with lyrics");
        {
            auto xml = juce::XmlDocument::parse(projFile);
            expect(xml != nullptr && xml->getChildByName("Lyrics") != nullptr,
                   "non-empty lyrics -> <Lyrics> element present");
        }

        AppSettings setLoad;
        s.appSettings = &setLoad;
        expect(ProjectManager::load(projFile, s), "load lyrics project");
        expect(juce::String::fromUTF8(setLoad.lyrics.c_str()) == lyrics,
               "lyrics restored exactly (multiline + special chars preserved)");

        // (3) <Lyrics> の無いプロジェクトを読むと、既存の歌詞は空にクリアされる
        //     (別プロジェクトの歌詞が持ち越されないこと = carry-over 防止の回帰テスト)
        AppSettings setStale; setStale.lyrics = std::string("古い歌詞が残っている");
        auto noLyricsFile = projDir.getChildFile("NoLyrics.uta");
        {
            juce::XmlElement root("UtawaveProject");
            root.setAttribute("version", "1.0");
            root.createNewChildElement("Settings");   // Lyrics 子要素なし
            root.writeTo(noLyricsFile);
        }
        s.appSettings = &setStale;
        expect(ProjectManager::load(noLyricsFile, s), "load project without <Lyrics>");
        expect(setStale.lyrics.empty(), "absent <Lyrics> clears pre-existing lyrics (no carry-over)");
    }

    // ── 遅延プラグイン復元 (deferredPlugins) ──
    // プロジェクトを開く体感短縮のため、State::deferredPlugins が非 null なら load は VST3 を
    // 同期生成せず情報だけを積む契約。内蔵エフェクト (format="Utawave") は従来どおり即時復元。
    // slotIndex 欠落 (旧ファイル) のフォールバックは同期経路の「その時点の getNumPlugins()」と
    // 一致すること (明示 slot 付きの遅延分とも衝突しない) を固定する。
    void testDeferredPluginRestore()
    {
        beginTest("load: deferredPlugins collects VST3 without instantiation; built-in restored inline");
        auto projDir = dir.getChildFile("ProjDefer");
        projDir.createDirectory();
        auto projFile = projDir.getChildFile("ProjDefer.uta");

        // 内蔵 EQ の保存 ID を実物から取得 (createIdentifierString はロケール非依存)
        juce::String eqId;
        {
            auto eq = BuiltInFactory::create("utawave.eq");
            expect(eq != nullptr, "factory creates built-in EQ");
            if (eq) eqId = eq->getPluginDescription().createIdentifierString();
        }

        // 手書き XML: トラックに [slot0 = 内蔵EQ, slot2 = VST3 (bypassed + state),
        // slotIndex 無し VST3]、マスターに [slot1 = VST3]
        juce::MemoryBlock stateBlob("hello", 5);
        {
            juce::XmlElement root("UtawaveProject");
            root.setAttribute("version", "1.0");
            auto* tracksEl = root.createNewChildElement("Tracks");
            auto* trackEl  = tracksEl->createNewChildElement("Track");
            trackEl->setAttribute("name", "Vocal");
            auto* pl = trackEl->createNewChildElement("Plugins");
            {
                auto* p = pl->createNewChildElement("Plugin");
                p->setAttribute("slotIndex", 0);
                p->setAttribute("id", eqId);
                p->setAttribute("format", "Utawave");
            }
            {
                auto* p = pl->createNewChildElement("Plugin");
                p->setAttribute("slotIndex", 2);
                p->setAttribute("id", "VST3-FakeComp-1a2b");
                p->setAttribute("name", "FakeComp");
                p->setAttribute("format", "VST3");
                p->setAttribute("bypassed", 1);
                p->setAttribute("state", stateBlob.toBase64Encoding());
            }
            {
                auto* p = pl->createNewChildElement("Plugin");   // slotIndex 属性なし (旧ファイル)
                p->setAttribute("id", "VST3-NoSlot-9z");
                p->setAttribute("name", "NoSlot");
                p->setAttribute("format", "VST3");
            }
            auto* masterEl = root.createNewChildElement("Master");
            auto* mpl = masterEl->createNewChildElement("Plugins");
            {
                auto* p = mpl->createNewChildElement("Plugin");
                p->setAttribute("slotIndex", 1);
                p->setAttribute("id", "VST3-MasterVerb-77");
                p->setAttribute("name", "MasterVerb");
                p->setAttribute("format", "VST3");
            }
            expect(root.writeTo(projFile), "test project XML written");
        }

        juce::AudioFormatManager fmt; fmt.registerBasicFormats();
        TrackManager tm(fmt);
        AppSettings as;
        PluginChain master;
        std::vector<ProjectManager::DeferredPlugin> deferred;

        ProjectManager::State s;
        s.trackManager    = &tm;
        s.appSettings     = &as;
        s.masterChain     = &master;
        s.pluginManager   = nullptr;      // 遅延経路は PluginManager 無しでも収集できる
        s.deferredPlugins = &deferred;
        expect(ProjectManager::load(projFile, s), "load succeeds");

        expectEquals(tm.getTrackCount(), 1, "one track loaded");
        auto* tr = tm.getTrack(0);
        expect(tr != nullptr, "track exists");
        if (tr == nullptr) return;

        // 内蔵 EQ は load 内で即時復元される (VST3 の遅延に巻き込まれない)
        expect(tr->getPluginChain().getPlugin(0) != nullptr, "built-in restored inline at slot 0");
        // VST3 はチェーンへ入らず遅延リストに XML 順で積まれる
        expectEquals((int) deferred.size(), 3, "three deferred VST3 entries collected");
        if (deferred.size() != 3) return;

        expect(deferred[0].track == tr,                     "entry 0 targets the track");
        expectEquals(deferred[0].id,   juce::String("VST3-FakeComp-1a2b"), "entry 0 id");
        expectEquals(deferred[0].name, juce::String("FakeComp"),           "entry 0 name");
        expectEquals(deferred[0].slotIndex, 2,              "entry 0 explicit slot preserved");
        expect(deferred[0].bypassed,                        "entry 0 bypass preserved");
        expectEquals(deferred[0].stateB64, stateBlob.toBase64Encoding(), "entry 0 state preserved");

        // slotIndex 欠落: 同期経路なら「内蔵@0 + VST@2 挿入後の getNumPlugins() = 3」に置かれる。
        // 遅延経路も simulatedSlots の追跡で同じ 3 になる (明示 slot2 と衝突しないことの回帰)
        expect(deferred[1].track == tr,                     "entry 1 targets the track");
        expectEquals(deferred[1].slotIndex, 3,              "entry 1 fallback slot matches sync path");
        expect(! deferred[1].bypassed,                      "entry 1 not bypassed");

        expect(deferred[2].track == nullptr,                "master entry has null track");
        expectEquals(deferred[2].id, juce::String("VST3-MasterVerb-77"), "master entry id");
        expectEquals(deferred[2].slotIndex, 1,              "master entry slot");
        // マスターチェーンにも VST3 は入っていない (内蔵なしなので空のまま)
        expectEquals(master.getNumPlugins(), 0, "master chain untouched by deferral");
    }
};

static ProjectManagerTests projectManagerTests;
}
