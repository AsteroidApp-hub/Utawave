// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

// TimelineView の編集系操作 (削除/コピー/ペースト/カット/複製/結合/無音カット/
// クロスフェード/分割/選択/ナッジ/横ズーム/全体フィット 等)。
// マーカー操作・名前編集も含む。
// TimelineView.cpp が肥大化したため分割。

#include "TimelineView.h"
#include "../Localisation.h"
#include "../AppColours.h"
#include "../Tracks/MidiClip.h"
#include "../Edit/SilenceDetector.h"
#include "../Edit/ClipInsertGeometry.h"
#include "TextImageCache.h"
#include <set>
#include <map>
#include <utility>

namespace
{
    // クリップの端 (start/end) に別クリップが重なっているか = その端のフェードが
    // クロスフェード由来か。コピー/複製で別位置へ移すと重なり相手がいなくなるため、
    // クロスフェード由来の大きなフェードはそのまま持っていくと「相手のいない巨大フェード」
    // になり描画が崩れる。重なり端のフェードは小さな既定値にリセットする判定に使う。
    bool clipEdgeOverlapsNeighbor(Lane* lane, AudioClip* self, double edgeTime)
    {
        if (!lane) return false;
        for (auto& cp : lane->clips)
        {
            auto* o = cp.get();
            if (o == self) continue;
            if (o->getStartPosition() < edgeTime - 0.001
                && o->getEndPosition() > edgeTime + 0.001)
                return true;
        }
        return false;
    }
}

void TimelineView::deleteSelectedClips()
{
    if (!selectedClip.valid() && extraSelections.empty()) return;

    // 全選択クリップを集約
    std::vector<ClipRef> all;
    if (selectedClip.valid()) all.push_back(selectedClip);
    for (auto& r : extraSelections) all.push_back(r);

    if (undoManager) undoManager->beginNewTransaction();
    for (auto& r : all)
    {
        if (undoManager)
            undoManager->perform(new EditActions::ClipDeleteAction(r.lane, r.clip, editChangeCb));
        else
        {
            auto& clips = r.lane->clips;
            clips.erase(std::remove_if(clips.begin(), clips.end(),
                [&](const auto& c){ return c.get() == r.clip; }), clips.end());
        }
    }
    clearAllSelections();
    repaint();
}

void TimelineView::deleteSelectionRange()
{
    if (!hasSelectionRange() || !undoManager) return;
    const double t1 = loopStartTV;
    const double t2 = loopEndTV;

    // 対象レーンはハイライト表示 (drawTrackRows の選択範囲描画) と一致させる:
    // 選択中の行 (トラック, 可視レーン) 群 = getSelectionLaneRows()。単一行選択は
    // フォーカスレーンのみ、複数行またぎは Lane 0 / テイクレーンを視覚どおりに対象
    // (Lane 0 で止めた選択はテイクレーンを含まない)。フォーカス無効 (どのトラック上
    // でもない場所からの選択 = 全トラックが全高でハイライトされる) は全トラック・全レーン。
    std::vector<std::pair<Track*, Lane*>> targets;
    const auto rows = getSelectionLaneRows();
    if (!rows.empty())
    {
        for (auto& [ti, li] : rows)
        {
            auto* track = trackManager.getTrack(ti);
            auto* lane  = track ? track->getLane(li) : nullptr;
            if (track && lane) targets.push_back({ track, lane });
        }
    }
    else
    {
        for (int ti = 0; ti < trackManager.getTrackCount(); ++ti)
        {
            auto* track = trackManager.getTrack(ti);
            if (!track) continue;
            for (int li = 0; li < track->getLaneCount(); ++li)
                if (auto* lane = track->getLane(li))
                    targets.push_back({ track, lane });
        }
    }
    if (targets.empty()) return;

    // クリップ選択は範囲編集で破棄され得るので先に解除 (stale ポインタ防止)。
    // 範囲選択とフォーカスレーンは残す。
    clearAllSelections();

    using namespace ClipInsertGeometry;
    constexpr double kEps = 1e-4;
    const auto clipAlive = clipAliveValidator();

    undoManager->beginNewTransaction();
    for (auto& [track, lane] : targets)
    {
        // アクションがレーンを変更するので live イテレートしない
        std::vector<AudioClip*> snap;
        for (auto& cp : lane->clips) snap.push_back(cp.get());
        for (auto* c : snap)
        {
            // kXf=0: 削除は接合相手がいないのでクロスフェードは作らない (隙間が残る)。
            // 切り口には小さな既定フェード (0.010) を置いてプチッ音を防ぐ。
            const Plan plan = planInsertOverlap(c->getStartPosition(), c->getEndPosition(),
                                                t1, t2, t2 - t1, 0.0, kEps);
            if (plan.kind == OverlapKind::None) continue;
            if (plan.kind == OverlapKind::Covered)
            {
                undoManager->perform(new EditActions::ClipDeleteAction(lane, c, editChangeCb));
                continue;
            }

            EditActions::ClipState before; before.capture(c);
            const bool   isSplit    = (plan.kind == OverlapKind::Split);
            const double splitLocal = plan.rightFileOffsetDelta;
            const float  dbAtSplit  = (isSplit && ! before.gainPoints.empty())
                                      ? c->getEnvelopeDBAt(splitLocal) : 0.0f;

            if (plan.kind == OverlapKind::LeftCut || isSplit)
            {
                // 既存を「左部分」へ縮める
                EditActions::ClipState after = before;
                after.duration = plan.leftDuration;
                after.fadeOut  = juce::jmin(0.010, plan.leftDuration * 0.5);
                undoManager->perform(new EditActions::ClipsPropertyAction(
                    { before }, { after }, editChangeCb, clipAlive));
            }
            else  // RightCut: 既存の頭を詰めて右部分にする
            {
                EditActions::ClipState after = before;
                after.startPos   = plan.rightStart;
                after.fileOffset = before.fileOffset + plan.rightFileOffsetDelta;
                after.duration   = plan.rightDuration;
                after.fadeIn     = juce::jmin(0.010, plan.rightDuration * 0.5);
                undoManager->perform(new EditActions::ClipsPropertyAction(
                    { before }, { after }, editChangeCb, clipAlive));
            }

            if (isSplit)
            {
                // 右側末尾を新クリップとして残す (色/カーブ/エンベロープ引き継ぎは makeSplitTail)
                EditActions::ClipParams rp;
                rp.file       = c->getFile();
                rp.startPos   = plan.rightStart;
                rp.duration   = plan.rightDuration;
                rp.fileOffset = before.fileOffset + plan.rightFileOffsetDelta;
                rp.gain       = before.gain;
                rp.name       = before.name;
                rp.colour     = c->getColour();
                rp.fadeIn     = juce::jmin(0.010, plan.rightDuration * 0.5);
                rp.fadeOut    = juce::jmin(before.fadeOut, plan.rightDuration * 0.5);
                makeSplitTail(lane, rp, before.fadeOutCurve, c->hasCustomColour(),
                              before.gainPoints, splitLocal, dbAtSplit,
                              track->getFormatManager(), track->getThumbnailCache());
            }
        }
    }
    repaint();
}

void TimelineView::deleteSelectedMidiClip()
{
    if (!selectedMidiClip || !selectedMidiTrack) return;
    auto* clip  = selectedMidiClip;
    auto* track = selectedMidiTrack;
    clearAllSelections();  // 削除されるクリップを指す選択を先に解除
    // Undo 対応で削除 (内部で onMidiClipWillBeRemoved でピアノロールを閉じる)
    pushMidiReplaceAction(track, { clip }, {});
}


bool TimelineView::clipStillExists(AudioClip* target) const
{
    if (!target) return false;
    for (int ti = 0; ti < trackManager.getTrackCount(); ++ti)
    {
        auto* track = trackManager.getTrack(ti);
        if (!track) continue;
        for (int li = 0; li < track->getLaneCount(); ++li)
        {
            auto* lane = track->getLane(li);
            if (!lane) continue;
            for (auto& cp : lane->clips)
                if (cp.get() == target) return true;
        }
    }
    return false;
}

