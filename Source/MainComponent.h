// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

#pragma once
#include <JuceHeader.h>
#include "Audio/AudioEngine.h"
#include "Audio/AudioFileImporter.h"
#include "Audio/AudioDeviceSettings.h"
#include "VST/PluginManager.h"
#include "Mac/GlobalKeyMonitor.h"
#include "Tracks/TrackManager.h"
#include "Recording/RecordingManager.h"
#include "Edit/EditActions.h"
#include "Project/ProjectManager.h"
#include "Project/AppPreferences.h"
#include "Export/ExportEngine.h"   // runExport の ExportEngine::Options 引数のため
#include "UI/Toolbar.h"
#include "UI/TrackHeaderPanel.h"
#include "UI/TimelineView.h"
#include "UI/MasterPanel.h"
#include "UI/StatusBar.h"
#include "UI/PianoRollEditor.h"  // MainComponent::PianoRollWindow が unique_ptr<PianoRollEditor> を保持
#include "UI/LyricsView.h"       // MainComponent::LyricsWindow が unique_ptr<LyricsView> を保持
#include "AppSettings.h"

class MainComponent : public juce::Component,
                      public  juce::DragAndDropContainer,    // FX チップ D&D の親
                      public  juce::MenuBarModel,
                      public  juce::ApplicationCommandTarget,  // プラグインエディタ越しでもショートカットを効かせる
                      private juce::Timer,
                      public  juce::KeyListener,
                      public  juce::ChangeListener   // AudioThumbnail のロード進捗通知用
{
public:
    MainComponent();
    ~MainComponent() override;

    // MenuBarModel
    juce::StringArray getMenuBarNames() override;
    juce::PopupMenu   getMenuForIndex(int topLevelMenuIndex, const juce::String& menuName) override;
    void              menuItemSelected(int menuItemID, int topLevelMenuIndex) override;

    // ChangeListener (AudioThumbnail からのロード進捗通知)
    void changeListenerCallback(juce::ChangeBroadcaster* src) override;

    // 起動画面に戻すなど、ホストウィンドウへ通知するコールバック
    std::function<void()> onCloseProject;
    std::function<void()> onNewProject;

    void paint(juce::Graphics&) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent&) override;
    bool keyPressed(const juce::KeyPress&, juce::Component*) override;
    bool keyPressed(const juce::KeyPress& key) override
    {
        return keyPressed(key, this);
    }

    // ApplicationCommandTarget
    enum CommandIDs
    {
        cmdPlayPause   = 0x6001,
        cmdStop        = 0x6002,
        cmdRecord      = 0x6003,
        cmdExport      = 0x6004,
        cmdSave        = 0x6005,
        cmdOpen        = 0x6006,
    };
    juce::ApplicationCommandTarget* getNextCommandTarget() override { return nullptr; }
    void getAllCommands(juce::Array<juce::CommandID>& commands) override;
    void getCommandInfo(juce::CommandID commandID, juce::ApplicationCommandInfo& info) override;
    bool perform(const InvocationInfo& info) override;

    juce::ApplicationCommandManager& getCommandManager() { return commandManager; }

    // 起動画面から呼ぶ：新規プロジェクトを生成し、現在のセッションを置き換える
    bool createNewProject(const juce::File& projectFile, double sampleRate, int bitDepth);
    // 起動画面から呼ぶ：既存プロジェクトを読み込み、現在のセッションを置き換える
    bool openExistingProject(const juce::File& projectFile);

    // ウィンドウ × ボタン等から呼ぶ: 未保存変更があれば確認ダイアログを出し、
    // Save / Don't Save の後に onProceed を呼ぶ。Cancel なら何もしない。
    void confirmCloseIfDirty(std::function<void()> onProceed);

