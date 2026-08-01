// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

#pragma once
#include <JuceHeader.h>
#include "UtawaveLookAndFeel.h"
#include "PluginBypassSwitch.h"

class PluginChain;

class MasterPanel : public juce::Component,
                    public juce::DragAndDropTarget
{
public:
    // マスターのインサートスロット数
    static constexpr int insertSlotCount { 6 };
    // D&D で「マスター」を表す trackIdx（-1）
    static constexpr int kMasterIndex { -1 };

    MasterPanel();
    ~MasterPanel() override;

    void paint(juce::Graphics&) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;

    // juce::DragAndDropTarget
    bool isInterestedInDragSource(const SourceDetails&) override;
    void itemDragEnter(const SourceDetails&) override;
    void itemDragMove (const SourceDetails&) override;
    void itemDragExit (const SourceDetails&) override;
    void itemDropped  (const SourceDetails&) override;

    void setLevels(float peakLdb, float peakRdb,
                   float vuLdb,   float vuRdb,
                   float peakHoldLdb, float peakHoldRdb);

    void resetPeakHold();
    // RESET 押下時に AudioEngine 側のピークホールドも 0 に戻すための通知
    // (これが無いと次の setLevels で engine の保持値が戻り CLIP! が再点灯する)
    std::function<void()> onResetPeakHold;

    // VU メータの 0 VU 基準レベル (dBFS)。色閾値 / 基準線描画に使用
    void setVuReferenceLevel(float dB) { vuReferenceLevel = dB; repaint(); }

    // マスターチェーンを差し込む（AudioEngine から）
    void setPluginChain(PluginChain* chain);
    void refreshChips();

    // INS スロットの表示切替（右クリックメニュー / AppSettings からの復元用）
    void setInsertSlotsVisible(bool v);
    bool isInsertSlotsVisible() const { return insertSlotsVisible; }
    std::function<void(bool)> onInsertSlotsVisibilityChanged;

    // マスターパネル自体の折りたたみ
    void setCollapsed(bool v);
    bool isCollapsed() const { return collapsed; }
    // パネル折りたたみ時に親（MainComponent）に通知してレイアウト変更
    std::function<void(bool)> onCollapseToggled;
    static constexpr int kCollapsedWidth { 22 };

    std::function<void(double)>      onMasterGainChanged;
    std::function<void(int)>         onPluginAddRequest;     // slotIdx
    std::function<void(int)>         onPluginEditRequest;    // slotIdx
    std::function<void(int)>         onPluginRemoveRequest;  // slotIdx
    std::function<void(int,int)>     onPluginSwapRequest;    // (a, b)
    std::function<void(int)>         onPluginBypassRequest;  // slotIdx のバイパスをトグル
    // D&D で他トラックから来た: srcTrackIdx, srcSlotIdx, dstSlot, copy
    std::function<void(int,int,int,bool)> onPluginDropFromOtherTrack;

private:
    void drawMeter(juce::Graphics& g, juce::Rectangle<int> b,
                   float db, float holdDb);
    void drawDbScale(juce::Graphics& g, juce::Rectangle<int> faderBounds);
    int  dbToY(float db, juce::Rectangle<int> bounds) const;
    // INS スロット枠の矩形（"INSERTS" タイトル含む）
    juce::Rectangle<int> getInsertsArea() const;
    juce::Rectangle<int> getInsertsInnerArea() const;
    int  findInsertSlotAt(juce::Point<int> localPos) const;

    UtawaveLookAndFeel laf;
    juce::Slider        masterFader;
    juce::Label         masterLabel;
    juce::Label         gainLabel;
    juce::TextButton    peakResetBtn;
    juce::TextButton    collapseBtn;

    PluginChain* pluginChain { nullptr };
    juce::OwnedArray<juce::TextButton> fxChips;
    // チップ左のワンクリック・バイパススイッチ (fxChips と index 並走・空きスロットは nullptr)
    juce::OwnedArray<PluginBypassSwitch> fxBypassBtns;

    // D&D 状態
    int  dragSourceSlotIdx { -1 };
    bool dragStarted       { false };
    int  dropHighlightSlot { -1 };

    // INS 表示状態（既定: 表示）
    bool insertSlotsVisible { true };
    // パネル折りたたみ状態（既定: 開）
    bool collapsed { false };

    float peakLdb { -96.0f };
    float peakRdb { -96.0f };
    float vuLdb   { -96.0f };
    float vuRdb   { -96.0f };
    float holdLdb { -96.0f };
    float holdRdb { -96.0f };
    float vuReferenceLevel { -18.0f };

    static constexpr float minDb = -60.0f;
    static constexpr float maxDb =  6.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MasterPanel)
};