bool TimelineView::midiClipStillExists(MidiClip* target, Track* owner) const
{
    if (!target || !owner) return false;
    for (int ti = 0; ti < trackManager.getTrackCount(); ++ti)
    {
        auto* track = trackManager.getTrack(ti);
        if (track != owner) continue;
        for (int ci = 0; ci < track->getMidiClipCount(); ++ci)
            if (track->getMidiClip(ci) == target) return true;
        return false;  // track は生存しているがクリップは破棄済み
    }
    return false;
}

void TimelineView::deleteSelectedCrossfade()
{
    if (!selectedCrossfade.valid()) return;

    AudioClip* clipA = selectedCrossfade.clipA;
    AudioClip* clipB = selectedCrossfade.clipB;

    // UAF 防止: 選択中のクロスフェードが Undo / 削除等の構造編集で既に解放されていないか検証。
    // (selectedCrossfade は生ポインタ。構造編集後にクリアされず残ると解放済みメモリを指す)
    if (!clipStillExists(clipA) || !clipStillExists(clipB))
    {
        selectedCrossfade.clear();
        repaint();
        return;
    }

    EditActions::ClipState oldA; oldA.capture(clipA);
    EditActions::ClipState oldB; oldB.capture(clipB);

    // 重なりを解消: clipA を clipB の手前で終わらせる
    double newDur = clipB->getStartPosition() - clipA->getStartPosition();
    if (newDur > 0.01)
        clipA->setDuration(newDur);  // setDuration が fadeOut を新 duration に再クランプ

    // クロスフェード由来の長いフェードを既定値に戻す。これをしないと clipA に過大な
    // fadeOut が、clipB に重なり相手のいない長い fadeIn が残り、無音から立ち上がる等の
    // 想定外のフェードになる。両クリップを通常の突き合わせ状態へ戻す。
    clipA->setFadeOutSecs(0.010);
    clipB->setFadeInSecs(0.010);

    EditActions::ClipState newA; newA.capture(clipA);
    EditActions::ClipState newB; newB.capture(clipB);

    if (undoManager && (oldA.differsFrom(newA) || oldB.differsFrom(newB)))
    {
        undoManager->beginNewTransaction();
        undoManager->perform(new EditActions::ClipsPropertyAction(
            { oldA, oldB }, { newA, newB }, editChangeCb, clipAliveValidator()));
    }
    else if (editChangeCb) editChangeCb();

    selectedCrossfade.clear();
    repaint();
}

void TimelineView::copySelectedClips()
{
    clipboard.clear();
    clipboardSelectionStart = -1.0;

    // クロスフェード由来のフェード判定は clipEdgeOverlapsNeighbor (ファイル先頭) を使う。
    // コピー先には重なり相手がいないため、重なり端 (クロスフェード由来) のフェードは小さな
    // 既定値にリセットしてコピーする。重なり相手のいない端の意図的フェードは保持。
    auto& edgeIsCrossfade = clipEdgeOverlapsNeighbor;

    // ── 選択範囲がある場合: 範囲と重なるクリップ部分だけをコピー ──
    if (hasSelectionRange())
    {
        const double t1 = loopStartTV;
        const double t2 = loopEndTV;
        clipboardSelectionStart = t1;

        // 範囲コピーは「選択範囲を作ったトラック (フォーカストラック)」のみを対象にする。
        // 以前は全トラックを走査していたため、空白部から範囲選択してコピペすると、範囲内の
        // 全トラックの波形が混ざって貼り付けられカオスになっていた。どのトラック上でもない
        // 場所での選択 (フォーカス無効) はコピーしない。
        const int focusTi = selectionFocusTrackIdx;
        auto* track = (focusTi >= 0 && focusTi < trackManager.getTrackCount())
                      ? trackManager.getTrack(focusTi) : nullptr;
        if (track)
        {
            // 再生対象レーンを特定（ソロ優先、なければ Lane 0）
            int playLaneIdx = 0;
            for (int li = 1; li < track->getLaneCount(); ++li)
            {
                auto* l = track->getLane(li);
                if (l && l->soloed) { playLaneIdx = li; break; }
            }
            auto* lane = track->getLane(playLaneIdx);
            // フォーカストラックのみが対象。レーンが無ければコピーするものは無い。
            if (!lane) { clipboardSelectionStart = -1.0; return; }

            // クリップを順に走査し、後の clip による重なりミュートを反映してコピー
            for (size_t i = 0; i < lane->clips.size(); ++i)
            {
                auto* clip = lane->clips[i].get();
                double origCS = clip->getStartPosition();
                double origCE = clip->getEndPosition();

                // 後の clip (前面) に覆われていない「見える区間」を区間減算で求める。
                // 中抜き (中央被覆) でも head/tail の複数区間に正しく分割され、再生
                // (overlap-mute の #H9 対応) と一致する。
                std::vector<std::pair<double, double>> visible;
                visible.push_back({ origCS, origCE });
                for (size_t j = i + 1; j < lane->clips.size(); ++j)
                {
                    auto* clipJ = lane->clips[j].get();
                    const double js = clipJ->getStartPosition();
                    const double je = clipJ->getEndPosition();
                    std::vector<std::pair<double, double>> next;
                    for (auto& seg : visible)
                    {
                        const double s = seg.first, e2 = seg.second;
                        if (je <= s || js >= e2) { next.push_back(seg); continue; }  // 重ならない
                        if (js > s)  next.push_back({ s, juce::jmin(js, e2) });        // 左側の残り
                        if (je < e2) next.push_back({ juce::jmax(je, s), e2 });        // 右側の残り
                        // [max(js,s), min(je,e2)] は覆われるので捨てる
                    }
                    visible.swap(next);
                    if (visible.empty()) break;
                }

                for (auto& seg : visible)
                {
                    const double effCS = seg.first;
                    const double effCE = seg.second;
                    if (effCE <= effCS + 0.001) continue;

                    // 選択範囲との交差
                    const double interStart = juce::jmax(effCS, t1);
                    const double interEnd   = juce::jmin(effCE, t2);
                    const double interDur   = interEnd - interStart;
                    if (interDur < 0.005) continue;

                    ClipboardEntry e;
                    e.params.file       = clip->getFile();
                    e.params.startPos   = interStart;
                    e.params.duration   = interDur;
                    e.params.fileOffset = clip->getFileOffset() + (interStart - origCS);
                    e.params.fadeIn     = (interStart <= origCS + 0.001 && !edgeIsCrossfade(lane, clip, origCS))
                                          ? clip->getFadeInSecs()  : 0.010;
                    e.params.fadeOut    = (interEnd   >= origCE - 0.001 && !edgeIsCrossfade(lane, clip, origCE))
                                          ? clip->getFadeOutSecs() : 0.010;
                    e.params.gain       = clip->getGain();
                    e.params.name       = clip->getName();
                    e.params.colour     = clip->getColour();
                    e.params.customColour = clip->hasCustomColour();
                    e.sourceTrack       = track;
                    e.sourceLane        = lane;
                    clipboard.push_back(e);
                }
            }
        }
        return;
    }

    // ── 範囲がない場合: 選択クリップを丸ごとコピー（従来動作） ──
    std::vector<ClipRef> all;
    if (selectedClip.valid()) all.push_back(selectedClip);
    for (auto& r : extraSelections) all.push_back(r);
    if (all.empty()) return;

    for (auto& r : all)
    {
        ClipboardEntry e;
        e.params.file       = r.clip->getFile();
        e.params.startPos   = r.clip->getStartPosition();
        e.params.duration   = r.clip->getDuration();
        e.params.fileOffset = r.clip->getFileOffset();
        e.params.fadeIn     = edgeIsCrossfade(r.lane, r.clip, r.clip->getStartPosition())
                              ? 0.010 : r.clip->getFadeInSecs();
        e.params.fadeOut    = edgeIsCrossfade(r.lane, r.clip, r.clip->getEndPosition())
                              ? 0.010 : r.clip->getFadeOutSecs();
        e.params.gain       = r.clip->getGain();
        e.params.name       = r.clip->getName();
        e.params.colour     = r.clip->getColour();
        e.params.customColour = r.clip->hasCustomColour();
        e.sourceTrack       = r.track;
        e.sourceLane        = r.lane;
        clipboard.push_back(e);
    }
}

