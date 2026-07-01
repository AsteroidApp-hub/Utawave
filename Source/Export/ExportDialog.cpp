// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

#include "ExportDialog.h"
#include "../Localisation.h"
#include "../AppColours.h"
#include "../UI/UtawaveLookAndFeel.h"

ExportDialog::ExportDialog(const Context& ctx) : context(ctx)
{
    // ツールチップを UtawaveLookAndFeel で描画する（角丸 + アクセント枠 + 余白）。
    // アプリはグローバル既定 LnF を設定していないため、各 TooltipWindow に明示適用する。
    // 静的インスタンスは全 TooltipWindow より長生きするのでダングリングしない。
    static UtawaveLookAndFeel tooltipLnF;
    tooltipWindow.setLookAndFeel(&tooltipLnF);

    titleLabel.setText(tr(u8"書き出し"), juce::dontSendNotification);
    titleLabel.setFont(juce::FontOptions(18.0f, juce::Font::bold));
    titleLabel.setColour(juce::Label::textColourId, juce::Colours::white);
    addAndMakeVisible(titleLabel);

    auto styleField = [](juce::Label& l, const juce::String& t)
    {
        l.setText(t, juce::dontSendNotification);
        l.setFont(juce::FontOptions(12.0f));
        l.setColour(juce::Label::textColourId, juce::Colour(0xffb0b6bd));
    };
    styleField(rangeLabel,      tr(u8"範囲"));
    styleField(formatLabel,     tr(u8"フォーマット"));
    styleField(sampleRateLabel, tr(u8"サンプルレート"));
    styleField(bitDepthLabel,   tr(u8"ビット深度"));
    styleField(nameLabel,       tr(u8"ファイル名（拡張子なし）"));
    styleField(tracksLabel,     tr(u8"書き出すトラック"));
    styleField(folderLabel,     tr(u8"出力フォルダ"));

    addAndMakeVisible(rangeLabel);
    addAndMakeVisible(formatLabel);
    addAndMakeVisible(sampleRateLabel);
    addAndMakeVisible(bitDepthLabel);
    addAndMakeVisible(nameLabel);
    addAndMakeVisible(tracksLabel);
    addAndMakeVisible(folderLabel);

    // RTZ ボタン風の塗りボタンを 3つラジオ動作させる
    const int rangeGroup = 2001;
    auto styleRangeBtn = [rangeGroup](juce::TextButton& b)
    {
        b.setColour(juce::TextButton::buttonColourId,   AppColours::buttonBg);
        b.setColour(juce::TextButton::buttonOnColourId, AppColours::accent);
        b.setColour(juce::TextButton::textColourOffId,  AppColours::textDim);
        b.setColour(juce::TextButton::textColourOnId,   juce::Colours::white);
        b.setClickingTogglesState(true);
        b.setRadioGroupId(rangeGroup);
        b.setWantsKeyboardFocus(false);
        b.setConnectedEdges(juce::Button::ConnectedOnLeft | juce::Button::ConnectedOnRight);
    };
    styleRangeBtn(rangeProjectBtn);
    styleRangeBtn(rangeRulerBtn);
    styleRangeBtn(rangeBarsBtn);
    // 連結時の端を矯正（先頭は左角丸、末尾は右角丸、間は両連結）
    rangeProjectBtn.setConnectedEdges(juce::Button::ConnectedOnRight);
    rangeBarsBtn.setConnectedEdges(juce::Button::ConnectedOnLeft);

    rangeProjectBtn.setToggleState(true, juce::dontSendNotification);
    rangeRulerBtn.setEnabled(context.rulerAvailable);
    if (! context.rulerAvailable)
        rangeRulerBtn.setTooltip(tr(u8"ルーラー（小節バー）を横ドラッグして範囲を指定してください"));
    addAndMakeVisible(rangeProjectBtn);
    addAndMakeVisible(rangeRulerBtn);
    addAndMakeVisible(rangeBarsBtn);

    // 範囲ボタンの切替で範囲入力欄の表示/非表示を更新
    rangeProjectBtn.onClick = [this] { updateRangeVisibility(); };
    rangeRulerBtn  .onClick = [this] { updateRangeVisibility(); };
    rangeBarsBtn   .onClick = [this] { updateRangeVisibility(); };

    // ルーラー範囲: 現在のルーラー範囲を読み取り専用で表示
    rulerRangeLabel.setJustificationType(juce::Justification::centredLeft);
    rulerRangeLabel.setColour(juce::Label::textColourId, juce::Colour(0xffb0b6bd));
    rulerRangeLabel.setText(context.rulerRangeDesc, juce::dontSendNotification);
    addChildComponent(rulerRangeLabel);

    // 小節範囲: 開始 / 終了小節の入力
    styleField(barStartLabel, tr(u8"開始小節"));
    styleField(barEndLabel,   tr(u8"終了小節"));
    addChildComponent(barStartLabel);
    addChildComponent(barEndLabel);
    barRangeSepLabel.setText(juce::String::fromUTF8(u8"〜"), juce::dontSendNotification);
    barRangeSepLabel.setJustificationType(juce::Justification::centred);
    barRangeSepLabel.setColour(juce::Label::textColourId, juce::Colour(0xffb0b6bd));
    addChildComponent(barRangeSepLabel);

    auto styleBarEditor = [](juce::TextEditor& ed)
    {
        ed.setJustification(juce::Justification::centred);
        ed.setIndents(2, 2);
        ed.setFont(juce::FontOptions(15.0f));
        ed.setInputRestrictions(6, "0123456789");
    };
    styleBarEditor(barStartEditor);
    styleBarEditor(barEndEditor);
    barStartEditor.setText("1", juce::dontSendNotification);
    barEndEditor.setText(juce::String(juce::jmax(1, context.projectEndBar)),
                         juce::dontSendNotification);
    addChildComponent(barStartEditor);
    addChildComponent(barEndEditor);

    formatBox.addItem("WAV",  1);
    formatBox.addItem("AIFF", 2);
    formatBox.addItem("MP3",  3);
    formatBox.setSelectedId(1, juce::dontSendNotification);
    addAndMakeVisible(formatBox);

    // MP3 ビットレート (kbps)
    styleField(mp3BitrateLabel, tr(u8"MP3 ビットレート"));
    addAndMakeVisible(mp3BitrateLabel);
    for (int k : { 96, 128, 160, 192, 224, 256, 320 })
        mp3BitrateBox.addItem(juce::String(k) + " kbps", k);
    mp3BitrateBox.setSelectedId(192, juce::dontSendNotification);
    addAndMakeVisible(mp3BitrateBox);

    sampleRateBox.addItem(tr(u8"プロジェクト")
                          + " (" + juce::String((int)context.projectSr) + " Hz)", 1);
    for (int sr : { 44100, 48000, 88200, 96000, 192000 })
        sampleRateBox.addItem(juce::String(sr) + " Hz", sr);
    sampleRateBox.setSelectedId(1, juce::dontSendNotification);
    addAndMakeVisible(sampleRateBox);

    bitDepthBox.addItem(tr(u8"16 bit"),       16);
    bitDepthBox.addItem(tr(u8"24 bit"),       24);
    bitDepthBox.addItem(tr(u8"32 bit float"), 32);
    bitDepthBox.setSelectedId(juce::jmax(16, context.projectBits), juce::dontSendNotification);
    addAndMakeVisible(bitDepthBox);

    ditherBtn.setToggleState(true, juce::dontSendNotification);
    ditherBtn.setColour(juce::ToggleButton::textColourId, juce::Colours::white);
    ditherBtn.setTooltip(tr(u8"16/24bit で書き出すときに加える微小なノイズです。量子化で生じる"
                            u8"ざらつき（特にフェードや小音量部の歪み）を聞こえにくい滑らかな"
                            u8"ノイズに変え、より自然な音にします。通常はオンのまま推奨。"
                            u8"32bit float では不要なため自動でオフになります。"));
    addAndMakeVisible(ditherBtn);

    autoRenameBtn.setToggleState(true, juce::dontSendNotification);
    autoRenameBtn.setColour(juce::ToggleButton::textColourId, juce::Colours::white);
    addAndMakeVisible(autoRenameBtn);

    revealAfterBtn.setToggleState(true, juce::dontSendNotification);
    revealAfterBtn.setColour(juce::ToggleButton::textColourId, juce::Colours::white);
    addAndMakeVisible(revealAfterBtn);

    // 実時間レンダリングは歌い手用途では使わないため非表示（常にオフライン書き出し）。
    // 互換のため getOptions() は常に false を返す（realtimeBtn は addAndMakeVisible しない）。
    realtimeBtn.setToggleState(false, juce::dontSendNotification);

    // 32bit float はディザ不要 → チェックを外して無効化。16/24bit に切り替えたら
    // 自動でチェックを付け、操作可能にする（その後ユーザーが外すことも可）。
    auto refreshDitherState = [this]
    {
        const bool is32 = (bitDepthBox.getSelectedId() == 32);
        // 注意: juce::Button::setToggleState は無効状態だと no-op（内部の isEnabled() ガード）。
        // setEnabled(false) を先に呼ぶと setToggleState が効かずチェックが残るため、
        // 「一旦有効化 → toggle を設定 → 最終的な有効/無効を設定」の順にする。
        ditherBtn.setEnabled(true);
        ditherBtn.setToggleState(!is32, juce::dontSendNotification);
        ditherBtn.setEnabled(!is32);
    };
    bitDepthBox.onChange = refreshDitherState;
    refreshDitherState();

    // MP3 選択時のみビットレート / ビット深度の有効状態を切替
    formatBox.onChange = [this]
    {
        const bool isMp3 = (formatBox.getSelectedId() == 3);
        mp3BitrateLabel.setVisible(isMp3);
        mp3BitrateBox.setVisible(isMp3);
        bitDepthLabel.setEnabled(!isMp3);
        bitDepthBox.setEnabled(!isMp3);
        resized();
    };
    formatBox.onChange();

    // モード切替: RTZ ボタン風の塗りラジオを 2つ
    const int modeGroup = 2002;
    auto styleModeBtn = [modeGroup](juce::TextButton& b)
    {
        b.setColour(juce::TextButton::buttonColourId,   AppColours::buttonBg);
        b.setColour(juce::TextButton::buttonOnColourId, AppColours::accent);
        b.setColour(juce::TextButton::textColourOffId,  AppColours::textDim);
        b.setColour(juce::TextButton::textColourOnId,   juce::Colours::white);
        b.setClickingTogglesState(true);
        b.setRadioGroupId(modeGroup);
        b.setWantsKeyboardFocus(false);
    };
    styleModeBtn(modeMixdownBtn);
    styleModeBtn(modeTrackBtn);
    modeMixdownBtn.setConnectedEdges(juce::Button::ConnectedOnRight);
    modeTrackBtn  .setConnectedEdges(juce::Button::ConnectedOnLeft);
    modeMixdownBtn.setToggleState(true, juce::dontSendNotification);
    modeMixdownBtn.onClick = [this] { updateModeVisibility(); };
    modeTrackBtn  .onClick = [this] { updateModeVisibility(); };
    addAndMakeVisible(modeMixdownBtn);
    addAndMakeVisible(modeTrackBtn);

    modeLabel.setText(tr(u8"書き出しモード"), juce::dontSendNotification);
    modeLabel.setFont(juce::FontOptions(12.0f));
    modeLabel.setColour(juce::Label::textColourId, juce::Colour(0xffb0b6bd));
    addAndMakeVisible(modeLabel);

    nameEditor.setText(context.defaultBaseName, juce::dontSendNotification);
    nameEditor.setJustification(juce::Justification::centredLeft);
    nameEditor.setIndents(8, 6);
    addAndMakeVisible(nameEditor);

    // ミックスダウン用のチャンネル数（モノ/ステレオ）
    styleField(mixChannelsLabel, tr(u8"出力チャンネル"));
    addAndMakeVisible(mixChannelsLabel);
    const int mixChGroup = 2003;
    auto styleMixCh = [mixChGroup](juce::TextButton& b, int connectedEdge)
    {
        b.setColour(juce::TextButton::buttonColourId,   AppColours::buttonBg);
        b.setColour(juce::TextButton::buttonOnColourId, AppColours::accent);
        b.setColour(juce::TextButton::textColourOffId,  AppColours::textDim);
        b.setColour(juce::TextButton::textColourOnId,   juce::Colours::white);
        b.setClickingTogglesState(true);
        b.setRadioGroupId(mixChGroup);
        b.setConnectedEdges(connectedEdge);
        b.setWantsKeyboardFocus(false);
    };
    styleMixCh(mixMonoBtn,   juce::Button::ConnectedOnRight);
    styleMixCh(mixStereoBtn, juce::Button::ConnectedOnLeft);
    mixStereoBtn.setToggleState(true, juce::dontSendNotification);
    addAndMakeVisible(mixMonoBtn);
    addAndMakeVisible(mixStereoBtn);

    tracksViewport.setViewedComponent(&tracksContent, false);
    tracksViewport.setScrollBarsShown(true, false);
    // スクロールバー背景は枠と一体に見せる（角付近で別矩形に見えないように透明）
    tracksViewport.setColour(juce::ScrollBar::backgroundColourId, juce::Colours::transparentBlack);
    addAndMakeVisible(tracksViewport);

    // ヘッダの全体チェックボックス: クリック後の状態を全トラックへ一括適用 (ON=全選択 / OFF=全解除)
    masterToggle.setColour(juce::ToggleButton::textColourId, juce::Colours::white);
    masterToggle.onClick = [this]
    {
        const bool target = masterToggle.getToggleState();
        for (auto* b : trackToggles) b->setToggleState(target, juce::dontSendNotification);
        syncMasterToggle();
    };
    addAndMakeVisible(masterToggle);

    folderEditor.setText(context.defaultFolder.getFullPathName(), juce::dontSendNotification);
    folderEditor.setJustification(juce::Justification::centredLeft);
    folderEditor.setIndents(8, 6);
    addAndMakeVisible(folderEditor);
    addAndMakeVisible(browseBtn);
    browseBtn.onClick = [this] { chooseFolder(); };

    exportBtn.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff2a78ff));
    exportBtn.setColour(juce::TextButton::textColourOnId,  juce::Colours::white);
    exportBtn.setColour(juce::TextButton::textColourOffId, juce::Colours::white);
    exportBtn.onClick = [this] { doExport(); };
    addAndMakeVisible(exportBtn);

    cancelBtn.onClick = [this] { if (onCancel) onCancel(); };
    addAndMakeVisible(cancelBtn);

    rebuildTrackList();
    updateModeVisibility();
    updateRangeVisibility();

    setSize(560, 600);
}

