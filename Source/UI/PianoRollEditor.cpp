// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

#include "PianoRollEditor.h"
#include "../Localisation.h"
#include "../AppColours.h"
#include <cstring>
#include <cmath>

// MIDI ノート編集を main UndoManager に乗せるための UndoableAction。
// (#36: PianoRoll の独自 Undo スタックを main 統合)
// before / after は MidiClip の MidiMessageSequence をまるごと保持する。
// perform()=Redo, undo() で clip シーケンスを差し替え、
// onReload(clip) コールバックで "そのクリップに対応する現在開いているエディタ" を更新する。
// (Action 作成時に Editor インスタンスを直接保持してしまうと、Editor を閉じて
//  開き直した後の undo で古い Editor を参照してしまい、新 Editor の UI が
//  更新されなくなる ── #36 修正第 1 弾のバグ)
namespace
{
    class MidiNotesAction : public juce::UndoableAction
    {
    public:
        MidiNotesAction(MidiClip& c,
                        std::function<void(MidiClip*)> reloadCb,
                        juce::MidiMessageSequence b,
                        juce::MidiMessageSequence a)
            : clip(c), onReload(std::move(reloadCb)),
              beforeSeq(std::move(b)), afterSeq(std::move(a)) {}

        bool perform() override { apply(afterSeq);  return true; }
        bool undo()    override { apply(beforeSeq); return true; }

    private:
        void apply(const juce::MidiMessageSequence& src)
        {
            auto& dst = clip.getSequence();
            dst.clear();
            dst.addSequence(src, 0.0);
            dst.updateMatchedPairs();
            if (onReload) onReload(&clip);
        }

        MidiClip& clip;
        std::function<void(MidiClip*)> onReload;
        juce::MidiMessageSequence beforeSeq;
        juce::MidiMessageSequence afterSeq;
    };
}

PianoRollEditor::PianoRollEditor(MidiClip& clipRef, double projectBpm,
                                 double initialFocusTimeSec)
    : clip(clipRef), bpm(projectBpm)
{
    setWantsKeyboardFocus(true);
    rebuildNotesFromClip();

    // ── グリッド (スナップ) 選択 ComboBox ──
    snapBox.addItem(tr(u8"GRID: Off"),  (int) SnapMode::Off + 1);
    snapBox.addItem(tr(u8"GRID: 1/1"),  (int) SnapMode::Bar + 1);
    snapBox.addItem(tr(u8"GRID: 1/2"),  (int) SnapMode::Half + 1);
    snapBox.addItem(tr(u8"GRID: 1/4"),  (int) SnapMode::Quarter + 1);
    snapBox.addItem(tr(u8"GRID: 1/8"),  (int) SnapMode::Eighth + 1);
    snapBox.addItem(tr(u8"GRID: 1/16"), (int) SnapMode::Sixteenth + 1);
    snapBox.addItem(tr(u8"GRID: 1/32"), (int) SnapMode::ThirtySecond + 1);
    snapBox.addSeparator();
    snapBox.addItem(tr(u8"GRID: 1/4 三連"), (int) SnapMode::QuarterT + 1);
    snapBox.addItem(tr(u8"GRID: 1/8 三連"), (int) SnapMode::EighthT + 1);
    snapBox.addItem(tr(u8"GRID: 1/16 三連"), (int) SnapMode::SixteenthT + 1);
    snapBox.setSelectedId((int) snapMode + 1, juce::dontSendNotification);
    snapBox.setColour(juce::ComboBox::backgroundColourId, juce::Colour(0xff2a2d31));
    snapBox.setColour(juce::ComboBox::textColourId,       juce::Colours::white);
    snapBox.setColour(juce::ComboBox::arrowColourId,      juce::Colours::white);
    snapBox.setColour(juce::ComboBox::outlineColourId,    juce::Colour(0xff555555));
    snapBox.setWantsKeyboardFocus(false);
    snapBox.onChange = [this]
    {
        snapMode = (SnapMode) (snapBox.getSelectedId() - 1);
        repaint();
    };
    addAndMakeVisible(snapBox);

    // 横スクロールバー
    hScrollBar.setAutoHide(false);
    hScrollBar.addListener(this);
    addAndMakeVisible(hScrollBar);

    // 初期縦スクロール: 既存ノートの中央あたりへ
    if (!notes.empty())
    {
        int minP = 127, maxP = 0;
        for (auto& n : notes) { minP = juce::jmin(minP, n.pitch); maxP = juce::jmax(maxP, n.pitch); }
        const int center = (minP + maxP) / 2;
        scrollY = juce::jmax(0, pitchToY(center) + scrollY - 200);
    }
    else
    {
        scrollY = juce::jmax(0, pitchToY(72) + scrollY - 200);
    }

    // 初期横スクロール: 指定された時刻 (再生バー位置など) を左寄せ
    if (initialFocusTimeSec >= 0.0)
    {
        scrollX     = juce::jmax(0, (int)(initialFocusTimeSec * pixelsPerSec) - 80);
        playheadSec = initialFocusTimeSec;
    }
}

