// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

// TimelineView の MIDI クリップ系ヘルパ・ダブルクリック生成・クリップ名インライン編集
// (TimelineView_Mouse.cpp から分割)。

#include "TimelineView.h"
#include "../Localisation.h"
#include "../AppColours.h"
#include "../Tracks/MidiClip.h"
#include "../Edit/SilenceDetector.h"
#include "../Audio/BpmDetector.h"
#include "../Audio/LufsMeter.h"
#include "../MIDI/MidiRecorder.h"
#include "TextImageCache.h"
#include <set>
#include <map>

TimelineView::MidiClipHit TimelineView::getMidiClipAt(int x, int y) const
{
    MidiClipHit r;
    auto area = getContentArea();
    const double bps = bpm / 60.0;
    const double tSec = (x - area.getX() + scrollX) / juce::jmax(1e-9, pixelsPerBeat * bps);

    for (int ti = 0; ti < trackManager.getTrackCount(); ++ti)
    {
        auto* tr = trackManager.getTrack(ti);
        if (!tr || !tr->isMidiTrack()) continue;
        const int trackTop = area.getY() + trackManager.getTrackY(ti) - scrollY;
        const int mainH    = tr->getMainHeight();
        const int trackBottom = trackTop + mainH;
        if (y < trackTop || y >= trackBottom) continue;
        for (int ci = 0; ci < tr->getMidiClipCount(); ++ci)
        {
            auto* mc = tr->getMidiClip(ci);
            if (!mc) continue;
            if (tSec >= mc->getStartPosition() && tSec <= mc->getEndPosition())
            {
                r.clip     = mc;
                r.track    = tr;
                r.trackIdx = ti;
                // ヘッダ (タイトル) 領域: クリップ上端から min(14, mainH/4) px
                const int headerH = juce::jmin(14, mainH / 4);
                r.isHeader = (y >= trackTop + 1 && y <= trackTop + headerH);
                // 左右端 6px = リサイズハンドル (クリップ幅が十分にあるとき)
                const double bps2 = bpm / 60.0;
                const int cx = (int)(mc->getStartPosition() * bps2 * pixelsPerBeat - scrollX);
                const int cw = juce::jmax(4, (int)(mc->getDuration() * bps2 * pixelsPerBeat));
                if (cw >= 16)
                {
                    if (x <= cx + 6)        { r.leftEdge  = true; r.isHeader = false; }
                    else if (x >= cx + cw - 6) { r.rightEdge = true; r.isHeader = false; }
                }
                return r;
            }
        }
    }
    return r;
}

Track* TimelineView::midiTrackAtY(int y, int& outTrackIdx) const
{
    outTrackIdx = -1;
    auto area = getContentArea();
    for (int ti = 0; ti < trackManager.getTrackCount(); ++ti)
    {
        auto* tr = trackManager.getTrack(ti);
        if (!tr || !tr->isMidiTrack()) continue;
        const int trackTop    = area.getY() + trackManager.getTrackY(ti) - scrollY;
        const int trackBottom = trackTop + tr->getMainHeight();
        if (y >= trackTop && y < trackBottom) { outTrackIdx = ti; return tr; }
    }
    return nullptr;
}

double TimelineView::barLengthSecs() const
{
    const int num = juce::jmax(1, appSettings.meterNumerator);
    const int den = juce::jmax(1, appSettings.meterDenominator);
    // 1 拍 = 4/den 四分音符。1 小節 = num 拍。
    return (60.0 / juce::jmax(1.0, bpm)) * num * (4.0 / den);
}

double TimelineView::gridUnitSecs() const
{
    const double unit = snapModeUnitSecs(appSettings.snapMode, bpm);
    // Off 時の最小サイズは 1 拍 (四分音符)
    return unit > 0.0 ? unit : (60.0 / juce::jmax(1.0, bpm));
}

