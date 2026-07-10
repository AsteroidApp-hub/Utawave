// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

// TrackHeaderView のマウス・ドラッグ&ドロップ関連処理。
// mouseMove / mouseDown (INS スロット / コンテキストメニュー / トラック選択) /
// mouseDrag / mouseUp / findInsertSlotAt / itemDrag* (FX チップ D&D)。
// TrackHeaderView.cpp が肥大化したため分割。

#include "TrackHeaderView.h"
#include "../Localisation.h"
#include "../AppColours.h"
#include "../VST/PluginChain.h"
#include "../Audio/LufsMeter.h"

void TrackHeaderView::mouseMove(const juce::MouseEvent& e)
{
    setMouseCursor((isInResizeZone(e) || isInLaneResizeZone(e))
                   ? juce::MouseCursor::UpDownResizeCursor
                   : juce::MouseCursor::NormalCursor);
}

void TrackHeaderView::mouseDown(const juce::MouseEvent& e)
{
    // 右クリック: 波形ボタン → 内蔵シンセ ON/OFF をトグル
    if (e.mods.isRightButtonDown() && e.eventComponent == &waveformBtn
        && track.isMidiTrack())
    {
        editTrackUndoable([this] { track.setSynthEnabled(!track.isSynthEnabled()); });
        // refresh: ボタンの見た目を更新
        const bool on = track.isSynthEnabled();
        if (on)
        {
            const int w = track.getSynthWaveform();
            const char* names[] = { "Sin", "Saw", "Sqr" };
            waveformBtn.setButtonText(names[juce::jlimit(0, 2, w)]);
            waveformBtn.setColour(juce::TextButton::textColourOffId, AppColours::accent);
        }
        else
        {
            waveformBtn.setButtonText("Off");
            waveformBtn.setColour(juce::TextButton::textColourOffId, AppColours::textDim);
        }
        // 直後の mouseUp で onClick が走るので、サイクルさせないようフラグを立てる
        waveformRightClickHandled = true;
        if (onChanged) onChanged();
        return;
    }

    // 右クリック: FX チップ上か判定 (チップなら専用メニューを出す)
    if (e.mods.isRightButtonDown())
    {
        for (int i = 0; i < fxChips.size(); ++i)
        {
            // 既存プラグインのチップ上で右クリック → 操作メニュー
            if (e.eventComponent == fxChips[i] && track.getPluginChain().getPlugin(i) != nullptr)
            {
                juce::PopupMenu m;
                const bool bypassed = track.getPluginChain().isBypassed(i);
                const int  maxSlot  = Track::insertSlotCount - 1;
                m.addItem(1, tr(u8"エディタを開く"));
                m.addItem(2, bypassed ? tr(u8"バイパスを解除") : tr(u8"バイパス"));
                m.addSeparator();
                m.addItem(4, tr(u8"上のスロットへ移動"), /*enabled*/ i > 0);
                m.addItem(5, tr(u8"下のスロットへ移動"), /*enabled*/ i < maxSlot);
                m.addSeparator();
                m.addItem(3, tr(u8"削除"));
                const int slotIdx = i;
                // メニュー表示中に TrackHeaderPanel::refresh 等でこのビューが破棄されうるため
                // 生 this を捕捉せず SafePointer で生存確認する (クラッシュレポート id29 の UAF 対策)
                juce::Component::SafePointer<TrackHeaderView> safe(this);
                m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(fxChips[i]),
                    [safe, slotIdx](int result)
                    {
                        auto* self = safe.getComponent();
                        if (self == nullptr) return;
                        switch (result)
                        {
                            case 1: if (self->onPluginEditRequest) self->onPluginEditRequest(slotIdx); break;
                            case 2: if (self->onPluginBypassRequest) self->onPluginBypassRequest(slotIdx);
                                    break;
                            case 3: if (self->onPluginRemoveRequest) self->onPluginRemoveRequest(slotIdx); break;
                            case 4: if (self->onPluginSwapRequest)
                                        self->onPluginSwapRequest(slotIdx, slotIdx - 1);
                                    break;
                            case 5: if (self->onPluginSwapRequest)
                                        self->onPluginSwapRequest(slotIdx, slotIdx + 1);
                                    break;
                            default: break;
                        }
                    });
                return;
            }
        }
    }

    // 右クリック: トラックカラー / 削除メニュー（子コンポーネント上でも発火）
    if (e.mods.isRightButtonDown())
    {
        if (onSelected) onSelected();
        juce::PopupMenu m;
        juce::PopupMenu colourMenu;
        static const std::array<std::pair<const char*, juce::Colour>, 8> palette = {{
            {"Blue",   juce::Colour(0xff3a6ea5)},
            {"Green",  juce::Colour(0xff5aa55a)},
            {"Red",    juce::Colour(0xffa55a5a)},
            {"Orange", juce::Colour(0xffa5925a)},
            {"Purple", juce::Colour(0xff7a5aa5)},
            {"Cyan",   juce::Colour(0xff5a9ea5)},
            {"Pink",   juce::Colour(0xffa55a92)},
            {"Steel",  juce::Colour(0xff5a7aa5)}
        }};
        for (size_t i = 0; i < palette.size(); ++i)
        {
            juce::PopupMenu::Item item;
            item.itemID = 100 + (int)i;
            item.text   = palette[i].first;
            item.colour = palette[i].second;
            colourMenu.addItem(item);
        }
        m.addSubMenu(tr(u8"トラックカラー"), colourMenu);
        m.addSeparator();

        // ── プラグイン管理 (追加は INS スロットから行う) ──
        const int numFx = track.getPluginChain().getNumPlugins();
        if (numFx > 0)
        {
            juce::PopupMenu fxList;
            for (int i = 0; i < numFx; ++i)
            {
                if (auto* p = track.getPluginChain().getPlugin(i))
                {
                    juce::PopupMenu single;
                    single.addItem(400 + i, tr(u8"エディタを開く"));
                    single.addItem(500 + i, tr(u8"削除"));
                    fxList.addSubMenu(p->getName(), single);
                }
            }
            m.addSubMenu(tr(u8"インサート FX"), fxList);
        }
        const bool canMeasureLoudness = !track.isClickTrack() && !track.isMidiTrack();
        if (canMeasureLoudness)
        {
            m.addSeparator();
            m.addItem(600, tr(u8"ラウドネスを ") + juce::String(loudnessTargetLufs, 1)
                              + tr(u8" LUFS に合わせる"));
        }
        // ── フォルダ所属 (フォルダトラック以外・Click 以外) ──
        if (!track.isFolderTrack() && !track.isClickTrack() && getFolderTargets)
        {
            const auto folders = getFolderTargets();
            if (!folders.empty() || track.getFolderParent() != nullptr)
            {
                // 複数選択中はまとめて移動できることを示す (削除と同じ「選択スコープの本数」。
                // getDeleteCount は選択集合の解決を共有しているのでそのまま使う)
                const int scopeN = getDeleteCount ? getDeleteCount() : 1;
                const juce::String suffix = scopeN > 1 ? " (" + juce::String(scopeN) + ")"
                                                       : juce::String();
                m.addSeparator();
                if (!folders.empty())
                {
                    juce::PopupMenu folderMenu;
                    for (size_t i = 0; i < folders.size() && i < 100; ++i)
                        folderMenu.addItem(700 + (int)i, folders[i].first,
                                           /*enabled*/ true,
                                           /*ticked*/ track.getFolderParent() != nullptr
                                                      && folders[i].second >= 0
                                                      && getFolderParentIdx
                                                      && getFolderParentIdx() == folders[i].second);
                    m.addSubMenu(tr(u8"フォルダへ移動") + suffix, folderMenu);
                }
                if (track.getFolderParent() != nullptr)
                    m.addItem(699, tr(u8"フォルダから出す") + suffix);
            }
        }
        m.addSeparator();
        // 通常複製はテイクリストごと複製。Option (Win は Alt) 押下時は Lane 0 のみ複製する。
        // フォルダトラックは複製不可 (子は複製対象外のため項目を出さない)
        if (!track.isFolderTrack())
            m.addItem(201, tr(u8"トラックを複製")
                              + platformShortcutText(tr(u8" (Option: TList を除く)")));
        // 複数選択中はまとめて削除できることを示す (選択中の N 本)
        const int delCount = getDeleteCount ? getDeleteCount() : 1;
        m.addItem(200, delCount > 1
                          ? (tr(u8"選択中のトラックを削除") + " (" + juce::String(delCount) + ")")
                          : tr(u8"トラックを削除"));
        // マウス位置でポップアップ (デフォルトの withTargetComponent はコンポーネント基準で
        // 右クリックすると枠下に出てしまうため、現在のマウススクリーン座標を指定する)
        const auto mouseScr = juce::Desktop::getMousePosition();
        // メニュー表示中に TrackHeaderPanel::refresh 等でこのビューが破棄されうるため
        // 生 this を捕捉せず SafePointer で生存確認してからメンバ関数へ委譲する
        // (クラッシュレポート id29 の UAF 対策)
        juce::Component::SafePointer<TrackHeaderView> safe(this);
        m.showMenuAsync(juce::PopupMenu::Options()
                            .withTargetScreenArea({ mouseScr.x, mouseScr.y, 1, 1 }),
            [safe](int result) {
                if (auto* self = safe.getComponent())
                    self->handleTrackContextMenuResult(result);
            });
        return;
    }

    // 左クリックで FX チップ上 → ドラッグ候補（mouseDrag でしきい値超えたら startDragging）
    if (e.mods.isLeftButtonDown())
    {
        for (int i = 0; i < fxChips.size(); ++i)
        {
            if (e.eventComponent == fxChips[i] && track.getPluginChain().getPlugin(i) != nullptr)
            {
                dragSourceSlotIdx = i;
                dragStarted       = false;
                dragStartPos      = e.getPosition();
                // 選択も発火しておく
                if (onSelected) onSelected();
                return;
            }
        }
    }

    // トラックヘッダーをクリックで選択 (修飾キー付き → Shift / Cmd で複数選択)
    if (onSelectedWithMods) onSelectedWithMods(e.mods);
    else if (onSelected)    onSelected();

    if (isInResizeZone(e))
    {
        draggingResize    = true;
        draggingLaneResize = false;
        dragStartHeight   = track.getCustomHeight();
        dragStartScreenY  = e.getScreenY();
        return;
    }
    if (isInLaneResizeZone(e))
    {
        draggingResize    = false;
        draggingLaneResize = true;
        dragStartHeight   = track.getLaneHeight();
        dragStartScreenY  = e.getScreenY();
        return;
    }
    // 折りたたみ時 (TList OFF) はテイク行ボタンを描画していないので、ヒットテストもスキップ
    // (paint() の collapsed 早期 return と対称。見えないレーンの phantom ヒットを防ぐ)。
    if (track.isLanesCollapsed() || track.getLaneCount() <= 1) return;
    // ↑ (このテイクを採用) ボタン: ソロ判定より先に処理し、ヒット時は必ず return
    // (隣接する S ボタンやトラック選択/リサイズへフォールスルーさせない)。
    for (int li = 1; li < track.getLaneCount(); ++li)
    {
        if (getLanePromoteBtnRect(li).contains(e.x, e.y))
        {
            const bool canPromote = getLanePromoteEnabled && getLanePromoteEnabled(li);
            if (canPromote && onLanePromoteRequest) onLanePromoteRequest(li);
            return;
        }
    }
    for (int li = 1; li < track.getLaneCount(); ++li)
    {
        if (getLaneSoloBtnRect(li).contains(e.x, e.y))
        {
            if (auto* clickedLane = track.getLane(li))
            {
                bool wasSoloed = clickedLane->soloed;
                // 排他: 他のレーンのソロを全て解除
                for (int j = 1; j < track.getLaneCount(); ++j)
                    if (auto* l = track.getLane(j)) l->soloed = false;
                // クリック対象をトグル（既に ON なら OFF、それ以外は ON）
                clickedLane->soloed = !wasSoloed;
                repaint();
                if (onChanged) onChanged();
            }
            return;
        }
    }
}

