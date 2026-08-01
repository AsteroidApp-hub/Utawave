// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

#include "MasterPanel.h"
#include "../Localisation.h"
#include "../AppColours.h"
#include "../VST/PluginChain.h"
#include "Meter.h"

namespace {
juce::String shortenPluginName(const juce::String& full)
{
    auto s = full;
    if (s.length() > 18) s = s.substring(0, 17) + tr(u8"…");
    return s;
}
} // namespace

MasterPanel::MasterPanel()
{
    setOpaque(true);   // paint() が fillAll で全面を塗る (GPU コンポジット軽量化)

    masterLabel.setText("MASTER", juce::dontSendNotification);
    masterLabel.setFont(juce::FontOptions(10.0f).withStyle("Bold"));
    masterLabel.setColour(juce::Label::textColourId, AppColours::textDim);
    masterLabel.setJustificationType(juce::Justification::centred);
    // MASTER ラベル上の右クリックを MasterPanel に伝搬（メニュー表示用）
    masterLabel.addMouseListener(this, false);
    addAndMakeVisible(masterLabel);

    masterFader.setLookAndFeel(&laf);
    masterFader.setSliderStyle(juce::Slider::LinearVertical);
    masterFader.setTextBoxStyle(juce::Slider::NoTextBox, true, 0, 0);
    masterFader.setRange(minDb, maxDb, 0.1);
    masterFader.setValue(0.0);
    masterFader.setDoubleClickReturnValue(true, 0.0);
    masterFader.onValueChange = [this]
    {
        double db   = masterFader.getValue();
        gainLabel.setText(juce::String(db, 1) + " dB", juce::dontSendNotification);
        if (onMasterGainChanged)
            onMasterGainChanged(juce::Decibels::decibelsToGain((float)db));
    };
    addAndMakeVisible(masterFader);

    gainLabel.setText("0.0 dB", juce::dontSendNotification);
    gainLabel.setFont(juce::FontOptions(10.0f));
    gainLabel.setColour(juce::Label::textColourId, AppColours::accent);
    gainLabel.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(gainLabel);

    peakResetBtn.setButtonText("RESET");
    peakResetBtn.setColour(juce::TextButton::buttonColourId,  AppColours::buttonBg);
    peakResetBtn.setColour(juce::TextButton::textColourOffId, AppColours::textDim);
    peakResetBtn.onClick = [this] { resetPeakHold(); };
    peakResetBtn.setWantsKeyboardFocus(false);
    masterFader.setWantsKeyboardFocus(false);
    addAndMakeVisible(peakResetBtn);

    // 折りたたみトグル（左上）。"<" = 折りたたむ、">" = 展開
    collapseBtn.setButtonText("<");
    collapseBtn.setColour(juce::TextButton::buttonColourId,  AppColours::buttonBg);
    collapseBtn.setColour(juce::TextButton::textColourOffId, AppColours::textDim);
    collapseBtn.setWantsKeyboardFocus(false);
    collapseBtn.setTooltip(tr(u8"マスターパネルを折りたたむ"));
    collapseBtn.onClick = [this] { setCollapsed(!collapsed); };
    addAndMakeVisible(collapseBtn);
}

MasterPanel::~MasterPanel()
{
    masterFader.setLookAndFeel(nullptr);
    if (pluginChain) pluginChain->onChainChanged = nullptr;
}

void MasterPanel::setPluginChain(PluginChain* chain)
{
    if (pluginChain) pluginChain->onChainChanged = nullptr;
    pluginChain = chain;
    if (pluginChain)
    {
        juce::Component::SafePointer<MasterPanel> safeSelf(this);
        pluginChain->onChainChanged = [safeSelf]
        {
            juce::MessageManager::callAsync([safeSelf]
            {
                if (auto* self = safeSelf.getComponent())
                {
                    self->refreshChips();
                    self->resized();
                    self->repaint();
                }
            });
        };
    }
    refreshChips();
    resized();
    repaint();
}