void PianoRollEditor::setPlayheadPosition(double secs)
{
    const double prev = playheadSec;
    playheadSec = secs;

    // 自動ページング: 再生バーがビュー右端を越えた (または左端より前へ戻った) ら、
    // 次のページが見えるよう横スクロールを飛ばす。設定 ON のときだけ。
    // (手動シーク = ルーラークリックはビュー内の位置なので発火しない)
    if (pagingEnabled && secs >= 0.0 && getWidth() > keyboardW)
    {
        const int phX = timeToX(secs);
        if (phX > getWidth() || phX < keyboardW)
        {
            // 再生バーをグリッド左端の少し右に置く (続き = 次ページの先頭が見える)
            const int leftMargin = 24;
            const int newScroll = juce::jmax(0,
                (int) std::round(secs * pixelsPerSec) - leftMargin);
            if (newScroll != scrollX)
            {
                scrollX = newScroll;
                updateScrollBarRange();   // hScrollBar の現在位置も同期
                repaint();
                return;
            }
        }
    }

    // 描画位置 (負値はクリップ左端にクランプ) の差分のみ repaint
    auto drawX = [this](double s) {
        return juce::jmax(keyboardW, timeToX(juce::jmax(0.0, s)));
    };
    // 未設定 (-1e9) からの遷移時は全体 repaint で確実に描画
    if (prev < -1e6 || secs < -1e6)
    {
        repaint();
        return;
    }
    const int x1 = drawX(prev) - 2;
    const int x2 = drawX(secs) + 3;
    repaint(juce::jmin(x1, x2), 0,
            std::abs(x2 - x1) + 4, getHeight());
}

PianoRollEditor::~PianoRollEditor() = default;

// ── 座標変換 ──────────────────────────────────────────────────────────────
int PianoRollEditor::pitchToY(int pitch) const
{
    // 高い音 = 上。 maxPitch (127) を y=0 に。
    return (maxPitch - pitch) * pitchHeight - scrollY + rulerH;
}

int PianoRollEditor::yToPitch(int y) const
{
    const int p = maxPitch - (y - rulerH + scrollY) / pitchHeight;
    return juce::jlimit(minPitch, maxPitch, p);
}

int PianoRollEditor::timeToX(double secs) const
{
    return keyboardW + (int)(secs * pixelsPerSec) - scrollX;
}

double PianoRollEditor::xToTime(int x) const
{
    return (double)(x - keyboardW + scrollX) / juce::jmax(1e-9, pixelsPerSec);
}

// ── シーケンス変換 ────────────────────────────────────────────────────────
void PianoRollEditor::rebuildNotesFromClip()
{
    // notes を作り直すと既存の index ベースの選択/ドラッグ状態は全て無効になる。
    // undo/redo (reloadNotesFromClip) でノート数が減ったとき、selected に残った
    // 範囲外 index で notes[idx] が OOB アクセスになるため、ここで一括リセットする。
    selected.clear();
    dragOrigNotes.clear();
    draggedIdx     = -1;
    velocityIdx    = -1;
    createdNoteIdx = -1;

    notes.clear();
    auto& seq = clip.getSequence();
    for (int i = 0; i < seq.getNumEvents(); ++i)
    {
        auto* ev = seq.getEventPointer(i);
        const auto& msg = ev->message;
        if (!msg.isNoteOn()) continue;
        Note n;
        n.pitch    = msg.getNoteNumber();
        n.velocity = msg.getFloatVelocity();
        n.startSec = msg.getTimeStamp();
        // ペア NoteOff の時刻を取得
        const int offIdx = seq.getIndexOfMatchingKeyUp(i);
        if (offIdx >= 0)
        {
            const double offTime = seq.getEventPointer(offIdx)->message.getTimeStamp();
            n.durationSec = juce::jmax(0.01, offTime - n.startSec);
        }
        else
        {
            n.durationSec = 0.25;
        }
        notes.push_back(n);
    }
}

void PianoRollEditor::writeNotesToClip()
{
    auto& seq = clip.getSequence();
    seq.clear();
    // MIDI チャンネルはクリップ設定に従う (ドラム用 ch10 等を保持するため)。
    // JUCE の MidiMessage は 1-based を取るので 0..15 → 1..16 に変換。
    const int ch = juce::jlimit(1, 16, clip.getChannel() + 1);
    for (auto& n : notes)
    {
        auto on  = juce::MidiMessage::noteOn (ch, n.pitch, n.velocity);
        auto off = juce::MidiMessage::noteOff(ch, n.pitch);
        on.setTimeStamp (n.startSec);
        off.setTimeStamp(n.startSec + n.durationSec);
        seq.addEvent(on);
        seq.addEvent(off);
    }
    seq.updateMatchedPairs();
    if (onChanged) onChanged();
}

// ── ヒットテスト ──────────────────────────────────────────────────────────
PianoRollEditor::HitResult PianoRollEditor::hitTestNote(juce::Point<int> p) const
{
    // velocity area は除外
    if (p.y > getHeight() - velocityH) return {};
    for (int i = (int) notes.size() - 1; i >= 0; --i)
    {
        const auto& n = notes[(size_t) i];
        const int x1 = timeToX(n.startSec);
        const int x2 = timeToX(n.startSec + n.durationSec);
        const int y  = pitchToY(n.pitch);
        if (p.x < x1 || p.x > x2) continue;
        if (p.y < y || p.y > y + pitchHeight) continue;
        HitResult r;
        r.noteIdx = i;
        const int edge = juce::jmin(6, (x2 - x1) / 3);
        if      (p.x < x1 + edge) r.kind = HitKind::NoteLeftEdge;
        else if (p.x > x2 - edge) r.kind = HitKind::NoteRightEdge;
        else                       r.kind = HitKind::NoteBody;
        return r;
    }
    return {};
}

int PianoRollEditor::hitTestVelocityBar(juce::Point<int> p) const
{
    const int top = getHeight() - velocityH;
    if (p.y < top) return -1;
    for (int i = (int) notes.size() - 1; i >= 0; --i)
    {
        const auto& n = notes[(size_t) i];
        // 描画と同じく、ベロシティバーはノート開始位置 (頭) に固定
        const int barX = timeToX(n.startSec) + 2;
        if (std::abs(p.x - barX) <= 4) return i;
    }
    return -1;
}