// トラック右クリックメニューの結果処理。showMenuAsync のコールバックは SafePointer で
// 生存確認してからここへ委譲する (メニュー表示中にトラック削除/refresh でこのビューが
// 破棄されると、生 this キャプチャのラムダは UAF になるため)。
void TrackHeaderView::handleTrackContextMenuResult(int result)
{
    if (result >= 100 && result <= 107) {
        juce::Colour cols[8] = {
            juce::Colour(0xff3a6ea5), juce::Colour(0xff5aa55a),
            juce::Colour(0xffa55a5a), juce::Colour(0xffa5925a),
            juce::Colour(0xff7a5aa5), juce::Colour(0xff5a9ea5),
            juce::Colour(0xffa55a92), juce::Colour(0xff5a7aa5)
        };
        const juce::Colour newCol = cols[result - 100];
        editTrackUndoable([this, newCol] { track.setColour(newCol); });
        repaint();
        if (onChanged) onChanged();
    }
    else if (result == 200) {
        if (onDeleteRequest) onDeleteRequest();
    }
    else if (result == 201) {
        // Option (Mac) / Alt (Win) 押下中はテイクリストを複製しない (Lane 0 のみ)
        const bool excludeTakes = juce::ModifierKeys::getCurrentModifiers().isAltDown();
        if (onDuplicateRequest) onDuplicateRequest(!excludeTakes);
    }
    else if (result == 699) {
        if (onMoveToFolder) onMoveToFolder(-1);   // フォルダから出す
    }
    else if (result >= 700 && result < 800) {
        if (onMoveToFolder && getFolderTargets)
        {
            const auto folders = getFolderTargets();
            const size_t i = (size_t)(result - 700);
            if (i < folders.size()) onMoveToFolder(folders[i].second);
        }
    }
    else if (result >= 400 && result < 500) {
        if (onPluginEditRequest) onPluginEditRequest(result - 400);
    }
    else if (result >= 500 && result < 600) {
        if (onPluginRemoveRequest) onPluginRemoveRequest(result - 500);
    }
    else if (result == 600) {
        // ラウドネスを -18 LUFS に合わせる: lane 0 の全クリップを連結して測定
        auto* lane = track.getLane(0);
        if (lane == nullptr || lane->clips.empty())
        {
            juce::AlertWindow::showAsync(juce::MessageBoxOptions()
                .withIconType(juce::MessageBoxIconType::WarningIcon)
                .withTitle(tr(u8"ラウドネス測定"))
                .withMessage(tr(u8"このトラックには測定可能なクリップがありません。"))
                .withButton("OK"), nullptr);
            return;
        }

        auto& fmt = track.getFormatManager();
        // 最初に読めるクリップから sample rate / channels を取得
        double sr = 0.0; int numCh = 0;
        for (auto& c : lane->clips)
        {
            std::unique_ptr<juce::AudioFormatReader> r(fmt.createReaderFor(c->getFile()));
            if (r && r->sampleRate > 0.0 && r->numChannels > 0)
            {
                sr = r->sampleRate; numCh = (int) r->numChannels; break;
            }
        }
        if (sr <= 0.0 || numCh <= 0)
        {
            juce::AlertWindow::showAsync(juce::MessageBoxOptions()
                .withIconType(juce::MessageBoxIconType::WarningIcon)
                .withTitle(tr(u8"ラウドネス測定"))
                .withMessage(tr(u8"クリップを読み込めませんでした。"))
                .withButton("OK"), nullptr);
            return;
        }

        LufsMeter::Measurer measurer(sr, numCh);
        // 開始位置順にソートして測定
        std::vector<AudioClip*> sorted;
        for (auto& c : lane->clips) sorted.push_back(c.get());
        std::sort(sorted.begin(), sorted.end(),
                  [](AudioClip* a, AudioClip* b)
                  { return a->getStartPosition() < b->getStartPosition(); });

        for (auto* c : sorted)
        {
            std::unique_ptr<juce::AudioFormatReader> reader(fmt.createReaderFor(c->getFile()));
            if (!reader || reader->sampleRate <= 0.0) continue;
            const double clipSr = reader->sampleRate;
            const int    clipCh = (int) reader->numChannels;
            if (std::abs(clipSr - sr) > 1.0 || clipCh != numCh) continue;

            const juce::int64 startS = (juce::int64) juce::jmax(0.0, c->getFileOffset() * clipSr);
            const juce::int64 endBy  = (juce::int64) ((c->getFileOffset() + c->getDuration()) * clipSr);
            const juce::int64 endS   = juce::jmin(reader->lengthInSamples, endBy);
            if (endS <= startS) continue;

            measurer.resetFilters();
            const int chunk = juce::jmax(1024, (int)(clipSr * 0.05));
            juce::AudioBuffer<float> buf(clipCh, chunk);
            juce::int64 pos = startS;
            while (pos < endS)
            {
                const int n = (int) juce::jmin((juce::int64) chunk, endS - pos);
                buf.clear();
                reader->read(&buf, 0, n, pos, true, true);
                juce::AudioBuffer<float> view(buf.getArrayOfWritePointers(), clipCh, 0, n);
                measurer.processBuffer(view, c->getGain());
                pos += n;
            }
        }

        const double measuredLufs = measurer.getIntegratedLufs();
        if (!std::isfinite(measuredLufs))
        {
            juce::AlertWindow::showAsync(juce::MessageBoxOptions()
                .withIconType(juce::MessageBoxIconType::WarningIcon)
                .withTitle(tr(u8"ラウドネス測定"))
                .withMessage(tr(u8"ラウドネスを測定できませんでした。\nクリップが短すぎるか、無音が多すぎます。"))
                .withButton("OK"), nullptr);
            return;
        }

        // 現在の track Vol を反映した「出力ラウドネス」を表示用に算出
        const double currentVolDb    = (double) track.getVolume();
        const double effectiveLufs   = measuredLufs + currentVolDb;
        const double targetLufs      = (double) loudnessTargetLufs;
        const double newVolUnclamped = targetLufs - measuredLufs;
        const double newVolDb        = juce::jlimit(-60.0, 6.0, newVolUnclamped);
        const double adjustmentDb    = newVolDb - currentVolDb;

        auto fmt2 = [](double v) {
            return (v >= 0.0 ? juce::String("+") : juce::String())
                   + juce::String(v, 1);
        };
        juce::String msg = tr(u8"現在のラウドネス: ")
            + juce::String(effectiveLufs, 1) + " LUFS\n"
            + tr(u8"ターゲット: ") + juce::String(targetLufs, 1) + " LUFS\n"
            + tr(u8"トラック Vol: ") + fmt2(currentVolDb) + " dB → "
            + fmt2(newVolDb) + " dB (" + fmt2(adjustmentDb) + " dB)";
        if (std::abs(newVolUnclamped - newVolDb) > 0.05)
            msg += tr(u8"\n※ Vol の上限/下限により制限されました");
        msg += tr(u8"\n\n適用しますか?");

        juce::Component::SafePointer<TrackHeaderView> safe(this);
        juce::AlertWindow::showAsync(juce::MessageBoxOptions()
            .withIconType(juce::MessageBoxIconType::QuestionIcon)
            .withTitle(tr(u8"ラウドネス調整"))
            .withMessage(msg)
            .withButton(tr(u8"適用"))
            .withButton(tr(u8"キャンセル")),
            [safe, newVolDb](int r)
            {
                if (r != 1) return;
                auto* self = safe.getComponent();
                if (self == nullptr) return;
                self->getTrack().setVolume((float) newVolDb);
                self->volSlider.setValue(newVolDb, juce::dontSendNotification);
                if (self->onChanged) self->onChanged();
                self->repaint();
            });
    }
}

