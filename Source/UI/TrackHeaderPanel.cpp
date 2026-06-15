// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

#include "TrackHeaderPanel.h"
#include "../Localisation.h"
#include "../AppColours.h"

TrackHeaderPanel::TrackHeaderPanel(TrackManager& tm) : trackManager(tm)
{
    setOpaque(true);   // paint() が fillAll で全面を塗る (GPU コンポジット軽量化)

    auto styleBtn = [this](juce::TextButton& btn, juce::String text)
    {
        btn.setButtonText(text);
        btn.setColour(juce::TextButton::buttonColourId,  AppColours::buttonBg);
        btn.setColour(juce::TextButton::textColourOffId, AppColours::accent);
        btn.setWantsKeyboardFocus(false);
        // 左クリックでもメニューを表示してモノ/ステレオ/クリックを選択
        btn.onClick = [this] { showAddTrackMenu(); };
    };
    styleBtn(addTrackBtn,  "+ Add Track");
    styleBtn(addTrackPlus, "+");

    addAndMakeVisible(addTrackBtn);

    // addTrackPlus は rulerHeader の子にして、スクロール時もルーラーごと最前面に
    rulerHeader.addAndMakeVisible(addTrackPlus);
    // INS スロット開閉トグルも rulerHeader の子 ("+" の直下に配置)
    rulerHeader.addAndMakeVisible(insToggleBtn);
    insToggleBtn.onClick = [this] { toggleAllInsertSlots(); };
    addAndMakeVisible(rulerHeader);

    addTrackBtn.addMouseListener(this, false);
    addTrackPlus.addMouseListener(this, false);
    rulerHeader.addMouseListener(this, false);  // ルーラー右クリックも受ける
}

void TrackHeaderPanel::RulerHeader::paint(juce::Graphics& g)
{
    g.setColour(AppColours::panelBg);
    g.fillRect(getLocalBounds());
    g.setColour(AppColours::separator);
    g.drawLine(0.0f, (float)(getHeight() - 1),
               (float)getWidth(), (float)(getHeight() - 1), 1.0f);

    g.setColour(AppColours::textDim);
    g.setFont(juce::FontOptions(11.0f).withStyle("Bold"));
    g.drawText("TRACKS", 10, 0, getWidth() - 44, getHeight(),
               juce::Justification::centredLeft);
}

void TrackHeaderPanel::paintOverChildren(juce::Graphics& g)
{
    // 並び替えドラッグ中だけ、ドロップ先 (差し込まれる箇所) に挿入ラインを描く。
    if (!dragReorderStarted || dropTargetIndex < 0 || headerViews.empty())
        return;

    // 挿入位置の Y: dropTargetIndex 番目ビューの上端 (末尾 = 最後のビューの下端)。
    int lineY = (dropTargetIndex < (int) headerViews.size())
                  ? headerViews[(size_t) dropTargetIndex]->getY()
                  : headerViews.back()->getBottom();
    // ルーラー (最前面) に被らないようクランプ。
    lineY = juce::jlimit(rulerH, getHeight(), lineY);

    const float x1 = 3.0f;
    const float x2 = (float) getWidth() - 3.0f;
    const float y  = (float) lineY;

    g.setColour(AppColours::accentHover);
    g.fillRect(x1, y - 1.5f, x2 - x1, 3.0f);

    // 左右端に内向きの三角キャップを付けて挿入位置を明示する。
    const float s = 4.0f;
    juce::Path caps;
    caps.addTriangle(x1, y - s, x1, y + s, x1 + s, y);
    caps.addTriangle(x2, y - s, x2, y + s, x2 - s, y);
    g.fillPath(caps);
}

void TrackHeaderPanel::showAddTrackMenu()
{
    juce::PopupMenu m;
    m.addItem(1, tr(u8"モノラルトラックを追加"));
    m.addItem(2, tr(u8"ステレオトラックを追加"));
    m.addItem(4, tr(u8"MIDI トラックを追加"));
    m.addSeparator();
    m.addItem(3, tr(u8"クリックトラックを追加"),
              !trackManager.hasClickTrack());
    m.showMenuAsync(juce::PopupMenu::Options(),
        [this](int result) {
            if (result == 1 && onAddTrackWithMode) onAddTrackWithMode(false);
            else if (result == 2 && onAddTrackWithMode) onAddTrackWithMode(true);
            else if (result == 4 && onAddMidiTrack) onAddMidiTrack();
            else if (result == 3 && onAddClickTrack) onAddClickTrack();
        });
}

