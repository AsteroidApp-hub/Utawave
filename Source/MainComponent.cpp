// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

#include "MainComponent.h"
#include "Localisation.h"
#include "AppColours.h"
#include "Project/RecentProjects.h"
#include "Audio/AudioDeviceSettings.h"
#include "Audio/LufsMeter.h"
#include "Export/ExportEngine.h"
#include "Export/ExportDialog.h"
#include "VST/PluginChain.h"
#include "UI/PluginManagerDialog.h"
#include "UI/PianoRollEditor.h"
#include "MIDI/MidiImporter.h"
#include "MIDI/MidiImportDialog.h"
#include <algorithm>

void MainComponent::applyTooltipVisibility()
{
    // ツールチップ用 LnF は TooltipWindow より長生きする必要があるため関数ローカル static。
    static UtawaveLookAndFeel tooltipLnF;
    if (appPrefs.showTooltips)
    {
        if (tooltipWindow == nullptr)
        {
            tooltipWindow = std::make_unique<juce::TooltipWindow>(this);
            tooltipWindow->setLookAndFeel(&tooltipLnF);
        }
    }
    else
    {
        tooltipWindow.reset();   // 破棄するとアプリ全体でツールチップが出なくなる
    }
}

MainComponent::MainComponent()
{
    setSize(1280, 800);
    setWantsKeyboardFocus(true);
    addKeyListener(this);

    // ツールチップ表示の生成 (appPrefs.showTooltips が ON のときのみ)。
    // 描画は UtawaveLookAndFeel（角丸 + アクセント枠 + 余白）を applyTooltipVisibility で適用する。
    applyTooltipVisibility();


    // 未保存プロジェクト用の仮フォルダを作成（録音/インポートはここに溜まる）
    {
        auto root = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
                        .getChildFile("Utawave");
        auto ts = juce::Time::getCurrentTime().formatted("%Y%m%d_%H%M%S");
        untitledProjectDir = root.getChildFile("Untitled-" + ts);
        untitledProjectDir.getChildFile("Audio").createDirectory();
        untitledProjectDir.getChildFile("Cache").createDirectory();
    }
    // 録音・インポート系をプロジェクトフォルダに連携
    recordingMgr.getAudioFolder = [this] { return getProjectAudioFolder(); };
    recordingMgr.setBitDepth(appSettings.projectBitDepth);

    // ステータスバーの CPU 表示: AudioDeviceManager の負荷を 0〜100% で返す
    statusBar.getCpuPercent = [this] {
        return audioEngine.getDeviceManager().getCpuUsage() * 100.0;
    };
    statusBar.onHelpClicked = [this] { showShortcutsDialog(); };
    statusBar.onLyricsClicked = [this] { toggleLyricsWindow(); };
    fileImporter.getCacheFolderCb = [this] { return getProjectCacheFolder(); };

    addAndMakeVisible(toolbar);
    addAndMakeVisible(trackHeaderPanel);
    addAndMakeVisible(timelineView);
    addAndMakeVisible(masterPanel);
    addAndMakeVisible(statusBar);

    // Transport
    toolbar.onPlay          = [this] { togglePlay(); };
    toolbar.onStop          = [this] { stopTransport(); };
    toolbar.onRecord        = [this] { toggleRecord(); };
    toolbar.onRewind        = [this] { stopTransport(); audioEngine.rewind();
                                       playPosition = 0.0;
                                       toolbar.setTimePosition(0.0, 1, 1);
                                       timelineView.setPlayheadPosition(0.0);
                                       propagatePlayheadToPianoRolls(0.0); };
    toolbar.onAudioSettings = [this] { showAudioSettings(); };
    toolbar.onPreferences   = [this] { showPreferences(); };
    toolbar.onImport        = [this] { showImportAnyDialog(); };
    toolbar.onExport        = [this] { showExportDialog(); };

    toolbar.onClipGainChanged = [this](bool v)
    {
        appSettings.showClipGain = v;
        timelineView.setAppSettings(appSettings);
        timelineView.repaint();
    };
    toolbar.onSnapModeSelected = [this](int mode)
    {
        appSettings.snapMode = (SnapMode)mode;
        timelineView.setAppSettings(appSettings);
        syncSnapLabelToSettings();
    };

    // Track management
    auto refreshTracks = [this]
    {
        trackHeaderPanel.refresh();
        timelineView.refresh();
        statusBar.setTrackCount(trackManager.getTrackCount());
    };

    trackManager.onChanged = refreshTracks;

    trackHeaderPanel.onAddTrack            = [this] { addTrack(); markProjectDirty(); };
    trackHeaderPanel.onAddTrackWithMode    = [this](bool stereo, bool appendAtBottom) {
        const int insertAfter = appendAtBottom ? -1 : newTrackInsertAfter();
        pushTrackAddUndo(trackManager.addTrack({}, stereo, insertAfter));
        markProjectDirty();
    };
    trackHeaderPanel.onAddMidiTrack        = [this] { addMidiTrack(); };
    trackHeaderPanel.onAddClickTrack       = [this] {
        if (auto* t = trackManager.addClickTrack())
        {
            // 既存のメトロノーム音色/アクセントは引き継ぎ、音量は -7 dB を既定にする
            // (生成直後から大きすぎないように。フェーダー (dB) で以後調整できる)。
            t->setClickSound(audioEngine.getMetronomeSound());
            t->setClickAccent(audioEngine.getMetronomeAccent());
            t->setVolume(-7.0f);
            t->setPan(audioEngine.getMetronomePan());
            pushTrackAddUndo(t);
        }
        // CLICK トラックを追加したら -7dB をエンジンへ反映し、追加直後から鳴る状態にする
        // (CLICK ボタンの点灯も syncClickTrackToEngine 内で同期される)。
        syncClickTrackToEngine();
        audioEngine.preparePlayback(trackManager);
        markProjectDirty();
    };
    trackHeaderPanel.onTrackSelected = [this](int idx)
    {
        // 主選択 index のみ更新する (インポート先トラック等で使う)。
        // 複数選択のハイライトは selectTrackForUI が既に更新しているので、ここで
        // setSelectedTrack() を呼ぶと Cmd/Shift で広げた選択が単一に潰れてしまう (要注意)。
        selectedTrackIndex = idx;
    };
    trackHeaderPanel.onGetNumInputChannels = [this]() -> int {
        return audioEngine.getNumInputChannels();
    };
    trackHeaderPanel.onGetTrackOutPeakL = [this](int idx) { return audioEngine.getTrackOutputPeakL(idx); };
    trackHeaderPanel.onGetTrackOutPeakR = [this](int idx) { return audioEngine.getTrackOutputPeakR(idx); };
    trackHeaderPanel.onGetTrackOutVUL   = [this](int idx) { return audioEngine.getTrackOutputVUL(idx); };
    trackHeaderPanel.onGetTrackOutVUR   = [this](int idx) { return audioEngine.getTrackOutputVUR(idx); };
    trackHeaderPanel.onPluginAddRequest    = [this](int trackIdx, int slot) { addPluginToTrack(trackIdx, slot); };
    trackHeaderPanel.onPluginEditRequest   = [this](int trackIdx, int slot) { openPluginEditor(trackIdx, slot); };
    trackHeaderPanel.onPluginRemoveRequest = [this](int trackIdx, int slot)
    {
        removePluginFromTrack(trackIdx, slot);
    };
    trackHeaderPanel.onPluginSwapRequest   = [this](int trackIdx, int a, int b)
    {
        swapTrackPluginsUndoable(trackIdx, a, b);
    };
    trackHeaderPanel.onPluginBypassRequest = [this](int trackIdx, int slot)
    {
        togglePluginBypassUndoable(trackIdx, slot);
    };
    trackHeaderPanel.onPluginDropAcrossTracks = [this](int srcTrack, int srcSlot,
                                                      int dstTrack, int dstSlot, bool copy)
    {
        handlePluginDropAcrossTracks(srcTrack, srcSlot, dstTrack, dstSlot, copy);
    };
    trackHeaderPanel.onTracksDeleteRequest = [this](const std::vector<int>& indices)
    {
        deleteTracks(indices);
    };
    trackHeaderPanel.onTrackDuplicateRequest = [this](int trackIdx, bool includeTakeLanes)
    {
        duplicateTrack(trackIdx, includeTakeLanes);
    };
    // トラック並べ替えの Undo (並べ替え自体は performReorder が実施済み)
    trackHeaderPanel.onTracksReordered = [this](std::vector<Track*> before, std::vector<Track*> after,
                                                std::vector<Track*> moved)
    {
        pushTrackReorderUndo(std::move(before), std::move(after), std::move(moved));
    };
    // テイクレーン ↑ ボタン: 範囲選択 or クリップ選択中のテイクを Lane 0 へ採用
    trackHeaderPanel.onLanePromoteRequest = [this](int trackIdx, int laneIdx)
    {
        if (timelineView.promoteTakeLane(trackIdx, laneIdx))
        {
            trackHeaderPanel.refresh();
            audioEngine.preparePlayback(trackManager);  // Shift+↑ と同じ後処理
            markProjectDirty();
        }
    };
    trackHeaderPanel.onCanPromoteLane = [this](int trackIdx, int laneIdx)
    {
        return timelineView.canPromoteTakeLane(trackIdx, laneIdx);
    };
    // トラックのプロパティ編集 (名前/色/シンセ設定) を Undo 対応で適用
    trackHeaderPanel.onTrackEditUndoable = [this](Track* t, std::function<void()> m)
    {
        applyTrackEditUndoable(t, std::move(m));
    };

    // Shift+Option+クリックで S / M を選択中トラックへ一括適用 (1 Undo にまとめる)。
    // クリックされたトラック自身も含め、選択集合の全トラックを同じ値 (value) にそろえる。
    trackHeaderPanel.onTrackSoloBatch = [this](int clickedIdx, bool value)
    { applyToggleToSelectedTracks(clickedIdx, value, /*solo=*/true); };
    trackHeaderPanel.onTrackMuteBatch = [this](int clickedIdx, bool value)
    { applyToggleToSelectedTracks(clickedIdx, value, /*solo=*/false); };

    trackHeaderPanel.onTrackChanged = [this]
    {
        markProjectDirty();
        // Input monitoring / Rec arm / モニターリバーブ量をエンジンへ反映
        // (Rev スライダー変更も onChanged 経由でここを通るのでモニターリバーブが即追従する)
        syncInputMonitorStateToEngine();

        // INS スロット表示の切替に追従してレイアウトを更新
        resized();

        // Click track の Vol/Pan/Mute/Sound/Accent をメトロノームへ同期 (無ければ停止)
        syncClickTrackToEngine();

        trackHeaderPanel.resized();
        timelineView.refresh();
        timelineView.repaint();
    };

    // 縦スクロール同期
    timelineView.onVerticalScroll = [this](int y)
    {
        trackHeaderPanel.setScrollY(y);
    };

    // ルーラークリック → プレイヘッドシーク (seekTo が遡及キャプチャの取り直しも行う)
    timelineView.onSeek = [this](double seconds)
    {
        seekTo(seconds);
    };

    // Master fader
    masterPanel.onMasterGainChanged = [this](double gain)
    {
        audioEngine.setMasterGain((float)gain);
    };

    // マスターチェーンを MasterPanel に渡す
    masterPanel.onResetPeakHold = [this] { audioEngine.resetPeakHold(); };
    masterPanel.setPluginChain(&audioEngine.getMasterChain());
    masterPanel.onPluginAddRequest    = [this](int slot) { addPluginToMaster(slot); };
    masterPanel.onPluginEditRequest   = [this](int slot) { openMasterPluginEditor(slot); };
    masterPanel.onPluginRemoveRequest = [this](int slot) { removeMasterPlugin(slot); };
    masterPanel.onPluginSwapRequest   = [this](int a, int b)
    {
        swapMasterPluginsUndoable(a, b);
    };
    masterPanel.onPluginBypassRequest = [this](int slot)
    {
        togglePluginBypassUndoable(-1, slot);   // -1 = マスター
    };
    masterPanel.onPluginDropFromOtherTrack = [this](int srcTrack, int srcSlot, int dstSlot, bool copy)
    {
        handlePluginDropFromTrackToMaster(srcTrack, srcSlot, dstSlot, copy);
    };

    // マスター INS スロット表示設定の永続化
    masterPanel.setInsertSlotsVisible(appSettings.masterInsertSlotsVisible);
    masterPanel.onInsertSlotsVisibilityChanged = [this](bool v)
    {
        appSettings.masterInsertSlotsVisible = v;
    };

    // マスターパネルの折りたたみ
    // ※ コールバックを先に張ってから setCollapsed を呼ぶ。
    //    こうしないと、初回の false→true 遷移で MainComponent::resized() が
    //    呼ばれず、パネル幅が 155 のまま collapsed 描画になる。
    masterPanel.onCollapseToggled = [this](bool v)
    {
        appSettings.masterPanelCollapsed = v;
        resized();
    };
    masterPanel.setCollapsed(appSettings.masterPanelCollapsed);

    // VU メータ基準レベル
    masterPanel.setVuReferenceLevel(appSettings.vuReferenceLevel);
    trackHeaderPanel.setVuReferenceLevel(appSettings.vuReferenceLevel);
    // ラウドネス自動調整ターゲット
    trackHeaderPanel.setLoudnessTargetLufs(appSettings.loudnessTargetLufs);

    // Undo/Redo
    // スナップショット系 Action は getSizeInUnits を override せず既定 10 を返すため、
    // 既定上限 (30000 ユニット ≒ 3000 トランザクション) では延命所有が溜まりやすい。
    // 初心者向け DAW には 200 トランザクションで十分なので明示的に上限を絞る。
    undoManager.setMaxNumberOfStoredUnits (30000, 200);
    timelineView.setUndoManager(&undoManager);
    timelineView.setEditChangeCallback([this]
    {
        markProjectDirty();
        trackHeaderPanel.refresh();
        timelineView.refresh();  // 内部で repaint() するため二重 repaint は不要
        // MIDI クリップ移動/リサイズで小節表示がずれないよう、開いている
        // ピアノロールも再描画する (小節番号は clip.getStartPosition() を毎回読む)。
        for (auto* w : pianoRollWindows)
            if (w && w->getEditor()) w->getEditor()->repaint();
        // 編集で AudioClip/Track が破棄/再生成されうるため、再生中は即 rebuild、
        // 停止中は次の play() で rebuild するよう dirty を立てる (音切れ防止)。
        audioEngine.invalidatePlayback();
    });
    // 破棄系 Undo Action が取り除いた AudioClip を遅延破棄へ渡す (UAF防止 + 再生中でも他トラックを
    // 止めない)。即破棄せず、参照中スナップショットが回収される時に解放させる。
    timelineView.setEditBeforeChangeCallback([this](std::vector<std::unique_ptr<AudioClip>>&& clips)
    {
        audioEngine.deferClipDestruction(std::move(clips));
    });

    // マーカー / ループ範囲のルーラー右クリックメニュー
    auto& ruler = timelineView.getRuler();
    // 時刻行 / 小節行の表示状態を AppSettings から復元 + 変更を保存
    ruler.setTimeRowVisible(appSettings.rulerTimeRowVisible);
    ruler.setBarsRowVisible(appSettings.rulerBarsRowVisible);
    trackHeaderPanel.setRulerHeight(ruler.getDesiredHeight());
    ruler.onTimeRowVisibilityChanged = [this](bool v)
    {
        appSettings.rulerTimeRowVisible = v;
        timelineView.resized();
        timelineView.repaint();
        trackHeaderPanel.setRulerHeight(timelineView.getRuler().getDesiredHeight());
    };
    ruler.onBarsRowVisibilityChanged = [this](bool v)
    {
        appSettings.rulerBarsRowVisible = v;
        timelineView.resized();
        timelineView.repaint();
        trackHeaderPanel.setRulerHeight(timelineView.getRuler().getDesiredHeight());
    };
    // ルーラー内編集 (マーカー/テンポ/拍子の削除・ドラッグ・色変更・名前変更) の Undo 記録
    ruler.onMusicEditCommitted = [this](const TimelineRuler::EditLists& before,
                                        const TimelineRuler::EditLists& after)
    {
        pushMusicUndoFromLists(before, after);
    };
    ruler.onAddMarker = [this](double t)
    {
        auto before = captureMusicState();
        timelineView.addMarkerAtTime(t);   // マーカー追加 (同期) + 名前編集開始
        pushMusicUndo(before);             // 追加を Undo 登録 (名前は finishMarkerNameEdit で別途)
    };
    ruler.onEditMarkerName = [this](int idx)
    {
        timelineView.beginMarkerNameEdit(idx);
    };
    ruler.onSnapTime = [this](double t) { return timelineView.snapTimePublic(t); };
    ruler.onToggleMarkerColors = [this](bool v)
    {
        appSettings.useMarkerColors = v;
        timelineView.setAppSettings(appSettings);
    };
    // 録音中のルーラー範囲 (ロケーター) 変更は禁止: エンジンのループラップは範囲の
    // atomic を毎ブロック見るが、ループ録音のテイクスライスは録音開始時のスナップショット
    // を使うため、途中で範囲が変わると全テイクが累積的にずれる。通常録音中にラップ範囲を
    // 作る/動かすのも尺計算 (停止位置 − 開始位置) を壊すので、停止まで一律に無視する
    ruler.onSetLoopStart = [this](double t)
    {
        if (isRecording) return;
        loopStartSecs = t;
        if (loopEndSecs <= loopStartSecs) loopEndSecs = loopStartSecs + 1.0;
        loopActive = true;
        timelineView.setRulerRange(loopStartSecs, loopEndSecs, loopActive);
        audioEngine.setLoopRange(loopStartSecs, loopEndSecs, loopActive);
    };
    ruler.onSetLoopEnd = [this](double t)
    {
        if (isRecording) return;
        loopEndSecs = t;
        if (loopEndSecs < loopStartSecs) std::swap(loopStartSecs, loopEndSecs);
        loopActive = true;
        timelineView.setRulerRange(loopStartSecs, loopEndSecs, loopActive);
        audioEngine.setLoopRange(loopStartSecs, loopEndSecs, loopActive);
    };
    ruler.onSetLoopRange = [this](double s, double e)
    {
        // ルーラー範囲のドラッグでは範囲だけを更新（ループ再生は LOOP ボタンで切替）。
        // 選択範囲とは独立 (ルーラー帯のみ更新)。
        if (isRecording) return;
        loopStartSecs = s;
        loopEndSecs   = e;
        timelineView.setRulerRange(loopStartSecs, loopEndSecs, loopActive);
        audioEngine.setLoopRange(loopStartSecs, loopEndSecs, loopActive);
    };

    // ドラッグ&ドロップ: 複数ファイルも 1 つの「インポート中…」ウィンドウでまとめて変換 → 配置する。
    timelineView.onImportAudioFiles = [this](const juce::Array<juce::File>& srcs,
                                             double dropTime, int targetTrackIdx) {
        importAudioFilesAtDrop(srcs, dropTime, targetTrackIdx);
    };
    timelineView.onMidiClipDoubleClicked = [this](MidiClip* mc, Track* t)
    {
        openPianoRollFor(mc, t);
    };
    // MIDI クリップ破棄前 (分割等) に、そのクリップを参照しているピアノロールを閉じる
    timelineView.onMidiClipWillBeRemoved = [this](MidiClip* mc)
    {
        for (int i = pianoRollWindows.size(); --i >= 0;)
            if (auto* w = pianoRollWindows[i]; w && w->getClip() == mc)
            {
                pianoRollWindows.remove(i);  // OwnedArray が delete → ウィンドウを閉じる
                break;
            }
    };
    timelineView.onWaveformRefreshNeeded = [this]
    {
        scheduleWaveformRefresh();
    };
    // クリップを全トラックより下の空き領域へドロップ → 新規トラックを作って移す
    timelineView.onMoveClipToNewTrack = [this](const EditActions::ClipParams& p, bool stereo,
                                               Lane* srcLane, AudioClip* srcClip)
    {
        moveClipToNewTrack(p, stereo, srcLane, srcClip);
    };

    timelineView.onImportMidi = [this](const juce::File& src, double dropTime)
    {
        importMidiFromFile(src, dropTime);
    };

    timelineView.onApplyDetectedBpm = [this](double newBpm)
    {
        auto before = captureMusicState();
        bpm = newBpm;
        appSettings.initialBpm = newBpm;
        toolbar.setBpm(bpm);
        timelineView.setBpm(bpm);
        timelineView.setAppSettings(appSettings);
        audioEngine.setMetronomeBpm(bpm);
        audioEngine.setAppSettings(appSettings);
        markProjectDirty();
        pushMusicUndo(before);   // テンポ検出の適用を Undo 対応
    };

    timelineView.onSetSelectionRange = [this](double s, double e)
    {
        // クリップ上半分のドラッグで範囲選択。ルーラー範囲 / ループ再生とは独立
        // (選択範囲はループ再生やプロジェクト保存に影響しない = それぞれ別々に機能)。
        timelineView.setSelectionRange(s, e, e > s + 0.001);
    };

    // 選択 (クリップ / 範囲) が変わったらヘッダの ↑ (採用) ボタンの活性表示を更新
    timelineView.onSelectionChanged = [this]
    {
        // クリップ / 範囲を選択したら、そのトラックをヘッダ側でも選択状態にする。
        // (選択が空の時は触らない = 空きクリックで手動のトラック選択を消さない)
        auto tracks = timelineView.getInvolvedTrackIndices();
        if (!tracks.empty())
        {
            trackHeaderPanel.setSelectedTracks(tracks);
            selectedTrackIndex = tracks.front();   // 主選択 index (インポート先等)
        }
        trackHeaderPanel.repaintHeaders();
    };

    toolbar.onLoopToggle = [this](bool v)
    {
        // 録音中の LOOP 切替は禁止: 通常録音中に ON にするとラップで位置が巻き戻り
        // 尺計算が壊れ、ループ録音中に OFF にするとテイクスライスの前提が崩れる。
        // ボタンは setClickingTogglesState でクリック時に自前トグル済みなので表示を戻す
        if (isRecording) { toolbar.setLoopActive(loopActive); return; }
        loopActive = v && (loopEndSecs > loopStartSecs + 0.001);
        timelineView.setRulerRange(loopStartSecs, loopEndSecs, loopActive);
        audioEngine.setLoopRange(loopStartSecs, loopEndSecs, loopActive);
        toolbar.setLoopActive(loopActive);
    };
    toolbar.onToolModeChanged = [this](int mode)
    {
        appSettings.toolMode = (ToolMode)mode;
        timelineView.setAppSettings(appSettings);
        toolbar.setToolMode(mode);
    };

    toolbar.onBpmChanged = [this](double newBpm)
    {
        auto before = captureMusicState();
        bpm = newBpm;
        appSettings.initialBpm = newBpm;
        timelineView.setBpm(bpm);
        timelineView.setAppSettings(appSettings);
        audioEngine.setMetronomeBpm(bpm);
        audioEngine.setAppSettings(appSettings);
        markProjectDirty();
        pushMusicUndo(before);   // BPM ラベル編集 / タップテンポを Undo 対応
    };

    toolbar.onMetronomeToggle = [this](bool on)
    {
        audioEngine.setMetronomeEnabled(on);
    };

    toolbar.onCountInChanged = [this](int bars) {
        appSettings.countInBars = bars;
        // 排他: カウントインを設定したらプリロールはオフ
        if (bars > 0)
        {
            appSettings.preRollSecs = 0.0;
            toolbar.setPreRollSecs(0.0);
        }
        toolbar.setCountInBars(bars);
    };

    toolbar.onPreRollChanged = [this](double secs) {
        appSettings.preRollSecs = secs;
        // 排他: プリロールを設定したらカウントインはオフ
        if (secs >= 0.5)
        {
            appSettings.countInBars = 0;
            toolbar.setCountInBars(0);
        }
        toolbar.setPreRollSecs(secs);
    };

    toolbar.onMetronomeSettings = [this]
    {
        // メトロノーム設定: 音量・パン・音色・アクセント
        class MetronomeSettings : public juce::Component
        {
        public:
            juce::Label  volLabel, panLabel, soundLabel, accentLabel;
            juce::Slider volSlider, panSlider;
            juce::ComboBox soundCombo;
            juce::ToggleButton accentBtn;
            std::function<void(float)> onVol;
            std::function<void(float)> onPan;
            std::function<void(int)>   onSound;
            std::function<void(bool)>  onAccent;

            MetronomeSettings(float curVol, float curPan, int curSound, bool curAccent)
            {
                auto setupLabel = [this](juce::Label& l, juce::String txt) {
                    l.setText(txt, juce::dontSendNotification);
                    l.setColour(juce::Label::textColourId, juce::Colours::white);
                    addAndMakeVisible(l);
                };
                setupLabel(volLabel,    tr(u8"音量"));
                setupLabel(panLabel,    tr(u8"パン"));
                setupLabel(soundLabel,  tr(u8"音色"));
                setupLabel(accentLabel, tr(u8"アクセント"));

                addAndMakeVisible(volSlider);
                volSlider.setSliderStyle(juce::Slider::LinearHorizontal);
                volSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 62, 18);
                // 音量はトラックフェーダーと同じ dB 表記にする (0.22 のような線形値は分かりにくい)。
                // 内部の線形ボリュームとの対応: トラック同期式 metronomeVolume = decibelsToGain(dB)*0.5 の
                // 逆変換で表示 dB = gainToDecibels(metronomeVolume * 2) とし、フェーダーの dB と一致させる。
                volSlider.setRange(-60.0, 6.0, 0.1);
                volSlider.setNumDecimalPlacesToDisplay(1);
                volSlider.setTextValueSuffix(" dB");
                volSlider.setValue(juce::Decibels::gainToDecibels(
                                       juce::jmax(1.0e-4f, curVol) * 2.0f, -60.0f),
                                   juce::dontSendNotification);
                volSlider.onValueChange = [this] {
                    const float v = (float) juce::Decibels::decibelsToGain(
                                        volSlider.getValue(), -60.0) * 0.5f;
                    if (onVol) onVol(v);
                };

                addAndMakeVisible(panSlider);
                panSlider.setSliderStyle(juce::Slider::LinearHorizontal);
                panSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 50, 18);
                panSlider.setRange(-1.0, 1.0, 0.01);
                panSlider.setValue(curPan);
                panSlider.setDoubleClickReturnValue(true, 0.0);
                panSlider.onValueChange = [this] { if (onPan) onPan((float)panSlider.getValue()); };

                addAndMakeVisible(soundCombo);
                soundCombo.addItem("Beep",    1);
                soundCombo.addItem("Stick",   2);
                soundCombo.addItem("Cowbell", 3);
                soundCombo.addItem("Wood",    4);
                soundCombo.addItem("Tick",    5);
                soundCombo.addItem("Bell",    6);
                soundCombo.setSelectedId(curSound + 1, juce::dontSendNotification);
                soundCombo.onChange = [this] { if (onSound) onSound(soundCombo.getSelectedId() - 1); };

                accentBtn.setButtonText(tr(u8"頭の拍を強くする"));
                accentBtn.setColour(juce::ToggleButton::textColourId, juce::Colours::white);
                accentBtn.setToggleState(curAccent, juce::dontSendNotification);
                accentBtn.onClick = [this] { if (onAccent) onAccent(accentBtn.getToggleState()); };
                addAndMakeVisible(accentBtn);

                setSize(320, 180);
            }
            // 外部 (CLICK トラックのフェーダー等) からの変更をダイアログ表示へ反映する。
            // dontSendNotification なのでコールバックは発火せず無限ループにならない。
            void pullValues(float volLinear, float pan, int sound, bool accent)
            {
                volSlider.setValue(juce::Decibels::gainToDecibels(
                                       juce::jmax(1.0e-4f, volLinear) * 2.0f, -60.0f),
                                   juce::dontSendNotification);
                panSlider.setValue(pan, juce::dontSendNotification);
                soundCombo.setSelectedId(sound + 1, juce::dontSendNotification);
                accentBtn.setToggleState(accent, juce::dontSendNotification);
            }
            void resized() override
            {
                int y = 10;
                volLabel.setBounds(8, y, 80, 20);
                volSlider.setBounds(90, y, 220, 20); y += 28;
                panLabel.setBounds(8, y, 80, 20);
                panSlider.setBounds(90, y, 220, 20); y += 28;
                soundLabel.setBounds(8, y, 80, 22);
                soundCombo.setBounds(90, y, 220, 22); y += 30;
                accentLabel.setBounds(8, y, 80, 22);
                accentBtn.setBounds(90, y, 220, 22);
            }
            void paint(juce::Graphics& g) override { g.fillAll(juce::Colour(0xff2a2a2a)); }
        };

        auto* dlg = new MetronomeSettings(
            audioEngine.getMetronomeVolume(),
            audioEngine.getMetronomePan(),
            audioEngine.getMetronomeSound(),
            audioEngine.getMetronomeAccent());

        // CLICK トラックがあれば、ダイアログの変更はトラック側に書き戻して両者を一致させる
        // (トラックのフェーダー (dB) / パン / 音色 / ACC の UI も追従する)。トラックが無い時は
        // 従来どおりエンジンへ直接反映する (CLICK ボタンだけでメトロノームを使うケース)。
        dlg->onVol    = [this](float v) {
            if (auto* ct = trackManager.getClickTrack())
            {
                // 線形ボリューム v → トラックの dB (フェーダーと同じスケール: trackDb = gainToDecibels(v*2))
                ct->setVolume(juce::Decibels::gainToDecibels(juce::jmax(1.0e-4f, v) * 2.0f, -60.0f));
                syncClickTrackToEngine();      // エンジン + CLICK ボタン + ダイアログ pull (冪等)
                trackHeaderPanel.refresh();    // フェーダー UI を追従
                markProjectDirty();
            }
            else audioEngine.setMetronomeVolume(v);
        };
        dlg->onPan    = [this](float p) {
            if (auto* ct = trackManager.getClickTrack())
            {
                ct->setPan(p);
                syncClickTrackToEngine();
                trackHeaderPanel.refresh();
                markProjectDirty();
            }
            else audioEngine.setMetronomePan(p);
        };
        dlg->onSound  = [this](int  s) {
            if (auto* ct = trackManager.getClickTrack())
            {
                ct->setClickSound(s);
                syncClickTrackToEngine();
                trackHeaderPanel.refresh();
                markProjectDirty();
            }
            else audioEngine.setMetronomeSound(s);
        };
        dlg->onAccent = [this](bool b) {
            if (auto* ct = trackManager.getClickTrack())
            {
                ct->setClickAccent(b);
                syncClickTrackToEngine();
                trackHeaderPanel.refresh();
                markProjectDirty();
            }
            else audioEngine.setMetronomeAccent(b);
        };

        // CLICK トラックのフェーダー等 → ダイアログへの追従経路を登録 (閉じれば SafePointer が no-op 化)
        metronomeDlgPull = [safe = juce::Component::SafePointer<MetronomeSettings>(dlg)]
                           (float v, float p, int s, bool a)
        {
            if (auto* d = safe.getComponent()) d->pullValues(v, p, s, a);
        };

        juce::DialogWindow::LaunchOptions opts;
        opts.content.setOwned(dlg);
        opts.dialogTitle = tr(u8"メトロノーム設定");
        opts.dialogBackgroundColour = juce::Colour(0xff2a2a2a);
        opts.escapeKeyTriggersCloseButton = true;
        opts.useNativeTitleBar = true;
        opts.resizable = false;
        opts.launchAsync();
    };
    ruler.onClearLoop = [this]
    {
        loopActive = false;
        loopStartSecs = 0.0;
        loopEndSecs   = 0.0;
        timelineView.setRulerRange(0.0, 0.0, false);
        audioEngine.setLoopRange(0.0, 0.0, false);
    };
    ruler.onClearMarkers = [this]
    {
        auto before = captureMusicState();
        timelineView.setMarkers({});
        pushMusicUndo(before);
    };

    ruler.onSnapTimeForBpm = [this](double t) { return timelineView.snapTimePublic(t); };

    ruler.onBpmChangesUpdated = [this](const std::vector<BpmChange>& list) {
        appSettings.bpmChanges = list;
        std::sort(appSettings.bpmChanges.begin(), appSettings.bpmChanges.end(),
                  [](const BpmChange& a, const BpmChange& b) { return a.timeSec < b.timeSec; });
        timelineView.setAppSettings(appSettings);
        audioEngine.setAppSettings(appSettings);
    };
    ruler.onMeterChangesUpdated = [this](const std::vector<MeterChange>& list) {
        appSettings.meterChanges = list;
        std::sort(appSettings.meterChanges.begin(), appSettings.meterChanges.end(),
                  [](const MeterChange& a, const MeterChange& b) { return a.barIndex < b.barIndex; });
        timelineView.setAppSettings(appSettings);
        audioEngine.setAppSettings(appSettings);
    };

    ruler.onEditBpm = [this](double clickedTime)
    {
        // 既存の BPM 変更点があれば編集、なければ新規挿入
        double curBpm = appSettings.bpmAtTime(clickedTime);
        // 厳密一致でなくても近傍の既存変更（±1ms）を編集対象に
        bool   editingExisting = false;
        for (auto& bc : appSettings.bpmChanges)
            if (std::abs(bc.timeSec - clickedTime) < 0.001) { curBpm = bc.bpm; editingExisting = true; break; }

        class BpmDlg : public juce::Component
        {
        public:
            juce::Label  timeLabel, bpmLabel;
            juce::Slider bpmSlider;
            juce::TextButton insertBtn, cancelBtn;
            std::function<void(double)> onInsert;  // bpm
            BpmDlg(double t0, double bpm0, bool atSongStart)
            {
                auto setupLabel = [this](juce::Label& l, juce::String txt) {
                    l.setText(txt, juce::dontSendNotification);
                    l.setColour(juce::Label::textColourId, juce::Colours::white);
                    addAndMakeVisible(l);
                };
                int mins = (int)t0 / 60;
                double s = t0 - mins * 60;
                juce::String tt = juce::String::formatted("%d:%06.3f", mins, s);
                setupLabel(timeLabel, tr(u8"位置  ") + tt + (atSongStart ? tr(u8"  (曲頭)") : juce::String()));
                setupLabel(bpmLabel,  tr(u8"BPM"));

                addAndMakeVisible(bpmSlider);
                bpmSlider.setSliderStyle(juce::Slider::LinearHorizontal);
                bpmSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 60, 22);
                bpmSlider.setRange(20.0, 300.0, 0.01);
                bpmSlider.setValue(bpm0);
                bpmSlider.setNumDecimalPlacesToDisplay(2);

                insertBtn.setButtonText(tr(u8"挿入"));
                cancelBtn.setButtonText(tr(u8"キャンセル"));
                addAndMakeVisible(insertBtn);
                addAndMakeVisible(cancelBtn);

                auto closeMe = [this] {
                    if (auto* dw = findParentComponentOfClass<juce::DialogWindow>())
                        dw->exitModalState(0);
                };
                insertBtn.onClick = [this, closeMe] {
                    if (onInsert) onInsert(bpmSlider.getValue());
                    closeMe();
                };
                cancelBtn.onClick = [closeMe] { closeMe(); };

                setSize(380, 160);
            }
            void resized() override
            {
                int y = 10;
                timeLabel.setBounds(8, y, 360, 22); y += 30;
                bpmLabel.setBounds(8, y, 50, 22);
                bpmSlider.setBounds(60, y, 300, 22); y += 36;
                int btnW = 100, btnH = 26, gap = 8;
                int x = (getWidth() - btnW * 2 - gap) / 2;
                insertBtn.setBounds(x, y, btnW, btnH); x += btnW + gap;
                cancelBtn.setBounds(x, y, btnW, btnH);
            }
            void paint(juce::Graphics& g) override { g.fillAll(juce::Colour(0xff2a2a2a)); }
        };

        bool atSongStart = clickedTime < 0.001;
        auto* dlg = new BpmDlg(clickedTime, curBpm, atSongStart);

        auto applyAndRefresh = [this] {
            std::sort(appSettings.bpmChanges.begin(), appSettings.bpmChanges.end(),
                      [](const BpmChange& a, const BpmChange& b) { return a.timeSec < b.timeSec; });
            timelineView.setAppSettings(appSettings);
            audioEngine.setAppSettings(appSettings);
        };

        dlg->onInsert = [this, clickedTime, applyAndRefresh](double newBpm) {
            auto before = captureMusicState();
            if (clickedTime < 0.001)
            {
                // 曲頭: 全体BPMとして扱う
                bpm = newBpm;
                appSettings.initialBpm = newBpm;
                timelineView.setBpm(bpm);
                toolbar.setBpm(newBpm);
                audioEngine.setMetronomeBpm(newBpm);
            }
            else
            {
                bool replaced = false;
                for (auto& bc : appSettings.bpmChanges)
                    if (std::abs(bc.timeSec - clickedTime) < 0.001) { bc.bpm = newBpm; replaced = true; break; }
                if (!replaced) appSettings.bpmChanges.push_back({ clickedTime, newBpm });
            }
            applyAndRefresh();
            markProjectDirty();
            pushMusicUndo(before);   // テンポ挿入/編集を Undo 対応
        };
        juce::ignoreUnused(editingExisting);

        juce::DialogWindow::LaunchOptions opts;
        opts.content.setOwned(dlg);
        opts.dialogTitle = tr(u8"BPM の設定");
        opts.dialogBackgroundColour = juce::Colour(0xff2a2a2a);
        opts.escapeKeyTriggersCloseButton = true;
        opts.useNativeTitleBar = true;
        opts.resizable = false;
        opts.launchAsync();
    };

    ruler.onEditMeter = [this](int clickedBar)
    {
        // 拍子設定: クリックした小節 + 分子/分母 + 挿入
        int curN = appSettings.meterNumerator, curD = appSettings.meterDenominator;
        // 既にこの小節に変更が入っていれば、その拍子を初期値に
        for (auto& mc : appSettings.meterChanges)
            if (mc.barIndex == clickedBar - 1) { curN = mc.numerator; curD = mc.denominator; break; }

        class MeterDlg : public juce::Component
        {
        public:
            juce::Label  barLabel, numLabel, denLabel;
            juce::Slider barSlider;
            juce::ComboBox numCombo, denCombo;
            juce::TextButton insertBtn, cancelBtn;
            std::function<void(int, int, int)> onInsert;  // bar, num, den
            std::function<void()>              onClose;
            MeterDlg(int curBar, int n0, int d0)
            {
                auto setupLabel = [this](juce::Label& l, juce::String txt) {
                    l.setText(txt, juce::dontSendNotification);
                    l.setColour(juce::Label::textColourId, juce::Colours::white);
                    addAndMakeVisible(l);
                };
                setupLabel(barLabel, tr(u8"小節"));
                setupLabel(numLabel, tr(u8"分子"));
                setupLabel(denLabel, tr(u8"分母"));

                addAndMakeVisible(barSlider);
                barSlider.setSliderStyle(juce::Slider::IncDecButtons);
                barSlider.setTextBoxStyle(juce::Slider::TextBoxLeft, false, 80, 22);
                barSlider.setRange(1, 999, 1);
                barSlider.setValue(curBar);

                addAndMakeVisible(numCombo);
                for (int i = 1; i <= 12; ++i) numCombo.addItem(juce::String(i), i);
                numCombo.setSelectedId(n0, juce::dontSendNotification);

                addAndMakeVisible(denCombo);
                for (int v : { 2, 4, 8, 16 }) denCombo.addItem(juce::String(v), v);
                denCombo.setSelectedId(d0, juce::dontSendNotification);

                insertBtn.setButtonText(tr(u8"挿入"));
                cancelBtn.setButtonText(tr(u8"キャンセル"));
                addAndMakeVisible(insertBtn);
                addAndMakeVisible(cancelBtn);

                auto closeMe = [this] {
                    if (auto* dw = findParentComponentOfClass<juce::DialogWindow>())
                        dw->exitModalState(0);
                };
                insertBtn.onClick = [this, closeMe] {
                    if (onInsert) onInsert((int)barSlider.getValue(),
                                           numCombo.getSelectedId(),
                                           denCombo.getSelectedId());
                    closeMe();
                };
                cancelBtn.onClick = [closeMe] { closeMe(); };

                setSize(360, 180);
            }
            void resized() override
            {
                int y = 10;
                barLabel.setBounds(8, y, 50, 22);
                barSlider.setBounds(60, y, 280, 22); y += 30;
                numLabel.setBounds(8, y, 50, 22);
                numCombo.setBounds(60, y, 280, 22); y += 30;
                denLabel.setBounds(8, y, 50, 22);
                denCombo.setBounds(60, y, 280, 22); y += 36;
                int btnW = 100, btnH = 26, gap = 8;
                int x = (getWidth() - btnW * 2 - gap) / 2;
                insertBtn.setBounds(x, y, btnW, btnH); x += btnW + gap;
                cancelBtn.setBounds(x, y, btnW, btnH);
            }
            void paint(juce::Graphics& g) override { g.fillAll(juce::Colour(0xff2a2a2a)); }
        };

        auto* dlg = new MeterDlg(clickedBar, curN, curD);
        auto applyAndRefresh = [this] {
            // 0番目（曲頭）は appSettings.meterNumerator/Denominator が反映する
            // それ以外は meterChanges を昇順に並べる
            std::sort(appSettings.meterChanges.begin(), appSettings.meterChanges.end(),
                      [](const MeterChange& a, const MeterChange& b) { return a.barIndex < b.barIndex; });
            timelineView.setAppSettings(appSettings);
            audioEngine.setAppSettings(appSettings);  // 拍子変更をオーディオスレッドへ伝搬
            audioEngine.setMetronomeBeatsPerBar(appSettings.meterNumerator);
        };
        dlg->onInsert = [this, applyAndRefresh](int bar, int n, int d) {
            auto before = captureMusicState();
            int barIdx = juce::jmax(0, bar - 1);
            if (barIdx == 0)
            {
                appSettings.meterNumerator   = n;
                appSettings.meterDenominator = d;
            }
            else
            {
                bool replaced = false;
                for (auto& mc : appSettings.meterChanges)
                    if (mc.barIndex == barIdx) { mc.numerator = n; mc.denominator = d; replaced = true; break; }
                if (!replaced)
                    appSettings.meterChanges.push_back({ barIdx, n, d });
            }
            applyAndRefresh();
            markProjectDirty();
            pushMusicUndo(before);   // 拍子挿入/編集を Undo 対応
        };
        juce::DialogWindow::LaunchOptions opts;
        opts.content.setOwned(dlg);
        opts.dialogTitle = tr(u8"拍子の設定");
        opts.dialogBackgroundColour = juce::Colour(0xff2a2a2a);
        opts.escapeKeyTriggersCloseButton = true;
        opts.useNativeTitleBar = true;
        opts.resizable = false;
        opts.launchAsync();
    };

    // Init
    toolbar.setBpm(bpm);
    audioEngine.setMetronomeBpm(bpm);
    toolbar.setTimePosition(0.0, 1, 1);
    statusBar.setSampleRate((int) appSettings.projectSampleRate);
    statusBar.setBitDepth(appSettings.projectBitDepth);
    statusBar.setTrackCount(0);

    audioEngine.initialise();

    // 録音レイテンシ補正の設定 (アプリ全体設定) をエンジンへ反映
    audioEngine.setRecordingLatencyComp(appPrefs.recLatencyAutoComp,
                                        appPrefs.recLatencyManualMs);
    // ディスクストリーミング (再生の音声読み込みを先読みスレッドへ分離) の ON/OFF を反映
    audioEngine.setDiskStreamingEnabled(appPrefs.diskStreaming);
    // オーディオのマルチコア処理 (トラック描画を複数コアへ分散) の ON/OFF を反映
    audioEngine.setMulticoreAudioEnabled(appPrefs.multicoreAudio);

    if (auto* dev = audioEngine.getDeviceManager().getCurrentAudioDevice())
    {
        int sr = (int)dev->getCurrentSampleRate();
        statusBar.setSampleRate(sr);
        timelineView.setSampleRate((double)sr);
    }

    // 20Hz: メーター/数値表示/ループ検出/録音オーバーレイの更新。再生バー自体は
    // VBlankAttachment (onVBlank) 側でディスプレイのリフレッシュに同期して動かす。
    startTimerHz(20);

    // 再生バーをディスプレイの垂直同期に合わせて更新 (60/120Hz 等に自動追従)。
    // タイムスタンプ付きコールバックで「次フレームの提示時刻」を受け取り等速補間する。
    vblankAttachment = juce::VBlankAttachment(this, [this](double ts) { onVBlank(ts); });

    // 自動保存タイマー
    autoSaveTimer = std::make_unique<AutoSaveTickTimer>();
    autoSaveTimer->tick = [this] { performAutoSave(); };
    restartAutoSaveTimer();

    deviceAutoSaver = std::make_unique<AudioDeviceSettings::AutoSaver>(
        audioEngine.getDeviceManager());

    pluginManager.initialise();

    // ApplicationCommandManager にトランスポートコマンドを登録し、
    // MainComponent およびプラグインエディタ越しでも効くキー入力受付を仕込む
    commandManager.registerAllCommandsForTarget(this);
    addKeyListener(commandManager.getKeyMappings());

    // プラグインのネイティブ Cocoa UI 越しでも Space 等が効くよう、
    // NSEvent ローカルモニターを設置（macOS のみ実体動作）
    globalKeyMonitor = std::make_unique<GlobalKeyMonitor>(
        [this](int unicode, int mods) -> bool
        {
            // テキスト入力中は GlobalKeyMonitor 側で既に除外されている
            switch (unicode)
            {
                case ' ':           // Space
                    juce::MessageManager::callAsync([this] { togglePlay(); });
                    return true;
                case 's': case 'S':
                    if (mods == 0) { juce::MessageManager::callAsync([this] { stopTransport(); }); return true; }
                    return false;
                case 'r': case 'R':
                    if (mods == 0) { juce::MessageManager::callAsync([this] { toggleRecord(); }); return true; }
                    return false;
                default:
                    return false;
            }
        });

   #if JUCE_MAC
    juce::MenuBarModel::setMacMainMenu(this);
   #else
    // Windows / Linux: ウィンドウ上端にメニューバーを表示
    menuBar = std::make_unique<juce::MenuBarComponent>(this);
    addAndMakeVisible(*menuBar);
   #endif
}