void TimelineView::clearMidiSelectionIfStale()
{
    auto stillExists = [this](MidiClip* c) -> bool
    {
        if (c == nullptr) return false;
        for (int ti = 0; ti < trackManager.getTrackCount(); ++ti)
        {
            auto* tr = trackManager.getTrack(ti);
            if (!tr || !tr->isMidiTrack()) continue;
            for (int ci = 0; ci < tr->getMidiClipCount(); ++ci)
                if (tr->getMidiClip(ci) == c) return true;
        }
        return false;
    };
    // 追加選択のうち破棄されたものを間引く
    extraMidiSelections.erase(
        std::remove_if(extraMidiSelections.begin(), extraMidiSelections.end(),
                       [&](const auto& p) { return !stillExists(p.first); }),
        extraMidiSelections.end());
    if (selectedMidiClip != nullptr && !stillExists(selectedMidiClip))
    {
        // プライマリが消えたら生存している追加選択を昇格 (無ければ解除)
        if (!extraMidiSelections.empty())
        {
            selectedMidiClip  = extraMidiSelections.front().first;
            selectedMidiTrack = extraMidiSelections.front().second;
            extraMidiSelections.erase(extraMidiSelections.begin());
        }
        else
        {
            selectedMidiClip  = nullptr;
            selectedMidiTrack = nullptr;
        }
    }
}

// 範囲選択 (setSelectionRange) のたびに呼ばれ、対象行にある MIDI クリップを
// MIDI 選択 (primary + extraMidiSelections) へ同期する。範囲と重なるクリップが対象
// (完全内包は要求しない = 掴み損ねに寛容)。範囲が有効な間は MIDI 選択を毎回置き換えるので、
// ドラッグで範囲を縮めれば外れたクリップの選択も解除される。範囲が無効なら何もしない
// (Cmd/Shift+クリックで作った選択を壊さない)
void TimelineView::syncMidiSelectionToRange()
{
    if (!hasSelectionRange()) return;

    // 対象トラック = 範囲選択がハイライトしている行のトラック (フォーカス無効なら全トラック)
    std::set<int> trackSet;
    auto rows = getSelectionLaneRows();
    if (rows.empty())
        for (int i = 0; i < trackManager.getTrackCount(); ++i) trackSet.insert(i);
    else
        for (const auto& r : rows) trackSet.insert(r.first);

    const double t1 = loopStartTV, t2 = loopEndTV;
    selectedMidiClip  = nullptr;
    selectedMidiTrack = nullptr;
    extraMidiSelections.clear();
    for (int ti : trackSet)
    {
        auto* tr = trackManager.getTrack(ti);
        if (!tr || !tr->isMidiTrack()) continue;
        for (int ci = 0; ci < tr->getMidiClipCount(); ++ci)
        {
            auto* mc = tr->getMidiClip(ci);
            if (!mc) continue;
            if (mc->getEndPosition() <= t1 + 1e-6 || mc->getStartPosition() >= t2 - 1e-6)
                continue;   // 範囲と重ならない
            if (selectedMidiClip == nullptr)
            {
                selectedMidiClip  = mc;
                selectedMidiTrack = tr;
            }
            else
            {
                extraMidiSelections.emplace_back(mc, tr);
            }
        }
    }
}

void TimelineView::pushMidiReplaceAction(Track* track,
                                         std::vector<MidiClip*> toRemove,
                                         std::vector<EditActions::MidiClipReplaceAction::NewMidiClip> toAdd,
                                         bool newTransaction)
{
    if (!track) return;
    auto onChange = [this]
    {
        clearMidiSelectionIfStale();
        if (editChangeCb) editChangeCb();
        repaint();
    };
    auto willRemove = [this](MidiClip* c)
    {
        if (onMidiClipWillBeRemoved) onMidiClipWillBeRemoved(c);
    };

    if (undoManager)
    {
        if (newTransaction)
            undoManager->beginNewTransaction();
        undoManager->perform(new EditActions::MidiClipReplaceAction(
            track, std::move(toRemove), std::move(toAdd), onChange, willRemove));
    }
    else
    {
        // Undo マネージャ未設定時のフォールバック (直接適用)
        for (auto* c : toRemove)
        {
            willRemove(c);
            for (int ci = 0; ci < track->getMidiClipCount(); ++ci)
                if (track->getMidiClip(ci) == c) { track->removeMidiClip(ci); break; }
        }
        for (const auto& np : toAdd)
            if (auto* c = track->addMidiClip(np.startPos, np.duration))
            {
                c->setName(np.name); c->setColour(np.colour); c->setChannel(np.channel);
                c->getSequence() = np.sequence;
            }
        onChange();
    }
}