void TrackHeaderView::mouseDrag(const juce::MouseEvent& e)
{
    int delta = e.getScreenY() - dragStartScreenY;
    if (draggingResize)
    {
        const int newH = dragStartHeight + delta;
        // 複数選択時は Panel が全選択トラックへ同じ高さを追従反映する。未設定なら自トラックへ。
        if (onResizeTo) onResizeTo(newH);
        else            track.setCustomHeight(newH);
        if (onChanged) onChanged();
    }
    else if (draggingLaneResize)
    {
        track.setCustomLaneHeight(dragStartHeight + delta);
        if (onChanged) onChanged();
    }
    else if (dragSourceSlotIdx >= 0 && !dragStarted)
    {
        // FX チップから D&D 開始（ヒステリシスで微小移動を無視）
        if (e.getDistanceFromDragStart() > 6)
        {
            if (auto* container = juce::DragAndDropContainer::findParentDragContainerFor(this))
            {
                dragStarted = true;
                juce::var desc;
                if (auto* obj = new juce::DynamicObject())
                {
                    obj->setProperty("kind", "plugin");
                    obj->setProperty("trackIdx", trackIndex);
                    obj->setProperty("slotIdx",  dragSourceSlotIdx);
                    desc = juce::var(obj);
                }
                // ドラッグ画像: チップを軽く拡大して半透明に
                juce::Image dragImg;
                if (auto* chip = (dragSourceSlotIdx < fxChips.size() ? fxChips[dragSourceSlotIdx] : nullptr))
                {
                    dragImg = chip->createComponentSnapshot(chip->getLocalBounds())
                                  .convertedToFormat(juce::Image::ARGB);
                    dragImg.multiplyAllAlphas(0.85f);
                }
                container->startDragging(desc, this, juce::ScaledImage(dragImg),
                                         /*allowDraggingToExternalWindows*/ true);
            }
        }
    }
}