// ── プレビュー ────────────────────────────────────────────────────────────
void PianoRollEditor::firePreview(int note, float velocity, double durationSec)
{
    if (!onPreviewNote) return;
    if (previewActiveNote >= 0 && previewActiveNote != note)
    {
        onPreviewNote(previewActiveNote, 0.0f, false);
        previewActiveNote = -1;
    }
    onPreviewNote(note, juce::jlimit(0.05f, 1.0f, velocity), true);
    previewActiveNote = note;
    const int ms = juce::jlimit(50, 5000, (int)(durationSec * 1000.0));
    juce::Component::SafePointer<PianoRollEditor> safe(this);
    juce::Timer::callAfterDelay(ms, [safe, note]
    {
        auto* self = safe.getComponent();
        if (!self || !self->onPreviewNote) return;
        if (self->previewActiveNote != note) return;
        self->onPreviewNote(note, 0.0f, false);
        self->previewActiveNote = -1;
    });
}

// ── マウス操作 ────────────────────────────────────────────────────────────
void PianoRollEditor::mouseDown(const juce::MouseEvent& e)
{
    grabKeyboardFocus();
    dragStart   = e.getPosition();
    draggedIdx  = -1;
    velocityIdx = -1;

    // ルーラー (上端の小節番号バー) クリック → プレイヘッドをクリップ内のその時刻へ
    // キーボード列より右側 (ノートグリッドの上) のみ反応する
    if (e.y < rulerH && e.x >= keyboardW)
    {
        const double secsInClip = juce::jmax(0.0, xToTime(e.x));
        setPlayheadPosition(secsInClip);  // 即座にビジュアル反映
        if (onSeek) onSeek(secsInClip);   // メイン側のプレイヘッドも同期
        return;
    }

    // Velocity 領域
    if (e.y > getHeight() - velocityH)
    {
        velocityIdx = hitTestVelocityBar(e.getPosition());
        if (velocityIdx >= 0)
        {
            snapshotForUndo();
            dragMode = DragMode::AdjustVelocity;
        }
        return;
    }

    // ノートヒット?
    auto hit = hitTestNote(e.getPosition());
    if (hit.noteIdx >= 0)
    {
        // 選択管理
        if (e.mods.isCommandDown())
        {
            if (selected.count(hit.noteIdx)) selected.erase(hit.noteIdx);
            else                              selected.insert(hit.noteIdx);
        }
        else if (e.mods.isShiftDown())
        {
            selected.insert(hit.noteIdx);
        }
        else if (!selected.count(hit.noteIdx))
        {
            selected.clear();
            selected.insert(hit.noteIdx);
        }
        draggedIdx = hit.noteIdx;
        snapshotForUndo();
        dragOrigNotes.clear();
        for (int idx : selected) dragOrigNotes.push_back(notes[(size_t) idx]);
        if      (hit.kind == HitKind::NoteLeftEdge)  dragMode = DragMode::ResizeLeft;
        else if (hit.kind == HitKind::NoteRightEdge) dragMode = DragMode::ResizeRight;
        else                                          dragMode = DragMode::MoveNotes;
        // 単一ノートを直接クリック → そのノートの長さだけプレビュー再生
        if (hit.kind == HitKind::NoteBody)
        {
            const auto& n = notes[(size_t) hit.noteIdx];
            firePreview(n.pitch, n.velocity, n.durationSec);
        }
        repaint();
        return;
    }

    // 空エリアクリック: 修飾なしなら範囲選択開始 (新規作成はダブルクリックで)
    if (!e.mods.isShiftDown() && !e.mods.isCommandDown())
        selected.clear();
    rubberBand = juce::Rectangle<int>(e.x, e.y, 0, 0);
    dragMode   = DragMode::RubberBand;
    repaint();
}

void PianoRollEditor::mouseDrag(const juce::MouseEvent& e)
{
    if (dragMode == DragMode::None) return;
    const int dx = e.x - dragStart.x;
    const int dy = e.y - dragStart.y;
    const double dtSec  = (double) dx / juce::jmax(1e-9, pixelsPerSec);
    const int    dPitch = -dy / pitchHeight;  // 上方向で音高アップ
    const double clipLen = clip.getDuration();  // 0 以下なら無制限

    if (dragMode == DragMode::MoveNotes && !dragOrigNotes.empty())
    {
        // アンカー (最初の選択ノート) の新位置をスナップし、その差分を全体に適用
        const double anchorOrig = dragOrigNotes.front().startSec;
        const double anchorNew  = snapTimeSecs(anchorOrig + dtSec);
        const double snappedDt  = anchorNew - anchorOrig;
        size_t k = 0;
        for (int idx : selected)
        {
            auto& n = notes[(size_t) idx];
            const auto& o = dragOrigNotes[k++];
            n.startSec = juce::jmax(0.0, o.startSec + snappedDt);
            // クリップ範囲を超えないようクランプ (末尾が clipLen を超えない)
            if (clipLen > 0.0)
                n.startSec = juce::jmin(n.startSec, juce::jmax(0.0, clipLen - n.durationSec));
            n.pitch    = juce::jlimit(minPitch, maxPitch, o.pitch + dPitch);
        }
        repaint();
    }
    else if (dragMode == DragMode::ResizeLeft && !dragOrigNotes.empty())
    {
        size_t k = 0;
        for (int idx : selected)
        {
            auto& n = notes[(size_t) idx];
            const auto& o = dragOrigNotes[k++];
            const double newEnd   = o.startSec + o.durationSec;
            const double newStart = snapTimeSecs(juce::jmax(0.0, o.startSec + dtSec));
            n.startSec    = juce::jmin(newStart, newEnd - 0.01);
            n.durationSec = juce::jmax(0.01, newEnd - n.startSec);
        }
        repaint();
    }
    else if (dragMode == DragMode::ResizeRight && !dragOrigNotes.empty())
    {
        size_t k = 0;
        for (int idx : selected)
        {
            auto& n = notes[(size_t) idx];
            const auto& o = dragOrigNotes[k++];
            double newEnd = snapTimeSecs(o.startSec + o.durationSec + dtSec);
            if (clipLen > 0.0) newEnd = juce::jmin(newEnd, clipLen);  // クリップ末尾でクランプ
            n.durationSec = juce::jmax(0.01, newEnd - o.startSec);
        }
        repaint();
    }
    else if (dragMode == DragMode::RubberBand)
    {
        rubberBand = juce::Rectangle<int>::leftTopRightBottom(
            juce::jmin(dragStart.x, e.x), juce::jmin(dragStart.y, e.y),
            juce::jmax(dragStart.x, e.x), juce::jmax(dragStart.y, e.y));
        // 重なるノートを選択
        selected.clear();
        for (int i = 0; i < (int) notes.size(); ++i)
        {
            const auto& n = notes[(size_t) i];
            const int x1 = timeToX(n.startSec);
            const int x2 = timeToX(n.startSec + n.durationSec);
            const int y1 = pitchToY(n.pitch);
            const int y2 = y1 + pitchHeight;
            if (x2 >= rubberBand.getX() && x1 <= rubberBand.getRight()
                && y2 >= rubberBand.getY() && y1 <= rubberBand.getBottom())
                selected.insert(i);
        }
        repaint();
    }
    else if (dragMode == DragMode::AdjustVelocity && velocityIdx >= 0)
    {
        const int top    = getHeight() - velocityH;
        const int bottom = getHeight();
        const float v = 1.0f - (float)(e.y - top) / (float)(bottom - top);
        notes[(size_t) velocityIdx].velocity = juce::jlimit(0.01f, 1.0f, v);
        repaint();
    }
}