void TrackHeaderPanel::mouseDown(const juce::MouseEvent& e)
{
    dragSourceIndex     = -1;
    dragReorderStarted  = false;
    dropTargetIndex     = -1;

    // ── 左クリック: トラック選択 (e.mods から確実にモディファイアを取得) ──
    // ボタン / スライダ / ComboBox 上のクリックも含めて、TrackHeaderView 配下なら
    // そのトラックを選択する。修飾キーで Shift / Cmd 複数選択も動作する。
    if (e.mods.isLeftButtonDown())
    {
        int hitIdx = -1;
        for (int i = 0; i < (int) headerViews.size(); ++i)
        {
            auto* hv = headerViews[(size_t) i].get();
            if (e.eventComponent == hv || hv->isParentOf(e.eventComponent))
            {
                hitIdx = i;
                break;
            }
        }
        if (hitIdx >= 0)
        {
            selectTrackForUI(hitIdx, e.mods);

            // 並び替えドラッグ用初期化 (修飾なし & 非インタラクティブ部のみ)
            pendingCollapseIdx = -1;
            const bool isInteractive = dynamic_cast<juce::Button*>(e.eventComponent)   != nullptr
                                    || dynamic_cast<juce::Slider*>(e.eventComponent)   != nullptr
                                    || dynamic_cast<juce::ComboBox*>(e.eventComponent) != nullptr;
            if (!isInteractive && !e.mods.isCommandDown() && !e.mods.isShiftDown())
            {
                dragSourceIndex   = hitIdx;
                dragReorderStart  = e.getEventRelativeTo(this).getPosition();
                // 複数選択中の通常クリック: ドラッグなら集合移動、クリックのみなら単一へ collapse。
                // (selectTrackForUI は選択済みトラックの集合を保持しているので size > 1 を見る)
                if (selectedIndices.count(hitIdx) && selectedIndices.size() > 1)
                    pendingCollapseIdx = hitIdx;
            }
        }
    }

    // + ボタン上の右クリックでも同じメニュー
    if ((e.eventComponent == &addTrackBtn || e.eventComponent == &addTrackPlus)
        && e.mods.isRightButtonDown())
    {
        showAddTrackMenu();
        return;
    }

    // TRACKS ヘッダ（ルーラー）上の右クリック → グローバル設定メニュー
    // ルーラー描画は rulerHeader 子コンポーネントに移したので、そのイベントも受ける
    if ((e.eventComponent == &rulerHeader
         || (e.eventComponent == this && e.y < rulerH))
        && e.mods.isRightButtonDown())
    {

        // 全トラックの INS 表示状態を集計（全 ON / 全 OFF / 混在）
        const int n = trackManager.getTrackCount();
        int onCount = 0;
        for (int i = 0; i < n; ++i)
            if (auto* t = trackManager.getTrack(i))
                if (t->isInsertSlotsVisible()) ++onCount;
        const bool allOn  = (n > 0 && onCount == n);

        juce::PopupMenu m;
        m.addItem(1, tr(u8"INS スロットを表示"), /*enabled*/ n > 0, /*ticked*/ allOn);
        m.addItem(2, tr(u8"INS スロットを非表示"), /*enabled*/ n > 0, /*ticked*/ onCount == 0);
        // マウスカーソル位置に表示
        const auto screenPt = e.getScreenPosition();
        const juce::Rectangle<int> targetArea(screenPt.x, screenPt.y, 1, 1);
        m.showMenuAsync(juce::PopupMenu::Options().withTargetScreenArea(targetArea),
            [this](int result)
            {
                if (result == 1 || result == 2)
                {
                    const bool show = (result == 1);
                    for (int i = 0; i < trackManager.getTrackCount(); ++i)
                        if (auto* t = trackManager.getTrack(i))
                            t->setInsertSlotsVisible(show);
                    if (onTrackChanged) onTrackChanged();
                    resized();
                    repaint();
                }
            });
        return;
    }
}