void TimelineView::cutSelectedClips()
{
    if (!selectedClip.valid() && extraSelections.empty()) return;
    copySelectedClips();
    deleteSelectedClips();
}

void TimelineView::pasteAtPlayhead(Track* preferredTrack)
{
    if (clipboard.empty()) return;

    // アンカー: 範囲コピー時は selection 開始、それ以外は最早の clip start
    double earliest;
    if (clipboardSelectionStart >= 0.0)
        earliest = clipboardSelectionStart;
    else
    {
        earliest = clipboard.front().params.startPos;
        for (auto& e : clipboard) earliest = juce::jmin(earliest, e.params.startPos);
    }

    if (undoManager) undoManager->beginNewTransaction();

    // ペーストは「最新 (上) を優先」: 追加するクリップと重なる既存クリップの重なり分をカットして
    // 重ならないようにする (録音やドラッグ重ねと同じ挙動)。重なったままだと最小クロスフェードを
    // 組めないことがあったため。すでに追加したペーストクリップ同士はカットしない (コピーした塊を保つ)。
    std::vector<AudioClip*> pastedClips;
    // 重なる既存クリップをカットし、接合部に最小クロスフェードを作る。ペーストクリップは後で
    // 最前面に追加されるので、重なり領域では既存側が下に隠れて「ずれ」て見えない。leftXfOut /
    // rightXfOut にペースト左右端のクロスフェード長を返し、呼び出し側がペーストクリップへ反映する。
    auto cutOverlapForPaste =
        [this](Lane* lane, double pStart, double pEnd,
               juce::AudioFormatManager& fmt, juce::AudioThumbnailCache& cache,
               const std::vector<AudioClip*>& pasted,
               double& leftXfOut, double& rightXfOut)
    {
        if (!lane || !undoManager) return;
        using namespace ClipInsertGeometry;
        constexpr double kEps = 1e-4;
        const double pasteDur = pEnd - pStart;
        // 先に対象を集める (アクションがレーンを変更するので live イテレートしない)
        std::vector<AudioClip*> targets;
        for (auto& cp : lane->clips)
        {
            auto* c = cp.get();
            bool isPasted = false;
            for (auto* p : pasted) if (p == c) { isPasted = true; break; }
            if (!isPasted) targets.push_back(c);
        }
        const auto clipAlive = clipAliveValidator();  // ループ外で 1 回構築 (各アクションへはコピー)
        for (auto* c : targets)
        {
            const Plan plan = planInsertOverlap(c->getStartPosition(), c->getEndPosition(),
                                                pStart, pEnd, pasteDur, kMinCrossfadeSecs, kEps);
            if (plan.kind == OverlapKind::None) continue;
            if (plan.kind == OverlapKind::Covered)
            {
                undoManager->perform(new EditActions::ClipDeleteAction(lane, c, editChangeCb));
                continue;
            }

            EditActions::ClipState before; before.capture(c);
            // Split は右側末尾を c のトリム前に控える (エンベロープ値 dbAtSplit を正確に取る)
            const bool isSplit = (plan.kind == OverlapKind::Split);
            const double splitLocal = plan.rightFileOffsetDelta;
            const float  dbAtSplit  = (isSplit && ! before.gainPoints.empty())
                                      ? c->getEnvelopeDBAt(splitLocal) : 0.0f;

            if (plan.kind == OverlapKind::LeftCut || plan.kind == OverlapKind::Split)
            {
                // 既存を「左部分」へ縮める + 接合部クロスフェード (0 のとき元フェードを尺で再クランプ)
                EditActions::ClipState after = before;
                after.duration = plan.leftDuration;
                after.fadeOut  = (plan.leftFadeOut > 0.001) ? plan.leftFadeOut
                                                            : juce::jmin(before.fadeOut, plan.leftDuration * 0.5);
                undoManager->perform(new EditActions::ClipsPropertyAction(
                    { before }, { after }, editChangeCb, clipAlive));
                leftXfOut = juce::jmax(leftXfOut, plan.insFadeIn);
            }
            else // RightCut
            {
                EditActions::ClipState after = before;
                after.startPos   = plan.rightStart;
                after.fileOffset = before.fileOffset + plan.rightFileOffsetDelta;
                after.duration   = plan.rightDuration;
                after.fadeIn     = (plan.rightFadeIn > 0.001) ? plan.rightFadeIn
                                                              : juce::jmin(before.fadeIn, plan.rightDuration * 0.5);
                undoManager->perform(new EditActions::ClipsPropertyAction(
                    { before }, { after }, editChangeCb, clipAlive));
                rightXfOut = juce::jmax(rightXfOut, plan.insFadeOut);
            }

            if (isSplit)
            {
                // 右側末尾を新クリップとして残す (色/カーブ/エンベロープ引き継ぎは makeSplitTail)
                EditActions::ClipParams rp;
                rp.file       = c->getFile();
                rp.startPos   = plan.rightStart;
                rp.duration   = plan.rightDuration;
                rp.fileOffset = before.fileOffset + plan.rightFileOffsetDelta;
                rp.gain       = before.gain;
                rp.name       = before.name;
                rp.colour     = c->getColour();
                rp.fadeIn     = (plan.rightFadeIn > 0.001) ? plan.rightFadeIn
                                                           : juce::jmin(0.010, plan.rightDuration * 0.5);
                rp.fadeOut    = juce::jmin(before.fadeOut, plan.rightDuration * 0.5);
                makeSplitTail(lane, rp, before.fadeOutCurve, c->hasCustomColour(),
                              before.gainPoints, splitLocal, dbAtSplit, fmt, cache);
                rightXfOut = juce::jmax(rightXfOut, plan.insFadeOut);
            }
        }
    };

    for (auto& e : clipboard)
    {
        Track* targetTrack = nullptr;
        Lane*  targetLane  = nullptr;
        // 1) preferredTrack（選択中トラック）が指定されていればそこへ
        if (preferredTrack && !preferredTrack->isClickTrack())
        {
            for (int i = 0; i < trackManager.getTrackCount(); ++i)
                if (trackManager.getTrack(i) == preferredTrack)
                {
                    targetTrack = preferredTrack;
                    targetLane  = preferredTrack->getLane(0);
                    break;
                }
        }
        // 2) 元のトラックが残っていればそこへ（クリックトラック除外）
        if (!targetTrack && e.sourceTrack && !e.sourceTrack->isClickTrack())
        {
            for (int i = 0; i < trackManager.getTrackCount(); ++i)
                if (trackManager.getTrack(i) == e.sourceTrack)
                {
                    targetTrack = e.sourceTrack;
                    targetLane  = e.sourceLane;
                    break;
                }
        }
        // 3) フォールバック: 最初のオーディオトラック（クリックトラック除外）
        if (!targetTrack)
        {
            for (int i = 0; i < trackManager.getTrackCount(); ++i)
            {
                auto* t = trackManager.getTrack(i);
                if (!t->isClickTrack()) { targetTrack = t; targetLane = t->getLane(0); break; }
            }
        }
        if (!targetTrack || !targetLane) continue;

        auto params = e.params;
        params.startPos = playheadSecs + (e.params.startPos - earliest);

        if (undoManager)
        {
            auto& fmt   = targetTrack->getFormatManager();
            auto& cache = targetTrack->getThumbnailCache();
            // 先に重なる既存クリップをカット (最新=ペーストを上に・録音と同様に重ねない)。
            // 接合部の最小クロスフェード長を受け取り、ペーストクリップの端フェードへ反映する
            // (両側にフェードが揃って初めて X が描かれ、等パワーで繋がる)。
            double leftXf = 0.0, rightXf = 0.0;
            cutOverlapForPaste(targetLane, params.startPos,
                               params.startPos + params.duration, fmt, cache, pastedClips,
                               leftXf, rightXf);
            if (leftXf  > 0.001) params.fadeIn  = juce::jmin(leftXf,  params.duration * 0.5);
            if (rightXf > 0.001) params.fadeOut = juce::jmin(rightXf, params.duration * 0.5);
            // ペーストクリップを追加し、以降のカット対象から除外するため記録する
            auto* add = new EditActions::ClipAddAction(targetLane, params, fmt, cache, editChangeCb);
            if (undoManager->perform(add))
                if (auto* added = add->getAddedClip())
                    pastedClips.push_back(added);
        }
    }
    repaint();
}

