// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

// TimelineView の波形クリップ右クリックメニュー (構築 + showMenuAsync 結果ハンドラ)。
// TimelineView_Mouse.cpp の mouseDown から抽出した showAudioClipContextMenu の実体。
// メニュー項目を追加する時はここを編集する (ID 体系はメソッド先頭のコメント参照)。

#include "TimelineView.h"
#include "../Localisation.h"
#include "../AppColours.h"
#include "../Edit/SilenceDetector.h"
#include "../Audio/BpmDetector.h"
#include "../Audio/LufsMeter.h"
#include "../Audio/PitchEngine.h"
#include <set>

// 波形クリップの右クリックメニュー (mouseDown から抽出)。メニュー構築と showMenuAsync の
// 結果ハンドラ一式。項目 ID 体系: 100-107=色 / 300 番台=編集操作 / 500 番台=キー変更 / 400=削除
void TimelineView::showAudioClipContextMenu(const ClipRef& rcRef, const juce::MouseEvent& e)
{
    grabKeyboardFocus();
    // 右クリックしたクリップを選択
    if (!isClipInSelection(rcRef.clip))
    {
        clearAllSelections();
        selectedClip = rcRef;
        repaint();
        notifySelectionChanged();  // 右クリック選択確定 → 採用ボタン活性を更新
    }

    juce::PopupMenu m;
    // J ラムダはこの関数の後段で定義されているため、ここでローカルに用意する

    // Take レーン (laneIdx > 0) のクリップ → 右クリックメニューからの「このテイクを使う」(ID 321) は
    // 一時的に非表示にしている。「選択範囲 + 波形クリップを選択中」にこのメニューから採用すると、
    // 採用自体と Undo 記録 (LaneSnapshotAction) はデータ上正しく行われるのに、まれに Undo しても
    // 見た目が戻らない (再現条件が不安定) 不具合があったため。テイク採用はトラックヘッダの ↑ ボタンと
    // Shift+↑ ショートカット (どちらも promoteTakeLane 経由・この症状が出ない) に一本化する。
    // 原因を特定できたら m.addItem(321, ...) を復活させる (result==321 のハンドラは残してある)。

    // 色変更サブメニュー
    juce::PopupMenu colourMenu;
    const std::array<std::pair<const char*, juce::Colour>, 8> palette = {{
        {"Blue",   juce::Colour(0xff3a6ea5)},
        {"Green",  juce::Colour(0xff5aa55a)},
        {"Red",    juce::Colour(0xffa55a5a)},
        {"Orange", juce::Colour(0xffa5925a)},
        {"Purple", juce::Colour(0xff7a5aa5)},
        {"Cyan",   juce::Colour(0xff5a9ea5)},
        {"Pink",   juce::Colour(0xffa55a92)},
        {"Steel",  juce::Colour(0xff5a7aa5)}
    }};
    for (size_t i = 0; i < palette.size(); ++i)
    {
        juce::PopupMenu::Item item;
        item.itemID = 100 + (int)i;
        item.text = palette[i].first;
        item.colour = palette[i].second;
        colourMenu.addItem(item);
    }
    m.addSubMenu("Clip Colour", colourMenu);

    // フェードカーブは初心者向けに既定 (リニア = 綺麗な直線の X) に固定し、
    // 種別選択 UI (リニア/対数/等パワー/S字) は出さない (細かい設定を隠す)。

    m.addSeparator();
    int extraCount = (int)extraSelections.size() + (selectedClip.valid() ? 1 : 0);
    m.addItem(300, tr(u8"クリップ結合 (Consolidate)"),
              extraCount >= 2);
    // クロスフェードを描く: 「範囲選択あり」かつ「その範囲内で 2 つのクリップが
    // 重なり/接触している (= クロスフェード可能な箇所がある)」時だけメニューに出す。
    // (クリップを選んだだけ・範囲選択なしのときは出さない)
    if (hasSelectionRange())
    {
        const double selS = loopStartTV;
        const double selE = loopEndTV;
        const double tol  = 0.005;
        bool junction = false;
        for (int ti = 0; ti < trackManager.getTrackCount() && !junction; ++ti)
        {
            auto* track = trackManager.getTrack(ti);
            if (!track) continue;
            for (int li = 0; li < track->getLaneCount() && !junction; ++li)
            {
                auto* lane = track->getLane(li);
                if (!lane) continue;
                for (size_t a = 0; a < lane->clips.size() && !junction; ++a)
                {
                    auto* ca = lane->clips[a].get();
                    for (size_t b = 0; b < lane->clips.size(); ++b)
                    {
                        if (b == a) continue;
                        auto* cb = lane->clips[b].get();
                        if (cb->getStartPosition() < ca->getStartPosition()) continue; // ca を左に
                        // 接触/重なり (ca.end + tol >= cb.start)
                        if (ca->getEndPosition() + tol < cb->getStartPosition()) continue;
                        // 境界/重なり区間が選択範囲と交差するか
                        const double jS = juce::jmin(ca->getEndPosition(), cb->getStartPosition());
                        const double jE = juce::jmax(ca->getEndPosition(), cb->getStartPosition());
                        if (juce::jmax(selS, jS) <= juce::jmin(selE, jE) + tol)
                            { junction = true; break; }
                    }
                }
            }
        }
        if (junction)
            m.addItem(320, tr(u8"クロスフェードを描く"));
    }
    m.addItem(310, tr(u8"無音区間をカット..."));
    m.addItem(330, tr(u8"テンポを検出"));
    m.addItem(340, tr(u8"ラウドネスを ") + juce::String(appSettings.loudnessTargetLufs, 1)
                      + tr(u8" LUFS に合わせる"));

    // ── キー変更 (-6 〜 +6 半音) サブメニュー ──
    // 現在のキー値はチェックマークで明示。 ★ は処理済みキャッシュあり。
    juce::PopupMenu pitchMenu;
    auto srcFileForMenu = rcRef.clip ? rcRef.clip->getFile() : juce::File();
    auto curStem        = srcFileForMenu.getFileNameWithoutExtension();
    int  currentSemis   = 0;
    juce::String srcStem = curStem;
    if (curStem.contains("_pitch"))
    {
        srcStem = curStem.upToLastOccurrenceOf("_pitch", false, false);
        currentSemis = curStem.fromLastOccurrenceOf("_pitch", false, false).getIntValue();
    }
    for (int s = -6; s <= 6; ++s)
    {
        juce::String label;
        if (s == 0) label = tr(u8"0 (元の音程)");
        else        label = (s > 0 ? juce::String("+") : juce::String()) + juce::String(s) + tr(u8" 半音");

        // キャッシュ判定
        if (s != 0 && srcFileForMenu.existsAsFile())
        {
            const juce::String suffix = (s > 0 ? juce::String("+") : juce::String()) + juce::String(s);
            auto cached = srcFileForMenu.getParentDirectory().getChildFile(srcStem + "_pitch" + suffix + ".wav");
            if (cached.existsAsFile())
                label += "  \xe2\x98\x85";  // ★ (UTF-8)
        }

        juce::PopupMenu::Item it;
        it.itemID  = 500 + 6 + s;
        it.text    = label;
        it.isTicked = (s == currentSemis);
        pitchMenu.addItem(it);
        if (s == 0) pitchMenu.addSeparator();
    }
    m.addSubMenu(tr(u8"キーを変更..."), pitchMenu);
    m.addSeparator();
    m.addItem(400, tr(u8"削除"));

    // メニュー表示中に view/クリップが破棄されることがあるため、SafePointer で
    // 自身の生存を、clipStillExists で rcRef.clip の生存を確認してから処理 (UAF 防止)
    m.showMenuAsync(juce::PopupMenu::Options(),
        [this, safe = juce::Component::SafePointer<TimelineView>(this), rcRef](int result) {
            if (safe.getComponent() == nullptr) return;
            if (result <= 0) return;
            if (!clipStillExists(rcRef.clip)) return;
            if (result >= 100 && result <= 107)
            {
                // 色変更
                const std::array<juce::Colour, 8> palette2 = {
                    juce::Colour(0xff3a6ea5), juce::Colour(0xff5aa55a),
                    juce::Colour(0xffa55a5a), juce::Colour(0xffa5925a),
                    juce::Colour(0xff7a5aa5), juce::Colour(0xff5a9ea5),
                    juce::Colour(0xffa55a92), juce::Colour(0xff5a7aa5)
                };
                juce::Colour newCol = palette2[result - 100];
                // 選択中のクリップ全てに適用 (Undo 対応)
                std::vector<ClipRef> all;
                if (selectedClip.valid()) all.push_back(selectedClip);
                for (auto& r : extraSelections) all.push_back(r);
                if (all.empty()) all.push_back(rcRef);
                std::vector<EditActions::ClipState> oldS, newS;
                for (auto& r : all)
                {
                    EditActions::ClipState s; s.capture(r.clip); oldS.push_back(s);
                    r.clip->setColour(newCol);
                    EditActions::ClipState ns; ns.capture(r.clip); newS.push_back(ns);
                }
                if (undoManager)
                {
                    undoManager->beginNewTransaction();
                    undoManager->perform(new EditActions::ClipsPropertyAction(
                        std::move(oldS), std::move(newS), editChangeCb));
                }
                repaint();
                return;
            }
            if (result == 300)
            {
                consolidateSelectedClips();
                return;
            }
            if (result == 320)
            {
                applyCrossfadeToSelection(FadeOpMode::CrossfadeOnly);
                return;
            }
            if (result == 321)
            {
                // 「このテイクを使う」: 右クリックしたテイクレーンから Lane 0 へ採用。
                // 選択範囲があればその範囲、無ければ右クリックしたクリップ全体を当てる
                // (= Shift+↑ / ↑ ボタンと同じ promoteTakeLane)。範囲が無いときは
                // 右クリッククリップを選択状態にして promoteTakeLane の「選択クリップ」
                // 経路を満たす。promoteRangeToLane0 が editChangeCb
                // (markProjectDirty / refresh / invalidatePlayback) を呼ぶので追加処理は不要。
                if (!hasSelectionRange())
                {
                    clearAllSelections();
                    selectedClip = rcRef;
                    notifySelectionChanged();
                }
                promoteTakeLane(rcRef.trackIdx, rcRef.laneIdx);
                return;
            }
            if (result == 310)
            {
                showStripSilenceDialog(rcRef);
                return;
            }
            if (result == 330)
            {
                // テンポを検出
                if (rcRef.valid() && rcRef.track && rcRef.clip)
                {

                    // 検出は重い (全ファイル走査) ので進捗ウィンドウ付きで別スレッド実行。
                    // モーダル実行中はクリップが破棄されないため参照キャプチャで安全。
                    struct DetectJob : juce::ThreadWithProgressWindow
                    {
                        juce::File file; juce::AudioFormatManager& fmt;
                        double off, dur; double result = 0.0;
                        DetectJob (juce::File f, juce::AudioFormatManager& fm, double o, double d,
                                   const juce::String& title, const juce::String& status)
                            : juce::ThreadWithProgressWindow (title, true, true),
                              file (f), fmt (fm), off (o), dur (d)
                        { setStatusMessage (status); }
                        void run() override
                        {
                            result = BpmDetector::detect (file, fmt, off, dur,
                                [this] (double frac) { setProgress (frac); return ! threadShouldExit(); });
                        }
                    };
                    DetectJob job (rcRef.clip->getFile(),
                                   rcRef.track->getFormatManager(),
                                   rcRef.clip->getFileOffset(),
                                   rcRef.clip->getDuration(),
                                   tr(u8"テンポ検出"), tr(u8"テンポを検出中…"));
                    if (! job.runThread()) return;   // ユーザーがキャンセル
                    const double bpm = job.result;
                    if (bpm <= 0.0)
                    {
                        juce::AlertWindow::showAsync(juce::MessageBoxOptions()
                            .withIconType(juce::MessageBoxIconType::WarningIcon)
                            .withTitle(tr(u8"テンポ検出"))
                            .withMessage(tr(u8"テンポを検出できませんでした。\nクリップが短すぎるか、明確なビートが見つかりません。"))
                            .withButton("OK"), nullptr);
                    }
                    else
                    {
                        const juce::String msg = tr(u8"検出されたテンポ: ")
                            + juce::String((int) std::round(bpm)) + " BPM\n\n"
                            + tr(u8"プロジェクトに適用しますか?");
                        juce::AlertWindow::showAsync(juce::MessageBoxOptions()
                            .withIconType(juce::MessageBoxIconType::QuestionIcon)
                            .withTitle(tr(u8"テンポ検出"))
                            .withMessage(msg)
                            .withButton(tr(u8"適用"))
                            .withButton(tr(u8"キャンセル")),
                            [this, bpm](int r) {
                                if (r == 1 && onApplyDetectedBpm)
                                    onApplyDetectedBpm(bpm);
                            });
                    }
                }
                return;
            }
            if (result == 340)
            {
                // ラウドネスを -18 LUFS に合わせる (クリップゲインを調整)
                if (rcRef.valid() && rcRef.track && rcRef.clip)
                {
                    const float  oldGain     = rcRef.clip->getGain();
                    const double oldGainDb   = 20.0 * std::log10(juce::jmax(1.0e-12f, oldGain));
                    const double measuredLufs = LufsMeter::measureFileSegment(
                        rcRef.clip->getFile(),
                        rcRef.track->getFormatManager(),
                        rcRef.clip->getFileOffset(),
                        rcRef.clip->getDuration(),
                        oldGain);
                    if (!std::isfinite(measuredLufs))
                    {
                        juce::AlertWindow::showAsync(juce::MessageBoxOptions()
                            .withIconType(juce::MessageBoxIconType::WarningIcon)
                            .withTitle(tr(u8"ラウドネス測定"))
                            .withMessage(tr(u8"ラウドネスを測定できませんでした。\nクリップが短すぎるか、無音が多すぎます。"))
                            .withButton("OK"), nullptr);
                        return;
                    }

                    const double targetLufs   = (double) appSettings.loudnessTargetLufs;
                    const double adjustmentDb = targetLufs - measuredLufs;
                    const float  newGain      = oldGain * (float) std::pow(10.0, adjustmentDb / 20.0);
                    const double newGainDb    = oldGainDb + adjustmentDb;

                    auto fmt2 = [](double v) {
                        return (v >= 0.0 ? juce::String("+") : juce::String())
                               + juce::String(v, 1);
                    };
                    const juce::String msg = tr(u8"現在のラウドネス: ")
                        + juce::String(measuredLufs, 1) + " LUFS\n"
                        + tr(u8"ターゲット: ") + juce::String(targetLufs, 1) + " LUFS\n"
                        + tr(u8"クリップゲイン: ") + fmt2(oldGainDb) + " dB → "
                        + fmt2(newGainDb) + " dB ("
                        + fmt2(adjustmentDb) + " dB)\n\n"
                        + tr(u8"適用しますか?");

                    juce::Component::SafePointer<TimelineView> safe(this);
                    juce::AlertWindow::showAsync(juce::MessageBoxOptions()
                        .withIconType(juce::MessageBoxIconType::QuestionIcon)
                        .withTitle(tr(u8"ラウドネス調整"))
                        .withMessage(msg)
                        .withButton(tr(u8"適用"))
                        .withButton(tr(u8"キャンセル")),
                        [safe, rcRef, newGain](int r)
                        {
                            if (r != 1) return;
                            auto* tv = safe.getComponent();
                            if (!tv || !rcRef.valid() || rcRef.clip == nullptr) return;
                            EditActions::ClipState oldS, newS;
                            oldS.capture(rcRef.clip);
                            rcRef.clip->setGain(newGain);
                            newS.capture(rcRef.clip);
                            if (tv->undoManager)
                            {
                                tv->undoManager->beginNewTransaction();
                                tv->undoManager->perform(new EditActions::ClipsPropertyAction(
                                    { oldS }, { newS }, tv->editChangeCb));
                            }
                            tv->repaint();
                        });
                }
                return;
            }
            // キー変更: ID 500..512 (= -6..+6 半音, 506 = 0)
            if (result >= 500 && result <= 512)
            {
                const int semis = result - 500 - 6;
                if (!rcRef.valid() || !rcRef.track || !rcRef.clip) return;
                auto* clip = rcRef.clip;
                auto srcFile = clip->getFile();

                // 元ファイル (拡張子付き) を解決
                // - 現在が xxx_pitch±N.<ext> なら xxx.<ext> に戻して探す
                // - 拡張子は .wav / .aif / .aiff / .mp3 / .m4a / .flac / .ogg を順に試す
                juce::File origFile = srcFile;
                {
                    auto stem = srcFile.getFileNameWithoutExtension();
                    if (stem.contains("_pitch"))
                    {
                        stem = stem.upToLastOccurrenceOf("_pitch", false, false);
                        auto dir = srcFile.getParentDirectory();
                        static const char* exts[] = { ".wav", ".aif", ".aiff", ".mp3", ".m4a", ".flac", ".ogg" };
                        juce::File found;
                        for (auto* e : exts)
                        {
                            auto f = dir.getChildFile(stem + e);
                            if (f.existsAsFile()) { found = f; break; }
                        }
                        if (found != juce::File()) origFile = found;
                    }
                }

                // semis == 0 (元の音程に戻す) の場合は元ファイルへ差し替えるだけ
                if (semis == 0)
                {
                    if (origFile == clip->getFile()) return;  // 既に元の状態
                    if (!origFile.existsAsFile())
                    {
                        juce::AlertWindow::showAsync(juce::MessageBoxOptions()
                            .withIconType(juce::MessageBoxIconType::WarningIcon)
                            .withTitle(tr(u8"キー変更"))
                            .withMessage(tr(u8"元の音程のファイルが見つかりませんでした。"))
                            .withButton("OK"), nullptr);
                        return;
                    }
                    clip->setFile(origFile);
                    clip->invalidateWaveformCache();
                    clip->refreshThumbnail();
                    if (editChangeCb) editChangeCb();
                    if (onWaveformRefreshNeeded) onWaveformRefreshNeeded();
                    repaint();
                    return;
                }

                // 変換目標ファイル名 = <元ステム>_pitch<符号><量>.wav
                srcFile = origFile;
                const juce::String suffix = (semis > 0 ? juce::String("+") : juce::String()) + juce::String(semis);
                auto outFile = srcFile.getParentDirectory().getChildFile(
                    srcFile.getFileNameWithoutExtension() + "_pitch" + suffix + ".wav");

                // 既存キャッシュがあれば即時切り替え
                if (outFile.existsAsFile())
                {
                    clip->setFile(outFile);
                    clip->invalidateWaveformCache();
                    clip->refreshThumbnail();
                    if (editChangeCb) editChangeCb();
                    if (onWaveformRefreshNeeded) onWaveformRefreshNeeded();
                    repaint();
                    return;
                }

                // ── プログレスウィンドウ付きで変換 ──
                class PitchJob : public juce::ThreadWithProgressWindow
                {
                public:
                    PitchJob(const juce::File& in, const juce::File& out,
                             juce::AudioFormatManager& fmt, double sem)
                        : juce::ThreadWithProgressWindow(
                            /*title*/ makeTitle(sem),
                            /*hasProgressBar*/ true,
                            /*hasCancelButton*/ false),
                          inFile(in), outFile(out), formatManager(fmt), semitones(sem) {}

                    static juce::String makeTitle(double sem)
                    {
                        const juce::String semStr = (sem > 0 ? juce::String("+") : juce::String())
                                                  + juce::String((int) sem);
                        return tr(u8"キーを ")
                             + semStr
                             + tr(u8" へ変換中...");
                    }

                    void run() override
                    {
                        ok = PitchEngine::processFile(inFile, outFile, formatManager,
                                                      semitones, 32,
                            [this](double p) { setProgress(p); });
                    }
                    bool ok { false };
                private:
                    juce::File inFile, outFile;
                    juce::AudioFormatManager& formatManager;
                    double semitones;
                };

                auto job = std::make_unique<PitchJob>(
                    srcFile, outFile, rcRef.track->getFormatManager(), (double) semis);
                const bool finished = job->runThread();  // モーダル
                if (!finished || !job->ok)
                {
                    juce::AlertWindow::showAsync(juce::MessageBoxOptions()
                        .withIconType(juce::MessageBoxIconType::WarningIcon)
                        .withTitle(tr(u8"キー変更"))
                        .withMessage(tr(u8"ピッチシフト処理に失敗しました。"))
                        .withButton("OK"), nullptr);
                    return;
                }
                clip->setFile(outFile);
                clip->invalidateWaveformCache();
                clip->refreshThumbnail();
                if (editChangeCb) editChangeCb();
                // ロード完了までポーリングして波形を再描画 (MainComponent 側に委譲)
                if (onWaveformRefreshNeeded) onWaveformRefreshNeeded();
                repaint();
                return;
            }
            // (「このテイクを使う」は ID 321 で上の方で処理済み。以前ここに ID 320 の
            //  ハンドラがあったが、320 は「クロスフェードを描く」と衝突して到達不能だった)
            if (result == 400)
            {
                deleteSelectedClips();
                return;
            }
        });
}
