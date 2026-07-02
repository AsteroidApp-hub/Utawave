// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

// TimelineView の重い編集操作 (結合/無音カット/クロスフェード/分割) (TimelineView_Edit.cpp から分割)。

#include "TimelineView.h"
#include "../Localisation.h"
#include "../AppColours.h"
#include "../Tracks/MidiClip.h"
#include "../Edit/SilenceDetector.h"
#include "TextImageCache.h"
#include <set>
#include <map>
#include <algorithm>   // std::sort (隣接ペアのクロスフェード)

void TimelineView::consolidateSelectedClips()
{
    std::vector<ClipRef> all;
    if (selectedClip.valid()) all.push_back(selectedClip);
    for (auto& r : extraSelections) all.push_back(r);
    if (all.size() < 2) return;

    // 同一トラック / 同一レーン内のクリップのみを対象とする（混在は警告）
    Track* track = all.front().track;
    Lane*  lane  = all.front().lane;
    for (auto& r : all)
        if (r.track != track || r.lane != lane) return;

    // 範囲を決定
    double minStart = all.front().clip->getStartPosition();
    double maxEnd   = all.front().clip->getEndPosition();
    for (auto& r : all)
    {
        minStart = juce::jmin(minStart, r.clip->getStartPosition());
        maxEnd   = juce::jmax(maxEnd,   r.clip->getEndPosition());
    }
    double durationSecs = maxEnd - minStart;
    if (durationSecs < 0.05) return;

    // バウンス先 WAV ファイルを作成（録音用と同じディレクトリに置く）
    auto outFile = juce::File::getSpecialLocation(juce::File::userMusicDirectory)
                       .getChildFile("Utawave")
                       .getChildFile("Consolidated_" + juce::Time::getCurrentTime().formatted("%Y%m%d_%H%M%S") + ".wav");
    outFile.getParentDirectory().createDirectory();

    // ファイルを開いて WAV ライターを作成
    juce::WavAudioFormat wavFormat;
    juce::AudioFormatManager& fmt = track->getFormatManager();
    int outSampleRate = (int)sampleRate;

    // 対象クリップのチャンネル数の最大値で出力（モノラルのみならモノラル）
    int outNumChannels = 1;
    for (auto& r : all)
    {
        std::unique_ptr<juce::AudioFormatReader> rd(fmt.createReaderFor(r.clip->getFile()));
        if (rd) outNumChannels = juce::jmax(outNumChannels, (int)rd->numChannels);
    }

    auto outStream = std::unique_ptr<juce::FileOutputStream>(outFile.createOutputStream());
    if (!outStream) return;
    std::unique_ptr<juce::AudioFormatWriter> writer(
        wavFormat.createWriterFor(outStream.get(), outSampleRate, outNumChannels, 24, {}, 0));
    if (!writer) return;
    outStream.release();

    // 全クリップを順に読み込んでミックス、書き出し
    juce::AudioBuffer<float> mixBuf(outNumChannels, (int)(durationSecs * outSampleRate) + 1);
    mixBuf.clear();

    for (auto& r : all)
    {
        auto* reader = fmt.createReaderFor(r.clip->getFile());
        if (!reader) continue;
        std::unique_ptr<juce::AudioFormatReader> readerOwner(reader);

        double clipDur     = r.clip->getDuration();
        double clipFileOfs = r.clip->getFileOffset();
        double offsetInMix = r.clip->getStartPosition() - minStart;

        int destStart = (int)(offsetInMix * outSampleRate);
        int numSamps  = (int)(clipDur * outSampleRate);
        if (destStart + numSamps > mixBuf.getNumSamples())
            numSamps = mixBuf.getNumSamples() - destStart;
        if (numSamps <= 0) continue;

        juce::AudioBuffer<float> tmp((int)reader->numChannels, numSamps);
        reader->read(&tmp, 0, numSamps, (juce::int64)(clipFileOfs * reader->sampleRate), true, true);

        // フェード適用（簡易版: リニアのみ）
        float fInSecs  = (float)r.clip->getFadeInSecs();
        float fOutSecs = (float)r.clip->getFadeOutSecs();
        for (int i = 0; i < numSamps; ++i)
        {
            float pos = (float)i / outSampleRate;
            float g = r.clip->getGain();
            if (fInSecs > 0.0f && pos < fInSecs)
                g *= AudioClip::applyFadeCurve(pos / fInSecs, r.clip->getFadeInCurve());
            if (fOutSecs > 0.0f)
            {
                float fOutStart = (float)clipDur - fOutSecs;
                if (pos > fOutStart)
                    g *= AudioClip::applyFadeCurve(((float)clipDur - pos) / fOutSecs, r.clip->getFadeOutCurve());
            }
            for (int ch = 0; ch < mixBuf.getNumChannels(); ++ch)
            {
                int srcCh = juce::jmin(ch, tmp.getNumChannels() - 1);
                mixBuf.addSample(ch, destStart + i, tmp.getSample(srcCh, i) * g);
            }
        }
    }

    writer->writeFromAudioSampleBuffer(mixBuf, 0, mixBuf.getNumSamples());
    writer.reset();

    // 元クリップを削除して、新クリップを追加（Undo 1 トランザクション）
    if (undoManager) undoManager->beginNewTransaction();
    for (auto& r : all)
    {
        if (undoManager)
            undoManager->perform(new EditActions::ClipDeleteAction(r.lane, r.clip, editChangeCb));
    }
    if (undoManager)
    {
        EditActions::ClipParams p;
        p.file       = outFile;
        p.startPos   = minStart;
        p.duration   = durationSecs;
        p.fileOffset = 0.0;
        p.fadeIn     = 0.010;
        p.fadeOut    = 0.010;
        p.gain       = 1.0f;
        p.name       = "Consolidated";
        p.colour     = track->getColour();
        undoManager->perform(new EditActions::ClipAddAction(
            lane, p, track->getFormatManager(), track->getThumbnailCache(), editChangeCb));
    }
    clearAllSelections();
    repaint();
}