// MainComponent::~MainComponent() は MainComponent_Plugins.cpp に移動。
// (OwnedArray<PluginEditorWindow / PianoRollWindow> の解体には完全型が必要なため、
//  これらクラス定義が見える .cpp に置く必要がある)

void MainComponent::timerCallback()
{
    // Metering: アイドル時 (再生/録音/入力モニターのいずれも無し) はメーター計算を省く。
    // 直後の数 tick は猶予として計算を続け、メーターが無音まで滑らかに減衰してから止める。
    bool meterActive = isPlaying || isRecording;
    if (!meterActive)
    {
        for (int i = 0; i < trackManager.getTrackCount(); ++i)
        {
            auto* t = trackManager.getTrack(i);
            if (t && (t->isRecArmed() || t->isInputMonitor()))
            {
                meterActive = true;
                break;
            }
        }
    }

    if (meterActive)
        idleMeterTicks = 0;
    else
        ++idleMeterTicks;

    // 猶予はエンジンの VU バリスティクス (時定数 300ms・表示床 -60dB 到達 ≈ 2.1s) が
    // 落ち切る長さ (3s)。最終 tick はバーを床へスナップして消灯を確定する。旧実装は
    // 1.5s で更新を打ち切っていたため、減衰途中の値 (≈ -43dB) でマスター VU の点灯が
    // 残ったままになっていた。ピークホールドの表示は保持する (RESET ボタンで消せる)
    constexpr int kMeterIdleGraceTicks = 60;   // 20Hz × 60 = 3s
    if (meterActive || idleMeterTicks <= kMeterIdleGraceTicks)
    {
        const bool snapFloor = !meterActive && idleMeterTicks >= kMeterIdleGraceTicks;
        masterPanel.setLevels(
            snapFloor ? -96.0f : audioEngine.getPeakL(),
            snapFloor ? -96.0f : audioEngine.getPeakR(),
            snapFloor ? -96.0f : audioEngine.getVUL(),
            snapFloor ? -96.0f : audioEngine.getVUR(),
            audioEngine.getPeakHoldL(), audioEngine.getPeakHoldR());

        // 入力レベルメータ（各トラック）
        trackHeaderPanel.updateInputLevels(
            [this, snapFloor](int ch){ return snapFloor ? -96.0f : audioEngine.getInputPeak(ch); },
            [this, snapFloor](int ch){ return snapFloor ? -96.0f : audioEngine.getInputVU(ch);   });
    }

    // Transport position（再生バー自体の描画は onVBlank 側。ここは数値表示・ループ検出）
    if (isPlaying)
    {
        playPosition = audioEngine.getCurrentPositionSeconds();

        int bar1, beat1;
        appSettings.barAndBeatAtTime(playPosition, bar1, beat1);
        toolbar.setTimePosition(playPosition, bar1, beat1);
        statusBar.setDuration(playPosition);

        // ループラップを検出したら直ちに再描画（再生バーと波形のズレ防止）
        int wraps = audioEngine.getLoopWrapCount();
        if (wraps != lastLoopWrapCount)
        {
            timelineView.repaint();
            // 録音中のループ録音テイク積み上げも同タイミングで処理
            if (isRecording)
            {
                while (lastLoopWrapCount < wraps)
                {
                    recordingMgr.onLoopWrap();
                    ++lastLoopWrapCount;
                }
                // ラップ毎に変化するのは録音アーム中トラック (テイクレーン追加) だけなので、
                // 全トラックの refresh はせず軽量版で済ませる (多トラック時のヒッチ防止)
                trackHeaderPanel.refreshRecArmedTracks();
                timelineView.refresh();
            }
            else
            {
                lastLoopWrapCount = wraps;
                // 遡及キャプチャは「連続再生」前提の対応付けがラップで壊れるため、
                // 現在位置 (ループ頭直後) から取り直す (シーク時と同じ作法)。これで
                // ラップ後の Cmd+Shift+R 確定やパンチインが正しい位置に配置される
                restartRetrospectiveAt(playPosition);
            }
        }
    }
    else
    {
        // 停止中もピアノロールの再生バーをオーディオエンジンの現在位置に同期し続ける
        // (stop 直後に repaint が抜ける問題への保険)
        propagatePlayheadToPianoRolls(audioEngine.getCurrentPositionSeconds());
    }

    // 録音中はライブ波形オーバーレイを伸ばし続けるため再描画するが、全面ではなく
    // 録音域 (録音トラック行) だけに絞ってちらつきと負荷を抑える。
    if (isRecording)
        timelineView.repaintRecordingArea();
}