void MasterPanel::setCollapsed(bool v)
{
    if (collapsed == v)
    {
        collapseBtn.setButtonText(v ? ">" : "<");
        collapseBtn.setTooltip(juce::String::fromUTF8(
            v ? u8"マスターパネルを展開" : u8"マスターパネルを折りたたむ"));
        return;
    }
    collapsed = v;
    // 折りたたみ時は本体コントロールを隠す
    const bool showContent = !collapsed;
    masterLabel.setVisible(showContent);
    masterFader.setVisible(showContent);
    gainLabel.setVisible(showContent);
    peakResetBtn.setVisible(showContent);
    for (auto* c : fxChips) c->setVisible(showContent && insertSlotsVisible);
    for (auto* sw : fxBypassBtns) if (sw) sw->setVisible(showContent && insertSlotsVisible);

    collapseBtn.setButtonText(collapsed ? ">" : "<");
    collapseBtn.setTooltip(juce::String::fromUTF8(
        collapsed ? u8"マスターパネルを展開" : u8"マスターパネルを折りたたむ"));

    if (onCollapseToggled) onCollapseToggled(collapsed);
    resized();
    repaint();
}

void MasterPanel::setInsertSlotsVisible(bool v)
{
    if (insertSlotsVisible == v) return;
    insertSlotsVisible = v;
    // チップ自身の可視性も切り替え
    for (auto* chip : fxChips)
        chip->setVisible(v);
    for (auto* sw : fxBypassBtns)
        if (sw) sw->setVisible(v);
    resized();
    repaint();
    if (onInsertSlotsVisibilityChanged) onInsertSlotsVisibilityChanged(v);
}

void MasterPanel::refreshChips()
{
    fxChips.clear();
    fxBypassBtns.clear();
    if (pluginChain == nullptr) return;

    // 再構築が折りたたみ中 / INS 非表示中に走っても表示状態を壊さない
    const bool showNow = !collapsed && insertSlotsVisible;

    for (int i = 0; i < insertSlotCount; ++i)
    {
        auto* btn = new juce::TextButton();
        btn->setWantsKeyboardFocus(false);

        auto* p = pluginChain->getPlugin(i);
        if (p != nullptr)
        {
            const bool bypassed = pluginChain->isBypassed(i);
            btn->setButtonText(shortenPluginName(p->getName()));
            btn->setColour(juce::TextButton::buttonColourId,
                           bypassed ? juce::Colour(0xff2a2d31) : juce::Colour(0xff2e4d7a));
            btn->setColour(juce::TextButton::buttonOnColourId,
                           bypassed ? juce::Colour(0xff35383d) : juce::Colour(0xff3a5a8a));
            btn->setColour(juce::TextButton::textColourOffId,
                           bypassed ? AppColours::textDim.withAlpha(0.7f) : juce::Colours::white);
            btn->setTooltip(p->getName()
                + (bypassed ? tr(u8"（バイパス中）") : juce::String()));
            const int slotIdx = i;
            btn->onClick = [this, slotIdx]
            {
                // Cmd+クリックでバイパス切替
                if (juce::ModifierKeys::currentModifiers.isCommandDown())
                {
                    if (onPluginBypassRequest) onPluginBypassRequest(slotIdx);
                    return;
                }
                if (onPluginEditRequest) onPluginEditRequest(slotIdx);
            };
            btn->addMouseListener(this, false);

            // チップ左のワンクリック・バイパススイッチ (LED ランプ)。
            // チップ左端の角丸を落として (ConnectedOnLeft) スイッチと一体の枠に見せる
            btn->setConnectedEdges(juce::Button::ConnectedOnLeft);
            auto* sw = new PluginBypassSwitch();
            sw->setBypassed(bypassed);
            sw->setTooltip(bypassed
                ? tr(u8"バイパス中（クリックでオンに戻す）")
                : tr(u8"クリックでバイパス（オフ）"));
            sw->onClick = [this, slotIdx]
            {
                if (onPluginBypassRequest) onPluginBypassRequest(slotIdx);
            };
            fxBypassBtns.add(sw);
            addAndMakeVisible(sw);
            sw->setVisible(showNow);
        }
        else
        {
            btn->setButtonText("+");
            btn->setColour(juce::TextButton::buttonColourId, juce::Colour(0x00000000));
            btn->setColour(juce::TextButton::buttonOnColourId, juce::Colour(0x30ffffff));
            btn->setColour(juce::TextButton::textColourOffId,
                           AppColours::textDim.withAlpha(0.55f));
            btn->setTooltip(platformShortcutText(tr(u8"クリックでプラグイン追加（Cmd+クリックでバイパス）")));
            const int slotIdx = i;
            btn->onClick = [this, slotIdx]
            {
                if (onPluginAddRequest) onPluginAddRequest(slotIdx);
            };
            // 空きスロットはスイッチ無し (index を fxChips と揃えるため nullptr を積む)
            fxBypassBtns.add(nullptr);
        }

        fxChips.add(btn);
        addAndMakeVisible(btn);
        btn->setVisible(showNow);
    }
}