void TimelineView::showStripSilenceDialog(const ClipRef& ref)
{
    if (!ref.valid()) return;

    class StripDlg : public juce::Component
    {
    public:
        juce::Label thresholdLabel, minSilenceLabel, padBeforeLabel, padAfterLabel, fadeLabel;
        juce::Slider thresholdSlider, minSilenceSlider, padBeforeSlider, padAfterSlider, fadeSlider;
        juce::TextButton applyBtn, cancelBtn;
        std::function<void(float thrDb, double minSilenceMs,
                           double padBeforeMs, double padAfterMs, double fadeMs)> onApply;

        StripDlg()
        {
            auto setupLabel = [this](juce::Label& l, juce::String txt) {
                l.setText(txt, juce::dontSendNotification);
                l.setColour(juce::Label::textColourId, juce::Colours::white);
                l.setFont(juce::FontOptions(12.0f));
                addAndMakeVisible(l);
            };
            auto setupSlider = [this](juce::Slider& s, double minV, double maxV, double interval,
                                       double init, const juce::String& suffix) {
                s.setRange(minV, maxV, interval);
                s.setValue(init, juce::dontSendNotification);
                s.setTextValueSuffix(suffix);
                s.setSliderStyle(juce::Slider::LinearHorizontal);
                s.setTextBoxStyle(juce::Slider::TextBoxRight, false, 70, 22);
                s.setColour(juce::Slider::textBoxBackgroundColourId, juce::Colour(0xff3a3a3a));
                s.setColour(juce::Slider::textBoxTextColourId,       juce::Colours::white);
                s.setColour(juce::Slider::textBoxOutlineColourId,    juce::Colour(0xff555555));
                s.setColour(juce::Slider::trackColourId,             juce::Colour(0xff6a90c0));
                addAndMakeVisible(s);
            };
            setupLabel(thresholdLabel,  tr(u8"閾値 (dB)"));
            setupLabel(minSilenceLabel, tr(u8"最短無音時間"));
            setupLabel(padBeforeLabel,  tr(u8"前パディング"));
            setupLabel(padAfterLabel,   tr(u8"後パディング"));
            setupLabel(fadeLabel,       tr(u8"境界フェード"));
            setupSlider(thresholdSlider,  -60.0,  -20.0,  1.0,  -40.0, " dB");
            setupSlider(minSilenceSlider, 100.0, 2000.0, 50.0, 300.0, " ms");
            setupSlider(padBeforeSlider,    0.0,  500.0, 10.0,  50.0, " ms");
            setupSlider(padAfterSlider,     0.0,  500.0, 10.0,  50.0, " ms");
            setupSlider(fadeSlider,         0.0,   50.0,  1.0,   5.0, " ms");

            applyBtn.setButtonText(tr(u8"適用"));
            applyBtn.onClick = [this] {
                if (onApply)
                    onApply((float)thresholdSlider.getValue(),
                            minSilenceSlider.getValue(),
                            padBeforeSlider.getValue(),
                            padAfterSlider.getValue(),
                            fadeSlider.getValue());
                if (auto* dw = findParentComponentOfClass<juce::DialogWindow>())
                    dw->exitModalState(1);
            };
            addAndMakeVisible(applyBtn);

            cancelBtn.setButtonText(tr(u8"キャンセル"));
            cancelBtn.onClick = [this] {
                if (auto* dw = findParentComponentOfClass<juce::DialogWindow>())
                    dw->exitModalState(0);
            };
            addAndMakeVisible(cancelBtn);

            setSize(440, 280);
        }
        void resized() override
        {
            const int labelW = 110;
            int y = 14;
            auto row = [&](juce::Label& l, juce::Slider& s) {
                l.setBounds(14, y, labelW, 24);
                s.setBounds(14 + labelW, y, getWidth() - 28 - labelW, 24);
                y += 30;
            };
            row(thresholdLabel,  thresholdSlider);
            row(minSilenceLabel, minSilenceSlider);
            row(padBeforeLabel,  padBeforeSlider);
            row(padAfterLabel,   padAfterSlider);
            row(fadeLabel,       fadeSlider);
            cancelBtn.setBounds(getWidth() - 220, getHeight() - 40, 100, 28);
            applyBtn .setBounds(getWidth() - 114, getHeight() - 40, 100, 28);
        }
        void paint(juce::Graphics& g) override { g.fillAll(juce::Colour(0xff2a2a2a)); }
    };

    auto* dlg = new StripDlg();
    dlg->onApply = [this, ref](float thrDb, double minMs,
                                double padBeforeMs, double padAfterMs, double fadeMs)
    {
        if (!ref.valid()) return;
        const double padBefore = padBeforeMs / 1000.0;
        const double padAfter  = padAfterMs  / 1000.0;
        const double fadeSecs  = fadeMs      / 1000.0;
        const double minSilence = minMs      / 1000.0;
        const float  thrLinear = juce::Decibels::decibelsToGain(thrDb);

        auto silence = SilenceDetector::detect(ref.clip->getFile(),
                                                ref.clip->getFileOffset(),
                                                ref.clip->getDuration(),
                                                thrLinear, minSilence,
                                                ref.track->getFormatManager());
        if (silence.empty())
        {
            juce::AlertWindow::showAsync(juce::MessageBoxOptions()
                .withIconType(juce::MessageBoxIconType::InfoIcon)
                .withTitle(tr(u8"無音区間なし"))
                .withMessage(tr(u8"指定条件で無音区間は見つかりませんでした。"))
                .withButton("OK"), nullptr);
            return;
        }
        auto keep = SilenceDetector::regionsToKeep(silence, ref.clip->getDuration(),
                                                    padBefore, padAfter);
        if (keep.empty())
        {
            juce::AlertWindow::showAsync(juce::MessageBoxOptions()
                .withIconType(juce::MessageBoxIconType::WarningIcon)
                .withTitle(tr(u8"全体が無音"))
                .withMessage(tr(u8"クリップ全体が無音と判定されました。実行を中止します。"))
                .withButton("OK"), nullptr);
            return;
        }

        std::vector<EditActions::StripSilenceAction::Segment> segs;
        segs.reserve(keep.size());
        for (auto& r : keep) segs.push_back({ r.start, r.end });

        // ゲインエンベロープを持つクリップは Strip Silence 実行で破棄される
        // (StripSilenceAction はセグメント分割の複雑度を抑えるためエンベロープを引き継がない)。
        // ユーザに警告し、続行確認を取る。
        auto runAction = [this, ref, segs = std::move(segs), fadeSecs]() mutable
        {
            if (undoManager)
            {
                undoManager->beginNewTransaction();
                undoManager->perform(new EditActions::StripSilenceAction(
                    ref.lane, ref.clip, std::move(segs), fadeSecs,
                    ref.track->getFormatManager(),
                    ref.track->getThumbnailCache(),
                    editChangeCb, editBeforeChangeCb));
            }
            clearAllSelections();
            repaint();
        };

        if (ref.clip->hasGainEnvelope())
        {
            juce::AlertWindow::showAsync(
                juce::MessageBoxOptions()
                    .withIconType(juce::MessageBoxIconType::QuestionIcon)
                    .withTitle(tr(u8"ゲインエンベロープを破棄しますか?"))
                    .withMessage(tr(u8"このクリップにはボリュームエンベロープが設定されています。\n"
                                    u8"無音区間カットを実行するとエンベロープは破棄されます。\n"
                                    u8"続行しますか?"))
                    .withButton(tr(u8"続行"))
                    .withButton(tr(u8"キャンセル")),
                [runAction = std::move(runAction)](int result) mutable
                {
                    if (result == 1) runAction();
                });
            return;
        }

        runAction();
    };

    juce::DialogWindow::LaunchOptions opts;
    opts.content.setOwned(dlg);
    opts.dialogTitle = tr(u8"無音区間をカット");
    opts.dialogBackgroundColour = juce::Colour(0xff2a2a2a);
    opts.escapeKeyTriggersCloseButton = true;
    opts.useNativeTitleBar = true;
    opts.resizable = false;
    opts.launchAsync();
}