void PianoRollEditor::mouseUp(const juce::MouseEvent&)
{
    if (dragMode == DragMode::MoveNotes
     || dragMode == DragMode::ResizeLeft
     || dragMode == DragMode::ResizeRight
     || dragMode == DragMode::AdjustVelocity
     || dragMode == DragMode::CreateNote)
    {
        writeNotesToClip();
    }
    dragMode    = DragMode::None;
    rubberBand  = {};
    draggedIdx  = -1;
    velocityIdx = -1;
    repaint();
}

void PianoRollEditor::mouseDoubleClick(const juce::MouseEvent& e)
{
    // 空エリアダブルクリック: 新規ノート追加
    if (e.y > getHeight() - velocityH) return;
    auto hit = hitTestNote(e.getPosition());
    if (hit.noteIdx >= 0)
    {
        // 既存ノートのダブルクリック: 削除
        snapshotForUndo();
        notes.erase(notes.begin() + hit.noteIdx);
        selected.clear();
        writeNotesToClip();
        repaint();
        return;
    }
    // クリップ範囲 [0, clipLen] の外には作らない / はみ出さない (#範囲限定)
    const double clipLen = clip.getDuration();
    double start = snapTimeSecs(juce::jmax(0.0, xToTime(e.x)));
    if (clipLen > 0.0 && start >= clipLen) return;
    const double u = snapUnitSecs();
    double dur = (u > 0.0) ? juce::jmax(0.05, u) : defaultNoteDurSec;
    if (clipLen > 0.0)
    {
        start = juce::jmin(start, clipLen - 0.01);
        dur   = juce::jmin(dur, clipLen - start);
    }
    snapshotForUndo();
    Note n;
    n.pitch       = yToPitch(e.y);
    n.startSec    = start;
    n.durationSec = juce::jmax(0.01, dur);
    n.velocity    = 0.8f;
    notes.push_back(n);
    selected.clear();
    selected.insert((int) notes.size() - 1);
    writeNotesToClip();
    repaint();
}

void PianoRollEditor::mouseMove(const juce::MouseEvent& e)
{
    auto hit = hitTestNote(e.getPosition());
    if (hit.noteIdx >= 0)
    {
        if      (hit.kind == HitKind::NoteLeftEdge
              || hit.kind == HitKind::NoteRightEdge)
            setMouseCursor(juce::MouseCursor::LeftRightResizeCursor);
        else
            setMouseCursor(juce::MouseCursor::DraggingHandCursor);
    }
    else
    {
        setMouseCursor(juce::MouseCursor::NormalCursor);
    }
}

void PianoRollEditor::mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    // 修飾キー判定: 両押しは Cmd を優先
    // 横スクロールは Option(Alt) または Ctrl のどちらでも発動できるよう許容する。
    // (macOS の Accessibility が Option+scroll を吸ってしまう環境への保険)
    const bool cmdHeld = e.mods.isCommandDown();
    const bool hScrollMod = !cmdHeld && (e.mods.isAltDown() || e.mods.isCtrlDown());

    if (cmdHeld)
    {
        // Cmd + スクロール: マウスカーソル位置を支点に横方向の拡大縮小
        const double oldPx = pixelsPerSec;
        const double newPx = juce::jlimit(20.0, 4000.0, oldPx * std::pow(1.5, w.deltaY));
        if (std::abs(newPx - oldPx) > 1e-6)
        {
            const double timeAtMouse = (double)(e.x - keyboardW + scrollX) / juce::jmax(1e-9, oldPx);
            pixelsPerSec = newPx;
            scrollX = juce::jmax(0, (int) std::round(timeAtMouse * newPx - (e.x - keyboardW)));
        }
    }
    else if (hScrollMod)
    {
        // Option / Ctrl + スクロール: ピアノロール全体の拡大縮小 (横+縦同時)
        // カーソル下の時間位置 / ピッチ位置を支点に縮尺を変える。
        const double oldPx     = pixelsPerSec;
        const double newPx     = juce::jlimit(20.0, 4000.0, oldPx * std::pow(1.3, w.deltaY));
        const int    oldPitchH = pitchHeight;
        int          newPitchH = oldPitchH;
        if (w.deltaY > 0)        newPitchH = juce::jmin(40, oldPitchH + 1);
        else if (w.deltaY < 0)   newPitchH = juce::jmax(6,  oldPitchH - 1);

        // 横方向: カーソル下の時間が動かないように scrollX を補正
        if (std::abs(newPx - oldPx) > 1e-6)
        {
            const double timeAtMouse = (double)(e.x - keyboardW + scrollX) / juce::jmax(1e-9, oldPx);
            pixelsPerSec = newPx;
            scrollX = juce::jmax(0, (int) std::round(timeAtMouse * newPx - (e.x - keyboardW)));
        }
        // 縦方向: カーソル下のピッチが動かないように scrollY を補正
        if (newPitchH != oldPitchH)
        {
            const int pitchAtMouse = juce::jlimit(
                minPitch, maxPitch,
                maxPitch - (e.y - rulerH + scrollY) / juce::jmax(1, oldPitchH));
            pitchHeight = newPitchH;
            scrollY = juce::jmax(0,
                (maxPitch - pitchAtMouse) * newPitchH - (e.y - rulerH));
        }
    }
    else if (e.mods.isShiftDown())
    {
        // Shift + スクロール: 横スクロール
        // 環境により deltaY が 0 で deltaX に入る場合があるので両方足す
        const double dx = w.deltaX + w.deltaY;
        scrollX = juce::jmax(0, scrollX - (int)(dx * 400));
    }
    else
    {
        // 縦スクロール (deltaY) と横スクロール (deltaX)
        scrollY = juce::jmax(0, scrollY - (int)(w.deltaY * 60));
        scrollX = juce::jmax(0, scrollX - (int)(w.deltaX * 200));
    }
    updateScrollBarRange();
    repaint();
}