void MasterPanel::setLevels(float pL, float pR, float vL, float vR, float hL, float hR)
{
    // 変化が小さければ repaint をスキップ（dB で 0.3 未満の差は知覚困難）
    auto eq = [](float a, float b) { return std::abs(a - b) < 0.3f; };
    if (eq(pL, peakLdb) && eq(pR, peakRdb)
        && eq(vL, vuLdb) && eq(vR, vuRdb)
        && eq(hL, holdLdb) && eq(hR, holdRdb))
        return;

    peakLdb = pL; peakRdb = pR;
    vuLdb   = vL; vuRdb   = vR;
    holdLdb = hL; holdRdb = hR;
    repaint();
}

void MasterPanel::resetPeakHold()
{
    holdLdb = -96.0f;
    holdRdb = -96.0f;
    // engine 側の保持値もリセットしないと次の setLevels で戻ってしまう
    if (onResetPeakHold) onResetPeakHold();
    repaint();
}

int MasterPanel::dbToY(float db, juce::Rectangle<int> b) const
{
    float norm = (db - minDb) / (maxDb - minDb);
    return b.getBottom() - (int)(b.getHeight() * juce::jlimit(0.0f, 1.0f, norm));
}

juce::Rectangle<int> MasterPanel::getInsertsArea() const
{
    // INS 枠は MASTER ラベル直下に配置（メーターはフェーダー横へ移動した）
    const int x = 6;
    const int y = 28;
    const int w = getWidth() - 12;
    const int h = insertSlotsVisible ? 130 : 0;
    return { x, y, w, h };
}

juce::Rectangle<int> MasterPanel::getInsertsInnerArea() const
{
    auto a = getInsertsArea();
    // タイトル分の高さを引いた内側
    return { a.getX() + 4, a.getY() + 13, a.getWidth() - 8, a.getHeight() - 14 };
}

int MasterPanel::findInsertSlotAt(juce::Point<int> localPos) const
{
    auto inner = getInsertsInnerArea();
    if (!inner.contains(localPos)) return -1;
    const int slotH = inner.getHeight() / insertSlotCount;
    if (slotH <= 0) return -1;
    int idx = (localPos.y - inner.getY()) / slotH;
    return juce::jlimit(0, insertSlotCount - 1, idx);
}

void MasterPanel::drawMeter(juce::Graphics& g, juce::Rectangle<int> b, float db, float holdDb)
{
    // 共通ヘルパに委譲 (TrackHeaderView と同じ色基準・基準線描画を一元化)
    MeterDraw::drawVertical(g, b, db, holdDb, vuReferenceLevel, minDb, maxDb);
}

void MasterPanel::drawDbScale(juce::Graphics& g, juce::Rectangle<int> faderBounds)
{
    g.setFont(juce::FontOptions(8.5f));

    struct Mark { float db; bool major; };
    const Mark marks[] = {
        {  6.0f, true  },
        {  0.0f, true  },
        { -6.0f, true  },
        {-12.0f, true  },
        {-18.0f, false },
        {-24.0f, true  },
        {-36.0f, false },
        {-48.0f, true  },
        {-60.0f, true  }
    };

    for (auto& m : marks)
    {
        int lineY = dbToY(m.db, faderBounds);
        g.setColour(m.major ? AppColours::separatorLight : AppColours::separator);
        g.drawHorizontalLine(lineY,
                             (float)(faderBounds.getX() - 6),
                             (float)faderBounds.getX());
        if (m.major)
        {
            g.setColour(m.db == 0.0f ? AppColours::textBright : AppColours::textDim);
            juce::String label = (m.db > 0 ? "+" : "") + juce::String((int)m.db);
            g.drawText(label,
                       faderBounds.getX() - 22, lineY - 5, 20, 10,
                       juce::Justification::centredRight);
        }
    }

    int zeroY = dbToY(0.0f, faderBounds);
    g.setColour(AppColours::accent.withAlpha(0.4f));
    g.drawHorizontalLine(zeroY,
                         (float)faderBounds.getX(),
                         (float)faderBounds.getRight());
}

