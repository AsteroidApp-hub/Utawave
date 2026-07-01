// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

#pragma once
#include <JuceHeader.h>
#include "../Localisation.h"
#include "ExportEngine.h"

class ExportDialog : public juce::Component
{
public:
    enum class RangeKind { Project, Ruler, Bars };

    struct TrackInfo
    {
        int          index { 0 };
        juce::String name;
        juce::Colour colour;
        bool         isStereoByDefault { false };  // トラック自体がステレオか
    };

    struct Context
    {
        juce::String defaultBaseName;   // mix-down 時のファイル名（拡張子なし）
        juce::File   defaultFolder;     // 出力フォルダ
        double       projectEndSec  { 0.0 };
        double       projectSr      { 48000.0 };
        int          projectBits    { 32 };
        // ルーラー範囲（ループ範囲を兼用）: ルーラー横ドラッグで指定した範囲。
        double       rulerStartSec  { 0.0 };
        double       rulerEndSec    { 0.0 };
        bool         rulerAvailable { false };
        juce::String rulerRangeDesc;                 // 表示用（例: "Bar 5 〜 33"）
        // 小節範囲: 1-based 小節番号 → 開始秒、末尾小節（デフォルト終了値）
        int          projectEndBar  { 1 };
        std::function<double(int)> barToSec;
        std::vector<TrackInfo> tracks;
    };

    ExportDialog(const Context& ctx);

    std::function<void(const ExportEngine::Options&)> onExport;
    std::function<void()> onCancel;

    void resized() override;
    void paint(juce::Graphics& g) override;
    void paintOverChildren(juce::Graphics& g) override;
    // トラックのチェックボックス上をドラッグして、通過した行のチェックをまとめて塗り替える
    // (下へなぞると連続でチェックが入る/外れる)。トグルは各行に addMouseListener 済み。
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp  (const juce::MouseEvent&) override;

private:
    void chooseFolder();
    void doExport();
    bool resolveRange(double& start, double& end) const;
    void rebuildTrackList();
    void layoutTrackRows();
    void updateModeVisibility();
    void updateRangeVisibility();   // ルーラー範囲モードのときだけ範囲表示ラベルを出す
    std::vector<int> getSelectedTrackIndices() const;
    int  getTrackChannels(int trackIndex) const;  // 1 = mono / 2 = stereo
    bool getTrackPreFader(int trackIndex) const;

    Context context;

    // このダイアログは独立した DialogWindow として表示されるため、MainComponent 側の
    // TooltipWindow は届かない。ダイアログ内の setTooltip を機能させるため自前で持つ。
    juce::TooltipWindow tooltipWindow { this };

    juce::Label    titleLabel;

    juce::Label    rangeLabel;
    juce::TextButton rangeProjectBtn   { tr(u8"プロジェクト全体") };
    juce::TextButton rangeRulerBtn     { tr(u8"ルーラー範囲") };
    juce::TextButton rangeBarsBtn      { tr(u8"小節範囲") };

    // ルーラー範囲モード用: 現在のルーラー範囲を読み取り専用で表示
    juce::Label      rulerRangeLabel;

    // 小節範囲モード用: 開始/終了小節の入力
    juce::Label      barStartLabel;
    juce::TextEditor barStartEditor;
    juce::Label      barRangeSepLabel;   // "〜"
    juce::Label      barEndLabel;
    juce::TextEditor barEndEditor;

    juce::Label    formatLabel;
    juce::ComboBox formatBox;
    juce::Label    mp3BitrateLabel;
    juce::ComboBox mp3BitrateBox;

    juce::Label    sampleRateLabel;
    juce::ComboBox sampleRateBox;

    juce::Label    bitDepthLabel;
    juce::ComboBox bitDepthBox;

    juce::ToggleButton ditherBtn       { tr(u8"TPDFディザ (16/24bit)") };
    juce::ToggleButton autoRenameBtn   { tr(u8"同名ファイルがある場合は自動連番") };
    juce::ToggleButton revealAfterBtn  { tr(u8"完了後に出力フォルダを開く") };
    juce::ToggleButton realtimeBtn     { tr(u8"実時間レンダリング（一部プラグインの互換用、再生長と同じ時間がかかります）") };

    // 書き出しモード（ミックスダウン / トラック書き出し）
    juce::Label      modeLabel;
    juce::TextButton modeMixdownBtn { tr(u8"ミックスダウン") };
    juce::TextButton modeTrackBtn   { tr(u8"トラック書き出し") };

    // mix-down 用
    juce::Label      nameLabel;
    juce::TextEditor nameEditor;
    juce::Label      mixChannelsLabel;
    juce::TextButton mixMonoBtn   { tr(u8"モノ") };
    juce::TextButton mixStereoBtn { tr(u8"ステレオ") };

    // stems 用: トラックチェックリスト
    static constexpr int kTrackRowH = 30;   // 1 行の高さ（描画・スナップ共通）

    // スクロール位置を行高にスナップして、半端な行が上下端で見切れないようにする Viewport
    struct RowSnapViewport : public juce::Viewport
    {
        int snapStep { 0 };
        void visibleAreaChanged(const juce::Rectangle<int>& newArea) override
        {
            if (snapStep <= 1) return;
            const int y = newArea.getY();
            const int snapped = ((y + snapStep / 2) / snapStep) * snapStep;
            if (snapped != y)
                setViewPosition(newArea.getX(), snapped);   // 1 段だけ再入し収束する
        }
    };

    juce::Label          tracksLabel;
    RowSnapViewport      tracksViewport;
    juce::Rectangle<int> listFrameBounds;   // リスト枠（角丸）の描画範囲

    // 行間に区切り線を引くだけのコンテンツコンポーネント
    struct TrackListContent : public juce::Component
    {
        int rowHeight { 26 };
        int numRows   { 0 };
        void paint(juce::Graphics& g) override
        {
            if (numRows <= 1) return;
            g.setColour(juce::Colour(0xff2c2f34));
            for (int i = 1; i < numRows; ++i)
                g.fillRect(juce::Rectangle<int>(8, i * rowHeight - 1, getWidth() - 16, 1));
        }
    };
    TrackListContent tracksContent;
    juce::OwnedArray<juce::ToggleButton> trackToggles;
    juce::OwnedArray<juce::TextButton>   trackMonoBtns;
    juce::OwnedArray<juce::TextButton>   trackStereoBtns;
    juce::OwnedArray<juce::TextButton>   trackPreBtns;
    juce::OwnedArray<juce::TextButton>   trackPostBtns;
    juce::TextButton selectAllBtn   { tr(u8"全選択") };
    juce::TextButton deselectAllBtn { tr(u8"全解除") };

    // チェックボックスのドラッグ塗り替え状態
    bool dragPaintActive { false };   // 塗り替えドラッグ中か
    bool dragPaintTarget { false };   // 塗る対象の状態 (開始行を反転した値)

    // 共通: 出力フォルダ
    juce::Label      folderLabel;
    juce::TextEditor folderEditor;
    juce::TextButton browseBtn { tr(u8"参照...") };

    juce::TextButton exportBtn { tr(u8"書き出し") };
    juce::TextButton cancelBtn { tr(u8"キャンセル") };

    std::unique_ptr<juce::FileChooser> fileChooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ExportDialog)
};
