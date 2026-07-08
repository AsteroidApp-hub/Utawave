// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

// Utawave — 録音まわりのユニットテスト
//
// オーディオデバイス不要・決定論的に検証する:
//   ・パンチインのクロスフェード Track::trimAndCrossfadeOnLane0 の全ケース
//       内包→消去 / またぎ→左右分割 (right の fileOffset 再計算・両端 30ms フェード) /
//       左端のみ重複→末尾トリム / 右端のみ重複→開始押し出し+fileOffset 前進 / 非重複→不変 /
//       新クリップに 30ms フェード / 残り ≤0.01s は消去
//   ・finishLiveRecording の lane0 配置と新録音自身のテイクバックアップ
//       (上書きされた既存クリップ / トリム断片はテイクレーンへ退避しない)
//   ・LiveRecordingBuffer (遅延確保・ピーク蓄積・reset・尺算術)
//
// trimAndCrossfadeOnLane0 は位置/フェード/fileOffset を操作するだけでデコードしないため
// ダミー File で可。finishLiveRecording は refreshThumbnail を呼ぶので実 WAV を使う。
// AudioFormatManager は各テストのローカル (静的だと終了時 leak assertion)。expect は ASCII。

#include <JuceHeader.h>
#include <cmath>
#include "../Source/Tracks/TrackManager.h"
#include "../Source/Tracks/Track.h"
#include "../Source/Tracks/AudioClip.h"
#include "../Source/Recording/LiveRecordingBuffer.h"
#include "../Source/Recording/RecordingManager.h"
#include "../Source/Audio/AudioEngine.h"

namespace
{
static bool approxEq(double a, double b, double eps) { return std::abs(a - b) < eps; }

class RecordingTests : public juce::UnitTest
{
public:
    RecordingTests() : juce::UnitTest("Recording (punch-in / takes)") {}

    static constexpr double kXfade = 0.030;   // trimAndCrossfadeOnLane0 と同じ

    void runTest() override
    {
        testPunchContained();
        testPunchStraddle();
        testPunchLeftOverlap();
        testPunchRightOverlap();
        testPunchNonOverlap();
        testPunchTinyRemainderErased();
        testFinishLiveRecordingNoDisplacedBackup();
        testFinishLiveRecordingThumbnailsMatch();
        testFinishLiveRecordingWithFileOffset();
        testLatencyCompensation();
        testLatencyCompensationFloor();
        testLiveRecordingBuffer();
        testLoopTakeSlice();
        testLiveRecordingLead();
        testLiveOverlayLeadIncludesComp();
        testFindFreeTakeLane();
        testLoopTakeLaneReuse();
        testLoopLastTakeOnLane0();
        testDiscardRecordingNormal();
        testDiscardRecordingLoop();
        testStopTakesOnlyNormal();
        testStopTakesOnlyLoop();
        testPunchFromRetroTakesOnlyAndDiscard();
    }

    // lane0 に直接クリップを足す (overlaps チェック無しでそのまま lane0 に入る)
    AudioClip* addLane0(Track* t, const juce::File& f, double start, double dur, double fo = 0.0)
    {
        auto* c = t->getLane(0)->addClip(f, start, dur, t->getFormatManager(), t->getThumbnailCache());
        if (c) c->setFileOffset(fo);
        return c;
    }

    AudioClip* clipAtStart(Lane* lane, double start, double tol = 0.02)
    {
        for (auto& c : lane->clips)
            if (std::abs(c->getStartPosition() - start) < tol) return c.get();
        return nullptr;
    }

    // ── パンチイン: 内包 (既存が新規の内側) → 消去 ──
    void testPunchContained()
    {
        beginTest("trimAndCrossfadeOnLane0: existing fully inside punch -> erased");
        juce::AudioFormatManager fmt; fmt.registerBasicFormats();
        TrackManager tm(fmt);
        auto* t = tm.addTrack();
        const juce::File dummy("/tmp/utawave_punch.wav");

        addLane0(t, dummy, 1.0, 1.0);          // existing [1,2]
        auto* nc = addLane0(t, dummy, 0.0, 4.0);   // new [0,4] (contains existing)
        t->trimAndCrossfadeOnLane0(nc, 0.0, 4.0);

        expect((int) t->getLane(0)->clips.size() == 1, "contained existing clip erased (only new remains)");
        expect(t->getLane(0)->clips[0].get() == nc, "remaining clip is the new recording");
    }

    // ── パンチイン: またぎ (既存が新規をまたぐ) → 左右分割 ──
    void testPunchStraddle()
    {
        beginTest("trimAndCrossfadeOnLane0: existing straddles punch -> split into left + right");
        juce::AudioFormatManager fmt; fmt.registerBasicFormats();
        TrackManager tm(fmt);
        auto* t = tm.addTrack();
        const juce::File dummy("/tmp/utawave_punch.wav");

        addLane0(t, dummy, 0.0, 4.0, /*fo=*/0.0);   // existing [0,4]
        auto* nc = addLane0(t, dummy, 1.0, 2.0);    // new [1,3] (punch start=1 dur=2)
        t->trimAndCrossfadeOnLane0(nc, 1.0, 2.0);

        // left [0, ~1.03] fadeOut 0.03 / new [1,3] / right [~2.97, 4] fileOffset ~2.97 fadeIn 0.03
        expect((int) t->getLane(0)->clips.size() == 3, "straddle -> 3 clips (left, new, right)");

        auto* left = clipAtStart(t->getLane(0), 0.0);
        expect(left != nullptr, "left piece exists at start 0");
        if (left)
        {
            expect(approxEq(left->getDuration(), 1.0 + kXfade, 1e-3), "left trimmed to punchStart + 30ms");
            expect(approxEq(left->getFadeOutSecs(), kXfade, 1e-4), "left has 30ms fade-out");
        }

        auto* right = clipAtStart(t->getLane(0), 3.0 - kXfade);
        expect(right != nullptr, "right piece exists at punchEnd - 30ms");
        if (right)
        {
            expect(approxEq(right->getDuration(), 4.0 - (3.0 - kXfade), 1e-3), "right spans to original end");
            expect(approxEq(right->getFileOffset(), 3.0 - kXfade, 1e-3),
                   "right fileOffset recalculated to keep file continuity");
            expect(approxEq(right->getFadeInSecs(), kXfade, 1e-4), "right has 30ms fade-in");
        }

        expect(approxEq(nc->getFadeInSecs(), kXfade, 1e-4),  "new clip has 30ms fade-in");
        expect(approxEq(nc->getFadeOutSecs(), kXfade, 1e-4), "new clip has 30ms fade-out");
    }