// VBlankAttachment コールバック: ディスプレイの垂直同期ごとに呼ばれ、視覚プレイヘッドを
// 更新する。timestampSec は「次に提示されるフレームの時刻 (秒・単調増加)」。これを壁時計
// として等速補間し、真のオーディオ位置へ緩く追従させることで、タイマー/オーディオブロック
// 境界のジッタ由来のカクツキ (特に横ズーム時) を排し、表示リフレッシュに乗せて最も滑らかに
// 動かす。再生中のみ更新し、停止中は平滑化をリセットする。
void MainComponent::onVBlank (double timestampSec)
{
    if (!isPlaying)
    {
        updatePlayheadScrub (timestampSec);  // ; / − による連続スクラブ (停止中のみ)
        playheadSmoothActive = false;  // 次回再生で再同期できるよう平滑化をリセット
        return;
    }
    scrubActive = false; scrubDir = 0;  // 再生中はスクラブしない

    const double rawPos = audioEngine.getCurrentPositionSeconds();

    if (!playheadSmoothActive)
    {
        smoothedPlayhead     = rawPos;
        playheadSmoothActive = true;
        lastRawPlayheadPos   = rawPos;
        lastLoopWrapWallSec  = -1.0e9;  // 再生開始直後はラップ補正しない
    }
    else
    {
        // 実位置 (rawPos) が後退したか。後方シーク / ループラップの判定に使う
        // (rawPos はサンプルカウンタなので、後退は本物のジャンプ時のみ起きる)。
        const bool rawWentBackward = rawPos < lastRawPlayheadPos - 0.001;

        // ループラップの実検出: ループ末尾付近 → ループ頭付近への後退ジャンプのみを
        // ラップとみなす (単なる後方シークでは誤発火させない)
        if (loopActive && loopEndSecs > loopStartSecs + 0.001
            && rawPos < lastRawPlayheadPos - 0.05
            && lastRawPlayheadPos > loopEndSecs - 0.5
            && rawPos < loopStartSecs + 0.5)
            lastLoopWrapWallSec = timestampSec;
        lastRawPlayheadPos = rawPos;

        // dt はフレーム落ち/バックグラウンド復帰に備えてクランプ
        const double dt = juce::jlimit(0.0, 0.1, timestampSec - lastPlayheadWallSec);
        smoothedPlayhead += dt;                  // 再生速度 1.0 で前進
        const double err = rawPos - smoothedPlayhead;
        // スナップは「実位置が飛んだ」時だけ: 前方ジャンプ (前方シーク) か、後方ジャンプ
        // (後方シーク / ループラップ = rawPos が実際に後退) のみ。それ以外で smoothed が
        // rawPos を追い越して err が大きく負になるのは「rawPos が一瞬止まっただけ」
        // (オーディオコールバックの一瞬の遅延・ファイル読み出しの詰まり等) なので、後方
        // スナップで戻さず緩く再収束させる。これで「数秒に1回バーが一瞬戻って止まる」
        // 視覚的ヒッチを防ぐ (壁時計前進は続くのでバーは滑らかに流れたまま)。
        if (err > 0.10 || (err < -0.10 && rawWentBackward))
            smoothedPlayhead = rawPos;           // 本物のシーク / ループ → スナップ
        else if (err < 0.0)
            smoothedPlayhead += err * 0.04;      // 追い越し (詰まり) → ゆっくり再収束・戻さない
        else
            smoothedPlayhead += err * 0.12;      // 通常の前方追従 (ドリフト防止)
    }
    lastPlayheadWallSec = timestampSec;

    // 視覚プレイヘッド: オーディオ出力レイテンシ + バッファサイズ分を後ろにずらして
    // 「いま耳に聞こえている音」と同期（バッファ未drainの分も補正）
    double visualPlayhead = smoothedPlayhead;
    if (auto* dev = audioEngine.getDeviceManager().getCurrentAudioDevice())
    {
        const double sr = dev->getCurrentSampleRate();
        if (sr > 0.0)
        {
            const int latencySamples = dev->getOutputLatencyInSamples()
                                       + dev->getCurrentBufferSizeSamples();
            const double latencySec = latencySamples / sr;
            visualPlayhead = juce::jmax(0.0, smoothedPlayhead - latencySec);
            // ループ範囲内ならラップを考慮。ただし「実際にラップした直後のレイテンシ窓」
            // に限定する。位置条件だけだと、ループ範囲へ線形に入った直後 (視覚位置がまだ
            // loopStart 手前) も満たしてしまい、一瞬ループ末尾に表示されてから頭に戻る
            if (loopActive && loopEndSecs > loopStartSecs + 0.001
                && smoothedPlayhead >= loopStartSecs && visualPlayhead < loopStartSecs
                && (timestampSec - lastLoopWrapWallSec) <= latencySec + 0.25)
            {
                visualPlayhead = loopEndSecs - (loopStartSecs - visualPlayhead);
                if (visualPlayhead < loopStartSecs)
                {
                    const double loopDur = loopEndSecs - loopStartSecs;
                    visualPlayhead = loopStartSecs
                        + std::fmod(smoothedPlayhead - latencySec - loopStartSecs + loopDur,
                                    loopDur);
                }
            }
        }
    }

    timelineView.setPlayheadPosition(visualPlayhead);
    propagatePlayheadToPianoRolls(visualPlayhead);

    // 録音中はライブ波形の先端 (再生バー付近・新着データ) を vblank ごとに描き直し、20Hz の
    // repaintRecordingArea だけでは 50ms + レイテンシ分カクついて追従する波形を、滑らかな
    // 再生バーに密着させる。帯を絞ってあるので毎フレーム描いても軽い (Windows で顕著だった
    // 「波形の描画が遅れる」対策)。
    if (isRecording)
        timelineView.repaintLiveLeadingEdge();
}

