// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

#pragma once
#include <JuceHeader.h>
#include "../Tracks/MidiClip.h"
#include "../AppSettings.h"  // SnapMode

// MIDI クリップ用ピアノロール エディタ
// 標準的な機能:
//   - ノートの追加 (空エリアダブルクリック / ペンモード中はクリック+ドラッグ・D でトグル) /
//     削除 (Delete) / 移動 (ドラッグ) / リサイズ (端ドラッグ) / 分割 (Option+クリック)
//   - 追従 (自動ページング) は F でトグル (ボタンと同一経路)
//   - L でレガート (選択ノートを次のノートの開始位置まで伸ばす)
//   - 範囲選択 (空エリアドラッグ) / Shift / Cmd で複数選択
//   - Cmd+C / Cmd+V / Cmd+X コピー・ペースト・カット
//   - Cmd+A 全選択
//   - Cmd+Z / Cmd+Shift+Z Undo / Redo (内部 UndoManager)
//   - 下部に Velocity バー (ドラッグで個別調整)
class PianoRollEditor : public juce::Component,
                        public juce::ScrollBar::Listener
{
public:
    PianoRollEditor(MidiClip& clipRef, double projectBpm,
                    double initialFocusTimeSec = -1.0);
    ~PianoRollEditor() override;

    // ノート単発プレビュー用コールバック (note, velocity, isOn)
    // ピアノロールがクリック/矢印移動などでノートを試聴する際に呼ばれる
    std::function<void(int /*note*/, float /*velocity*/, bool /*isOn*/)> onPreviewNote;

    // ルーラー (小節番号バー) クリックでプレイヘッドを移動させたいときの通知。
    // 引数はクリップ先頭からの秒数 (>= 0)。MainComponent 側で
    // 「クリップ start position + 引数」を全体プレイヘッドに反映する。
    std::function<void(double /*secsInClip*/)> onSeek;

    // Space キーで再生 / 停止をトグルする通知。ピアノロールは独立ウィンドウ (always-on-top) の
    // ため、フォーカスがある間 Space がメイン画面のコマンドへ届かない。macOS は GlobalKeyMonitor が
    // Space をグローバル捕捉して消費するのでこの経路は使われないが、Windows/Linux にはそれが無く
    // Space が宙に浮いて「ピアノロールから再生できない」ため、keyPressed からこれを呼ぶ。
    std::function<void()> onTogglePlay;

    // 再生バー位置 (クリップ先頭からの秒数) を更新
    void setPlayheadPosition(double secs);

    // 自動ページング (再生バーがビュー外へ出たら次ページへ横スクロール) の ON/OFF。
    // アプリ全体設定 (AppPreferences::midiPagingEnabled) から MainComponent が設定する。
    // ルーラー右上の FOLLOW ボタンとも同期する。
    void setPagingEnabled(bool v);

    // FOLLOW ボタンでユーザーが追従を切替えたときの通知。
    // MainComponent がアプリ設定 (midiPagingEnabled) へ書き戻して永続化する。
    std::function<void(bool)> onFollowToggled;

    // ツールチップの表示 ON/OFF (アプリ設定 showTooltips から MainComponent が設定)。
    // ピアノロールは独立ウィンドウ (別ピア) のため、メイン画面の TooltipWindow では
    // チップが出ない (JUCE は親と同じピアのコンポーネントしか対象にしない)。窓専用の
    // TooltipWindow をここで生成/破棄する。
    void setTooltipsEnabled(bool enabled);

    // グリッドモード変更
    void setSnapMode(SnapMode m) { snapMode = m; repaint(); }
    SnapMode getSnapMode() const { return snapMode; }

    void paint(juce::Graphics&) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseDoubleClick(const juce::MouseEvent&) override;
    void mouseMove(const juce::MouseEvent&) override;
    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    void modifierKeysChanged(const juce::ModifierKeys&) override;
    bool keyPressed(const juce::KeyPress&) override;

    // juce::ScrollBar::Listener
    void scrollBarMoved(juce::ScrollBar*, double newRangeStart) override;

    // 編集後、MidiClip の MidiMessageSequence へ反映するコールバック
    std::function<void()> onChanged;

    // メイン UndoManager との統合 (set すれば Cmd+Z/Cmd+Shift+Z が main 側で動く)。
    // nullptr のままなら従来通り内部スタックを使用 (後方互換)。
    void setUndoManager(juce::UndoManager* um) { externalUndoManager = um; }

    // undo/redo で MidiClip が書き換わったときに、現在開いている
    // PianoRollEditor を見つけて reloadNotesFromClip() を呼ぶためのコールバック。
    // (Editor インスタンスではなく MainComponent 側で集中管理する経路)
    void setExternalReloadCallback(std::function<void(MidiClip*)> cb)
    {
        externalReloadCallback = std::move(cb);
    }

    // ピアノロールにフォーカスがある状態で Cmd+Z/Cmd+Shift+Z/Cmd+Y を押した時に呼ばれる。
    // 共有 UndoManager を直接 undo/redo すると、履歴の先頭が AudioClip 編集 (分割/削除等) の
    // 場合にタイムラインの選択生ポインタがダングリングしうるため、メイン側で
    // clearSelectionsAfterExternalEdit() を呼んでもらう (メイン経路の keyPressed と同じ後始末)。
    void setExternalUndoRedoCallback(std::function<void()> cb)
    {
        externalUndoRedoCallback = std::move(cb);
    }

    // MidiClip 側のシーケンスが外部から書き換わった (undo/redo 等) ときに
    // 内部 Note 配列を再構築する。MidiNotesAction から呼ばれる。
    void reloadNotesFromClip();

private:
    struct Note
    {
        int   pitch    { 60 };
        float velocity { 0.8f };
        double startSec { 0.0 };
        double durationSec { 0.25 };
        bool operator==(const Note& o) const
        {
            return pitch == o.pitch
                && std::abs(velocity - o.velocity) < 1e-6f
                && std::abs(startSec - o.startSec) < 1e-9
                && std::abs(durationSec - o.durationSec) < 1e-9;
        }
    };

    // ─ 座標変換 ─
    int     pitchToY(int pitch) const;
    int     yToPitch(int y) const;
    int     timeToX(double secs) const;
    double  xToTime(int x) const;

    // ─ ヒットテスト ─
    enum class HitKind { None, NoteBody, NoteLeftEdge, NoteRightEdge };
    struct HitResult { int noteIdx { -1 }; HitKind kind { HitKind::None }; };
    HitResult hitTestNote(juce::Point<int> p) const;
    int       hitTestVelocityBar(juce::Point<int> p) const;  // noteIdx, -1 = none

    // ─ シーケンス変換 ─
    void rebuildNotesFromClip();
    void writeNotesToClip();

    // ─ 編集操作 ─
    int  createNoteAt(juce::Point<int> pos);        // 新規ノート作成 (index / -1)。undo snapshot 込み
    void splitNoteAt(int noteIdx, double rawSecs);  // Option+クリックのノート分割 (クリック位置ちょうど)
    void deleteSelected();
    void selectAll();
    void copySelected();
    void cutSelected();
    void pasteAtPlayhead();
    void nudgeSelected(double secs, int semis);
    void legatoSelected();  // L: 選択ノートを次のノートの開始位置まで伸ばす (レガート)
    void snapshotForUndo();  // 状態を内部 Undo スタックへ保存

    // ─ 描画ヘルパー ─
    void drawKeyboard(juce::Graphics&) const;
    void drawGrid(juce::Graphics&) const;
    void drawNotes(juce::Graphics&) const;
    void drawVelocityArea(juce::Graphics&) const;

    MidiClip&           clip;
    double              bpm;
    std::vector<Note>   notes;
    std::set<int>       selected;

    // ビュー状態
    double pixelsPerSec { 200.0 };
    int    pitchHeight  { 14 };
    int    scrollX      { 0 };
    int    scrollY      { 0 };
    int    keyboardW    { 48 };
    int    velocityH    { 80 };
    int    rulerH       { 22 };

    // ピッチ表示範囲 (常時)
    static constexpr int minPitch = 0;
    static constexpr int maxPitch = 127;

    // ドラッグ状態
    enum class DragMode { None, MoveNotes, ResizeLeft, ResizeRight, RubberBand, AdjustVelocity, CreateNote };
    DragMode          dragMode { DragMode::None };
    juce::Point<int>  dragStart;
    int               draggedIdx { -1 };
    int               velocityIdx { -1 };
    std::vector<Note> dragOrigNotes;
    juce::Rectangle<int> rubberBand;

    // ノート作成中の状態
    int    createdNoteIdx { -1 };
    double createdStartSec { 0.0 };

    // クリップボード
    std::vector<Note> clipboard;
    double            clipboardMinStart { 0.0 };

    // 内部 Undo スタック (シンプルなノートリスト snapshot)。
    // externalUndoManager が設定されていればそちらを優先使用する (#36)。
    std::vector<std::vector<Note>> undoStack;
    std::vector<std::vector<Note>> redoStack;
    static constexpr int kUndoMax = 100;

    // メイン UndoManager (main 統合用)。nullptr なら内部スタック動作。
    juce::UndoManager* externalUndoManager { nullptr };
    // MidiClip が undo/redo で書き換わったときに該当エディタを更新するコールバック。
    // MainComponent 側で pianoRollWindows を走査して reloadNotesFromClip() を呼ぶ。
    std::function<void(MidiClip*)> externalReloadCallback;
    std::function<void()>          externalUndoRedoCallback;
    // snapshotForUndo() で保存した「編集前のシーケンス」。
    // 編集が完了したタイミング (callAsync) で MidiNotesAction として確定する。
    juce::MidiMessageSequence pendingBeforeSeq;
    bool                       pendingCommit { false };
    // ドラッグ (ジェスチャ) 中は true。commit を mouseUp まで保留してドラッグ全体を
    // 1 つの Undo アクションにまとめる (callAsync 頼みだとドラッグイベントの合間に
    // commit が発火して微小トランザクションに分割されうる)
    bool                       gestureActive { false };
    void commitPendingUndoAction();

    // 既定値
    double defaultNoteDurSec { 0.25 };  // 新規作成時のデフォルト長 (約 16 分音符 @ 120bpm)

    // プレビュー中のノートと note-off スケジューラ
    int previewActiveNote { -1 };
    void firePreview(int note, float velocity, double durationSec);

    // 再生バー位置 (クリップ先頭からの秒)
    // 既定値 -1e9 = 未設定 (描画スキップ)。-1.0 等の通常の負値は「クリップより前」を意味し
    // ピアノロール左端にクランプ表示する。
    double playheadSec { -1e9 };

    // 自動ページング (再生バーがビュー外へ出たら次ページへ横スクロール)。MainComponent が設定。
    bool pagingEnabled { false };
    // 手動横スクロールで再生バーがビュー外に出ている間はページングを一時停止する
    // (「スクロールすると追従に引き戻される」防止)。再生バーがビュー内へ戻ると自動再開。
    bool followSuspended { false };
    void noteManualHScroll();  // 手動横スクロール後に一時停止状態を更新する

    // ペンツール (ON 中は空きエリアのクリックでノート作成 + ドラッグで長さ調整)。
    // Cmd を押している間は penMode に依らず一時的にペンとして振る舞う (penActive)。
    bool penMode { false };

    // ホバー時のカーソルを位置と修飾キーから決める (mouseMove / modifierKeysChanged 共用)
    void updateHoverCursor(juce::Point<int> pos, const juce::ModifierKeys& mods);

    // ルーラー右上のツールボタン (追従 = プレイヘッド追従アイコン / ペン = 鉛筆アイコン)
    // 追従はテキスト "追従" だと Windows で潰れて読みにくいためアイコン化した
    juce::DrawableButton followBtn { "follow", juce::DrawableButton::ImageOnButtonBackground };
    juce::DrawableButton penBtn { "pen", juce::DrawableButton::ImageOnButtonBackground };

    // この窓専用のツールチップ表示 (setTooltipsEnabled で生成/破棄)
    std::unique_ptr<juce::TooltipWindow> tooltipWin;

    // グリッド (スナップ) 設定
    SnapMode       snapMode { SnapMode::Sixteenth };
    juce::ComboBox snapBox;
    double snapTimeSecs(double secs) const;
    double snapUnitSecs() const;

    // 横スクロールバー (底部)
    juce::ScrollBar hScrollBar { false };  // false = 横方向
    static constexpr int kScrollBarH = 12;
    void updateScrollBarRange();           // 内容幅と現在のビュー幅をスクロールバーに反映

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PianoRollEditor)
};