void ExportDialog::rebuildTrackList()
{
    trackToggles.clear();
    trackMonoBtns.clear();
    trackStereoBtns.clear();
    trackPreBtns.clear();
    trackPostBtns.clear();

    auto styleSegBtn = [](juce::TextButton& b, int groupId, int connectedEdges)
    {
        b.setColour(juce::TextButton::buttonColourId,   AppColours::buttonBg);
        b.setColour(juce::TextButton::buttonOnColourId, AppColours::accent);
        b.setColour(juce::TextButton::textColourOffId,  AppColours::textDim);
        b.setColour(juce::TextButton::textColourOnId,   juce::Colours::white);
        b.setClickingTogglesState(true);
        b.setRadioGroupId(groupId);
        b.setConnectedEdges(connectedEdges);
        b.setWantsKeyboardFocus(false);
    };

    for (int i = 0; i < (int)context.tracks.size(); ++i)
    {
        auto& ti = context.tracks[(size_t)i];

        auto* sel = new juce::ToggleButton(ti.name);
        sel->setToggleState(true, juce::dontSendNotification);
        sel->setColour(juce::ToggleButton::textColourId, ti.colour.brighter(0.4f));
        // 自己トグルは無効化し、切替はダイアログの mouseDown/mouseDrag が制御する
        // (クリックで反転 / ドラッグで通過行をまとめて塗り替え)。ダブルトグルを防ぐため。
        sel->setClickingTogglesState(false);
        sel->addMouseListener(this, false);
        trackToggles.add(sel);
        tracksContent.addAndMakeVisible(sel);

        // 行毎にラジオグループ ID を 2 つ割り当て（M/S と Pre/Post で別々の排他グループ）
        const int chGroup  = 1001 + i * 2;
        const int faGroup  = 1002 + i * 2;

        auto* pre = new juce::TextButton("Pre");
        styleSegBtn(*pre, faGroup, juce::Button::ConnectedOnRight);
        pre->setTooltip(tr(u8"クリップゲインのみ反映"));
        pre->setToggleState(true, juce::dontSendNotification);  // 既定: Pre
        trackPreBtns.add(pre);
        tracksContent.addAndMakeVisible(pre);

        auto* post = new juce::TextButton("Post");
        styleSegBtn(*post, faGroup, juce::Button::ConnectedOnLeft);
        post->setTooltip(tr(u8"トラックVol/Pan/マスターまで反映"));
        trackPostBtns.add(post);
        tracksContent.addAndMakeVisible(post);

        auto* mono = new juce::TextButton(tr(u8"モノ"));
        styleSegBtn(*mono, chGroup, juce::Button::ConnectedOnRight);
        mono->setToggleState(!ti.isStereoByDefault, juce::dontSendNotification);
        trackMonoBtns.add(mono);
        tracksContent.addAndMakeVisible(mono);

        auto* stereo = new juce::TextButton(tr(u8"ステレオ"));
        styleSegBtn(*stereo, chGroup, juce::Button::ConnectedOnLeft);
        stereo->setToggleState(ti.isStereoByDefault, juce::dontSendNotification);
        trackStereoBtns.add(stereo);
        tracksContent.addAndMakeVisible(stereo);
    }

    layoutTrackRows();
    syncMasterToggle();   // 初期状態 (既定は全 ON) を全体チェックへ反映
}