// ゼロクロスを検索 (AudioEngine.cpp 内の同名 static 関数と等価)
static juce::int64 findZeroCrossingLocal(juce::AudioFormatReader* reader,
                                          juce::int64 nearSample,
                                          juce::int64 searchRange,
                                          bool backward)
{
    if (reader == nullptr || searchRange <= 0) return nearSample;
    const int bufSize = (int) juce::jmin((juce::int64) 512, searchRange);
    juce::AudioBuffer<float> buf(1, bufSize);
    buf.clear();
    juce::int64 startSample = backward
                               ? juce::jmax((juce::int64) 0, nearSample - searchRange)
                               : nearSample;
    reader->read(&buf, 0, bufSize, startSample, true, false);
    if (backward)
    {
        float prev = buf.getSample(0, bufSize - 1);
        for (int i = bufSize - 2; i >= 0; --i)
        {
            float curr = buf.getSample(0, i);
            if ((prev <= 0.0f && curr > 0.0f) || (prev > 0.0f && curr <= 0.0f))
                return startSample + i + 1;
            prev = curr;
        }
    }
    else
    {
        float prev = buf.getSample(0, 0);
        for (int i = 1; i < bufSize; ++i)
        {
            float curr = buf.getSample(0, i);
            if ((prev <= 0.0f && curr > 0.0f) || (prev > 0.0f && curr <= 0.0f))
                return startSample + i;
            prev = curr;
        }
    }
    return nearSample;
}