    // ── パンチイン: 左端のみ重複 (既存が左から食い込む) → 末尾トリム ──
    void testPunchLeftOverlap()
    {
        beginTest("trimAndCrossfadeOnLane0: existing overlaps from the left -> trimmed end + fade-out");
        juce::AudioFormatManager fmt; fmt.registerBasicFormats();
        TrackManager tm(fmt);
        auto* t = tm.addTrack();
        const juce::File dummy("/tmp/utawave_punch.wav");

        addLane0(t, dummy, 0.0, 2.0);            // existing [0,2]
        auto* nc = addLane0(t, dummy, 1.0, 2.0); // new [1,3]
        t->trimAndCrossfadeOnLane0(nc, 1.0, 2.0);

        auto* left = clipAtStart(t->getLane(0), 0.0);
        expect(left != nullptr, "existing kept (left part)");
        if (left)
        {
            expect(approxEq(left->getDuration(), 1.0 + kXfade, 1e-3), "trimmed to punchStart + 30ms");
            expect(approxEq(left->getFadeOutSecs(), kXfade, 1e-4), "30ms fade-out at the join");
        }
        expect((int) t->getLane(0)->clips.size() == 2, "no extra split piece for left-only overlap");
    }

    // ── パンチイン: 右端のみ重複 (既存が右へ食い込む) → 開始押し出し + fileOffset 前進 ──
    void testPunchRightOverlap()
    {
        beginTest("trimAndCrossfadeOnLane0: existing overlaps from the right -> start pushed + fileOffset advanced");
        juce::AudioFormatManager fmt; fmt.registerBasicFormats();
        TrackManager tm(fmt);
        auto* t = tm.addTrack();
        const juce::File dummy("/tmp/utawave_punch.wav");

        addLane0(t, dummy, 2.0, 2.0, /*fo=*/0.0);   // existing [2,4] fileOffset 0
        auto* nc = addLane0(t, dummy, 1.0, 2.0);    // new [1,3]
        t->trimAndCrossfadeOnLane0(nc, 1.0, 2.0);

        // existing pushed to max(2, endPos-30ms) = max(2, 2.97) = 2.97
        auto* right = clipAtStart(t->getLane(0), 3.0 - kXfade);
        expect(right != nullptr, "existing start pushed to punchEnd - 30ms");
        if (right)
        {
            expect(approxEq(right->getStartPosition(), 3.0 - kXfade, 1e-3), "start pushed right");
            expect(approxEq(right->getFileOffset(), (3.0 - kXfade) - 2.0, 1e-3),
                   "fileOffset advanced by the trimmed amount");
            expect(approxEq(right->getDuration(), 4.0 - (3.0 - kXfade), 1e-3), "duration shortened from the left");
            expect(approxEq(right->getFadeInSecs(), kXfade, 1e-4), "30ms fade-in at the join");
        }
        expect((int) t->getLane(0)->clips.size() == 2, "no extra split piece for right-only overlap");
    }

    // ── パンチイン: 非重複 → 不変 ──
    void testPunchNonOverlap()
    {
        beginTest("trimAndCrossfadeOnLane0: non-overlapping existing clip is untouched");
        juce::AudioFormatManager fmt; fmt.registerBasicFormats();
        TrackManager tm(fmt);
        auto* t = tm.addTrack();
        const juce::File dummy("/tmp/utawave_punch.wav");

        auto* other = addLane0(t, dummy, 5.0, 1.0);   // existing [5,6] (far away)
        const double offFade = other->getFadeOutSecs();
        auto* nc = addLane0(t, dummy, 1.0, 2.0);      // new [1,3]
        t->trimAndCrossfadeOnLane0(nc, 1.0, 2.0);

        expect(approxEq(other->getStartPosition(), 5.0, 1e-9), "non-overlapping clip start unchanged");
        expect(approxEq(other->getDuration(), 1.0, 1e-9), "non-overlapping clip duration unchanged");
        expect(approxEq(other->getFadeOutSecs(), offFade, 1e-9), "non-overlapping clip fade unchanged");
    }

    // ── パンチイン: トリム後の残りが ≤0.01s → 消去 ──
    void testPunchTinyRemainderErased()
    {
        beginTest("trimAndCrossfadeOnLane0: tiny trimmed remainder (<= 0.01s) is erased");
        juce::AudioFormatManager fmt; fmt.registerBasicFormats();
        TrackManager tm(fmt);
        auto* t = tm.addTrack();
        const juce::File dummy("/tmp/utawave_punch.wav");

        // existing [2.999, 3.005] : 右端だけ punch [1,3] を僅かに超える → 開始押し出しで残り ~0.006s
        addLane0(t, dummy, 2.999, 0.006);
        auto* nc = addLane0(t, dummy, 1.0, 2.0);   // new [1,3]
        t->trimAndCrossfadeOnLane0(nc, 1.0, 2.0);

        expect((int) t->getLane(0)->clips.size() == 1, "tiny remainder erased (only new remains)");
        expect(t->getLane(0)->clips[0].get() == nc, "remaining clip is the new recording");
    }

    // 前半 loud / 後半 quiet の識別しやすい WAV (波形の取り違えを検出するため)
    juce::File writeWavHalves(const juce::File& dir, const juce::String& name, double sr, double secs)
    {
        auto f = dir.getChildFile(name);
        f.deleteFile();
        std::unique_ptr<juce::FileOutputStream> os(f.createOutputStream());
        if (os == nullptr) return {};
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::AudioFormatWriter> w(wav.createWriterFor(os.get(), sr, 1, 16, {}, 0));
        if (w == nullptr) return {};
        os.release();
        const int n = (int)(sr * secs);
        juce::AudioBuffer<float> buf(1, n);
        for (int i = 0; i < n; ++i)
        {
            const float amp = (i < n / 2) ? 0.5f : 0.04f;   // 前半 loud / 後半 quiet
            buf.setSample(0, i, amp * std::sin(2.0 * juce::MathConstants<double>::pi * 220.0 * i / sr));
        }
        w->writeFromAudioSampleBuffer(buf, 0, n);
        return f;
    }