void TrackHeaderPanel::mouseDrag(const juce::MouseEvent& e)
{
    if (dragSourceIndex < 0) return;
    const auto local = e.getEventRelativeTo(this);
    if (!dragReorderStarted
        && local.getPosition().getDistanceFrom(dragReorderStart) > 6)
    {
        // ドラッグ開始時、source も選択に追加する (未選択ならそれ単体を選択)
        if (!selectedIndices.count(dragSourceIndex))
            setSelectedTrack(dragSourceIndex);
        dragReorderStarted = true;
    }
    if (!dragReorderStarted) return;

    // ドロップ位置候補を決定 (各 view の中点で前/後を判定)
    int targetSlot = (int) headerViews.size();  // 末尾
    for (int i = 0; i < (int) headerViews.size(); ++i)
    {
        const auto b = headerViews[(size_t) i]->getBounds();
        if (local.y < b.getY() + b.getHeight() / 2) { targetSlot = i; break; }
    }
    if (targetSlot != dropTargetIndex)
    {
        dropTargetIndex = targetSlot;
        repaint();
    }
}

void TrackHeaderPanel::mouseUp(const juce::MouseEvent&)
{
    if (dragReorderStarted && dropTargetIndex >= 0)
        performReorder(dropTargetIndex);
    else if (!dragReorderStarted && pendingCollapseIdx >= 0)
    {
        // ドラッグせずに離した = 単なるクリック → 複数選択を解除してその 1 本だけにする
        setSelectedTrack(pendingCollapseIdx);
        if (onTrackSelected) onTrackSelected(pendingCollapseIdx);
    }
    pendingCollapseIdx = -1;
    dragSourceIndex    = -1;
    dragReorderStarted = false;
    dropTargetIndex    = -1;
    repaint();
}

void TrackHeaderPanel::performReorder(int dropIndex)
{
    std::vector<int> sel(selectedIndices.begin(), selectedIndices.end());
    std::sort(sel.begin(), sel.end());
    if (sel.empty()) return;

    // moveTrack を呼ぶたびに index が変わるので、Track ポインタで追跡する
    std::vector<Track*> movers;
    for (int i : sel)
        if (auto* t = trackManager.getTrack(i)) movers.push_back(t);

    // Undo 用に並べ替え前のトラック順を控える
    std::vector<Track*> beforeOrder;
    for (int i = 0; i < trackManager.getTrackCount(); ++i)
        beforeOrder.push_back(trackManager.getTrack(i));

    // dropIndex を「動かさない最初のトラックの直前」と解釈し anchor を確定
    Track* anchorTrack = nullptr;
    for (int i = dropIndex; i < trackManager.getTrackCount(); ++i)
    {
        auto* t = trackManager.getTrack(i);
        if (std::find(movers.begin(), movers.end(), t) == movers.end())
        {
            anchorTrack = t;
            break;
        }
    }

    // movers を相対順序を保ったまま anchor の直前へ順次移動
    auto indexOf = [&](Track* tgt) -> int
    {
        for (int i = 0; i < trackManager.getTrackCount(); ++i)
            if (trackManager.getTrack(i) == tgt) return i;
        return -1;
    };
    for (auto* m : movers)
    {
        const int from = indexOf(m);
        if (from < 0) continue;
        int to;
        if (anchorTrack)
        {
            const int anchorIdx = indexOf(anchorTrack);
            to = (from < anchorIdx) ? anchorIdx - 1 : anchorIdx;
        }
        else
        {
            to = trackManager.getTrackCount() - 1;
        }
        if (from != to) trackManager.moveTrack(from, to);
    }

    // 並べ替え後のトラック順を控える (Undo 用)
    std::vector<Track*> afterOrder;
    for (int i = 0; i < trackManager.getTrackCount(); ++i)
        afterOrder.push_back(trackManager.getTrack(i));

    // 選択集合を新しい位置に再構築
    selectedIndices.clear();
    for (int i = 0; i < trackManager.getTrackCount(); ++i)
    {
        auto* t = trackManager.getTrack(i);
        if (std::find(movers.begin(), movers.end(), t) != movers.end())
            selectedIndices.insert(i);
    }
    selectionAnchor = selectedIndices.empty() ? -1 : *selectedIndices.begin();

    // 選択ハイライトを移動先へ追従させる。moveTrack() は呼ぶたびに onChanged →
    // refresh() を発火し、その時点ではまだ selectedIndices が旧位置のままなので、
    // 並べ替え後のビューに古い選択が貼られてしまう。再構築済みの selectedIndices を
    // ここで貼り直して、移動したトラックの選択が正しい新位置に出るようにする。
    applySelectionToViews();

    // 実際に順序が変わったら Undo 履歴へ積む (呼び出し側 = MainComponent が perform)。
    // movers (= 移動した選択トラック) を渡し、undo/redo 後の選択復元に使う。
    if (beforeOrder != afterOrder && onTracksReordered)
        onTracksReordered(std::move(beforeOrder), std::move(afterOrder), std::move(movers));

    if (onTrackChanged) onTrackChanged();
}