void TimelineView::duplicateSelectedClips()
{
    std::vector<ClipRef> all;
    if (selectedClip.valid()) all.push_back(selectedClip);
    for (auto& r : extraSelections) all.push_back(r);
    if (all.empty()) return;

    if (undoManager) undoManager->beginNewTransaction();
    for (auto& r : all)
    {
        auto* c = r.clip;
        EditActions::ClipParams p;
        p.file       = c->getFile();
        p.startPos   = c->getEndPosition();
        p.duration   = c->getDuration();
        p.fileOffset = c->getFileOffset();
        // クロスフェード由来の端フェードは複製先に相手がいないのでリセット (コピペと同じ)
        p.fadeIn     = clipEdgeOverlapsNeighbor(r.lane, c, c->getStartPosition())
                       ? 0.010 : c->getFadeInSecs();
        p.fadeOut    = clipEdgeOverlapsNeighbor(r.lane, c, c->getEndPosition())
                       ? 0.010 : c->getFadeOutSecs();
        p.gain       = c->getGain();
        p.name       = c->getName();
        p.colour     = c->getColour();
        p.customColour = c->hasCustomColour();

        if (undoManager)
        {
            undoManager->perform(new EditActions::ClipAddAction(
                r.lane, p,
                r.track->getFormatManager(),
                r.track->getThumbnailCache(),
                editChangeCb));
        }
    }
    repaint();
}


void TimelineView::setSelectionFocus(int trackIdx, int laneIdx)
{
    selectionFocusTrackIdx    = trackIdx;
    selectionFocusLaneIdx     = laneIdx;
    selectionFocusTrackEndIdx = -1;   // 外部からの単一レーン指定はスパンを畳む
    selectionFocusLaneEndIdx  = -1;
    repaint();
}

bool TimelineView::getSelectionTrackSpan(int& lo, int& hi) const
{
    const int count = trackManager.getTrackCount();
    if (selectionFocusTrackIdx < 0 || selectionFocusTrackIdx >= count) return false;
    lo = hi = selectionFocusTrackIdx;
    if (selectionFocusTrackEndIdx >= 0)
    {
        const int end = juce::jlimit(0, count - 1, selectionFocusTrackEndIdx);
        lo = juce::jmin(lo, end);
        hi = juce::jmax(hi, end);
    }
    return true;
}

std::vector<std::pair<int, int>> TimelineView::getSelectionLaneRows() const
{
    std::vector<std::pair<int, int>> rows;
    const int count = trackManager.getTrackCount();
    if (selectionFocusTrackIdx < 0 || selectionFocusTrackIdx >= count) return rows;

    // アンカー行ともう一端の行を視覚順 (トラック昇順 → レーン昇順) に正規化
    int tA = selectionFocusTrackIdx;
    int lA = juce::jmax(0, selectionFocusLaneIdx);
    int tB = tA, lB = lA;
    if (selectionFocusTrackEndIdx >= 0)
    {
        tB = juce::jlimit(0, count - 1, selectionFocusTrackEndIdx);
        lB = juce::jmax(0, selectionFocusLaneEndIdx);
    }
    if (tB < tA || (tB == tA && lB < lA))
    {
        std::swap(tA, tB);
        std::swap(lA, lB);
    }

    for (int t = tA; t <= tB; ++t)
    {
        auto* tr = trackManager.getTrack(t);
        if (!tr) continue;
        if (tr->getTotalHeight() <= 0) continue;   // 閉じたフォルダ配下 (非表示行) は対象外
        // 可視行のみ: 折りたたみ中は Lane 0 だけ (隠れているテイクレーンは対象にしない)
        const bool expanded = !tr->isLanesCollapsed() && tr->getLaneCount() > 1;
        const int lastLane  = expanded ? tr->getLaneCount() - 1 : 0;
        const int from = (t == tA) ? juce::jmin(lA, lastLane) : 0;
        const int to   = (t == tB) ? juce::jmin(lB, lastLane) : lastLane;
        for (int l = from; l <= to; ++l)
            rows.push_back({ t, l });
    }
    return rows;
}

bool TimelineView::isSelectionMultiRow() const
{
    return getSelectionLaneRows().size() > 1;
}

bool TimelineView::selectionRangeCoversTrack(int trackIdx) const
{
    if (!hasSelectionRange()) return false;
    int lo = 0, hi = 0;
    if (!getSelectionTrackSpan(lo, hi))
        return true;   // フォーカス無効 = 全トラック対象 (全高ハイライトと一致)
    return trackIdx >= lo && trackIdx <= hi;
}

bool TimelineView::laneRowAtY(int relY, int& trackIdx, int& laneIdx) const
{
    const int count = trackManager.getTrackCount();
    if (count <= 0) return false;

    int t = trackManager.trackAtY(relY);
    if (t < 0)
    {
        if (relY < 0) { trackIdx = 0; laneIdx = 0; return true; }   // 先頭より上
        // 末尾より下 → 最後の「可視」トラック (高さ 0 = 閉じたフォルダ配下は飛ばす) の
        // 最終可視レーンへクランプ
        t = count - 1;
        while (t > 0 && trackManager.getTrack(t)
               && trackManager.getTrack(t)->getTotalHeight() <= 0)
            --t;
        auto* tr = trackManager.getTrack(t);
        trackIdx = t;
        laneIdx  = (tr && !tr->isLanesCollapsed() && tr->getLaneCount() > 1)
                   ? tr->getLaneCount() - 1 : 0;
        return true;
    }

    auto* tr = trackManager.getTrack(t);
    int l = 0;
    if (tr && !tr->isLanesCollapsed() && tr->getLaneCount() > 1)
    {
        const int yInTrack = relY - trackManager.getTrackY(t);
        const int mainH    = tr->getMainHeight();
        if (yInTrack >= mainH)
            l = juce::jmin(1 + (yInTrack - mainH) / tr->getLaneHeight(),
                           tr->getLaneCount() - 1);
    }
    trackIdx = t;
    laneIdx  = l;
    return true;
}

std::vector<int> TimelineView::getInvolvedTrackIndices() const
{
    std::set<int> s;   // 自動でソート + 重複排除
    if (selectedClip.valid() && selectedClip.trackIdx >= 0)
        s.insert(selectedClip.trackIdx);
    for (auto& r : extraSelections)
        if (r.valid() && r.trackIdx >= 0)
            s.insert(r.trackIdx);
    if (selectedMidiClip != nullptr && selectedMidiTrack != nullptr)
        for (int i = 0; i < trackManager.getTrackCount(); ++i)
            if (trackManager.getTrack(i) == selectedMidiTrack) { s.insert(i); break; }
    if (hasSelectionRange())
    {
        int lo = 0, hi = 0;
        if (getSelectionTrackSpan(lo, hi))
            for (int ti = lo; ti <= hi; ++ti)
                s.insert(ti);
    }
    return std::vector<int>(s.begin(), s.end());
}