void TimelineView::applyCrossfadeToSelection(FadeOpMode mode)
{
    const bool useSelRange = hasSelectionRange();

    // 選択中クリップ群 (選択範囲なしの場合の従来挙動で使う)
    std::vector<ClipRef> sel;
    if (selectedClip.valid()) sel.push_back(selectedClip);
    for (auto& r : extraSelections) sel.push_back(r);

    // 集める対象レーン:
    //  ・選択範囲あり: 全トラックの全レーンを対象 (選択範囲だけで決定可能)
    //  ・選択範囲なし: 選択中クリップのレーンのみ (従来挙動)
    std::vector<Lane*> lanes;
    if (useSelRange)
    {
        for (int ti = 0; ti < trackManager.getTrackCount(); ++ti)
        {
            auto* track = trackManager.getTrack(ti);
            if (!track) continue;
            for (int li = 0; li < track->getLaneCount(); ++li)
                if (auto* lane = track->getLane(li))
                    lanes.push_back(lane);
        }
    }
    else
    {
        if (sel.empty()) return;
        for (auto& r : sel)
            if (r.lane && std::find(lanes.begin(), lanes.end(), r.lane) == lanes.end())
                lanes.push_back(r.lane);
    }

    // 操作種別: クロスフェード / フェードインのみ / フェードアウトのみ
    struct Op
    {
        enum Kind { Crossfade, FadeInOnly, FadeOutOnly } kind { Crossfade };
        Lane* lane { nullptr };
        AudioClip* clipA { nullptr };  // FadeInOnly では nullptr
        AudioClip* clipB { nullptr };  // FadeOutOnly では nullptr
        double xfStart { 0.0 };
        double xfEnd   { 0.0 };
    };
    std::vector<Op> ops;
    auto seenPairs = std::set<std::pair<AudioClip*, AudioClip*>>{};
    auto seenClips = std::set<AudioClip*>{};

    const double selS = loopStartTV;
    const double selE = loopEndTV;

    if (useSelRange)
    {
        const double tol    = 0.005;
        const double gapTol = 0.005;
        for (auto* lane : lanes)
        {
            if (mode == FadeOpMode::CrossfadeOnly)
            {
                // #X1: 選択範囲内で接触/重なっている「隣接ペアごと」に局所的な最小クロスフェードを作る。
                // 選択範囲全体を 1 つの巨大クロスフェードにすると、範囲内の無関係なクリップまで
                // 巻き込んで誤ったクロスフェードになるため (旧実装の不具合)。
                std::vector<AudioClip*> sorted;
                for (auto& cPtr : lane->clips) sorted.push_back(cPtr.get());
                std::sort(sorted.begin(), sorted.end(),
                          [](AudioClip* a, AudioClip* b)
                          { return a->getStartPosition() < b->getStartPosition(); });
                for (size_t i = 0; i + 1 < sorted.size(); ++i)
                {
                    AudioClip* clipA = sorted[i];
                    AudioClip* clipB = sorted[i + 1];
                    // 隣接して接触/重なっているペアのみ (離れているクリップは繋がない)
                    if (clipA->getEndPosition() + gapTol < clipB->getStartPosition()) continue;
                    // clipB が clipA に内包される (clipB.end <= clipA.end) のは正規の境界ではない
                    // (clipA のフェードアウトが clipB の無い位置に来てしまう)。クロスフェードしない。
                    if (clipB->getEndPosition() <= clipA->getEndPosition() + 0.001) continue;
                    // 境界/重なり区間が選択範囲と交差する junction のみ (範囲外には影響しない)
                    const double jS = juce::jmin(clipA->getEndPosition(), clipB->getStartPosition());
                    const double jE = juce::jmax(clipA->getEndPosition(), clipB->getStartPosition());
                    if (juce::jmax(selS, jS) > juce::jmin(selE, jE) + tol) continue;
                    auto key = std::make_pair(juce::jmin(clipA, clipB), juce::jmax(clipA, clipB));
                    if (seenPairs.count(key)) continue;
                    seenPairs.insert(key);
                    // 局所最小クロスフェード: clipB は動かさず (xfStart=clipB.start)、clipA を
                    // 必要分だけ右へ伸ばす (xfEnd=clipA.end → 既存の重なり、無ければ最低 kXfade)。
                    ops.push_back({ Op::Crossfade, lane, clipA, clipB,
                                    clipB->getStartPosition(), clipA->getEndPosition() });
                }
                continue;   // F (FadesOnly) 用ロジックへは進まない
            }

            // ── F (FadesOnly): 端が選択範囲内のクリップに個別フェード (fade-out / fade-in) ──
            AudioClip* clipA = nullptr;
            AudioClip* clipB = nullptr;
            for (auto& cPtr : lane->clips)
            {
                auto* c = cPtr.get();
                const double cs = c->getStartPosition();
                const double ce = c->getEndPosition();
                if (ce >= selS - tol && ce <= selE + tol)
                    if (!clipA || ce > clipA->getEndPosition()) clipA = c;
                if (cs >= selS - tol && cs <= selE + tol)
                    if (!clipB || cs < clipB->getStartPosition()) clipB = c;
            }
            const bool haveA = (clipA != nullptr);
            const bool haveB = (clipB != nullptr);
            const bool sameClip = (haveA && haveB && clipA == clipB);
            if (haveA && !sameClip && !seenClips.count(clipA))
            {
                seenClips.insert(clipA);
                ops.push_back({ Op::FadeOutOnly, lane, clipA, nullptr, selS, selE });
            }
            if (haveB && !sameClip && !seenClips.count(clipB))
            {
                seenClips.insert(clipB);
                ops.push_back({ Op::FadeInOnly, lane, nullptr, clipB, selS, selE });
            }
        }
    }
    else
    {
        // 選択範囲なし: 重なりベース (従来の挙動)
        for (auto& r : sel)
        {
            if (!r.lane) continue;
            for (auto& cPtr : r.lane->clips)
            {
                auto* nb = cPtr.get();
                if (nb == r.clip) continue;
                const double aS = r.clip->getStartPosition();
                const double aE = r.clip->getEndPosition();
                const double bS = nb->getStartPosition();
                const double bE = nb->getEndPosition();
                const double ovS = juce::jmax(aS, bS);
                const double ovE = juce::jmin(aE, bE);
                if (ovE - ovS < 0.001) continue;

                AudioClip* clipA = (aS < bS) ? r.clip : nb;
                AudioClip* clipB = (aS < bS) ? nb     : r.clip;
                // clipB が clipA に内包される (clipB.end <= clipA.end) のは正規の境界ではない
                // (clipA のフェードアウトが clipB の無い位置に来る)。クロスフェードしない。
                if (clipB->getEndPosition() <= clipA->getEndPosition() + 0.001) continue;
                // 同一の連続音声 (分割片) 同士でも Linear クロスフェードは完全再構成のため許可する。
                auto key = std::make_pair(juce::jmin(clipA, clipB), juce::jmax(clipA, clipB));
                if (seenPairs.count(key)) continue;
                seenPairs.insert(key);
                ops.push_back({ Op::Crossfade, r.lane, clipA, clipB, ovS, ovE });
            }
        }
    }
    if (ops.empty()) return;

    std::vector<EditActions::ClipState> oldStates, newStates;

    // クリップは移動しない。フェードのみ設定。
    // 選択範囲を超えてフェードが伸びないよう、クリップの実位置と選択範囲の
    // 交差区間でフェード長を決める。
    for (auto& op : ops)
    {
        if (op.kind == Op::Crossfade)
        {
            EditActions::ClipState oA, oB;
            oA.capture(op.clipA); oB.capture(op.clipB);
            oldStates.push_back(oA); oldStates.push_back(oB);

            // クロスフェードが「選択範囲 [selS, selE] 全体」を覆うようにする。
            // clipA を右 (selE) へ、clipB を左 (selS) へ伸ばし、両者が [selS, selE] で重なる。
            // こうすると境界が選択範囲の途中にあっても、ユーザーが選んだ範囲にそのまま
            // クロスフェードが作られる (従来は clipA だけ右へ伸ばし、境界より右側だけに
            // クロスフェードができて「選択と無関係な位置」に見えていた)。
            // 接触 (overlap=0) でも確実に成立するよう最低 kXfade(30ms) は確保する。
            constexpr double kXfade = 0.030;
            const double aStart = op.clipA->getStartPosition();

            // (1) clipA を selE (最低でも境界+30ms) まで右へ伸ばす。ファイル長は超えない (#M2)。
            {
                const double desiredEnd = juce::jmax(op.xfEnd, op.clipB->getStartPosition() + kXfade);
                if (op.clipA->getEndPosition() < desiredEnd)
                {
                    const double fileLen = op.clipA->getThumbnail().getTotalLength();
                    const double maxEnd  = (fileLen > 0.0)
                                           ? aStart + (fileLen - op.clipA->getFileOffset())
                                           : desiredEnd;
                    const double targetEnd = juce::jmin(desiredEnd, maxEnd);
                    if (targetEnd > aStart + 0.01)
                        op.clipA->setDuration(targetEnd - aStart);
                }
            }

            // (2) clipB を selS まで左へ伸ばす。ファイル先頭は超えない (fileOffset>=0)。
            {
                const double bStartOrig = op.clipB->getStartPosition();
                const double wantShift  = bStartOrig - op.xfStart;          // 左へ伸ばしたい量
                const double maxShift   = juce::jmax(0.0, op.clipB->getFileOffset());  // リードイン余白
                const double shift      = juce::jmin(wantShift, maxShift);
                if (shift > 0.001)
                {
                    op.clipB->setStartPosition(bStartOrig - shift);
                    op.clipB->setFileOffset (op.clipB->getFileOffset() - shift);
                    op.clipB->setDuration   (op.clipB->getDuration()   + shift);
                }
            }

            const double bStart = op.clipB->getStartPosition();  // 伸長後の値

            // フェード長 = 実際の overlap。ただし両クリップ長の半分を超えないよう先にクランプし
            // 両側に同じ値を設定する。これをしないと setFadeXxxSecs の個別クランプで左右非対称になり
            // 描画 (重なり全体の X) と実音がずれる (#M1)。
            double overlap = op.clipA->getEndPosition() - bStart;
            overlap = juce::jmin(overlap,
                                 op.clipA->getDuration() * 0.5,
                                 op.clipB->getDuration() * 0.5);
            if (overlap > 0.001)
            {
                double fadeOutA = overlap;
                double fadeInB  = overlap;

                // ゼロクロス・スナップ: フェード端 (ゲイン包絡線の角) を信号 ≈ 0 の位置に
                // 合わせてプチッ音を抑える。フェードを少し延長して角をゼロ交差へ動かす。
                if (appSettings.zeroCrossingFade)
                {
                    auto* rA = op.clipA->getOrCreateReader();
                    auto* rB = op.clipB->getOrCreateReader();
                    const double srA = op.clipA->getCachedSampleRate();
                    const double srB = op.clipB->getCachedSampleRate();
                    if (rA != nullptr && srA > 0.0)
                    {
                        // clipA のフェードアウト開始点 (end - fadeOut) 付近を後方検索
                        const juce::int64 boundA = (juce::int64)((op.clipA->getFileOffset()
                                                    + op.clipA->getDuration() - fadeOutA) * srA);
                        const juce::int64 rangeA = (juce::int64)(srA * 0.011);  // ~11ms (内部で512に制限)
                        const juce::int64 zcA = findZeroCrossingLocal(rA, boundA, rangeA, true);
                        fadeOutA += juce::jmax(0.0, (double)(boundA - zcA) / srA);
                    }
                    if (rB != nullptr && srB > 0.0)
                    {
                        // clipB のフェードイン終了点 (start + fadeIn) 付近を前方検索
                        const juce::int64 boundB = (juce::int64)((op.clipB->getFileOffset() + fadeInB) * srB);
                        const juce::int64 rangeB = (juce::int64)(srB * 0.011);
                        const juce::int64 zcB = findZeroCrossingLocal(rB, boundB, rangeB, false);
                        fadeInB += juce::jmax(0.0, (double)(zcB - boundB) / srB);
                    }
                    fadeOutA = juce::jmin(fadeOutA, op.clipA->getDuration() * 0.5);
                    fadeInB  = juce::jmin(fadeInB,  op.clipB->getDuration() * 0.5);
                }

                op.clipA->setFadeOutSecs(fadeOutA);
                op.clipB->setFadeInSecs(fadeInB);
            }

            EditActions::ClipState nA, nB;
            nA.capture(op.clipA); nB.capture(op.clipB);
            newStates.push_back(nA); newStates.push_back(nB);
        }
        else if (op.kind == Op::FadeInOnly)
        {
            EditActions::ClipState oB; oB.capture(op.clipB);
            oldStates.push_back(oB);

            // fade-in: clipB.start から selE までの長さ
            const double fadeIn = juce::jmax(0.001, op.xfEnd - op.clipB->getStartPosition());
            op.clipB->setFadeInSecs(fadeIn);

            EditActions::ClipState nB; nB.capture(op.clipB);
            newStates.push_back(nB);
        }
        else if (op.kind == Op::FadeOutOnly)
        {
            EditActions::ClipState oA; oA.capture(op.clipA);
            oldStates.push_back(oA);

            // fade-out: selS から clipA.end までの長さ
            const double fadeOut = juce::jmax(0.001, op.clipA->getEndPosition() - op.xfStart);
            op.clipA->setFadeOutSecs(fadeOut);

            EditActions::ClipState nA; nA.capture(op.clipA);
            newStates.push_back(nA);
        }
    }

    if (undoManager)
    {
        undoManager->beginNewTransaction();
        undoManager->perform(new EditActions::ClipsPropertyAction(
            std::move(oldStates), std::move(newStates), editChangeCb, clipAliveValidator()));
    }
    else
    {
        if (editChangeCb) editChangeCb();
    }
    repaint();
}