void TimelineView::splitMidiClip(Track* track, MidiClip* clip, double absSplitTime)
{
    if (!track || !clip) return;
    const double cs = clip->getStartPosition();
    const double ce = clip->getEndPosition();
    // 分割位置を GRID にスナップ (Off なら正確なクリック位置)
    const double splitAbs = snapTime(absSplitTime);
    if (splitAbs <= cs + 1e-4 || splitAbs >= ce - 1e-4) return;  // 端すぎ → 何もしない
    const double relSplit = splitAbs - cs;

    // left / right を NewMidiClip として組み立て (元クリップは Undo 用に保持して差し替え)
    using NewMidiClip = EditActions::MidiClipReplaceAction::NewMidiClip;
    NewMidiClip left, right;
    left.startPos  = cs;       left.duration  = relSplit;
    right.startPos = splitAbs;  right.duration = ce - splitAbs;
    for (auto* np : { &left, &right })
    {
        np->name    = clip->getName();
        np->colour  = clip->getColour();
        np->channel = clip->getChannel();
    }

    // ノートを開始位置で左右に振り分け (境界を跨ぐノートは左側で relSplit に切り詰め)
    auto& seq = clip->getSequence();
    for (int i = 0; i < seq.getNumEvents(); ++i)
    {
        auto* ev = seq.getEventPointer(i);
        const auto& m = ev->message;
        if (!m.isNoteOn())
        {
            // CC / ピッチベンド等 (ピアノロールの下部レーンで編集) も時刻で振り分けて保全する
            if (!m.isNoteOff())
            {
                auto copy = m;
                if (m.getTimeStamp() < relSplit)
                    left.sequence.addEvent(copy);
                else
                {
                    copy.setTimeStamp(m.getTimeStamp() - relSplit);
                    right.sequence.addEvent(copy);
                }
            }
            continue;
        }
        const double onT  = m.getTimeStamp();
        const int    offI = seq.getIndexOfMatchingKeyUp(i);
        const double offT = (offI >= 0) ? seq.getEventPointer(offI)->message.getTimeStamp() : onT + 0.25;

        auto addNote = [&](NewMidiClip& dst, double on, double off)
        {
            if (off - on <= 0.001) return;
            auto nOn  = juce::MidiMessage::noteOn (m.getChannel(), m.getNoteNumber(), m.getFloatVelocity());
            auto nOff = juce::MidiMessage::noteOff(m.getChannel(), m.getNoteNumber());
            nOn.setTimeStamp(on); nOff.setTimeStamp(off);
            dst.sequence.addEvent(nOn);
            dst.sequence.addEvent(nOff);
        };

        if (onT < relSplit) addNote(left,  onT,            juce::jmin(offT, relSplit));
        else                addNote(right, onT - relSplit, offT - relSplit);
    }
    left.sequence.updateMatchedPairs();
    right.sequence.updateMatchedPairs();

    clearAllSelections();  // 元クリップを指す選択を先に解除 (差し替えで破棄されるため)
    pushMidiReplaceAction(track, { clip }, { std::move(left), std::move(right) });
}

// 選択中の MIDI クリップ (プライマリ + 追加選択) のうち track 上のものを 1 クリップへ結合する。
// 範囲 = [最小開始, 最大終端]。ノート/CC はクリップ相対時刻を保ったまま結合クリップへ写す
// (録音でクリップが複数に分かれた時に 1 つへまとめる用途・要望 2026-07)。1 Undo で往復
void TimelineView::mergeSelectedMidiClips(Track* track)
{
    if (track == nullptr) return;
    std::vector<MidiClip*> targets;
    if (selectedMidiTrack == track && selectedMidiClip != nullptr)
        targets.push_back(selectedMidiClip);
    for (const auto& p : extraMidiSelections)
        if (p.second == track && p.first != nullptr)
            targets.push_back(p.first);
    if ((int) targets.size() < 2) return;

    // 開始順に並べ、範囲と結合シーケンスを組み立てる
    std::sort(targets.begin(), targets.end(),
              [](const MidiClip* a, const MidiClip* b)
              { return a->getStartPosition() < b->getStartPosition(); });
    const double newStart = targets.front()->getStartPosition();
    double newEnd = newStart;
    for (auto* c : targets) newEnd = juce::jmax(newEnd, c->getEndPosition());

    using NewMidiClip = EditActions::MidiClipReplaceAction::NewMidiClip;
    NewMidiClip merged;
    merged.startPos = newStart;
    merged.duration = newEnd - newStart;
    merged.name     = targets.front()->getName();
    merged.colour   = targets.front()->getColour();
    merged.channel  = targets.front()->getChannel();
    for (auto* c : targets)
        merged.sequence.addSequence(c->getSequence(), c->getStartPosition() - newStart);
    merged.sequence.updateMatchedPairs();

    clearAllSelections();  // 元クリップを指す選択を先に解除 (差し替えで破棄されるため)
    pushMidiReplaceAction(track, std::move(targets), { std::move(merged) });
}