    // 通常録音で lane0 クリップとテイク退避が「同じ波形」を描くことの検証。
    // (両者は同一ファイル・fileOffset 0 のはずなのに描画が食い違う不具合の回帰テスト)
    void testFinishLiveRecordingThumbnailsMatch()
    {
        beginTest("finishLiveRecording: lane0 clip and take backup have identical thumbnails");
        juce::AudioFormatManager fmt; fmt.registerBasicFormats();
        TrackManager tm(fmt);
        auto* t = tm.addTrack();
        auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                       .getChildFile("UtawaveRecTests4");
        dir.createDirectory();
        auto f = writeWavHalves(dir, "rec.wav", 48000.0, 4.0);
        expect(f.existsAsFile(), "distinctive WAV written");

        t->startLiveRecording(1.0);
        t->finishLiveRecording(f, 1.0, 4.0);   // lane0 + take backup, both fileOffset 0

        AudioClip* lane0clip = nullptr;
        for (auto& c : t->getLane(0)->clips) if (c->getFile() == f) lane0clip = c.get();
        AudioClip* takeClip = nullptr;
        for (int li = 1; li < t->getLaneCount(); ++li)
            for (auto& c : t->getLane(li)->clips) if (c->getFile() == f) takeClip = c.get();
        expect(lane0clip != nullptr, "lane0 clip exists");
        expect(takeClip != nullptr, "take backup exists");
        if (lane0clip && takeClip)
        {
            expect(approxEq(lane0clip->getFileOffset(), 0.0, 1e-9), "lane0 fileOffset 0");
            expect(approxEq(takeClip->getFileOffset(), 0.0, 1e-9), "take fileOffset 0");

            for (int i = 0; i < 400; ++i)
            {
                if (lane0clip->getThumbnail().isFullyLoaded()
                    && takeClip->getThumbnail().isFullyLoaded()) break;
                juce::Thread::sleep(15);
            }
            float maxDiff = 0.0f;
            for (int k = 0; k < 40; ++k)
            {
                double t0 = (k / 40.0) * 4.0, t1 = ((k + 1) / 40.0) * 4.0;
                float a0 = 0, a1 = 0, b0 = 0, b1 = 0;
                lane0clip->getThumbnail().getApproximateMinMax(t0, t1, 0, a0, a1);
                takeClip ->getThumbnail().getApproximateMinMax(t0, t1, 0, b0, b1);
                maxDiff = juce::jmax(maxDiff, std::abs(a1 - b1), std::abs(a0 - b0));
            }
            logMessage("thumb maxDiff=" + juce::String(maxDiff, 4)
                       + " lane0loaded=" + juce::String((int)lane0clip->getThumbnail().isFullyLoaded())
                       + " takeloaded=" + juce::String((int)takeClip->getThumbnail().isFullyLoaded()));
            expect(maxDiff < 0.02f, "lane0 and take thumbnails match across the whole clip");
        }
        dir.deleteRecursively();
    }

    // ── 実 WAV を一時生成 (16bit mono/220Hz) ──
    juce::File writeWav(const juce::File& dir, const juce::String& name, double sr, double secs)
    {
        auto f = dir.getChildFile(name);
        f.deleteFile();
        std::unique_ptr<juce::FileOutputStream> os(f.createOutputStream());
        if (os == nullptr) return {};
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::AudioFormatWriter> w(
            wav.createWriterFor(os.get(), sr, 1, 16, {}, 0));
        if (w == nullptr) return {};
        os.release();   // writer がストリームを所有
        const int n = (int) (sr * secs);
        juce::AudioBuffer<float> buf(1, n);
        for (int i = 0; i < n; ++i)
            buf.setSample(0, i, 0.1f * std::sin(2.0 * juce::MathConstants<double>::pi * 220.0 * i / sr));
        w->writeFromAudioSampleBuffer(buf, 0, n);
        return f;   // writer のデストラクタで flush/close
    }

    int countBackups(Track* t, const juce::File& file, double fo, double dur)
    {
        int n = 0;
        for (int li = 1; li < t->getLaneCount(); ++li)
            for (auto& c : t->getLane(li)->clips)
                if (c->getFile() == file
                    && approxEq(c->getFileOffset(), fo, 1e-3)
                    && approxEq(c->getDuration(), dur, 1e-3))
                    ++n;
        return n;
    }

    // テイクレーン (lane >= 1) 上の、指定ファイルを参照するクリップ総数 (断片も含む)
    int countClipsForFile(Track* t, const juce::File& file)
    {
        int n = 0;
        for (int li = 1; li < t->getLaneCount(); ++li)
            for (auto& c : t->getLane(li)->clips)
                if (c->getFile() == file)
                    ++n;
        return n;
    }

    // ── finishLiveRecording: 上書きされた Lane 0 の既存クリップはテイクレーンへ退避しない ──
    // (上塗り・要望 2026-07 の回帰テスト)。各録音は録音時に自分自身をバックアップするので
    // テイク履歴はそれで揃う。旧実装は重なりクリップを再退避しており、パンチインでトリム
    // 済みの断片が (file, fileOffset, duration) 違いの部分クリップとしてテイクに混入していた。
    void testFinishLiveRecordingNoDisplacedBackup()
    {
        beginTest("finishLiveRecording: overwritten lane0 clips are NOT backed up to take lanes");
        juce::AudioFormatManager fmt; fmt.registerBasicFormats();
        TrackManager tm(fmt);
        auto* t = tm.addTrack();
        auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                       .getChildFile("UtawaveRecTests");
        dir.createDirectory();
        auto wavOld  = writeWav(dir, "old.wav",  48000.0, 4.0);
        auto wavNew  = writeWav(dir, "new.wav",  48000.0, 2.0);
        auto wavNew2 = writeWav(dir, "new2.wav", 48000.0, 2.0);
        expect(wavNew.existsAsFile(), "test WAV written");

        addLane0(t, wavOld, 0.0, 4.0);             // existing recording on lane 0
        t->startLiveRecording(1.0);
        auto* rec = t->finishLiveRecording(wavNew, 1.0, 2.0);   // punch [1,3]

        expect(rec != nullptr, "finishLiveRecording returns the new clip");
        bool newInLane0 = false;
        for (auto& c : t->getLane(0)->clips) if (c.get() == rec) newInLane0 = true;
        expect(newInLane0, "new recording lives on lane 0 (punch-in continuity)");

        expect(countClipsForFile(t, wavOld) == 0,
               "overwritten lane0 clip is NOT copied to a take lane");
        expect(countBackups(t, wavNew, 0.0, 2.0) == 1,
               "the new recording itself is backed up once");

        // 2 回目のパンチイン: 1 回目でトリム済みの lane0 断片も退避されない
        // (断片が部分クリップとしてテイクに混入していた旧挙動の回帰テスト)
        t->startLiveRecording(1.5);
        t->finishLiveRecording(wavNew2, 1.5, 2.0);   // punch [1.5,3.5] over rec1 [1,3]

        expect(countClipsForFile(t, wavOld) == 0,
               "trimmed fragments of the original clip stay out of take lanes");
        expect(countClipsForFile(t, wavNew) == 1,
               "take lanes keep only recording 1's own full backup (no trimmed fragments)");
        expect(countBackups(t, wavNew, 0.0, 2.0) == 1,
               "recording 1's backup is still the untouched full take");
        expect(countBackups(t, wavNew2, 0.0, 2.0) == 1,
               "recording 2 is backed up once");

        dir.deleteRecursively();
    }

    // ── finishLiveRecording: fileOffset 指定 (録音レイテンシ補正でタイムライン 0 に
    //    クランプされた分の読み飛ばし) が lane0 クリップとテイク退避の両方へ入る ──
    void testFinishLiveRecordingWithFileOffset()
    {
        beginTest("finishLiveRecording: explicit fileOffset applied to lane0 clip and take backup");
        juce::AudioFormatManager fmt; fmt.registerBasicFormats();
        TrackManager tm(fmt);
        auto* t = tm.addTrack();
        auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                       .getChildFile("UtawaveRecTests4");
        dir.createDirectory();
        auto wavNew = writeWav(dir, "new.wav", 48000.0, 2.0);

        t->startLiveRecording(0.0);
        auto* rec = t->finishLiveRecording(wavNew, 0.0, 1.95, /*fileOffset=*/0.05);

        expect(rec != nullptr, "finishLiveRecording returns the new clip");
        if (rec)
            expect(approxEq(rec->getFileOffset(), 0.05, 1e-6),
                   "lane0 clip skips the clamped head via fileOffset");
        expect(countBackups(t, wavNew, 0.05, 1.95) >= 1,
               "take backup carries the same (file, fileOffset, duration) tuple");

        dir.deleteRecursively();
    }