void MasterPanel::paint(juce::Graphics& g)
{
    g.fillAll(AppColours::headerBg);
    g.setColour(AppColours::separator);
    g.drawLine(0.0f, 0.0f, 0.0f, (float)getHeight(), 1.0f);

    // 折りたたみ時: 縦書き "MASTER" + 細い 4 本メーター
    // メーターの Y 範囲は展開時と同じ計算式で算出し、開閉でメーター高さを一致させる
    if (collapsed)
    {
        const int rowTop    = (insertSlotsVisible ? getInsertsArea().getBottom() : 28) + 8;
        const int rowBottom = getHeight() - 52 - 16 - 6;
        const int labelsH   = 12 + 1 + 9 + 2;
        const int meterTop  = rowTop + labelsH;
        const int meterH    = juce::jmax(40, rowBottom - meterTop);

        // "MASTER" 縦書き: メーター直上に詰めて配置（大きな空白を作らない）
        g.setColour(AppColours::textDim.withAlpha(0.6f));
        g.setFont(juce::FontOptions(10.0f).withStyle("Bold"));
        juce::String label = "MASTER";
        const int charH      = 12;
        const int labelTotal = label.length() * charH;
        // メーター開始の少し上から下に向かって描画。先頭が collapseBtn (y<=24) より下なら表示
        const int yStart = meterTop - 6 - labelTotal;
        if (yStart >= 28)
        {
            for (int i = 0; i < label.length(); ++i)
                g.drawText(label.substring(i, i + 1),
                           0, yStart + i * charH, getWidth(), charH,
                           juce::Justification::centred);
        }

        // 細メーター: Peak L / Peak R / VU L / VU R
        const int barW    = 3;
        const int innerGp = 1;
        const int groupGp = 3;
        const int totalW  = barW * 4 + innerGp * 2 + groupGp;
        const int startX  = (getWidth() - totalW) / 2;
        const int peakLX  = startX;
        const int peakRX  = peakLX + barW + innerGp;
        const int vuLX    = peakRX + barW + groupGp;
        const int vuRX    = vuLX  + barW + innerGp;

        drawMeter(g, { peakLX, meterTop, barW, meterH }, peakLdb, holdLdb);
        drawMeter(g, { peakRX, meterTop, barW, meterH }, peakRdb, holdRdb);
        drawMeter(g, { vuLX,   meterTop, barW, meterH }, vuLdb,   -96.0f);
        drawMeter(g, { vuRX,   meterTop, barW, meterH }, vuRdb,   -96.0f);
        return;
    }

    // ── メーターレイアウト計算（PEAK | フェーダー | VU を横並び）──
    const int rowTop    = (insertSlotsVisible ? getInsertsArea().getBottom() : 28) + 8;
    const int clipH     = 16;
    const int clipGap   = 6;
    const int rowBottom = getHeight() - 52 - clipH - clipGap;

    const int sectLabelH = 12;   // "PEAK" / "VU"
    const int subLabelH  = 9;    // "L" / "R"
    const int labelsH    = sectLabelH + 1 + subLabelH + 2;  // 24
    const int meterTop   = rowTop + labelsH;
    const int meterH     = juce::jmax(40, rowBottom - meterTop);

    // 横方向: [Peak L | Peak R]  [Fader]  [VU L | VU R]  をセンタリング
    const int meterColW   = 16;
    const int meterColGap = 3;
    const int sectionW    = meterColW * 2 + meterColGap;
    const int faderW      = 28;
    const int sectionGap  = 8;
    const int totalW      = sectionW + sectionGap + faderW + sectionGap + sectionW;
    const int startX      = (getWidth() - totalW) / 2;
    const int peakX       = startX;
    const int vuX         = startX + sectionW + sectionGap + faderW + sectionGap;

    // PEAK セクション
    g.setColour(AppColours::textDim);
    g.setFont(juce::FontOptions(9.0f));
    g.drawText("PEAK", peakX, rowTop, sectionW, sectLabelH, juce::Justification::centred);
    g.drawText("L", peakX,                       rowTop + sectLabelH + 1, meterColW, subLabelH, juce::Justification::centred);
    g.drawText("R", peakX + meterColW + meterColGap, rowTop + sectLabelH + 1, meterColW, subLabelH, juce::Justification::centred);
    drawMeter(g, { peakX,                          meterTop, meterColW, meterH }, peakLdb, holdLdb);
    drawMeter(g, { peakX + meterColW + meterColGap, meterTop, meterColW, meterH }, peakRdb, holdRdb);

    // VU セクション
    g.setColour(AppColours::textDim);
    g.setFont(juce::FontOptions(9.0f));
    g.drawText("VU", vuX, rowTop, sectionW, sectLabelH, juce::Justification::centred);
    g.drawText("L", vuX,                         rowTop + sectLabelH + 1, meterColW, subLabelH, juce::Justification::centred);
    g.drawText("R", vuX + meterColW + meterColGap, rowTop + sectLabelH + 1, meterColW, subLabelH, juce::Justification::centred);
    drawMeter(g, { vuX,                          meterTop, meterColW, meterH }, vuLdb, -96.0f);
    drawMeter(g, { vuX + meterColW + meterColGap, meterTop, meterColW, meterH }, vuRdb, -96.0f);

    // CLIP インジケータ（メーター行の下）
    const int clipY = rowBottom + clipGap;
    const int clipX = 6;
    const int clipBoxW = getWidth() - 12;
    // 0 dBFS 以上に達したら点灯 = 真のピークオーバー。RESET まで保持。
    const bool clipping = holdLdb >= 0.0f || holdRdb >= 0.0f;
    g.setColour(clipping ? AppColours::meterRed : AppColours::buttonBg);
    g.fillRoundedRectangle((float)clipX, (float)clipY, (float)clipBoxW, (float)clipH, 2.0f);
    g.setColour(clipping ? juce::Colours::white : AppColours::textDim);
    g.setFont(juce::FontOptions(9.0f));
    g.drawText(clipping ? "CLIP!" : "OK", clipX, clipY, clipBoxW, clipH, juce::Justification::centred);

    // ── INS スロット枠（トラック側と同じスタイル）──
    auto frame = getInsertsArea();
    if (insertSlotsVisible && frame.getHeight() > 30)
    {
        g.setColour(juce::Colour(0xff1c1f24));
        g.fillRoundedRectangle(frame.toFloat(), 4.0f);
        g.setColour(juce::Colour(0xff3a3d42));
        g.drawRoundedRectangle(frame.toFloat().reduced(0.5f), 4.0f, 1.0f);

        g.setColour(AppColours::textDim.withAlpha(0.7f));
        g.setFont(juce::FontOptions(9.0f, juce::Font::bold));
        g.drawText("INSERTS", frame.getX() + 6, frame.getY() + 1,
                   frame.getWidth() - 12, 12, juce::Justification::centredLeft);

        auto inner = getInsertsInnerArea();
        const int slotH = inner.getHeight() / insertSlotCount;
        g.setColour(juce::Colour(0xff2a2d31));
        for (int i = 1; i < insertSlotCount; ++i)
        {
            const int yy = inner.getY() + i * slotH;
            g.fillRect(inner.getX() + 2, yy, inner.getWidth() - 4, 1);
        }

        // D&D ハイライト
        if (dropHighlightSlot >= 0 && dropHighlightSlot < insertSlotCount && slotH > 0)
        {
            const int hy = inner.getY() + dropHighlightSlot * slotH;
            const bool copyMode = juce::ModifierKeys::currentModifiers.isAltDown();
            const auto col = copyMode ? juce::Colour(0xff44dd88) : AppColours::accent;
            g.setColour(col.withAlpha(0.18f));
            g.fillRoundedRectangle((float)inner.getX(), (float)(hy + 1),
                                   (float)inner.getWidth(), (float)(slotH - 2), 3.0f);
            g.setColour(col.withAlpha(0.85f));
            g.drawRoundedRectangle((float)inner.getX() + 0.5f, (float)(hy + 1) + 0.5f,
                                   (float)inner.getWidth() - 1.0f, (float)(slotH - 2) - 1.0f,
                                   3.0f, 1.5f);
        }
    }
}