private:
    static constexpr int toolbarHeight    { 56 };
    static constexpr int statusBarHeight  { 22 };
    static constexpr int trackHeaderWidth { 240 };
    static constexpr int masterPanelWidth { 155 };

    void timerCallback() override;

    void addTrack();
    void addMidiTrack();   // 空の MIDI トラックを作成 (ハモリ/ガイド打ち込み用)
    void addFolderTrack(); // フォルダトラック (グループバス) を作成 (環境設定で有効化)
    // 右クリック「フォルダへ移動 (folderIdx) / フォルダから出す (-1)」(FolderAssignAction で Undo 可)
    void moveTrackToFolder(int trackIdx, int folderIdx);
    // 入力モニター/Rec アーム/モニターリバーブ量をまとめてオーディオエンジンへ反映する。
    void syncInputMonitorStateToEngine();
    // CLICK トラックの Vol/Pan/Mute/Sound/Accent/Rate をメトロノームへ反映し、CLICK ボタンの点灯も
    // 同期する。CLICK トラックが無ければ何もしない (CLICK ボタンでの独立トグルを尊重する)。
    // 追加 / 変更 / 削除 / プロジェクト読込の各所から呼ぶ。
    void syncClickTrackToEngine();
    // 開いているメトロノーム設定ダイアログへ現在値を反映する更新関数 (ダイアログ生成時に設定)。
    // CLICK トラックのフェーダー変更がダイアログにも追従するための経路。引数は
    // (線形ボリューム, パン, 音色 index, アクセント)。SafePointer 越しなので閉じていれば no-op。
    std::function<void(float, float, int, bool)> metronomeDlgPull;
    void togglePlay();
    void stopTransport();
    // 再生位置を seconds へ移動し、エンジン / ツールバー / タイムライン / ピアノロールを同期する。
    // (N/B の小節移動、マーカー移動、ルーラークリック等の共通処理)
    void seekTo(double seconds);
    // 再生中の遡及キャプチャ (バックグラウンド録音) を破棄して位置 t から取り直す。
    // 遡及ファイルは「サンプル 0 = キャプチャ開始位置からの連続再生」前提でタイムラインへ
    // 対応付けるため、シークすると対応が壊れ、その後のパンチイン (Punch From Retro) や
    // Cmd+Shift+R の確定クリップが移動前の位置基準で置かれてずれる。シーク時に必ず呼ぶ。
    void restartRetrospectiveAt(double t);
    void startRecording();
    // takesOnly=true は録音をテイクレーンだけに確定する (Lane 0 に置かない)。
    // Q リテイクの「テイクを残す」設定 (AppPreferences::retakeKeepsTake) 専用
    void stopRecording(bool takesOnly = false);
    void toggleRecord();
    // リテイク (Q): 録音中のみ、今のテイクを破棄 (設定 ON ならテイクレーンへ確定) して
    // 録音開始位置 (R 押下位置) へ戻り、すぐ録音をやり直す。
    // 録音中でなければ何もしない (Q で通常の録音開始はさせない)。
    void retakeRecording();
    // 録音可能 (Rec アーム済み) トラックが無いまま R を押したときの案内。
    // 空の録音用トラックがあればアームして録音、無ければ追加して録音するか確認する
    // (オケ等クリップを持つトラックには誤って重ねない)。詳細は MainComponent.cpp。
    void promptRecordingSetup();
    int  countRecArmedTracks() const;
    // 録音に使える「空のオーディオトラック」を返す (選択中の空トラック優先 / 無ければ nullptr)。
    Track* findEmptyRecordTrack();
    void showAudioSettings();
    void showImportDialog();
    void showImportMidiDialog();
    // ツールバーの IMPORT ボタン用: オーディオ / MIDI を 1 つのピッカーで選び拡張子で振り分ける
    void showImportAnyDialog();
    // 選択済みオーディオファイルを変換 (進捗付き) してプロジェクト先頭へ配置する (Cmd+I / IMPORT 共用)
    void placeImportedAudioFiles(const juce::Array<juce::File>& audioFiles);
    // D&D 用: ファイルとドロップ位置を指定して MIDI 取り込みダイアログを表示
    void importMidiFromFile(const juce::File& midiFile, double dropTimeOverride);
    void showPreferences();
    void showExportDialog();
    // 書き出し本体 (ジョブ構築 → モーダル ExportTask → 完了処理)。showExportDialog の onExport
    // ラムダから委譲する。ラムダ内でやると exitModalState 後の runThread (入れ子モーダル) 中に
    // onExport クロージャ (ExportDialog 所有) が破棄され captured this が dangling になるため、
    // this を明示レシーバに取れるメンバ関数へ分離している。
    void runExport(const ExportEngine::Options& optionsIn);
    void showMidiExportDialog();   // MIDI (SMF) を書き出す。設定でメニュー表示を ON にした時のみ使用
    void showPluginManager();
    void showAboutDialog();
    void showShortcutsDialog();
    void showDocumentation();      // 同梱の使い方ドキュメント (HTML) を既定ブラウザで開く
    void addPluginToTrack(int trackIdx, int slotIdx = -1);
    void openPluginEditor(int trackIdx, int slotIdx);
    void closePluginEditorFor(juce::AudioPluginInstance* plugin);
    void removePluginFromTrack(int trackIdx, int slotIdx);
    // D&D: 他トラックからのドロップ。move (copy=false) または copy (copy=true)
    // srcTrack == -1 はマスターを指す
    void handlePluginDropAcrossTracks(int srcTrack, int srcSlot,
                                      int dstTrack, int dstSlot, bool copy);

    // マスターチェーン操作
    void addPluginToMaster(int slotIdx);
    void openMasterPluginEditor(int slotIdx);
    void removeMasterPlugin(int slotIdx);
    void handlePluginDropFromTrackToMaster(int srcTrack, int srcSlot,
                                           int dstSlot, bool copy);

    // プラグインを正しく破棄するための自己管理ウィンドウ
    // (OwnedArray の解体には完全型が必要なため、ヘッダ内に定義を置く)
    class PluginEditorWindow : public juce::DocumentWindow
    {
    public:
        PluginEditorWindow(juce::AudioPluginInstance& p,
                           std::function<void(PluginEditorWindow*)> onCloseCb);
        ~PluginEditorWindow() override;
        void closeButtonPressed() override;
        juce::AudioPluginInstance* getPlugin() const { return &plugin; }
    private:
        // ウィンドウのリサイズ制約をプラグインエディタ側の constrainer へ委譲し、タイトルバー等の
        // 枠分を加味する (JUCE 公式 AudioPluginHost と同じ作法・Melodyne 等の可変エディタ用)。
        class DecoratorConstrainer : public juce::BorderedComponentBoundsConstrainer
        {
        public:
            explicit DecoratorConstrainer(juce::DocumentWindow& w) : window(w) {}
            juce::ComponentBoundsConstrainer* getWrappedConstrainer() const override
            {
                auto* ed = dynamic_cast<juce::AudioProcessorEditor*>(window.getContentComponent());
                return ed != nullptr ? ed->getConstrainer() : nullptr;
            }
            juce::BorderSize<int> getAdditionalBorder() const override
            {
                juce::BorderSize<int> frame;
                if (auto* peer = window.getPeer())
                    if (const auto f = peer->getFrameSizeIfPresent())
                        frame = *f;
                return frame.addedTo(window.getContentComponentBorder());
            }
        private:
            juce::DocumentWindow& window;
        };

        juce::AudioPluginInstance& plugin;
        juce::AudioProcessorEditor* editor { nullptr };
        DecoratorConstrainer constrainer { *this };
        std::function<void(PluginEditorWindow*)> onClose;
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PluginEditorWindow)
    };
    juce::OwnedArray<PluginEditorWindow> pluginEditorWindows;

    // ピアノロール 自己管理ウィンドウ
    class PianoRollWindow : public juce::DocumentWindow
    {
    public:
        PianoRollWindow(MidiClip& mc, Track& tr, double bpm,
                        double initialFocusTimeSec,
                        std::function<void(PianoRollWindow*)> onCloseCb,
                        std::function<void()> onChangedCb,
                        std::function<void(int, float, bool)> onPreviewCb,
                        std::function<void(double /*secsInClip*/)> onSeekCb);
        ~PianoRollWindow() override;
        void closeButtonPressed() override;
        MidiClip* getClip() const { return clipPtr; }
        PianoRollEditor* getEditor() const { return editor.get(); }
    private:
        MidiClip* clipPtr { nullptr };
        std::unique_ptr<PianoRollEditor> editor;
        std::function<void(PianoRollWindow*)> onClose;
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PianoRollWindow)
    };
    juce::OwnedArray<PianoRollWindow> pianoRollWindows;

    // 歌詞表示 自己管理ウィンドウ (常に最前面)。歌唱中に歌詞をコピペして読む。
    // テキストはプロジェクト (.uta) の AppSettings.lyrics に保存する。
    class LyricsWindow : public juce::DocumentWindow
    {
    public:
        LyricsWindow(const juce::String& initialText, int fontSize,
                     std::function<void(LyricsWindow*)> onCloseCb,
                     std::function<void(const juce::String&)> onTextChangedCb,
                     std::function<void(int)> onFontSizeChangedCb);
        ~LyricsWindow() override;
        void closeButtonPressed() override;
        LyricsView* getView() const { return view.get(); }
    private:
        std::unique_ptr<LyricsView> view;
        std::function<void(LyricsWindow*)> onClose;
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LyricsWindow)
    };
    std::unique_ptr<LyricsWindow> lyricsWindow;
    void openLyricsWindow();
    void toggleLyricsWindow();   // 開いていれば閉じる / 閉じていれば開く (ステータスバー用)

    // ショートカット一覧は非モーダルの窓で保持し、ステータスバー再クリックでトグルできる
    std::unique_ptr<juce::DialogWindow> shortcutsWindow;

    void openPianoRollFor(class MidiClip* clip, class Track* track);
    void propagatePlayheadToPianoRolls(double playheadSecs);
    // 開いている全ピアノロールへ自動ページング設定 (appPrefs.midiPagingEnabled) を反映
    void applyMidiPagingToOpenEditors();
    // appPrefs.showTooltips に応じて TooltipWindow を生成 / 破棄する (OFF で説明を出さない)
    void applyTooltipVisibility();
    void commitRetrospective();
    // ── オーディオインポート ──
    // 変換本体 (ファイル I/O のみ・UI/trackManager に触れない)。バックグラウンドスレッドから
    // 呼ばれる。onProgress は 0..1 の進捗を報告し false で中断する。
    juce::File doImportConversion(const juce::File& src,
                                  const std::function<bool(double)>& onProgress);

    // 複数ファイルをまとめて変換した結果 (UI スレッドでクリップ化する)。
    struct ImportedItem
    {
        juce::String name;       // トラック名に使う元ファイル名 (拡張子なし)
        juce::File   file;       // Audio/ 内の変換後ファイル
        double       dur    { 0.0 };
        bool         stereo { false };
        bool         hasVol { false };   // ラウドネス測定で track vol を決めたか
        float        volDb  { 0.0f };
    };
    // sources を順に変換 (リサンプル / メタ除去 / コピー + 尺・ch + ラウドネス測定)。
    // report(overall 0..1, label) は進捗報告で false を返すと中断する。ファイル I/O のみで
    // UI / trackManager のミューテーションはしないため、バックグラウンドスレッドから呼べる。
    std::vector<ImportedItem> convertImportSources(
        const juce::Array<juce::File>& sources,
        const std::function<bool(double, const juce::String&)>& report);
    // 進捗ウィンドウ「インポート中…」付きで sources をまとめて変換する (合計が小さければ窓を出さない)。
    // メッセージスレッドから呼ぶこと (モーダル窓を出すため)。
    std::vector<ImportedItem> convertImportSourcesWithProgress(const juce::Array<juce::File>& sources);
    // ドラッグ&ドロップ経路: 変換 → dropTime / targetTrackIdx に従ってクリップ配置 +
    // ラウドネス適用 + リフレッシュ。複数ドロップも 1 つの進捗ウィンドウにまとまる。
    void importAudioFilesAtDrop(const juce::Array<juce::File>& sources,
                                double dropTime, int targetTrackIdx);
    // クリップを全トラックより下の空き領域へドロップした時: 新規トラックを作って移す。
    // TrackAdd + ClipDelete + ClipAdd を 1 トランザクションに積み Cmd+Z 1 回で戻す。
    void moveClipToNewTrack(const EditActions::ClipParams& p, bool stereo,
                            Lane* srcLane, AudioClip* srcClip);
    // ファイル単体のラウドネスを測定し、target に合わせる track vol(dB) を outVolDb に返す。
    // 測定不能なら false (autoNormalizeOnImport の判定は呼び出し側)。
    bool computeLoudnessTargetVol(const juce::File& file, double fileOffset,
                                  double duration, float& outVolDb);

    // AudioThumbnail はバックグラウンドで非同期に読み込まれるため、
    // 録音停止直後 / プロジェクト読み込み直後の repaint では波形が空状態になる。
    // 各クリップの波形キャッシュを破棄しつつ、複数回遅延 repaint して
    // 読み込み完了を反映させる。
    void scheduleWaveformRefresh();

    // 波形 (AudioThumbnail) のロード残数を数えてステータスバーに進捗表示する。
    // 全クリップがロード完了したら「完了」メッセージを出す。
    void updateWaveformLoadingStatus();
    bool waveformWasLoading { false };

    // appSettings.projectSampleRate をオーディオデバイスに適用
    // （プロジェクト作成 / 読み込み時に呼び、デバイスをプロジェクトに追従させる）
    void applyProjectSampleRateToDevice();


    // トラックを複製 (右クリックメニュー「トラックを複製」から呼ばれる)。
    // TrackManager::duplicateTrack でクリップ/設定をコピーし、
    // この関数でプラグインチェーンをクローンする。
    // ツールバーの GRID 表示を appSettings.snapMode に同期する (ラベル + 点灯)。
    // プロジェクト読込後にも呼ぶ (スナップは効くのに表示が Off のままになるのを防ぐ)
    void syncSnapLabelToSettings();
    // includeTakeLanes=false (Option 押下複製) なら Lane 0 のみ複製しテイクリストは複製しない。
    void duplicateTrack(int sourceTrackIdx, bool includeTakeLanes = true);
    // 「+」ボタンで新規トラックを追加する位置: 選択中トラックがあればその直後 (最大選択 index)、
    // 無ければ -1 (= 末尾) を返す。
    int newTrackInsertAfter() const;
    // トラック追加/複製を Undo 可能にする (t は追加済みであること。最初の perform は no-op)。
    // newTransaction=false で現在のトランザクションに積む (MIDI インポートのように複数
    // トラック + テンポ/拍子を 1 回の Undo で戻したい場合)。
    // 実装は MainComponent_Plugins.cpp — undo 時にプラグインエディタ / ピアノロールを
    // 閉じるため完全型が必要
    void pushTrackAddUndo(Track* t, bool newTransaction = true);
    // トラック並べ替えの Undo を履歴へ積む (並べ替え自体は TrackHeaderPanel が実施済み)。
    // selected = 移動したトラック群。undo/redo 後に identity で選択を貼り直し選択状態を維持する。
    void pushTrackReorderUndo(std::vector<Track*> before, std::vector<Track*> after,
                              std::vector<Track*> selected);
    // D&D でフォルダ所属が変わった並べ替えの Undo を積む (FolderAssignAction = 並び + 所属を
    // 1 アクションで往復。適用自体は TrackHeaderPanel::performReorder が実施済み = 初回 perform は冪等)
    void pushTrackFolderDropUndo(std::vector<Track*> before, std::vector<Track*> after,
                                 std::vector<Track*> moved,
                                 std::vector<std::pair<Track*, Track*>> parentBefore,
                                 Track* afterParent);
    // トラック削除を Undo 履歴へ積んで実行する (TrackDeleteAction)。延命所有なので Cmd+Z で
    // 同一インスタンス (プラグイン / クリップ / テイク含む) が元の位置へ復帰する。
    // newTransaction=false は「フォルダ削除時の子の所属解除」など同一トランザクションに続けて積む用
    void pushTrackDeleteUndo(std::vector<Track*> tracks, bool newTransaction = true);

    // 指定 index 群のトラックをまとめて削除する (Undo 可能・プラグイン後処理 + 再生スナップショット更新)。
    void deleteTracks(std::vector<int> indices);

    // ── 音楽情報 (マーカー / テンポ変更 / 拍子変更 / 曲全体 BPM) の Undo ──
    // これらは appSettings とルーラーに分散して保持されるが、Undo では値スナップショットを
    // まとめて差し替える (生ポインタを持たないのでトラック削除等でダングリングしない)。
    struct MusicState
    {
        double initialBpm       { 120.0 };
        int    meterNumerator   { 4 };
        int    meterDenominator { 4 };
        std::vector<BpmChange>   bpmChanges;
        std::vector<MeterChange> meterChanges;
        std::vector<Marker>      markers;
        bool operator==(const MusicState& o) const;
        bool operator!=(const MusicState& o) const { return !(*this == o); }
    };
    MusicState captureMusicState() const;
    void       applyMusicState(const MusicState&);
    void       pushMusicUndo(const MusicState& before);  // after=capture; 差があれば Undo 登録
    // ルーラー内編集 (マーカー/テンポ/拍子の削除・ドラッグ・色変更) の before/after 3 リストから
    // Undo を登録する (曲頭 BPM/拍子は変わらないので現在値を使う)。
    void       pushMusicUndoFromLists(const TimelineRuler::EditLists& before,
                                      const TimelineRuler::EditLists& after);

    // ── トラックのプロパティ (名前 / 色 / 内蔵シンセ設定) の Undo ──
    // mutate を実行し、その前後の TrackState をスナップショットして Undo 登録する。
    // Track* は apply 時に trackManager 内の生存を確認してから書き戻す (削除済みなら no-op)。
    struct TrackState
    {
        juce::String name;
        juce::Colour colour { juce::Colour(0xff3a6ea5) };
        bool synthEnabled      { false };
        int  synthWaveform     { 0 };
        int  octaveShift       { 0 };
        int  semitoneTranspose { 0 };
        float volume     { 0.0f };
        float pan        { 0.0f };
        float reverbSend { 0.0f };
        bool  muted  { false };
        bool  soloed { false };
        bool operator==(const TrackState& o) const;
        bool operator!=(const TrackState& o) const { return !(*this == o); }
    };
    TrackState captureTrackState(Track*) const;
    // newTransaction=false は呼び出し側が先に beginNewTransaction() を 1 回呼んだ前提で、
    // 複数トラックの編集を 1 つの Undo にまとめる一括用 (Shift+Option+S/M 等)。
    void       applyTrackEditUndoable(Track*, std::function<void()> mutate, bool newTransaction = true);
    // Shift+Option+クリックの一括ソロ/ミュート: 選択集合 (∪ clickedIdx) を value にそろえて 1 Undo。
    void       applyToggleToSelectedTracks(int clickedIdx, bool value, bool solo);
    bool       trackStillExists(Track*) const;
    // ── プラグインチェーン操作の Undo (追加 / 削除 / バイパス / 並べ替え / トラック間移動) ──
    // チェーンは Track* (nullptr=マスター) を apply 時に解決する。削除済みトラックなら no-op。
    // (チェーン変更は onChainChanged で UI が自動更新されるため、onChange は markProjectDirty のみ)
    PluginChain* resolveChainForUndo(Track* track);  // track==nullptr → マスター。削除済みは nullptr
    std::function<PluginChain*()> makeChainResolver(Track* track);  // apply 時に解決するクロージャ
    void swapTrackPluginsUndoable(int trackIdx, int a, int b);
    void swapMasterPluginsUndoable(int a, int b);
    void togglePluginBypassUndoable(int trackIdx, int slot);  // trackIdx<0 → マスター

    // プロジェクト管理
    void saveProject();        // 既存パスがあればそこへ、無ければ saveAs
    void saveProjectAs(std::function<void(bool savedOk)> onDone = {});  // 完了コールバック付き
    void openProject();        // ファイルチョーザでファイルを選択
    // announce=true のときだけ保存成功をステータスバーに表示する (明示保存のみ。
    // クラッシュ復旧・新規作成の内部保存では false にして誤った通知を出さない)
    bool saveProjectTo(const juce::File& f, bool announce = true);
    // 未保存変更の追跡
    void markProjectDirty()  { projectDirty = true; }
    void clearProjectDirty() { projectDirty = false; }

    // 自動保存
    void   performAutoSave();
    void   restartAutoSaveTimer();
    void   offerAutoSaveRecoveryIfNeeded(const juce::File& projectFile);
    // 世代バックアップ: 命名/列挙/間引き/復旧判定は BackupManager (Source/Project/BackupManager.h)
    // に集約 (オーディオ/GUI 非依存・単体テスト可能)。ここはフォルダ文脈を渡す薄いラッパのみ。
    juce::File   getBackupDir() const;     // <project>/Backup を作成して返す ({}=不可)
    juce::String backupBaseName() const;   // 保存済み=名前(拡張子なし) / 未保存(Untitled)=空
    struct AutoSaveTickTimer : public juce::Timer
    {
        std::function<void()> tick;
        void timerCallback() override { if (tick) tick(); }
    };
    std::unique_ptr<AutoSaveTickTimer> autoSaveTimer;

    bool loadProjectFrom(const juce::File& f, bool isRecovery = false);
    juce::File currentProjectFile;
    juce::File untitledProjectDir;  // 未保存時の仮プロジェクトフォルダ

    // プロジェクトフォルダ取得（未保存なら untitled、保存済みなら current の親）
    juce::File getProjectFolder() const;
    juce::File getProjectAudioFolder() const;
    juce::File getProjectCacheFolder() const;
    juce::File getProjectExportFolder() const;
    // 全クリップの音声ファイルを新プロジェクトの Audio フォルダへ移動
    void migrateAudioToProjectFolder(const juce::File& newProjectFile);

    std::unique_ptr<juce::FileChooser> fileChooser;

    AudioEngine       audioEngine;
    TrackManager      trackManager  { audioEngine.getFormatManager() };
    RecordingManager  recordingMgr  { audioEngine, trackManager,
                                      audioEngine.getFormatManager() };
    AudioFileImporter fileImporter  { audioEngine.getFormatManager() };
    std::unique_ptr<AudioDeviceSettings::AutoSaver> deviceAutoSaver;
    PluginManager     pluginManager;
    juce::ApplicationCommandManager commandManager;
    std::unique_ptr<GlobalKeyMonitor> globalKeyMonitor;

   #if ! JUCE_MAC
    // Windows / Linux ではウィンドウ内に MenuBarComponent を表示する
    // (macOS は setMacMainMenu で OS のグローバルメニューバーに統合される)
    std::unique_ptr<juce::MenuBarComponent> menuBar;
   #endif

    TransportBar     toolbar;
    TrackHeaderPanel trackHeaderPanel { trackManager };
    TimelineView     timelineView     { trackManager };
    MasterPanel      masterPanel;
    StatusBar        statusBar;
    // ホバー時のツールチップ表示 (INS トグル等のアイコンボタンの説明)。
    // これを 1 つ置くだけでアプリ全体の setTooltip が機能する。appPrefs.showTooltips が
    // OFF のときは生成しない (= 説明を出さない)。applyTooltipVisibility() で生成/破棄を切替。
    std::unique_ptr<juce::TooltipWindow> tooltipWindow;

    bool        isPlaying         { false };
    bool        isRecording       { false };
    double      bpm               { 120.0 };
    double      playPosition      { 0.0 };
    double      playStartPos      { 0.0 };
    int         selectedTrackIndex { -1 };
    AppSettings appSettings;
    // アプリ全体設定 (プロジェクト非依存)。MIDI 書き出しメニューの表示可否などを保持
    AppPreferences appPrefs { AppPreferences::load() };
    juce::UndoManager undoManager;
    bool projectDirty { false };   // 未保存変更があるかどうか
    // 書き出し (ExportTask::runThread のモーダル) 実行中フラグ。モーダル中もメッセージループは
    // 回るため、GlobalKeyMonitor (NSEvent ローカルモニタ) の Space/R や macOS メインメニューの
    // ショートカットが素通りして transport / Undo / プロジェクト遷移を叩ける。書き出しスレッドと
    // 同一チェーンの並行処理 (書き出し破損) やトラック破棄 (bouncedChains の生ポインタ UAF) に
    // なるため、このフラグでキー/メニュー経路を全て無視する (進捗ウィンドウのキャンセルは
    // モーダル側の操作なので影響しない)
    bool exportInProgress { false };
    double loopStartSecs { 0.0 };
    double loopEndSecs   { 0.0 };
    bool   loopActive    { false };
    int    lastLoopWrapCount { 0 };

    // アイドル時にメーター計算をスキップするためのカウンタ (猶予 tick を数える)
    int    idleMeterTicks { 0 };

    // 再生バー平滑化: 視覚プレイヘッドは VBlankAttachment (onVBlank) でディスプレイの
    // リフレッシュに同期して更新し、提示時刻タイムスタンプで等速補間する。これにより
    // タイマー/オーディオブロック境界のジッタ由来のカクツキ (特に横ズーム時) を除去し、
    // 60/120Hz どちらのパネルでもフレームを無駄にせず最も滑らかに動かす。
    bool   playheadSmoothActive { false };
    double smoothedPlayhead     { 0.0 };
    double lastPlayheadWallSec  { 0.0 };
    // ループラップの実検出 (onVBlank): レイテンシ補正の「ループ末尾へ巻き戻し表示」は
    // 実際にラップした直後のレイテンシ窓でだけ行う。位置だけで判定すると、ループ範囲へ
    // 線形に入った直後 (視覚位置がまだ loopStart 手前) も誤ってループ末尾に表示される
    double lastRawPlayheadPos   { 0.0 };
    double lastLoopWrapWallSec  { -1.0e9 };
    juce::VBlankAttachment vblankAttachment;
    void   onVBlank (double timestampSec);  // ディスプレイ垂直同期に合わせた再生バー更新

    // 再生バーの連続スクラブ ( ; = 進む / − = 戻る、停止中・押している間 等速でヌルヌル)。
    // onVBlank がキー状態を毎フレーム poll し、一定速度で動かす。
    void   updatePlayheadScrub (double timestampSec);
    bool   scrubActive      { false };
    int    scrubDir         { 0 };       // -1 / 0 / +1
    double lastScrubWallSec { 0.0 };

    // 録音前の Lane 0 + テイクレーン スナップショット (Undo で録音クリップを除去するため)
    struct PreRecSnapshot
    {
        Track* track { nullptr };
        std::vector<EditActions::LaneSnapshotAction::ClipSnap> lane0Snap;
        // テイクレーン (lane 1..) の録音前状態。[i] = lane i+1。録音で増えた/変わった
        // レーンを特定し、ループ録音のテイクを Undo で 1 つずつ戻すために使う
        std::vector<std::vector<EditActions::LaneSnapshotAction::ClipSnap>> takeLaneSnaps;
    };
    std::vector<PreRecSnapshot> preRecSnaps;
    // 直近の録音がループ録音だったか (停止時の Undo トランザクション分割方式の選択に使う)
    bool lastRecordingWasLoop { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainComponent)
};
