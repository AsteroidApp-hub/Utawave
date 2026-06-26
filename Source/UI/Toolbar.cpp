// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

#include "Toolbar.h"
#include "../Localisation.h"
#include "../AppColours.h"
#include "TapTempoDialog.h"

TransportBar::TransportBar()
{
    auto styleBtn = [this](juce::TextButton& btn, juce::Colour bg, juce::Colour fg)
    {
        btn.setColour(juce::TextButton::buttonColourId,   bg);
        btn.setColour(juce::TextButton::buttonOnColourId, bg.brighter(0.2f));
        btn.setColour(juce::TextButton::textColourOffId,  fg);
        btn.setColour(juce::TextButton::textColourOnId,   fg);
        addAndMakeVisible(btn);
    };

    styleBtn(rewindBtn,        AppColours::buttonBg,  AppColours::text);
    styleBtn(stopBtn,          AppColours::buttonBg,  AppColours::text);
    styleBtn(playBtn,          AppColours::playGreen, juce::Colours::white);
    styleBtn(recBtn,           AppColours::recRed,    juce::Colours::white);
    styleBtn(tapTempoBtn,      AppColours::buttonBg,  AppColours::textDim);
    styleBtn(metronomeBtn,     AppColours::buttonBg,  AppColours::textDim);
    styleBtn(audioSettingsBtn, AppColours::buttonBg,  AppColours::text);

    // ── トランスポートアイコン LookAndFeel ──
    // Component ID で描画するアイコンを判別
    rewindBtn.setComponentID("rewind");
    stopBtn  .setComponentID("stop");
    playBtn  .setComponentID("play");
    recBtn   .setComponentID("rec");
    rewindBtn.setButtonText("");
    stopBtn  .setButtonText("");
    playBtn  .setButtonText("");
    recBtn   .setButtonText("");

    // アイコンのみのボタンは機能が分かりにくいのでツールチップで補足
    rewindBtn.setTooltip(tr(u8"先頭に戻る"));
    stopBtn  .setTooltip(tr(u8"停止 (S)"));
    playBtn  .setTooltip(tr(u8"再生 / 停止 (Space)"));
    recBtn   .setTooltip(tr(u8"録音開始 / 停止 (R)"));

    static struct TransportIconLF : public juce::LookAndFeel_V4 {
        void drawButtonText(juce::Graphics& g, juce::TextButton& b,
                            bool, bool) override
        {
            auto bounds = b.getLocalBounds().toFloat().reduced(10.0f, 8.0f);
            const auto id = b.getComponentID();

            // テキスト色を踏襲（背景色は元のまま）
            auto tc = b.findColour(b.getToggleState()
                                   ? juce::TextButton::textColourOnId
                                   : juce::TextButton::textColourOffId);
            g.setColour(tc);

            if (id == "rewind")
            {
                // ⏮ : 縦棒 + 左向き三角 ×2
                float left  = bounds.getX();
                float top   = bounds.getY();
                float right = bounds.getRight();
                float bot   = bounds.getBottom();
                float midY  = bounds.getCentreY();
                float halfW = (right - left - 3.0f) / 2.0f;

                g.fillRect(left, top, 2.0f, bot - top);
                juce::Path p;
                p.addTriangle(left + 3.0f + halfW, top,
                              left + 3.0f + halfW, bot,
                              left + 3.0f,         midY);
                p.addTriangle(right,                 top,
                              right,                 bot,
                              left + 3.0f + halfW,   midY);
                g.fillPath(p);
            }
            else if (id == "stop")
            {
                // ⏹ 角丸の正方形
                float side = juce::jmin(bounds.getWidth(), bounds.getHeight());
                auto sq = juce::Rectangle<float>(side, side)
                              .withCentre(bounds.getCentre());
                g.fillRoundedRectangle(sq, 1.5f);
            }
            else if (id == "play")
            {
                // ▶ 右向き三角形
                juce::Path p;
                p.addTriangle(bounds.getX(),     bounds.getY(),
                              bounds.getX(),     bounds.getBottom(),
                              bounds.getRight(), bounds.getCentreY());
                g.fillPath(p);
            }
            else if (id == "rec")
            {
                // ⏺ 円
                float side = juce::jmin(bounds.getWidth(), bounds.getHeight());
                auto sq = juce::Rectangle<float>(side, side)
                              .withCentre(bounds.getCentre());
                g.fillEllipse(sq);
            }
        }
    } transportIconLF;
    rewindBtn.setLookAndFeel(&transportIconLF);
    stopBtn  .setLookAndFeel(&transportIconLF);
    playBtn  .setLookAndFeel(&transportIconLF);
    recBtn   .setLookAndFeel(&transportIconLF);

    // LOOP ボタン: ∞ シンボルを描く
    class LoopBtnLF : public juce::LookAndFeel_V4
    {
    public:
        void drawButtonBackground(juce::Graphics& g, juce::Button& btn,
                                  const juce::Colour&, bool, bool) override
        {
            auto bounds = btn.getLocalBounds().toFloat().reduced(2.0f);
            bool on = btn.getToggleState();
            g.setColour(on ? juce::Colour(0xff5a8aaa) : juce::Colour(0xff2a2a2a));
            g.fillRoundedRectangle(bounds, 4.0f);
            g.setColour(on ? juce::Colours::white.withAlpha(0.6f)
                          : juce::Colour(0xff666666));
            g.drawRoundedRectangle(bounds, 4.0f, 1.0f);

            // ∞ 描画: 2つの円弧を交差させる lemniscate 風
            float cx = bounds.getCentreX();
            float cy = bounds.getCentreY();
            float w  = bounds.getWidth();
            float h  = bounds.getHeight();
            float r  = juce::jmin(w * 0.22f, h * 0.32f);
            float dx = r * 0.85f;

            juce::Path p;
            // 左円
            p.addEllipse(cx - dx - r, cy - r, r * 2.0f, r * 2.0f);
            // 右円
            p.addEllipse(cx + dx - r, cy - r, r * 2.0f, r * 2.0f);

            g.setColour(on ? juce::Colours::white : juce::Colour(0xffaaaaaa));
            g.strokePath(p, juce::PathStrokeType(1.8f));
        }
        void drawButtonText(juce::Graphics&, juce::TextButton&, bool, bool) override {}
    };
    static LoopBtnLF loopLF;
    loopBtn.setLookAndFeel(&loopLF);
    loopBtn.setClickingTogglesState(true);
    loopBtn.setTooltip(tr(u8"ループ再生のオン/オフ"));
    loopBtn.setWantsKeyboardFocus(false);
    loopBtn.onClick = [this] { if (onLoopToggle) onLoopToggle(loopBtn.getToggleState()); };
    addAndMakeVisible(loopBtn);
    tapTempoBtn.setButtonText("TAP");
    tapTempoBtn.setTooltip(tr(u8"クリックでタップ計測ウィンドウを開く（TAP ボタン連打で計測）"));
    tapTempoBtn.setWantsKeyboardFocus(false);
    tapTempoBtn.onClick = [this]
    {

        auto* dlg = new TapTempoDialog(currentBpm);
        dlg->onApply = [this](double newBpm)
        {
            currentBpm = newBpm;
            bpmLabel.setText(juce::String((int)newBpm) + " BPM", juce::dontSendNotification);
            if (onBpmChanged) onBpmChanged(newBpm);
        };

        juce::DialogWindow::LaunchOptions opts;
        opts.content.setOwned(dlg);
        opts.dialogTitle                  = tr(u8"タップでテンポを検出");
        opts.dialogBackgroundColour       = juce::Colour(0xff2a2a2a);
        opts.escapeKeyTriggersCloseButton = true;
        opts.useNativeTitleBar            = true;
        opts.resizable                    = false;
        opts.launchAsync();
    };
    metronomeBtn.setButtonText("CLICK");
    metronomeBtn.setTooltip(tr(u8"メトロノームのオン/オフ（右クリックで音色・音量を設定）"));
    audioSettingsBtn.setButtonText("AUDIO");
    audioSettingsBtn.setTooltip(tr(u8"マイクやスピーカーなどオーディオ機器の設定です。音が出ない・録音できないときはここで入力と出力を選びます"));

    rewindBtn.onClick        = [this] { if (onRewind)        onRewind(); };
    stopBtn.onClick          = [this] { if (onStop)          onStop(); };
    playBtn.onClick          = [this] { if (onPlay)          onPlay(); };
    recBtn.onClick           = [this] { if (onRecord)        onRecord(); };
    audioSettingsBtn.onClick = [this] { if (onAudioSettings) onAudioSettings(); };

    // ── 環境設定ボタン ──
    prefsBtn.setButtonText("SETTING");
    prefsBtn.setColour(juce::TextButton::buttonColourId,  AppColours::buttonBg);
    prefsBtn.setColour(juce::TextButton::textColourOffId, AppColours::text);
    prefsBtn.setTooltip(platformShortcutText(tr(u8"アプリ全体の設定です。録音・保存・表示などの動作を細かく変更できます (Cmd+,)")));
    prefsBtn.setWantsKeyboardFocus(false);
    prefsBtn.onClick = [this] { if (onPreferences) onPreferences(); };
    addAndMakeVisible(prefsBtn);

    // 読み込み / 書き出しボタン (AUDIO/SETTING の左に配置・操作を見つけやすく)
    styleBtn(importBtn, AppColours::buttonBg, AppColours::text);
    importBtn.setButtonText("IMPORT");
    importBtn.setTooltip(platformShortcutText(tr(u8"オーディオ / MIDI ファイルを読み込みます (Cmd+I)")));
    importBtn.setWantsKeyboardFocus(false);
    importBtn.onClick = [this] { if (onImport) onImport(); };
    addAndMakeVisible(importBtn);

    styleBtn(exportBtn, AppColours::buttonBg, AppColours::text);
    exportBtn.setButtonText("EXPORT");
    exportBtn.setTooltip(platformShortcutText(tr(u8"ファイルの書き出しのウィンドウを表示します (Cmd+E)")));
    exportBtn.setWantsKeyboardFocus(false);
    exportBtn.onClick = [this] { if (onExport) onExport(); };
    addAndMakeVisible(exportBtn);

    // CLICK（メトロノーム）ボタンをトグル化、右クリックで設定
    metronomeBtn.setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xff44aa88));
    metronomeBtn.setColour(juce::TextButton::textColourOnId,   juce::Colours::white);
    metronomeBtn.setClickingTogglesState(true);
    metronomeBtn.onClick = [this] {
        if (onMetronomeToggle) onMetronomeToggle(metronomeBtn.getToggleState());
    };
    metronomeBtn.addMouseListener(this, false);  // 右クリック検出用

    // RTZ ボタンは廃止 (環境設定の「停止時に再生開始位置へ戻る」で切り替え)
    // XFADE / ZC ボタンは廃止 (クロスフェードは F キー + 選択範囲のみで作成)

    // GAIN ボタン（クリップゲインライン表示）
    gainBtn.setColour(juce::TextButton::buttonColourId,   AppColours::buttonBg);
    gainBtn.setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xffaa6a2a));
    gainBtn.setColour(juce::TextButton::textColourOffId,  AppColours::textDim);
    gainBtn.setColour(juce::TextButton::textColourOnId,   juce::Colours::white);
    gainBtn.setClickingTogglesState(true);
    gainBtn.setButtonText("GAIN");
    gainBtn.setTooltip(tr(u8"クリップゲインラインを表示（ブレークポイント編集可能）"));
    gainBtn.setWantsKeyboardFocus(false);
    gainBtn.onClick = [this] {
        if (onClipGainChanged) onClipGainChanged(gainBtn.getToggleState());
    };
    // GAIN ボタンは一旦非表示（必要になったら setVisible(true) で復活可）
    addChildComponent(gainBtn);

    // ツール切替ボタン (CLICK / LINK / RANGE)
    class ToolBtnLF : public juce::LookAndFeel_V4
    {
    public:
        enum class IconType { Click, Link, Range };
        IconType type; juce::Colour onColour;
        ToolBtnLF(IconType t, juce::Colour c) : type(t), onColour(c) {}
        void drawButtonBackground(juce::Graphics& g, juce::Button& btn,
                                  const juce::Colour&, bool, bool) override
        {
            auto bounds = btn.getLocalBounds().toFloat().reduced(2.0f);
            bool on = btn.getToggleState();
            g.setColour(on ? onColour : juce::Colour(0xff2a2a2a));
            g.fillRoundedRectangle(bounds, 4.0f);
            g.setColour(on ? juce::Colours::white.withAlpha(0.6f) : juce::Colour(0xff666666));
            g.drawRoundedRectangle(bounds, 4.0f, 1.0f);

            float cx = bounds.getCentreX();
            float cy = bounds.getCentreY();
            auto iconCol = on ? juce::Colours::white : juce::Colour(0xffaaaaaa);
            g.setColour(iconCol);

            switch (type)
            {
                case IconType::Click:
                {
                    // 矢印カーソルのアイコン
                    juce::Path p;
                    p.startNewSubPath(cx - 4.0f, cy - 6.0f);
                    p.lineTo(cx - 4.0f,  cy + 5.0f);
                    p.lineTo(cx - 1.0f,  cy + 2.0f);
                    p.lineTo(cx + 1.0f,  cy + 6.0f);
                    p.lineTo(cx + 2.5f,  cy + 5.0f);
                    p.lineTo(cx + 0.5f,  cy + 1.0f);
                    p.lineTo(cx + 4.0f,  cy + 0.5f);
                    p.closeSubPath();
                    g.fillPath(p);
                    break;
                }
                case IconType::Link:
                {
                    // 鎖（2つの楕円が連結）
                    g.drawEllipse(cx - 6.0f, cy - 3.0f, 5.0f, 6.0f, 1.6f);
                    g.drawEllipse(cx + 1.0f, cy - 3.0f, 5.0f, 6.0f, 1.6f);
                    g.fillRect(cx - 1.0f, cy - 0.75f, 2.0f, 1.5f);
                    break;
                }
                case IconType::Range:
                {
                    // I-beam
                    g.fillRect(cx - 0.75f, cy - 6.0f, 1.5f, 12.0f);
                    g.fillRect(cx - 4.5f,  cy - 6.0f, 9.0f, 1.5f);
                    g.fillRect(cx - 4.5f,  cy + 4.5f, 9.0f, 1.5f);
                    break;
                }
            }
        }
        void drawButtonText(juce::Graphics&, juce::TextButton&, bool, bool) override {}
    };

    static ToolBtnLF clickLF (ToolBtnLF::IconType::Click, juce::Colour(0xff44aa88));
    static ToolBtnLF linkLF  (ToolBtnLF::IconType::Link,  juce::Colour(0xff7a5aa5));
    static ToolBtnLF rangeLF (ToolBtnLF::IconType::Range, juce::Colour(0xff5a8aaa));

    clickToolBtn.setLookAndFeel(&clickLF);
    clickToolBtn.setClickingTogglesState(false);
    clickToolBtn.setTooltip(tr(u8"クリック/移動ツールのみ"));
    clickToolBtn.setWantsKeyboardFocus(false);
    clickToolBtn.onClick = [this] { if (onToolModeChanged) onToolModeChanged(0); };
    addAndMakeVisible(clickToolBtn);

    linkToolBtn.setLookAndFeel(&linkLF);
    linkToolBtn.setClickingTogglesState(false);
    linkToolBtn.setTooltip(tr(u8"リンク: 両方のツールを併用（スマートツール）"));
    linkToolBtn.setWantsKeyboardFocus(false);
    linkToolBtn.onClick = [this] { if (onToolModeChanged) onToolModeChanged(2); };
    addAndMakeVisible(linkToolBtn);

    rangeToolBtn.setLookAndFeel(&rangeLF);
    rangeToolBtn.setClickingTogglesState(false);
    rangeToolBtn.setTooltip(tr(u8"範囲選択ツールのみ"));
    rangeToolBtn.setWantsKeyboardFocus(false);
    rangeToolBtn.onClick = [this] { if (onToolModeChanged) onToolModeChanged(1); };
    addAndMakeVisible(rangeToolBtn);

    setToolMode(2);  // 初期は Both

    // GRID ボタン（グリッドスナップ Off/Bar/Beat 循環）
    snapBtn.setColour(juce::TextButton::buttonColourId,   AppColours::buttonBg);
    snapBtn.setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xff5a8aaa));
    snapBtn.setColour(juce::TextButton::textColourOffId,  AppColours::textDim);
    snapBtn.setColour(juce::TextButton::textColourOnId,   juce::Colours::white);
    snapBtn.setButtonText("GRID: Off");
    snapBtn.setTooltip(tr(u8"グリッドスナップ（クリックで Off/Bar/Beat 切替）"));
    snapBtn.setWantsKeyboardFocus(false);
    snapBtn.onClick = [this] {
        juce::PopupMenu m;
        m.addItem(1, "Off");
        m.addSeparator();
        m.addItem(2, tr(u8"1/1    小節"));
        m.addItem(3, tr(u8"1/2    二分音符"));
        m.addItem(4, tr(u8"1/4    四分音符"));
        m.addItem(5, tr(u8"1/8    八分音符"));
        m.addItem(6, tr(u8"1/16   十六分音符"));
        m.addItem(7, tr(u8"1/32   三十二分音符"));
        m.addSeparator();
        m.addItem(8,  tr(u8"1/4 T  四分三連"));
        m.addItem(9,  tr(u8"1/8 T  八分三連"));
        m.addItem(10, tr(u8"1/16 T 十六分三連"));
        m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(snapBtn),
            [this](int result) {
                if (result <= 0) return;
                if (onSnapModeSelected) onSnapModeSelected(result - 1);
            });
    };
    addAndMakeVisible(snapBtn);

    // ── 高さ控えめなボタン用 LookAndFeel（フォントサイズ固定） ──
    static struct StackedBtnLF : public juce::LookAndFeel_V4 {
        juce::Font getTextButtonFont(juce::TextButton&, int) override
        {
            return juce::Font(juce::FontOptions(11.5f).withStyle("Bold"));
        }
    } stackedBtnLF;

    // ── カウントインボタン（上段・クリックでメニュー） ──
    countInBtn.setLookAndFeel(&stackedBtnLF);
    preRollBtn.setLookAndFeel(&stackedBtnLF);
    countInBtn.setColour(juce::TextButton::buttonColourId,   AppColours::buttonBg);
    countInBtn.setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xff5a8aaa));
    countInBtn.setColour(juce::TextButton::textColourOffId,  AppColours::textDim);
    countInBtn.setColour(juce::TextButton::textColourOnId,   juce::Colours::white);
    countInBtn.setButtonText("C-IN: Off");
    countInBtn.setTooltip(tr(u8"録音ボタンを押すと、指定した小節数ぶん手前から再生してから録音に入ります。プリロールとは同時に使えません"));
    countInBtn.setWantsKeyboardFocus(false);
    countInBtn.onClick = [this] {
        juce::PopupMenu m;
        m.addItem(1, tr(u8"なし"));
        m.addSeparator();
        m.addItem(2, tr(u8"1 小節"));
        m.addItem(3, tr(u8"2 小節"));
        m.addItem(4, tr(u8"4 小節"));
        m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(countInBtn),
            [this](int r) {
                if (r <= 0 || !onCountInChanged) return;
                int bars = (r == 1 ? 0 : r == 2 ? 1 : r == 3 ? 2 : 4);
                onCountInChanged(bars);
            });
    };
    addAndMakeVisible(countInBtn);

    // ── プリロールボタン（下段・クリックでメニュー） ──
    preRollBtn.setColour(juce::TextButton::buttonColourId,   AppColours::buttonBg);
    preRollBtn.setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xff5a8aaa));
    preRollBtn.setColour(juce::TextButton::textColourOffId,  AppColours::textDim);
    preRollBtn.setColour(juce::TextButton::textColourOnId,   juce::Colours::white);
    preRollBtn.setButtonText("PRE: Off");
    preRollBtn.setTooltip(tr(u8"録音ボタンを押すと、指定した秒数ぶん手前から再生してから録音に入ります。カウントインとは同時に使えません"));
    preRollBtn.setWantsKeyboardFocus(false);
    preRollBtn.onClick = [this] {
        juce::PopupMenu m;
        m.addItem(1, tr(u8"なし"));
        m.addSeparator();
        m.addItem(2, tr(u8"1 秒"));
        m.addItem(3, tr(u8"2 秒"));
        m.addItem(4, tr(u8"3 秒"));
        m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(preRollBtn),
            [this](int r) {
                if (r <= 0 || !onPreRollChanged) return;
                double secs = (r == 1 ? 0.0 : r == 2 ? 1.0 : r == 3 ? 2.0 : 3.0);
                onPreRollChanged(secs);
            });
    };
    addAndMakeVisible(preRollBtn);

    // キーボードフォーカスを全ボタンから外す（グローバルショートカットを守る）
    for (auto* btn : std::initializer_list<juce::Component*>{
            &rewindBtn, &stopBtn, &playBtn, &recBtn,
            &tapTempoBtn, &metronomeBtn, &audioSettingsBtn,
            &importBtn, &exportBtn,
            &gainBtn, &snapBtn, &loopBtn,
            &clickToolBtn, &linkToolBtn, &rangeToolBtn,
            &countInBtn, &preRollBtn,
            &bpmLabel, &barsBeatLabel, &timeLabel })
        btn->setWantsKeyboardFocus(false);

    auto styleLabel = [this](juce::Label& lbl, juce::String txt, float sz, bool bright = false)
    {
        lbl.setText(txt, juce::dontSendNotification);
        lbl.setFont(juce::FontOptions(sz));
        lbl.setColour(juce::Label::textColourId,
                      bright ? AppColours::textBright : AppColours::text);
        lbl.setJustificationType(juce::Justification::centred);
        addAndMakeVisible(lbl);
    };

    styleLabel(bpmLabel,      "120.0 BPM",   11.5f);
    bpmLabel.setTooltip(tr(u8"テンポ。ダブルクリックで数値を入力 / TAP ボタンで実測"));
    bpmLabel.setEditable(false, true, false);
    bpmLabel.setColour(juce::Label::backgroundWhenEditingColourId, juce::Colours::black.withAlpha(0.7f));
    bpmLabel.setColour(juce::Label::textWhenEditingColourId,        juce::Colours::white);
    // 編集開始時: BPM の文字列を外して数字のみにし、全選択して即上書き/削除できるように
    bpmLabel.onEditorShow = [this] {
        if (auto* ed = bpmLabel.getCurrentTextEditor())
        {
            ed->setText(juce::String(currentBpm, 1), false);
            ed->selectAll();
        }
    };
    bpmLabel.onTextChange = [this] {
        juce::String txt = bpmLabel.getText().replace("BPM", "").trim();
        double newBpm = txt.getDoubleValue();
        if (newBpm >= 20.0 && newBpm <= 999.0)
        {
            currentBpm = newBpm;
            if (onBpmChanged) onBpmChanged(newBpm);
        }
        bpmLabel.setText(juce::String(currentBpm, 1) + " BPM", juce::dontSendNotification);
    };
    styleLabel(barsBeatLabel, "1 | 1 | 000", 14.0f, true);
    styleLabel(timeLabel,     "00:00:000",   11.5f);
}