void ExportDialog::layoutTrackRows()
{
    const int rowH = kTrackRowH;   // セグメントボタン分の縦余裕（描画・スナップ共通）
    const int btnH = 22;
    const int numRows = (int)context.tracks.size();
    const int totalH  = juce::jmax(rowH, numRows * rowH);

    // 行数がビューポートより多いと縦スクロールバーが右端に出る。その幅を内容幅から
    // 差し引かないと、最右の [ステレオ] ボタンがスクロールバーに隠れてしまう。
    const bool needsScroll = totalH > tracksViewport.getHeight();
    const int  sbW = needsScroll ? tracksViewport.getScrollBarThickness() : 0;
    const int  contentW = juce::jmax(200, tracksViewport.getWidth() - sbW);

    // スクロールは 1 行単位（ホイール 1 ノッチ = 1 行）、位置も行高にスナップ
    tracksViewport.snapStep = rowH;
    tracksViewport.getVerticalScrollBar().setSingleStepSize(rowH);

    tracksContent.rowHeight = rowH;
    tracksContent.numRows   = numRows;
    tracksContent.setSize(contentW, totalH);

    for (int i = 0; i < trackToggles.size(); ++i)
    {
        const int y = i * rowH + (rowH - btnH) / 2;
        // 左 12 / 右 8 の余白でチェックボックス列を枠から十分内側に置く
        auto fullRow = juce::Rectangle<int>(12, y, tracksContent.getWidth() - 20, btnH);

        // 右から: [モノ|ステレオ]  [Pre|Post]
        const int chW = 64;     // 1セグメント幅
        const int faW = 52;
        const int gap = 10;

        auto stereoRect = fullRow.removeFromRight(chW);
        auto monoRect   = fullRow.removeFromRight(chW);
        fullRow.removeFromRight(gap);
        auto postRect   = fullRow.removeFromRight(faW);
        auto preRect    = fullRow.removeFromRight(faW);
        fullRow.removeFromRight(gap);
        // 残り = チェックボックス + トラック名
        auto nameRow = fullRow;

        trackStereoBtns[i]->setBounds(stereoRect);
        trackMonoBtns[i]  ->setBounds(monoRect);
        trackPostBtns[i]  ->setBounds(postRect);
        trackPreBtns[i]   ->setBounds(preRect);
        trackToggles[i]   ->setBounds(nameRow);
    }
}