// ── MIDI 録音の確定配置 ──────────────────────────────────────────────────
// キャプチャ列 (タイムライン秒タイムスタンプ) を小節単位のクリップにして track へ置く。
// 重なった既存クリップは置換し、録音範囲の外へはみ出した部分だけを断片として残す
// (音声のパンチイン trimAndCrossfadeOnLane0 と同じ「上書き」作法・テイクレーンは作らない)。
// 全体を 1 つの MidiClipReplaceAction で積む = Cmd+Z 1 回で録音前へ戻る
void TimelineView::placeRecordedMidiClip(Track* track,
                                         const std::vector<juce::MidiMessage>& absEvents,
                                         double stopPos)
{
    if (track == nullptr) return;
    double firstOn = 0.0, lastEnd = 0.0;
    if (!MidiRecorder::noteSpan(absEvents, stopPos, firstOn, lastEnd))
        return;   // ノートが 1 つも無ければクリップを作らない (CC だけの誤爆防止)
    firstOn = juce::jmax(0.0, firstOn);
    lastEnd = juce::jmax(lastEnd, firstOn + 0.01);

    // クリップ範囲は小節単位 (MIDI クリップの既存仕様)。先頭 = 最初のノートを含む小節頭、
    // 末尾 = 最後のノート終端を含む小節の次の小節頭 (境界ちょうどはそのまま)
    const double clipStart = ruler.barStartSecs(ruler.barAtTime(firstOn));
    const double clipEnd   = ruler.barStartSecs(
        ruler.barAtTime(juce::jmax(clipStart, lastEnd) - 1e-6) + 1);
    if (clipEnd <= clipStart + 1e-6) return;

    using NewMidiClip = EditActions::MidiClipReplaceAction::NewMidiClip;
    NewMidiClip np;
    np.startPos = clipStart;
    np.duration = clipEnd - clipStart;
    np.name     = track->getName();
    np.sequence = MidiRecorder::buildClipSequence(absEvents, clipStart, clipEnd, stopPos);

    std::vector<MidiClip*>   removes;
    std::vector<NewMidiClip> adds;
    for (int ci = 0; ci < track->getMidiClipCount(); ++ci)
    {
        auto* c = track->getMidiClip(ci);
        if (c == nullptr) continue;
        const double cs = c->getStartPosition();
        const double ce = c->getEndPosition();
        if (ce <= clipStart + 1e-6 || cs >= clipEnd - 1e-6) continue;   // 重なりなし

        removes.push_back(c);
        // 録音範囲の外へはみ出した部分を断片として残す (ノートは境界でクランプして振り分け。
        // splitMidiClip と同じ要領。既存クリップも小節単位なので断片も小節単位になる)
        auto makePiece = [&](double pieceStart, double pieceEnd)
        {
            if (pieceEnd - pieceStart <= 1e-4) return;
            NewMidiClip piece;
            piece.startPos = pieceStart;
            piece.duration = pieceEnd - pieceStart;
            piece.name     = c->getName();
            piece.colour   = c->getColour();
            piece.channel  = c->getChannel();
            const double relA = pieceStart - cs;
            const double relB = pieceEnd   - cs;
            const auto& seq = c->getSequence();
            for (int i = 0; i < seq.getNumEvents(); ++i)
            {
                auto* ev = seq.getEventPointer(i);
                const auto& m = ev->message;
                if (!m.isNoteOn())
                {
                    // CC / ピッチベンド等は断片の範囲内のものを保全する
                    if (!m.isNoteOff())
                    {
                        const double t = m.getTimeStamp();
                        if (t >= relA && t < relB)
                        {
                            auto copy = m;
                            copy.setTimeStamp(t - relA);
                            piece.sequence.addEvent(copy);
                        }
                    }
                    continue;
                }
                const double onT  = m.getTimeStamp();
                const int    offI = seq.getIndexOfMatchingKeyUp(i);
                const double offT = (offI >= 0)
                    ? seq.getEventPointer(offI)->message.getTimeStamp() : onT + 0.25;
                const double on  = juce::jmax(onT,  relA);
                const double off = juce::jmin(offT, relB);
                if (off - on <= 0.001) continue;
                auto nOn  = juce::MidiMessage::noteOn (m.getChannel(), m.getNoteNumber(),
                                                       m.getFloatVelocity());
                auto nOff = juce::MidiMessage::noteOff(m.getChannel(), m.getNoteNumber());
                nOn.setTimeStamp (on  - relA);
                nOff.setTimeStamp(off - relA);
                piece.sequence.addEvent(nOn);
                piece.sequence.addEvent(nOff);
            }
            piece.sequence.updateMatchedPairs();
            adds.push_back(std::move(piece));
        };
        makePiece(cs, clipStart);
        makePiece(clipEnd, ce);
    }
    adds.push_back(std::move(np));

    clearAllSelections();   // 置換で破棄されるクリップを指す選択を先に解除
    pushMidiReplaceAction(track, std::move(removes), std::move(adds));
}