void TrackHeaderPanel::mouseDoubleClick(const juce::MouseEvent& e)
{
    // 空いた領域（トラック一覧の下）ダブルクリック → モノラルトラックを追加
    if (e.eventComponent != this) return;

    const int rulerH_ = rulerH;
    if (e.y < rulerH_) return;

    int tracksBottom = rulerH_;
    for (int i = 0; i < trackManager.getTrackCount(); ++i)
        tracksBottom += trackManager.getTrack(i)->getTotalHeight();
    tracksBottom -= scrollY;

    int addBtnY = getHeight() - 34;
    if (e.y > tracksBottom && e.y < addBtnY)
    {
        if (onAddTrackWithMode) onAddTrackWithMode(false);
    }
}

TrackHeaderPanel::~TrackHeaderPanel() {}

void TrackHeaderPanel::setSelectedTrack(int index)
{
    // 単一選択: 他をクリアして index のみを選択 (anchor 更新)
    selectedIndices.clear();
    if (index >= 0 && index < (int) headerViews.size())
        selectedIndices.insert(index);
    selectionAnchor = index;
    applySelectionToViews();
}

void TrackHeaderPanel::toggleTrackInSelection(int index)
{
    if (index < 0 || index >= (int) headerViews.size()) return;
    if (selectedIndices.count(index))
    {
        selectedIndices.erase(index);
        if (selectionAnchor == index) selectionAnchor = -1;
    }
    else
    {
        selectedIndices.insert(index);
        selectionAnchor = index;
    }
    applySelectionToViews();
}

void TrackHeaderPanel::selectTrackRange(int index)
{
    if (index < 0 || index >= (int) headerViews.size()) return;
    if (selectionAnchor < 0)
    {
        setSelectedTrack(index);
        return;
    }
    const int lo = juce::jmin(selectionAnchor, index);
    const int hi = juce::jmax(selectionAnchor, index);
    selectedIndices.clear();
    for (int i = lo; i <= hi; ++i) selectedIndices.insert(i);
    applySelectionToViews();
}

void TrackHeaderPanel::applySelectionToViews()
{
    for (int i = 0; i < (int) headerViews.size(); ++i)
        headerViews[(size_t) i]->setSelected(selectedIndices.count(i) > 0);
}

void TrackHeaderPanel::clearTrackSelection()
{
    selectedIndices.clear();
    selectionAnchor = -1;
    applySelectionToViews();
}

void TrackHeaderPanel::setSelectedTracks(const std::vector<int>& indices)
{
    selectedIndices.clear();
    for (int i : indices)
        if (i >= 0 && i < (int) headerViews.size())
            selectedIndices.insert(i);
    selectionAnchor = selectedIndices.empty() ? -1 : *selectedIndices.begin();
    applySelectionToViews();
}

int TrackHeaderPanel::deleteScopeCount(int idx) const
{
    // idx が複数選択に含まれている時だけ「選択集合をまとめて削除」とみなす。
    if (selectedIndices.count(idx) > 0 && selectedIndices.size() > 1)
        return (int) selectedIndices.size();
    return 1;
}