void TrackHeaderView::mouseUp(const juce::MouseEvent&)
{
    draggingResize     = false;
    draggingLaneResize = false;
    dragSourceSlotIdx  = -1;
    dragStarted        = false;
    setMouseCursor(juce::MouseCursor::NormalCursor);
}

int TrackHeaderView::findInsertSlotAt(juce::Point<int> localPos) const
{
    if (!insSlotsShown()) return -1;   // フォルダは表示設定 (showFolderPanRevIns) も見る
    const int w = getWidth();
    if (w <= controlsWidth) return -1;

    const int controlsW = controlsWidth;  // ローカルエイリアス (既存コードを最小変更)
    const int totalH = getHeight();
    const int laneCount = track.getLaneCount();
    const bool collapsed = track.isLanesCollapsed() || laneCount <= 1;
    const int mainH = collapsed ? totalH : track.getMainHeight();

    const int frameX = controlsW + 4;
    const int frameY = 0;
    const int frameW = w - controlsW - 8;
    const int frameH = juce::jmin(insFrameH, mainH);
    if (localPos.x < frameX || localPos.x >= frameX + frameW) return -1;
    if (localPos.y < frameY || localPos.y >= frameY + frameH) return -1;

    // スロット高は描画 (paint / resized) と同じ固定値 (insSlotH)。innerH/数で割ると
    // トラックが枠より低い時にスロットが圧縮され、描画 (固定高 + クリップ) とヒット判定が
    // ずれる (8 スロットで顕著)。可視範囲は上の frameH ガードが既に制限している。
    const int innerY = frameY + 13;
    const int slotH  = insSlotH;
    if (slotH <= 0) return -1;
    // 描画 (resized) と同じ「枠に収まるチップだけ表示」基準で可視スロット数を数え、それを超える
    // (= チップ非表示の) スロットへは落とさない。frameH の下端に数 px 残る「収まらないスロットの帯」
    // へ D&D で落として隠れスロットにプラグインが入り「消えた」ように見えるのを防ぐ。
    int visibleSlots = 0;
    while (visibleSlots < Track::insertSlotCount
           && innerY + visibleSlots * slotH + 1 + (slotH - 2) <= frameH)
        ++visibleSlots;
    if (visibleSlots <= 0) return -1;
    int idx = (localPos.y - innerY) / slotH;
    return juce::jlimit(0, visibleSlots - 1, idx);
}