bool TimelineView::moveSelectionFocusLane(int delta)
{
    if (selectionFocusTrackIdx < 0
        || selectionFocusTrackIdx >= trackManager.getTrackCount())
        return false;
    // 複数行 (トラック/テイクレーン) またぎ選択中の ↑↓ はまずアンカー行単体へ畳む
    // (フォーカスレーン移動はテイク比較用の単一レーン操作のため)
    if (isSelectionMultiRow())
    {
        selectionFocusTrackEndIdx = -1;
        selectionFocusLaneEndIdx  = -1;
        repaint();
        return true;
    }
    auto* track = trackManager.getTrack(selectionFocusTrackIdx);
    if (!track) return false;

    int newIdx = selectionFocusLaneIdx + delta;
    if (newIdx < 0 || newIdx >= track->getLaneCount()) return false;

    selectionFocusLaneIdx = newIdx;
    repaint();
    return true;
}

bool TimelineView::copySelectionRangeToRecLane()
{
    Track* track = nullptr;
    Lane*  srcLane = nullptr;
    double t1 = 0.0, t2 = 0.0;

    // ① 範囲選択 + フォーカスレーンがあればそれを使用
    if (hasSelectionRange()
        && selectionFocusTrackIdx >= 0
        && selectionFocusLaneIdx > 0
        && selectionFocusTrackIdx < trackManager.getTrackCount())
    {
        track = trackManager.getTrack(selectionFocusTrackIdx);
        if (track) srcLane = track->getLane(selectionFocusLaneIdx);
        t1 = loopStartTV;
        t2 = loopEndTV;
    }
    // ② 範囲が無い場合: 選択中クリップ（テイクレーン上）の全範囲を使用
    else if (selectedClip.valid()
             && selectedClip.laneIdx > 0
             && selectedClip.track != nullptr
             && clipStillExists(selectedClip.clip))   // deref 前に生存確認 (UAF 防止)
    {
        track   = selectedClip.track;
        srcLane = selectedClip.lane;
        t1      = selectedClip.clip->getStartPosition();
        t2      = selectedClip.clip->getEndPosition();
    }
    else
    {
        return false;
    }

    return promoteRangeToLane0(track, srcLane, t1, t2);
}