// ; (JIS の「+」キー位置) で進む / − で戻る。停止中に押している間、ディスプレイ垂直同期ごとに
// キー状態を poll し、一定速度で再生バーを滑らかに動かす。OS のキーリピート (固定レート・初動
// ディレイ) に依らず、フレーム単位の連続移動なのでヌルヌル動く (加速はなし・等速)。
void MainComponent::updatePlayheadScrub (double timestampSec)
{
    auto stop = [this] { scrubActive = false; scrubDir = 0; };

    // アプリ非前面、またはテキスト入力中 (BPM 編集 / クリップ名インライン編集 等) は無効。
    // isKeyCurrentlyDown はグローバルなキー状態を見るため、これらを除外しないと誤爆する。
    if (! juce::Process::isForegroundProcess()) { stop(); return; }
    if (auto* f = juce::Component::getCurrentlyFocusedComponent())
        if (dynamic_cast<juce::TextEditor*> (f) != nullptr) { stop(); return; }

    const bool fwd  = juce::KeyPress::isKeyCurrentlyDown (';')
                   || juce::KeyPress::isKeyCurrentlyDown (juce::KeyPress::numberPadAdd);
    const bool back = juce::KeyPress::isKeyCurrentlyDown ('-')
                   || juce::KeyPress::isKeyCurrentlyDown (juce::KeyPress::numberPadSubtract);
    const int dir = (fwd == back) ? 0 : (fwd ? +1 : -1);
    if (dir == 0) { stop(); return; }

    constexpr double scrubSpeed = 3.0;   // 秒/秒 (一定・遅すぎず早すぎず)

    // 開始フレーム/方向転換は prime のみ (dt=0)。以降は提示時刻の差分で等速移動。
    double dt;
    if (! scrubActive || dir != scrubDir) { scrubActive = true; dt = 0.0; }
    else                                    dt = juce::jlimit (0.0, 0.05, timestampSec - lastScrubWallSec);
    scrubDir = dir;
    lastScrubWallSec = timestampSec;

    if (dt > 0.0)
        seekTo (audioEngine.getCurrentPositionSeconds() + dir * scrubSpeed * dt);  // seekTo が 0 クランプ + 全同期
}