void TrackHeaderPanel::requestDeleteSelectedOrTrack(int idx)
{
    std::vector<int> indices;
    if (selectedIndices.count(idx) > 0 && selectedIndices.size() > 1)
        indices.assign(selectedIndices.begin(), selectedIndices.end());  // 複数選択をまとめて
    else
        indices.push_back(idx);                                         // 単体

    if (onTracksDeleteRequest)
        onTracksDeleteRequest(indices);
}

void TrackHeaderPanel::selectTrackForUI(int index, juce::ModifierKeys mods)
{
    if (mods.isShiftDown())
        selectTrackRange(index);
    else if (mods.isCommandDown())
        toggleTrackInSelection(index);
    else if (selectedIndices.count(index))
        // 既に選択済み: そのまま (まとめてドラッグできるよう選択を保持)
        selectionAnchor = index;
    else
        setSelectedTrack(index);
    if (onTrackSelected) onTrackSelected(index);
}

int TrackHeaderPanel::headerViewAtY(int y) const
{
    for (int i = 0; i < (int) headerViews.size(); ++i)
        if (headerViews[(size_t) i]->getBounds().contains(0, y))
            return i;
    return -1;
}

void TrackHeaderPanel::refresh()
{
    const int n = trackManager.getTrackCount();

    // 現在のトラック集合 (順序込み) を取得
    std::vector<Track*> current;
    current.reserve((size_t) n);
    for (int i = 0; i < n; ++i)
        current.push_back(trackManager.getTrack(i));

    // 高速パス: トラック集合 / 順序 / 数が変わっていなければ、ビューを破棄せず
    // 各ビューの軽量更新だけ行う。ここでは Track ポインタは必ず生存している
    // (トラック削除は集合を変えるので下の完全再構築パスに落ちる)。
    if (current == displayedTracks
        && (int) headerViews.size() == n)
    {
        for (auto& v : headerViews)
            v->refresh();
        resized();
        return;
    }

    displayedTracks = current;

    headerViews.clear();

    for (int i = 0; i < n; ++i)
    {
        auto* track = trackManager.getTrack(i);
        auto view = std::make_unique<TrackHeaderView>(*track);
        int  idx  = i;  // キャプチャ用コピー
        view->onChanged             = [this] { if (onTrackChanged) onTrackChanged(); };
        view->onInputMonitorChanged = [this](bool) { if (onTrackChanged) onTrackChanged(); };
        // 名前/色/シンセ設定の編集を Undo 対応で適用 (対象 Track* を束ねて委譲)
        view->onEditUndoable        = [this, track](std::function<void()> m)
        {
            if (onTrackEditUndoable) onTrackEditUndoable(track, std::move(m));
            else if (m)              m();
        };
        // トラック選択は panel.mouseDown 内で e.mods から直接判定するので、
        // view->onSelected / onSelectedWithMods は無効化 (二重発火による上書き防止)。
        // ただし child component (M/S/R/I 等) からの onClick → onSelected コールに
        // 反応してしまうと、その時点では modifier 情報が失われており、shift+click 後の
        // 範囲選択を単一選択に戻してしまう。よって両方とも空にしておく。
        view->onSelected         = nullptr;
        view->onSelectedWithMods = nullptr;
        view->onDeleteRequest = [this, idx]
        {
            // idx が複数選択に含まれていれば選択集合をまとめて、そうでなければ idx 単体を削除。
            // プラグイン後処理・確認・再描画は MainComponent (onTracksDeleteRequest) に委譲する。
            requestDeleteSelectedOrTrack(idx);
        };
        // 削除メニューのラベルに「選択中の N 本」を出すため、削除対象数を返す
        view->getDeleteCount = [this, idx] { return deleteScopeCount(idx); };
        view->onDuplicateRequest = [this, idx]
        {
            // プラグインクローンは MainComponent 側で行う必要があるため必ず委譲
            if (onTrackDuplicateRequest)
                onTrackDuplicateRequest(idx);
            if (onTrackChanged) onTrackChanged();
        };
        view->onLanePromoteRequest   = [this, idx](int lane) { if (onLanePromoteRequest)  onLanePromoteRequest(idx, lane); };
        view->getLanePromoteEnabled  = [this, idx](int lane) { return onCanPromoteLane && onCanPromoteLane(idx, lane); };
        view->onPluginAddRequest     = [this, idx](int slot) { if (onPluginAddRequest)    onPluginAddRequest(idx, slot); };
        view->onPluginEditRequest    = [this, idx](int slot) { if (onPluginEditRequest)   onPluginEditRequest(idx, slot); };
        view->onPluginRemoveRequest  = [this, idx](int slot) { if (onPluginRemoveRequest) onPluginRemoveRequest(idx, slot); };
        view->onPluginSwapRequest    = [this, idx](int a, int b) { if (onPluginSwapRequest) onPluginSwapRequest(idx, a, b); };
        view->onPluginBypassRequest  = [this, idx](int slot) { if (onPluginBypassRequest) onPluginBypassRequest(idx, slot); };
        view->onPluginDropFromOtherTrack = [this, idx](int srcTrack, int srcSlot, int dstSlot, bool copy)
        {
            if (onPluginDropAcrossTracks)
                onPluginDropAcrossTracks(srcTrack, srcSlot, idx, dstSlot, copy);
        };
        view->setTrackIndex(idx);
        if (onGetNumInputChannels)
            view->getNumInputChannels = onGetNumInputChannels;
        view->setVuReferenceLevel(vuReferenceLevel);
        view->setLoudnessTargetLufs(loudnessTargetLufs);
        view->refresh();
        // パネル側でドラッグ&ドロップ並び替えを処理するため、view のマウスイベント
        // (子コンポーネント含む) を転送
        view->addMouseListener(this, /*wantsEventsForAllNestedChildComponents*/ true);
        addAndMakeVisible(*view);
        headerViews.push_back(std::move(view));
    }
    applySelectionToViews();

    // トラックが 0 本の時に FX を押した意図を、今追加されたトラックへ反映する。
    // pendingInsApply は明示トグル時のみ立つので、プロジェクト読込では発火せず
    // 読込んだ per-track の insertSlotsVisible を壊さない。
    if (pendingInsApply && n > 0)
    {
        for (int i = 0; i < n; ++i)
            if (auto* t = trackManager.getTrack(i))
                t->setInsertSlotsVisible(insSlotsOn);
        pendingInsApply = false;
        if (onTrackChanged) onTrackChanged();   // ヘッダ幅の再計算
    }

    resized();
}

