// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

#include "PianoRollEditor.h"
#include "../Localisation.h"
#include "../AppColours.h"
#include "UtawaveLookAndFeel.h"
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

    // ── 追従 (再生バー追従) / ペンツール トグルボタン ──
    // DrawableButton (ImageOnButtonBackground) も背景色は TextButton の colour id を使うため
    // 共通ヘルパで設定できる。
    auto setupToolButton = [](juce::Button& b)
    {
        b.setClickingTogglesState(true);
        b.setColour(juce::TextButton::buttonColourId,   juce::Colour(0xff2a2d31));
        b.setColour(juce::TextButton::buttonOnColourId, AppColours::accent);
        b.setColour(juce::TextButton::textColourOffId,  juce::Colours::white);
        b.setColour(juce::TextButton::textColourOnId,   juce::Colours::white);
        b.setWantsKeyboardFocus(false);
    };
    setupToolButton(followBtn);
    {
        // 追従アイコン: プレイヘッドの縦線 + それを追う右向き矢印
        // (「表示が再生バーに追従する」= 言語非依存で伝わる。テキスト "追従" は Windows で潰れる)
        juce::Path follow;
        follow.addRectangle(0.22f, 0.18f, 0.11f, 0.64f);              // プレイヘッドの縦線
        follow.addRectangle(0.42f, 0.42f, 0.28f, 0.16f);             // 矢印のシャフト
        follow.addTriangle(0.62f, 0.26f,  0.62f, 0.74f,  0.86f, 0.50f); // 矢印の先端
        juce::DrawablePath dp;
        dp.setPath(follow);
        dp.setFill(juce::Colours::white);
        followBtn.setImages(&dp);
    }
    followBtn.setToggleState(pagingEnabled, juce::dontSendNotification);
    followBtn.setTooltip(tr(u8"再生バーに追従して自動スクロールします (手動で横スクロールすると一時停止し、再生バーが戻ると再開)") + " (F)");
    followBtn.onClick = [this]
    {
        pagingEnabled   = followBtn.getToggleState();
        followSuspended = false;
        if (onFollowToggled) onFollowToggled(pagingEnabled);
    };
    addAndMakeVisible(followBtn);

    setupToolButton(penBtn);
    {
        // 鉛筆アイコン (斜めの軸 + 先端の三角)。単位正方形で描き DrawableButton が縮尺する
        juce::Path pen;
        pen.addQuadrilateral(0.30f, 0.60f,  0.64f, 0.26f,  0.76f, 0.38f,  0.42f, 0.72f);
        pen.addTriangle(0.20f, 0.82f,  0.26f, 0.64f,  0.38f, 0.76f);
        juce::DrawablePath dp;
        dp.setPath(pen);
        dp.setFill(juce::Colours::white);
        penBtn.setImages(&dp);
    }
    penBtn.setTooltip(platformShortcutText(
        tr(u8"ペンツール: 空きエリアのクリックでノートを作成し、そのままドラッグで長さを調整 (Cmd を押している間は一時的にペンになります / ペンに関係なく Option+クリックでノート分割)")) + " (D)");
    penBtn.onClick = [this] { penMode = penBtn.getToggleState(); };
    addAndMakeVisible(penBtn);

    // ── ステップ入力トグル + ノート長 ComboBox ──
    setupToolButton(stepBtn);
    {
        // 階段アイコン (左下から右上へ上がる 3 段 = 順に置いていくイメージ)
        juce::Path stairs;
        stairs.addRectangle(0.16f, 0.62f, 0.22f, 0.22f);
        stairs.addRectangle(0.40f, 0.40f, 0.22f, 0.44f);
        stairs.addRectangle(0.64f, 0.18f, 0.22f, 0.66f);
        juce::DrawablePath dp;
        dp.setPath(stairs);
        dp.setFill(juce::Colours::white);
        stepBtn.setImages(&dp);
    }
    stepBtn.setTooltip(
        tr(u8"ステップ入力: MIDIキーボードを弾くと、カーソル位置へ選んだ長さのノートを順に置きます (←/→ でカーソル移動・同時に押した鍵は和音)") + " (I)");
    stepBtn.onClick = [this]
    {
        stepMode = stepBtn.getToggleState();
        stepHeldKeys.clear();
        if (stepMode)
        {
            // 一度も選んでいなければ現在の GRID から長さを引き継ぐ (Off は既定 1/8 のまま)
            if (!stepLenUserSet && snapMode != SnapMode::Off)
            {
                stepLenMode = snapMode;
                stepLenBox.setSelectedId((int) stepLenMode + 1, juce::dontSendNotification);
            }
            // カーソルは再生バー位置 (クリップ範囲内・グリッドへスナップ) から始める。
            // スナップの丸めがクリップ末尾を越えると全ノートが「置けない」判定になるため、
            // スナップ後にもう一度クリップ範囲へクランプする
            double start = (playheadSec > -1e6) ? juce::jmax(0.0, playheadSec) : 0.0;
            const double clipLen = clip.getDuration();
            if (clipLen > 0.0) start = juce::jmin(start, clipLen);
            stepPosSec = juce::jmax(0.0, snapTimeSecs(start));
            if (clipLen > 0.0) stepPosSec = juce::jmin(stepPosSec, clipLen);
        }
        stepLenBox.setVisible(stepMode);
        resized();
        repaint();
    };
    // MIDI キーボード未接続では入力手段が無いため、ボタンは接続中のみ表示
    // (setMidiInputAvailable で切替。既定は非表示)
    addChildComponent(stepBtn);

    // ステップで置くノートの長さ (GRID と同じ選択肢・Off は除く)
    stepLenBox.addItem(tr(u8"STEP: 1/1"),  (int) SnapMode::Bar + 1);
    stepLenBox.addItem(tr(u8"STEP: 1/2"),  (int) SnapMode::Half + 1);
    stepLenBox.addItem(tr(u8"STEP: 1/4"),  (int) SnapMode::Quarter + 1);
    stepLenBox.addItem(tr(u8"STEP: 1/8"),  (int) SnapMode::Eighth + 1);
    stepLenBox.addItem(tr(u8"STEP: 1/16"), (int) SnapMode::Sixteenth + 1);
    stepLenBox.addItem(tr(u8"STEP: 1/32"), (int) SnapMode::ThirtySecond + 1);
    stepLenBox.addSeparator();
    stepLenBox.addItem(tr(u8"STEP: 1/4 三連"),  (int) SnapMode::QuarterT + 1);
    stepLenBox.addItem(tr(u8"STEP: 1/8 三連"),  (int) SnapMode::EighthT + 1);
    stepLenBox.addItem(tr(u8"STEP: 1/16 三連"), (int) SnapMode::SixteenthT + 1);
    stepLenBox.setSelectedId((int) stepLenMode + 1, juce::dontSendNotification);
    stepLenBox.setColour(juce::ComboBox::backgroundColourId, juce::Colour(0xff2a2d31));
    stepLenBox.setColour(juce::ComboBox::textColourId,       juce::Colours::white);
    stepLenBox.setColour(juce::ComboBox::arrowColourId,      juce::Colours::white);
    stepLenBox.setColour(juce::ComboBox::outlineColourId,    juce::Colour(0xff555555));
    stepLenBox.setWantsKeyboardFocus(false);
    stepLenBox.setTooltip(tr(u8"ステップ入力で置くノートの長さ"));
    stepLenBox.onChange = [this]
    {
        stepLenMode    = (SnapMode) (stepLenBox.getSelectedId() - 1);
        stepLenUserSet = true;
    };
    stepLenBox.setVisible(false);   // ステップ入力 ON の間だけ表示
    addChildComponent(stepLenBox);

    // ── 下部レーンの種類選択 (Velocity / ピッチベンド / CC) ──
    // 左ヘッダセル (鍵盤列と同じ幅) に収まる小型ボタン。クリックで正式名称のポップアップ
    laneBtn.setColour(juce::TextButton::buttonColourId,  juce::Colour(0xff2a2d31));
    laneBtn.setColour(juce::TextButton::textColourOffId, juce::Colours::white);
    laneBtn.setWantsKeyboardFocus(false);
    laneBtn.setTooltip(platformShortcutText(
        tr(u8"下のレーンに表示する情報を選択します (Velocity 以外: Cmd+ドラッグで描画 (ペンツール中は Cmd 不要)・ドラッグで範囲選択して Delete で削除・Option+ドラッグで消去・ダブルクリックで数値入力)")));
    laneBtn.onClick = [this] { showLaneMenu(); };
    addAndMakeVisible(laneBtn);
    selectCtrlLane(CtrlLane::Velocity);   // ボタンラベルを初期化

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