int MainComponent::newTrackInsertAfter() const
{
    const auto& sel = trackHeaderPanel.getSelectedTrackIndices();
    if (sel.empty()) return -1;                 // 未選択 → 末尾
    return *std::max_element(sel.begin(), sel.end());  // 選択の最下段の直後へ
}

void MainComponent::addTrack()
{
    pushTrackAddUndo(trackManager.addTrack({}, /*stereo=*/false, newTrackInsertAfter()));
    markProjectDirty();
}

void MainComponent::syncInputMonitorStateToEngine()
{
    bool  anyMonitoring = false;
    bool  anyRecArmed   = false;
    float monRev        = 0.0f;   // モニター中トラックの Rev (複数なら最大)
    PluginChain* monChain = nullptr;   // 主モニタトラック (先頭の input-monitor) の INS チェーン
    int   monInputCh    = 0;      // 主モニタトラックの入力チャンネル / mono・stereo / pan / 音量 (返しの定位用)
    bool  monStereo     = false;
    float monPan        = 0.0f;
    float monVolDb      = 0.0f;
    bool  havePrimary   = false;
    for (int i = 0; i < trackManager.getTrackCount(); ++i)
    {
        auto* t = trackManager.getTrack(i);
        if (!t) continue;
        if (t->isInputMonitor())
        {
            anyMonitoring = true;
            monRev = juce::jmax(monRev, t->getReverbSend());
            if (!havePrimary)   // 先頭の input-monitor トラックを FX / 入力定位 / パンの主対象にする
            {
                monChain   = &t->getPluginChain();
                monInputCh = t->getInputChannel();
                monStereo  = t->isStereo();
                monPan     = t->getPan();
                monVolDb   = t->getVolume();
                havePrimary = true;
            }
        }
        if (t->isRecArmed()) anyRecArmed = true;
    }
    audioEngine.setInputMonitoringActive(anyMonitoring);
    audioEngine.setAnyTrackRecArmed(anyRecArmed);
    // 再生時の送りと同じ二乗テーパーを通す (モニターの返しだけ効きが強くならないように)
    audioEngine.setMonitorReverbSend(Track::reverbSendGain(monRev));
    // 返し音に INS を通すか (アプリ全体設定)。OFF のときはドライ返しのまま (nullptr を渡す)。
    // 入力チャンネル / mono・stereo / pan / 音量 は chain の有無に依らず常に渡す (返しの定位用)。
    audioEngine.setMonitorChain(appPrefs.monitorThroughInserts ? monChain : nullptr,
                                monInputCh, monStereo, monPan, monVolDb);
}

void MainComponent::syncClickTrackToEngine()
{
    auto* t = trackManager.getClickTrack();
    if (t == nullptr)
        return;   // CLICK トラックが無いときは何もしない (CLICK ボタンでの独立トグルを尊重する)

    const float volLinear = juce::Decibels::decibelsToGain(t->getVolume()) * 0.5f;
    audioEngine.setMetronomeVolume(volLinear);
    audioEngine.setMetronomePan(t->getPan());
    audioEngine.setMetronomeEnabled(!t->isMuted());
    audioEngine.setMetronomeSound(t->getClickSound());
    audioEngine.setMetronomeAccent(t->isClickAccent());
    const float mul = (t->getClickRate() == 1) ? 0.5f
                    : (t->getClickRate() == 2) ? 2.0f : 1.0f;
    audioEngine.setMetronomeRateMul(mul);
    toolbar.setMetronomeActive(!t->isMuted());   // CLICK ボタンの点灯をトラック状態に同期

    // 開いているメトロノーム設定ダイアログにも反映 (フェーダー → ダイアログの追従)
    if (metronomeDlgPull)
        metronomeDlgPull(volLinear, t->getPan(), t->getClickSound(), t->isClickAccent());
}

void MainComponent::addMidiTrack()
{
    // 空の MIDI トラック (ハモリ / ガイドメロディの手打ち込み用)。
    // SMF インポート経由の MIDI トラックと既定値をそろえる。
    int midiCount = 0;
    for (int i = 0; i < trackManager.getTrackCount(); ++i)
        if (auto* t = trackManager.getTrack(i); t && t->isMidiTrack())
            ++midiCount;

    auto* track = trackManager.addTrack("MIDI " + juce::String(midiCount + 1), /*stereo=*/true,
                                        newTrackInsertAfter(), /*midi=*/true);
    if (!track) return;
    track->setVolume(-14.0f);  // 内蔵シンセ出力は大きめなので控えめに

    // クリップは空のまま作らない。ユーザがタイムライン上で
    // Option+ドラッグ または 空きエリアのダブルクリックで MIDI クリップを作る。

    pushTrackAddUndo(track);

    // 内蔵シンセの確保 / 再生キャッシュを更新 (停止中なら次の play() で rebuild)。
    audioEngine.invalidatePlayback();
    markProjectDirty();
}