// ── チェックボックスのドラッグ塗り替え ──
// トグルは各行に addMouseListener(this) 済みなので、チェックボックス上での押下・ドラッグは
// ここへ届く (mouse は開始トグルが capture するため、ドラッグ中は他の行の上でもここに来る)。
void ExportDialog::mouseDown(const juce::MouseEvent& e)
{
    const int row = trackToggles.indexOf(dynamic_cast<juce::ToggleButton*>(e.eventComponent));
    if (row < 0) return;   // チェックボックス以外 (Pre/Post 等・空きエリア) は無視
    dragPaintTarget = ! trackToggles[row]->getToggleState();   // 開始行を反転した状態を塗る
    dragPaintActive = true;
    trackToggles[row]->setToggleState(dragPaintTarget, juce::dontSendNotification);
    syncMasterToggle();
}

void ExportDialog::mouseDrag(const juce::MouseEvent& e)
{
    if (! dragPaintActive) return;
    // ドラッグ位置を tracksContent 座標へ変換し、その Y にある行を塗る (X は問わない)。
    const int yInContent = e.getEventRelativeTo(&tracksContent).getPosition().getY();
    if (yInContent < 0) return;
    const int row = yInContent / kTrackRowH;
    if (row >= 0 && row < trackToggles.size()
        && trackToggles[row]->getToggleState() != dragPaintTarget)
    {
        trackToggles[row]->setToggleState(dragPaintTarget, juce::dontSendNotification);
        syncMasterToggle();
    }
}