// 指定したテイクレーンから Lane 0 へ範囲 [t1, t2] を採用する共通実体。
// copySelectionRangeToRecLane (Shift+↑) と promoteTakeLane (↑ ボタン / 右クリック「このテイクを使う」)
// の両方が呼ぶ。
bool TimelineView::promoteRangeToLane0(Track* track, Lane* srcLane, double t1, double t2)
{
    if (!track || !srcLane) return false;
    auto* dstLane = track->getLane(0);
    if (!dstLane) return false;
    if (t2 <= t1 + 0.001) return false;

    // 範囲と重なる src クリップを収集
    std::vector<AudioClip*> srcs;
    for (auto& cp : srcLane->clips)
        if (cp->getStartPosition() < t2 - 0.001
            && cp->getEndPosition()   > t1 + 0.001)
            srcs.push_back(cp.get());
    if (srcs.empty()) return false;

    // 全変更を 1 つの Undo 単位に束ねるためレーン全体のスナップショットを取る
    std::vector<EditActions::LaneSnapshotAction::ClipSnap> beforeSnap;
    for (auto& cp : dstLane->clips)
        beforeSnap.push_back(EditActions::LaneSnapshotAction::ClipSnap::capture(cp.get()));

    constexpr double kXfade = kMinCrossfadeSecs;   // テイク差し込みの最小クロスフェード (30ms)
    // 境界で 30ms overlap を残してトリムするため、トリム範囲を内側に狭める
    const double trimT1 = t1 + kXfade;
    const double trimT2 = t2 - kXfade;

    // ── Lane 0 の既存クリップで [trimT1, trimT2] と被る部分を分割/トリム/削除 ──
    auto& dstClips = dstLane->clips;
    std::vector<std::unique_ptr<AudioClip>> appended;
    for (auto it = dstClips.begin(); it != dstClips.end(); )
    {
        auto* clip = it->get();
        const double cs = clip->getStartPosition();
        const double ce = clip->getEndPosition();

        // 採用範囲を覆い、かつクロスフェード余白 (kXfade) を加えた [t1-kXfade, t2+kXfade] に
        // 収まるクリップは、範囲に完全に置き換えられる対象として丸ごと削除する。
        // 主目的は「同じ範囲への再差し替え」: 直前に差し込んだテイクは境界クロスフェードで
        // 範囲を最大 kXfade はみ出すため、そのまま分割すると両端に小さな断片が残り、新テイクが
        // 本来の隣ではなく断片とクロスフェードして右側の境界が壊れる。範囲外のクリップや、
        // 範囲を大きく超える本来の隣 (= はみ出しが kXfade より大きい) には影響しない。
        {
            const bool overlapsRange     = (ce > t1 + 0.001 && cs < t2 - 0.001);
            const bool containedInMargin = (cs >= t1 - kXfade - 0.001
                                            && ce <= t2 + kXfade + 0.001);
            if (overlapsRange && containedInMargin)
            {
                it = dstClips.erase(it);
                continue;
            }
        }

        if (ce <= trimT1 + 0.001 || cs >= trimT2 - 0.001) { ++it; continue; }

        if (cs >= trimT1 - 0.001 && ce <= trimT2 + 0.001)
        {
            it = dstClips.erase(it);
            continue;
        }

        if (cs < trimT1 - 0.001 && ce > trimT2 + 0.001)
        {
            const double origOffset      = clip->getFileOffset();
            const float  origGain        = clip->getGain();
            const juce::String origName  = clip->getName();
            const juce::Colour origColour = clip->getColour();
            const bool   origHasCustomCol = clip->hasCustomColour();

            clip->setDuration(trimT1 - cs);
            clip->setFadeOutSecs(juce::jmin(kXfade, clip->getDuration() * 0.5));

            auto rightClip = std::make_unique<AudioClip>(
                clip->getFile(), trimT2, ce - trimT2,
                track->getFormatManager(), track->getThumbnailCache());
            rightClip->setFileOffset(origOffset + (trimT2 - cs));
            rightClip->setGain(origGain);
            rightClip->setName(origName);
            if (origHasCustomCol) rightClip->setColour(origColour);
            rightClip->setFadeInSecs(juce::jmin(kXfade, rightClip->getDuration() * 0.5));
            appended.push_back(std::move(rightClip));

            ++it;
            continue;
        }

        if (cs < trimT1 - 0.001 && ce <= trimT2 + 0.001)
        {
            clip->setDuration(trimT1 - cs);
            clip->setFadeOutSecs(juce::jmin(kXfade, clip->getDuration() * 0.5));
            ++it;
            continue;
        }

        if (cs >= trimT1 - 0.001 && ce > trimT2 + 0.001)
        {
            const double trim = trimT2 - cs;
            clip->setStartPosition(trimT2);
            clip->setDuration(ce - trimT2);
            clip->setFileOffset(clip->getFileOffset() + trim);
            clip->setFadeInSecs(juce::jmin(kXfade, clip->getDuration() * 0.5));
            ++it;
            continue;
        }

        ++it;
    }
    for (auto& nc : appended) dstClips.push_back(std::move(nc));

    // ── 採用クリップを Lane 0 に追加 (両端にフェード適用) ──
    std::set<AudioClip*> newTakeClips;
    for (auto* src : srcs)
    {
        double clipS = src->getStartPosition();
        double clipE = src->getEndPosition();
        double rs = juce::jmax(t1, clipS);
        double re = juce::jmin(t2, clipE);
        if (re <= rs + 0.001) continue;

        double newDur    = re - rs;
        double newOffset = src->getFileOffset() + (rs - clipS);

        auto* nc = dstLane->addClip(src->getFile(), rs, newDur,
                                     track->getFormatManager(),
                                     track->getThumbnailCache());
        if (nc)
        {
            nc->setFileOffset(newOffset);
            nc->setGain(src->getGain());
            nc->setName(src->getName());
            // src がカスタム色を持っている場合のみ引き継ぐ。
            // 持っていない場合はトラック色に従わせるため何もしない (デフォルト)。
            if (src->hasCustomColour()) nc->setColour(src->getColour());
            // フェードは後段の「境界一元パス」が決める。ここでは小さなデフォルト
            // (クリック防止) だけ。隣がいない端はこの小フェード、隣がいる境界は一元パスが
            // クロスフェードに上書きする (重ねられない側に 30ms の単独三角を残さない)。
            nc->setFadeInSecs(0.005);
            nc->setFadeOutSecs(0.005);
            newTakeClips.insert(nc);
        }
    }

    // ── 古いクロスフェードの残骸を削除 ──
    // 新 take クリップ以外のクリップ同士が重なっていて、その重なり領域が
    // take の挿入範囲 [t1, t2] と交差する場合のみ解消する。
    // (take と無関係な既存クロスフェードはそのまま保持する)
    {
        auto& cs = dstLane->clips;
        bool changed = true;
        while (changed)
        {
            changed = false;
            for (size_t i = 0; i < cs.size() && !changed; ++i)
            {
                for (size_t j = 0; j < cs.size() && !changed; ++j)
                {
                    if (i == j) continue;
                    auto* A = cs[i].get();
                    auto* B = cs[j].get();
                    if (newTakeClips.count(A) || newTakeClips.count(B)) continue;
                    const double aS = A->getStartPosition();
                    const double aE = A->getEndPosition();
                    const double bS = B->getStartPosition();
                    const double bE = B->getEndPosition();
                    // A が左、B が右で重なる
                    if (!(aS <= bS && bS < aE - 0.001 && aE <= bE + 0.001)) continue;
                    // 重なり領域 [bS, aE] が take 範囲 [t1, t2] と交差しない場合はスキップ
                    const double ovS = bS;
                    const double ovE = aE;
                    if (ovE <= t1 + 0.001 || ovS >= t2 - 0.001) continue;

                    const double shift = aE - bS;
                    const double newBStart = aE;
                    const double newBDur   = bE - newBStart;
                    if (newBDur <= 0.01)
                    {
                        cs.erase(cs.begin() + (std::ptrdiff_t)j);
                    }
                    else
                    {
                        B->setStartPosition(newBStart);
                        B->setFileOffset(B->getFileOffset() + shift);
                        B->setDuration(newBDur);
                        B->setFadeInSecs(0.0);
                    }
                    A->setFadeOutSecs(0.0);
                    changed = true;
                }
            }
        }
    }

    // ── overlap が実在するクリップのみフェードを overlap に縮める ──
    // 旧クロスフェードの残骸 (= 隣に重なっている相手がいるのに fade が overlap を超える)
    // だけを対象にし、独立したクリップ (neighbor が無くフェードは意図的) は触らない。
    // また take 挿入範囲 [t1, t2] と完全に無関係なクリップも触らない。
    {
        auto& cs = dstLane->clips;
        for (auto& cpA : cs)
        {
            auto* A = cpA.get();
            const double aS = A->getStartPosition();
            const double aE = A->getEndPosition();
            // take 範囲と関係ないクリップはスキップ (フェード保持)
            if (aE <= t1 + 0.001 || aS >= t2 - 0.001) continue;

            double maxLeftOverlap  = 0.0;
            double maxRightOverlap = 0.0;
            for (auto& cpB : cs)
            {
                auto* B = cpB.get();
                if (B == A) continue;
                const double bS = B->getStartPosition();
                const double bE = B->getEndPosition();
                if (bS < aS && bE > aS)
                    maxLeftOverlap = juce::jmax(maxLeftOverlap, bE - aS);
                if (bS < aE && bE > aE)
                    maxRightOverlap = juce::jmax(maxRightOverlap, aE - bS);
            }
            // overlap が存在する場合のみ縮める (独立クリップのフェードは保持)
            if (maxLeftOverlap > 0.001
                && A->getFadeInSecs() > maxLeftOverlap + 0.001)
                A->setFadeInSecs(maxLeftOverlap);
            if (maxRightOverlap > 0.001
                && A->getFadeOutSecs() > maxRightOverlap + 0.001)
                A->setFadeOutSecs(maxRightOverlap);
        }
    }

    // ── take 境界のクロスフェードを一元決定する単一パス ──
    // dstLane->clips を開始順にソートし、隣接ペア (A=左, B=右) のみ処理する。
    // take↔take / take↔既存 を区別せず一様に扱い、左右で同じ方法で重なりを作る:
    //   ・同一連続音声 (分割) は重ねない (両側フェード 0 = 突き合わせ、コーム防止)
    //   ・重なり不足なら A のリードアウト と B のリードイン から対称に重ねる
    //     (take の左端 fileOffset≈0 でも、A=左隣のリードアウトで左側の重なりを作れる)
    //   ・両側に同値の相補フェードを設定 (片側だけの単独三角を根絶)
    //   ・どうしても重ねられない接触境界は両側に小フェード (対称・クリック防止)
    // 各ペアの調整は A.end / B.start しか動かさないので、隣の境界には波及しない。
    // take が絡む境界のみ対象 (無関係な既存境界は一切触らない)。
    {
        auto& clips = dstLane->clips;
        std::vector<AudioClip*> sorted;
        sorted.reserve(clips.size());
        for (auto& cp : clips) sorted.push_back(cp.get());
        std::sort(sorted.begin(), sorted.end(),
                  [](AudioClip* a, AudioClip* b){ return a->getStartPosition() < b->getStartPosition(); });

        auto fileLenOf = [](AudioClip* c) -> double {
            if (auto* r = c->getOrCreateReader())
                if (r->sampleRate > 0.0) return (double) r->lengthInSamples / r->sampleRate;
            return c->getThumbnail().getTotalLength();
        };

        for (size_t i = 0; i + 1 < sorted.size(); ++i)
        {
            AudioClip* A = sorted[i];      // 左
            AudioClip* B = sorted[i + 1];  // 右
            if (!newTakeClips.count(A) && !newTakeClips.count(B)) continue;  // take 境界のみ

            // 同一連続音声 (テイクを同じ位置に差し込んだ等) も突き合わせにせずクロスフェードを描く。
            // 重なり区間は同じファイルの同じ位置 = 同一サンプルなので、線形カーブなら
            // (1-t)*x + t*x = x で完全に透過する (コーム/レベルバンプ無し)。後段で curve を Linear に強制。
            const bool sameAudio = AudioClip::isSameContinuousAudio(*A, *B);

            double overlap = A->getEndPosition() - B->getStartPosition();
            if (overlap < -0.005) continue;   // 大きな隙間 (意図的な間) は触らない

            if (overlap < kXfade - 0.001)     // 重なり不足 → A 右 / B 左 へ対称に伸ばす
            {
                const double need     = kXfade - overlap;
                const double leadOutA = juce::jmax(0.0, fileLenOf(A) - (A->getFileOffset() + A->getDuration()));
                const double leadInB  = juce::jmax(0.0, B->getFileOffset());
                double extA = juce::jmin(leadOutA, need * 0.5);
                double extB = juce::jmin(leadInB,  need * 0.5);
                if (extA < need * 0.5) extB = juce::jmin(leadInB,  need - extA);  // 片側不足は他方で補う
                if (extB < need * 0.5) extA = juce::jmin(leadOutA, need - extB);
                if (extA > 0.0) A->setDuration(A->getDuration() + extA);          // A を右へ伸ばす
                if (extB > 0.0)                                                   // B を左へ伸ばす
                {
                    B->setStartPosition(B->getStartPosition() - extB);
                    B->setFileOffset   (B->getFileOffset()   - extB);
                    B->setDuration     (B->getDuration()     + extB);
                }
                overlap = A->getEndPosition() - B->getStartPosition();
            }

            if (overlap > 0.001)
            {
                const double fade = juce::jmin(overlap, A->getDuration() * 0.5, B->getDuration() * 0.5);
                if (sameAudio)
                {
                    // 同一連続音声は線形カーブで重なりを透過させる (上記参照)
                    A->setFadeOutCurve(FadeCurve::Linear);
                    B->setFadeInCurve(FadeCurve::Linear);
                }
                A->setFadeOutSecs(fade);   // 両側に同値 = 対称な X (#M1)
                B->setFadeInSecs(fade);
            }
            else if (sameAudio)
            {
                // 重ねられない端 (ファイル境界等) で連続音声: 突き合わせ (ディップ/クリック無し)
                A->setFadeOutSecs(0.0);
                B->setFadeInSecs(0.0);
            }
            else
            {
                // 重ねられない接触境界: 単独三角を残さず両側に小さな対称フェード
                A->setFadeOutSecs(0.005);
                B->setFadeInSecs(0.005);
            }
        }
    }

    // Undo 用に最終状態をスナップショットし、レーン全体置き換えアクションとして記録
    if (undoManager)
    {
        std::vector<EditActions::LaneSnapshotAction::ClipSnap> afterSnap;
        for (auto& cp : dstLane->clips)
            afterSnap.push_back(EditActions::LaneSnapshotAction::ClipSnap::capture(cp.get()));
        undoManager->beginNewTransaction("Use take");
        undoManager->perform(new EditActions::LaneSnapshotAction(
            dstLane, std::move(beforeSnap), std::move(afterSnap),
            track->getFormatManager(), track->getThumbnailCache(),
            editChangeCb, editBeforeChangeCb));
    }
    else if (editChangeCb) editChangeCb();
    refresh();
    repaint();
    return true;
}