void MainComponent::togglePlay()
{
    isPlaying = !isPlaying;
    if (isPlaying)
    {
        playStartPos = audioEngine.getCurrentPositionSeconds();
        audioEngine.preparePlayback(trackManager);
        audioEngine.play();

        // 遡及録音: 録音アーム済みトラックがあれば再生中バックグラウンド録音を開始
        if (appSettings.retrospectiveEnabled && !isRecording
            && !recordingMgr.hasRetrospective())
        {
            for (int i = 0; i < trackManager.getTrackCount(); ++i)
            {
                auto* t = trackManager.getTrack(i);
                if (t->isRecArmed())
                {
                    recordingMgr.startRetrospective(t, playStartPos);
                    break;
                }
            }
        }
    }
    else
    {
        // 録音中なら確定してバックアップ
        if (isRecording) stopRecording();
        // 遡及録音を破棄（commit はしない）
        if (recordingMgr.hasRetrospective())
            recordingMgr.stopRetrospective(false,
                audioEngine.getCurrentPositionSeconds());

        audioEngine.stop();
        toolbar.setPlaying(false);
        toolbar.setRecording(false);

        // RTZ: 再生開始位置へ戻る
        if (appSettings.returnToStartOnStop)
        {
            audioEngine.setPosition(playStartPos);
            playPosition = playStartPos;
            int bar1, beat1;
            appSettings.barAndBeatAtTime(playStartPos, bar1, beat1);
            toolbar.setTimePosition(playStartPos, bar1, beat1);
            timelineView.setPlayheadPosition(playStartPos);
            propagatePlayheadToPianoRolls(playStartPos);
        }
        else
        {
            propagatePlayheadToPianoRolls(audioEngine.getCurrentPositionSeconds());
        }
        return;
    }
    toolbar.setPlaying(isPlaying);
}

void MainComponent::seekTo(double seconds)
{
    // 録音中はシーク禁止 (ルーラークリック / N・B / マーカージャンプ等の全経路)。
    // 録音の尺は「停止位置 − 開始位置」のタイムライン差分で、書き込みゲートも
    // タイムラインの連続前進を前提にしているため、シークするとテイクがずれる
    if (isRecording) return;

    const double t = juce::jmax(0.0, seconds);
    audioEngine.setPosition(t);
    restartRetrospectiveAt(t);
    playPosition = t;
    playStartPos = t;   // RTZ 基準も更新
    int bar1, beat1;
    appSettings.barAndBeatAtTime(t, bar1, beat1);
    toolbar.setTimePosition(t, bar1, beat1);
    timelineView.setPlayheadPosition(t);
    propagatePlayheadToPianoRolls(t);
}

void MainComponent::stopTransport()
{
    if (isRecording) stopRecording();
    if (recordingMgr.hasRetrospective())
        recordingMgr.stopRetrospective(false, audioEngine.getCurrentPositionSeconds());
    isPlaying = false;
    audioEngine.stop();
    toolbar.setPlaying(false);
    toolbar.setRecording(false);

    // RTZ: Stop時に再生開始位置へ戻る（設定ON時）
    if (appSettings.returnToStartOnStop)
    {
        audioEngine.setPosition(playStartPos);
        playPosition = playStartPos;
        int bar1, beat1;
        appSettings.barAndBeatAtTime(playStartPos, bar1, beat1);
        toolbar.setTimePosition(playStartPos, bar1, beat1);
        timelineView.setPlayheadPosition(playStartPos);
        propagatePlayheadToPianoRolls(playStartPos);
    }
    else
    {
        // RTZ OFF: 停止位置でピアノロール再生バーも揃える
        propagatePlayheadToPianoRolls(audioEngine.getCurrentPositionSeconds());
    }
}

void MainComponent::restartRetrospectiveAt(double t)
{
    // 録音中は対象外 (遡及キャプチャは録音開始で Punch From Retro へ移行済みか破棄済み)
    if (isRecording || !recordingMgr.hasRetrospective()) return;

    recordingMgr.stopRetrospective(false, t);   // 破棄 (ファイルも消える)

    if (!isPlaying || !appSettings.retrospectiveEnabled) return;
    for (int i = 0; i < trackManager.getTrackCount(); ++i)
    {
        auto* trk = trackManager.getTrack(i);
        if (trk != nullptr && trk->isRecArmed())
        {
            recordingMgr.startRetrospective(trk, t);
            break;
        }
    }
}

int MainComponent::countRecArmedTracks() const
{
    int n = 0;
    for (int i = 0; i < trackManager.getTrackCount(); ++i)
        if (auto* t = trackManager.getTrack(i); t != nullptr && t->isRecArmed())
            ++n;
    return n;
}

Track* MainComponent::findEmptyRecordTrack()
{
    auto recordable = [](const Track* t)
    { return t != nullptr && !t->isMidiTrack() && !t->isClickTrack(); };
    auto isEmpty = [](const Track* t)
    {
        if (t->getMidiClipCount() > 0) return false;
        for (int li = 0; li < t->getLaneCount(); ++li)
            if (auto* l = t->getLane(li); l != nullptr && !l->clips.empty())
                return false;   // クリップを持つ (= オケ等) は録音先にしない
        return true;
    };
    // 1) 選択中トラックが「空の録音可能オーディオ」なら最優先 (ユーザーの明示選択)
    if (selectedTrackIndex >= 0 && selectedTrackIndex < trackManager.getTrackCount())
        if (auto* s = trackManager.getTrack(selectedTrackIndex); recordable(s) && isEmpty(s))
            return s;
    // 2) それ以外は先頭の「空の録音可能オーディオ」
    for (int i = 0; i < trackManager.getTrackCount(); ++i)
        if (auto* t = trackManager.getTrack(i); recordable(t) && isEmpty(t))
            return t;
    return nullptr;
}

void MainComponent::promptRecordingSetup()
{
    juce::Component::SafePointer<MainComponent> self(this);

    if (auto* cand = findEmptyRecordTrack())
    {
        // 空の録音用トラックがある → アームして録音するか確認 (ワンタップ録音準備)
        Track* t = cand;
        juce::AlertWindow::showAsync(juce::MessageBoxOptions()
            .withIconType(juce::MessageBoxIconType::QuestionIcon)
            .withTitle(tr(u8"録音の準備"))
            .withMessage(tr(u8"このトラックを録音状態にして録音を開始しますか？")
                         + juce::String("\n\n") + t->getName())
            .withButton(tr(u8"録音する"))
            .withButton(tr(u8"キャンセル")),
            [self, t](int r)
            {
                if (r != 1 || self == nullptr) return;
                if (self->trackManager.indexOf(t) < 0) return;   // 表示中に消えた場合に備える
                t->setRecArmed(true);
                self->syncInputMonitorStateToEngine();
                self->trackHeaderPanel.refresh();
                self->startRecording();
            });
        return;
    }

    // 空の録音用トラックが無い (トラック未作成 / オケしか無い等) → 新規トラック追加をうながすだけ。
    // (自動でトラックを足して録音まで始めるのは大袈裟なので、案内に留めてユーザーに追加させる)
    juce::ignoreUnused(self);
    juce::AlertWindow::showMessageBoxAsync(
        juce::MessageBoxIconType::InfoIcon,
        tr(u8"録音の準備"),
        tr(u8"録音できる空のトラックがありません。\n新規トラックを追加してください。"));
}

void MainComponent::startRecording()
{
    if (isRecording) return;

    // Rec アーム済みトラックが無ければ、ここで案内する。
    // (従来は警告も無く「再生だけ」始まって何も録れず、完璧なテイクが残らない事故になっていた)
    if (countRecArmedTracks() == 0)
    {
        promptRecordingSetup();
        return;
    }

    // 録音開始位置 = 現在の再生位置
    const double recStart = audioEngine.getCurrentPositionSeconds();

    // カウントイン/プレロールは「停止状態から R を押したとき」のみ適用。
    // 再生中のパンチインでは巻き戻さずその場から書き出し開始。
    const bool fromStop  = !isPlaying;
    const int    cBars   = fromStop ? appSettings.countInBars : 0;
    const int    bpb     = juce::jmax(1, appSettings.meterNumerator);
    const double secsPerBeat = 60.0 / juce::jmax(1.0, bpm);
    const double countInDur = cBars * bpb * secsPerBeat;
    const double preRollDur = fromStop ? juce::jmax(0.0, appSettings.preRollSecs) : 0.0;

    const double playFrom   = juce::jmax(0.0, recStart - countInDur - preRollDur);

    // ループ録音条件: ループがアクティブで recStart が loopEnd より前なら自動でループ録音モード
    // （recStart < loopStart の場合も pre-loop 部分はスキップしてループ周回をテイクへ配置）
    const bool useLoopRec = loopActive
                            && (loopEndSecs > loopStartSecs + 0.05)
                            && recStart <  loopEndSecs   + 0.001;

    // ループ録音時は遡及録音を破棄して新規ファイルへ（Punch From Retro と排他）
    if (useLoopRec && recordingMgr.hasRetrospective())
        recordingMgr.stopRetrospective(false, recStart);
    // それ以外で遡及録音アクティブの場合は RecordingManager 側で Punch From Retro モードに
    // 切替わり、同じファイルを使い続けて停止時に offset 付きクリップで配置される。

    if (fromStop)
    {
        // RTZ の戻り先は R 押下位置 (recStart)。カウントイン/プリロールの頭 (playFrom) に
        // すると、停止のたびに再生バーがカウントイン分だけ手前へ戻ってしまう
        playStartPos = recStart;
        audioEngine.setPosition(playFrom);
        isPlaying = true;
        audioEngine.preparePlayback(trackManager);
        audioEngine.play();
        toolbar.setPlaying(true);
    }

    // ループラップカウントをリセット（録音開始時点を 0 として測定）
    audioEngine.resetLoopWrapCount();
    lastLoopWrapCount = 0;

    // Undo 用: 録音前の Lane 0 スナップショット (REC アーム済みトラック)。
    // 通常録音だけでなく Punch From Retro も同じトラックに新クリップを置くので対象に含める。
    preRecSnaps.clear();
    for (int i = 0; i < trackManager.getTrackCount(); ++i)
    {
        auto* t = trackManager.getTrack(i);
        if (!t || !t->isRecArmed()) continue;
        auto* lane = t->getLane(0);
        if (!lane) continue;
        PreRecSnapshot ps;
        ps.track = t;
        for (auto& c : lane->clips)
            ps.lane0Snap.push_back(EditActions::LaneSnapshotAction::ClipSnap::capture(c.get()));
        // テイクレーン (lane 1..) も控える: 録音で増えた/変わったレーンを停止時に特定する
        for (int li = 1; li < t->getLaneCount(); ++li)
        {
            std::vector<EditActions::LaneSnapshotAction::ClipSnap> laneSnap;
            if (auto* l = t->getLane(li))
                for (auto& c : l->clips)
                    laneSnap.push_back(EditActions::LaneSnapshotAction::ClipSnap::capture(c.get()));
            ps.takeLaneSnaps.push_back(std::move(laneSnap));
        }
        preRecSnaps.push_back(std::move(ps));
    }

    isRecording = recordingMgr.startRecording(recStart, playFrom,
                                              useLoopRec,
                                              loopStartSecs, loopEndSecs);
    lastRecordingWasLoop = isRecording && useLoopRec;
    toolbar.setRecording(isRecording);

    // 録音ファイルを作れなかったトラックがあれば通知する (silent failure 防止。
    // ディスク満杯・権限エラー等で一部または全トラックが録音できないケース)
    if (const auto& failed = recordingMgr.getLastStartFailures(); !failed.isEmpty())
    {
        juce::AlertWindow::showAsync(juce::MessageBoxOptions()
            .withIconType(juce::MessageBoxIconType::WarningIcon)
            .withTitle(tr(u8"録音を開始できないトラックがあります"))
            .withMessage(tr(u8"以下のトラックの録音ファイルを作成できませんでした。\nディスクの空き容量と書き込み権限を確認してください。\n\n")
                         + failed.joinIntoString("\n"))
            .withButton("OK"), nullptr);
    }

    if (!isRecording)
        preRecSnaps.clear();  // 録音が始まらなかった場合はスナップショットも破棄

    if (isRecording)
    {
        trackHeaderPanel.refresh();
        timelineView.refresh();
        if (fromStop)
            timelineView.setPlayheadPosition(playFrom);
    }
}

