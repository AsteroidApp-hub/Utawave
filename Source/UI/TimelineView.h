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
    int  getDesiredHeight() const
    {
        const int hMarker = 16;
        const int hTempo  = 14;
        const int hMeter  = 14;
        const int hBars   = barsRowVisible ? 20 : 0;
        const int hTime   = timeRowVisible ? 16 : 0;
        return hMarker + hTempo + hMeter + hBars + hTime;
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
    // 指定の時間範囲 [startSec, endSec] を覆う縦帯だけを部分 repaint する。
    // サムネイル非同期ロードの進捗通知で、変わったクリップの領域だけを描き直す用途
    // (全面 repaint だと巨大クリップ全体を fillPath で描き直して数百 ms 固まるため)。
    void repaintTimeRange(double startSec, double endSec);
    void setPlayheadPosition(double seconds);
    void setScrollY(int y);

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
    void setLoopRange(double s, double e, bool active)
    {
        loopStartTV = s; loopEndTV = e; loopActiveTV = active;
        ruler.setLoopRange(s, e, active);
        repaint();
        notifySelectionChanged();  // 範囲変化 → ヘッダの採用ボタン活性表示を更新
    }
    bool hasSelectionRange() const { return loopEndTV > loopStartTV + 0.001; }
    double getSelectionStart() const { return loopStartTV; }
    double getSelectionEnd()   const { return loopEndTV; }
    void splitAtSelection();  // B キー: 選択範囲端でクリップを切る

    // 選択範囲のフォーカスレーン（テイク比較用）
    int  getSelectionFocusTrack() const { return selectionFocusTrackIdx; }
    int  getSelectionFocusLane()  const { return selectionFocusLaneIdx;  }
    void setSelectionFocus(int trackIdx, int laneIdx);
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
    // 構造編集 (分割/削除/Undo) 後、selectedMidiClip が実在しなくなっていたら選択を解除する
    // (作り直しや破棄で生ポインタがダングリングするのを防ぐ)
    void   clearMidiSelectionIfStale();
    // MIDI クリップの差し替え (分割/削除/作成) を Undo 対応で実行する。
    // toRemove を取り除き toAdd を生成。undo/redo で同一インスタンス復元 + ピアノロール整合。
    void   pushMidiReplaceAction(Track* track,
                                 std::vector<MidiClip*> toRemove,
                                 std::vector<EditActions::MidiClipReplaceAction::NewMidiClip> toAdd);
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
    juce::TextButton hZoomResetBtn { u8"↺" };  // 横ズームを全体表示にリセット (Shift+F と同じ)
    juce::TextButton vZoomOutBtn { tr(u8"−") };
    juce::TextButton vZoomInBtn  { "+" };
    juce::TextButton vZoomResetBtn { u8"↺" };  // 波形振幅を既定 (1.0) に戻す
    void applyHorizontalZoomStep(double deltaY);  // deltaY > 0 で拡大
    void applyVerticalZoomStep(double deltaY);    // deltaY > 0 で拡大 (波形振幅)
    void resetVerticalZoom();                     // 波形振幅を既定値へリセット
    void scrollByTracks(int steps);               // steps>0 = 下(後ろ) / <0 = 上(前)へ N トラック

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
    // 高速横スクロール中フラグ: 巨大クリップ (Path-2) の帯再生成は重い fillPath なので、速い
    // スクロールでマージン帯を横切るたびに走るとカクつく。速度がしきい値を超える間だけ再生成を
    // 遅延し (既存帯を blit して滑らかに流す)、止まってからタイマーで 1 度だけ綺麗に再描画する。
    // 遅いスクロールでは false のまま = 従来どおり鮮明に追従描画する (空白を出さない)。
    bool         deferBigClipRegen { false };
    double       scrollVelPxPerSec { 0.0 };  // 平滑化した横スクロール速度 (px/秒)
    juce::uint32 lastHScrollMs     { 0 };
    void noteHorizontalScrollVelocity(double pxMoved);  // 速度更新 + 高速判定 (TimelineView.cpp)
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

    // 編集状態
    ClipRef    selectedClip;
    std::vector<ClipRef> extraSelections;  // 追加選択（プライマリ以外）
    // マウスが乗っているクリップ（フェードハンドルをホバー時のみ強調表示するため）
    AudioClip* hoveredClip { nullptr };
    // カーソルが当たっているフェードハンドル (FadeIn / FadeOut / None)。
    // その三角だけを強くハイライトして「掴める」ことを示す。
    DragMode   hoveredHandle { DragMode::None };
    // MIDI クリップ選択 (AudioClip とは別系統)
    MidiClip*  selectedMidiClip  { nullptr };
    Track*     selectedMidiTrack { nullptr };
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