bool TrackHeaderView::isInterestedInDragSource(const SourceDetails& d)
{
    if (auto* obj = d.description.getDynamicObject())
        return obj->hasProperty("kind") && obj->getProperty("kind").toString() == "plugin";
    return false;
}

void TrackHeaderView::itemDragEnter(const SourceDetails& d)
{
    itemDragMove(d);
}

void TrackHeaderView::itemDragMove(const SourceDetails& d)
{
    int newSlot = findInsertSlotAt(d.localPosition);
    if (newSlot != dropHighlightSlot)
    {
        dropHighlightSlot = newSlot;
        repaint();
    }
}

void TrackHeaderView::itemDragExit(const SourceDetails&)
{
    if (dropHighlightSlot != -1)
    {
        dropHighlightSlot = -1;
        repaint();
    }
}

void TrackHeaderView::itemDropped(const SourceDetails& d)
{
    const int targetSlot = findInsertSlotAt(d.localPosition);
    dropHighlightSlot = -1;
    repaint();
    if (targetSlot < 0) return;

    auto* obj = d.description.getDynamicObject();
    if (!obj) return;

    const int srcTrack = (int) obj->getProperty("trackIdx");  // -1 = マスター
    const int srcSlot  = (int) obj->getProperty("slotIdx");
    if (srcSlot < 0) return;

    // Option (Alt) でコピー、それ以外は移動
    const bool copy = juce::ModifierKeys::currentModifiers.isAltDown();

    if (srcTrack == trackIndex)
    {
        // 同一トラック内: 同じスロットへのドロップは無視
        if (srcSlot == targetSlot) return;
        // Option+ドラッグ: 同一トラックでもコピー（プラグイン複製）
        if (copy)
        {
            if (onPluginDropFromOtherTrack)
                onPluginDropFromOtherTrack(srcTrack, srcSlot, targetSlot, true);
            return;
        }
        // 通常ドラッグ: スロット入れ替え
        if (onPluginSwapRequest) onPluginSwapRequest(srcSlot, targetSlot);
        return;
    }

    // 他トラックまたはマスター(-1)からのドロップ
    if (onPluginDropFromOtherTrack)
        onPluginDropFromOtherTrack(srcTrack, srcSlot, targetSlot, copy);
}
