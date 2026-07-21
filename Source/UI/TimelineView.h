// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

#pragma once
#include <JuceHeader.h>
#include "../Localisation.h"
#include <vector>
#include "../Tracks/TrackManager.h"
#include "../AppSettings.h"
#include "../Edit/EditActions.h"

struct Marker
{
    double       time   { 0.0 };
    juce::String name;
    juce::Colour colour { juce::Colour(0xffffaa44) };  // デフォルト橙
};

// クロスフェードへの参照（選択・削除で使用）
struct CrossfadeRef
{
    AudioClip* clipA { nullptr };  // 左クリップ（終端が右クリップ内に入る）
    AudioClip* clipB { nullptr };  // 右クリップ（開始が左クリップ内に入る）
    bool valid() const { return clipA != nullptr && clipB != nullptr; }
    void clear()       { clipA = nullptr; clipB = nullptr; }
    bool operator==(const CrossfadeRef& o) const { return clipA == o.clipA && clipB == o.clipB; }
};

// クリップへの参照（選択・編集操作で使用）
struct ClipRef
{
    Track*     track   { nullptr };
    Lane*      lane    { nullptr };
    AudioClip* clip    { nullptr };
    int        trackIdx { -1 };
    int        laneIdx  { -1 };

    bool valid() const { return clip != nullptr; }
    void clear() { track = nullptr; lane = nullptr; clip = nullptr; trackIdx = -1; laneIdx = -1; }
    bool operator==(const ClipRef& o) const { return clip == o.clip; }
};

class TimelineRuler : public juce::Component
{
public:
    TimelineRuler();
    void paint(juce::Graphics&) override;

    void setBpm(double bpm)           { currentBpm = bpm;    repaint(); }
    void setPixelsPerBeat(double ppb) { pixelsPerBeat = ppb; repaint(); }
    void setScrollX(double x)         { scrollX = x;         repaint(); }
    void setPlayheadX(double newX)
    {
        if (std::abs(newX - playheadX) < 0.5) return;
        const int oldXi = (int)(playheadX - scrollX);
        const int newXi = (int)(newX      - scrollX);
        playheadX = newX;
        // プレイヘッド線が走る帯だけを repaint（全面 repaint を回避）
        const int x1 = juce::jmin(oldXi, newXi) - 2;
        const int x2 = juce::jmax(oldXi, newXi) + 3;
        repaint(juce::Rectangle<int>(x1, 0, x2 - x1, getHeight()));
    }
    void setMarkers(const std::vector<Marker>& m) { markers = m; repaint(); }
    const std::vector<Marker>& getMarkers() const { return markers; }
    void setLoopRange(double startSecs, double endSecs, bool active)
    {
        loopStart = startSecs; loopEnd = endSecs; loopActive = active; repaint();
    }
    void setUseMarkerColors(bool v) { useMarkerColors = v; repaint(); }
    bool getUseMarkerColors() const { return useMarkerColors; }
    void setMeter(int num, int den) { meterNum = num; meterDen = den; repaint(); }
    // 時刻行の表示切替
    void setTimeRowVisible(bool v)  { timeRowVisible = v; repaint(); }
    bool isTimeRowVisible() const   { return timeRowVisible; }
    std::function<void(bool)> onTimeRowVisibilityChanged;
    // 小節（バー番号）行の表示切替
    void setBarsRowVisible(bool v)  { barsRowVisible = v; repaint(); }
    bool isBarsRowVisible() const   { return barsRowVisible; }
    std::function<void(bool)> onBarsRowVisibilityChanged;
    // 現在の必要な高さ（行表示状態から算出）
    // ── 行の高さ (paint / getDesiredHeight / hitTestLoopHandle で共有・drift 防止) ──
    // Marker / Tempo / Meter は常時表示 (固定)。Bars / Time のみ表示切替可。
    static constexpr int hMarkerRow = 16;
    static constexpr int hTempoRow  = 14;
    static constexpr int hMeterRow  = 14;
    static constexpr int hBarsRow   = 20;
    static constexpr int hTimeRow   = 16;
    int  getDesiredHeight() const
    {
        const int hBars = barsRowVisible ? hBarsRow : 0;
        const int hTime = timeRowVisible ? hTimeRow : 0;
        return hMarkerRow + hTempoRow + hMeterRow + hBars + hTime;
    }
    void setMeterChanges(const std::vector<MeterChange>& mc) { meterChanges = mc; repaint(); }
    void setBpmChanges(const std::vector<BpmChange>& bc)     { bpmChanges = bc;   repaint(); }
    const std::vector<BpmChange>&   getBpmChanges()   const { return bpmChanges; }
    const std::vector<MeterChange>& getMeterChanges() const { return meterChanges; }