    // ── 録音レイテンシ補正の配置計算 (RecordingManager::compensateLatency, 純関数) ──
    void testLatencyCompensation()
    {
        beginTest("compensateLatency: shift earlier / clamp at zero / negative comp");

        // 補正 0 は恒等
        {
            const auto p = RecordingManager::compensateLatency(2.0, 4.0, 0.5, 0.0);
            expect(approxEq(p.start, 2.0, 1e-9) && approxEq(p.dur, 4.0, 1e-9)
                   && approxEq(p.fileOffset, 0.5, 1e-9), "comp 0 is identity");
        }
        // 通常: 開始だけ手前へ (尺・fileOffset 不変)
        {
            const auto p = RecordingManager::compensateLatency(2.0, 4.0, 0.0, 0.020);
            expect(approxEq(p.start, 1.98, 1e-9), "start shifted earlier by comp");
            expect(approxEq(p.dur, 4.0, 1e-9), "duration unchanged");
            expect(approxEq(p.fileOffset, 0.0, 1e-9), "fileOffset unchanged");
        }
        // タイムライン 0 から録音: 0 にクランプし、不足分は fileOffset 読み飛ばし + 尺短縮
        {
            const auto p = RecordingManager::compensateLatency(0.0, 4.0, 0.0, 0.050);
            expect(approxEq(p.start, 0.0, 1e-9), "start clamped at timeline zero");
            expect(approxEq(p.fileOffset, 0.050, 1e-9), "clamped amount skipped via fileOffset");
            expect(approxEq(p.dur, 3.95, 1e-9), "duration reduced by the clamped amount");
        }
        // 部分クランプ: start < comp の分だけ fileOffset へ
        {
            const auto p = RecordingManager::compensateLatency(0.03, 4.0, 0.0, 0.050);
            expect(approxEq(p.start, 0.0, 1e-9), "start clamped");
            expect(approxEq(p.fileOffset, 0.020, 1e-9), "residual only goes to fileOffset");
            expect(approxEq(p.dur, 3.98, 1e-9), "duration reduced by residual only");
        }
        // 既存 fileOffset (遡及録音のパンチ) に加算される
        {
            const auto p = RecordingManager::compensateLatency(0.01, 2.0, 1.5, 0.030);
            expect(approxEq(p.fileOffset, 1.52, 1e-9), "residual added on top of base fileOffset");
            expect(approxEq(p.dur, 1.98, 1e-9), "duration reduced by residual");
        }
        // 負の補正 (手動オフセットで遅らせる) は単純に後ろへ
        {
            const auto p = RecordingManager::compensateLatency(1.0, 2.0, 0.0, -0.010);
            expect(approxEq(p.start, 1.01, 1e-9), "negative comp shifts later");
            expect(approxEq(p.dur, 2.0, 1e-9) && approxEq(p.fileOffset, 0.0, 1e-9),
                   "duration and fileOffset unchanged for negative comp");
        }
        // 補正がクリップ全長を超える病的ケースでも dur は負にならない
        {
            const auto p = RecordingManager::compensateLatency(0.0, 0.02, 0.0, 0.050);
            expect(p.dur >= 0.0, "duration never goes negative");
        }
    }

    // ── レイテンシ補正のアンカークランプ (floorStart) ──
    // 録音の配置はアンカー (R 押下位置 / ループ頭) を floorStart に渡し、クリップ左端が
    // 補正量ぶんアンカーより前へ飛び出さないようにする (見た目が再生バーに揃う)。
    // 補正分は fileOffset へ乗るので内容の時間整合は保たれ、左端を伸ばせば復元できる
    void testLatencyCompensationFloor()
    {
        beginTest("compensateLatency floorStart: clip head clamps to the musical anchor");

        // アンカー = start (録音の全配置経路と同じ渡し方): 開始はアンカーに留まり、
        // 補正分は fileOffset 読み飛ばし + 尺短縮になる
        {
            const auto p = RecordingManager::compensateLatency(10.0, 4.0, 0.5, 0.030, 10.0);
            expect(approxEq(p.start, 10.0, 1e-9), "start stays at the anchor");
            expect(approxEq(p.fileOffset, 0.53, 1e-9), "comp goes into fileOffset");
            expect(approxEq(p.dur, 3.97, 1e-9), "duration reduced by comp");
        }
        // 負の補正 (手動で遅らせる) はアンカーより後ろなのでクランプしない
        {
            const auto p = RecordingManager::compensateLatency(10.0, 4.0, 0.0, -0.020, 10.0);
            expect(approxEq(p.start, 10.02, 1e-9) && approxEq(p.fileOffset, 0.0, 1e-9)
                   && approxEq(p.dur, 4.0, 1e-9), "negative comp is not clamped");
        }
        // floorStart 省略 (= 0) は従来挙動: 手前へずらしたまま
        {
            const auto p = RecordingManager::compensateLatency(10.0, 4.0, 0.0, 0.030);
            expect(approxEq(p.start, 9.97, 1e-9), "default floor keeps the shifted position");
        }
        // 負の floorStart はタイムライン 0 にクランプ
        {
            const auto p = RecordingManager::compensateLatency(0.01, 4.0, 0.0, 0.050, -5.0);
            expect(approxEq(p.start, 0.0, 1e-9), "floor never goes below timeline zero");
        }
    }

    // ── LiveRecordingBuffer ──
    void testLiveRecordingBuffer()
    {
        beginTest("LiveRecordingBuffer: deferred alloc, peak accumulation, abs, duration, reset");
        LiveRecordingBuffer buf;

        // reset 前: ピーク無し・pushSamples は no-op (peaksPtr == nullptr)
        expect(buf.getPeakCount() == 0, "no peaks before reset");
        std::vector<float> pre(1024, 0.5f);
        buf.pushSamples(pre.data(), (int) pre.size());
        expect(buf.getPeakCount() == 0, "pushSamples before reset is a no-op");

        buf.reset();
        expect(buf.getPeakCount() == 0, "0 peaks right after reset");

        // samplesPerPeak * 3 サンプル (0.5) → 3 ピーク、各 0.5
        const int spp = LiveRecordingBuffer::samplesPerPeak;
        std::vector<float> d((size_t) spp * 3, 0.5f);
        buf.pushSamples(d.data(), (int) d.size());
        expect(buf.getPeakCount() == 3, "3 peaks after 3 * samplesPerPeak");
        expect(approxEq(buf.getPeak(0), 0.5, 1e-6), "peak = abs max of block");

        // 負値 → abs で蓄積
        std::vector<float> dn((size_t) spp, -0.8f);
        buf.pushSamples(dn.data(), (int) dn.size());
        expect(buf.getPeakCount() == 4, "4th peak appended");
        expect(approxEq(buf.getPeak(3), 0.8, 1e-6), "peak uses absolute value");

        // 尺 = peaks * samplesPerPeak / sampleRate
        expect(approxEq(buf.getDurationSeconds(48000.0), 4.0 * spp / 48000.0, 1e-9),
               "duration = peakCount * samplesPerPeak / sampleRate");

        // reset でクリア
        buf.reset();
        expect(buf.getPeakCount() == 0, "reset clears peaks");
    }