void ExportDialog::mouseUp(const juce::MouseEvent&)
{
    dragPaintActive = false;
}

void ExportDialog::syncMasterToggle()
{
    // 全トラックが ON のときだけ全体チェックを ON にする (1 つでも OFF なら OFF)
    bool all = trackToggles.size() > 0;
    for (auto* b : trackToggles)
        if (! b->getToggleState()) { all = false; break; }
    masterToggle.setToggleState(all, juce::dontSendNotification);
}

std::vector<int> ExportDialog::getSelectedTrackIndices() const
{
    std::vector<int> out;
    for (int i = 0; i < trackToggles.size(); ++i)
        if (trackToggles[i]->getToggleState() && i < (int)context.tracks.size())
            out.push_back(context.tracks[(size_t)i].index);
    return out;
}

int ExportDialog::getTrackChannels(int trackIndex) const
{
    for (int i = 0; i < (int)context.tracks.size(); ++i)
        if (context.tracks[(size_t)i].index == trackIndex && i < trackStereoBtns.size())
            return trackStereoBtns[i]->getToggleState() ? 2 : 1;
    return 2;
}

bool ExportDialog::getTrackPreFader(int trackIndex) const
{
    for (int i = 0; i < (int)context.tracks.size(); ++i)
        if (context.tracks[(size_t)i].index == trackIndex && i < trackPreBtns.size())
            return trackPreBtns[i]->getToggleState();
    return false;
}