    // ── マーカー / テンポ変更 / 拍子変更の Undo 記録 ──
    // ルーラー内部で直接編集される 3 つのリスト (markers / bpmChanges / meterChanges) を
    // before/after でまとめて捕捉し、ホスト (MainComponent) へ通知する。ドラッグは mouseDown で
    // beginMusicEdit() し mouseUp で commitMusicEdit() することで 1 アクションに集約する。
    struct EditLists
    {
        std::vector<Marker>      markers;
        std::vector<BpmChange>   bpmChanges;
        std::vector<MeterChange> meterChanges;
    };
    EditLists snapshotEditLists() const { return { markers, bpmChanges, meterChanges }; }
    void beginMusicEdit() { pendingBefore = snapshotEditLists(); hasPendingMusicEdit = true; }
    void commitMusicEdit()
    {
        if (! hasPendingMusicEdit) return;
        hasPendingMusicEdit = false;
        if (onMusicEditCommitted) onMusicEditCommitted(pendingBefore, snapshotEditLists());
    }
    // before/after の 3 リストを受け取り、ホストが Undo を記録する
    std::function<void(const EditLists&, const EditLists&)> onMusicEditCommitted;
    // 指定 1-based 小節番号 (1=曲頭) の拍子を返す
    void getMeterAtBar1(int bar1, int& outNum, int& outDen) const;
    // 1-based 小節番号 → 開始時刻（秒）
    double barStartSecs(int bar1) const;
    // 時刻（秒）→ その時刻を含む 1-based 小節番号
    int barAtTime(double secs) const;
    // ピクセル x → 1-based 小節番号（クリック判定用）
    int xToBar1(int x) const;
    // 指定時刻のBPM
    double bpmAt(double t) const;
    std::function<void(int)>    onEditMeter;  // Meter 行 Cmd+クリックで発火（クリックした小節番号を渡す）
    std::function<void(double)> onEditBpm;    // Tempo 行 Cmd+クリックで発火（スナップ後の時刻を渡す）
    std::function<double(double)> onSnapTimeForBpm;  // Tempo 行 Cmd+クリック時のスナップ
    // ドラッグでマーカー位置を更新（リストを更新して通知）
    std::function<void(const std::vector<BpmChange>&)>   onBpmChangesUpdated;
    std::function<void(const std::vector<MeterChange>&)> onMeterChangesUpdated;

    double getPixelsPerBeat() const   { return pixelsPerBeat; }

    // クリック/ドラッグで時間位置をシーク
    std::function<void(double)> onSeek;
    // タイムラインルーラーを上下ドラッグして横方向にズーム
    // 引数: 新しい pixelsPerBeat, クリック時刻 (秒), ルーラー上のクリック x 座標
    std::function<void(double /*newPpb*/, double /*centerTime*/, int /*centerXLocal*/)> onZoomDragged;
    std::function<void(double)>          onAddMarker;
    std::function<void(int)>             onEditMarkerName;
    std::function<double(double)>        onSnapTime;       // マーカードラッグ時のスナップ
    std::function<void(bool)>            onToggleMarkerColors;
    std::function<void(double)>          onSetLoopStart;
    std::function<void(double)>          onSetLoopEnd;
    std::function<void(double, double)>  onSetLoopRange;  // ドラッグで両端
    std::function<void()>                onClearLoop;
    std::function<void()>                onClearMarkers;

    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseMove(const juce::MouseEvent&) override;
    void mouseEnter(const juce::MouseEvent&) override;
    void mouseExit(const juce::MouseEvent&) override;
    void mouseDoubleClick(const juce::MouseEvent&) override;
    void modifierKeysChanged(const juce::ModifierKeys&) override;

private:
    double xToSeconds(int x) const;

    double currentBpm   { 120.0 };
    double pixelsPerBeat { 80.0 };
    double scrollX       { 0.0 };
    double playheadX     { 0.0 };
    std::vector<Marker> markers;
    double loopStart    { 0.0 };
    double loopEnd      { 0.0 };
    bool   loopActive   { false };
    bool   timeRowVisible { true };
    bool   barsRowVisible { true };

    // ループ範囲ドラッグ用
    double dragStartTime  { 0.0 };
    int    dragStartX     { 0 };
    int    dragStartY     { 0 };
    bool   isDraggingLoop { false };
    // ルーラー範囲（ループ範囲を兼用）の端ハンドル
    // (Studio One / Cubase のロケーターのように左右端を掴んでリサイズ。全体移動は廃止 =
    //  範囲内の通常クリックは上下ドラッグのズーム/シークに使うため)
    int    draggingLoopHandle { 0 };   // 0=なし 1=左端 2=右端
    // ルーラー範囲の端ヒットテスト (1=左端 2=右端 0=外)
    int    hitTestLoopHandle(int x, int y) const;
    // 上下ドラッグでズーム
    bool   isDraggingZoom { false };
    double zoomStartPpb   { 80.0 };
    double zoomCenterTime { 0.0 };
    int    zoomCenterXLocal { 0 };
    // クリックでのシークは mouseUp で確定する。マウスダウン即シークだと、上下ドラッグでの
    // ズーム (や横ドラッグでのループ範囲指定) の最中に再生バーがクリック位置へ飛んでしまい、
    // 再生中の操作で再生位置が乱れるため。ドラッグがズーム/ループに化けたら予約を取り消す。
    bool   seekArmed { false };
    // マーカードラッグ用
    int    draggingMarkerIdx { -1 };
    int    markerDragStartX  { 0 };
    bool   markerDragMoved   { false };
    bool   useMarkerColors   { true };
    int    meterNum { 4 };
    int    meterDen { 4 };
    std::vector<MeterChange> meterChanges;
    std::vector<BpmChange>   bpmChanges;

    // Undo 用: 編集開始時点の 3 リストのスナップショット
    EditLists pendingBefore;
    bool      hasPendingMusicEdit { false };

    // テンポ/拍子マーカーのドラッグ
    int  draggingBpmIdx   { -1 };
    int  draggingMeterIdx { -1 };
    int  hitTestBpmMarker(int x, int y) const;     // bpmChanges のインデックス、外れたら -1
    int  hitTestMeterMarker(int x, int y) const;   // meterChanges のインデックス、外れたら -1

    int  hitTestMarker(int x, int y) const;
    void showMarkerContextMenu(int idx);
    // 右クリック位置に応じた各種メニュー (マーカー/テンポ/拍子/共通)。macOS で mouseDown 内の
    // 同期表示が即閉じされるのを避けるため、mouseDown から callAsync で遅延呼び出しする
    void showRulerContextMenuAt(int x, int y);