void MainComponent::stopRecording()
{
    if (!isRecording) return;

    double stopPos = audioEngine.getCurrentPositionSeconds();
    recordingMgr.stopRecording(stopPos);
    isRecording = false;
    markProjectDirty();   // 録音結果はプロジェクトの変更
    toolbar.setRecording(false);

    trackHeaderPanel.refresh();
    timelineView.refresh();
    scheduleWaveformRefresh();

    // Undo Action: 録音前後の Lane 0 とテイクレーンを LaneSnapshotAction で積む。
    // Undo すると録音前の状態に戻り、録音クリップは Lane から外れるが
    // WAV ファイル自体は Audio フォルダに残る (削除はしない)。
    // ・通常録音: Lane 0 + テイク退避を 1 トランザクション (Undo 1 回で丸ごと戻る)
    // ・ループ録音: 1 テイク = 1 トランザクション (Undo を押すたび新しいテイクから 1 つずつ
    //   戻る。複数トラック同時録音は同じ周回のテイクを 1 つに束ねる)
    if (!preRecSnaps.empty())
    {
        using ClipSnap = EditActions::LaneSnapshotAction::ClipSnap;
        undoManager.beginNewTransaction();
        auto onChange = [this]
        {
            markProjectDirty();
            trackHeaderPanel.refresh();
            timelineView.refresh();
            // LaneSnapshotAction の perform()/undo() は Lane クリップを作り直すため、
            // 作り直し後のクリップに対して波形の非同期ロードを仕掛け直す
            // (録音直後に波形が出ない / Undo で消える問題への対処)。
            scheduleWaveformRefresh();
            audioEngine.preparePlayback(trackManager);
        };
        auto deferSink = [this](std::vector<std::unique_ptr<AudioClip>>&& clips)
                         { audioEngine.deferClipDestruction(std::move(clips)); };
        auto snapsDiffer = [](const std::vector<ClipSnap>& a, const std::vector<ClipSnap>& b)
        {
            if (a.size() != b.size()) return true;
            for (size_t i = 0; i < a.size(); ++i)
                if (a[i].file != b[i].file
                 || std::abs(a[i].startPos   - b[i].startPos)   > 1e-6
                 || std::abs(a[i].duration   - b[i].duration)   > 1e-6
                 || std::abs(a[i].fileOffset - b[i].fileOffset) > 1e-6)
                    return true;
            return false;
        };

        // Lane 0 (パンチイン本体): 全トラックまとめて現在のトランザクションへ
        for (auto& ps : preRecSnaps)
        {
            if (!ps.track) continue;
            auto* lane = ps.track->getLane(0);
            if (!lane) continue;

            std::vector<ClipSnap> afterSnap;
            for (auto& c : lane->clips)
                afterSnap.push_back(ClipSnap::capture(c.get()));

            if (!snapsDiffer(ps.lane0Snap, afterSnap)) continue;  // 変化なしは積まない

            undoManager.perform(new EditActions::LaneSnapshotAction(
                lane, std::move(ps.lane0Snap), std::move(afterSnap),
                trackManager.getFormatManager(), trackManager.getThumbnailCache(),
                onChange, deferSink));
        }

        // テイクレーン (lane 1..): 録音で増えた/変わったレーンを収集 (レーン昇順 = テイクの
        // 時系列順)。ループ録音はテイク順にトランザクションを切り、通常録音は上の Lane 0 と
        // 同じトランザクションに含める
        struct LaneDiff
        {
            Lane* lane { nullptr };
            std::vector<ClipSnap> before, after;
        };
        std::vector<std::vector<LaneDiff>> perTrackDiffs;  // [トラック][テイク順]
        size_t maxTakes = 0;
        for (auto& ps : preRecSnaps)
        {
            if (!ps.track) continue;
            std::vector<LaneDiff> diffs;
            for (int li = 1; li < ps.track->getLaneCount(); ++li)
            {
                auto* lane = ps.track->getLane(li);
                if (!lane) continue;
                LaneDiff d;
                d.lane = lane;
                if ((size_t)(li - 1) < ps.takeLaneSnaps.size())
                    d.before = std::move(ps.takeLaneSnaps[(size_t)(li - 1)]);
                for (auto& c : lane->clips)
                    d.after.push_back(ClipSnap::capture(c.get()));
                if (snapsDiffer(d.before, d.after))
                    diffs.push_back(std::move(d));
            }
            maxTakes = juce::jmax(maxTakes, diffs.size());
            perTrackDiffs.push_back(std::move(diffs));
        }
        for (size_t t = 0; t < maxTakes; ++t)
        {
            if (lastRecordingWasLoop)
                undoManager.beginNewTransaction();
            for (auto& diffs : perTrackDiffs)
                if (t < diffs.size())
                    undoManager.perform(new EditActions::LaneSnapshotAction(
                        diffs[t].lane, std::move(diffs[t].before), std::move(diffs[t].after),
                        trackManager.getFormatManager(), trackManager.getThumbnailCache(),
                        onChange, deferSink));
        }
        preRecSnaps.clear();
    }
}

void MainComponent::scheduleWaveformRefresh()
{
    // AudioThumbnail はバックグラウンドで非同期にロードされ、進捗ごとに
    // ChangeBroadcaster::sendChangeMessage() を発火するので、その通知を購読する。
    // (ポーリング不要 → CPU/バッテリを浪費しない)
    trackManager.forEachAudioClip([&](AudioClip& c)
    {
        auto& thumb = c.getThumbnail();
        // 波形描画キャッシュ (Image) は常に破棄する。録音直後の退避クリップなどは、
        // 確定前の fileOffset (= 0) で一度キャッシュされた古い波形画像が残り、その後
        // 正しい fileOffset を入れても再描画されず「テイクだけ違う領域の波形が出る」
        // ことがある。ここで全クリップのキャッシュを捨てれば、次の repaint で必ず
        // 現在の fileOffset / gain で描き直される (再構築コストは録音停止/読込/Undo 等の
        // ユーザー操作時のみで、毎フレームではないため軽微)。
        c.invalidateWaveformCache();
        if (!thumb.isFullyLoaded())
        {
            // 重複登録を防ぐため一度外してから付け直す。
            // (AudioThumbnail 破棄時には ChangeBroadcaster::~ で自動解除される)
            thumb.removeChangeListener(this);
            thumb.addChangeListener(this);
        }
        return true;
    });
    // キャッシュを破棄したので必ず描き直す (ロード済みクリップも現在の fileOffset で再構築)。
    timelineView.repaint();

    updateWaveformLoadingStatus();
}

void MainComponent::updateWaveformLoadingStatus()
{
    int pending = 0, total = 0;
    trackManager.forEachAudioClip([&](AudioClip& c)
    {
        ++total;
        if (!c.getThumbnail().isFullyLoaded()) ++pending;
        return true;
    });

    statusBar.setWaveformProgress(pending, total);

    if (pending > 0)
    {
        waveformWasLoading = true;
    }
    else
    {
        if (waveformWasLoading)
        {
            // ロード中 → 全完了に切り替わった瞬間だけ完了メッセージを出す
            statusBar.setMessage(tr(u8"波形の読み込みが完了しました"), 3000);
            // 波形デコードが走って全クリップのサムネイルが揃った瞬間に、その完全な
            // キャッシュをディスクへ永続化する (次回読み込みが爆速になる)。
            // force=true で音声署名スキップを無効化する: 既存 thumbnails.bin が
            // 「デコード未完了のまま保存された空/部分キャッシュ」でも、署名が同じでも必ず
            // 上書きする (空キャッシュが署名スキップで永久に残り毎回再デコードになる不具合の修正)。
            // load / import / 録音 / undo いずれの完了でも、保存済みプロジェクトなら保存する。
            if (currentProjectFile.existsAsFile())
                trackManager.saveThumbnailCache(getProjectCacheFolder().getChildFile("thumbnails.bin"),
                                                getProjectAudioFolder(), /*force=*/true);
        }
        // 完了 (即時キャッシュヒット / デコード後どちらでも) でフラグをクリア。
        waveformWasLoading = false;
    }
}

void MainComponent::changeListenerCallback(juce::ChangeBroadcaster* src)
{
    // どのクリップのサムネイルか特定し、波形キャッシュを破棄して repaint。
    // ロード完了したらリスナーを外す。
    trackManager.forEachAudioClip([&](AudioClip& c)
    {
        if (&c.getThumbnail() != src) return true;  // 続行
        c.invalidateWaveformCache();
        // 全面 repaint をやめ、変わったクリップの時間範囲だけを部分 repaint する。
        // 全面だと画面内の巨大クリップ全体を drawClipWaveform→fillPath で描き直し、
        // デコード進捗ごと (数秒おき) に約 250ms メッセージスレッドが固まる (再生バーが一瞬停止)。
        timelineView.repaintTimeRange(c.getStartPosition(),
                                      c.getStartPosition() + c.getDuration());
        if (c.getThumbnail().isFullyLoaded())
            c.getThumbnail().removeChangeListener(this);
        // ロード残数の進捗を更新 (完了で「完了」メッセージ)
        updateWaveformLoadingStatus();
        return false;  // 見つけたので終了
    });
}

void MainComponent::toggleRecord()
{
    if (isRecording) { stopRecording(); return; }
    // 再生中でもパンチインで録音開始可能（R キー / REC ボタン共通）
    startRecording();
}

void MainComponent::commitRetrospective()
{
    if (!recordingMgr.hasRetrospective()) return;
    double endSec = audioEngine.getCurrentPositionSeconds();
    recordingMgr.stopRetrospective(true, endSec);
    trackHeaderPanel.refresh();
    timelineView.refresh();
    scheduleWaveformRefresh();   // 録音クリップの波形を非同期ロード完了で反映
    audioEngine.preparePlayback(trackManager);
}

// ── 自動保存 ─────────────────────────────────────────────────────────
void MainComponent::syncSnapLabelToSettings()
{
    // ラベル表は SnapMode の並び (AppSettings.h) と一致させること
    static const char* labels[] = {
        "Off", "1/1", "1/2", "1/4", "1/8", "1/16", "1/32",
        "1/4 T", "1/8 T", "1/16 T"
    };
    const int mode = (int) appSettings.snapMode;
    if (mode >= 0 && mode < (int)(sizeof(labels) / sizeof(labels[0])))
        toolbar.setSnapLabel(labels[mode], mode != 0);
}

void MainComponent::duplicateTrack(int sourceTrackIdx, bool includeTakeLanes)
{
    auto* src = trackManager.getTrack(sourceTrackIdx);
    if (!src || src->isClickTrack()) return;

    // クリップ・設定をコピー (戻り値はすぐ後ろに挿入された新しいトラック)
    auto* dst = trackManager.duplicateTrack(sourceTrackIdx, includeTakeLanes);
    if (!dst) return;

    // プラグインチェーンをクローンする:
    // 各プラグインの状態を取得し、PluginManager 経由で新インスタンスを生成。
    // (AudioPluginInstance は非コピーなので状態転送が唯一の手段)
    const double pluginSr = audioEngine.getSampleRate() > 0
                                ? audioEngine.getSampleRate() : 48000.0;
    const int    pluginBs = 512;
    auto& fmgr      = pluginManager.getFormatManager();
    auto& srcChain  = src->getPluginChain();
    auto& dstChain  = dst->getPluginChain();

    for (int i = 0; i < srcChain.getNumPlugins(); ++i)
    {
        auto* srcPlugin = srcChain.getPlugin(i);
        if (!srcPlugin) continue;  // 空スロットはスキップ

        const auto desc = srcPlugin->getPluginDescription();
        juce::String err;
        std::unique_ptr<juce::AudioPluginInstance> newInstance(
            fmgr.createPluginInstance(desc, pluginSr, pluginBs, err));
        if (!newInstance) continue;  // 生成失敗 (まれ) はそのスロットを空にして続行

        // 状態 (パラメータ・プリセット位置) を転送
        juce::MemoryBlock state;
        srcPlugin->getStateInformation(state);
        if (state.getSize() > 0)
            newInstance->setStateInformation(state.getData(), (int) state.getSize());

        dstChain.insertPluginAt(i, std::move(newInstance));
        if (srcChain.isBypassed(i))
            dstChain.setBypassed(i, true);
    }

    // 複製も「追加」として Undo 可能に (プラグインクローン後に積むので、undo→redo で
    // クローン済みインスタンスごと同一トラックが復帰する)
    pushTrackAddUndo(dst);

    // UI / オーディオエンジンに通知
    trackHeaderPanel.refresh();
    timelineView.refresh();
    audioEngine.invalidatePlayback();
    markProjectDirty();
}

void MainComponent::deleteTracks(std::vector<int> indices)
{
    // 重複排除 + 範囲外除去
    std::sort(indices.begin(), indices.end());
    indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
    indices.erase(std::remove_if(indices.begin(), indices.end(),
        [this](int i) { return i < 0 || i >= trackManager.getTrackCount(); }), indices.end());
    if (indices.empty()) return;

    // index は削除で変わるため Track* に解決してから Undo アクションへ渡す。
    // 削除は Undo 可能 (TrackDeleteAction) になったので、以前の「取り消せません」確認は廃止
    // (Cmd+Z で同一インスタンスが元の位置へ復帰する。クリップ削除と同じ作法)。
    std::vector<Track*> tracks;
    tracks.reserve(indices.size());
    for (int idx : indices)
        if (auto* t = trackManager.getTrack(idx)) tracks.push_back(t);
    if (tracks.empty()) return;

    pushTrackDeleteUndo(std::move(tracks));
}