TransportBar::~TransportBar() {}

void TransportBar::mouseDown(const juce::MouseEvent& e)
{
    if (e.eventComponent == &metronomeBtn && e.mods.isRightButtonDown())
    {
        if (onMetronomeSettings) onMetronomeSettings();
    }
}

void TransportBar::paint(juce::Graphics& g)
{
    g.fillAll(AppColours::headerBg);

    // bottom border
    g.setColour(AppColours::separator);
    g.drawLine(0.0f, (float)getHeight() - 1.0f,
               (float)getWidth(), (float)getHeight() - 1.0f, 1.0f);

    // section separators
    auto sep = [&](int x)
    {
        g.setColour(AppColours::separator);
        g.drawLine((float)x, 10.0f, (float)x, (float)getHeight() - 10.0f, 1.0f);
    };

    // after transport | after bpm+tap+click | before AUDIO
    sep(transportRight + 10);
    sep(bpmBlockRight + 10);
    sep(getWidth() - audioBlockWidth - 10);
}

void TransportBar::resized()
{
    const int h   = 28;
    const int bw  = 52;
    const int y   = (getHeight() - h) / 2;
    const int pad = 4;

    // ── Right: IMPORT + EXPORT | AUDIO + SETTING (固定右端) ──
    const int settingW  = 64;
    const int ioW       = 62;   // IMPORT / EXPORT 幅
    const int ioGap     = 14;   // IO グループと AUDIO グループの区切り
    const int audioLeft = getWidth() - bw - settingW - 14;
    const int exportLeft = audioLeft  - ioGap - ioW;
    const int importLeft = exportLeft - pad   - ioW;
    importBtn       .setBounds(importLeft, y, ioW, h);
    exportBtn       .setBounds(exportLeft, y, ioW, h);
    audioSettingsBtn.setBounds(audioLeft,  y, bw, h);
    prefsBtn        .setBounds(getWidth() - settingW - 8, y, settingW, h);
    // 区切り線 (paint の sep) は IMPORT の左に来るよう右ブロック幅を更新
    audioBlockWidth = getWidth() - importLeft + 6;

    // ── Left: 2x2 ブロック: Bar|Beat | BPM  /  Time | TAP(小) ──
    const int colW     = 100;
    const int bpmW2    = 90;
    const int innerGap = 8;
    const int blockW   = colW + innerGap + bpmW2;
    const int lineH    = h / 2;      // 14 px/line
    const int blockTop = (getHeight() - h) / 2;

    int x = 8;
    barsBeatLabel.setBounds(x,                       blockTop,         colW,  lineH);
    timeLabel    .setBounds(x,                       blockTop + lineH, colW,  lineH);
    bpmLabel     .setBounds(x + colW + innerGap,     blockTop,         bpmW2, lineH);
    // BPM の下に TAP と CLICK を横並びで小さく配置
    {
        const int colX  = x + colW + innerGap;
        const int btnW  = (bpmW2 - 4) / 2;  // 2 ボタンで bpmW2 幅を分け合う
        tapTempoBtn .setBounds(colX,              blockTop + lineH, btnW, lineH);
        metronomeBtn.setBounds(colX + btnW + 4,   blockTop + lineH, btnW, lineH);
    }
    x += blockW + 16;

    // ── Transport: Rewind/Stop/Play/Rec/Loop ──
    const int tBw = 32;
    const int tH  = 32;
    const int tY  = (getHeight() - tH) / 2;
    const int loopW = tBw;
    rewindBtn.setBounds(x, tY, tBw, tH);   x += tBw + pad;
    stopBtn  .setBounds(x, tY, tBw, tH);   x += tBw + pad;
    playBtn  .setBounds(x, tY, tBw, tH);   x += tBw + pad;
    recBtn   .setBounds(x, tY, tBw, tH);   x += tBw + pad;
    loopBtn  .setBounds(x, tY, loopW, tH); x += loopW;
    transportRight = x;
    x += 10;

    // ── C-IN / PRE 縦積み (トランスポートの右隣、録音制御グループ) ──
    const int stackW = 96;
    const int halfH  = (h - 2) / 2;
    countInBtn.setBounds(x, y,             stackW, halfH);
    preRollBtn.setBounds(x, y + halfH + 2, stackW, halfH);
    x += stackW + 12;

    // ── Middle: 各種トグル + GRID (TAP/CLICK は BPM 下へ移動済み) ──
    // gainBtn は非表示中のためレイアウトをスキップ
    clickToolBtn.setBounds(x, y, 30, h);    x += 30;
    linkToolBtn.setBounds (x, y, 24, h);    x += 24;
    rangeToolBtn.setBounds(x, y, 30, h);    x += 30 + 4;
    snapBtn.setBounds     (x, y, 78, h);
    bpmBlockRight = x + 78;
}