void ExportDialog::updateModeVisibility()
{
    const bool trackMode = modeTrackBtn.getToggleState();

    // ミックスダウン: ファイル名 + 出力チャンネル
    nameLabel.setVisible(!trackMode);
    nameEditor.setVisible(!trackMode);
    mixChannelsLabel.setVisible(!trackMode);
    mixMonoBtn.setVisible(!trackMode);
    mixStereoBtn.setVisible(!trackMode);

    // トラック書き出し: トラック一覧 + ヘッダの全体チェック
    tracksLabel.setVisible(trackMode);
    tracksViewport.setVisible(trackMode);
    masterToggle.setVisible(trackMode);

    resized();
    repaint();  // 旧モードのコンポーネント描画残り（縁のはみ出し）を全面再描画で消す
}

void ExportDialog::updateRangeVisibility()
{
    rulerRangeLabel.setVisible(rangeRulerBtn.getToggleState());
    const bool barsMode = rangeBarsBtn.getToggleState();
    barStartLabel.setVisible(barsMode);
    barStartEditor.setVisible(barsMode);
    barRangeSepLabel.setVisible(barsMode);
    barEndLabel.setVisible(barsMode);
    barEndEditor.setVisible(barsMode);
    resized();
    repaint();
}

void ExportDialog::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xff1a1a1a));

    // トラックリストの背景（角丸）。枠線と角の整形は paintOverChildren で中身の上に描く
    if (tracksViewport.isVisible())
    {
        auto frame = listFrameBounds.toFloat();
        g.setColour(juce::Colour(0xff222428));
        g.fillRoundedRectangle(frame, 6.0f);
    }
}

void ExportDialog::paintOverChildren(juce::Graphics& g)
{
    if (! tracksViewport.isVisible())
        return;

    auto frame = listFrameBounds.toFloat();
    const float radius = 6.0f;

    // 矩形のスクロール内容/スクロールバーが角丸の四隅からはみ出して上部が凸凹に
    // 見えるのを防ぐ。四隅の余白を背景色で塗って丸めてから、枠線を最前面へ重ねる。
    juce::Path corners;
    corners.setUsingNonZeroWinding(false);          // even-odd: 外側矩形 − 角丸 = 四隅だけ
    corners.addRectangle(frame.expanded(1.0f));
    corners.addRoundedRectangle(frame, radius);
    g.setColour(juce::Colour(0xff1a1a1a));
    g.fillPath(corners);

    g.setColour(juce::Colour(0xff3a3d42));
    g.drawRoundedRectangle(frame, radius, 1.0f);
}