// ── MIDI クリップのクォンタイズ (タイムライン右クリック) ─────────────────
// 全ノートの開始位置を GRID へスナップする (長さは維持・CC 等はそのまま)。スナップは
// タイムライン絶対位置で行うので途中テンポ/変拍子にも追従する (snapTime = GridSnapMath)。
// クリップのインスタンスは保つ (MidiSequenceAction) ため、開いているピアノロールは
// onMidiClipContentChanged → reloadNotesFromClip で追従する
void TimelineView::quantizeMidiClip(Track* track, MidiClip* clip)
{
    if (track == nullptr || clip == nullptr) return;
    if (snapModeUnitSecs(appSettings.snapMode, bpm) <= 0.0) return;   // GRID Off

    const double cs  = clip->getStartPosition();
    const double dur = clip->getDuration();
    auto& seq = clip->getSequence();

    juce::MidiMessageSequence after;
    bool changed = false;
    for (int i = 0; i < seq.getNumEvents(); ++i)
    {
        auto* ev = seq.getEventPointer(i);
        const auto& m = ev->message;
        if (m.isNoteOff()) continue;                       // ペアの off は on 側で出す
        if (!m.isNoteOn()) { after.addEvent(m); continue; } // CC / ピッチベンド等は保持
        const double onT  = m.getTimeStamp();
        const int    offI = seq.getIndexOfMatchingKeyUp(i);
        const double offT = (offI >= 0)
            ? seq.getEventPointer(offI)->message.getTimeStamp() : onT + 0.25;
        double s = snapTime(cs + onT) - cs;
        s = juce::jlimit(0.0, juce::jmax(0.0, dur - 0.01), s);
        const double len = juce::jmax(0.01, juce::jmin(offT - onT, dur - s));
        if (std::abs(s - onT) > 1e-9) changed = true;
        auto nOn  = juce::MidiMessage::noteOn (m.getChannel(), m.getNoteNumber(),
                                               m.getFloatVelocity());
        auto nOff = juce::MidiMessage::noteOff(m.getChannel(), m.getNoteNumber());
        nOn.setTimeStamp(s);
        nOff.setTimeStamp(s + len);
        after.addEvent(nOn);
        after.addEvent(nOff);
    }
    if (!changed) return;   // 既にグリッド上 → Undo に積まない
    after.sort();
    after.updateMatchedPairs();

    auto onApplied = [this](MidiClip* c)
    {
        if (onMidiClipContentChanged) onMidiClipContentChanged(c);
        if (editChangeCb) editChangeCb();
        repaint();
    };
    juce::MidiMessageSequence before;
    before.addSequence(seq, 0.0);

    if (undoManager)
    {
        undoManager->beginNewTransaction();
        undoManager->perform(new EditActions::MidiSequenceAction(
            clip, std::move(before), std::move(after), onApplied));
    }
    else
    {
        seq.clear();
        seq.addSequence(after, 0.0);
        seq.updateMatchedPairs();
        onApplied(clip);
    }
}