void TransportBar::setLoopActive(bool v)
{
    loopBtn.setToggleState(v, juce::dontSendNotification);
}

void TransportBar::setMetronomeActive(bool v)
{
    metronomeBtn.setToggleState(v, juce::dontSendNotification);
}

void TransportBar::setCountInBars(int bars)
{
    juce::String value = (bars <= 0) ? "Off"
                                     : (juce::String(bars) + (bars == 1 ? " bar" : " bars"));
    countInBtn.setButtonText("C-IN: " + value);
    countInBtn.setToggleState(bars > 0, juce::dontSendNotification);
}

void TransportBar::setPreRollSecs(double secs)
{
    juce::String value = (secs < 0.5) ? "Off"
                                       : (juce::String((int)std::round(secs)) + " s");
    preRollBtn.setButtonText("PRE: " + value);
    preRollBtn.setToggleState(secs >= 0.5, juce::dontSendNotification);
}

void TransportBar::setToolMode(int mode)
{
    // mode: 0=Click, 1=Selection, 2=Both（リンク）
    bool click  = (mode == 0 || mode == 2);
    bool range  = (mode == 1 || mode == 2);
    bool linked = (mode == 2);
    clickToolBtn.setToggleState(click,  juce::dontSendNotification);
    rangeToolBtn.setToggleState(range,  juce::dontSendNotification);
    linkToolBtn.setToggleState (linked, juce::dontSendNotification);
}

