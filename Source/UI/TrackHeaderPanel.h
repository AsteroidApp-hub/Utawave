// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

#pragma once
#include <JuceHeader.h>
#include <set>
#include "../Tracks/TrackManager.h"
#include "TrackHeaderView.h"

class TrackHeaderPanel : public juce::Component
{
public:
    // ルーラー高さ。TimelineView 側の高さに合わせて MainComponent から更新する
    int rulerH { 80 };
    void setRulerHeight(int h) { rulerH = juce::jmax(0, h); resized(); repaint(); }

    TrackHeaderPanel(TrackManager& tm);
    ~TrackHeaderPanel() override;

    void paint(juce::Graphics&) override;
    // 並び替えドラッグ中の挿入位置ライン。トラックヘッダ (子コンポーネント) の上に描くため
    // paint ではなく paintOverChildren を使う。
    void paintOverChildren(juce::Graphics&) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseDoubleClick(const juce::MouseEvent&) override;

    void refresh();
    // ループ録音のラップ時用の軽量版: 録音アーム中のトラック (= テイクレーンが増えた
    // トラック) のビューだけ refresh して再レイアウトする。他トラックの ComboBox 再構築や
    // 全面 repaint を毎ラップ繰り返さない (多トラック時のラップ瞬間のヒッチ防止)。
    // トラック集合が変わっていた場合は安全側で refresh() に委譲する。
    void refreshRecArmedTracks();
    void setScrollY(int y);
    // VU メータの 0 VU 基準レベル (dBFS) を全トラックビューに反映
    void setVuReferenceLevel(float dB);
    // ラウドネス自動調整のターゲット (LUFS) を全トラックビューに反映
    void setLoudnessTargetLufs(float lufs);
    // 入力レベル更新（タイマーから呼ぶ）。levelGetter は input ch index を取り (peak,vu) を返す
    void updateInputLevels(std::function<float(int)> peakGetter,
                           std::function<float(int)> vuGetter);
    // トラック出力メータ取得（MIDI トラック等で input チャンネルが無い場合に使う）
    std::function<float(int)> onGetTrackOutPeakL;
    std::function<float(int)> onGetTrackOutPeakR;
    std::function<float(int)> onGetTrackOutVUL;
    std::function<float(int)> onGetTrackOutVUR;