void TrackHeaderPanel::refreshRecArmedTracks()
{
    const int n = trackManager.getTrackCount();
    if ((int) headerViews.size() != n || (int) displayedTracks.size() != n)
    {
        refresh();
        return;
    }
    for (int i = 0; i < n; ++i)
    {
        auto* t = trackManager.getTrack(i);
        if (t != displayedTracks[(size_t) i]) { refresh(); return; }
        if (t && t->isRecArmed())
            headerViews[(size_t) i]->refresh();
    }
    resized();  // テイクレーン追加で高さが変わるため全体を再レイアウト
}

void TrackHeaderPanel::setVuReferenceLevel(float dB)
{
    vuReferenceLevel = dB;
    for (auto& v : headerViews)
        v->setVuReferenceLevel(dB);
}

void TrackHeaderPanel::setLoudnessTargetLufs(float lufs)
{
    loudnessTargetLufs = lufs;
    for (auto& v : headerViews)
        v->setLoudnessTargetLufs(lufs);
}

void TrackHeaderPanel::setScrollY(int y)
{
    scrollY = y;
    resized();
}

void TrackHeaderPanel::repaintHeaders()
{
    // 各ビューを直接 repaint (refresh() の再生成/resized は走らせない = 範囲ドラッグ毎フレームでも軽い)
    for (auto& v : headerViews)
        if (v) v->repaint();
}