    // ── ループ録音のテイクスライス (RecordingManager::loopTakeSlice 純関数) ──
    // onLoopWrap / 停止時スライスの共通式。ループ内途中スタートとカウントイン遡及録音の
    // fileOffset 回帰テスト (ずれると「途中から録音されたような波形」になる)
    void testLoopTakeSlice()
    {
        beginTest("loopTakeSlice: mid-loop start / pre-loop start / count-in lead offsets");
        using RM = RecordingManager;

        // ループ内の途中 (8s) から開始、ループ [5, 10]、カウントイン無し
        // 1 周目 = [8, 10] (2s)、ファイル先頭から
        {
            auto s0 = RM::loopTakeSlice(0, 8.0, 8.0, 5.0, 10.0);
            expect(approxEq(s0.pos, 8.0, 1e-9) && approxEq(s0.dur, 2.0, 1e-9)
                   && approxEq(s0.fileOffset, 0.0, 1e-9), "take1 = [start, loopEnd], offset 0");
            // 2 周目以降はループ頭フル尺。offset は 1 周目の実録音長 (2s) 基準で累積
            auto s1 = RM::loopTakeSlice(1, 8.0, 8.0, 5.0, 10.0);
            expect(approxEq(s1.pos, 5.0, 1e-9) && approxEq(s1.dur, 5.0, 1e-9)
                   && approxEq(s1.fileOffset, 2.0, 1e-9), "take2 at loop head, offset = firstPass");
            auto s2 = RM::loopTakeSlice(2, 8.0, 8.0, 5.0, 10.0);
            expect(approxEq(s2.fileOffset, 7.0, 1e-9), "take3 offset += loopDur");
        }

        // ループ開始前 (3s) から開始 (pre-loop warm-up): 1 周目 = [3, 10] (7s) 統合テイク
        {
            auto s0 = RM::loopTakeSlice(0, 3.0, 3.0, 5.0, 10.0);
            expect(approxEq(s0.pos, 3.0, 1e-9) && approxEq(s0.dur, 7.0, 1e-9)
                   && approxEq(s0.fileOffset, 0.0, 1e-9), "pre-loop start: take1 includes warm-up");
            auto s1 = RM::loopTakeSlice(1, 3.0, 3.0, 5.0, 10.0);
            expect(approxEq(s1.fileOffset, 7.0, 1e-9), "take2 offset = preLoop + loopDur");
        }

        // カウントイン遡及録音: 再生開始 6s (= fileStartPos)、R 押下 8s。先行録音 2s が
        // fileOffset に乗る (クリップ左端を伸ばすとブレスを復元できる量)
        {
            auto s0 = RM::loopTakeSlice(0, 8.0, 6.0, 5.0, 10.0);
            expect(approxEq(s0.pos, 8.0, 1e-9) && approxEq(s0.dur, 2.0, 1e-9)
                   && approxEq(s0.fileOffset, 2.0, 1e-9), "count-in lead lands in take1 fileOffset");
            auto s1 = RM::loopTakeSlice(1, 8.0, 6.0, 5.0, 10.0);
            expect(approxEq(s1.fileOffset, 4.0, 1e-9), "take2 offset = lead + firstPass");
            auto s2 = RM::loopTakeSlice(2, 8.0, 6.0, 5.0, 10.0);
            expect(approxEq(s2.fileOffset, 9.0, 1e-9), "take3 offset = lead + firstPass + loopDur");
        }
    }

    // ── ライブ波形オーバーレイのリード (カウントイン/プリロールの非表示先行録音分) ──
    void testLiveRecordingLead()
    {
        beginTest("startLiveRecording: display start + hidden buffer lead");
        juce::AudioFormatManager fmt;
        fmt.registerBasicFormats();
        TrackManager tm(fmt);
        auto* t = tm.addTrack({}, false);

        // カウントイン 2s: 表示開始は R 位置 (8s)、バッファ先頭 2s は非表示リード
        t->startLiveRecording(8.0, 2.0);
        expect(approxEq(t->getRecordingStartPos(), 8.0, 1e-9), "display start = R position");
        expect(approxEq(t->getLiveBufferLeadSecs(), 2.0, 1e-9), "lead = count-in duration");

        // ループラップ後相当: 表示をループ頭へ、リードを差し替え
        // (実運用の値は RecordingManager が決める。補正込みの値は下の
        //  testLiveOverlayLeadIncludesComp で検証)
        t->setRecordingStartPos(5.0);
        t->setLiveBufferLeadSecs(0.0);
        expect(approxEq(t->getRecordingStartPos(), 5.0, 1e-9), "display start moves to loop head");
        expect(approxEq(t->getLiveBufferLeadSecs(), 0.0, 1e-9), "lead replaced after wrap");

        // 負のリードは保持される (負のレイテンシ補正 = 内容をその分遅い位置に描き、
        // 確定クリップの後ろずらし配置と一致させる) / リード無し開始は 0
        t->setLiveBufferLeadSecs(-1.0);
        expect(approxEq(t->getLiveBufferLeadSecs(), -1.0, 1e-9),
               "negative lead preserved (content drawn later)");
        t->startLiveRecording(3.0);
        expect(approxEq(t->getLiveBufferLeadSecs(), 0.0, 1e-9), "default start has no lead");
    }