void MasterPanel::resized()
{
    // 折りたたみトグル: 左上 16x18 程度
    if (collapsed)
        collapseBtn.setBounds((getWidth() - 18) / 2, 6, 18, 18);
    else
        collapseBtn.setBounds(6, 6, 18, 18);

    if (collapsed)
    {
        // 折りたたみ時はトグル以外は配置不要
        return;
    }

    masterLabel.setBounds(0, 4, getWidth(), 18);

    // フェーダーは PEAK と VU の間（中央）に配置。高さはメーターと揃える
    const int rowTop    = (insertSlotsVisible ? getInsertsArea().getBottom() : 28) + 8;
    const int rowBottom = getHeight() - 52 - 16 - 6;
    const int labelsH   = 12 + 1 + 9 + 2;
    const int meterTop  = rowTop + labelsH;
    const int meterH    = juce::jmax(40, rowBottom - meterTop);

    const int meterColW   = 16;
    const int meterColGap = 3;
    const int sectionW    = meterColW * 2 + meterColGap;
    const int faderW      = 28;
    const int sectionGap  = 8;
    const int totalW      = sectionW + sectionGap + faderW + sectionGap + sectionW;
    const int startX      = (getWidth() - totalW) / 2;
    const int faderX      = startX + sectionW + sectionGap;

    masterFader.setBounds(faderX, meterTop, faderW, meterH);

    const int bottom = getHeight() - 52;
    gainLabel.setBounds(0, bottom + 2, getWidth(), 16);
    peakResetBtn.setBounds(6, getHeight() - 26, getWidth() - 12, 20);

    // INS チップの配置 (プラグイン入りスロットは左端にバイパススイッチを敷く)
    auto inner = getInsertsInnerArea();
    const int slotH = inner.getHeight() / insertSlotCount;
    const int padX  = 2;
    for (int i = 0; i < fxChips.size(); ++i)
    {
        const int chipY = inner.getY() + i * slotH + 1;
        const int chipH = juce::jmax(0, slotH - 2);
        int cx = inner.getX() + padX;
        int cw = inner.getWidth() - padX * 2;
        auto* sw = (i < fxBypassBtns.size() ? fxBypassBtns[i] : nullptr);
        if (sw)
        {
            // 隙間ゼロ = チップと一体の枠の左セグメント
            const int swW = 16;
            sw->setBounds(cx, chipY, swW, chipH);
            cx += swW;
            cw -= swW;
        }
        fxChips[i]->setBounds(cx, chipY, cw, chipH);
    }
}