void TrackHeaderPanel::updateInputLevels(std::function<float(int)> peakGetter,
                                          std::function<float(int)> vuGetter)
{
    const int numIn = onGetNumInputChannels ? onGetNumInputChannels() : 0;

    for (size_t i = 0; i < headerViews.size(); ++i)
    {
        auto* track = trackManager.getTrack((int)i);
        if (!track || track->isClickTrack()) continue;

        // MIDI トラックは入力ではなくトラック出力（シンセ出力）をメータ表示
        if (track->isMidiTrack())
        {
            const int idx = (int)i;
            const float pL = onGetTrackOutPeakL ? onGetTrackOutPeakL(idx) : -96.0f;
            const float pR = onGetTrackOutPeakR ? onGetTrackOutPeakR(idx) : -96.0f;
            const float vL = onGetTrackOutVUL   ? onGetTrackOutVUL(idx)   : -96.0f;
            const float vR = onGetTrackOutVUR   ? onGetTrackOutVUR(idx)   : -96.0f;
            headerViews[i]->updateInputLevels(pL, pR, vL, vR);
            continue;
        }

        // R (rec armed) でも Input Monitor でもないトラックはトラック出力レベル
        // (再生中の音量) を表示。Input Monitor 点灯中は入力レベルを表示して、
        // 声を出すと Peak/VU が動くようにする。
        if (!track->isRecArmed() && !track->isInputMonitor())
        {
            const int idx = (int)i;
            const float pL = onGetTrackOutPeakL ? onGetTrackOutPeakL(idx) : -96.0f;
            const float pR = onGetTrackOutPeakR ? onGetTrackOutPeakR(idx) : -96.0f;
            const float vL = onGetTrackOutVUL   ? onGetTrackOutVUL(idx)   : -96.0f;
            const float vR = onGetTrackOutVUR   ? onGetTrackOutVUR(idx)   : -96.0f;
            headerViews[i]->updateInputLevels(pL, pR, vL, vR);
            continue;
        }

        const int ch = track->getInputChannel();
        const bool stereo = track->isStereo();

        // 入力チャンネル有効性チェック:
        //   モノ:    ch < numIn
        //   ステレオ: ch+1 < numIn (ペアが取れる)
        const bool valid = stereo ? (ch >= 0 && ch + 1 < numIn)
                                  : (ch >= 0 && ch     < numIn);
        if (!valid)
        {
            headerViews[i]->updateInputLevels(-96.0f, -96.0f, -96.0f, -96.0f);
            continue;
        }

        float pL = peakGetter(ch);
        float pR = stereo ? peakGetter(ch + 1) : pL;
        float vL = vuGetter(ch);
        float vR = stereo ? vuGetter(ch + 1) : vL;
        headerViews[i]->updateInputLevels(pL, pR, vL, vR);
    }
}

void TrackHeaderPanel::paint(juce::Graphics& g)
{
    g.fillAll(AppColours::headerBg);

    // right border
    g.setColour(AppColours::separator);
    g.drawLine((float)getWidth() - 1.0f, 0.0f,
               (float)getWidth() - 1.0f, (float)getHeight(), 1.0f);

    if (trackManager.getTrackCount() == 0)
    {
        g.setColour(AppColours::textDim);
        g.setFont(juce::FontOptions(12.0f));
        g.drawText("No tracks yet.",
                   0, rulerH + 28, getWidth(), 20, juce::Justification::centred);
    }

    // 並び替えドラッグ中のドロップ位置インジケータ (オレンジ太線)
    if (dragReorderStarted && dropTargetIndex >= 0)
    {
        int y;
        if (dropTargetIndex < (int) headerViews.size())
            y = headerViews[(size_t) dropTargetIndex]->getY();
        else if (!headerViews.empty())
            y = headerViews.back()->getBottom();
        else
            y = rulerH;
        g.setColour(AppColours::accent);
        g.fillRect(0, y - 1, getWidth() - 1, 3);
    }
}