bool PianoRollEditor::keyPressed(const juce::KeyPress& k)
{
    const auto mods = k.getModifiers();
    if (k == juce::KeyPress::deleteKey || k == juce::KeyPress::backspaceKey)
    {
        deleteSelected();
        return true;
    }
    // Space = 再生 / 停止。ピアノロールは独立ウィンドウなので、フォーカス中はここで拾って
    // メイン側のトランスポートへ委譲しないと Windows で再生できない (macOS は GlobalKeyMonitor が
    // Space を消費するため keyPressed には届かず、この分岐は Windows/Linux でのみ実質作動する)。
    if (k == juce::KeyPress::spaceKey)
    {
        if (onTogglePlay) onTogglePlay();
        return true;
    }
    if (mods.isCommandDown() && k.getKeyCode() == 'A')      { selectAll(); return true; }
    if (mods.isCommandDown() && k.getKeyCode() == 'C')      { copySelected(); return true; }
    if (mods.isCommandDown() && k.getKeyCode() == 'X')      { cutSelected(); return true; }
    if (mods.isCommandDown() && k.getKeyCode() == 'V')      { pasteAtPlayhead(); return true; }
    if ((mods.isCommandDown() && mods.isShiftDown()
            && (k.getKeyCode() == 'Z' || k.getKeyCode() == 'z'))
        || (mods.isCommandDown() && !mods.isShiftDown()
            && (k.getKeyCode() == 'Y' || k.getKeyCode() == 'y')))
    {
        // Redo (Cmd+Shift+Z / Cmd+Y)
        if (externalUndoManager != nullptr)
        {
            // 連続編集中の未確定アクションがあれば確定してから redo
            if (pendingCommit) commitPendingUndoAction();
            // 共有 UndoManager の redo が AudioClip 編集を巻き戻した場合に備え、実際に
            // やり直したときだけタイムライン側の選択生ポインタをクリアしてもらう (UAF 防止)
            if (externalUndoManager->redo() && externalUndoRedoCallback)
                externalUndoRedoCallback();
        }
        else if (!redoStack.empty())
        {
            undoStack.push_back(notes);
            notes = redoStack.back();
            redoStack.pop_back();
            writeNotesToClip();
            repaint();
        }
        return true;
    }
    if (mods.isCommandDown()
        && (k.getKeyCode() == 'Z' || k.getKeyCode() == 'z'))
    {
        // Undo
        if (externalUndoManager != nullptr)
        {
            if (pendingCommit) commitPendingUndoAction();
            // 共有 UndoManager の undo が AudioClip 編集を巻き戻した場合に備え、実際に
            // 巻き戻したときだけタイムライン側の選択生ポインタをクリアしてもらう (UAF 防止)
            if (externalUndoManager->undo() && externalUndoRedoCallback)
                externalUndoRedoCallback();
        }
        else if (!undoStack.empty())
        {
            redoStack.push_back(notes);
            notes = undoStack.back();
            undoStack.pop_back();
            writeNotesToClip();
            repaint();
        }
        return true;
    }
    if (k.getKeyCode() == juce::KeyPress::leftKey)   { nudgeSelected(-0.01, 0); return true; }
    if (k.getKeyCode() == juce::KeyPress::rightKey)  { nudgeSelected( 0.01, 0); return true; }
    if (k.getKeyCode() == juce::KeyPress::upKey)     { nudgeSelected(0.0,  1); return true; }
    if (k.getKeyCode() == juce::KeyPress::downKey)   { nudgeSelected(0.0, -1); return true; }

    // Shift+0〜9: グリッド (Snap) 切替 (メイン画面と同一マッピング)
    // 0=Off, 1=1/1, ..., 6=1/32, 7=1/4 三連, 8=1/8 三連, 9=1/16 三連
    if (mods.isShiftDown() && !mods.isCommandDown() && !mods.isAltDown() && !mods.isCtrlDown())
    {
        int kc = k.getKeyCode();
        int digit = -1;
        if (kc >= '0' && kc <= '9') digit = kc - '0';
        else switch (kc) {
            case '!': digit = 1; break;
            case '"': digit = 2; break;  // JIS
            case '@': digit = 2; break;  // US
            case '#': digit = 3; break;
            case '$': digit = 4; break;
            case '%': digit = 5; break;
            case '&': digit = 6; break;  // JIS
            case '^': digit = 6; break;  // US
            case '\'': digit = 7; break; // JIS
            case '(': digit = 8; break;  // JIS
            case '*': digit = 8; break;  // US
            case ')': digit = 9; break;  // JIS
        }
        if (digit >= 0 && digit <= 9)
        {
            snapMode = (SnapMode) digit;
            snapBox.setSelectedId(digit + 1, juce::dontSendNotification);
            repaint();
            return true;
        }
    }
    return false;
}