void TimelineView::splitAtSelection()
{
    if (!hasSelectionRange()) return;
    double t1 = loopStartTV;
    double t2 = loopEndTV;

    auto splitAtTime = [&](double t)
    {
        for (int ti = 0; ti < trackManager.getTrackCount(); ++ti)
        {
            auto* track = trackManager.getTrack(ti);
            for (int li = 0; li < track->getLaneCount(); ++li)
            {
                auto* lane = track->getLane(li);
                if (!lane) continue;
                std::vector<AudioClip*> snap;
                for (auto& cp : lane->clips) snap.push_back(cp.get());
                for (auto* clip : snap)
                {
                    if (clip->getStartPosition() < t - 0.001
                        && clip->getEndPosition() > t + 0.001)
                    {
                        if (undoManager)
                            undoManager->perform(new EditActions::ClipSplitAction(
                                lane, clip, t,
                                track->getFormatManager(),
                                track->getThumbnailCache(),
                                editChangeCb, editBeforeChangeCb));
                    }
                }
            }
        }
    };

    if (undoManager) undoManager->beginNewTransaction("Split at selection");
    splitAtTime(t1);
    splitAtTime(t2);

    // 範囲内のクリップを自動選択
    clearAllSelections();
    for (int ti = 0; ti < trackManager.getTrackCount(); ++ti)
    {
        auto* track = trackManager.getTrack(ti);
        for (int li = 0; li < track->getLaneCount(); ++li)
        {
            auto* lane = track->getLane(li);
            if (!lane) continue;
            for (auto& cp : lane->clips)
            {
                auto* clip = cp.get();
                if (clip->getStartPosition() >= t1 - 0.001
                    && clip->getEndPosition()   <= t2 + 0.001
                    && clip->getEndPosition()   >  clip->getStartPosition() + 0.001)
                {
                    ClipRef ref;
                    ref.track = track; ref.lane = lane; ref.clip = clip;
                    ref.trackIdx = ti; ref.laneIdx = li;
                    if (!selectedClip.valid()) selectedClip = ref;
                    else extraSelections.push_back(ref);
                }
            }
        }
    }
    repaint();
}