    // ── ライブ波形リードにレイテンシ補正が乗る (RecordingManager 経由・改修 2026-07) ──
    // 確定クリップの fileOffset には補正量 comp が乗る (頭がトリムされる) ため、録音中の
    // 表示リードにも同じ comp を足す。これで録音中と確定後の波形位置が一致する。
    // 3 経路: 通常録音開始 (preRec + comp) / ループラップ後 (comp) /
    // Punch From Retro (retro 開始時にスナップショットした retroLatencyComp)。
    // AudioEngine はデバイス無しで構築し、補正は手動オフセット (auto OFF) で与える。
    void testLiveOverlayLeadIncludesComp()
    {
        beginTest("live overlay lead includes latency comp (normal / wrap / punch-from-retro)");
        juce::AudioFormatManager fmt; fmt.registerBasicFormats();
        TrackManager tm(fmt);
        AudioEngine engine;
        RecordingManager rm(engine, tm, fmt);

        auto tmpDir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                          .getChildFile("UtawaveRecLeadTest");
        tmpDir.deleteRecursively();
        tmpDir.createDirectory();
        rm.getAudioFolder = [tmpDir] { return tmpDir; };

        auto* t = tm.addTrack({}, false);
        t->setRecArmed(true);

        // ── 通常録音: lead = 先行録音分 (preRec) + comp ──
        engine.setRecordingLatencyComp(false, 20.0);   // comp = 0.020s (手動のみ)
        bool ok = rm.startRecording(1.0, 0.5);          // R=1.0 / 再生 0.5 から (preRec 0.5)
        expect(ok, "normal recording starts");
        expect(approxEq(t->getRecordingStartPos(), 1.0, 1e-9), "display start = R position");
        expect(approxEq(t->getLiveBufferLeadSecs(), 0.52, 1e-9), "lead = preRec + comp");
        rm.stopRecording(1.005);   // 極小尺 → クリップ配置なしで終了

        // ── 負の comp は負リードとして保持 (内容を遅く描き、確定クリップの
        //    後ろずらし配置と一致させる) ──
        engine.setRecordingLatencyComp(false, -10.0);
        ok = rm.startRecording(2.0, 2.0);
        expect(ok, "recording starts with negative comp");
        expect(approxEq(t->getLiveBufferLeadSecs(), -0.01, 1e-9),
               "negative comp becomes negative lead");
        rm.stopRecording(2.005);

        // ── ループ録音: ラップ後の lead は comp (カウントインのリードは落ちるが補正は残る) ──
        engine.setRecordingLatencyComp(false, 30.0);   // comp = 0.030s
        ok = rm.startRecording(6.0, 6.0, true, 5.0, 10.0);
        expect(ok, "loop recording starts");
        expect(approxEq(t->getLiveBufferLeadSecs(), 0.03, 1e-9), "loop take1 lead = comp");
        rm.onLoopWrap();
        expect(approxEq(t->getRecordingStartPos(), 5.0, 1e-9), "after wrap display at loop head");
        expect(approxEq(t->getLiveBufferLeadSecs(), 0.03, 1e-9), "after wrap lead = comp (not 0)");
        rm.stopRecording(6.01);

        // ── Punch From Retro: lead = retro 開始時のスナップショット (R 押下時の値ではない) ──
        engine.setRecordingLatencyComp(false, 40.0);   // retro 開始時 comp = 0.040s
        ok = rm.startRetrospective(t, 2.0);
        expect(ok, "retrospective starts");
        engine.setRecordingLatencyComp(false, 10.0);   // R 押下時は別の値に変えておく
        ok = rm.startRecording(3.0, 3.0);              // retro アクティブ + アーム済み → パンチ
        expect(ok, "punch-from-retro starts");
        expect(approxEq(t->getRecordingStartPos(), 3.0, 1e-9), "punch display start = R position");
        expect(approxEq(t->getLiveBufferLeadSecs(), 0.04, 1e-9),
               "punch lead = retro comp snapshot");
        rm.stopRecording(3.005);

        tmpDir.deleteRecursively();
    }

    // ── 空きテイクレーン検索 (Track::findFreeTakeLaneIndex) ──
    void testFindFreeTakeLane()
    {
        beginTest("findFreeTakeLaneIndex: reuse first free lane / minLaneIdx / create new");
        juce::AudioFormatManager fmt; fmt.registerBasicFormats();
        TrackManager tm(fmt);
        auto* t = tm.addTrack();
        const juce::File dummy("/tmp/utawave_takelane.wav");

        // lane1 = [5,10] のクリップ持ち / lane2 = 空
        t->ensureLane(2);
        t->getLane(1)->addClip(dummy, 5.0, 5.0, t->getFormatManager(), t->getThumbnailCache());

        expect(t->findFreeTakeLaneIndex(5.0, 10.0) == 2, "overlapping lane1 skipped -> lane2");
        expect(t->findFreeTakeLaneIndex(12.0, 13.0) == 1, "non-overlapping span reuses lane1");
        expect(t->findFreeTakeLaneIndex(10.0, 12.0) == 1, "touching boundary (end==start) is free");

        // minLaneIdx で下方向を強制 (空きの lane2 を飛ばして新規 lane3)
        const int before = t->getLaneCount();
        const int idx = t->findFreeTakeLaneIndex(5.0, 10.0, 3);
        expect(idx == 3 && t->getLaneCount() == before + 1, "minLaneIdx forces new lane below");

        // minLaneIdx < 1 は 1 に切り上げ (Lane 0 はテイクレーンにしない)
        expect(t->findFreeTakeLaneIndex(12.0, 13.0, -5) == 1, "minLaneIdx clamps to 1");
    }

    // ── ループ録音のテイクレーン再利用 (改修 2026-07 の回帰テスト) ──
    // 旧実装は「クリップを持つ最後のレーンの次」から採番していたため、ループ範囲外に
    // クリップがあるだけの空きレーンを飛ばして毎回新規レーンを作っていた。新実装は
    // 通常録音の backupToTakeLane と同じく重ならない既存レーンを再利用する。
    // セッション内では lastTakeLaneIdx + 1 から探すので必ず下方向へ積まれる
    // (テイクの上から下 = 時系列順・Undo のレーン昇順前提を維持)。
    void testLoopTakeLaneReuse()
    {
        beginTest("loop recording reuses free take lanes before creating new ones");
        juce::AudioFormatManager fmt; fmt.registerBasicFormats();
        TrackManager tm(fmt);
        AudioEngine engine;
        RecordingManager rm(engine, tm, fmt);

        auto tmpDir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                          .getChildFile("UtawaveTakeLaneTest");
        tmpDir.deleteRecursively();
        tmpDir.createDirectory();
        rm.getAudioFolder = [tmpDir] { return tmpDir; };

        auto* t = tm.addTrack({}, false);
        t->setRecArmed(true);
        const juce::File dummy("/tmp/utawave_takelane2.wav");

        // lane1 = ループ範囲外 [20,21] のみ (= ループ録音にとって空き) /
        // lane2 = ループ範囲内 [5,8] を持つ (= 使用中)
        t->ensureLane(2);
        t->getLane(1)->addClip(dummy, 20.0, 1.0, t->getFormatManager(), t->getThumbnailCache());
        t->getLane(2)->addClip(dummy, 5.0, 3.0, t->getFormatManager(), t->getThumbnailCache());

        // ループ [5,10]・6.0 から録音開始 (comp はデバイス無し + 手動 0 = 0)
        bool ok = rm.startRecording(6.0, 6.0, true, 5.0, 10.0);
        expect(ok, "loop recording starts");

        // Take 1 [6,10) はループ範囲外クリップしかない lane1 を再利用 (旧実装は lane3 新規)
        rm.onLoopWrap();
        expect(t->getLaneCount() == 3, "take1 reuses lane1 (no new lane)");
        expect((int) t->getLane(1)->clips.size() == 2, "take1 lands in lane1");
        expect(clipAtStart(t->getLane(1), 6.0) != nullptr, "take1 at recording start");

        // Take 2 [5,10) は lane2 が使用中なので新規 lane3 へ (下方向へ積む)
        rm.onLoopWrap();
        expect(t->getLaneCount() == 4, "take2 creates lane3");
        expect((int) t->getLane(3)->clips.size() == 1, "take2 lands in new lane3");
        auto* take2 = clipAtStart(t->getLane(3), 5.0);
        expect(take2 != nullptr && approxEq(take2->getDuration(), 5.0, 1e-6)
               && approxEq(take2->getFileOffset(), 4.0, 1e-6),
               "take2 geometry: loop head, full loop dur, offset = first pass");
        expect((int) t->getLane(2)->clips.size() == 1, "busy lane2 untouched");

        rm.stopRecording(10.0);
        tmpDir.deleteRecursively();
    }

