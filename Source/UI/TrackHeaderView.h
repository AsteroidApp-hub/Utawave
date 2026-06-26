// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

#pragma once
#include <JuceHeader.h>
#include "../Localisation.h"
#include "../Tracks/Track.h"

class TrackHeaderView : public juce::Component,
                        public juce::DragAndDropTarget,
                        public juce::TooltipClient
{
public:
    // 既存コントロール (Vol/Pan/Rev/M/S/R/I 等) の固定幅。
    // INS スロット表示時はこの右側に追加で枠が広がる。
    // MainComponent::trackHeaderWidth と同じ値を意図しているので変更時は両方更新する。
    static constexpr int controlsWidth = 240;

    explicit TrackHeaderView(Track& track);
    ~TrackHeaderView() override;

    void paint(juce::Graphics&) override;
    void resized() override;
    void mouseMove(const juce::MouseEvent&) override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;

    // juce::TooltipClient — テイクレーンの ↑ (差し込み) / S (ソロ) は paint() で直接描く
    // 疑似ボタンなので setTooltip を持てない。ホバー位置から該当ボタンの説明を返す。
    juce::String getTooltip() override;

    // juce::DragAndDropTarget
    bool isInterestedInDragSource(const SourceDetails&) override;
    void itemDragEnter(const SourceDetails&) override;
    void itemDragMove (const SourceDetails&) override;
    void itemDragExit (const SourceDetails&) override;
    void itemDropped  (const SourceDetails&) override;

    // レーン数が変わったとき呼ぶ
    void refresh();

    // 入力レベル更新（dB 値）— Timer から定期的に呼ぶ
    void updateInputLevels(float peakL, float peakR, float vuL, float vuR);

    // VU メータの 0 VU 基準レベル (dBFS)
    void setVuReferenceLevel(float dB) { vuReferenceLevel = dB; repaint(); }
    // ラウドネス自動調整のターゲット (LUFS)
    void setLoudnessTargetLufs(float lufs) { loudnessTargetLufs = lufs; }

    std::function<void()>      onChanged;
    // メイン高さリサイズのドラッグ中に新しい高さ (px) を通知する。Panel 側で複数選択トラックへ
    // 同じ高さを追従反映する用途 (未設定ならビューが自トラックへ直接 setCustomHeight する)。
    std::function<void(int)>   onResizeTo;
    std::function<void(bool)>  onInputMonitorChanged;
    std::function<int()>       getNumInputChannels;
    std::function<void()>      onSelected;   // トラック選択コールバック (互換用)
    // 修飾キー付き選択コールバック (Shift / Cmd を Panel 側に伝える)
    std::function<void(juce::ModifierKeys)> onSelectedWithMods;
    std::function<void()>      onDeleteRequest;        // トラック削除リクエスト
    std::function<int()>       getDeleteCount;         // 削除メニューで実際に消える本数 (複数選択時の表示用)
    std::function<void()>      onDuplicateRequest;     // トラック複製リクエスト
    // テイクレーン (laneIndex>0) の ↑ ボタン: 指定レーンを Lane 0 へ採用
    std::function<void(int)>   onLanePromoteRequest;
    // ↑ ボタンの活性判定 (laneIndex)。未設定時は非活性扱い。
    std::function<bool(int)>   getLanePromoteEnabled;
    std::function<void(int)>   onPluginAddRequest;     // プラグイン追加（slotIdx を指定）
    std::function<void(int)>   onPluginEditRequest;    // 指定スロットのエディタを開く
    std::function<void(int)>   onPluginRemoveRequest;  // 指定スロットを削除
    std::function<void(int,int)> onPluginSwapRequest;  // (a, b) を入れ替え
    std::function<void(int)>     onPluginBypassRequest; // 指定スロットのバイパスをトグル
    // D&D: プラグインを別トラックへ移動 (copy=false) / コピー (copy=true)
    // 引数: srcTrackIdx, srcSlotIdx, dstSlotIdx, copy
    std::function<void(int,int,int,bool)> onPluginDropFromOtherTrack;
    // プロパティ編集 (名前/色/シンセ設定) を Undo 対応で適用する委譲。
    // 渡した mutate を実行し、その前後差分を 1 つの Undo として記録させる。
    // 未設定なら mutate を直接実行 (フォールバック)。
    std::function<void(std::function<void()>)> onEditUndoable;

    void setSelected(bool s) { selected = s; repaint(); }
    bool isSelected() const  { return selected; }
    // メイン/レーン高さのリサイズドラッグ中か。Panel が reorder ドラッグの誤発火を抑止する判定に使う。
    bool isResizing() const  { return draggingResize || draggingLaneResize; }

    Track& getTrack() { return track; }

    // 自身のトラックインデックス（D&D 識別用）。-1 = 未設定
    void setTrackIndex(int idx) { trackIndex = idx; }
    int  getTrackIndex() const  { return trackIndex; }

private:
    static constexpr int resizeZone  { 6 };
    static constexpr int subLaneIndent { 14 };  // サブレーンの左インデント幅
    // INS パネルの固定サイズ (トラック高さを大きくしてもスロットは伸縮しない)
    static constexpr int insHeaderH { 13 };
    static constexpr int insSlotH   { 22 };
    static constexpr int insFrameH  { insHeaderH + 1 + insSlotH * Track::insertSlotCount };  // 8 枠 = 190
    // トラック高さ上限は INS 枠の高さと一致させる (8 スロット全部が出せる所まで)。
    static_assert(insFrameH == Track::maxHeight,
                  "Track::maxHeight は INS パネルの高さ (insFrameH) と一致させること");

    // mutate を Undo 対応で実行する (onEditUndoable があればそこへ委譲)
    void editTrackUndoable(std::function<void()> mutate)
    {
        if (onEditUndoable) onEditUndoable(std::move(mutate));
        else if (mutate)    mutate();
    }
    // Vol/Pan/Rev スライダのドラッグ中フラグと、ドラッグ開始時のモデル値。
    // ドラッグはライブ適用し、ドラッグ終了で「開始値→終了値」を 1 つの Undo にまとめる。
    bool   mixDragging  { false };
    double mixDragBefore { 0.0 };

    // refresh() から呼ぶ表示更新 (移調ラベル / 波形ボタン)。ctor で設定する。
    std::function<void()> updateMidiInfoDisplay;
    std::function<void()> refreshWaveformDisplay;

    bool isInResizeZone(const juce::MouseEvent& e) const;
    bool isInLaneResizeZone(const juce::MouseEvent& e) const;
    // サブレーンの S ボタン矩形を返す（描画・ヒットテスト共通）
    juce::Rectangle<int> getLaneSoloBtnRect(int laneIndex) const;
    // サブレーンの ↑ (採用) ボタン矩形。S ボタンの左隣（描画・ヒットテスト共通）
    juce::Rectangle<int> getLanePromoteBtnRect(int laneIndex) const;

    Track& track;

    // メイントラックボタン
    juce::TextButton muteBtn    { "M" };
    juce::TextButton soloBtn    { "S" };
    juce::TextButton recBtn     { "R" };
    juce::TextButton monBtn     { "I" };
    juce::TextButton lanesBtn   { "Lanes v" };  // レーン開閉（底部に配置）
    juce::Label      inputLabel;               // "In:" ラベル
    juce::ComboBox   inputChBox;               // 入力チャンネル選択
    juce::Label      stereoBadge;              // "ST" / "MO" 表示
    juce::Slider     volSlider;
    juce::Slider     panSlider;
    juce::Slider     revSlider;  // 簡易リバーブセンド (0..1)
    juce::Label      nameLabel;
    // クリックトラック専用
    juce::ComboBox   clickSoundBox;
    juce::TextButton clickAccentBtn { "ACC" };
    juce::TextButton clickHalfBtn   { tr(u8"½") };
    juce::TextButton clickDoubleBtn { "x2" };

    // MIDI トラック専用: 移調 + 波形
    juce::TextButton octDownBtn  { tr(u8"Oct▼") };
    juce::TextButton octUpBtn    { tr(u8"Oct▲") };
    juce::TextButton semiDownBtn { tr(u8"♭") };
    juce::TextButton semiUpBtn   { tr(u8"♯") };
    juce::Label      midiInfoLabel;   // "+0" など現在の総移調量
    juce::TextButton waveformBtn;     // クリックで波形サイクル、右クリック/Cmd+クリックで内蔵シンセ ON/OFF
    bool waveformRightClickHandled { false };   // 右クリック直後の onClick を抑止

    // ── インサート FX スロット ──
    void rebuildInsertChips();
    juce::OwnedArray<juce::TextButton> fxChips;   // 4 スロット固定枠

    void populateInputChannelBox();

    bool selected           { false };
    bool draggingResize     { false };  // メイン高さリサイズ中
    bool draggingLaneResize { false };  // レーン高さリサイズ中
    int  dragStartHeight    { 0 };
    int  dragStartScreenY   { 0 };
    int  trackIndex         { -1 };

    // D&D: プラグインチップのドラッグ管理
    int  dragSourceSlotIdx  { -1 };   // mouseDown 時、ドラッグ候補スロット (>=0)
    bool dragStarted        { false };
    juce::Point<int> dragStartPos;

    // D&D: ドロップ位置のハイライト中スロット
    int  dropHighlightSlot  { -1 };

    // ヘッダー内のローカル座標 → INS スロットインデックス（外なら -1）
    int  findInsertSlotAt(juce::Point<int> localPos) const;

    // 入力レベル（dB）
    float inPeakL { -96.0f }, inPeakR { -96.0f };
    float inVUL   { -96.0f }, inVUR   { -96.0f };
    float vuReferenceLevel { -18.0f };
    float loudnessTargetLufs { -24.0f };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TrackHeaderView)
};