void ExportDialog::resized()
{
    auto r = getLocalBounds().reduced(20);
    titleLabel.setBounds(r.removeFromTop(28));
    r.removeFromTop(8);

    auto fieldRow = [&r](juce::Label& lbl, juce::Component& c)
    {
        lbl.setBounds(r.removeFromTop(16));
        c.setBounds(r.removeFromTop(28));
        r.removeFromTop(8);
    };

    // 範囲: ラベル + 3つのラジオを横並び
    rangeLabel.setBounds(r.removeFromTop(16));
    {
        auto row = r.removeFromTop(26);
        const int w = row.getWidth() / 3;
        rangeProjectBtn.setBounds(row.removeFromLeft(w));
        rangeRulerBtn  .setBounds(row.removeFromLeft(w));
        rangeBarsBtn   .setBounds(row);
        r.removeFromTop(8);
    }

    // ルーラー範囲モードのときだけ: 現在のルーラー範囲を表示する行
    if (rangeRulerBtn.getToggleState())
    {
        rulerRangeLabel.setBounds(r.removeFromTop(22));
        r.removeFromTop(8);
    }

    // 小節範囲モードのときだけ: [開始小節 (editor)] 〜 [終了小節 (editor)]
    if (rangeBarsBtn.getToggleState())
    {
        auto row = r.removeFromTop(26);
        const int edW  = 52;
        const int edH  = 22;
        const int lblW = 56;
        const int sepW = 16;
        auto editorRect = [edW, edH](juce::Rectangle<int> cell)
        {
            return cell.withSizeKeepingCentre(edW, edH);
        };
        barStartLabel.setBounds(row.removeFromLeft(lblW));
        barStartEditor.setBounds(editorRect(row.removeFromLeft(edW)));
        barRangeSepLabel.setBounds(row.removeFromLeft(sepW));
        row.removeFromLeft(8);
        barEndLabel.setBounds(row.removeFromLeft(lblW));
        barEndEditor.setBounds(editorRect(row.removeFromLeft(edW)));
        r.removeFromTop(8);
    }

    fieldRow(formatLabel, formatBox);

    // MP3 ビットレート（MP3 選択時のみ可視）
    if (mp3BitrateBox.isVisible())
        fieldRow(mp3BitrateLabel, mp3BitrateBox);

    {
        auto row = r.removeFromTop(46);
        auto half = row.getWidth() / 2 - 6;
        {
            auto col = row.removeFromLeft(half);
            sampleRateLabel.setBounds(col.removeFromTop(16));
            sampleRateBox.setBounds(col.removeFromTop(28));
        }
        row.removeFromLeft(12);
        {
            auto col = row;
            bitDepthLabel.setBounds(col.removeFromTop(16));
            bitDepthBox.setBounds(col.removeFromTop(28));
        }
        r.removeFromTop(6);
    }

    ditherBtn.setBounds(r.removeFromTop(22));
    autoRenameBtn.setBounds(r.removeFromTop(22));
    revealAfterBtn.setBounds(r.removeFromTop(22));
    r.removeFromTop(8);

    // 書き出しモード（ミックスダウン / トラック書き出し）
    modeLabel.setBounds(r.removeFromTop(16));
    {
        auto row = r.removeFromTop(28);
        const int w = row.getWidth() / 2;
        modeMixdownBtn.setBounds(row.removeFromLeft(w));
        modeTrackBtn  .setBounds(row);
        r.removeFromTop(10);
    }

    // 下部固定: ボタン → 出力フォルダ（編集行 → ラベル）の順で底から積む
    auto buttonRow = r.removeFromBottom(36);
    exportBtn.setBounds(buttonRow.removeFromRight(120));
    buttonRow.removeFromRight(8);
    cancelBtn.setBounds(buttonRow.removeFromRight(120));
    r.removeFromBottom(12);

    {
        auto editorRow = r.removeFromBottom(28);
        browseBtn.setBounds(editorRow.removeFromRight(80));
        editorRow.removeFromRight(6);
        folderEditor.setBounds(editorRow);
    }
    folderLabel.setBounds(r.removeFromBottom(16));
    r.removeFromBottom(10);

    if (modeTrackBtn.getToggleState())
    {
        // トラック書き出しモード: ヘッダ (ラベル + 全体チェック) + トラック一覧
        auto headerRow = r.removeFromTop(22);
        // トグルの文字は高さ依存 (LookAndFeel_V4: 高さ×0.75・上限15px)。ラベル「書き出すトラック」
        // (12px) に合わせて高さ16へ抑え、行内で縦中央に置く。
        masterToggle.setBounds(headerRow.removeFromRight(120).withSizeKeepingCentre(120, 16));
        tracksLabel.setBounds(headerRow);   // 12px ラベルは行内で縦中央に揃う
        r.removeFromTop(6);

        // 枠は使える領域いっぱいに角丸で描く。内側のビューポートは行高の倍数に丸めて
        // 半端な行が見切れないようにし、余りは枠下端の内側パディングとして残す。
        // 上下インセットは角丸半径(6)以上にして、右端スクロールバーが角丸コーナーに
        // 食い込まないようにする（左右は 4px）。
        listFrameBounds = r;
        auto inner = r.reduced(4, 8);
        inner.removeFromBottom(inner.getHeight() % kTrackRowH);
        tracksViewport.setBounds(inner);
        layoutTrackRows();
    }
    else
    {
        // ミックスダウンモード: ファイル名 + 出力チャンネル
        nameLabel.setBounds(r.removeFromTop(16));
        nameEditor.setBounds(r.removeFromTop(28));
        r.removeFromTop(10);
        mixChannelsLabel.setBounds(r.removeFromTop(16));
        {
            auto row = r.removeFromTop(28);
            const int w = juce::jmin(row.getWidth() / 2, 140);
            mixMonoBtn.setBounds(row.removeFromLeft(w));
            mixStereoBtn.setBounds(row.removeFromLeft(w));
        }
    }
}