    // ── ループ録音停止で最後のテイクが Lane 0 にも置かれる (要望 2026-07 の回帰テスト) ──
    // 旧実装はテイクレーンに入るだけで Lane 0 が空のままだった。新実装は最後に配置した
    // テイクと同内容 (file/start/dur/fileOffset) を Lane 0 へ置き、パンチインと同じ作法
    // (trimAndCrossfadeOnLane0) で既存クリップをトリムする。
    void testLoopLastTakeOnLane0()
    {
        beginTest("loop recording places the last take on lane0 at stop");
        juce::AudioFormatManager fmt; fmt.registerBasicFormats();
        TrackManager tm(fmt);
        AudioEngine engine;
        RecordingManager rm(engine, tm, fmt);

        auto tmpDir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                          .getChildFile("UtawaveLane0TakeTest");
        tmpDir.deleteRecursively();
        tmpDir.createDirectory();
        rm.getAudioFolder = [tmpDir] { return tmpDir; };

        auto* t = tm.addTrack({}, false);
        t->setRecArmed(true);
        const juce::File dummy("/tmp/utawave_lane0take.wav");

        // Lane 0 に既存クリップ [0,20] (オケ的な下地) を置いておく → 停止時にトリムされる
        auto* pre = addLane0(t, dummy, 0.0, 20.0);
        expect(pre != nullptr, "pre-existing lane0 clip");

        // ループ [5,10]・6.0 から録音開始、2 周分をリアルタイム配置 (comp = 0)
        bool ok = rm.startRecording(6.0, 6.0, true, 5.0, 10.0);
        expect(ok, "loop recording starts");
        rm.onLoopWrap();   // take1 [6,10)
        rm.onLoopWrap();   // take2 [5,10) fileOffset 4
        rm.stopRecording(10.0);

        // Lane 0: 最後のテイク (take2) と同ジオメトリのクリップが置かれ、既存クリップは
        // パンチイン同様に左右へトリムされる (左 [0,~5.03] / take2 [5,10) / 右 [~9.97,20])
        auto* lane0 = t->getLane(0);
        expect((int) lane0->clips.size() == 3, "lane0 = left piece + last take + right piece");
        auto* placed = clipAtStart(lane0, 5.0);
        expect(placed != nullptr && approxEq(placed->getDuration(), 5.0, 1e-6)
               && approxEq(placed->getFileOffset(), 4.0, 1e-6),
               "lane0 clip = last take geometry (loop head, loop dur, offset = first pass)");
        auto* left = clipAtStart(lane0, 0.0);
        expect(left != nullptr && left->getEndPosition() < 5.0 + kXfade + 1e-6,
               "left piece trimmed to punch boundary");

        // テイクレーン側にも同じテイクが残っている (lane0 への配置はコピー)
        auto* take2 = clipAtStart(t->getLane(2), 5.0);
        expect(take2 != nullptr && approxEq(take2->getFileOffset(), 4.0, 1e-6),
               "take2 remains in take lane");

        tmpDir.deleteRecursively();
    }
    // ── Q リテイク (破棄): 通常録音の discardRecording ──
    // クリップ/テイク/録音ファイルを一切残さず、既存 Lane 0 は触らない。
    // getActiveRecordStartPos (リテイクの戻り先) の録音中/非録音中の値も固定する
    void testDiscardRecordingNormal()
    {
        beginTest("discardRecording: normal recording leaves no clip / take / file");
        juce::AudioFormatManager fmt; fmt.registerBasicFormats();
        TrackManager tm(fmt);
        AudioEngine engine;
        RecordingManager rm(engine, tm, fmt);

        auto tmpDir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                          .getChildFile("UtawaveDiscardNormalTest");
        tmpDir.deleteRecursively();
        tmpDir.createDirectory();
        rm.getAudioFolder = [tmpDir] { return tmpDir; };

        auto* t = tm.addTrack({}, false);
        t->setRecArmed(true);
        const juce::File dummy("/tmp/utawave_discard_pre.wav");
        addLane0(t, dummy, 0.0, 10.0);   // 既存の下地 (discard で触らないこと)

        expect(approxEq(rm.getActiveRecordStartPos(9.9), 9.9, 1e-9), "anchor fallback when idle");

        expect(rm.startRecording(2.0, 2.0), "recording starts");
        expect(rm.isRecording(), "recording flag on");
        expect(t->hasLiveRecording(), "live overlay armed");
        expect(tmpDir.getNumberOfChildFiles(juce::File::findFiles, "*.wav") == 1,
               "recording file created");
        expect(approxEq(rm.getActiveRecordStartPos(9.9), 2.0, 1e-9),
               "anchor = record start while recording");

        rm.discardRecording();
        expect(!rm.isRecording(), "recording flag off after discard");
        expect(!t->hasLiveRecording(), "live overlay cancelled");
        expect(t->getLaneCount() == 1 && (int) t->getLane(0)->clips.size() == 1,
               "lane0 untouched and no take lanes added");
        expect(tmpDir.getNumberOfChildFiles(juce::File::findFiles, "*.wav") == 0,
               "recording file deleted");
        expect(approxEq(rm.getActiveRecordStartPos(9.9), 9.9, 1e-9), "anchor fallback after discard");

        tmpDir.deleteRecursively();
    }

    // ── Q リテイク (破棄): ループ録音のリアルタイム配置テイクもレーンから取り除く ──
    // 外したクリップは遅延破棄 (deferClipDestruction) 経由なのでエンジンが延命所有する
    void testDiscardRecordingLoop()
    {
        beginTest("discardRecording: loop recording removes realtime takes and file");
        juce::AudioFormatManager fmt; fmt.registerBasicFormats();
        TrackManager tm(fmt);
        AudioEngine engine;
        RecordingManager rm(engine, tm, fmt);

        auto tmpDir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                          .getChildFile("UtawaveDiscardLoopTest");
        tmpDir.deleteRecursively();
        tmpDir.createDirectory();
        rm.getAudioFolder = [tmpDir] { return tmpDir; };

        auto* t = tm.addTrack({}, false);
        t->setRecArmed(true);

        expect(rm.startRecording(6.0, 6.0, true, 5.0, 10.0), "loop recording starts");
        rm.onLoopWrap();   // take1 [6,10) -> lane1
        rm.onLoopWrap();   // take2 [5,10) -> lane2
        expect(t->getLaneCount() == 3
               && (int) t->getLane(1)->clips.size() == 1
               && (int) t->getLane(2)->clips.size() == 1,
               "realtime takes placed on take lanes");

        rm.discardRecording();
        expect(!rm.isRecording(), "recording flag off after discard");
        expect(t->getLane(1)->clips.empty() && t->getLane(2)->clips.empty(),
               "realtime takes removed from lanes");
        expect(t->getLane(0)->clips.empty(), "lane0 stays empty");
        expect(tmpDir.getNumberOfChildFiles(juce::File::findFiles, "*.wav") == 0,
               "recording file deleted");

        tmpDir.deleteRecursively();
    }