// ───────────────────── 音楽情報 (マーカー/テンポ/拍子/曲BPM) の Undo ─────────────────────
bool MainComponent::MusicState::operator==(const MusicState& o) const
{
    if (initialBpm != o.initialBpm || meterNumerator != o.meterNumerator
        || meterDenominator != o.meterDenominator) return false;
    if (bpmChanges.size() != o.bpmChanges.size()) return false;
    for (size_t i = 0; i < bpmChanges.size(); ++i)
        if (bpmChanges[i].timeSec != o.bpmChanges[i].timeSec
            || bpmChanges[i].bpm != o.bpmChanges[i].bpm) return false;
    if (meterChanges.size() != o.meterChanges.size()) return false;
    for (size_t i = 0; i < meterChanges.size(); ++i)
        if (meterChanges[i].barIndex != o.meterChanges[i].barIndex
            || meterChanges[i].numerator != o.meterChanges[i].numerator
            || meterChanges[i].denominator != o.meterChanges[i].denominator) return false;
    if (markers.size() != o.markers.size()) return false;
    for (size_t i = 0; i < markers.size(); ++i)
        if (markers[i].time != o.markers[i].time
            || markers[i].name != o.markers[i].name
            || markers[i].colour != o.markers[i].colour) return false;
    return true;
}

MainComponent::MusicState MainComponent::captureMusicState() const
{
    MusicState s;
    s.initialBpm       = appSettings.initialBpm;
    s.meterNumerator   = appSettings.meterNumerator;
    s.meterDenominator = appSettings.meterDenominator;
    s.bpmChanges       = appSettings.bpmChanges;
    s.meterChanges     = appSettings.meterChanges;
    s.markers          = timelineView.getMarkers();
    return s;
}

void MainComponent::applyMusicState(const MusicState& s)
{
    appSettings.initialBpm       = s.initialBpm;
    appSettings.meterNumerator   = s.meterNumerator;
    appSettings.meterDenominator = s.meterDenominator;
    appSettings.bpmChanges       = s.bpmChanges;
    appSettings.meterChanges     = s.meterChanges;
    bpm = s.initialBpm;
    toolbar.setBpm(bpm);
    timelineView.setBpm(bpm);
    timelineView.setMarkers(s.markers);          // markers はルーラー保持
    timelineView.setAppSettings(appSettings);    // bpm/meter changes をルーラーへ + 再計算
    audioEngine.setMetronomeBpm(bpm);
    audioEngine.setMetronomeBeatsPerBar(appSettings.meterNumerator);
    audioEngine.setAppSettings(appSettings);
    markProjectDirty();
    // グリッド/マーカー/テンポ表示の再描画のみで十分 (トラック行は変わらない)。
    // refresh() を使うと add-marker 直後に開く名前エディタに影響しうるため repaint に留める。
    timelineView.repaint();
}

void MainComponent::pushMusicUndo(const MusicState& before)
{
    MusicState after = captureMusicState();
    if (before == after) return;   // 変化なし → Undo を積まない
    undoManager.beginNewTransaction();
    undoManager.perform(new EditActions::SnapshotAction<MusicState>(
        before, after, [this](const MusicState& s) { applyMusicState(s); }));
}

void MainComponent::pushMusicUndoFromLists(const TimelineRuler::EditLists& before,
                                           const TimelineRuler::EditLists& after)
{
    // ルーラー内編集は曲頭 BPM/拍子を変えないので、両者に現在値を入れる。
    MusicState b = captureMusicState();
    MusicState a = b;
    b.bpmChanges = before.bpmChanges; b.meterChanges = before.meterChanges; b.markers = before.markers;
    a.bpmChanges = after.bpmChanges;  a.meterChanges = after.meterChanges;  a.markers = after.markers;
    if (b == a) return;
    // ルーラー編集はライブ適用済み。undoManager.perform() が apply(a) を呼ぶので
    // ここで appSettings / dirty / 再描画が after に揃う (二重適用は避ける)。
    undoManager.beginNewTransaction();
    undoManager.perform(new EditActions::SnapshotAction<MusicState>(
        b, a, [this](const MusicState& s) { applyMusicState(s); }));
}

// ───────────────────── トラックのプロパティ (名前/色/シンセ) の Undo ─────────────────────
bool MainComponent::TrackState::operator==(const TrackState& o) const
{
    return name == o.name && colour == o.colour && synthEnabled == o.synthEnabled
        && synthWaveform == o.synthWaveform && octaveShift == o.octaveShift
        && semitoneTranspose == o.semitoneTranspose
        && volume == o.volume && pan == o.pan && reverbSend == o.reverbSend
        && muted == o.muted && soloed == o.soloed;
}

bool MainComponent::trackStillExists(Track* t) const
{
    if (!t) return false;
    for (int i = 0; i < trackManager.getTrackCount(); ++i)
        if (trackManager.getTrack(i) == t) return true;
    return false;
}

MainComponent::TrackState MainComponent::captureTrackState(Track* t) const
{
    TrackState s;
    if (!t) return s;
    s.name              = t->getName();
    s.colour            = t->getColour();
    s.synthEnabled      = t->isSynthEnabled();
    s.synthWaveform     = t->getSynthWaveform();
    s.octaveShift       = t->getOctaveShift();
    s.semitoneTranspose = t->getSemitoneTranspose();
    s.volume            = t->getVolume();
    s.pan               = t->getPan();
    s.reverbSend        = t->getReverbSend();
    s.muted             = t->isMuted();
    s.soloed            = t->isSoloed();
    return s;
}

void MainComponent::applyTrackEditUndoable(Track* t, std::function<void()> mutate, bool newTransaction)
{
    if (!t || !mutate) return;
    TrackState before = captureTrackState(t);
    mutate();
    TrackState after = captureTrackState(t);
    if (before == after) return;   // 実際の変化なし
    // newTransaction=false なら呼び出し側が開始したトランザクションへ積む (一括編集を 1 Undo に)。
    if (newTransaction) undoManager.beginNewTransaction();
    // 差分適用: このトランザクションで実際に変化したフィールドだけを undo/redo で書き戻す。
    // TrackState 全体を無条件に復元すると、Undo を通さない他経路 (ラウドネス調整 /
    // メトロノーム設定ダイアログ等) で変えた vol/pan/reverbSend を、無関係なトラック編集
    // (リネーム等) の undo が踏み越えて静かに巻き戻してしまう (しかも記録が無いので redo でも
    // 戻らない)。変化フィールドのみに限定してこの取りこぼしを防ぐ。
    undoManager.perform(new EditActions::SnapshotAction<TrackState>(
        before, after, [this, t, before, after](const TrackState& s)
        {
            if (!trackStillExists(t)) return;   // トラック削除済みなら no-op (ダングリング回避)
            if (before.name             != after.name)             t->setName(s.name);
            if (before.colour           != after.colour)           t->setColour(s.colour);
            if (before.synthEnabled     != after.synthEnabled)     t->setSynthEnabled(s.synthEnabled);
            if (before.synthWaveform    != after.synthWaveform)    t->setSynthWaveform(s.synthWaveform);
            if (before.octaveShift      != after.octaveShift)      t->setOctaveShift(s.octaveShift);
            if (before.semitoneTranspose!= after.semitoneTranspose)t->setSemitoneTranspose(s.semitoneTranspose);
            if (before.volume           != after.volume)           t->setVolume(s.volume);
            if (before.pan              != after.pan)              t->setPan(s.pan);
            if (before.reverbSend       != after.reverbSend)       t->setReverbSend(s.reverbSend);
            if (before.muted            != after.muted)            t->setMuted(s.muted);
            if (before.soloed           != after.soloed)           t->setSoloed(s.soloed);
            trackHeaderPanel.refresh();         // 名前/移調/波形/フェーダー等の表示を更新
            if (trackHeaderPanel.onTrackChanged) trackHeaderPanel.onTrackChanged();
            audioEngine.invalidatePlayback();   // 内蔵シンセ/ソロ等をエンジンへ反映
        }));
}

void MainComponent::applyToggleToSelectedTracks(int clickedIdx, bool value, bool solo)
{
    const int trackCount = trackManager.getTrackCount();
    // 対象 = ヘッダの複数選択集合 ∪ クリックされたトラック。境界外 index は弾く。
    const auto& sel = trackHeaderPanel.getSelectedTrackIndices();
    std::vector<int> scope;
    for (int i : sel)
        if (i >= 0 && i < trackCount) scope.push_back(i);
    if (clickedIdx >= 0 && clickedIdx < trackCount
        && std::find(scope.begin(), scope.end(), clickedIdx) == scope.end())
        scope.push_back(clickedIdx);
    if (scope.empty()) return;

    // 複数トラックの変更を 1 つの Undo にまとめる (先頭で 1 回だけトランザクション開始)。
    undoManager.beginNewTransaction();
    for (int i : scope)
        if (auto* t = trackManager.getTrack(i))
            applyTrackEditUndoable(t,
                [t, solo, value] { if (solo) t->setSoloed(value); else t->setMuted(value); },
                /*newTransaction=*/false);
}

bool MainComponent::computeLoudnessTargetVol(const juce::File& file, double fileOffset,
                                             double duration, float& outVolDb)
{
    // クリップのゲインは触らず、ファイル単体のラウドネスを測定 (gain=1.0)
    const double measuredLufs = LufsMeter::measureFileSegment(
        file, trackManager.getFormatManager(), fileOffset, duration, /*gain*/ 1.0f);
    if (!std::isfinite(measuredLufs)) return false;

    // 出力ラウドネス = measured + trackVol となるように trackVol を決定
    const double targetLufs = (double) appSettings.loudnessTargetLufs;
    outVolDb = (float) juce::jlimit(-60.0, 6.0, targetLufs - measuredLufs);
    return true;
}

void MainComponent::applyProjectSampleRateToDevice()
{
    auto& dm = audioEngine.getDeviceManager();
    auto setup = dm.getAudioDeviceSetup();
    const double targetSr = appSettings.projectSampleRate;

    // プロジェクト SR をデバイスへ要求する (既に一致していれば触らない)。デバイスを再設定すると
    // 一瞬途切れるため、不要な設定変更は避ける。失敗しても続行 — 下でデバイスが実際に動いている
    // SR を projectSampleRate に採用する。
    if (targetSr > 0.0 && std::abs(setup.sampleRate - targetSr) >= 0.5)
    {
        setup.sampleRate = targetSr;
        dm.setAudioDeviceSetup(setup, true);
    }

    // 【肝】プロジェクト SR を「デバイスが実際に動いている SR」に確定する。Windows (WASAPI 共有
    // モード等) では要求 SR を出せず別 SR のままになることがあるが、その場合でも
    // projectSampleRate == 実デバイス SR に揃えることで、録音 (デバイス SR) とインポート
    // (projectSampleRate へリサンプル) の不一致 = 音切れ/再生速度ずれを構造的に防ぐ。
    double actualSr = dm.getAudioDeviceSetup().sampleRate;
    if (auto* dev = dm.getCurrentAudioDevice())
        if (dev->getCurrentSampleRate() > 0.0) actualSr = dev->getCurrentSampleRate();

    if (actualSr > 0.0 && std::abs(actualSr - appSettings.projectSampleRate) >= 0.5)
    {
        appSettings.projectSampleRate = actualSr;
        audioEngine.setAppSettings(appSettings);
    }
    statusBar.setSampleRate((int) appSettings.projectSampleRate);
}

void MainComponent::mouseDown(const juce::MouseEvent&)
{
    // どこをクリックしてもキーボードフォーカスをメインに戻す
    grabKeyboardFocus();
}

void MainComponent::paint(juce::Graphics& g)
{
    g.fillAll(AppColours::background);
}

void MainComponent::resized()
{
    auto b = getLocalBounds();
   #if ! JUCE_MAC
    if (menuBar) menuBar->setBounds(b.removeFromTop(24));
   #endif
    toolbar.setBounds(b.removeFromTop(toolbarHeight));
    statusBar.setBounds(b.removeFromBottom(statusBarHeight));
    const int mpW = masterPanel.isCollapsed() ? MasterPanel::kCollapsedWidth : masterPanelWidth;
    masterPanel.setBounds(b.removeFromRight(mpW));

    // いずれかのトラックで INS スロットが表示されていればトラックヘッダー列を広げる
    bool anyInsVisible = false;
    for (int i = 0; i < trackManager.getTrackCount(); ++i)
        if (auto* t = trackManager.getTrack(i); t && t->isInsertSlotsVisible())
        { anyInsVisible = true; break; }
    const int headerW = trackHeaderWidth + (anyInsVisible ? Track::insertAreaWidth : 0);

    trackHeaderPanel.setBounds(b.removeFromLeft(headerW));
    timelineView.setBounds(b);
}
