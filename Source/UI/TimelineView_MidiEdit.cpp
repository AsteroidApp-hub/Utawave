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
    if (selectedMidiClip == nullptr) return;
    for (int ti = 0; ti < trackManager.getTrackCount(); ++ti)
    {
        auto* tr = trackManager.getTrack(ti);
        if (!tr || !tr->isMidiTrack()) continue;
        for (int ci = 0; ci < tr->getMidiClipCount(); ++ci)
            if (tr->getMidiClip(ci) == selectedMidiClip) return;  // まだ実在 → 据え置き
    }
    selectedMidiClip  = nullptr;
    selectedMidiTrack = nullptr;
}

void TimelineView::pushMidiReplaceAction(Track* track,
                                         std::vector<MidiClip*> toRemove,
                                         std::vector<EditActions::MidiClipReplaceAction::NewMidiClip> toAdd)
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
        if (!m.isNoteOn()) continue;
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