void MasterPanel::mouseDown(const juce::MouseEvent& e)
{
    // 右クリック: MASTER ラベル領域（y < 28, メーター開始前）→ 表示切替メニュー
    if (e.mods.isRightButtonDown()
        && (e.eventComponent == this || e.eventComponent == &masterLabel)
        && e.getPosition().y < 28)
    {
        juce::PopupMenu m;
        m.addItem(1, tr(u8"INS スロットを表示"),   /*enabled*/ true, /*ticked*/ insertSlotsVisible);
        m.addItem(2, tr(u8"INS スロットを非表示"), /*enabled*/ true, /*ticked*/ !insertSlotsVisible);
        const auto screenPt = e.getScreenPosition();
        const juce::Rectangle<int> targetArea(screenPt.x, screenPt.y, 1, 1);
        m.showMenuAsync(juce::PopupMenu::Options().withTargetScreenArea(targetArea),
            [this](int result)
            {
                if (result == 1 || result == 2)
                    setInsertSlotsVisible(result == 1);
            });
        return;
    }

    // 右クリック: FX チップ上 → 操作メニュー
    if (e.mods.isRightButtonDown() && pluginChain != nullptr)
    {
        for (int i = 0; i < fxChips.size(); ++i)
        {
            if (e.eventComponent == fxChips[i] && pluginChain->getPlugin(i) != nullptr)
            {
                juce::PopupMenu m;
                const bool bypassed = pluginChain->isBypassed(i);
                const int  maxSlot  = insertSlotCount - 1;
                m.addItem(1, tr(u8"エディタを開く"));
                m.addItem(2, bypassed ? tr(u8"バイパスを解除") : tr(u8"バイパス"));
                m.addSeparator();
                m.addItem(4, tr(u8"上のスロットへ移動"), /*enabled*/ i > 0);
                m.addItem(5, tr(u8"下のスロットへ移動"), /*enabled*/ i < maxSlot);
                m.addSeparator();
                m.addItem(3, tr(u8"削除"));
                const int slotIdx = i;
                m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(fxChips[i]),
                    [this, slotIdx](int result)
                    {
                        switch (result)
                        {
                            case 1: if (onPluginEditRequest) onPluginEditRequest(slotIdx); break;
                            case 2: if (onPluginBypassRequest) onPluginBypassRequest(slotIdx);
                                    break;
                            case 3: if (onPluginRemoveRequest) onPluginRemoveRequest(slotIdx); break;
                            case 4: if (onPluginSwapRequest) onPluginSwapRequest(slotIdx, slotIdx - 1); break;
                            case 5: if (onPluginSwapRequest) onPluginSwapRequest(slotIdx, slotIdx + 1); break;
                            default: break;
                        }
                    });
                return;
            }
        }
    }

    // 左クリックで FX チップ上 → ドラッグ候補
    if (e.mods.isLeftButtonDown() && pluginChain != nullptr)
    {
        for (int i = 0; i < fxChips.size(); ++i)
        {
            if (e.eventComponent == fxChips[i] && pluginChain->getPlugin(i) != nullptr)
            {
                dragSourceSlotIdx = i;
                dragStarted = false;
                return;
            }
        }
    }
}