bool TimelineView::canPromoteTakeLane(int trackIdx, int laneIdx) const
{
    if (laneIdx <= 0) return false;   // Lane 0 は採用先なので対象外
    if (trackIdx < 0 || trackIdx >= trackManager.getTrackCount()) return false;
    auto* track = trackManager.getTrack(trackIdx);
    if (!track) return false;
    auto* lane = track->getLane(laneIdx);
    if (!lane) return false;

    // ① 範囲選択あり: 範囲が「このトラック」を対象にしていて (別トラック上の範囲選択には
    //    反応しない)、そのレーンに範囲と重なるクリップがあれば採用可能
    if (selectionRangeCoversTrack(trackIdx))
    {
        for (auto& cp : lane->clips)
            if (cp->getStartPosition() < loopEndTV - 0.001
                && cp->getEndPosition()   > loopStartTV + 0.001)
                return true;
    }
    // ② クリップ選択中: 選択クリップがこのトラック・このテイクレーン上なら採用可能
    //    (生ポインタ比較のみ。deref しないので dangling でも安全)
    if (selectedClip.valid()
        && selectedClip.track   == track
        && selectedClip.laneIdx == laneIdx)
        return true;

    return false;
}

bool TimelineView::promoteTakeLane(int trackIdx, int laneIdx)
{
    if (laneIdx <= 0) return false;
    if (trackIdx < 0 || trackIdx >= trackManager.getTrackCount()) return false;
    auto* track = trackManager.getTrack(trackIdx);
    if (!track) return false;
    auto* srcLane = track->getLane(laneIdx);
    if (!srcLane) return false;

    double t1 = 0.0, t2 = 0.0;
    // 範囲選択が最優先 (ただしこのトラックを対象にしている時のみ。別トラック上の
    // 範囲選択は無視する = canPromoteTakeLane の活性条件と一致)。
    // 無ければ選択中クリップ全体を当てこむ。
    if (selectionRangeCoversTrack(trackIdx))
    {
        t1 = loopStartTV;
        t2 = loopEndTV;
    }
    else if (selectedClip.valid()
             && selectedClip.track   == track
             && selectedClip.laneIdx == laneIdx
             && clipStillExists(selectedClip.clip))   // UAF ガード (deref 前に生存確認)
    {
        t1 = selectedClip.clip->getStartPosition();
        t2 = selectedClip.clip->getEndPosition();
    }
    else
        return false;

    return promoteRangeToLane0(track, srcLane, t1, t2);
}

bool TimelineView::toggleFocusLaneSolo()
{
    Track* track = nullptr;
    Lane*  lane  = nullptr;

    // ① フォーカスレーンが Take レーンならそれを使う
    if (selectionFocusTrackIdx >= 0
        && selectionFocusLaneIdx > 0
        && selectionFocusTrackIdx < trackManager.getTrackCount())
    {
        track = trackManager.getTrack(selectionFocusTrackIdx);
        if (track) lane = track->getLane(selectionFocusLaneIdx);
    }
    // ② フォーカス無しでも、Take レーン上のクリップが選択されていればそれを使う
    else if (selectedClip.valid()
             && selectedClip.laneIdx > 0
             && selectedClip.track != nullptr
             && selectedClip.lane  != nullptr)
    {
        track = selectedClip.track;
        lane  = selectedClip.lane;
    }

    if (!track || !lane) return false;

    bool newState = !lane->soloed;
    // 排他: 同じトラックの他レーンの Solo を解除
    for (int li = 1; li < track->getLaneCount(); ++li)
        if (auto* l = track->getLane(li)) l->soloed = false;
    lane->soloed = newState;

    if (editChangeCb) editChangeCb();
    repaint();
    return true;
}

void TimelineView::selectAllClips()
{
    clearAllSelections();
    bool primarySet = false;
    for (int ti = 0; ti < trackManager.getTrackCount(); ++ti)
    {
        auto* track = trackManager.getTrack(ti);
        for (int li = 0; li < track->getLaneCount(); ++li)
        {
            auto* lane = track->getLane(li);
            if (!lane) continue;
            for (auto& cPtr : lane->clips)
            {
                ClipRef ref;
                ref.track = track; ref.lane = lane; ref.clip = cPtr.get();
                ref.trackIdx = ti; ref.laneIdx = li;
                if (!primarySet) { selectedClip = ref; primarySet = true; }
                else extraSelections.push_back(ref);
            }
        }
    }
    repaint();
}

void TimelineView::nudgeSelectedClips(double seconds)
{
    std::vector<ClipRef> all;
    if (selectedClip.valid()) all.push_back(selectedClip);
    for (auto& r : extraSelections) all.push_back(r);
    if (all.empty()) return;

    std::vector<EditActions::ClipState> oldStates, newStates;
    for (auto& r : all)
    {
        EditActions::ClipState oldS; oldS.capture(r.clip);
        oldStates.push_back(oldS);
        double newStart = juce::jmax(0.0, r.clip->getStartPosition() + seconds);
        r.clip->setStartPosition(newStart);
        EditActions::ClipState newS; newS.capture(r.clip);
        newStates.push_back(newS);
    }

    if (undoManager)
    {
        bool changed = false;
        for (size_t i = 0; i < oldStates.size(); ++i)
            if (oldStates[i].differsFrom(newStates[i])) { changed = true; break; }
        if (changed)
        {
            undoManager->beginNewTransaction("Nudge");
            undoManager->perform(new EditActions::ClipsPropertyAction(
                std::move(oldStates), std::move(newStates), editChangeCb, clipAliveValidator()));
        }
    }
    repaint();
}
void TimelineView::applyHorizontalZoomStep(double deltaY)
{
    // Cmd+スクロールと同じロジック (再生バー中心)。1 ステップ = 倍率一定 (×2 / ÷2) の指数ズーム
    const double bps      = bpm / 60.0;
    const double contentW = (double) getContentArea().getWidth();
    pixelsPerBeat = juce::jlimit(1.0, maxPixelsPerBeat(), pixelsPerBeat * std::pow(2.0, deltaY));
    scrollX = juce::jmax(0.0, playheadSecs * bps * pixelsPerBeat - contentW * 0.5);
    ruler.setPlayheadX(playheadSecs * bps * pixelsPerBeat);
    ruler.setPixelsPerBeat(pixelsPerBeat);
    ruler.setScrollX(scrollX);
    hScrollBar.setCurrentRange(scrollX, hScrollBar.getCurrentRangeSize());
    resized();
    repaint();
}