void TrackHeaderPanel::resized()
{
    // ルーラーは最前面に出す。toFront でスクロール時もこの順序を保証
    rulerHeader.setBounds(0, 0, getWidth(), rulerH);
    rulerHeader.toFront(false);

    // トラック追加 "+" と FX (INS 開閉) ボタンを "TRACKS" ラベル下の左端に横並び配置する。
    // どちらも左アンカーなので、INS スロットを開いてヘッダ幅が広がってもボタンは動かない (ユーザ要望)。
    const int btnH   = 20;
    const int btnY   = rulerHeader.getHeight() - btnH - 4;
    const int plusW  = 24;
    const int fxW    = 32;
    const int btnGap = 4;
    addTrackPlus.setBounds(10, btnY, plusW, btnH);
    insToggleBtn.setBounds(10 + plusW + btnGap, btnY, fxW, btnH);

    addTrackBtn.setBounds(8, getHeight() - 34, getWidth() - 16, 26);

    const int listTop = rulerH - scrollY;
    int y = listTop;

    for (int i = 0; i < (int)headerViews.size(); ++i)
    {
        int h = trackManager.getTrack(i)->getTotalHeight();
        headerViews[(size_t)i]->setBounds(0, y, getWidth() - 1, h);
        y += h;
    }

    updateInsToggleState();
}

void TrackHeaderPanel::toggleAllInsertSlots()
{
    const int n = trackManager.getTrackCount();

    if (n == 0)
    {
        // トラックがまだ無い時もトグル意図だけ覚えておく。次にトラックを追加した
        // 時 (refresh) に INS 表示で出す。ここでは開く先のトラックが無いので幅は変わらない。
        insSlotsOn = !insSlotsOn;
        pendingInsApply = true;
        updateInsToggleState();
        return;
    }

    int onCount = 0;
    for (int i = 0; i < n; ++i)
        if (auto* t = trackManager.getTrack(i))
            if (t->isInsertSlotsVisible()) ++onCount;

    // 1 つも表示されていなければ全表示、そうでなければ全非表示にする
    const bool show = (onCount == 0);
    insSlotsOn = show;
    pendingInsApply = false;
    for (int i = 0; i < n; ++i)
        if (auto* t = trackManager.getTrack(i))
            t->setInsertSlotsVisible(show);

    // ヘッダ幅の再計算 + 全体レイアウト更新 (MainComponent::resized 経由)
    if (onTrackChanged) onTrackChanged();
    resized();
    repaint();
}

void TrackHeaderPanel::updateInsToggleState()
{
    const int n = trackManager.getTrackCount();
    int onCount = 0;
    for (int i = 0; i < n; ++i)
        if (auto* t = trackManager.getTrack(i))
            if (t->isInsertSlotsVisible()) ++onCount;

    // トラックがある時はボタンの点灯を実状態に同期する (右クリックや読込で変わるため)。
    // 0 本の時はユーザのトグル意図 (insSlotsOn) を保つ。
    if (n > 0)
        insSlotsOn = (onCount > 0);

    // FX ボタンはトラックの有無に依らず常に表示する (0 本でも押せば次のトラックを INS 表示で出す)
    insToggleBtn.setVisible(true);
    insToggleBtn.setActive(insSlotsOn);
    insToggleBtn.setTooltip(tr(insSlotsOn ? u8"INS スロットを非表示" : u8"INS スロットを表示"));
}

void TrackHeaderPanel::InsToggleButton::paintButton(juce::Graphics& g,
                                                    bool highlighted, bool down)
{
    auto bounds = getLocalBounds().toFloat().reduced(1.0f);

    juce::Colour bg = active ? AppColours::accent.withAlpha(0.22f) : AppColours::buttonBg;
    if (down)             bg = bg.brighter(0.12f);
    else if (highlighted) bg = bg.brighter(0.06f);
    g.setColour(bg);
    g.fillRoundedRectangle(bounds, 3.0f);
    g.setColour(active ? AppColours::accent.withAlpha(0.85f) : AppColours::separator);
    g.drawRoundedRectangle(bounds.reduced(0.5f), 3.0f, 1.0f);

    // "FX" ラベル (INS スロットの開閉。記号より分かりやすいテキスト表記)
    g.setColour(active ? AppColours::accent : AppColours::textDim);
    g.setFont(juce::FontOptions(11.0f, juce::Font::bold));
    g.drawText("FX", getLocalBounds(), juce::Justification::centred);
}