void MasterPanel::mouseDrag(const juce::MouseEvent& e)
{
    if (dragSourceSlotIdx >= 0 && !dragStarted && pluginChain != nullptr)
    {
        if (e.getDistanceFromDragStart() > 6)
        {
            if (auto* container = juce::DragAndDropContainer::findParentDragContainerFor(this))
            {
                dragStarted = true;
                juce::var desc;
                if (auto* obj = new juce::DynamicObject())
                {
                    obj->setProperty("kind", "plugin");
                    obj->setProperty("trackIdx", kMasterIndex);
                    obj->setProperty("slotIdx",  dragSourceSlotIdx);
                    desc = juce::var(obj);
                }
                juce::Image dragImg;
                if (auto* chip = (dragSourceSlotIdx < fxChips.size() ? fxChips[dragSourceSlotIdx] : nullptr))
                {
                    dragImg = chip->createComponentSnapshot(chip->getLocalBounds())
                                  .convertedToFormat(juce::Image::ARGB);
                    dragImg.multiplyAllAlphas(0.85f);
                }
                container->startDragging(desc, this, juce::ScaledImage(dragImg), true);
            }
        }
    }
}

void MasterPanel::mouseUp(const juce::MouseEvent&)
{
    dragSourceSlotIdx = -1;
    dragStarted = false;
}

bool MasterPanel::isInterestedInDragSource(const SourceDetails& d)
{
    if (auto* obj = d.description.getDynamicObject())
        return obj->hasProperty("kind") && obj->getProperty("kind").toString() == "plugin";
    return false;
}

void MasterPanel::itemDragEnter(const SourceDetails& d) { itemDragMove(d); }

void MasterPanel::itemDragMove(const SourceDetails& d)
{
    int s = findInsertSlotAt(d.localPosition);
    if (s != dropHighlightSlot) { dropHighlightSlot = s; repaint(); }
}

void MasterPanel::itemDragExit(const SourceDetails&)
{
    if (dropHighlightSlot != -1) { dropHighlightSlot = -1; repaint(); }
}

void MasterPanel::itemDropped(const SourceDetails& d)
{
    const int targetSlot = findInsertSlotAt(d.localPosition);
    dropHighlightSlot = -1;
    repaint();
    if (targetSlot < 0) return;

    auto* obj = d.description.getDynamicObject();
    if (!obj) return;

    const int srcTrack = (int) obj->getProperty("trackIdx");
    const int srcSlot  = (int) obj->getProperty("slotIdx");
    if (srcSlot < 0) return;

    const bool copy = juce::ModifierKeys::currentModifiers.isAltDown();

    if (srcTrack == kMasterIndex)
    {
        if (srcSlot == targetSlot) return;
        // Option+ドラッグ: マスター内でもコピー（プラグイン複製）
        if (copy)
        {
            if (onPluginDropFromOtherTrack)
                onPluginDropFromOtherTrack(kMasterIndex, srcSlot, targetSlot, true);
            return;
        }
        // 通常ドラッグ: スロット入れ替え
        if (onPluginSwapRequest) onPluginSwapRequest(srcSlot, targetSlot);
        return;
    }

    // 他トラックからマスターへドロップ
    if (onPluginDropFromOtherTrack)
        onPluginDropFromOtherTrack(srcTrack, srcSlot, targetSlot, copy);
}