void PianoRollEditor::setTooltipsEnabled(bool enabled)
{
    // ピアノロールは独立ウィンドウ (別ピア) のため専用の TooltipWindow を持つ
    // (メイン画面の TooltipWindow は同一ピアのコンポーネントにしかチップを出さない)。
    if (enabled)
    {
        if (tooltipWin == nullptr)
        {
            // LnF は TooltipWindow より長生きする必要があるため関数ローカル static
            // (MainComponent::applyTooltipVisibility と同じ作法)
            static UtawaveLookAndFeel tooltipLnF;
            tooltipWin = std::make_unique<juce::TooltipWindow>(this);
            tooltipWin->setLookAndFeel(&tooltipLnF);
        }
    }
    else
    {
        tooltipWin.reset();
    }
}

void PianoRollEditor::setPagingEnabled(bool v)
{
    pagingEnabled   = v;
    followSuspended = false;
    followBtn.setToggleState(v, juce::dontSendNotification);
}

void PianoRollEditor::noteManualHScroll()
{
    // 手動横スクロールで再生バーがビュー外に出たらページングを一時停止する
    // (これが無いと、再生中に別の場所を見ようとスクロールした瞬間に引き戻される)。
    // ビュー内へ戻した / 再生バーが追いついたら setPlayheadPosition が自動再開する。
    if (playheadSec > -1e6)
    {
        const int phX = timeToX(juce::jmax(0.0, playheadSec));
        followSuspended = (phX < keyboardW || phX > getWidth());
    }
}