void TimelineView::mouseDoubleClick(const juce::MouseEvent& e)
{
    // ── MIDI クリップのダブルクリック → ピアノロール起動 ──
    {
        const auto h = getMidiClipAt(e.x, e.y);
        if (h.clip != nullptr)
        {
            if (onMidiClipDoubleClicked) onMidiClipDoubleClicked(h.clip, h.track);
            return;
        }
    }

    // ── MIDI トラックの空きエリアをダブルクリック → 1 小節クリップを作成 ──
    {
        int mti = -1;
        if (auto* mt = midiTrackAtY(e.y, mti); mt != nullptr)
        {
            const double bar = barLengthSecs();
            const double t   = juce::jmax(0.0, xToPosition(e.x));
            const double startBar = std::floor(t / bar) * bar;  // 小節頭にスナップ
            EditActions::MidiClipReplaceAction::NewMidiClip np;
            np.startPos = startBar;
            np.duration = bar;
            np.name     = mt->getName();
            pushMidiReplaceAction(mt, {}, { std::move(np) });  // Undo 対応で作成
            return;
        }
    }

    auto ref = getClipAt(e.x, e.y);
    if (!ref.valid()) return;

    // クリップ上部（タイトル領域 約16px）をダブルクリック → 名前編集
    auto area2     = getContentArea();
    int trackTop2  = area2.getY() + trackManager.getTrackY(ref.trackIdx) - scrollY;
    int lTop2      = trackTop2 + (ref.laneIdx == 0 ? 0
                      : ref.track->getMainHeight() + (ref.laneIdx - 1) * ref.track->getLaneHeight());
    if (e.y >= lTop2 + 1 && e.y <= lTop2 + 16)
        beginNameEditing(ref);
}

void TimelineView::beginNameEditing(const ClipRef& ref)
{
    if (!ref.valid()) return;
    finishNameEditing(false);

    editingNameClip = ref.clip;

    // クリップの位置とサイズ
    const double bps = bpm / 60.0;
    int cx = (int)(ref.clip->getStartPosition() * bps * pixelsPerBeat - scrollX);
    int cw = juce::jmax(40, (int)(ref.clip->getDuration() * bps * pixelsPerBeat));
    auto area2     = getContentArea();
    int trackTop2  = area2.getY() + trackManager.getTrackY(ref.trackIdx) - scrollY;
    int lTop2      = trackTop2 + (ref.laneIdx == 0 ? 0
                      : ref.track->getMainHeight() + (ref.laneIdx - 1) * ref.track->getLaneHeight());

    nameEditor = std::make_unique<juce::TextEditor>();
    nameEditor->setBounds(cx + 6, lTop2 + 1, juce::jmin(cw - 8, 240), 16);
    nameEditor->setText(ref.clip->getName(), juce::dontSendNotification);
    nameEditor->setFont(juce::FontOptions(11.0f));
    nameEditor->setColour(juce::TextEditor::backgroundColourId, juce::Colours::black.withAlpha(0.7f));
    nameEditor->setColour(juce::TextEditor::textColourId,        juce::Colours::white);
    nameEditor->setColour(juce::TextEditor::highlightColourId,   juce::Colour(0xff5a8aaa));
    nameEditor->setColour(juce::TextEditor::outlineColourId,     juce::Colour(0xffaaaaaa));
    nameEditor->setBorder({1, 1, 1, 1});
    nameEditor->onReturnKey = [this] { finishNameEditing(true); };
    nameEditor->onEscapeKey = [this] { finishNameEditing(false); };
    nameEditor->onFocusLost = [this] { finishNameEditing(true); };
    addAndMakeVisible(nameEditor.get());
    nameEditor->grabKeyboardFocus();
    nameEditor->selectAll();
}

void TimelineView::finishNameEditing(bool commit)
{
    if (!nameEditor) return;
    if (commit && editingNameClip)
    {
        juce::String newName = nameEditor->getText().trim();
        if (newName.isNotEmpty() && newName != editingNameClip->getName())
        {
            // 名前変更を Undo 対応で記録 (name は ClipState に含まれる)
            EditActions::ClipState oldS; oldS.capture(editingNameClip);
            editingNameClip->setName(newName);
            EditActions::ClipState newS; newS.capture(editingNameClip);
            if (undoManager)
            {
                undoManager->beginNewTransaction();
                undoManager->perform(new EditActions::ClipsPropertyAction(
                    std::vector<EditActions::ClipState>{ oldS },
                    std::vector<EditActions::ClipState>{ newS },
                    [this] { if (editChangeCb) editChangeCb(); repaint(); },
                    clipAliveValidator()));
            }
            else if (editChangeCb) editChangeCb();
        }
    }
    removeChildComponent(nameEditor.get());
    nameEditor.reset();
    editingNameClip = nullptr;
    repaint();
}