    // 左クリックでのシーク + ドラッグ初期化 (Bars/Time 行・Tempo 行・Meter 行で共用)
    void beginSeekDrag(const juce::MouseEvent& e);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TimelineRuler)
};

class TimelineView : public juce::Component,
                     public juce::ScrollBar::Listener,
                     public juce::FileDragAndDropTarget,
                     private juce::Timer
{
public:
    TimelineView(TrackManager& tm);
    ~TimelineView() override;

    bool isInterestedInFileDrag(const juce::StringArray& files) override;
    void filesDropped(const juce::StringArray& files, int x, int y) override;
    void fileDragEnter(const juce::StringArray&, int, int) override;
    void fileDragExit(const juce::StringArray&) override;

    // オーディオファイル D&D 取り込み要求。複数ドロップをまとめて MainComponent 側で
    // 変換 (進捗ウィンドウ付き) → dropTime / targetTrackIdx に従い配置する。
    std::function<void(const juce::Array<juce::File>& /*sources*/,
                       double /*dropTime*/, int /*targetTrackIdx*/)> onImportAudioFiles;
    // MIDI ファイル (.mid/.midi) を指定位置に取り込むコールバック
    std::function<void(const juce::File&, double /*dropTime*/)> onImportMidi;
    // 「テンポを検出」メニューから検出された BPM をプロジェクトへ適用するコールバック
    std::function<void(double /*bpm*/)> onApplyDetectedBpm;
    // MIDI クリップをダブルクリックした時のコールバック (ピアノロール起動用)
    std::function<void(class MidiClip* /*clip*/, Track* /*track*/)> onMidiClipDoubleClicked;
    // MIDI クリップを破棄する直前に通知 (開いているピアノロールを閉じる等)
    std::function<void(class MidiClip* /*clip*/)> onMidiClipWillBeRemoved;
    // MIDI クリップのシーケンス内容がタイムライン側の編集 (クォンタイズ等) で書き換わった時の
    // 通知。MainComponent が開いているピアノロールへ reloadNotesFromClip を届ける
    std::function<void(class MidiClip* /*clip*/)> onMidiClipContentChanged;
    // MIDI 録音 (キーボード) 中の表示: Rec アーム済み MIDI トラックの行に録音開始位置 →
    // 再生バーの赤い領域を描く (描画のみ・録音状態の真実源は MainComponent)
    void setMidiRecordingIndicator(bool active, double startPos)
    {
        midiRecIndicator      = active;
        midiRecIndicatorStart = startPos;
        repaint();
    }
    // MIDI 録音の確定配置: キャプチャ列 (タイムライン秒タイムスタンプ) を小節単位のクリップに
    // して track へ置く。重なった既存クリップは置換 (音声のパンチインと同じ作法・テイク無し)。
    // 1 トラック 1 トランザクション (pushMidiReplaceAction 経由で Undo 可)
    void placeRecordedMidiClip(Track* track, const std::vector<juce::MidiMessage>& absEvents,
                               double stopPos);
    // クリップを全トラックより下の空き領域へドロップした時: 新規トラックを作って移す。
    // MainComponent が Undo (TrackAdd + ClipDelete + ClipAdd) を 1 トランザクションで配線する。
    std::function<void(const EditActions::ClipParams& /*p*/, bool /*stereo*/,
                       Lane* /*srcLane*/, AudioClip* /*srcClip*/)> onMoveClipToNewTrack;
    // 波形ファイルが差し替わった時 (キー変更等) に MainComponent 側で
    // 完了までポーリングして波形を再描画させるためのコールバック
    std::function<void()> onWaveformRefreshNeeded;

    void paint(juce::Graphics&) override;
    void resized() override;
    void scrollBarMoved(juce::ScrollBar*, double) override;
    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

    void refresh();
    void setBpm(double bpm);
    void setSampleRate(double sr) { sampleRate = sr; }
    // 録音中のライブ波形が伸びる領域だけを部分 repaint する (全面 repaint のちらつき防止)
    void repaintRecordingArea();
    // 録音中、再生バー付近の「新着波形が載る先端の狭い帯」だけを vblank ごとに部分 repaint する。
    // ライブ波形レーンは 20Hz の repaintRecordingArea でしか描き直されないため、vblank で滑らかに
    // 進む再生バーに対して波形の先端が 50ms + レイテンシ分カクついて追従する (Windows の大きめ音声
    // バッファ + Direct2D で顕著)。先端の狭い帯に絞れば全波形を毎フレーム描き直さずコストを抑えつつ
    // (draw は clip 域だけパス化する)、波形の先端を再生バーに密着させられる。
    void repaintLiveLeadingEdge();
    // 指定の時間範囲 [startSec, endSec] を覆う縦帯だけを部分 repaint する。
    // サムネイル非同期ロードの進捗通知で、変わったクリップの領域だけを描き直す用途
    // (全面 repaint だと巨大クリップ全体を fillPath で描き直して数百 ms 固まるため)。
    void repaintTimeRange(double startSec, double endSec);
    void setPlayheadPosition(double seconds);
    void setScrollY(int y);
    // 指定トラックのメイン部が見えるまで縦スクロールする (テンキーの選択移動で使用)。
    // 既に見えていれば何もしない。ヘッダ側へも onVerticalScroll で同期する。
    void scrollTrackIntoView(int trackIdx);

    double getScrollX() const { return scrollX; }
    int    getScrollY() const { return scrollY; }

    // 横ズーム量 (px / beat)。プロジェクトに保存して次回ロード時に復元するため公開。
    double getHorizontalZoom() const { return pixelsPerBeat; }
    // 横ズームの上限 (px/beat)。最大でも約 4 拍がビュー幅に収まる所までに制限する
    // (極端な拡大は描画が重くなる割に用途が無いため)。未レイアウト時はハード上限を返す。
    double maxPixelsPerBeat() const;
    void   setHorizontalZoom(double ppb)
    {
        pixelsPerBeat = juce::jlimit(1.0, maxPixelsPerBeat(), ppb);
        ruler.setPixelsPerBeat(pixelsPerBeat);
        resized();
        repaint();
    }

    // 編集操作（MainComponent から呼ぶ）
    // 構造編集 (Undo/Redo 等) の後に呼ぶ。選択中の生ポインタ (clip / crossfade) が
    // 解放済みクリップを指したまま残るのを防ぎ、Delete 時の UAF を回避する。
    void clearSelectionsAfterExternalEdit() { clearAllSelections(); repaint(); }
    bool hasSelectedClip()       const { return selectedClip.valid(); }
    bool hasSelectedCrossfade()  const { return selectedCrossfade.valid(); }
    // 現在の選択 (オーディオ/MIDI クリップ + 範囲選択のフォーカス) が属するトラック index 群。
    // トラックヘッダの選択ハイライトを timeline 選択に追従させるのに使う (昇順・重複なし)。
    std::vector<int> getInvolvedTrackIndices() const;
    bool hasSelectedMidiClip()   const { return selectedMidiClip != nullptr; }
    bool hasClipboardClip()      const { return !clipboard.empty(); }
    void deleteSelectedClips();
    void deleteSelectedMidiClip();   // 選択中の MIDI クリップを削除
    void deleteSelectedCrossfade();
    // 範囲選択内の波形を削除 (Delete/Backspace)。対象はハイライト表示と一致:
    // 単一トラック選択はフォーカスレーンのみ、複数トラックまたぎはスパン内の全トラック・
    // 全レーン、フォーカス無効なら全トラック・全レーン。範囲を跨ぐクリップは
    // トリム / 分割し、切り口には小さな既定フェードを置く。範囲選択自体は残す
    // (↑↓ でフォーカスレーンを移して別テイクも続けて削除できる)。
    void deleteSelectionRange();
    void nudgeSelectedClips(double seconds);
    void copySelectedClips();
    void cutSelectedClips();
    void pasteAtPlayhead(Track* preferredTrack = nullptr);
    void duplicateSelectedClips();
    void selectAllClips();
    void consolidateSelectedClips();
    void showStripSilenceDialog(const ClipRef& ref);
    // F = fade-in / fade-out のみ、X = crossfade のみ
    enum class FadeOpMode { FadesOnly, CrossfadeOnly };
    void applyCrossfadeToSelection(FadeOpMode mode = FadeOpMode::CrossfadeOnly);

    double getPlayheadSecs() const { return playheadSecs; }

    void setMarkers(const std::vector<Marker>& m)
    {
        ruler.setMarkers(m);
    }
    const std::vector<Marker>& getMarkers() const { return ruler.getMarkers(); }
    // ルーラー範囲（ロケーター / ループ範囲）: ルーラーの帯だけを更新する。
    // 選択範囲 (loopStartTV/...) とは独立 (それぞれ別々に機能させる要望のため)。
    void setRulerRange(double s, double e, bool active)
    {
        ruler.setLoopRange(s, e, active);   // ruler 自身が repaint する
    }
    // 選択範囲（クリップ域のドラッグ / Shift+Enter）: 選択だけを更新する。
    // ルーラー帯は触らない。
    void setSelectionRange(double s, double e, bool active)
    {
        loopStartTV = s; loopEndTV = e; loopActiveTV = active;
        syncMidiSelectionToRange();  // 範囲内の MIDI クリップも選択される (2026-07 要望)
        repaint();
        notifySelectionChanged();  // 範囲変化 → ヘッダの採用ボタン活性表示を更新
    }
    bool hasSelectionRange() const { return loopEndTV > loopStartTV + 0.001; }
    double getSelectionStart() const { return loopStartTV; }
    double getSelectionEnd()   const { return loopEndTV; }
    // P キー用: 範囲選択があればその範囲、無ければ選択クリップ群 (primary + 追加選択) の
    // 全体 [start,end] を返す。どちらも無ければ false。
    bool getRangeForRulerFromSelection(double& start, double& end) const;
    void splitAtSelection();  // B キー: 選択範囲端でクリップを切る

    // 選択範囲のフォーカスレーン（テイク比較用）
    int  getSelectionFocusTrack() const { return selectionFocusTrackIdx; }
    int  getSelectionFocusLane()  const { return selectionFocusLaneIdx;  }
    void setSelectionFocus(int trackIdx, int laneIdx);
    // 範囲選択のトラックスパン [lo, hi] (複数トラックまたぎドラッグ対応)。
    // 単一トラックなら lo == hi == フォーカストラック。フォーカス無効
    // (どのトラック上でもない場所からの選択) なら false。
    bool getSelectionTrackSpan(int& lo, int& hi) const;
    // 範囲選択が対象にしている行 (trackIdx, laneIdx) の一覧 (視覚順)。
    // アンカー行〜もう一端の行の間の全「可視」行 (途中トラックが折りたたみ中なら
    // Lane 0 のみ・展開中ならテイクレーンも視覚どおり含む)。単一行選択なら
    // フォーカス行 1 件。フォーカス無効なら空 (呼び出し側で全トラック扱い)。
    std::vector<std::pair<int, int>> getSelectionLaneRows() const;
    // 範囲選択が複数行 (トラック/テイクレーンまたぎ) か (true ならフォーカス
    // レーン単体の強調ではなく行スパンでハイライト/編集する)
    bool isSelectionMultiRow() const;
    // 範囲選択がこのトラックを対象にしているか (ハイライト表示と一致)。
    // 範囲なし = false / スパン内 = true / フォーカス無効 (どのトラック上でもない選択
    // = 全トラックが全高でハイライト) = true。テイク採用ボタンの活性などで
    // 「別トラックの範囲選択に反応しない」ために使う。
    bool selectionRangeCoversTrack(int trackIdx) const;
    // ±delta だけフォーカスレーンを上下に移動。範囲が合えば true
    bool moveSelectionFocusLane(int delta);
    // フォーカスレーンの選択範囲を Lane 0（録音レーン）にコピー
    bool copySelectionRangeToRecLane();
    // テイクレーン (laneIdx>0) の ↑ ボタン用: 指定レーンから Lane 0 へ採用する。
    // 範囲選択中はその範囲を、無ければ選択中クリップ全体を当てこむ。フォーカス状態に依存しない。
    bool promoteTakeLane(int trackIdx, int laneIdx);
    // ↑ ボタンの活性条件: 範囲選択がそのレーンのクリップと重なる、または
    // 選択中クリップがそのトラック・レーン上にある時 true。
    bool canPromoteTakeLane(int trackIdx, int laneIdx) const;
    // 現在の BPM / 拍子から 1 小節の秒数を返す (N/B の小節移動などで使う)
    double barLengthSecs() const;
    // フォーカスレーンの Solo をトグル（lane 1 以降のみ有効）
    bool toggleFocusLaneSolo();
    TimelineRuler& getRuler() { return ruler; }
    double snapTimePublic(double secs) const { return snapTime(secs); }

    // 全体表示: 末尾コンテンツ + 2 小節がビューに収まるよう pixelsPerBeat を設定
    void zoomToFitAll();

    // マーカー操作（MainComponent から呼ぶ）
    void addMarkerAtTime(double time, const juce::String& name = {});
    bool jumpToNextMarker(double currentTime, double& outTime);
    bool jumpToPrevMarker(double currentTime, double& outTime);
    void beginMarkerNameEdit(int markerIdx);

    std::function<void(int)>            onVerticalScroll;
    std::function<void(double)>         onSeek;
    std::function<void(double, double)> onSetSelectionRange; // クリップ上部ドラッグで選択範囲設定
    // 選択 (クリップ / 範囲) が変化したとき通知。ヘッダのテイク採用ボタンの活性表示更新に使う。
    std::function<void()>               onSelectionChanged;

    void setAppSettings(const AppSettings& s)
    {
        appSettings = s;
        ruler.setUseMarkerColors(s.useMarkerColors);
        ruler.setMeter(s.meterNumerator, s.meterDenominator);
        ruler.setMeterChanges(s.meterChanges);
        ruler.setBpmChanges(s.bpmChanges);
        repaint();  // GRID / 拍子 / BPM 変更を背景グリッド等へ即反映
    }
    void setUndoManager(juce::UndoManager* um)        { undoManager = um; }
    void setEditChangeCallback(std::function<void()> cb) { editChangeCb = std::move(cb); }
    // 破棄系の Undo Action (Snapshot/Split/StripSilence) が取り除いた AudioClip の所有権を受け取る
    // 遅延破棄シンク。MainComponent 側で audioEngine.deferClipDestruction() を割り当てる。即破棄せず
    // 参照中スナップショットの寿命に合わせて回収させることで、再生中編集でも他トラックを止めない。
    void setEditBeforeChangeCallback(std::function<void(std::vector<std::unique_ptr<AudioClip>>&&)> cb)
    { editBeforeChangeCb = std::move(cb); }

private:
    // ルーラー高さ: ruler の表示状態から動的に決まる
    int rulerHeight() const { return ruler.getDesiredHeight(); }
    static constexpr int scrollBarSize { 12 };
    // 右側 / 下側: vScrollBar 列 + 縦/横ズームボタンが入る固定幅 (resized() と一致させる)
    static constexpr int scrollLaneSize { 18 };

    // 「意図的なクロスフェード」と判定するフェード長の下限。クロスフェードのフェードは
    // 約 30ms で作られるが、overlap = end - start を大きな位置 (例: 9.3s) から引くと float の
    // 桁落ちで 0.0299999... と 30ms をわずかに下回ることがある。そこで判定はデフォルトの
    // クリック防止フェード (5〜10ms) と確実に区別できる 20ms を閾値にする (描画/カーソル共通)。
    static constexpr double kCrossfadeFadeMinSecs { 0.020 };
    // 差し込み (ドラッグ別トラック / ペースト) で接合部に作る最小クロスフェード長。
    // 幾何計算は ClipInsertGeometry::planInsertOverlap、末尾分割は makeSplitTail に一本化。
    static constexpr double kMinCrossfadeSecs    { 0.030 };

    // ヒット判定の許容値 (カーソル判定 mouseMove / 掴み判定 mouseDown / 描画で共通)
    static constexpr int    kXfadeHandleHitPx    { 8 };      // クロスフェード両端ハンドル半径
    static constexpr int    kHandleHitPx         { 6 };      // フェードチップ / GainPoint 半径
    static constexpr double kOverlapEpsilonSecs  { 0.001 };  // 「重なりあり」とみなす下限 (1ms)

    // クロスフェードを「視覚化・操作対象」として扱うかの単一の真実源 (#L3)。
    // autoCrossfade ON、または ≥kCrossfadeFadeMinSecs のフェードを持つ実クロスフェードのみ。
    // カーソル判定 (mouseMove) / ヒット判定 (mouseDown) / Lane0 オーバーレイ描画で共用する。
    // 条件を変える時はここだけ直す (旧実装は 3 箇所に同じ式が複製されズレるバグの温床だった)。
    bool isCrossfadeInteractive(const AudioClip* a, const AudioClip* b) const
    {
        return appSettings.autoCrossfade
            || a->getFadeOutSecs() >= kCrossfadeFadeMinSecs
            || b->getFadeInSecs()  >= kCrossfadeFadeMinSecs;
    }

    void timerCallback() override;
    void drawTrackRows(juce::Graphics& g, juce::Rectangle<int> area);
    void drawClip(juce::Graphics& g, AudioClip& clip,
                  juce::Rectangle<int> laneBounds, juce::Colour trackColour,
                  bool isSelected = false, Lane* ownerLane = nullptr);
    // 高ズーム時のサンプル単位波形描画
    void drawClipSamples(juce::Graphics& g, AudioClip& clip,
                          juce::Rectangle<int> wfRect, juce::Colour wfColour,
                          float verticalZoom);
    // ズームアウト時の波形シルエットを area へ AA 描画する。サムネイルの min/max を
    // フロート座標の Path 塗りにすることで、整数矩形の櫛状エッジを滑らかにする
    void drawClipWaveform(juce::Graphics& g, AudioClip& clip, juce::Rectangle<int> area,
                          double fileStart, double durationSecs, float verticalZoom,
                          juce::Colour colour);
    // 重なり領域に X 字のクロスフェードカーブを上書き描画
    void drawCrossfadeOverlay(juce::Graphics& g, struct Lane* lane,
                               juce::Rectangle<int> laneBounds);
    // MIDI クリップ描画（ノートピアノロール風プレビュー）
    void drawMidiClip(juce::Graphics& g, class MidiClip& clip,
                      juce::Rectangle<int> laneBounds, juce::Colour trackColour);

    // マウス操作
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseMove(const juce::MouseEvent&) override;
    void mouseExit(const juce::MouseEvent&) override;
    void mouseDoubleClick(const juce::MouseEvent&) override;

    // ヒットテスト
    ClipRef getClipAt(int x, int y) const;
    // MIDI クリップヒット: ヘッダ領域 (isHeader) / 左右端 (leftEdge/rightEdge) を判定
    struct MidiClipHit { MidiClip* clip { nullptr }; Track* track { nullptr }; int trackIdx { -1 };
                         bool isHeader { false }; bool leftEdge { false }; bool rightEdge { false }; };
    MidiClipHit getMidiClipAt(int x, int y) const;
    // クリップの有無に関わらず、y にある MIDI トラックを返す (空エリアでのクリップ作成用)
    Track* midiTrackAtY(int y, int& outTrackIdx) const;
    // 現在の GRID 設定の 1 単位の秒数 (GRID:Off のときは 1 拍を返す = 最小サイズ用)
    double gridUnitSecs() const;
    // MIDI クリップを absSplitTime (小節頭にスナップ) で 2 つに分割する
    void   splitMidiClip(Track* track, MidiClip* clip, double absSplitTime);
    // MIDI クリップの全ノートを GRID にクォンタイズする (開始位置のみ・長さ維持。
    // クリップのインスタンスは保つ = 開いているピアノロールは reload で追従)
    void   quantizeMidiClip(Track* track, MidiClip* clip);
    // MIDI 録音インジケータ (setMidiRecordingIndicator)
    bool   midiRecIndicator { false };
    double midiRecIndicatorStart { 0.0 };
    // 構造編集 (分割/削除/Undo) 後、selectedMidiClip が実在しなくなっていたら選択を解除する
    // (作り直しや破棄で生ポインタがダングリングするのを防ぐ)
    void   clearMidiSelectionIfStale();
    // 範囲選択の対象行にある MIDI クリップを選択へ同期する (範囲ドラッグで MIDI も選ばれる)
    void   syncMidiSelectionToRange();
    // MIDI クリップの差し替え (分割/削除/作成) を Undo 対応で実行する。
    // toRemove を取り除き toAdd を生成。undo/redo で同一インスタンス復元 + ピアノロール整合。
    // newTransaction=false で直前のトランザクションへ合流 (複数トラックの一括削除を 1 Undo に)
    void   pushMidiReplaceAction(Track* track,
                                 std::vector<MidiClip*> toRemove,
                                 std::vector<EditActions::MidiClipReplaceAction::NewMidiClip> toAdd,
                                 bool newTransaction = true);
    enum class DragMode { None, Move, FadeIn, FadeOut, ResizeLeft, ResizeRight, CrossfadeLeft, CrossfadeRight, CrossfadeCenter, Gain, GainPoint, Selection };
    DragMode getDragMode(const ClipRef& ref, int mouseX, int mouseY) const;
    juce::Rectangle<int> getContentArea() const;

    double positionToX(double seconds) const;
    double xToPosition(int x) const;
    double snapTime(double secs) const;  // グリッドスナップ

    TrackManager& trackManager;

    TimelineRuler ruler;
    juce::ScrollBar hScrollBar { false };
    juce::ScrollBar vScrollBar { true };
    // 横ズーム / 縦ズーム (波形振幅) のスライダ
    // 横/縦ズーム用の +/- ボタン (Cmd+スクロールと同等の挙動・再生バー起点)
    juce::TextButton hZoomOutBtn { tr(u8"−") };
    juce::TextButton hZoomInBtn  { "+" };
    juce::TextButton hZoomResetBtn { tr(u8"↺") };  // 横ズームを全体表示にリセット (Shift+F と同じ)
    juce::TextButton vZoomOutBtn { tr(u8"−") };
    juce::TextButton vZoomInBtn  { "+" };
    juce::TextButton vZoomResetBtn { tr(u8"↺") };  // 波形振幅を既定 (1.0) に戻す
    void applyHorizontalZoomStep(double deltaY);  // deltaY > 0 で拡大
    void applyVerticalZoomStep(double deltaY);    // deltaY > 0 で拡大 (波形振幅)
    void resetVerticalZoom();                     // 波形振幅を既定値へリセット
    void scrollByTracks(int steps);               // steps>0 = 下(後ろ) / <0 = 上(前)へ N トラック

    // コンテンツ域相対 y (スクロール込み) から (トラック, 可視レーン) 行を推定する。
    // トラック域の外は端の行へクランプ (上 = 先頭トラック Lane 0 / 下 = 末尾トラックの
    // 最終可視レーン)。トラックが 1 本も無ければ false。範囲選択の縦ドラッグで使う。
    bool laneRowAtY(int relY, int& trackIdx, int& laneIdx) const;

    // 差し込みで内包クリップを分割した「右側末尾」を作る共通ヘルパ (ドラッグ別トラック /
    // ペースト / 同一レーンドラッグで共用)。undoManager があれば ClipAddAction、無ければ
    // 直接 addClip。色 (トラック色追従の維持) / フェードカーブ / ゲインエンベロープの右シフトを
    // 一括適用する。srcGainPoints は元クリップの全ポイント、splitLocalSecs は右側の開始 (元
    // クリップローカル秒)、dbAtSplit はそこのエンベロープ値。
    AudioClip* makeSplitTail(Lane* lane, const EditActions::ClipParams& params,
                             FadeCurve srcFadeOutCurve, bool srcHasCustomColour,
                             const std::vector<GainPoint>& srcGainPoints,
                             double splitLocalSecs, float dbAtSplit,
                             juce::AudioFormatManager& fmt, juce::AudioThumbnailCache& cache);

    double pixelsPerBeat  { 80.0 };
    double scrollX        { 0.0 };
    int    scrollY        { 0 };
    // 縦スクロールのトラックスナップ用: ホイール delta を累積し、しきい値ごとに 1 トラック送る。
    // ゆっくり=1 トラックずつ (スナップ維持)、速いフリック=スクロール量に比例して複数トラック。
    // アイドル (前回ホイールから一定時間経過) で端数をリセットし、ゆっくり時は厳密に 1:1 にする。
    double       wheelAccumY  { 0.0 };
    juce::uint32 lastWheelMs  { 0 };
    // 横ズーム中フラグ: 連続ズーム中は波形キャッシュを再生成せず既存を拡縮 blit するだけにし、
    // ズームが止まって (アイドルタイマー) から 1 度だけ綺麗に再描画する (もたつき防止)。
    bool         zoomActive   { false };
    // 横スクロール中フラグ: 巨大クリップ (Path-2) の帯再生成は重い fillPath なので、スクロール中に
    // 走ると一瞬カクつく。スクロール中は速度に依らず再生成を遅延し (既存帯を blit して滑らかに流す)、
    // 止まってからタイマーで 1 度だけ綺麗に再描画する (詳細は noteHorizontalScroll)。
    bool         deferBigClipRegen { false };
    void noteHorizontalScroll();  // スクロール中の Path-2 帯再生成を遅延 (TimelineView.cpp)
    double playheadSecs   { 0.0 };
    double bpm            { 120.0 };
    double sampleRate     { 44100.0 };
    double waveformZoom   { 1.0 };
    double loopStartTV    { 0.0 };
    double loopEndTV      { 0.0 };
    bool   loopActiveTV   { false };

    // 描画用の使い回しスクラッチ (毎描画のヒープ確保を回避)
    std::vector<juce::Point<float>> samplePtsScratch;  // サンプル単位描画の点列 (スプライン用)
    std::vector<float>              waveTopScratch;     // 波形シルエットの上辺 (max)
    std::vector<float>              waveBotScratch;     // 波形シルエットの下辺 (min)
    std::vector<float>              gridBarXScratch;    // 背景グリッドの小節線 X (毎 paint 確保を避け再利用)
    std::vector<float>              gridLineXScratch;   // 背景グリッドの GRID 単位線 X

    // 編集状態
    ClipRef    selectedClip;
    std::vector<ClipRef> extraSelections;  // 追加選択（プライマリ以外）
    // マウスが乗っているクリップ（フェードハンドルをホバー時のみ強調表示するため）
    AudioClip* hoveredClip { nullptr };
    // カーソルが当たっているフェードハンドル (FadeIn / FadeOut / None)。
    // その三角だけを強くハイライトして「掴める」ことを示す。
    DragMode   hoveredHandle { DragMode::None };
    // MIDI クリップ選択 (AudioClip とは別系統)。Shift/Cmd+クリックで複数選択できる
    // (extraMidiSelections = プライマリ以外。トラックをまたいでもよい)
    MidiClip*  selectedMidiClip  { nullptr };
    Track*     selectedMidiTrack { nullptr };
    std::vector<std::pair<MidiClip*, Track*>> extraMidiSelections;
    bool isMidiClipSelected(const MidiClip* c) const
    {
        if (c == nullptr) return false;
        if (c == selectedMidiClip) return true;
        for (const auto& p : extraMidiSelections)
            if (p.first == c) return true;
        return false;
    }
    int selectedMidiClipCount() const
    {
        return (selectedMidiClip != nullptr ? 1 : 0) + (int) extraMidiSelections.size();
    }
    // 選択中の MIDI クリップのうち track 上のものを結合して 1 クリップにする (右クリックメニュー)
    void mergeSelectedMidiClips(Track* track);
    DragMode   dragMode        { DragMode::None };
    double     dragStartSecs   { 0.0 };
    double     clipOrigStart   { 0.0 };
    double     clipOrigEnd     { 0.0 };
    double     clipBOrigEnd    { 0.0 };
    double     origFadeIn      { 0.0 };
    double     origFadeOut     { 0.0 };
    double     origFileOffset  { 0.0 };
    float      origClipGain    { 1.0f };
    // 複数選択ドラッグ用
    std::vector<double> extraOrigStarts;
    // Move ドラッグ中のホバー先トラック/レーン（ゴースト表示用）
    int dragHoverTrackIdx { -1 };
    int dragHoverLaneIdx  { -1 };
    // ラバーバンド範囲選択
    bool       rubberBandActive { false };
    juce::Point<int> rubberBandStart;
    juce::Point<int> rubberBandEnd;
    bool isClipInSelection(const AudioClip* clip) const;
    bool clipStillExists(AudioClip* clip) const;  // 全レーンを走査して生存確認 (UAF 防止)
    // ClipsPropertyAction 用の生存チェック (undo/redo 時に破棄済みクリップを restore しない)。
    // ClipsPropertyAction を生成する全経路で必ず渡すこと (v0.5.6 redo UAF クラッシュの再発防止)
    EditActions::ClipValidator clipAliveValidator()
    {
        return [this](AudioClip* c) { return clipStillExists(c); };
    }
    // 波形クリップの右クリックメニュー (構築 + 結果ハンドラ。mouseDown から抽出)
    void showAudioClipContextMenu(const ClipRef& rcRef, const juce::MouseEvent& e);
    bool midiClipStillExists(MidiClip* clip, Track* owner) const;  // track と clip の両方を生存確認
    void clearAllSelections();
    // 選択変化を owner へ通知 (callback only。各サイトの repaint() は据え置き)
    void notifySelectionChanged() { if (onSelectionChanged) onSelectionChanged(); }
    // copySelectionRangeToRecLane / promoteTakeLane の共通実体。
    // track / srcLane と挿入範囲 [t1, t2] を受け取り Lane 0 へ採用する。
    bool promoteRangeToLane0(Track* track, Lane* srcLane, double t1, double t2);
    void addToSelection(const ClipRef& ref);
    AudioClip*   crossfadeNeighbor { nullptr };
    CrossfadeRef selectedCrossfade;
    int          draggedGainPointIdx { -1 };  // GainPoint ドラッグ中のインデックス

    // クリップ名編集
    std::unique_ptr<juce::TextEditor> nameEditor;
    AudioClip*                        editingNameClip { nullptr };
    void beginNameEditing(const ClipRef& ref);
    void finishNameEditing(bool commit);

    // マーカー名編集（インライン TextEditor）
    std::unique_ptr<juce::TextEditor> markerNameEditor;
    int                               editingMarkerIdx { -1 };
    void finishMarkerNameEdit(bool commit);

    // Undo/Redo
    juce::UndoManager*                  undoManager { nullptr };
    std::function<void()>               editChangeCb;
    std::function<void(std::vector<std::unique_ptr<AudioClip>>&&)> editBeforeChangeCb;  // 破棄系: 取り除いたクリップの遅延破棄
    std::vector<EditActions::ClipState> preDragStates;

    // クリップボード（コピー＆ペースト用）
    struct ClipboardEntry
    {
        EditActions::ClipParams params;
        Track* sourceTrack { nullptr };
        Lane*  sourceLane  { nullptr };
    };
    std::vector<ClipboardEntry> clipboard;
    double clipboardSelectionStart { -1.0 };  // 範囲コピー時のアンカー（負値=範囲コピーではない）

    // カット操作プレビュー
    bool showCutLine { false };
    int  cutLineX    { 0 };

    // D&D ハイライト
    bool fileDragActive { false };
    int  fileDragHoverTrack { -1 };

    // 選択範囲のフォーカスレーン（テイク比較）
    int selectionFocusTrackIdx { -1 };
    int selectionFocusLaneIdx  { -1 };
    // 範囲選択の縦スパンの「もう一端」の行 (トラック, 可視レーン)。複数トラック /
    // 複数テイクレーンまたぎドラッグ用。EndIdx が -1 なら単一行選択 (従来動作・
    // フォーカスレーンのみ)。選択される行は視覚順でアンカー行〜この行の全可視行
    // (Lane 0 で止めればテイクレーンは含まれない = WYSIWYG)。実際の行集合は
    // getSelectionLaneRows() が解決し、ハイライト/削除/ヘッダ連動が追従する
    int selectionFocusTrackEndIdx { -1 };
    int selectionFocusLaneEndIdx  { -1 };

    AppSettings appSettings;

    // MIDI クリップ移動ドラッグ
    MidiClip* draggingMidiClip { nullptr };
    double    midiDragOrigStart { 0.0 };
    int       midiDragStartX    { 0 };

    // MIDI クリップ 左右リサイズ
    MidiClip* resizingMidiClip   { nullptr };
    bool      midiResizeLeft     { false };
    double    midiResizeOrigStart{ 0.0 };
    double    midiResizeOrigEnd  { 0.0 };

    // MIDI クリップ 新規作成 (Option+ドラッグ)。anchor は小節スナップ済みの開始位置。
    MidiClip* creatingMidiClip   { nullptr };
    double    midiCreateAnchor   { 0.0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TimelineView)
};
