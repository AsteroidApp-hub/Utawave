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
//   - ステップ入力は I でトグル (MIDI キーボードでカーソル位置へ順にノートを置く。
//     長さは STEP コンボ = GRID と同じ音価から選択・←/→ でカーソル移動 = 休符送り。
//     S は macOS の GlobalKeyMonitor が「停止」として消費するため使えない)
//   - L でレガート (選択ノートを次のノートの開始位置まで伸ばす)
//   - Q でクォンタイズ (選択ノート / 未選択なら全ノートの開始を GRID の最寄りへスナップ)
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
    void mouseExit(const juce::MouseEvent&) override;
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

    // ── ノート編集コマンド (右クリックメニュー / ショートカット。テストからも直接呼ぶ) ──
    void duplicateSelected();   // Cmd+D: 選択ノートを選択スパン分だけ後ろへ複製 (GRID 倍数へ切上げ)
    void quantizeNoteEnds();    // 終端を GRID の最寄りへ (長さ側のクォンタイズ。選択 / 無選択は全ノート)
    void equalizeLengths();     // 選択ノートの長さを「最も早いノート」の長さに揃える
    void resolveOverlaps();     // 同一ピッチの重なりをトリム (内包は後発を削除。選択 / 無選択は全ノート)

    // ── テスト用シーム (UtawaveTests がレーン編集をヘッドレスで駆動する) ──
    // sendNotificationSync で本物のボタン/コンボ経路 (onClick / onChange) を同期実行する
    void setPenModeForTests(bool v)      { penBtn.setToggleState(v, juce::sendNotificationSync); }
    bool getPenModeForTests() const      { return penMode; }
    void setCtrlLaneForTests(int idx)    { selectCtrlLane((CtrlLane) idx); }
    bool hasCtrlSelectionForTests() const { return ctrlSelActive; }
    int  getSelectedCountForTests() const { return (int) selected.size(); }
    void setStepModeForTests(bool v)
    {
        setMidiInputAvailable(true);   // ボタンは MIDI 接続中のみ有効なので先に開放
        stepBtn.setToggleState(v, juce::sendNotificationSync);
    }
    double getStepPosForTests() const { return stepPosSec; }

    // ── ステップ入力 (MIDI キーボードで順にノートを置く・I でトグル) ──
    // MainComponent が MIDI キーボードのノートを (message thread で) ここへ転送する。
    // 同時に押した鍵は同じ位置に和音として置き、全部離した時にカーソルが 1 ステップ進む。
    bool isStepInputActive() const { return stepMode; }
    void handleStepInputMidi(int note, float velocity, bool isOn);
    // MIDI キーボードが使える時だけステップ入力ボタンを出す (未接続では入力手段が無い)。
    // MainComponent が生成時 (openPianoRollFor) と接続状態の変化時 (applyMidiInputFromPrefs)
    // に設定する。切断時はステップ入力中でも解除する
    void setMidiInputAvailable(bool v);

    // Velocity 領域の高さ (境界ドラッグで調整可)。MainComponent がアプリ設定
    // (AppPreferences::pianoRollVelocityH) から生成時に適用し、変更を保存する
    void setVelocityAreaHeight(int h)
    {
        velocityH = juce::jlimit(kVelMinH, kVelMaxH, h);
        layoutLaneBox();
        repaint();
    }
    std::function<void(int)> onVelocityAreaResized;

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
    void quantizeSelected(); // Q: 選択ノート (未選択なら全ノート) の開始を GRID にスナップ
    void showNoteContextMenu();  // ノート/選択の右クリックメニュー (終端クォンタイズ・重なり解消等)
    // ノート名 ("C4" 等)。鍵盤ラベルと同じ表記 (オクターブ = pitch/12 - 1)
    static juce::String noteNameFor(int pitch);
    void snapshotForUndo();  // 状態を内部 Undo スタックへ保存

    // ─ 描画ヘルパー ─
    void drawKeyboard(juce::Graphics&) const;
    void drawGrid(juce::Graphics&) const;
    void drawNotes(juce::Graphics&) const;
    void drawVelocityArea(juce::Graphics&) const;
    void drawCtrlLane(juce::Graphics&) const;      // Velocity 以外の下部レーン (CC / PB)
    void drawValueReadout(juce::Graphics&) const;  // ドラッグ/ホバー中の数値表示

    // ── 下部レーン (Velocity / MIDI コントロール) ──────────────────────────
    // Velocity 以外は MidiClip シーケンス内のイベント (ピッチベンド / CC) を直接編集する。
    // ノート以外のイベントは ctrlMsgs に保持し、writeNotesToClip がノートと一緒に書き戻す
    // (旧実装は書き戻しでノート以外を全て失っていた = SMF 由来の CC も編集で消えていた)。
    enum class CtrlLane { Velocity = 0, PitchBend, Modulation, Expression, Pan, Sustain };
    CtrlLane        ctrlLane { CtrlLane::Velocity };
    // レーン選択: 左ヘッダセル (鍵盤列と同じ幅) に収まる小型ボタン (短縮名 + ▾)。
    // クリックで正式名称のポップアップから選ぶ (幅 44px に正式名称は入らないため)
    juce::TextButton laneBtn;
    void selectCtrlLane(CtrlLane lane);            // 切替の実処理 (ボタンラベル/選択解除/repaint)
    void showLaneMenu();
    static const char* laneShortName(CtrlLane lane);
    void layoutLaneBox();                          // velocityH に追従して置き直す
    // レーンの枠 (背景 / 左ヘッダセル / 目盛りラベル / 拍グリッド)。Velocity・コントロール共用
    void drawLaneFrame(juce::Graphics&) const;
    std::vector<juce::MidiMessage> ctrlMsgs;       // ノート以外の全イベント (時刻順・クリップ相対秒)

    static bool msgMatchesLane(const juce::MidiMessage& m, CtrlLane lane);
    juce::MidiMessage makeLaneMessage(CtrlLane lane, int value, double t) const;
    juce::String laneName(CtrlLane lane) const;
    static juce::String laneValueText(CtrlLane lane, int value);  // PB は ±表示 / Pan は L/C/R / Sustain は ON/OFF
    int  laneValueFromY(int y, CtrlLane lane) const;   // レーン内 y → 値 (PB 0..16383 / CC 0..127)
    int  laneValueToY(int value, CtrlLane lane) const;
    int  laneEffectiveValueAt(double t, CtrlLane lane) const;  // t 時点の実効値 (直前イベント / 既定値)
    void removeLaneEventsBetween(double t1, double t2, CtrlLane lane);
    int  insertLaneEvent(const juce::MidiMessage& m);  // 時刻順を保って挿入 (挿入 index を返す)
    // 既存イベント点の掴み調整 (素のクリックで点を掴んで値/時刻をドラッグ)。-1 = 非ヒット
    int  hitTestCtrlPoint(juce::Point<int> p) const;
    int  ctrlPointIdx { -1 };                          // ドラッグ中の ctrlMsgs index
    static constexpr int kCtrlPointHitPx { 6 };
    // ドラッグ 1 点を適用 (前回点から線形補間で埋める)。erase=true は範囲の消去のみ
    void applyCtrlDragPoint(juce::Point<int> pos, bool erase);
    double ctrlDragLastT { -1.0 };
    int    ctrlDragLastV { 0 };
    bool   ctrlErasing   { false };
    bool   ctrlFreeDraw  { false };   // Cmd+Option: GRID を一時解除して可変 (フリー) 描画
    static constexpr double kCtrlStepSec { 0.01 };  // GRID:Off 時の補間の最小間隔 (10ms)

    // レーンの時間範囲選択 (通常ドラッグ)。Delete で範囲内のレーンイベントを一括削除する
    bool   ctrlSelActive  { false };
    double ctrlSelT1      { 0.0 };
    double ctrlSelT2      { 0.0 };
    double ctrlSelAnchorT { 0.0 };
    void   deleteCtrlSelection();

    // ダブルクリックの数値直接入力 (小さな TextEditor を出す。Enter 確定 / Esc・フォーカス喪失で取消)
    std::unique_ptr<juce::TextEditor> ctrlValueEditor;
    double ctrlValueEditT { 0.0 };
    void beginCtrlValueEdit(juce::Point<int> pos);
    void applyCtrlValueEdit(const juce::String& text);

    // 数値の見える化: ドラッグ / ホバー中に「レーン名 値」をカーソル脇に表示する
    juce::String     ctrlReadout;
    juce::Point<int> ctrlReadoutPos;
    void setReadout(const juce::String& text, juce::Point<int> pos);
    void clearReadout();

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
    // Velocity 領域の高さ。上端の境界を上下ドラッグで調整できる (kVelMinH〜、上限は
    // グリッド域を最低 kGridMinH 残す)。値はアプリ全体設定 (AppPreferences::pianoRollVelocityH)
    // として記憶し、次に開くピアノロールにも引き継ぐ
    int    velocityH    { 80 };
    static constexpr int kVelMinH        { 40 };
    static constexpr int kVelMaxH        { 400 };
    static constexpr int kGridMinH       { 100 };  // 調整中もノートグリッドを最低これだけ残す
    static constexpr int kVelResizeHitPx { 4 };    // 境界の掴み判定 (±px)
    int    velResizeStartH { 80 };                 // ドラッグ開始時の velocityH
    int    rulerH       { 22 };
    // ルーラーの上のツールバー段 (ステップ入力/追従/ペン/GRID 等のボタン置き場)。
    // 以前は小節番号バーにボタンを重ねていたが、機能が増えて番号と被り見づらくなった
    // ため段を分離した。グリッド域の上端は topH() = toolbarH + rulerH
    int    toolbarH     { 26 };
    int    topH() const { return toolbarH + rulerH; }

    // ピッチ表示範囲 (常時)
    static constexpr int minPitch = 0;
    static constexpr int maxPitch = 127;

    // ドラッグ状態
    enum class DragMode { None, MoveNotes, ResizeLeft, ResizeRight, RubberBand, AdjustVelocity,
                          CreateNote, ResizeVelocityArea, DrawCtrl, SelectCtrlRange, RulerDrag,
                          AuditionKey, VelocityRamp, MoveCtrlPoint };

    // 左鍵盤クリックの試聴 (押している間鳴らす・縦ドラッグでグリッサンド)。-1 = 非試聴
    int auditionPitch { -1 };

    // ベロシティ一括編集: バードラッグ開始時の (index, 元 velocity)。ドラッグしたバーが
    // 複数選択に含まれていれば選択全体へ同じ増減を適用する
    std::vector<std::pair<int, float>> velDragOrig;
    // ノートグリッド上の Cmd+Shift+ドラッグでのベロシティ調整 (和音でバーが重なっていても
    // ノート自体を掴めば個別に調整できる)。true の間は絶対 y でなく相対 dy で増減する
    bool velDragFromGrid { false };
    // ベロシティのランプ描画 (バーの無い空きをドラッグ → 範囲内のノートに直線の値を適用)
    double rampT0 { 0.0 }, rampT1 { 0.0 };
    float  rampV0 { 0.0f }, rampV1 { 0.0f };
    void   applyVelocityRamp();   // [rampT0..T1] のノートへ線形補間値を適用 (ドラッグ中毎回)

    // ルーラー (小節番号バー) のドラッグ: クリック = mouseUp で確定シーク / 縦ドラッグ = 横ズーム
    // (メインタイムラインの TimelineRuler::beginSeekDrag と同じ操作感。ズーム中に再生バーが
    // 飛ばないようシークは方向が確定するまで保留する)
    bool   rulerSeekArmed      { false };
    bool   rulerZooming        { false };
    double rulerZoomStartPps   { 200.0 };
    double rulerZoomAnchorTime { 0.0 };   // クリック位置の時刻 (ズームの支点・シーク先)
    int    rulerZoomAnchorX    { 0 };
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
    double                     pendingBeforeDur { 0.0 };   // Cmd+D のクリップ自動延長を undo で戻すため
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

    // ステップ入力 (階段アイコン)。ON 中は stepLenBox (ノート長) を隣に出す。
    // ボタン自体は MIDI キーボード接続中のみ表示 (setMidiInputAvailable)
    juce::DrawableButton stepBtn { "step", juce::DrawableButton::ImageOnButtonBackground };
    bool           midiInputAvailable { false };
    bool           stepMode { false };
    SnapMode       stepLenMode { SnapMode::Eighth };   // ステップで置くノートの長さ
    bool           stepLenUserSet { false };           // 一度も触っていなければ ON 時に GRID から引き継ぐ
    juce::ComboBox stepLenBox;                         // GRID と同じ音価の選択肢 (Off は除く)
    double         stepPosSec { 0.0 };                 // 入力カーソル (クリップ先頭からの秒)
    std::set<int>  stepHeldKeys;                       // 押下中の鍵 (全て離れたら 1 ステップ進む)
    double         stepChordPos { 0.0 };               // いま置いている和音の開始位置
    double stepLenSecs() const { return snapModeUnitSecs(stepLenMode, bpm); }
    void   moveStepCursor(double newPos);              // クランプ + スクロール追従 + repaint

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