void PianoRollEditor::deleteSelected()
{
    if (selected.empty()) return;
    snapshotForUndo();
    // 大きい index から消す
    std::vector<int> idx(selected.begin(), selected.end());
    std::sort(idx.begin(), idx.end(), std::greater<int>());
    for (int i : idx)
        if (i >= 0 && i < (int) notes.size())
            notes.erase(notes.begin() + i);
    selected.clear();
    writeNotesToClip();
    repaint();
}

void PianoRollEditor::selectAll()
{
    selected.clear();
    for (int i = 0; i < (int) notes.size(); ++i) selected.insert(i);
    repaint();
}

void PianoRollEditor::copySelected()
{
    clipboard.clear();
    if (selected.empty()) return;
    double minStart = std::numeric_limits<double>::max();
    for (int i : selected)
    {
        clipboard.push_back(notes[(size_t) i]);
        minStart = juce::jmin(minStart, notes[(size_t) i].startSec);
    }
    clipboardMinStart = minStart;
}

void PianoRollEditor::cutSelected()
{
    copySelected();
    deleteSelected();
}

void PianoRollEditor::pasteAtPlayhead()
{
    if (clipboard.empty()) return;
    snapshotForUndo();
    // 既存ノートの末尾 + 0.05 を起点に配置 (シンプル運用)
    double base = 0.0;
    for (auto& n : notes) base = juce::jmax(base, n.startSec + n.durationSec);
    selected.clear();
    for (auto& src : clipboard)
    {
        Note nn = src;
        nn.startSec = (src.startSec - clipboardMinStart) + base;
        notes.push_back(nn);
        selected.insert((int) notes.size() - 1);
    }
    writeNotesToClip();
    repaint();
}

void PianoRollEditor::nudgeSelected(double secs, int semis)
{
    if (selected.empty()) return;
    snapshotForUndo();
    const double clipLen = clip.getDuration();
    for (int idx : selected)
    {
        auto& n = notes[(size_t) idx];
        n.startSec = juce::jmax(0.0, n.startSec + secs);
        // クリップ範囲を超えないようクランプ (末尾が clipLen を超えない)
        if (clipLen > 0.0)
            n.startSec = juce::jmin(n.startSec, juce::jmax(0.0, clipLen - n.durationSec));
        n.pitch    = juce::jlimit(minPitch, maxPitch, n.pitch + semis);
    }
    writeNotesToClip();
    // 単一ノート選択 + ピッチ変更時はプレビュー
    if (selected.size() == 1 && semis != 0)
    {
        const int idx = *selected.begin();
        if (idx >= 0 && idx < (int) notes.size())
        {
            const auto& n = notes[(size_t) idx];
            firePreview(n.pitch, n.velocity, juce::jmin(0.4, n.durationSec));
        }
    }
    repaint();
}

void PianoRollEditor::snapshotForUndo()
{
    if (externalUndoManager != nullptr)
    {
        // main UndoManager 経路:
        // 編集前のシーケンスを 1 度だけキャプチャし、同一メッセージループ内の
        // 後続変更はまとめて 1 アクションとして確定する (連続ドラッグ等で
        // 過剰に Undo エントリが増えないように)。
        if (!pendingCommit)
        {
            pendingCommit    = true;
            pendingBeforeSeq.clear();
            pendingBeforeSeq.addSequence(clip.getSequence(), 0.0);
            juce::Component::SafePointer<PianoRollEditor> safeThis(this);
            juce::MessageManager::callAsync([safeThis]
            {
                if (auto* self = safeThis.getComponent())
                    self->commitPendingUndoAction();
            });
        }
        return;
    }

    // フォールバック: 内部 Undo スタック (後方互換)
    undoStack.push_back(notes);
    if ((int) undoStack.size() > kUndoMax) undoStack.erase(undoStack.begin());
    redoStack.clear();
}

// 2 つの MIDI シーケンスが (イベント数・各イベントの時刻と生データまで) 同一か。
// 「ノートをクリックして選択しただけ」等で実変化が無いとき空アクションを積まないための判定。
static bool midiSequencesEqual(const juce::MidiMessageSequence& a,
                               const juce::MidiMessageSequence& b)
{
    if (a.getNumEvents() != b.getNumEvents()) return false;
    for (int i = 0; i < a.getNumEvents(); ++i)
    {
        const auto& ma = a.getEventPointer(i)->message;
        const auto& mb = b.getEventPointer(i)->message;
        if (std::abs(ma.getTimeStamp() - mb.getTimeStamp()) > 1.0e-9) return false;
        if (ma.getRawDataSize() != mb.getRawDataSize()) return false;
        if (std::memcmp(ma.getRawData(), mb.getRawData(),
                        (size_t) ma.getRawDataSize()) != 0) return false;
    }
    return true;
}

void PianoRollEditor::commitPendingUndoAction()
{
    if (!pendingCommit || externalUndoManager == nullptr) return;
    pendingCommit = false;

    // 編集後のシーケンスを取得 (writeNotesToClip が同期化を担っているので
    // この時点で clip.getSequence() は最新)
    juce::MidiMessageSequence afterSeq;
    afterSeq.addSequence(clip.getSequence(), 0.0);

    // 実際にノートが変化していなければ Undo 履歴に積まない (クリック選択だけで MoveNotes
    // モードに入り writeNotesToClip → commit まで到達しても、空アクションで Cmd+Z が
    // 無反応になるのを防ぐ)。
    if (midiSequencesEqual(pendingBeforeSeq, afterSeq))
    {
        pendingBeforeSeq.clear();
        return;
    }

    externalUndoManager->beginNewTransaction("Edit MIDI notes");
    externalUndoManager->perform(new MidiNotesAction(
        clip,
        externalReloadCallback,  // クリップ→現在開いている Editor を解決するコールバック
        std::move(pendingBeforeSeq),
        std::move(afterSeq)));

    pendingBeforeSeq.clear();
}