void PianoRollEditor::setPlayheadPosition(double secs)
{
    const double prev = playheadSec;
    playheadSec = secs;

    // 自動ページング: 再生バーがビュー右端を越えた (または左端より前へ戻った) ら、
    // 次のページが見えるよう横スクロールを飛ばす。設定 ON のときだけ。
    // (手動シーク = ルーラークリックはビュー内の位置なので発火しない)
    if (secs >= 0.0 && getWidth() > keyboardW)
    {
        const int phX = timeToX(secs);
        const bool inView = (phX >= keyboardW && phX <= getWidth());
        if (inView)
            followSuspended = false;  // 再生バーがビューへ戻ったら追従を再開
        if (pagingEnabled && !followSuspended && !inView)
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
    return (maxPitch - pitch) * pitchHeight - scrollY + topH();
}

int PianoRollEditor::yToPitch(int y) const
{
    const int p = maxPitch - (y - topH() + scrollY) / pitchHeight;
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
    ctrlValueEditor.reset();   // 再構築で入力対象が変わるため数値入力は取消

    notes.clear();
    ctrlMsgs.clear();
    auto& seq = clip.getSequence();
    for (int i = 0; i < seq.getNumEvents(); ++i)
    {
        auto* ev = seq.getEventPointer(i);
        const auto& msg = ev->message;
        // ノート以外 (CC / ピッチベンド / PC 等) は ctrlMsgs へ保持する。
        // 下部レーンで編集し、writeNotesToClip がノートと一緒に書き戻す
        if (!msg.isNoteOn() && !msg.isNoteOff())
        {
            ctrlMsgs.push_back(msg);
            continue;
        }
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

    // **開始時刻の昇順で emit する** (肝): notes ベクタは分割で末尾に断片を push するため
    // 時刻順とは限らない (例: 左半分をカットすると [A[0,1], B[2,4], C[1,2]] のように C が
    // B より後ろに来る)。ベクタ順のまま emit すると、同一ピッチで隣接するノートの境界で
    // 次のノートの note-on が前のノートの note-off より前に挿入され、再読込
    // (rebuildNotesFromClip の getIndexOfMatchingKeyUp) が depth カウントで誤ペアリングして
    // **右側のノートが 0 長に潰れる** (「左をカットすると右が縮む」バグ・再生も無音化)。
    // 時刻順に並べれば各ノートの note-off が次の note-on より前に入り正しくペアリングされる。
    // selected は notes への index なので、ここでは並べ替えたコピーを emit して実体は触らない。
    std::vector<const Note*> ordered;
    ordered.reserve(notes.size());
    for (const auto& n : notes) ordered.push_back(&n);
    std::stable_sort(ordered.begin(), ordered.end(),
                     [](const Note* a, const Note* b) { return a->startSec < b->startSec; });

    for (const auto* np : ordered)
    {
        const auto& n = *np;
        auto on  = juce::MidiMessage::noteOn (ch, n.pitch, n.velocity);
        auto off = juce::MidiMessage::noteOff(ch, n.pitch);
        on.setTimeStamp (n.startSec);
        off.setTimeStamp(n.startSec + n.durationSec);
        seq.addEvent(on);
        seq.addEvent(off);
    }
    // ノート以外のイベント (CC / ピッチベンド等・下部レーンで編集) も書き戻す。
    // addEvent は時刻順に挿入されるためノートペアの並び (on → off) は壊れない
    for (const auto& m : ctrlMsgs)
        seq.addEvent(m);
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
    // ジェスチャ (ドラッグ) 中は Undo の commit を保留する。callAsync の commit は
    // ドラッグイベントの合間 (ランループが回った時) に発火しうるため、これが無いと
    // 1 回のドラッグが複数の微小トランザクションに分割され、Undo 1 回でドラッグ全体が
    // 戻らないことがある。mouseUp で 1 アクションとして確定する
    gestureActive = true;
    dragStart   = e.getPosition();
    draggedIdx  = -1;
    velocityIdx = -1;

    // ツールバー段 (ボタン行) の空きエリアは何もしない (ボタン自体は子コンポーネントが受ける)
    if (e.y < toolbarH)
        return;

    // ルーラー (小節番号バー): クリック = シーク (mouseUp で確定)、縦ドラッグ = 横ズーム
    // (メインのルーラーと同じ。即シークするとズーム操作で再生バーが飛ぶため保留する)
    if (e.y < topH() && e.x >= keyboardW)
    {
        dragMode            = DragMode::RulerDrag;
        rulerSeekArmed      = true;
        rulerZooming        = false;
        rulerZoomStartPps   = pixelsPerSec;
        rulerZoomAnchorTime = juce::jmax(0.0, xToTime(e.x));
        rulerZoomAnchorX    = e.x;
        return;
    }

    // Velocity 領域の上端境界を掴んで高さを調整 (領域内の判定より先に見る)
    if (std::abs(e.y - (getHeight() - velocityH)) <= kVelResizeHitPx)
    {
        dragMode        = DragMode::ResizeVelocityArea;
        velResizeStartH = velocityH;
        return;
    }

    // Velocity 領域 / コントロールレーン
    if (e.y > getHeight() - velocityH)
    {
        // 左ヘッダセル (レーン名ボタン / 目盛り) はキャンバス操作の対象外
        if (e.x < keyboardW)
            return;
        if (ctrlLane == CtrlLane::Velocity)
        {
            velocityIdx = hitTestVelocityBar(e.getPosition());
            if (velocityIdx >= 0)
            {
                snapshotForUndo();
                dragMode = DragMode::AdjustVelocity;
                setReadout(juce::String((int) std::lround(
                               notes[(size_t) velocityIdx].velocity * 127.0f)),
                           e.getPosition());
            }
        }
        else if ((penMode && !e.mods.isShiftDown())
                 || e.mods.isCommandDown() || e.mods.isAltDown())
        {
            // Cmd+ドラッグで描画 / Option+ドラッグで消去。素のクリックでは値を入れない
            // (誤クリックでピッチベンド等が書き換わる事故防止。Cmd = ペンの一時作画と同じ作法)。
            // **ペンツール (D) 中は Cmd 不要で素のドラッグでも描画** (ノートグリッドのペンと
            // 同じ体系。範囲選択はペン中でも Shift+ドラッグでできる)
            snapshotForUndo();
            dragMode      = DragMode::DrawCtrl;
            ctrlErasing   = e.mods.isAltDown();
            ctrlDragLastT = -1.0;
            applyCtrlDragPoint(e.getPosition(), ctrlErasing);
            repaint();
        }
        else
        {
            // 素のドラッグ = 時間範囲の選択 (Delete で範囲内のイベントを一括削除)。
            // ドラッグせずクリックだけなら mouseUp で選択解除
            const double clipLen = clip.getDuration();
            const double maxT = (clipLen > 0.0) ? clipLen : 1.0e9;
            ctrlSelAnchorT = juce::jlimit(0.0, maxT, xToTime(e.x));
            ctrlSelT1 = ctrlSelT2 = ctrlSelAnchorT;
            ctrlSelActive = true;
            dragMode = DragMode::SelectCtrlRange;
            repaint();
        }
        return;
    }

    // ノートヒット?
    auto hit = hitTestNote(e.getPosition());

    // Option+クリック: ノートを分割 (タイムラインの Alt+Click 分割と同じ作法・グリッドスナップ)
    if (hit.noteIdx >= 0 && e.mods.isAltDown() && !e.mods.isCommandDown())
    {
        splitNoteAt(hit.noteIdx, xToTime(e.x));
        return;
    }

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

    // ペン (penMode ON、または Cmd を押している間の一時ペン): 空エリアクリックで
    // ノート作成 (そのままドラッグで長さ調整)。Shift 付きは範囲選択に回す (ペン中でも
    // 複数選択できるように)。Cmd+クリックがノート上に当たった場合は上のノートブロックで
    // トグル選択されて return 済みなので、ここに来るのは空エリアのみ。
    const bool penActive = penMode || e.mods.isCommandDown();
    if (penActive && e.x >= keyboardW && e.y >= topH() && !e.mods.isShiftDown())
    {
        const int idx = createNoteAt(e.getPosition());
        if (idx >= 0)
        {
            createdNoteIdx  = idx;
            createdStartSec = notes[(size_t) idx].startSec;
            dragMode        = DragMode::CreateNote;
            const auto& n = notes[(size_t) idx];
            firePreview(n.pitch, n.velocity, n.durationSec);
            repaint();
        }
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
    else if (dragMode == DragMode::CreateNote
          && createdNoteIdx >= 0 && createdNoteIdx < (int) notes.size())
    {
        // ペンで作成中: 横ドラッグで長さ (グリッド単位)、縦ドラッグでピッチを調整
        auto& n = notes[(size_t) createdNoteIdx];
        double newEnd = snapTimeSecs(xToTime(e.x));
        const double u = snapUnitSecs();
        newEnd = juce::jmax(newEnd, createdStartSec + ((u > 0.0) ? u : 0.01));
        if (clipLen > 0.0) newEnd = juce::jmin(newEnd, clipLen);
        n.durationSec = juce::jmax(0.01, newEnd - createdStartSec);
        const int newPitch = yToPitch(e.y);
        if (newPitch != n.pitch)
        {
            n.pitch = newPitch;
            firePreview(n.pitch, n.velocity, n.durationSec);
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
        const int bottom = getHeight() - kScrollBarH;   // バーの描画レンジと揃える
        const float v = 1.0f - (float)(e.y - top) / (float) juce::jmax(1, bottom - top);
        auto& n = notes[(size_t) velocityIdx];
        n.velocity = juce::jlimit(0.01f, 1.0f, v);
        // 入力中の数値をカーソル脇に表示 (見える化・数値のみ)
        setReadout(juce::String((int) std::lround(n.velocity * 127.0f)), e.getPosition());
        repaint();
    }
    else if (dragMode == DragMode::DrawCtrl)
    {
        applyCtrlDragPoint(e.getPosition(), ctrlErasing);
        repaint();
    }
    else if (dragMode == DragMode::SelectCtrlRange)
    {
        const double clipLen = clip.getDuration();
        const double maxT = (clipLen > 0.0) ? clipLen : 1.0e9;
        const double t = juce::jlimit(0.0, maxT, xToTime(e.x));
        ctrlSelT1 = juce::jmin(ctrlSelAnchorT, t);
        ctrlSelT2 = juce::jmax(ctrlSelAnchorT, t);
        repaint();
    }
    else if (dragMode == DragMode::RulerDrag)
    {
        // 方向確定: 縦が支配的なら横ズームへ (メインの TimelineRuler と同じしきい値)
        if (!rulerZooming && std::abs(dy) > 6 && std::abs(dy) > std::abs(dx) * 1.5)
            rulerZooming = true;
        if (rulerZooming)
        {
            rulerSeekArmed = false;   // ズーム操作は再生位置を動かさない
            // 下方向 = 拡大 (1px ≈ 1.2% の指数変化)。クリック位置の時刻を支点に保つ
            const double factor = std::pow(1.012, (double) dy);
            const double newPps = juce::jlimit(20.0, 4000.0, rulerZoomStartPps * factor);
            if (std::abs(newPps - pixelsPerSec) > 1e-9)
            {
                pixelsPerSec = newPps;
                scrollX = juce::jmax(0, (int) std::round(
                    rulerZoomAnchorTime * newPps - (rulerZoomAnchorX - keyboardW)));
                updateScrollBarRange();
                repaint();
            }
        }
    }
    else if (dragMode == DragMode::ResizeVelocityArea)
    {
        // 境界を上へドラッグ = 領域を広げる。グリッド域を最低 kGridMinH 残す
        const int maxH = juce::jmax(kVelMinH, getHeight() - topH() - kGridMinH);
        velocityH = juce::jlimit(kVelMinH, juce::jmin(kVelMaxH, maxH),
                                 velResizeStartH - dy);
        layoutLaneBox();
        repaint();
    }
}

void PianoRollEditor::mouseUp(const juce::MouseEvent&)
{
    if (dragMode == DragMode::MoveNotes
     || dragMode == DragMode::ResizeLeft
     || dragMode == DragMode::ResizeRight
     || dragMode == DragMode::AdjustVelocity
     || dragMode == DragMode::CreateNote
     || dragMode == DragMode::DrawCtrl)
    {
        writeNotesToClip();
    }
    // Velocity 領域の高さ変更を確定 (アプリ設定として保存してもらう)
    if (dragMode == DragMode::ResizeVelocityArea && onVelocityAreaResized)
        onVelocityAreaResized(velocityH);
    // ルーラー: ズームに化けなかった純粋なクリックはここでシークを確定する
    if (dragMode == DragMode::RulerDrag && rulerSeekArmed)
    {
        const double secsInClip = rulerZoomAnchorTime;
        setPlayheadPosition(secsInClip);
        if (onSeek) onSeek(secsInClip);
        // ステップ入力中はカーソルも移動する (入力位置の指定手段。GRID にスナップ)
        if (stepMode)
            moveStepCursor(snapTimeSecs(secsInClip));
    }
    rulerSeekArmed = false;
    rulerZooming   = false;
    // ドラッグせずクリックしただけ (幅ほぼ 0) なら選択解除
    if (dragMode == DragMode::SelectCtrlRange && ctrlSelT2 - ctrlSelT1 < 1.0e-4)
        ctrlSelActive = false;
    ctrlDragLastT = -1.0;
    clearReadout();
    dragMode       = DragMode::None;
    rubberBand     = {};
    draggedIdx     = -1;
    velocityIdx    = -1;
    createdNoteIdx = -1;
    // ジェスチャ終了: 保留していた Undo をドラッグ全体で 1 アクションとして確定する
    gestureActive = false;
    if (pendingCommit) commitPendingUndoAction();
    repaint();
}

void PianoRollEditor::mouseDoubleClick(const juce::MouseEvent& e)
{
    // コントロールレーンのダブルクリック = 数値の直接入力 (小さな入力欄を出す)。
    // ペン (または Cmd/Option の描画・消去) 中は無効 — 1・2 クリック目が既に点を
    // 描いており、続けて入力欄を出すと描いた点と二重管理になるため
    if (e.y > getHeight() - velocityH
        && std::abs(e.y - (getHeight() - velocityH)) > kVelResizeHitPx
        && e.x >= keyboardW
        && ctrlLane != CtrlLane::Velocity)
    {
        if (!penMode && !e.mods.isCommandDown() && !e.mods.isAltDown())
            beginCtrlValueEdit(e.getPosition());
        return;
    }
    // ペン中は無効 (penMode ON、または Cmd 一時ペン。1 クリック目で既にノートを作成済みで、
    // ここで既存ノート削除を走らせると作ったばかりのノートがダブルクリック判定で即消える)
    if (penMode || e.mods.isCommandDown()) return;
    // ツールバー / ルーラー段ではノートを作らない (グリッド域のみ)
    if (e.y < topH()) return;
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
    if (createNoteAt(e.getPosition()) < 0) return;
    writeNotesToClip();
    repaint();
}

// 新規ノートを作成して index を返す (作れない位置なら -1)。undo snapshot と選択更新まで
// 行うが、writeNotesToClip は呼ばない (ダブルクリックは即確定 / ペンはドラッグ終了の
// mouseUp で確定と、呼び出し側でタイミングが異なるため)。
int PianoRollEditor::createNoteAt(juce::Point<int> pos)
{
    // クリップ範囲 [0, clipLen] の外には作らない / はみ出さない (#範囲限定)
    const double clipLen = clip.getDuration();
    double start = snapTimeSecs(juce::jmax(0.0, xToTime(pos.x)));
    if (clipLen > 0.0 && start >= clipLen) return -1;
    const double u = snapUnitSecs();
    double dur = (u > 0.0) ? juce::jmax(0.05, u) : defaultNoteDurSec;
    if (clipLen > 0.0)
    {
        start = juce::jmin(start, clipLen - 0.01);
        dur   = juce::jmin(dur, clipLen - start);
    }
    snapshotForUndo();
    Note n;
    n.pitch       = yToPitch(pos.y);
    n.startSec    = start;
    n.durationSec = juce::jmax(0.01, dur);
    n.velocity    = 0.8f;
    notes.push_back(n);
    selected.clear();
    selected.insert((int) notes.size() - 1);
    return (int) notes.size() - 1;
}

// Option+クリックのノート分割。分割点は「クリックした位置ちょうど」で行う (グリッド
// スナップしない)。スナップすると分割点がクリック位置からグリッド線へ移動し、隣接ノートを
// 続けてカットしたときに片側が縮んだ / 分割位置がずれたように見えるため (ユーザー報告
// 2026-07)。ノート分割は精密操作なので、他 DAW のハサミツール同様クリック位置で正確に切る。
void PianoRollEditor::splitNoteAt(int noteIdx, double rawSecs)
{
    if (noteIdx < 0 || noteIdx >= (int) notes.size()) return;
    const Note orig  = notes[(size_t) noteIdx];
    const double start = orig.startSec;
    const double end   = orig.startSec + orig.durationSec;
    const double kMin  = 0.01;  // 分割後の最小ノート長
    const double t = rawSecs;
    if (t <= start + kMin || t >= end - kMin) return;  // 端に近すぎて分割できない
    snapshotForUndo();
    notes[(size_t) noteIdx].durationSec = t - start;
    Note right = orig;
    right.startSec    = t;
    right.durationSec = end - t;
    notes.push_back(right);
    selected.clear();
    selected.insert(noteIdx);
    selected.insert((int) notes.size() - 1);
    writeNotesToClip();
    repaint();
}

void PianoRollEditor::mouseMove(const juce::MouseEvent& e)
{
    updateHoverCursor(e.getPosition(), e.mods);

    // 数値の見える化 (ホバー): 下部レーンでは現在値をカーソル脇に表示する。
    // Velocity はバー上のノートの値、CC / ピッチベンドはその時点の実効値
    if (e.y > getHeight() - velocityH
        && std::abs(e.y - (getHeight() - velocityH)) > kVelResizeHitPx
        && e.x >= keyboardW)
    {
        if (ctrlLane == CtrlLane::Velocity)
        {
            const int idx = hitTestVelocityBar(e.getPosition());
            if (idx >= 0)
                setReadout(juce::String((int) std::lround(
                               notes[(size_t) idx].velocity * 127.0f)),
                           e.getPosition());
            else
                clearReadout();
        }
        else
        {
            const double t = juce::jmax(0.0, xToTime(e.x));
            setReadout(laneValueText(ctrlLane, laneEffectiveValueAt(t, ctrlLane)),
                       e.getPosition());
        }
    }
    else
    {
        clearReadout();
    }
}

void PianoRollEditor::mouseExit(const juce::MouseEvent&)
{
    clearReadout();
}

void PianoRollEditor::updateHoverCursor(juce::Point<int> pos, const juce::ModifierKeys& mods)
{
    // Velocity 領域の上端境界: 上下ドラッグで高さ調整できることを示す
    if (std::abs(pos.y - (getHeight() - velocityH)) <= kVelResizeHitPx)
    {
        setMouseCursor(juce::MouseCursor::UpDownResizeCursor);
        return;
    }
    // コントロールレーン (Velocity 以外): 描画できる状態 (ペンツール中 / Cmd / Option) だけ
    // 十字カーソル。素のドラッグは範囲選択なので通常カーソルのまま
    if (pos.y > getHeight() - velocityH && ctrlLane != CtrlLane::Velocity
        && pos.x >= keyboardW)
    {
        setMouseCursor((penMode || mods.isCommandDown() || mods.isAltDown())
                           ? juce::MouseCursor::CrosshairCursor
                           : juce::MouseCursor::NormalCursor);
        return;
    }
    auto hit = hitTestNote(pos);
    if (hit.noteIdx >= 0)
    {
        if (mods.isAltDown() && !mods.isCommandDown())
            setMouseCursor(juce::MouseCursor::IBeamCursor);  // Option+クリック = 分割
        else if (hit.kind == HitKind::NoteLeftEdge
              || hit.kind == HitKind::NoteRightEdge)
            setMouseCursor(juce::MouseCursor::LeftRightResizeCursor);
        else
            setMouseCursor(juce::MouseCursor::DraggingHandCursor);
    }
    else if ((penMode || mods.isCommandDown())
             && pos.y >= topH() && pos.y <= getHeight() - velocityH
             && pos.x >= keyboardW)
    {
        // ペン (または Cmd 一時ペン) で書ける空きエリア
        setMouseCursor(juce::MouseCursor::CrosshairCursor);
    }
    else
    {
        setMouseCursor(juce::MouseCursor::NormalCursor);
    }
}

void PianoRollEditor::modifierKeysChanged(const juce::ModifierKeys& mods)
{
    // Cmd の押下/解除でカーソル (一時ペンの十字) をマウスを動かさずに更新する。
    // ドラッグ中は変えない。フォーカスが無い純ホバー時は mouseMove 側が拾う。
    if (dragMode == DragMode::None && isMouseOver())
        updateHoverCursor(getMouseXYRelative(), mods);
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
                maxPitch - (e.y - topH() + scrollY) / juce::jmax(1, oldPitchH));
            pitchHeight = newPitchH;
            scrollY = juce::jmax(0,
                (maxPitch - pitchAtMouse) * newPitchH - (e.y - topH()));
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
    noteManualHScroll();  // 再生バーがビュー外に出たら追従 (ページング) を一時停止
    updateScrollBarRange();
    repaint();
}

bool PianoRollEditor::keyPressed(const juce::KeyPress& k)
{
    const auto mods = k.getModifiers();
    if (k == juce::KeyPress::deleteKey || k == juce::KeyPress::backspaceKey)
    {
        // コントロールレーンの範囲選択があればそちらを優先して一括削除
        if (ctrlLane != CtrlLane::Velocity && ctrlSelActive)
        {
            deleteCtrlSelection();
            return true;
        }
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
    // ステップ入力中の ←/→ は入力カーソルを 1 ステップ動かす (→ = 休符送りを兼ねる)。
    // ノートのナッジより優先する (ステップ入力の主操作のため)。↑/↓ は従来どおりピッチナッジ
    if (stepMode && !mods.isAnyModifierKeyDown())
    {
        const double len = stepLenSecs();
        if (len > 0.0)
        {
            if (k.getKeyCode() == juce::KeyPress::leftKey)  { moveStepCursor(stepPosSec - len); return true; }
            if (k.getKeyCode() == juce::KeyPress::rightKey) { moveStepCursor(stepPosSec + len); return true; }
        }
    }
    if (k.getKeyCode() == juce::KeyPress::leftKey)   { nudgeSelected(-0.01, 0); return true; }
    if (k.getKeyCode() == juce::KeyPress::rightKey)  { nudgeSelected( 0.01, 0); return true; }
    if (k.getKeyCode() == juce::KeyPress::upKey)     { nudgeSelected(0.0,  1); return true; }
    if (k.getKeyCode() == juce::KeyPress::downKey)   { nudgeSelected(0.0, -1); return true; }

    // F: 追従 (自動ページング) トグル / D: ペンツール トグル。
    // setToggleState の sendNotification が onClick を発火するため、ボタンクリックと
    // 完全に同一経路 (追従はアプリ設定への保存まで揃う)。修飾付き (Cmd+F 等) は対象外
    if (!mods.isAnyModifierKeyDown())
    {
        const int kc = k.getKeyCode();
        if (kc == 'F' || kc == 'f')
        {
            followBtn.setToggleState(!followBtn.getToggleState(), juce::sendNotification);
            return true;
        }
        if (kc == 'D' || kc == 'd')
        {
            penBtn.setToggleState(!penBtn.getToggleState(), juce::sendNotification);
            return true;
        }
        // I: ステップ入力トグル (ボタンクリックと同一経路)。S は使わない — macOS の
        // GlobalKeyMonitor が無修飾 S を「停止」として全ウィンドウで消費するため届かない
        // (D/F/L と同じく監視対象外のキーから選定。I = step Input)。
        // MIDI キーボード未接続 (ボタン非表示) の間はキーも効かせない
        if ((kc == 'I' || kc == 'i') && midiInputAvailable)
        {
            stepBtn.setToggleState(!stepBtn.getToggleState(), juce::sendNotification);
            return true;
        }
        // L: レガート (選択ノートを次のノートの開始位置まで伸ばす)
        if (kc == 'L' || kc == 'l')
        {
            legatoSelected();
            return true;
        }
        // Q: クォンタイズ (選択ノート / 未選択なら全ノートの開始を GRID へ)。
        // メイン側の Q (リテイク) は録音中のみ GlobalKeyMonitor が消費するため衝突しない
        if (kc == 'Q' || kc == 'q')
        {
            quantizeSelected();
            return true;
        }
    }

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

void PianoRollEditor::legatoSelected()
{
    // レガート: 選択した各ノートを「次のノート」の開始位置まで伸ばす。
    // 「次のノート」= そのノートの開始より後に始まる、全ノート中で最も近い開始時刻のノート
    // (ピッチは問わない = 単旋律のガイド/ハモリ用途で「隙間を埋める」動作)。伸ばすだけで、
    // 既に次のノートへ届いている/重なっているノートは縮めない (要望「伸ばす」に合わせ短縮しない)。
    // 次のノートが無い (最後のノート) 場合は変更しない。
    if (selected.empty()) return;
    const double kEps = 1.0e-6;

    // 先に更新内容 (index, 新 duration) を集める。実際の変更前に snapshotForUndo を呼びたい &
    // 更新中も notes を読み取り専用で走査したいため 2 パスにする。
    std::vector<std::pair<int, double>> updates;
    for (int idx : selected)
    {
        if (idx < 0 || idx >= (int) notes.size()) continue;
        const auto& n = notes[(size_t) idx];
        double nextStart = std::numeric_limits<double>::max();
        for (int j = 0; j < (int) notes.size(); ++j)
        {
            if (j == idx) continue;
            const double s = notes[(size_t) j].startSec;
            if (s > n.startSec + kEps && s < nextStart) nextStart = s;
        }
        if (nextStart == std::numeric_limits<double>::max()) continue;  // 次が無い
        const double newDur = nextStart - n.startSec;
        if (newDur > n.durationSec + kEps)                             // 伸ばす場合のみ
            updates.emplace_back(idx, newDur);
    }
    if (updates.empty()) return;

    snapshotForUndo();
    for (const auto& u : updates)
        notes[(size_t) u.first].durationSec = juce::jmax(0.01, u.second);
    writeNotesToClip();
    repaint();
}

void PianoRollEditor::quantizeSelected()
{
    // クォンタイズ: 選択ノート (未選択なら全ノート) の開始位置を GRID の最寄りへスナップする。
    // 長さは維持 (クリップ末尾へはみ出す分だけ短縮)。GRID:Off のときは何もしない
    const double u = snapUnitSecs();
    if (u <= 0.0 || notes.empty()) return;

    std::vector<int> targets;
    if (selected.empty())
        for (int i = 0; i < (int) notes.size(); ++i) targets.push_back(i);
    else
        targets.assign(selected.begin(), selected.end());

    const double clipLen = clip.getDuration();
    snapshotForUndo();   // 変化が無ければ commit 側 (midiSequencesEqual) が捨てる
    bool changed = false;
    for (int idx : targets)
    {
        if (idx < 0 || idx >= (int) notes.size()) continue;
        auto& n = notes[(size_t) idx];
        double s = std::round(n.startSec / u) * u;
        s = juce::jmax(0.0, s);
        if (clipLen > 0.0) s = juce::jmin(s, juce::jmax(0.0, clipLen - 0.01));
        double d = n.durationSec;
        if (clipLen > 0.0) d = juce::jmin(d, clipLen - s);
        if (std::abs(s - n.startSec) < 1.0e-9) continue;
        n.startSec    = s;
        n.durationSec = juce::jmax(0.01, d);
        changed = true;
    }
    if (!changed) return;
    writeNotesToClip();
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
    // ドラッグ中は確定しない (pendingCommit は保持)。mouseUp が改めて呼び、
    // ドラッグ開始前 → 終了後の差分を 1 つの Undo アクションにまとめる
    if (gestureActive) return;
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

    // ツールバー段 (ボタン行) + ルーラー (小節番号バー)。ボタンを小節番号に重ねると
    // 機能が増えたとき番号が隠れて見づらいため、段を分けて描く
    g.setColour(AppColours::panelBg);
    g.fillRect(0, 0, getWidth(), topH());
    g.setColour(AppColours::separator.withAlpha(0.6f));
    g.drawLine(0.0f, (float) toolbarH, (float) getWidth(), (float) toolbarH);  // 段の区切り (控えめ)
    g.setColour(AppColours::separator);
    g.drawLine(0.0f, (float) topH(), (float) getWidth(), (float) topH());

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
            g.drawLine((float) x, (float) topH(), (float) x, (float)(getHeight() - velocityH));
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
        g.drawLine((float) x, (float) topH(), (float) x, (float)(getHeight() - velocityH));
        if (isBar)
        {
            g.setColour(AppColours::text);
            g.drawText(juce::String(absBeat / 4 + 1), x + 2, toolbarH + 2, 40, rulerH - 4,
                       juce::Justification::centredLeft);
        }
    }

    // クリップ範囲外 (末尾より右) を暗く塗って編集可能域を明示 (#範囲限定)
    {
        const double clipLen = clip.getDuration();
        if (clipLen > 0.0)
        {
            const int xEnd = timeToX(clipLen);
            const int top  = topH();
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
            // 下部レーン (Velocity / コントロール) には引かない (数値編集の邪魔になるため)
            g.fillRect(phX, topH(), 1, juce::jmax(0, getHeight() - velocityH - topH()));
        }
    }

    // ステップ入力カーソル (アクセント色の縦線 + 上端の下向き三角)
    if (stepMode)
    {
        const int sx = timeToX(juce::jmax(0.0, stepPosSec));
        if (sx >= keyboardW && sx < getWidth())
        {
            g.setColour(AppColours::accent);
            g.fillRect(sx - 1, topH(), 2, getHeight() - velocityH - topH());
            juce::Path tri;
            tri.addTriangle((float) sx - 5.0f, (float) topH(),
                            (float) sx + 5.0f, (float) topH(),
                            (float) sx,        (float) topH() + 7.0f);
            g.fillPath(tri);
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

    // ドラッグ / ホバー中の数値表示 (最前面)
    drawValueReadout(g);
}

void PianoRollEditor::drawKeyboard(juce::Graphics& g) const
{
    // 左の鍵盤エリア
    g.setColour(juce::Colour(0xff222428));
    g.fillRect(0, topH(), keyboardW, getHeight() - velocityH - topH());

    static const bool blackKeys[12] = { false, true, false, true, false, false, true, false, true, false, true, false };
    for (int p = minPitch; p <= maxPitch; ++p)
    {
        const int y = pitchToY(p);
        if (y + pitchHeight < topH() || y > getHeight() - velocityH) continue;
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
    g.drawVerticalLine(keyboardW - 1, (float) topH(), (float) (getHeight() - velocityH));
}

void PianoRollEditor::drawNotes(juce::Graphics& g) const
{
    // ノートは鍵盤 / ルーラー / Velocity 領域に侵食しないようクリップ
    juce::Graphics::ScopedSaveState ss(g);
    g.reduceClipRegion(keyboardW, topH(),
                       juce::jmax(0, getWidth() - keyboardW),
                       juce::jmax(0, getHeight() - topH() - velocityH));

    const auto baseColour = clip.getColour();
    for (int i = 0; i < (int) notes.size(); ++i)
    {
        const auto& n = notes[(size_t) i];
        const int x1 = timeToX(n.startSec);
        const int x2 = timeToX(n.startSec + n.durationSec);
        const int y  = pitchToY(n.pitch);
        if (x2 < keyboardW || x1 > getWidth() || y + pitchHeight < topH()
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

const char* PianoRollEditor::laneShortName(CtrlLane lane)
{
    // 左ヘッダセル (幅 ~44px) に収まる短縮名。正式名称はポップアップ / ツールチップで見せる
    switch (lane)
    {
        case CtrlLane::PitchBend:  return "PB";
        case CtrlLane::Modulation: return "Mod";
        case CtrlLane::Expression: return "Exp";
        case CtrlLane::Pan:        return "Pan";
        case CtrlLane::Sustain:    return "Sus";
        case CtrlLane::Velocity:
        default:                   return "Vel";
    }
}

void PianoRollEditor::selectCtrlLane(CtrlLane lane)
{
    ctrlLane = lane;
    laneBtn.setButtonText(juce::String(laneShortName(lane)) + " "
                          + juce::String::fromUTF8(u8"▾"));
    ctrlSelActive = false;       // 範囲選択はレーン単位なので切替で解除
    ctrlValueEditor.reset();     // 数値入力中なら取消
    clearReadout();
    repaint();
}

void PianoRollEditor::showLaneMenu()
{
    juce::PopupMenu m;
    auto add = [&](CtrlLane lane)
    {
        m.addItem((int) lane + 1, laneName(lane), true, ctrlLane == lane);
    };
    add(CtrlLane::Velocity);
    add(CtrlLane::PitchBend);
    add(CtrlLane::Modulation);
    add(CtrlLane::Expression);
    add(CtrlLane::Pan);
    add(CtrlLane::Sustain);
    m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&laneBtn),
        [safe = juce::Component::SafePointer<PianoRollEditor>(this)](int result)
        {
            if (auto* self = safe.getComponent(); self != nullptr && result > 0)
                self->selectCtrlLane((CtrlLane) (result - 1));
        });
}

// レーンの枠: コンテンツ背景 + 拍グリッド + クリップ範囲外の暗転 + 左ヘッダセル (レーン名
// ボタンの下に値の目盛り)。参考にした一般的な DAW のレーン表示と同じく、レーンを本体グリッド
// から独立した 1 つのパネルとして見せる (バーやイベントが鍵盤列側へはみ出さない)
void PianoRollEditor::drawLaneFrame(juce::Graphics& g) const
{
    const int top     = getHeight() - velocityH;
    const int cBottom = getHeight() - kScrollBarH;   // コンテンツ下端 (スクロールバー帯は含めない)

    // コンテンツ背景 (全幅)
    g.setColour(juce::Colour(0xff15171a));
    g.fillRect(0, top, getWidth(), velocityH);

    // レーン内の拍グリッド (本体と同じ位置・控えめな色) + クリップ範囲外の暗転
    {
        juce::Graphics::ScopedSaveState ss(g);
        g.reduceClipRegion(keyboardW, top + 1,
                           juce::jmax(0, getWidth() - keyboardW),
                           juce::jmax(0, cBottom - top - 1));
        const double secsPerBeat   = 60.0 / juce::jmax(1.0, bpm);
        const double leftSec       = xToTime(keyboardW);
        const double rightSec      = xToTime(getWidth());
        const int clipStartBeats   = (int) std::llround(clip.getStartPosition() / secsPerBeat);
        for (int b = (int) std::floor(leftSec / secsPerBeat);
             b <= (int) std::ceil(rightSec / secsPerBeat); ++b)
        {
            const int x = timeToX(b * secsPerBeat);
            if (x < keyboardW || x >= getWidth()) continue;
            const bool isBar = ((b + clipStartBeats) % 4 == 0);
            g.setColour(isBar ? juce::Colour(0xff2c2f33) : juce::Colour(0xff232527));
            g.drawVerticalLine(x, (float)(top + 1), (float) cBottom);
        }
        const double clipLen = clip.getDuration();
        if (clipLen > 0.0)
        {
            const int xEnd = timeToX(clipLen);
            if (xEnd < getWidth())
            {
                g.setColour(juce::Colours::black.withAlpha(0.45f));
                g.fillRect(juce::jmax(keyboardW, xEnd), top + 1,
                           getWidth() - juce::jmax(keyboardW, xEnd), cBottom - top - 1);
            }
        }
    }

    // 左ヘッダセル (鍵盤列と同じ幅)
    g.setColour(AppColours::panelBg);
    g.fillRect(0, top, keyboardW, velocityH);
    g.setColour(AppColours::separator);
    g.drawVerticalLine(keyboardW - 1, (float) top, (float) getHeight());

    // 値の目盛り (上端 / 中央 / 下端の値。数値の見える化)
    const char* topLbl = "127";
    const char* midLbl = nullptr;
    const char* botLbl = "0";
    switch (ctrlLane)
    {
        case CtrlLane::PitchBend: topLbl = "+8192"; midLbl = "0"; botLbl = "-8192"; break;
        case CtrlLane::Pan:       topLbl = "R63";   midLbl = "C"; botLbl = "L64";   break;
        case CtrlLane::Sustain:   topLbl = "ON";                  botLbl = "OFF";   break;
        default: break;
    }
    g.setFont(juce::FontOptions(9.0f));
    g.setColour(AppColours::textDim);
    const int lw = keyboardW - 8;
    g.drawText(topLbl, 2, top + 23, lw, 10, juce::Justification::centredRight);
    if (midLbl != nullptr)
        g.drawText(midLbl, 2, (top + 23 + cBottom - 12) / 2, lw, 10,
                   juce::Justification::centredRight);
    g.drawText(botLbl, 2, cBottom - 12, lw, 10, juce::Justification::centredRight);

    // 上端の区切り線 (全幅・最後に引いて境界を明瞭に)
    g.setColour(AppColours::separatorLight);
    g.drawHorizontalLine(top, 0.0f, (float) getWidth());
}

void PianoRollEditor::drawVelocityArea(juce::Graphics& g) const
{
    drawLaneFrame(g);

    if (ctrlLane != CtrlLane::Velocity)
    {
        drawCtrlLane(g);
        return;
    }

    // ベロシティバーはコンテンツ領域にクリップ (ヘッダセル / スクロールバー帯へはみ出さない)
    const int top     = getHeight() - velocityH;
    const int cBottom = getHeight() - kScrollBarH;
    juce::Graphics::ScopedSaveState ss(g);
    g.reduceClipRegion(keyboardW, top + 1,
                       juce::jmax(0, getWidth() - keyboardW),
                       juce::jmax(0, cBottom - top - 1));

    const auto baseColour = clip.getColour();
    for (int i = 0; i < (int) notes.size(); ++i)
    {
        const auto& n = notes[(size_t) i];
        // ベロシティバーはノートの開始位置 (頭) に揃える。
        // 長さを変えても頭がずれないよう、中央ではなく start に固定する。
        const int barX = timeToX(n.startSec) + 2;
        if (barX < keyboardW || barX > getWidth()) continue;
        const int h = (int) (n.velocity * (velocityH - kScrollBarH - 10));
        const int by = cBottom - 4 - h;
        const auto col = selected.count(i) ? baseColour.brighter(0.4f) : baseColour;
        g.setColour(col);
        g.fillRect(barX - 2, by, 4, h);
        g.setColour(col.withAlpha(0.5f));
        g.drawLine((float)(barX - 4), (float) by, (float)(barX + 4), (float) by, 1.0f);
    }
}

// ── 下部コントロールレーン (ピッチベンド / CC) ─────────────────────────────
bool PianoRollEditor::msgMatchesLane(const juce::MidiMessage& m, CtrlLane lane)
{
    switch (lane)
    {
        case CtrlLane::PitchBend:  return m.isPitchWheel();
        case CtrlLane::Modulation: return m.isController() && m.getControllerNumber() == 1;
        case CtrlLane::Expression: return m.isController() && m.getControllerNumber() == 11;
        case CtrlLane::Pan:        return m.isController() && m.getControllerNumber() == 10;
        case CtrlLane::Sustain:    return m.isController() && m.getControllerNumber() == 64;
        case CtrlLane::Velocity:
        default:                   return false;
    }
}

juce::MidiMessage PianoRollEditor::makeLaneMessage(CtrlLane lane, int value, double t) const
{
    const int ch = juce::jlimit(1, 16, clip.getChannel() + 1);
    juce::MidiMessage m;
    switch (lane)
    {
        case CtrlLane::PitchBend:  m = juce::MidiMessage::pitchWheel(ch, juce::jlimit(0, 16383, value)); break;
        case CtrlLane::Modulation: m = juce::MidiMessage::controllerEvent(ch, 1,  juce::jlimit(0, 127, value)); break;
        case CtrlLane::Expression: m = juce::MidiMessage::controllerEvent(ch, 11, juce::jlimit(0, 127, value)); break;
        case CtrlLane::Pan:        m = juce::MidiMessage::controllerEvent(ch, 10, juce::jlimit(0, 127, value)); break;
        case CtrlLane::Sustain:    m = juce::MidiMessage::controllerEvent(ch, 64, value >= 64 ? 127 : 0); break;
        case CtrlLane::Velocity:
        default: break;
    }
    m.setTimeStamp(t);
    return m;
}

juce::String PianoRollEditor::laneName(CtrlLane lane) const
{
    switch (lane)
    {
        case CtrlLane::PitchBend:  return tr(u8"ピッチベンド");
        case CtrlLane::Modulation: return tr(u8"モジュレーション");
        case CtrlLane::Expression: return tr(u8"エクスプレッション");
        case CtrlLane::Pan:        return tr(u8"パン");
        case CtrlLane::Sustain:    return tr(u8"サスティーン");
        case CtrlLane::Velocity:
        default:                   return "Velocity";
    }
}

juce::String PianoRollEditor::laneValueText(CtrlLane lane, int value)
{
    switch (lane)
    {
        case CtrlLane::PitchBend:
        {
            const int c = value - 8192;   // センター 0 の ± 表示
            return (c > 0 ? "+" : "") + juce::String(c);
        }
        case CtrlLane::Pan:
            if (value == 64) return "C";
            return value < 64 ? "L" + juce::String(64 - value)
                              : "R" + juce::String(value - 64);
        case CtrlLane::Sustain:
            return value >= 64 ? "ON" : "OFF";
        default:
            return juce::String(value);
    }
}

int PianoRollEditor::laneValueFromY(int y, CtrlLane lane) const
{
    const int y0 = getHeight() - velocityH + 6;
    const int y1 = getHeight() - kScrollBarH - 6;   // コンテンツ下端 (スクロールバー帯は除く)
    const float norm = 1.0f - juce::jlimit(0.0f, 1.0f,
        (float)(y - y0) / (float) juce::jmax(1, y1 - y0));
    switch (lane)
    {
        case CtrlLane::PitchBend: return (int) std::lround(norm * 16383.0f);
        case CtrlLane::Sustain:   return norm >= 0.5f ? 127 : 0;
        default:                  return (int) std::lround(norm * 127.0f);
    }
}

int PianoRollEditor::laneValueToY(int value, CtrlLane lane) const
{
    const int y0 = getHeight() - velocityH + 6;
    const int y1 = getHeight() - kScrollBarH - 6;   // laneValueFromY と同じレンジ
    const float maxV = (lane == CtrlLane::PitchBend) ? 16383.0f : 127.0f;
    const float norm = juce::jlimit(0.0f, 1.0f, (float) value / maxV);
    return y1 - (int) std::lround(norm * (float)(y1 - y0));
}

int PianoRollEditor::laneEffectiveValueAt(double t, CtrlLane lane) const
{
    // t 時点の実効値 = 直前 (t 以下) の同レーンイベント。無ければレーンの既定値
    int v = -1;
    for (const auto& m : ctrlMsgs)
    {
        if (m.getTimeStamp() > t) break;   // ctrlMsgs は時刻順
        if (msgMatchesLane(m, lane))
            v = (lane == CtrlLane::PitchBend) ? m.getPitchWheelValue() : m.getControllerValue();
    }
    if (v >= 0) return v;
    switch (lane)
    {
        case CtrlLane::PitchBend:  return 8192;   // センター
        case CtrlLane::Expression: return 127;    // 全開 (GM 既定)
        case CtrlLane::Pan:        return 64;     // センター
        default:                   return 0;      // モジュレーション / サスティーン
    }
}

void PianoRollEditor::removeLaneEventsBetween(double t1, double t2, CtrlLane lane)
{
    ctrlMsgs.erase(std::remove_if(ctrlMsgs.begin(), ctrlMsgs.end(),
        [&](const juce::MidiMessage& m)
        {
            const double t = m.getTimeStamp();
            return t >= t1 && t <= t2 && msgMatchesLane(m, lane);
        }), ctrlMsgs.end());
}

void PianoRollEditor::insertLaneEvent(const juce::MidiMessage& m)
{
    // 時刻順を保って挿入 (laneEffectiveValueAt / 描画のステップ線が順序に依存する)
    auto it = std::upper_bound(ctrlMsgs.begin(), ctrlMsgs.end(), m.getTimeStamp(),
        [](double t, const juce::MidiMessage& x) { return t < x.getTimeStamp(); });
    ctrlMsgs.insert(it, m);
}

void PianoRollEditor::applyCtrlDragPoint(juce::Point<int> pos, bool erase)
{
    const double clipLen = clip.getDuration();
    const double maxT = (clipLen > 0.0) ? juce::jmax(0.0, clipLen - 1e-4) : 1.0e9;
    double t = juce::jlimit(0.0, maxT, xToTime(pos.x));
    // GRID オン時は入力位置をグリッドへスナップ (グリッド単位の段付き入力になる)。
    // 消去はスナップしない (掃いた範囲をそのまま消す)
    const double u = snapUnitSecs();
    if (!erase && u > 0.0)
        t = juce::jlimit(0.0, maxT, std::round(t / u) * u);
    const int v = laneValueFromY(pos.y, ctrlLane);

    // 掃いた区間の既存イベントを除去してから (erase はここまで)、新しい点を置く。
    // 速いドラッグでも隙間が出ないよう、前回点から線形補間で埋める
    // (間隔 = GRID オンならグリッド単位 / Off なら kCtrlStepSec)
    const bool hasPrev = ctrlDragLastT >= 0.0;
    const double segA = hasPrev ? juce::jmin(ctrlDragLastT, t) : t;
    const double segB = hasPrev ? juce::jmax(ctrlDragLastT, t) : t;
    removeLaneEventsBetween(segA - 1e-6, segB + 1e-6, ctrlLane);

    if (!erase)
    {
        const double step = (u > 0.0) ? u : kCtrlStepSec;
        // サスティーンは 2 値 (ON/OFF) なので補間しない (掃いた区間を最後の値で埋める)
        if (hasPrev && ctrlLane != CtrlLane::Sustain && segB - segA > step)
        {
            const double span = ctrlDragLastT == t ? 1.0 : (t - ctrlDragLastT);
            const double tt0 = (u > 0.0) ? std::ceil((segA + 1e-9) / u) * u : segA + step;
            for (double tt = tt0; tt < segB - 1e-9; tt += step)
            {
                const double frac = juce::jlimit(0.0, 1.0, (tt - ctrlDragLastT) / span);
                const int vv = (int) std::lround(ctrlDragLastV + frac * (v - ctrlDragLastV));
                insertLaneEvent(makeLaneMessage(ctrlLane, vv, tt));
            }
        }
        insertLaneEvent(makeLaneMessage(ctrlLane, v, t));
    }

    ctrlDragLastT = t;
    ctrlDragLastV = v;
    setReadout(erase ? tr(u8"消去") : laneValueText(ctrlLane, v), pos);
}

// レーンの範囲選択内の同レーンイベントを一括削除する (Delete キー)
void PianoRollEditor::deleteCtrlSelection()
{
    if (!ctrlSelActive) return;
    snapshotForUndo();
    removeLaneEventsBetween(ctrlSelT1 - 1e-6, ctrlSelT2 + 1e-6, ctrlLane);
    ctrlSelActive = false;
    writeNotesToClip();
    repaint();
}

// ダブルクリック位置の数値直接入力 (Enter で確定 / Esc・フォーカス喪失で取消)。
// GRID オン時は入力位置をグリッドへスナップする
void PianoRollEditor::beginCtrlValueEdit(juce::Point<int> pos)
{
    const double clipLen = clip.getDuration();
    const double maxT = (clipLen > 0.0) ? juce::jmax(0.0, clipLen - 1e-4) : 1.0e9;
    double t = juce::jlimit(0.0, maxT, xToTime(pos.x));
    const double u = snapUnitSecs();
    if (u > 0.0)
        t = juce::jlimit(0.0, maxT, std::round(t / u) * u);
    ctrlValueEditT = t;

    ctrlValueEditor = std::make_unique<juce::TextEditor>();
    const int w = 64, h = 18;
    const int x = juce::jlimit(keyboardW, juce::jmax(keyboardW, getWidth() - w - 2), pos.x - w / 2);
    const int y = juce::jlimit(getHeight() - velocityH + 1,
                               juce::jmax(getHeight() - velocityH + 1,
                                          getHeight() - kScrollBarH - h - 1),
                               pos.y - h / 2);
    ctrlValueEditor->setBounds(x, y, w, h);
    ctrlValueEditor->setFont(juce::FontOptions(12.0f));
    ctrlValueEditor->setColour(juce::TextEditor::backgroundColourId, juce::Colours::black.withAlpha(0.85f));
    ctrlValueEditor->setColour(juce::TextEditor::textColourId, juce::Colours::white);
    ctrlValueEditor->setColour(juce::TextEditor::outlineColourId, AppColours::accent);
    ctrlValueEditor->setInputRestrictions(6, "-0123456789");
    ctrlValueEditor->setJustification(juce::Justification::centred);
    // 初期値 = その位置の実効値 (ピッチベンドは ± の符号付き数値で入出力)
    const int eff = laneEffectiveValueAt(t, ctrlLane);
    ctrlValueEditor->setText(juce::String(ctrlLane == CtrlLane::PitchBend ? eff - 8192 : eff),
                             juce::dontSendNotification);
    ctrlValueEditor->selectAll();
    ctrlValueEditor->onReturnKey = [this]
    {
        if (ctrlValueEditor != nullptr)
            applyCtrlValueEdit(ctrlValueEditor->getText());
        ctrlValueEditor.reset();
        repaint();
    };
    ctrlValueEditor->onEscapeKey  = [this] { ctrlValueEditor.reset(); repaint(); };
    ctrlValueEditor->onFocusLost  = [this] { ctrlValueEditor.reset(); repaint(); };
    addAndMakeVisible(*ctrlValueEditor);
    ctrlValueEditor->grabKeyboardFocus();
}

void PianoRollEditor::applyCtrlValueEdit(const juce::String& text)
{
    if (text.trim().isEmpty()) return;
    const int raw = text.getIntValue();
    // ピッチベンドは ±8192 の符号付き入力 → 内部値 0..16383 へ (それ以外は 0..127)
    const int value = (ctrlLane == CtrlLane::PitchBend)
        ? juce::jlimit(0, 16383, raw + 8192)
        : juce::jlimit(0, 127, raw);
    snapshotForUndo();
    // 同時刻の既存イベントを置き換える (二重登録防止)
    removeLaneEventsBetween(ctrlValueEditT - 1e-4, ctrlValueEditT + 1e-4, ctrlLane);
    insertLaneEvent(makeLaneMessage(ctrlLane, value, ctrlValueEditT));
    writeNotesToClip();
}

void PianoRollEditor::drawCtrlLane(juce::Graphics& g) const
{
    const int top     = getHeight() - velocityH;
    const int cBottom = getHeight() - kScrollBarH;
    const int bottom  = getHeight();

    juce::Graphics::ScopedSaveState ss(g);
    g.reduceClipRegion(keyboardW, top + 1,
                       juce::jmax(0, getWidth() - keyboardW),
                       juce::jmax(0, cBottom - top - 1));

    // 範囲選択のハイライト (Delete で範囲内のイベントを一括削除)
    if (ctrlSelActive)
    {
        const int x1 = timeToX(ctrlSelT1);
        const int x2 = timeToX(ctrlSelT2);
        if (x2 > x1)
        {
            g.setColour(AppColours::accent.withAlpha(0.15f));
            g.fillRect(x1, top + 1, x2 - x1, cBottom - top - 2);
            g.setColour(AppColours::accent.withAlpha(0.6f));
            g.drawRect(x1, top + 1, x2 - x1, cBottom - top - 2, 1);
        }
    }

    // センターライン (ピッチベンド ±0 / パン C)
    if (ctrlLane == CtrlLane::PitchBend || ctrlLane == CtrlLane::Pan)
    {
        const int cy = laneValueToY(ctrlLane == CtrlLane::PitchBend ? 8192 : 64, ctrlLane);
        g.setColour(AppColours::separator);
        g.drawHorizontalLine(cy, (float) keyboardW, (float) getWidth());
    }

    // ステップ線 (値の持続) + イベント点。クリップ範囲外 (末尾より右) は暗いまま
    const auto col = clip.getColour().brighter(0.2f);
    const double clipLen = clip.getDuration();
    const int laneRight  = (clipLen > 0.0) ? juce::jmin(getWidth(), timeToX(clipLen)) : getWidth();
    int prevX = keyboardW;
    int prevY = laneValueToY(laneEffectiveValueAt(juce::jmax(0.0, xToTime(keyboardW)), ctrlLane), ctrlLane);
    bool any = false;
    g.setColour(col.withAlpha(0.85f));
    for (const auto& m : ctrlMsgs)
    {
        if (!msgMatchesLane(m, ctrlLane)) continue;
        const int x = timeToX(m.getTimeStamp());
        const int y = laneValueToY((ctrlLane == CtrlLane::PitchBend)
                                       ? m.getPitchWheelValue() : m.getControllerValue(),
                                   ctrlLane);
        if (x > prevX)
            g.drawHorizontalLine(prevY, (float) prevX, (float) juce::jmin(x, laneRight));
        if (x >= keyboardW && x <= laneRight)
        {
            g.drawVerticalLine(x, (float) juce::jmin(prevY, y), (float) juce::jmax(prevY, y));
            g.fillRect(x - 1, y - 1, 3, 3);
        }
        prevX = juce::jmax(prevX, x);
        prevY = y;
        any = true;
    }
    // 最後のイベント以降 (またはイベント無し) は実効値の水平線を薄く引く
    g.setColour(col.withAlpha(any ? 0.85f : 0.30f));
    if (laneRight > prevX)
        g.drawHorizontalLine(prevY, (float) prevX, (float) laneRight);

    juce::ignoreUnused(bottom);
}

void PianoRollEditor::drawValueReadout(juce::Graphics& g) const
{
    if (ctrlReadout.isEmpty()) return;
    g.setFont(juce::FontOptions(11.0f));
    const int w = juce::GlyphArrangement::getStringWidthInt(
                      juce::Font(juce::FontOptions(11.0f)), ctrlReadout) + 12;
    const int h = 18;
    int x = ctrlReadoutPos.x + 14;
    int y = ctrlReadoutPos.y - h - 6;
    x = juce::jlimit(0, juce::jmax(0, getWidth() - w), x);
    y = juce::jlimit(0, juce::jmax(0, getHeight() - h), y);
    g.setColour(juce::Colours::black.withAlpha(0.75f));
    g.fillRoundedRectangle((float) x, (float) y, (float) w, (float) h, 3.0f);
    g.setColour(juce::Colours::white);
    g.drawText(ctrlReadout, x, y, w, h, juce::Justification::centred);
}

void PianoRollEditor::setReadout(const juce::String& text, juce::Point<int> pos)
{
    if (ctrlReadout == text && (pos - ctrlReadoutPos).getDistanceFromOrigin() < 2) return;
    ctrlReadout    = text;
    ctrlReadoutPos = pos;
    repaint();
}

void PianoRollEditor::clearReadout()
{
    if (ctrlReadout.isEmpty()) return;
    ctrlReadout.clear();
    repaint();
}

void PianoRollEditor::layoutLaneBox()
{
    // 左ヘッダセル (鍵盤列と同じ幅) の上部に収める
    laneBtn.setBounds(2, getHeight() - velocityH + 3, keyboardW - 5, 17);
}

void PianoRollEditor::resized()
{
    // ツールバー段 (ルーラーの上) の右寄せ: GRID ComboBox、その左に ペン (アイコン) /
    // 追従 (アイコン) / ステップ (アイコン)。ステップ入力 ON の間はさらに左へ STEP 長 ComboBox。
    // 小節番号バーとは段を分けているので、機能が増えても番号と重ならない
    const int ty = 3;
    const int th = toolbarH - 6;
    snapBox.setBounds(getWidth() - 130, ty, 124, th);
    penBtn.setBounds(snapBox.getX() - 4 - 28, ty, 28, th);
    followBtn.setBounds(penBtn.getX() - 4 - 28, ty, 28, th);
    stepBtn.setBounds(followBtn.getX() - 4 - 28, ty, 28, th);
    stepLenBox.setBounds(stepBtn.getX() - 4 - 110, ty, 110, th);
    // 下部レーンの種類選択 (Velocity 領域の左上・velocityH に追従)
    layoutLaneBox();
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
        noteManualHScroll();  // 手動スクロール → 追従の一時停止判定
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

// ── ステップ入力 ─────────────────────────────────────────────────────────
void PianoRollEditor::setMidiInputAvailable(bool v)
{
    midiInputAvailable = v;
    stepBtn.setVisible(v);
    if (!v && stepMode)
    {
        // 切断されたら入力手段が無いのでステップ入力も解除する (カーソル表示も消す)
        stepMode = false;
        stepBtn.setToggleState(false, juce::dontSendNotification);
        stepHeldKeys.clear();
        repaint();
    }
    stepLenBox.setVisible(v && stepMode);
}

void PianoRollEditor::moveStepCursor(double newPos)
{
    const double clipLen = clip.getDuration();
    stepPosSec = juce::jmax(0.0, newPos);
    if (clipLen > 0.0) stepPosSec = juce::jmin(stepPosSec, clipLen);
    // カーソルがビュー外へ出たら追いかける (置いた続きが常に見える)
    const int x = timeToX(stepPosSec);
    if (x < keyboardW || x > getWidth() - 40)
    {
        scrollX = juce::jmax(0, (int) std::round(stepPosSec * pixelsPerSec) - 80);
        updateScrollBarRange();
    }
    repaint();
}

// MIDI キーボードのノートを受けてカーソル位置へノートを置く (MainComponent から message
// thread で呼ばれる)。同時に押した鍵は同じ開始位置 (stepChordPos) に和音として置き、
// 全ての鍵が離れた時点でカーソルを 1 ステップ進める (一般的な DAW のステップ入力と同じ)。
void PianoRollEditor::handleStepInputMidi(int note, float velocity, bool isOn)
{
    if (!stepMode) return;
    const double len = stepLenSecs();
    if (len <= 0.0) return;
    const double clipLen = clip.getDuration();

    if (!isOn)
    {
        // 対応する note-on を受けていない迷子 note-off (S トグル前から押していた鍵等) では
        // 進めない — 空集合のまま advance すると stale な stepChordPos へカーソルが飛ぶ
        if (stepHeldKeys.erase(note) > 0 && stepHeldKeys.empty())
            moveStepCursor(stepChordPos + len);
        return;
    }

    if (stepHeldKeys.empty())
    {
        stepChordPos = stepPosSec;
        selected.clear();   // 新しいステップ = 新しい和音の選択に切り替える
    }
    stepHeldKeys.insert(note);

    // クリップ末尾より先には置けない (advance の対称性のため held には積んだままにする)
    if (clipLen > 0.0 && stepChordPos >= clipLen - 1e-9)
        return;

    snapshotForUndo();
    Note n;
    n.pitch       = juce::jlimit(0, 127, note);
    n.velocity    = juce::jlimit(0.05f, 1.0f, velocity);
    n.startSec    = stepChordPos;
    n.durationSec = juce::jmax(0.01, (clipLen > 0.0) ? juce::jmin(len, clipLen - stepChordPos) : len);
    notes.push_back(n);
    selected.insert((int) notes.size() - 1);
    writeNotesToClip();
    repaint();
}