    // ── Q リテイク (テイクを残す): 通常録音の takesOnly 停止 ──
    // テイクレーンにだけ確定し、Lane 0 は配置もトリムもしない
    void testStopTakesOnlyNormal()
    {
        beginTest("stopRecording(takesOnly): normal recording -> take lane only, lane0 untouched");
        juce::AudioFormatManager fmt; fmt.registerBasicFormats();
        TrackManager tm(fmt);
        AudioEngine engine;
        RecordingManager rm(engine, tm, fmt);

        auto tmpDir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                          .getChildFile("UtawaveTakesOnlyNormalTest");
        tmpDir.deleteRecursively();
        tmpDir.createDirectory();
        rm.getAudioFolder = [tmpDir] { return tmpDir; };

        auto* t = tm.addTrack({}, false);
        t->setRecArmed(true);
        const juce::File dummy("/tmp/utawave_takesonly_pre.wav");
        addLane0(t, dummy, 0.0, 10.0);   // 既存の下地 (トリムされないこと)

        expect(rm.startRecording(2.0, 2.0), "recording starts");
        rm.stopRecording(4.0, /*takesOnly*/ true);

        expect(!rm.isRecording(), "recording flag off");
        expect(!t->hasLiveRecording(), "live overlay cancelled");
        auto* lane0 = t->getLane(0);
        expect((int) lane0->clips.size() == 1, "lane0 clip count unchanged");
        auto* pre = clipAtStart(lane0, 0.0);
        expect(pre != nullptr && approxEq(pre->getDuration(), 10.0, 1e-9),
               "lane0 clip untrimmed (no punch placement)");
        expect(t->getLaneCount() >= 2, "take lane exists");
        auto* take = clipAtStart(t->getLane(1), 2.0);
        expect(take != nullptr && approxEq(take->getDuration(), 2.0, 1e-6)
               && approxEq(take->getFileOffset(), 0.0, 1e-9),
               "take = [2,4) with fileOffset 0 (same formula as normal stop)");
        expect(tmpDir.getNumberOfChildFiles(juce::File::findFiles, "*.wav") == 1,
               "recording file kept (referenced by the take)");

        tmpDir.deleteRecursively();
    }

    // ── Q リテイク (テイクを残す): ループ録音は「最後のテイクの Lane 0 配置」だけを省く ──
    void testStopTakesOnlyLoop()
    {
        beginTest("stopRecording(takesOnly): loop recording keeps takes, skips lane0 copy");
        juce::AudioFormatManager fmt; fmt.registerBasicFormats();
        TrackManager tm(fmt);
        AudioEngine engine;
        RecordingManager rm(engine, tm, fmt);

        auto tmpDir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                          .getChildFile("UtawaveTakesOnlyLoopTest");
        tmpDir.deleteRecursively();
        tmpDir.createDirectory();
        rm.getAudioFolder = [tmpDir] { return tmpDir; };

        auto* t = tm.addTrack({}, false);
        t->setRecArmed(true);
        const juce::File dummy("/tmp/utawave_takesonly_loop_pre.wav");
        addLane0(t, dummy, 0.0, 20.0);   // 既存の下地 (トリムされないこと)

        expect(rm.startRecording(6.0, 6.0, true, 5.0, 10.0), "loop recording starts");
        rm.onLoopWrap();   // take1 [6,10)
        rm.onLoopWrap();   // take2 [5,10) fileOffset 4
        rm.stopRecording(10.0, /*takesOnly*/ true);

        auto* lane0 = t->getLane(0);
        expect((int) lane0->clips.size() == 1, "lane0 untouched (no last-take copy)");
        auto* pre = clipAtStart(lane0, 0.0);
        expect(pre != nullptr && approxEq(pre->getDuration(), 20.0, 1e-9),
               "lane0 clip untrimmed");
        expect(clipAtStart(t->getLane(1), 6.0) != nullptr, "take1 remains on take lane");
        auto* take2 = clipAtStart(t->getLane(2), 5.0);
        expect(take2 != nullptr && approxEq(take2->getFileOffset(), 4.0, 1e-6),
               "take2 remains with loop-slice geometry");

        tmpDir.deleteRecursively();
    }

    // ── Punch From Retro: takesOnly はテイクレーンのみ / discard は retro ファイルごと破棄 ──
    void testPunchFromRetroTakesOnlyAndDiscard()
    {
        beginTest("punch-from-retro: takesOnly keeps take lane only, discard deletes retro file");
        juce::AudioFormatManager fmt; fmt.registerBasicFormats();
        TrackManager tm(fmt);
        AudioEngine engine;
        RecordingManager rm(engine, tm, fmt);

        auto tmpDir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                          .getChildFile("UtawavePunchRetroQTest");
        tmpDir.deleteRecursively();
        tmpDir.createDirectory();
        rm.getAudioFolder = [tmpDir] { return tmpDir; };

        auto* t = tm.addTrack({}, false);
        t->setRecArmed(true);

        // takesOnly: 遡及キャプチャ (再生 1.0〜) → 3.0 でパンチ → Q (テイクを残す)
        expect(rm.startRetrospective(t, 1.0), "retro capture starts");
        expect(rm.startRecording(3.0, 3.0), "punch from retro starts");
        expect(approxEq(rm.getActiveRecordStartPos(0.0), 3.0, 1e-9), "anchor = punch position");
        rm.stopRecording(5.0, /*takesOnly*/ true);
        expect(!rm.isRecording() && !rm.hasRetrospective(), "recording and retro ended");
        expect(t->getLane(0)->clips.empty(), "lane0 untouched (no punch placement)");
        auto* take = clipAtStart(t->getLane(1), 3.0);
        expect(take != nullptr && approxEq(take->getDuration(), 2.0, 1e-6)
               && approxEq(take->getFileOffset(), 2.0, 1e-6),
               "take = [3,5) with fileOffset = recStart - playStart");
        expect(tmpDir.getNumberOfChildFiles(juce::File::findFiles, "*.wav") == 1,
               "retro file kept (referenced by the take)");

        // discard: 2 回目のパンチを破棄 → 新しいクリップは増えず 2 つ目の retro ファイルも消える
        expect(rm.startRetrospective(t, 6.0), "retro capture restarts");
        expect(rm.startRecording(8.0, 8.0), "second punch starts");
        rm.discardRecording();
        expect(!rm.isRecording() && !rm.hasRetrospective(), "second punch fully discarded");
        expect((int) t->getLane(1)->clips.size() == 1 && t->getLane(0)->clips.empty(),
               "no new clips from the discarded punch");
        expect(tmpDir.getNumberOfChildFiles(juce::File::findFiles, "*.wav") == 1,
               "second retro file deleted, first kept");

        tmpDir.deleteRecursively();
    }
};

static RecordingTests recordingTests;
}