void PianoRollEditor::reloadNotesFromClip()
{
    rebuildNotesFromClip();
    repaint();
    if (onChanged) onChanged();
}

// ── 描画 ──────────────────────────────────────────────────────────────────
void PianoRollEditor::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xff1a1c1f));

    // ルーラー (上)
    g.setColour(AppColours::panelBg);
    g.fillRect(0, 0, getWidth(), rulerH);
    g.setColour(AppColours::separator);
    g.drawLine(0.0f, (float) rulerH, (float) getWidth(), (float) rulerH);

    // ── BPM に応じた拍のグリッド ──
    const double secsPerBeat = 60.0 / juce::jmax(1.0, bpm);
    g.setColour(AppColours::textDim);
    g.setFont(juce::FontOptions(10.0f));
    const double leftSec = xToTime(keyboardW);
    const double rightSec = xToTime(getWidth());

    // 細かいグリッド (SnapMode 単位、薄め)
    const double snapU = snapUnitSecs();
    if (snapU > 0.0 && snapU * pixelsPerSec >= 4.0)  // 4px 未満なら描画省略
    {
        const int firstU = (int) std::floor(leftSec / snapU);
        const int lastU  = (int) std::ceil(rightSec / snapU);
        g.setColour(juce::Colour(0xff232527));
        for (int i = firstU; i <= lastU; ++i)
        {
            const double t = i * snapU;
            // 拍/小節と一致する場合はスキップ (下で太く描画)
            if (std::abs(std::fmod(t, secsPerBeat)) < 0.001) continue;
            const int x = timeToX(t);
            if (x < keyboardW || x >= getWidth()) continue;
            g.drawLine((float) x, (float) rulerH, (float) x, (float)(getHeight() - velocityH));
        }
    }

    // 小節番号はクリップの「現在の」タイムライン位置を反映する (クリップ移動で更新)。
    // clip.getStartPosition() を毎フレーム読み、クリップ先頭の絶対拍数を加える。
    const int clipStartBeats = (int) std::llround(clip.getStartPosition() / secsPerBeat);
    const int firstBeat  = (int) std::floor(leftSec / secsPerBeat);
    const int lastBeat   = (int) std::ceil(rightSec / secsPerBeat);
    for (int b = firstBeat; b <= lastBeat; ++b)
    {
        const int x = timeToX(b * secsPerBeat);
        if (x < keyboardW || x >= getWidth()) continue;
        const int absBeat = b + clipStartBeats;          // 絶対拍 (タイムライン基準)
        const bool isBar = (absBeat % 4 == 0);
        g.setColour(isBar ? AppColours::separatorLight : AppColours::separator);
        g.drawLine((float) x, (float) rulerH, (float) x, (float)(getHeight() - velocityH));
        if (isBar)
        {
            g.setColour(AppColours::text);
            g.drawText(juce::String(absBeat / 4 + 1), x + 2, 2, 40, rulerH - 4,
                       juce::Justification::centredLeft);
        }
    }

    // クリップ範囲外 (末尾より右) を暗く塗って編集可能域を明示 (#範囲限定)
    {
        const double clipLen = clip.getDuration();
        if (clipLen > 0.0)
        {
            const int xEnd = timeToX(clipLen);
            const int top  = rulerH;
            const int bot  = getHeight() - velocityH;
            if (xEnd < getWidth())
            {
                g.setColour(juce::Colours::black.withAlpha(0.45f));
                g.fillRect(juce::jmax(keyboardW, xEnd), top,
                           getWidth() - juce::jmax(keyboardW, xEnd), bot - top);
            }
            if (xEnd >= keyboardW && xEnd < getWidth())
            {
                g.setColour(AppColours::separatorLight);
                g.drawLine((float) xEnd, (float) top, (float) xEnd, (float) bot, 1.5f);
            }
        }
    }

    drawKeyboard(g);
    drawNotes(g);
    drawVelocityArea(g);

    // 再生バー (オレンジの縦線)
    // playheadSec が -∞ (≒ 未設定) でなければ常に描画する。
    // クリップ開始より前 (playheadSec < 0) のときは左端 (keyboardW) にクランプ表示。
    if (playheadSec > -1e6)
    {
        const double drawSec = juce::jmax(0.0, playheadSec);
        const int phX = juce::jmax(keyboardW, timeToX(drawSec));
        if (phX < getWidth())
        {
            g.setColour(AppColours::playhead);
            g.fillRect(phX, rulerH, 1, getHeight() - rulerH);
        }
    }

    // ラバーバンド
    if (dragMode == DragMode::RubberBand && !rubberBand.isEmpty())
    {
        g.setColour(AppColours::accent.withAlpha(0.15f));
        g.fillRect(rubberBand);
        g.setColour(AppColours::accent.withAlpha(0.8f));
        g.drawRect(rubberBand, 1);
    }
}