    std::function<void()>     onAddTrack;          // モノ（既定）
    // (stereo, appendAtBottom): appendAtBottom=true で選択に依らず末尾へ追加 (ダブルクリック用)
    std::function<void(bool, bool)> onAddTrackWithMode;  // false=mono, true=stereo
    std::function<void()>     onAddMidiTrack;      // 空の MIDI トラック（ハモリ/ガイド打ち込み用）
    std::function<void()>     onAddClickTrack;
    std::function<void()>     onAddFolderTrack;    // フォルダトラック (グループバス)
    // フォルダトラックの追加メニューを出すか (AppPreferences::enableFolderTracks)。未設定は出さない
    std::function<bool()>     folderTracksEnabled;
    // フォルダトラックに Pan/Rev/INS スロットを表示するか (AppPreferences::showFolderPanRevIns)。
    // 未設定は表示。通常トラックには影響しない
    std::function<bool()>     folderExtrasEnabled;
    // 右クリック「フォルダへ移動 / フォルダから出す」: trackIdx 群を folderIdx (-1 = 出す) へ。
    // クリックしたトラックが複数選択に含まれていれば選択集合をまとめて渡す (削除と同じ作法)。
    // Undo (FolderAssignAction) は MainComponent 側で積む
    std::function<void(const std::vector<int>&, int)> onTracksMoveToFolder;
    std::function<void()>     onTrackChanged;
    std::function<int()>     onGetNumInputChannels;
    std::function<void(int)> onTrackSelected;
    std::function<void(int, int)> onPluginAddRequest;    // trackIdx, slotIdx
    std::function<void(int, int)> onPluginEditRequest;   // trackIdx, slotIdx
    std::function<void(int, int)> onPluginRemoveRequest; // trackIdx, slotIdx
    std::function<void(int, int, int)> onPluginSwapRequest; // trackIdx, slotA, slotB
    std::function<void(int, int)> onPluginBypassRequest;    // trackIdx, slotIdx
    // D&D: srcTrackIdx, srcSlotIdx, dstTrackIdx, dstSlotIdx, copy(true=Optionでコピー)
    std::function<void(int, int, int, int, bool)> onPluginDropAcrossTracks;
    // 削除する trackIdx 群（複数選択をまとめて削除。プラグイン後処理・確認は呼び出し側でやる）
    std::function<void(const std::vector<int>&)> onTracksDeleteRequest;
    std::function<void(int, bool)> onTrackDuplicateRequest;  // trackIdx, includeTakeLanes（プラグインも含めて複製。false=Lane 0 のみ）
    // 並べ替え完了通知 (Undo 用): 変更前/変更後のトラック順 + 移動した (選択中の) トラック群
    // (Track* 列)。並べ替えは performReorder が既に実施済みで、呼び出し側は履歴に積むだけ。
    // 移動トラック群は undo/redo 後に identity で選択を貼り直すのに使う (選択状態を維持)。
    std::function<void(std::vector<Track*>, std::vector<Track*>, std::vector<Track*>)> onTracksReordered;
    // D&D でフォルダ所属が変わった並べ替えの完了通知 (Undo 用。並べ替えも所属変更も panel が
    // 実施済み)。moved = 動かしたトラック群 / parentBefore = (子, 変更前の親) /
    // afterParent = 新しい親 (nullptr = フォルダから出した)。呼び出し側 (MainComponent) は
    // FolderAssignAction を履歴に積む (最初の perform は適用済み状態への再適用 = 冪等)。
    std::function<void(std::vector<Track*>, std::vector<Track*>, std::vector<Track*>,
                       std::vector<std::pair<Track*, Track*>>, Track*)> onTracksReorderedToFolder;
    // テイクレーン ↑ ボタン: 指定トラックの指定レーンを Lane 0 へ採用
    std::function<void(int, int)> onLanePromoteRequest;  // trackIdx, laneIdx
    // ↑ ボタンの活性判定 (trackIdx, laneIdx)
    std::function<bool(int, int)> onCanPromoteLane;
    // トラックのプロパティ編集 (名前/色/シンセ) を Undo 対応で適用する委譲。
    // 引数: 対象 Track*, 実行する mutate。
    std::function<void(Track*, std::function<void()>)> onTrackEditUndoable;

    // Shift+Option+クリックで S / M を選択中トラックへ一括適用する委譲。
    // 引数: クリックされた trackIdx, クリック後の新しい値 (true=ON)。MainComponent が選択集合へ適用。
    std::function<void(int, bool)> onTrackSoloBatch;
    std::function<void(int, bool)> onTrackMuteBatch;

    // 全トラックヘッダを再描画する (選択/範囲変化で ↑ ボタンの活性表示を更新)
    void repaintHeaders();

    void setSelectedTrack(int index);  // 単一選択 (他をクリア)
    // 複数選択 API
    // Cmd+クリック相当: 選択への追加 / トグル
    void toggleTrackInSelection(int index);
    // Shift+クリック相当: anchor から index までを範囲選択
    void selectTrackRange(int index);
    // 現在の選択集合 (アンカー含む)
    const std::set<int>& getSelectedTrackIndices() const { return selectedIndices; }
    // 選択を全解除 (トラック削除後などに呼ぶ)
    void clearTrackSelection();
    // 外部 (タイムラインのクリップ/範囲選択) からトラック選択ハイライトを設定する。
    // onTrackSelected は呼ばない (ハイライトと主選択 index の更新は呼び出し側で行う)。
    void setSelectedTracks(const std::vector<int>& indices);

private:
    void showAddTrackMenu();
    // 全トラックの INS スロット表示を一括トグルする (insToggleBtn と右クリックメニュー共用ロジック)
    void toggleAllInsertSlots();
    // insToggleBtn のハイライト / ツールチップ / 可視状態を現在の全トラック INS 状態へ同期
    void updateInsToggleState();

    TrackManager& trackManager;
    int scrollY { 0 };

    // INS スロット一括表示の状態 (FX ボタンの点灯/トグル意図)。トラックが 1 本もない
    // 状態で FX を押した時の「次に追加するトラックを INS 表示で出す」意図をここに覚えておき、
    // pendingInsApply が立っていれば次の refresh() で新規トラックへ反映する。
    // (プロジェクト読込は明示トグルではないので pendingInsApply を立てず、読込んだ
    //  per-track の insertSlotsVisible を壊さない)
    bool insSlotsOn     { false };
    bool pendingInsApply { false };