AudioClip* TimelineView::makeSplitTail(Lane* lane, const EditActions::ClipParams& params,
                                       FadeCurve srcFadeOutCurve, bool srcHasCustomColour,
                                       const std::vector<GainPoint>& srcGainPoints,
                                       double splitLocalSecs, float dbAtSplit,
                                       juce::AudioFormatManager& fmt, juce::AudioThumbnailCache& cache)
{
    AudioClip* tail = nullptr;
    if (undoManager)
    {
        // ClipAddAction は同一インスタンスを延命所有するので、追加後に設定した値は undo/redo を跨ぐ
        auto* add = new EditActions::ClipAddAction(lane, params, fmt, cache, editChangeCb);
        undoManager->perform(add);
        tail = add->getAddedClip();
    }
    else
    {
        // undoManager 無し (テスト等のフォールバック): 直接追加
        tail = lane->addClip(params.file, params.startPos, params.duration, fmt, cache);
        if (tail)
        {
            tail->setFileOffset(params.fileOffset);
            tail->setFadeInSecs(params.fadeIn);
            tail->setFadeOutSecs(params.fadeOut);
            tail->setGain(params.gain);
            if (params.name.isNotEmpty()) tail->setName(params.name);
        }
    }
    if (tail != nullptr)
    {
        tail->setFadeOutCurve(srcFadeOutCurve);
        // 色は params.customColour に依らずここで確定する (setColour は無条件で
        // customColour 化するため、元がトラック色追従なら resetColour で追従に戻す)。
        if (srcHasCustomColour) tail->setColour(params.colour);
        else                    tail->resetColour();
        // ゲインエンベロープの右側を時刻シフトして引き継ぐ
        if (! srcGainPoints.empty())
            tail->getGainPointsRW() = ClipInsertGeometry::shiftRightEnvelope(
                srcGainPoints, splitLocalSecs, dbAtSplit);
    }
    return tail;
}

void TimelineView::scrollByTracks(int steps)
{
    // 縦スクロールを「スナップ境界」単位で送る。境界は各トラックの先頭 + テイクリストが
    // 展開されているトラックの各テイクレーン先頭。これにより、折りたたみ中はトラック 1 つずつ、
    // テイクリスト展開中はテイクレーン 1 つずつスクロールできる (テイクが増えても一気に飛ばない)。
    // メイン部 (≤maxHeight) もレーン (laneHeight) も常にビューポートより低いので、境界スナップ
    // だけで全域に届く (旧来の「トラックがビューポートより高い時のページ送り」は不要になった)。
    const int count = trackManager.getTrackCount();
    if (count <= 0 || steps == 0) return;

    const int viewportH = getContentArea().getHeight();
    const int totalH    = trackManager.getTotalHeight();
    // maxScroll は vScrollBar と同じ「末尾余白込み」(jmax(400, totalH+200)) で計算する。
    // これを合わせないとホイールがスクロールバーより手前 (余白の分) で止まる。
    const int maxScroll = juce::jmax(0, juce::jmax(400, totalH + 200) - viewportH);

    // スナップ境界を昇順で収集 (トラックを上から走査するので自然に昇順)。
    std::vector<int> bounds;
    bounds.reserve((size_t) count * 2 + 1);
    int y = 0;
    for (int i = 0; i < count; ++i)
    {
        auto* t = trackManager.getTrack(i);
        if (t == nullptr) continue;
        if (t->getTotalHeight() <= 0) continue;           // 閉じたフォルダ配下は境界を作らない
        bounds.push_back(y);                              // トラック先頭 (メイン部)
        const int laneCount = t->getLaneCount();
        if (! t->isLanesCollapsed() && laneCount > 1)     // テイクリスト展開中
        {
            int ly = y + t->getMainHeight();
            for (int l = 1; l < laneCount; ++l)           // 各テイクレーン先頭
            {
                if (ly > maxScroll) break;                // 範囲外 (下端余白に入る分) は maxScroll で代表
                bounds.push_back(ly);
                ly += t->getLaneHeight();
            }
        }
        y += t->getTotalHeight();
    }
    // 末尾 (下端余白込みの最大スクロール) まで届くよう maxScroll を最終境界に加える。
    if (bounds.empty() || bounds.back() < maxScroll)
        bounds.push_back(maxScroll);

    // 現在位置 (scrollY 以下で最大の境界) のインデックス。
    int idx = 0;
    for (int i = 0; i < (int) bounds.size(); ++i)
    {
        if (bounds[(size_t) i] <= scrollY) idx = i;
        else break;
    }

    // 下: idx+steps。上: scrollY が境界からズレている (途中にいる) なら基準を 1 つ繰り上げる
    // (例: 途中で 1 つ上へ = 直前の境界 = bounds[idx] へ snap)。
    int targetIdx = idx + steps;
    if (steps < 0 && bounds[(size_t) idx] != scrollY) targetIdx += 1;
    targetIdx = juce::jlimit(0, (int) bounds.size() - 1, targetIdx);

    const int target = juce::jlimit(0, maxScroll, bounds[(size_t) targetIdx]);
    if (target == scrollY) return;
    scrollY = target;
    vScrollBar.setCurrentRange(scrollY, vScrollBar.getCurrentRangeSize());
    if (onVerticalScroll) onVerticalScroll(scrollY);
    repaint();
}

void TimelineView::applyVerticalZoomStep(double deltaY)
{
    // Shift+Option+スクロールと同じ (波形振幅ズーム)
    waveformZoom = juce::jlimit(0.1, 6.0, waveformZoom * std::pow(1.5, deltaY));
    repaint();
}
void TimelineView::resetVerticalZoom()
{
    // 波形振幅を既定 (ピーク 0dB がレーン全高まで届く見た目) に戻す
    waveformZoom = 1.0;
    repaint();
}
void TimelineView::zoomToFitAll()
{
    // 全クリップの末尾を集計
    double contentEndSec = 0.0;
    for (int ti = 0; ti < trackManager.getTrackCount(); ++ti)
    {
        auto* tr = trackManager.getTrack(ti);
        if (!tr) continue;
        for (int li = 0; li < tr->getLaneCount(); ++li)
        {
            auto* ln = tr->getLane(li);
            if (!ln) continue;
            for (auto& c : ln->clips)
                if (c) contentEndSec = juce::jmax(contentEndSec, c->getEndPosition());
        }
        for (int ci = 0; ci < tr->getMidiClipCount(); ++ci)
            if (auto* mc = tr->getMidiClip(ci))
                contentEndSec = juce::jmax(contentEndSec, mc->getEndPosition());
    }

    // 2 小節分のマージン (4/4 なら 8 拍)
    const int meterNum = juce::jmax(1, appSettings.meterNumerator);
    const double extraBeats = 2.0 * meterNum;
    const double bps = bpm / 60.0;
    const double totalBeats = juce::jmax(8.0, contentEndSec * bps + extraBeats);

    const double contentW = (double)getContentArea().getWidth();
    if (contentW <= 0.0 || totalBeats <= 0.0) return;

    const double newPxPerBeat = juce::jlimit(1.0, 2000.0, contentW / totalBeats);
    pixelsPerBeat = newPxPerBeat;
    scrollX = 0.0;

    ruler.setPlayheadX(playheadSecs * bps * pixelsPerBeat);
    ruler.setPixelsPerBeat(pixelsPerBeat);
    ruler.setScrollX(scrollX);
    hScrollBar.setCurrentRange(scrollX, hScrollBar.getCurrentRangeSize());
    resized();
    repaint();
}


//==============================================================================
// D&D ドロップターゲット
//==============================================================================