void PianoRollEditor::drawKeyboard(juce::Graphics& g) const
{
    // 左の鍵盤エリア
    g.setColour(juce::Colour(0xff222428));
    g.fillRect(0, rulerH, keyboardW, getHeight() - velocityH - rulerH);

    static const bool blackKeys[12] = { false, true, false, true, false, false, true, false, true, false, true, false };
    for (int p = minPitch; p <= maxPitch; ++p)
    {
        const int y = pitchToY(p);
        if (y + pitchHeight < rulerH || y > getHeight() - velocityH) continue;
        const bool isBlack = blackKeys[p % 12];
        g.setColour(isBlack ? juce::Colour(0xff111213) : juce::Colour(0xffe8e8e8));
        g.fillRect(0, y, keyboardW - 1, pitchHeight - 1);
        // C のラベル
        if (p % 12 == 0)
        {
            g.setColour(juce::Colour(0xff222428));
            g.setFont(juce::FontOptions(9.5f));
            g.drawText("C" + juce::String(p / 12 - 1),
                       2, y, keyboardW - 4, pitchHeight,
                       juce::Justification::centredLeft);
        }
        // 行のヘアライン (横方向に薄く)
        g.setColour(isBlack ? juce::Colour(0xff202225) : juce::Colour(0xff2a2c2f));
        g.drawHorizontalLine(y + pitchHeight - 1, (float) keyboardW, (float) getWidth());
        // C のところは強調
        if (p % 12 == 0)
        {
            g.setColour(juce::Colour(0xff35383d));
            g.drawHorizontalLine(y + pitchHeight - 1,
                                  (float) keyboardW, (float) getWidth());
        }
    }
    // 鍵盤の右端境界
    g.setColour(AppColours::separator);
    g.drawVerticalLine(keyboardW - 1, (float) rulerH, (float) (getHeight() - velocityH));
}

void PianoRollEditor::drawNotes(juce::Graphics& g) const
{
    // ノートは鍵盤 / ルーラー / Velocity 領域に侵食しないようクリップ
    juce::Graphics::ScopedSaveState ss(g);
    g.reduceClipRegion(keyboardW, rulerH,
                       juce::jmax(0, getWidth() - keyboardW),
                       juce::jmax(0, getHeight() - rulerH - velocityH));

    const auto baseColour = clip.getColour();
    for (int i = 0; i < (int) notes.size(); ++i)
    {
        const auto& n = notes[(size_t) i];
        const int x1 = timeToX(n.startSec);
        const int x2 = timeToX(n.startSec + n.durationSec);
        const int y  = pitchToY(n.pitch);
        if (x2 < keyboardW || x1 > getWidth() || y + pitchHeight < rulerH
            || y > getHeight() - velocityH) continue;
        const auto col = selected.count(i)
            ? baseColour.brighter(0.4f)
            : baseColour;
        g.setColour(col);
        g.fillRect(x1, y + 1, juce::jmax(2, x2 - x1) - 1, pitchHeight - 2);
        g.setColour(juce::Colour(0xff000000).withAlpha(0.4f));
        g.drawRect(x1, y + 1, juce::jmax(2, x2 - x1) - 1, pitchHeight - 2, 1);
    }
}

void PianoRollEditor::drawVelocityArea(juce::Graphics& g) const
{
    const int top    = getHeight() - velocityH;
    g.setColour(juce::Colour(0xff15171a));
    g.fillRect(0, top, getWidth(), velocityH);
    g.setColour(AppColours::separator);
    g.drawHorizontalLine(top, 0.0f, (float) getWidth());

    g.setColour(AppColours::textDim);
    g.setFont(juce::FontOptions(10.0f));
    g.drawText("Velocity", 6, top + 2, keyboardW - 8, 14, juce::Justification::centredLeft);

    const auto baseColour = clip.getColour();
    for (int i = 0; i < (int) notes.size(); ++i)
    {
        const auto& n = notes[(size_t) i];
        // ベロシティバーはノートの開始位置 (頭) に揃える。
        // 長さを変えても頭がずれないよう、中央ではなく start に固定する。
        const int barX = timeToX(n.startSec) + 2;
        if (barX < keyboardW || barX > getWidth()) continue;
        const int h = (int) (n.velocity * (velocityH - 8));
        const int by = getHeight() - 4 - h;
        const auto col = selected.count(i) ? baseColour.brighter(0.4f) : baseColour;
        g.setColour(col);
        g.fillRect(barX - 2, by, 4, h);
        g.setColour(col.withAlpha(0.5f));
        g.drawLine((float)(barX - 4), (float) by, (float)(barX + 4), (float) by, 1.0f);
    }
}

void PianoRollEditor::resized()
{
    // 右上隅に GRID ComboBox
    snapBox.setBounds(getWidth() - 130, 2, 124, rulerH - 4);
    // 底部に横スクロールバー (Velocity 領域の下)
    hScrollBar.setBounds(keyboardW, getHeight() - kScrollBarH,
                          juce::jmax(0, getWidth() - keyboardW), kScrollBarH);
    updateScrollBarRange();
}

void PianoRollEditor::updateScrollBarRange()
{
    // 表示すべき総時間 = ノート末尾 + 余裕 (最低 30 秒) を秒で確保
    double endSec = 30.0;
    for (auto& n : notes)
        endSec = juce::jmax(endSec, n.startSec + n.durationSec);
    endSec += 4.0;  // 末尾に少し余白

    const double totalPx  = endSec * pixelsPerSec;
    const double viewPx   = juce::jmax(1, getWidth() - keyboardW);
    hScrollBar.setRangeLimits(0.0, juce::jmax(viewPx, totalPx));
    hScrollBar.setCurrentRange((double) scrollX, viewPx, juce::dontSendNotification);
}

void PianoRollEditor::scrollBarMoved(juce::ScrollBar*, double newRangeStart)
{
    int newX = juce::jmax(0, (int) std::round(newRangeStart));
    if (newX != scrollX)
    {
        scrollX = newX;
        repaint();
    }
}

double PianoRollEditor::snapUnitSecs() const
{
    return snapModeUnitSecs(snapMode, bpm);
}

double PianoRollEditor::snapTimeSecs(double secs) const
{
    const double u = snapUnitSecs();
    if (u <= 0.0) return secs;
    return std::round(secs / u) * u;
}