void TransportBar::setSnapLabel(const juce::String& label, bool active)
{
    snapBtn.setButtonText("GRID: " + label);
    snapBtn.setToggleState(active, juce::dontSendNotification);
    // ボタン色を切替（active=橙、非active=灰）
    snapBtn.setColour(juce::TextButton::buttonColourId,
                      active ? juce::Colour(0xff5a8aaa) : AppColours::buttonBg);
    snapBtn.setColour(juce::TextButton::textColourOffId,
                      active ? juce::Colours::white : AppColours::textDim);
    snapBtn.repaint();
}

void TransportBar::setPlaying(bool p)
{
    playing = p;
    playBtn.setColour(juce::TextButton::buttonColourId,
                      p ? AppColours::playGreen.brighter(0.3f) : AppColours::playGreen);
    repaint();
}

void TransportBar::setRecording(bool r)
{
    recording = r;
    recBtn.setColour(juce::TextButton::buttonColourId,
                     r ? AppColours::recRed.brighter(0.4f) : AppColours::recRed);
    repaint();
}

void TransportBar::setBpm(double bpm)
{
    currentBpm = bpm;
    // 整数なら "120 BPM"、小数あれば "120.5 BPM" の形式
    bool isInt = std::abs(bpm - std::round(bpm)) < 0.01;
    juce::String txt = isInt ? juce::String((int)std::round(bpm))
                              : juce::String(bpm, 1);
    bpmLabel.setText(txt + " BPM", juce::dontSendNotification);
}

void TransportBar::setTimePosition(double seconds, int bars, int beats)
{
    int mins = (int)seconds / 60;
    int secs = (int)seconds % 60;
    int ms   = (int)((seconds - (int)seconds) * 1000.0);

    barsBeatLabel.setText(juce::String(bars) + " | " + juce::String(beats) + " | 000",
                          juce::dontSendNotification);
    timeLabel.setText(juce::String::formatted("%02d:%02d:%03d", mins, secs, ms),
                      juce::dontSendNotification);
}