    juce::TextButton addTrackBtn;
    juce::TextButton addTrackPlus;

    // スクロールしてもルーラーが最前面に出るよう、専用 Component に分離する。
    // paint() で TRACKS テキスト + 背景を描き、addTrackPlus を子として保持する。
    class RulerHeader : public juce::Component
    {
    public:
        RulerHeader(TrackHeaderPanel& p) : panel(p) {}
        void paint(juce::Graphics&) override;
        TrackHeaderPanel& panel;
    };
    RulerHeader rulerHeader { *this };

    // INS スロット一括開閉トグル ("TRACKS" ラベル下・左端の "+" の右隣に配置)。
    // 右クリックメニューでしか出来なかった全トラックの INS スロット表示切替を可視ボタン化する。
    // ラベルは分かりやすさ優先で "FX"。表示中はアクセント色で点灯する。
    class InsToggleButton : public juce::Button
    {
    public:
        InsToggleButton() : juce::Button("FX") { setWantsKeyboardFocus(false); }
        void setActive(bool a) { if (active != a) { active = a; repaint(); } }
    private:
        void paintButton(juce::Graphics&, bool highlighted, bool down) override;
        bool active { false };
    };
    InsToggleButton insToggleBtn;

    std::vector<std::unique_ptr<TrackHeaderView>> headerViews;
    // 現在表示中のトラック集合 (順序込み)。これと一致するなら refresh() は
    // 破棄/再生成せず軽量な per-view 更新だけで済ませる (#6)。
    std::vector<Track*> displayedTracks;
    float vuReferenceLevel { -18.0f };
    float loudnessTargetLufs { -24.0f };

    // 複数選択状態 (anchor を含む全ての選択 index)
    std::set<int> selectedIndices;
    int selectionAnchor { -1 };  // 直近に単一選択した index (Shift+クリック範囲の起点)

    // 並び替えドラッグ用
    int  dragSourceIndex { -1 };
    bool dragReorderStarted { false };
    juce::Point<int> dragReorderStart;
    int  dropTargetIndex { -1 };  // 描画用: 行間の位置 (0..count, count = 末尾)
    // 「フォルダへ入れる」ドロップ先 (フォルダ行の中央帯にホバー中のフォルダ track index、-1 = なし)。
    // 行の上下端 1/4 は従来の挿入線 (並べ替え) のままにして、行前後へのドロップも共存させる。
    int  dropTargetFolderIdx { -1 };
    // 複数選択中のトラックを通常クリックした時: ドラッグなら集合移動、クリックのみ (ドラッグせず
    // 離す) なら単一選択へ collapse する。その候補 index を mouseDown で覚え mouseUp で確定する。
    int  pendingCollapseIdx { -1 };

    // ヘルパー
    int  headerViewAtY(int y) const;
    void applySelectionToViews();
    void selectTrackForUI(int index, juce::ModifierKeys mods);
    // dropOnFolder != nullptr は「フォルダ行への直接ドロップ」(所属先を強制。dropIndex は
    // そのフォルダのラン末尾ギャップを渡す)。nullptr ならギャップ位置から所属先を導出する
    // (開いているフォルダのラン内ギャップ = そのフォルダへ / ラン外 = トップレベルへ)。
    void performReorder(int dropIndex, Track* dropOnFolder = nullptr);
    // ギャップ dropIndex の前後 (movers を除く) から所属先フォルダを導出 (nullptr = トップレベル)。
    // 閉じたフォルダのランには吸い込まない (入れたい時はフォルダ行へ直接ドロップ)。
    Track* folderForDropGap(int dropIndex, const std::vector<Track*>& movers);
    // idx の削除要求を解決する: idx が複数選択に含まれていれば選択集合を、そうでなければ
    // idx 単体を削除対象として onTracksDeleteRequest へ渡す。
    void requestDeleteSelectedOrTrack(int idx);
    // idx の削除メニューで実際に消える本数 (メニュー表示用)。idx が複数選択中なら選択数、
    // そうでなければ 1。
    int  deleteScopeCount(int idx) const;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TrackHeaderPanel)
};