bool ExportDialog::resolveRange(double& start, double& end) const
{
    if (rangeRulerBtn.getToggleState())
    {
        start = context.rulerStartSec;
        end   = context.rulerEndSec;
    }
    else if (rangeBarsBtn.getToggleState())
    {
        const int sb = juce::jmax(1, barStartEditor.getText().getIntValue());
        const int eb = juce::jmax(sb, barEndEditor.getText().getIntValue());
        if (context.barToSec)
        {
            start = context.barToSec(sb);
            end   = context.barToSec(eb + 1);   // 終了小節を含める（次の小節頭まで）
        }
        else
        {
            start = 0.0;
            end   = context.projectEndSec;
        }
    }
    else
    {
        start = 0.0;
        end   = context.projectEndSec;
    }
    return end > start;
}

void ExportDialog::chooseFolder()
{
    juce::File current(folderEditor.getText());
    auto initDir = current.isDirectory() ? current
                                         : (current.getParentDirectory().isDirectory()
                                              ? current.getParentDirectory()
                                              : juce::File::getSpecialLocation(juce::File::userDocumentsDirectory));

    fileChooser = std::make_unique<juce::FileChooser>(
        tr(u8"出力フォルダを選択"), initDir, "*", true);

    fileChooser->launchAsync(juce::FileBrowserComponent::openMode
                            | juce::FileBrowserComponent::canSelectDirectories,
        [this](const juce::FileChooser& fc)
        {
            auto f = fc.getResult();
            if (f.isDirectory())
                folderEditor.setText(f.getFullPathName(), juce::dontSendNotification);
        });
}

void ExportDialog::doExport()
{
    double start = 0.0, end = 0.0;
    if (!resolveRange(start, end))
    {
        juce::AlertWindow::showAsync(juce::MessageBoxOptions()
            .withIconType(juce::MessageBoxIconType::WarningIcon)
            .withTitle(tr(u8"範囲エラー"))
            .withMessage(tr(u8"書き出し範囲が無効です"))
            .withButton("OK"), nullptr);
        return;
    }

    juce::File folder(folderEditor.getText().trim());
    if (folder.getFullPathName().isEmpty())
    {
        juce::AlertWindow::showAsync(juce::MessageBoxOptions()
            .withIconType(juce::MessageBoxIconType::WarningIcon)
            .withTitle(tr(u8"出力フォルダ未指定"))
            .withMessage(tr(u8"出力フォルダを指定してください"))
            .withButton("OK"), nullptr);
        return;
    }
    folder.createDirectory();

    const bool stems = modeTrackBtn.getToggleState();
    auto selected = getSelectedTrackIndices();
    if (selected.empty())
    {
        juce::AlertWindow::showAsync(juce::MessageBoxOptions()
            .withIconType(juce::MessageBoxIconType::WarningIcon)
            .withTitle(tr(u8"トラック未選択"))
            .withMessage(tr(u8"書き出すトラックを選択してください"))
            .withButton("OK"), nullptr);
        return;
    }

    auto baseName = nameEditor.getText().trim();
    if (!stems && baseName.isEmpty()) baseName = juce::String("Export");

    ExportEngine::Format fmt = ExportEngine::Format::WAV;
    if (formatBox.getSelectedId() == 2) fmt = ExportEngine::Format::AIFF;
    else if (formatBox.getSelectedId() == 3) fmt = ExportEngine::Format::MP3;

    ExportEngine::Options opts;
    opts.file       = folder;     // 常にフォルダを保持
    opts.baseName   = baseName;
    opts.format     = fmt;
    opts.bitDepth   = bitDepthBox.getSelectedId();
    opts.sampleRate = (sampleRateBox.getSelectedId() == 1)
                          ? 0.0
                          : (double)sampleRateBox.getSelectedId();
    opts.startSec   = start;
    opts.endSec     = end;
    // 32bit float はディザ不要。UI 状態に関わらず必ずオフにする（保険）。
    opts.dither     = ditherBtn.getToggleState() && opts.bitDepth != 32;
    opts.stems      = stems;
    opts.autoRename  = autoRenameBtn.getToggleState();
    opts.revealAfter = revealAfterBtn.getToggleState();
    opts.realtime    = realtimeBtn.getToggleState();
    opts.mp3BitrateKbps = mp3BitrateBox.getSelectedId() > 0 ? mp3BitrateBox.getSelectedId() : 192;
    opts.numChannels = mixStereoBtn.getToggleState() ? 2 : 1;
    opts.selectedTrackIndices = std::move(selected);
    for (auto idx : opts.selectedTrackIndices)
    {
        opts.trackChannelsMap[idx] = getTrackChannels(idx);
        opts.trackPreFaderMap[idx] = getTrackPreFader(idx);
    }

    if (onExport) onExport(opts);
}
