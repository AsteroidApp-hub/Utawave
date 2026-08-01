// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

#include "TrackHeaderView.h"
#include "../Localisation.h"
#include "../AppColours.h"
#include "Meter.h"
#include "../VST/PluginChain.h"
#include "../Audio/LufsMeter.h"
#include "../Audio/AppAudioCapture.h"   // アプリケーショントラックのアプリ選択メニュー

namespace {
// プラグイン名を短くする（メーカー名や余計な情報を削る）
juce::String shortenPluginName(const juce::String& full)
{
    auto s = full;
    // 30 文字超は省略
    if (s.length() > 22) s = s.substring(0, 21) + "…";
    return s;
}
} // namespace

TrackHeaderView::TrackHeaderView(Track& t) : track(t)
{
    // paint() が fillAll で全面を塗るため不透明。CoreAnimation のブレンド合成を省いて
    // GPU コンポジットを軽くする (トラック数に比例して効く → 多トラック時のカクツキ低減)。
    setOpaque(true);

    // ── トラック名 ──
    nameLabel.setText(track.getName(), juce::dontSendNotification);
    nameLabel.setFont(juce::FontOptions(13.0f));  // 少し大きく
    nameLabel.setColour(juce::Label::textColourId, AppColours::textBright);
    // ダブルクリックのみ編集可能（シングルクリックは選択モードに使う）
    nameLabel.setEditable(false, true, false);
    nameLabel.onTextChange = [this] {
        const juce::String newName = nameLabel.getText();
        editTrackUndoable([this, newName] { track.setName(newName); });
        if (onChanged) onChanged();
    };
    addAndMakeVisible(nameLabel);

    auto styleToggle = [](juce::TextButton& btn, juce::Colour onCol,
                          juce::Colour onText = juce::Colours::white)
    {
        btn.setColour(juce::TextButton::buttonColourId,   AppColours::buttonBg);
        btn.setColour(juce::TextButton::buttonOnColourId, onCol);
        btn.setColour(juce::TextButton::textColourOffId,  AppColours::textDim);
        btn.setColour(juce::TextButton::textColourOnId,   onText);
        btn.setClickingTogglesState(true);
    };

    styleToggle(muteBtn, juce::Colour(0xffffaa00), juce::Colours::black);
    muteBtn.onClick = [this] {
        // setClickingTogglesState(true) なので onClick 時点でボタンの toggle は既に反転済み。
        // track 側はまだ旧値なので editTrackUndoable の前後スナップショットで差分が取れる。
        const bool v = muteBtn.getToggleState();
        // Shift+Option+クリック: クリック後の値を選択中トラックへ一括適用 (Panel が委譲)。
        const auto mods = juce::ModifierKeys::currentModifiers;
        if (mods.isShiftDown() && mods.isAltDown() && onMuteBatch) { onMuteBatch(v); return; }
        if (onSelected) onSelected();
        editTrackUndoable([this, v] { track.setMuted(v); });
        if (onChanged) onChanged();
    };
    addAndMakeVisible(muteBtn);

    styleToggle(soloBtn, juce::Colour(0xff44aaff), juce::Colours::black);
    soloBtn.onClick = [this] {
        const bool v = soloBtn.getToggleState();
        // Shift+Option+クリック: クリック後の値を選択中トラックへ一括適用 (Panel が委譲)。
        const auto mods = juce::ModifierKeys::currentModifiers;
        if (mods.isShiftDown() && mods.isAltDown() && onSoloBatch) { onSoloBatch(v); return; }
        if (onSelected) onSelected();
        editTrackUndoable([this, v] { track.setSoloed(v); });
        if (onChanged) onChanged();
    };
    addAndMakeVisible(soloBtn);

    styleToggle(recBtn, AppColours::recRed);
    recBtn.onClick = [this] {
        if (onSelected) onSelected();
        track.setRecArmed(recBtn.getToggleState()); if (onChanged) onChanged();
    };
    addAndMakeVisible(recBtn);

    styleToggle(monBtn, juce::Colour(0xff44cc88));
    monBtn.onClick = [this] {
        if (onSelected) onSelected();
        track.setInputMonitor(monBtn.getToggleState());
        if (onInputMonitorChanged) onInputMonitorChanged(monBtn.getToggleState());
        if (onChanged) onChanged();
    };
    addAndMakeVisible(monBtn);

    // ── TList ボタン（常時表示・トグル）──
    // ON (active, orange)  = Takeレーンを表示  → lanesCollapsed = false
    // OFF (inactive, grey) = Takeレーンを非表示 → lanesCollapsed = true
    lanesBtn.setColour(juce::TextButton::buttonColourId,   AppColours::buttonBg);
    lanesBtn.setColour(juce::TextButton::buttonOnColourId, AppColours::accent);
    lanesBtn.setColour(juce::TextButton::textColourOffId,  AppColours::textDim);
    lanesBtn.setColour(juce::TextButton::textColourOnId,   juce::Colours::white);
    lanesBtn.setClickingTogglesState(true);
    lanesBtn.setButtonText("TList");
    lanesBtn.setTooltip(tr(u8"テイク（録り直し）レーンの表示 / 非表示。録り直すと前のテイクはここに残ります"));
    lanesBtn.setWantsKeyboardFocus(false);
    lanesBtn.onClick = [this]
    {
        // ON = 表示, OFF = 非表示
        track.setLanesCollapsed(!lanesBtn.getToggleState());
        if (onChanged) onChanged();
    };
    addAndMakeVisible(lanesBtn);

    // ── Vol / Pan ──
    auto styleSlider = [this](juce::Slider& s, double lo, double hi, double def)
    {
        s.setSliderStyle(juce::Slider::LinearHorizontal);
        s.setTextBoxStyle(juce::Slider::NoTextBox, true, 0, 0);
        s.setRange(lo, hi, 0.01);
        s.setValue(def, juce::dontSendNotification);
        s.setDoubleClickReturnValue(true, def);
        s.setColour(juce::Slider::backgroundColourId, AppColours::meterBg);
        s.setColour(juce::Slider::trackColourId,      AppColours::separator);
        s.setColour(juce::Slider::thumbColourId,      AppColours::accent);
        s.setWantsKeyboardFocus(false);
        addAndMakeVisible(s);
    };
    styleSlider(volSlider, -60.0, 6.0, 0.0);
    styleSlider(panSlider, -1.0,  1.0, 0.0);
    panSlider.setColour(juce::Slider::thumbColourId, AppColours::textDim);
    styleSlider(revSlider, 0.0, 1.0, 0.0);
    revSlider.setColour(juce::Slider::thumbColourId, juce::Colour(0xff66aadd));
    revSlider.setColour(juce::Slider::trackColourId, juce::Colour(0xff3a5566));

    // Vol スライダ: 右側に dB 値を表示 (0.1 dB ステップ、クリックで数値入力可)
    volSlider.setRange(-60.0, 6.0, 0.1);
    volSlider.setNumDecimalPlacesToDisplay(1);
    volSlider.setTextValueSuffix(" dB");
    volSlider.setTextBoxStyle(juce::Slider::TextBoxRight, /*readOnly*/ false, 50, 12);
    volSlider.setColour(juce::Slider::textBoxTextColourId,       AppColours::text);
    volSlider.setColour(juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
    volSlider.setColour(juce::Slider::textBoxOutlineColourId,    juce::Colours::transparentBlack);
    volSlider.setColour(juce::Slider::textBoxHighlightColourId,  AppColours::accent.withAlpha(0.3f));
    // JUCE Slider 内部 Label のフォントが大きすぎるので 9px に縮小。
    // あわせてキーボードフォーカスを外す: 編集可能 Label は setEditable が自動で focus 有効に
    // するため、dB 数値をクリックすると編集後も Label がフォーカスを保持し、↑↓ が Label → 親
    // Slider へ伝播して Slider::keyPressed が Vol を ±step 変えてしまう (トラック選択の ↑↓
    // 移動を吸う)。クリック編集は Label::mouseUp が担うので focus 無しでも従来どおり動く
    for (int i = 0; i < volSlider.getNumChildComponents(); ++i)
        if (auto* lbl = dynamic_cast<juce::Label*>(volSlider.getChildComponent(i)))
        {
            lbl->setFont(juce::FontOptions(9.5f));
            lbl->setWantsKeyboardFocus(false);
        }

    volSlider.setValue(track.getVolume(),     juce::dontSendNotification);
    panSlider.setValue(track.getPan(),        juce::dontSendNotification);
    revSlider.setValue(track.getReverbSend(), juce::dontSendNotification);

    // Vol/Pan/Rev スライダを Undo 対応で配線する。
    // ・ドラッグ中: onValueChange ごとにライブ適用 (音は即追従)。Undo には積まない。
    // ・ドラッグ終了: 開始値→終了値を 1 トランザクションにまとめて記録 (editTrackUndoable)。
    // ・テキスト入力 / ダブルクリック既定値: ドラッグでないので onValueChange で単発記録。
    auto wireMixSlider = [this](juce::Slider& s,
                                std::function<float()> getter,
                                std::function<void(float)> setter)
    {
        s.onDragStart = [this, getter] { mixDragging = true; mixDragBefore = getter(); };
        s.onValueChange = [this, &s, getter, setter]
        {
            if (onSelected) onSelected();
            const float v = (float) s.getValue();
            if (mixDragging)
            {
                setter(v);   // ライブ適用 (Undo はドラッグ終了でまとめて積む)
            }
            else
            {
                // ドラッグ外の単発変更。モデルはまだ旧値なので editTrackUndoable の
                // 前後スナップショットで before(旧)→after(v) の差分が取れる。
                editTrackUndoable([setter, v] { setter(v); });
            }
            if (onChanged) onChanged();
        };
        s.onDragEnd = [this, getter, setter]
        {
            mixDragging = false;
            const float after = getter();          // ドラッグ中にライブ適用済みの最終値
            if (mixDragBefore != after)
            {
                setter((float) mixDragBefore);     // capture-before が開始値を見れるよう一旦戻す
                editTrackUndoable([setter, after] { setter(after); });
            }
        };
    };
    wireMixSlider(volSlider, [this] { return track.getVolume(); },
                            [this](float v) { track.setVolume(v); });
    wireMixSlider(panSlider, [this] { return track.getPan(); },
                            [this](float v) { track.setPan(v); });
    wireMixSlider(revSlider, [this] { return track.getReverbSend(); },
                            [this](float v) { track.setReverbSend(v); });

    // ── In: ラベル + ComboBox ──
    inputLabel.setText("In:", juce::dontSendNotification);
    inputLabel.setFont(juce::FontOptions(10.0f));
    inputLabel.setColour(juce::Label::textColourId, AppColours::textDim);
    inputLabel.setJustificationType(juce::Justification::centredRight);
    addAndMakeVisible(inputLabel);

    inputChBox.setColour(juce::ComboBox::backgroundColourId, AppColours::buttonBg);
    inputChBox.setColour(juce::ComboBox::textColourId,       AppColours::text);
    inputChBox.setColour(juce::ComboBox::arrowColourId,      AppColours::textDim);
    inputChBox.setColour(juce::ComboBox::outlineColourId,    AppColours::separator);
    inputChBox.setTooltip(tr(u8"このトラックに録音する入力（マイク）を選びます"));
    inputChBox.setWantsKeyboardFocus(false);
    inputChBox.onChange = [this]
    {
        int id = inputChBox.getSelectedId();
        if (id >= 1000) track.setInputChannel(id - 1000);  // ステレオ: 左ch index
        else            track.setInputChannel(id - 1);     // モノ: 0-based
        if (onChanged) onChanged();
    };
    addAndMakeVisible(inputChBox);

    // ── インサート FX スロット枠（右クリックで表示切替、4 スロット固定） ──
    // PluginChain の変更通知 → チップ再構築（async 中に view が破棄されても安全に）
    juce::Component::SafePointer<TrackHeaderView> safeSelf(this);
    track.getPluginChain().onChainChanged = [safeSelf]
    {
        juce::MessageManager::callAsync([safeSelf]
        {
            if (auto* self = safeSelf.getComponent())
            {
                self->rebuildInsertChips();
                self->resized();
                self->repaint();
                // チェーンが変わったらエンジンへ通知する。入力モニターの返し (setMonitorChain)
                // を再公開 + 再 prepare させるため (これが無いと、停止中に追加したトラック等の
                // 未 prepare チェーンに挿したプラグインがモニター返しで効かない/メーターが反応しない)。
                if (self->onChanged) self->onChanged();
            }
        });
    };
    rebuildInsertChips();

    // ステレオ/モノ 表記（M ボタンの左隣・読み取り専用・控えめなテキストのみ）
    stereoBadge.setText(track.isStereo() ? "stereo" : "mono", juce::dontSendNotification);
    stereoBadge.setFont(juce::FontOptions(10.0f));
    stereoBadge.setJustificationType(juce::Justification::centredRight);
    stereoBadge.setColour(juce::Label::textColourId,
                          AppColours::textDim.withAlpha(0.6f));
    stereoBadge.setColour(juce::Label::backgroundColourId, juce::Colours::transparentBlack);
    addAndMakeVisible(stereoBadge);

    // フォーカス無効化
    for (auto* c : std::initializer_list<juce::Component*>{
            &muteBtn, &soloBtn, &recBtn, &monBtn, &nameLabel, &inputLabel })
        c->setWantsKeyboardFocus(false);

    // ラベル・スライダー・ボタンへの右クリックも TrackHeaderView::mouseDown に転送
    // → トラックヘッダーのどこを右クリックしてもメニューが出る
    for (auto* c : std::initializer_list<juce::Component*>{
            &nameLabel, &volSlider, &panSlider, &revSlider, &lanesBtn,
            &inputLabel, &inputChBox,
            &muteBtn, &soloBtn, &recBtn, &monBtn, &stereoBadge })
        c->addMouseListener(this, false);

    // 初期状態反映
    muteBtn.setToggleState(track.isMuted(),        juce::dontSendNotification);
    soloBtn.setToggleState(track.isSoloed(),        juce::dontSendNotification);
    recBtn.setToggleState (track.isRecArmed(),      juce::dontSendNotification);
    monBtn.setToggleState (track.isInputMonitor(),  juce::dontSendNotification);
    lanesBtn.setToggleState(!track.isLanesCollapsed(), juce::dontSendNotification);

    // MIDI トラックでは録音・モニター・入力選択・TList を非表示
    // （内蔵シンセが直接 MIDI を鳴らすので入力ソースの概念が無い）
    if (track.isMidiTrack())
    {
        recBtn.setVisible(false);
        monBtn.setVisible(false);
        inputLabel.setVisible(false);
        inputChBox.setVisible(false);
        lanesBtn.setVisible(false);

        auto styleSmallBtn = [](juce::TextButton& b)
        {
            b.setColour(juce::TextButton::buttonColourId,   AppColours::buttonBg);
            b.setColour(juce::TextButton::textColourOffId,  AppColours::textDim);
            b.setColour(juce::TextButton::textColourOnId,   juce::Colours::white);
            b.setWantsKeyboardFocus(false);
        };
        styleSmallBtn(octDownBtn);
        styleSmallBtn(octUpBtn);
        styleSmallBtn(semiDownBtn);
        styleSmallBtn(semiUpBtn);

        auto updateMidiInfo = [this]
        {
            int total = track.getTotalTransposeSemitones();
            juce::String s = (total > 0 ? "+" : "") + juce::String(total);
            midiInfoLabel.setText(s, juce::dontSendNotification);
        };

        octDownBtn.onClick = [this, updateMidiInfo]
        {
            editTrackUndoable([this] { track.setOctaveShift(track.getOctaveShift() - 1); });
            updateMidiInfo();
            if (onChanged) onChanged();
        };
        octUpBtn.onClick = [this, updateMidiInfo]
        {
            editTrackUndoable([this] { track.setOctaveShift(track.getOctaveShift() + 1); });
            updateMidiInfo();
            if (onChanged) onChanged();
        };
        semiDownBtn.onClick = [this, updateMidiInfo]
        {
            editTrackUndoable([this] { track.setSemitoneTranspose(track.getSemitoneTranspose() - 1); });
            updateMidiInfo();
            if (onChanged) onChanged();
        };
        semiUpBtn.onClick = [this, updateMidiInfo]
        {
            editTrackUndoable([this] { track.setSemitoneTranspose(track.getSemitoneTranspose() + 1); });
            updateMidiInfo();
            if (onChanged) onChanged();
        };
        addAndMakeVisible(octDownBtn);
        addAndMakeVisible(octUpBtn);
        addAndMakeVisible(semiDownBtn);
        addAndMakeVisible(semiUpBtn);

        midiInfoLabel.setFont(juce::FontOptions(10.0f));
        midiInfoLabel.setColour(juce::Label::textColourId, AppColours::textDim);
        midiInfoLabel.setJustificationType(juce::Justification::centred);
        updateMidiInfo();
        addAndMakeVisible(midiInfoLabel);

        // 波形サイクルボタン (Sin / Saw / Sqr)。右クリック / Cmd+クリック で内蔵シンセ ON/OFF
        auto waveformName = [](int w) -> juce::String
        {
            switch (w) {
                case 0: return "Sin";
                case 1: return "Saw";
                case 2: return "Sqr";
                default: return "Saw";
            }
        };
        auto refreshWaveformBtn = [this, waveformName]
        {
            const bool on = track.isSynthEnabled();
            if (on)
            {
                waveformBtn.setButtonText(waveformName(track.getSynthWaveform()));
                waveformBtn.setColour(juce::TextButton::textColourOffId, AppColours::accent);
            }
            else
            {
                waveformBtn.setButtonText("Off");
                waveformBtn.setColour(juce::TextButton::textColourOffId, AppColours::textDim);
            }
            waveformBtn.setTooltip(platformShortcutText(on
                ? tr(u8"クリック: 波形切替 / 右クリック または Cmd+クリック: 内蔵シンセを OFF")
                : tr(u8"内蔵シンセ OFF 中。右クリック または Cmd+クリックで ON に戻す")));
        };
        waveformBtn.setColour(juce::TextButton::buttonColourId,  AppColours::buttonBg);
        waveformBtn.setWantsKeyboardFocus(false);
        refreshWaveformBtn();
        // refresh() から移調ラベル / 波形ボタン表示を更新できるよう保持
        updateMidiInfoDisplay  = updateMidiInfo;
        refreshWaveformDisplay = refreshWaveformBtn;
        waveformBtn.onClick = [this, refreshWaveformBtn]
        {
            // 右クリック直後の onClick は抑止（波形サイクルが走ってしまうのを防ぐ）
            if (waveformRightClickHandled)
            {
                waveformRightClickHandled = false;
                return;
            }
            // Cmd+クリック: 内蔵シンセ ON/OFF
            if (juce::ModifierKeys::currentModifiers.isCommandDown())
            {
                editTrackUndoable([this] { track.setSynthEnabled(!track.isSynthEnabled()); });
                refreshWaveformBtn();
                if (onChanged) onChanged();
                return;
            }
            // 通常クリック: 波形サイクル（OFF 中も波形選択は記憶しておく）
            editTrackUndoable([this] { track.setSynthWaveform((track.getSynthWaveform() + 1) % 3); });
            refreshWaveformBtn();
            if (onChanged) onChanged();
        };
        // 右クリック判定は mouseDown 経由で
        waveformBtn.addMouseListener(this, false);
        addAndMakeVisible(waveformBtn);
    }

    // クリックトラックでは録音・モニター・入力選択・TList を非表示
    // 代わりに音色 ComboBox とアクセントトグルを表示
    if (track.isClickTrack())
    {
        recBtn.setVisible(false);
        monBtn.setVisible(false);
        inputLabel.setVisible(false);
        inputChBox.setVisible(false);
        lanesBtn.setVisible(false);

        clickSoundBox.addItem("Beep",    1);
        clickSoundBox.addItem("Stick",   2);
        clickSoundBox.addItem("Cowbell", 3);
        clickSoundBox.addItem("Wood",    4);
        clickSoundBox.addItem("Tick",    5);
        clickSoundBox.addItem("Bell",    6);
        clickSoundBox.setSelectedId(track.getClickSound() + 1, juce::dontSendNotification);
        clickSoundBox.setColour(juce::ComboBox::backgroundColourId, AppColours::buttonBg);
        clickSoundBox.setColour(juce::ComboBox::textColourId,       AppColours::text);
        clickSoundBox.setColour(juce::ComboBox::arrowColourId,      AppColours::textDim);
        clickSoundBox.setColour(juce::ComboBox::outlineColourId,    AppColours::separator);
        clickSoundBox.setWantsKeyboardFocus(false);
        clickSoundBox.onChange = [this] {
            track.setClickSound(clickSoundBox.getSelectedId() - 1);
            if (onChanged) onChanged();
        };
        addAndMakeVisible(clickSoundBox);
        clickSoundBox.addMouseListener(this, false);

        clickAccentBtn.setColour(juce::TextButton::buttonColourId,   AppColours::buttonBg);
        clickAccentBtn.setColour(juce::TextButton::buttonOnColourId, AppColours::accent);
        clickAccentBtn.setColour(juce::TextButton::textColourOffId,  AppColours::textDim);
        clickAccentBtn.setColour(juce::TextButton::textColourOnId,   juce::Colours::white);
        clickAccentBtn.setClickingTogglesState(true);
        clickAccentBtn.setToggleState(track.isClickAccent(), juce::dontSendNotification);
        clickAccentBtn.setWantsKeyboardFocus(false);
        clickAccentBtn.setTooltip(juce::String::fromUTF8(
            u8"アクセント (Accent): 小節頭の 1 拍目だけ強めに鳴らす"));
        clickAccentBtn.onClick = [this] {
            track.setClickAccent(clickAccentBtn.getToggleState());
            if (onChanged) onChanged();
        };
        addAndMakeVisible(clickAccentBtn);
        clickAccentBtn.addMouseListener(this, false);

        // x2 / ½ レートボタン（排他: 押すと相手解除、自身を再度押すと解除）
        auto styleRateBtn = [](juce::TextButton& b) {
            b.setColour(juce::TextButton::buttonColourId,   AppColours::buttonBg);
            b.setColour(juce::TextButton::buttonOnColourId, AppColours::accent);
            b.setColour(juce::TextButton::textColourOffId,  AppColours::textDim);
            b.setColour(juce::TextButton::textColourOnId,   juce::Colours::white);
            b.setClickingTogglesState(false);  // 手動でトグル管理
            b.setWantsKeyboardFocus(false);
        };
        styleRateBtn(clickHalfBtn);
        styleRateBtn(clickDoubleBtn);
        clickHalfBtn.setToggleState(track.getClickRate() == 1, juce::dontSendNotification);
        clickDoubleBtn.setToggleState(track.getClickRate() == 2, juce::dontSendNotification);
        clickHalfBtn.onClick = [this] {
            int newRate = (track.getClickRate() == 1) ? 0 : 1;
            track.setClickRate(newRate);
            clickHalfBtn.setToggleState(newRate == 1,   juce::dontSendNotification);
            clickDoubleBtn.setToggleState(false,        juce::dontSendNotification);
            if (onChanged) onChanged();
        };
        clickDoubleBtn.onClick = [this] {
            int newRate = (track.getClickRate() == 2) ? 0 : 2;
            track.setClickRate(newRate);
            clickDoubleBtn.setToggleState(newRate == 2, juce::dontSendNotification);
            clickHalfBtn.setToggleState(false,          juce::dontSendNotification);
            if (onChanged) onChanged();
        };
        addAndMakeVisible(clickHalfBtn);
        addAndMakeVisible(clickDoubleBtn);
        clickHalfBtn.addMouseListener(this, false);
        clickDoubleBtn.addMouseListener(this, false);
    }

    // アプリケーショントラック: 録音 (R) / 入力モニター (I) / 入力ch / Rev (リバーブ送りは
    // エンジンが経路を持たない) / TList (レーン無し) は使わないので非表示。In: 行の位置に
    // アプリ選択ボタンを出す (resized() が毎回確定する)
    if (track.isAppCaptureTrack())
    {
        recBtn.setVisible(false);
        monBtn.setVisible(false);
        stereoBadge.setVisible(false);
        lanesBtn.setVisible(false);
        revSlider.setVisible(false);

        appSelectBtn.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff3a3a3a));
        appSelectBtn.setColour(juce::TextButton::textColourOffId, juce::Colours::white);
        appSelectBtn.setTooltip(tr(u8"取り込むアプリを選びます (音を再生中のアプリが表示されます)"));
        appSelectBtn.onClick = [this] { showAppSelectMenu(); };
        addAndMakeVisible(appSelectBtn);
    }

    // フォルダトラック (グループバス): 録音/モニター/入力は使わないので非表示
    // (In:/Pan/Rev/INS の表示は resized() が毎回確定する。Pan/Rev/INS はアプリ設定
    //  showFolderPanRevIns = folderExtrasOn() でユーザーが表示/非表示を選べる)。
    // TList ボタンはフォルダの Open/Close (子トラック行の開閉) に転用する。
    if (track.isFolderTrack())
    {
        recBtn.setVisible(false);
        monBtn.setVisible(false);
        stereoBadge.setVisible(false);

        // ラベルは操作 (これから起きること) を示す: 開いている間は "Close"、閉じている間は "Open"。
        // トグルの点灯 (accent) は TList と同じく「展開中」を表す。
        lanesBtn.setToggleState(track.isFolderOpen(), juce::dontSendNotification);
        lanesBtn.setButtonText(track.isFolderOpen() ? "Close" : "Open");
        lanesBtn.setTooltip(tr(u8"フォルダを開閉します (閉じると中のトラックが隠れます)"));
        lanesBtn.onClick = [this]
        {
            track.setFolderOpen(lanesBtn.getToggleState());
            lanesBtn.setButtonText(track.isFolderOpen() ? "Close" : "Open");
            if (onChanged) onChanged();
        };
    }
}

TrackHeaderView::~TrackHeaderView()
{
    // ※ ここで track にアクセスしてはいけない。トラック削除フローでは、
    //    Track が先に破棄されてから refresh() で view が破棄されるため、
    //    track 参照が dangling になっている可能性がある。
    //    PluginChain は Track と一緒に死ぬので onChainChanged のクリアは不要。
    //    また refresh() でビューを差し替える場合は、新しい view の constructor が
    //    onChainChanged を上書きするため、古い lambda は呼ばれない。
}

void TrackHeaderView::rebuildInsertChips()
{
    fxChips.clear();
    fxBypassBtns.clear();
    auto& chain = track.getPluginChain();
    const int maxSlots = Track::insertSlotCount;

    for (int i = 0; i < maxSlots; ++i)
    {
        auto* btn = new juce::TextButton();
        btn->setWantsKeyboardFocus(false);

        // 各スロットを位置指定で扱う: getPlugin(i) が null なら空スロット
        if (chain.getPlugin(i) != nullptr)
        {
            // 既存プラグイン: 塗りつぶしのチップ
            if (auto* p = chain.getPlugin(i))
            {
                btn->setButtonText(shortenPluginName(p->getName()));
                btn->setColour(juce::TextButton::buttonColourId,
                               chain.isBypassed(i) ? juce::Colour(0xff2a2d31)
                                                   : juce::Colour(0xff2e4d7a));
                btn->setColour(juce::TextButton::buttonOnColourId,
                               chain.isBypassed(i) ? juce::Colour(0xff35383d)
                                                   : juce::Colour(0xff3a5a8a));
                btn->setColour(juce::TextButton::textColourOffId,
                               chain.isBypassed(i)
                                   ? AppColours::textDim.withAlpha(0.7f)
                                   : juce::Colours::white);
                btn->setTooltip(p->getName()
                    + (chain.isBypassed(i)
                        ? tr(u8"（バイパス中）")
                        : juce::String()));
                const int slotIdx = i;
                btn->onClick = [this, slotIdx]
                {
                    // Cmd+クリックでバイパス切替（macOS）/ Ctrl+クリック（Windows 等）
                    if (juce::ModifierKeys::currentModifiers.isCommandDown())
                    {
                        togglePluginBypass(slotIdx);
                        return;
                    }
                    if (onPluginEditRequest) onPluginEditRequest(slotIdx);
                };
                // 右クリックで context menu
                btn->addMouseListener(this, false);

                // チップ左のワンクリック・バイパススイッチ (LED ランプ)。
                // チップ左端の角丸を落として (ConnectedOnLeft) スイッチと一体の枠に見せる
                btn->setConnectedEdges(juce::Button::ConnectedOnLeft);
                auto* sw = new PluginBypassSwitch();
                sw->setBypassed(chain.isBypassed(i));
                sw->setTooltip(chain.isBypassed(i)
                    ? tr(u8"バイパス中（クリックでオンに戻す）")
                    : tr(u8"クリックでバイパス（オフ）"));
                sw->onClick = [this, slotIdx] { togglePluginBypass(slotIdx); };
                fxBypassBtns.add(sw);
                addAndMakeVisible(sw);
            }
        }
        else
        {
            // 空きスロット: 中央に "+" を表示（クリックで slot i に追加）
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
    }
}

void TrackHeaderView::togglePluginBypass(int slotIdx)
{
    // Undo + PDC 再構築のため MainComponent の togglePluginBypassUndoable 経由を正とする。
    // チップの見た目更新は PluginBypassAction が onChainChanged を発火 → callAsync で再構築。
    if (onPluginBypassRequest)
    {
        onPluginBypassRequest(slotIdx);
        return;
    }
    auto& chain = track.getPluginChain();   // フォールバック (未配線時のみ)
    chain.setBypassed(slotIdx, !chain.isBypassed(slotIdx));
    rebuildInsertChips();
    resized();
    if (onChanged) onChanged();
}

void TrackHeaderView::updateInputLevels(float peakL, float peakR, float vuL, float vuR)
{
    // 入力値を直接適用せず、滑らかに追従させる。
    // Peak: 立ち上がりは速く (新値が大きければ即時)、立ち下がりはゆっくり
    // VU:   立ち上がり/立ち下がりとも穏やかに
    auto smoothPeak = [](float current, float target)
    {
        if (target >= current) return target;                // 立ち上がり即時
        return current * 0.78f + target * 0.22f;             // 立ち下がりゆっくり
    };
    auto smoothVU = [](float current, float target)
    {
        return current * 0.62f + target * 0.38f;             // 上下とも穏やかに (やや機敏)
    };

    const float newPeakL = smoothPeak(inPeakL, peakL);
    const float newPeakR = smoothPeak(inPeakR, peakR);
    const float newVUL   = smoothVU  (inVUL,   vuL);
    const float newVUR   = smoothVU  (inVUR,   vuR);

    // クリップ検出は「生のピーク」で行う (平滑化で 0dBFS 到達が均されると見落とすため)。
    // 0 dBFS 以上に達したら一定時間 赤を保持して、瞬間的なクリップも視認できるようにする。
    // 保持は MainComponent の 20Hz タイマーが停止後にメータ更新を打ち切る猶予 (idleMeterTicks<30
    // = 1500ms) より必ず短くする。同値/超過だと、停止直前に鳴ったクリップの赤を消す最後の repaint が
    // 更新打ち切りの後に来て、赤が消えず残ってしまう (両者を等しくしない = ここを 1500 未満に保つ)。
    constexpr juce::uint32 kPeakClipHoldMs = 1200;
    if (peakL >= MeterDraw::kRedDb || peakR >= MeterDraw::kRedDb)
        peakClipUntilMs = juce::Time::getMillisecondCounter() + kPeakClipHoldMs;
    const bool clipNow = juce::Time::getMillisecondCounter() < peakClipUntilMs;

    // 変化が小さく、かつクリップ表示状態も変わらなければ repaint を省略
    // (dB で 0.3 未満の差は知覚しづらい / クリップの点灯・消灯時は必ず描き直す)
    auto closeEnough = [](float a, float b) { return std::abs(a - b) < 0.3f; };
    if (clipNow == peakClipShown
        && closeEnough(newPeakL, inPeakL) && closeEnough(newPeakR, inPeakR)
        && closeEnough(newVUL, inVUL)   && closeEnough(newVUR, inVUR))
        return;

    inPeakL = newPeakL; inPeakR = newPeakR;
    inVUL   = newVUL;   inVUR   = newVUR;
    peakClipShown = clipNow;
    repaint(0, 58, juce::jmin(getWidth(), controlsWidth), 20);
}

void TrackHeaderView::refresh()
{
    // 名前 / 移調 / 波形表示はモデルから引き直す (Undo/Redo で変わったときに追従させる)
    if (! nameLabel.isBeingEdited())
        nameLabel.setText(track.getName(), juce::dontSendNotification);
    if (updateMidiInfoDisplay)  updateMidiInfoDisplay();
    if (refreshWaveformDisplay) refreshWaveformDisplay();
    muteBtn.setToggleState(track.isMuted(),        juce::dontSendNotification);
    soloBtn.setToggleState(track.isSoloed(),        juce::dontSendNotification);
    recBtn.setToggleState (track.isRecArmed(),      juce::dontSendNotification);
    monBtn.setToggleState (track.isInputMonitor(),  juce::dontSendNotification);
    if (track.isFolderTrack())
    {
        // フォルダは TList ボタンを Open/Close に転用している
        lanesBtn.setToggleState(track.isFolderOpen(), juce::dontSendNotification);
        lanesBtn.setButtonText(track.isFolderOpen() ? "Close" : "Open");
    }
    else
        lanesBtn.setToggleState(!track.isLanesCollapsed(), juce::dontSendNotification);
    if (track.isAppCaptureTrack())
        appSelectBtn.setButtonText(track.getAppCaptureExe().isNotEmpty()
                                       ? track.getAppCaptureExe()
                                       : tr(u8"アプリを選択…"));
    volSlider.setValue(track.getVolume(),     juce::dontSendNotification);
    panSlider.setValue(track.getPan(),        juce::dontSendNotification);
    revSlider.setValue(track.getReverbSend(), juce::dontSendNotification);
    if (track.isClickTrack())
    {
        clickSoundBox.setSelectedId(track.getClickSound() + 1, juce::dontSendNotification);
        clickAccentBtn.setToggleState(track.isClickAccent(), juce::dontSendNotification);
        clickHalfBtn.setToggleState  (track.getClickRate() == 1, juce::dontSendNotification);
        clickDoubleBtn.setToggleState(track.getClickRate() == 2, juce::dontSendNotification);
    }
    populateInputChannelBox();
    resized();
    repaint();
}

void TrackHeaderView::populateInputChannelBox()
{
    int maxCh = (getNumInputChannels ? getNumInputChannels() : 2);
    maxCh = juce::jmax(1, maxCh);
    inputChBox.clear(juce::dontSendNotification);

    if (track.isStereo())
    {
        // Stereo: ペアの左ch番号 = inputChannel として保存
        // Item ID = 1000 + leftCh（0,2,4,...）
        bool anyPair = false;
        for (int left = 0; left + 1 < maxCh; left += 2)
        {
            inputChBox.addItem("Inputs " + juce::String(left + 1) + "-" + juce::String(left + 2),
                               1000 + left);
            anyPair = true;
        }
        if (!anyPair)
        {
            // 入力が1chしかない場合: 選択不可の Nothing 項目を表示
            inputChBox.addItem(tr(u8"Nothing (入力ペア無し)"), 999);
            inputChBox.setItemEnabled(999, false);
            inputChBox.setSelectedId(999, juce::dontSendNotification);
        }
        else
        {
            int leftCh = track.getInputChannel();
            if (leftCh % 2 != 0) leftCh = 0;  // ステレオは偶数 ch から
            inputChBox.setSelectedId(1000 + leftCh, juce::dontSendNotification);
        }
    }
    else
    {
        // Mono
        for (int i = 0; i < maxCh; ++i)
            inputChBox.addItem("Input " + juce::String(i + 1), i + 1);
        int ch = track.getInputChannel();
        inputChBox.setSelectedId(ch + 1, juce::dontSendNotification);
    }
}

bool TrackHeaderView::isInResizeZone(const juce::MouseEvent& e) const
{
    // メイン部の境界（折りたたみ時 = ヘッダー末尾、展開時 = メイン/レーン境界）
    const int mainH = track.getMainHeight();
    return std::abs(e.y - mainH) <= resizeZone;
}

bool TrackHeaderView::isInLaneResizeZone(const juce::MouseEvent& e) const
{
    // 展開時のみ末尾レーンの下端でリサイズ可能
    if (track.isLanesCollapsed() || track.getLaneCount() <= 1) return false;
    return e.y >= getHeight() - resizeZone;
}

juce::Rectangle<int> TrackHeaderView::getLaneSoloBtnRect(int laneIndex) const
{
    const int mainH = track.getMainHeight();
    const int laneH = track.getLaneHeight();
    int subY = mainH + (laneIndex - 1) * laneH;
    int bW = 20, bH = 16;
    return { getWidth() - bW - 4, subY + (laneH - bH) / 2, bW, bH };
}

juce::Rectangle<int> TrackHeaderView::getLanePromoteBtnRect(int laneIndex) const
{
    // S ボタンの左隣に同サイズで配置 (INS 列の有無に依らず S と一緒に動く)。
    constexpr int gap = 2;
    auto s = getLaneSoloBtnRect(laneIndex);
    return s.translated(-(s.getWidth() + gap), 0);
}

juce::String TrackHeaderView::getTooltip()
{
    // 折りたたみ時はテイク行ボタンを描いていない (paint の早期 return と対称)。
    if (track.isLanesCollapsed() || track.getLaneCount() <= 1) return {};
    const auto p = getMouseXYRelative();
    for (int li = 1; li < track.getLaneCount(); ++li)
    {
        if (getLanePromoteBtnRect(li).contains(p))
            return tr(u8"このテイクを録音レーンへ差し込みます。"
                      "選択範囲があればその範囲だけ、無ければ選択中のクリップを採用し、"
                      "差し込んだ前後は自動でクロスフェードして繋ぎます。"
                      "タイムラインでテイクを選んでから Shift+↑ でも同じ操作ができます");
        if (getLaneSoloBtnRect(li).contains(p))
            return tr(u8"このテイクだけを試聴 (ソロ) します。録り直した複数のテイクを"
                      "聴き比べて、どれを採用するか選ぶときに使います");
    }
    return {};
}

void TrackHeaderView::paint(juce::Graphics& g)
{
    const int w = getWidth(), laneCount = track.getLaneCount();
    const int totalH   = getHeight();
    const bool collapsed = track.isLanesCollapsed() || laneCount <= 1;
    const int mainH = collapsed ? totalH : track.getMainHeight();
    const int laneH = track.getLaneHeight();

    g.fillAll(AppColours::headerBg);

    // 選択中トラックのハイライト
    // ※ INS スロット表示中は枠が INS パネルからはみ出てオレンジ線が見えるため、
    //    ハイライトはコントロール領域 (0..controlsWidth) のみに限定する。
    if (selected)
    {
        const int selW = (track.isInsertSlotsVisible() && w > controlsWidth) ? controlsWidth : w;
        g.setColour(AppColours::accent.withAlpha(0.12f));
        g.fillRect(0, 0, selW, mainH);
        g.setColour(AppColours::accent.withAlpha(0.6f));
        g.drawRect(0, 0, selW, mainH, 1);
    }

    g.setColour(track.getColour());
    g.fillRect(0, 0, 4, totalH);

    // フォルダトラック: グループバスであることが分かるよう、トラック色の薄い下地を敷く
    // (旧 "FOLDER" バッジは Pan 行と重なって文字化けして見えたため撤去。識別は下地 +
    //  Open/Close ボタンで足りる)
    if (track.isFolderTrack())
    {
        const int selW = (track.isInsertSlotsVisible() && w > controlsWidth) ? controlsWidth : w;
        g.setColour(track.getColour().withAlpha(0.10f));
        g.fillRect(4, 0, selW - 4, mainH);
    }

    if (track.isRecArmed())
    {
        g.setColour(AppColours::recRed.withAlpha(0.08f));
        g.fillRect(4, 0, w - 4, mainH);
    }

    // ── INS スロット枠 (トラック右側に固定スロット) ──
    if (track.isInsertSlotsVisible() && w > controlsWidth + 8)
    {
        const int frameX = controlsWidth + 4;
        const int frameY = 0;            // 上端から
        const int frameW = w - controlsWidth - 8;
        // INS パネルは圧縮しない: 常に固定サイズ (insFrameH / insSlotH) で描き、トラックを縮めた
        // 時はメイン部の高さでクリップして「そのままのサイズで下が隠れる」ようにする。
        const int insVisibleH = juce::jmin(insFrameH, mainH);

        // 既存コントロール領域との区切り（縦の太線）
        g.setColour(juce::Colour(0xff121316));
        g.fillRect(controlsWidth, 0, 4, mainH);

        // 可視領域でクリップしてから固定サイズで描く (はみ出しがレーン/タイムラインに侵食しない)
        juce::Graphics::ScopedSaveState clipState(g);
        g.reduceClipRegion(controlsWidth, 0, w - controlsWidth, insVisibleH);

        // 背景パネル（やや暗め）— 常に固定 insFrameH。下端はクリップで隠れる
        g.setColour(juce::Colour(0xff1c1f24));
        g.fillRoundedRectangle((float)frameX, (float)frameY, (float)frameW, (float)insFrameH, 4.0f);
        // 外枠
        g.setColour(juce::Colour(0xff3a3d42));
        g.drawRoundedRectangle((float)frameX + 0.5f, (float)frameY + 0.5f,
                               (float)frameW - 1.0f, (float)insFrameH - 1.0f, 4.0f, 1.0f);

        // 上端の "INSERTS" ヘッダ
        g.setColour(AppColours::textDim.withAlpha(0.7f));
        g.setFont(juce::FontOptions(9.0f, juce::Font::bold));
        g.drawText("INSERTS", frameX + 6, frameY + 1, frameW - 12, 12,
                   juce::Justification::centredLeft);

        // スロット区切り線 — 固定スロット高 (insSlotH) で。圧縮しない (はみ出しはクリップ)
        const int innerY = frameY + 13;
        const int slotH  = insSlotH;
        g.setColour(juce::Colour(0xff2a2d31));
        for (int i = 1; i < Track::insertSlotCount; ++i)
        {
            const int y = innerY + i * slotH;
            g.fillRect(frameX + 6, y, frameW - 12, 1);
        }

        // D&D 中の落下先ハイライト
        if (dropHighlightSlot >= 0 && dropHighlightSlot < Track::insertSlotCount)
        {
            const int hy = innerY + dropHighlightSlot * slotH;
            const bool copyMode = juce::ModifierKeys::currentModifiers.isAltDown();
            const auto col = copyMode ? juce::Colour(0xff44dd88) : AppColours::accent;
            g.setColour(col.withAlpha(0.18f));
            g.fillRoundedRectangle((float)(frameX + 4), (float)(hy + 1),
                                   (float)(frameW - 8), (float)(slotH - 2), 3.0f);
            g.setColour(col.withAlpha(0.85f));
            g.drawRoundedRectangle((float)(frameX + 4) + 0.5f, (float)(hy + 1) + 0.5f,
                                   (float)(frameW - 8) - 1.0f, (float)(slotH - 2) - 1.0f,
                                   3.0f, 1.5f);
        }
    }

    g.setColour(AppColours::textDim);
    g.setFont(juce::FontOptions(10.0f));
    // スライダーと同じ Y/高さで描画 → 縦中央揃え。
    // トラックを縮めて収まらない行は resized() でコントロールを隠すので、ラベルも描かない
    // (mainH 基準は resized() の showVolRow / showPanRevRow と一致させる)。
    if (mainH >= 44)
        g.drawText("Vol", 6, 30, 22, 12, juce::Justification::centredLeft);
    if (mainH >= 58 && folderExtrasOn())   // フォルダで Pan/Rev 非表示設定ならラベルも描かない
    {
        g.drawText("Pan", 6, 44, 22, 12, juce::Justification::centredLeft);
        // Rev ラベルは Pan スライダー右端の少し先 (Pan は左半分まで)
        const int wLbl = juce::jmin(getWidth(), controlsWidth);
        const int panW = (wLbl - 34) / 2;
        const int revLblX = 30 + panW + 6;
        if (!track.isAppCaptureTrack())   // アプリトラックは Rev 非対応 (スライダーも非表示)
            g.drawText("Rev", revLblX, 44, 22, 12, juce::Justification::centredLeft);
    }

    // 入力レベルメータ（Peak + VU）-- クリックトラックや、縮めて収まらない時は表示しない
    if (!track.isClickTrack() && mainH >= 77)
    {
        const int meterX = 30;
        // INS スロット表示中はそちらに侵食しないよう、コントロール領域内に収める
        const int controlsRight = juce::jmin(w, controlsWidth);
        const int meterW = controlsRight - meterX - 4;
        const int peakY  = 60;
        const int vuY    = 69;   // 隙間を広めに（旧 67）
        const int meterH = 6;    // 旧 5

        auto norm = [](float db) {
            return juce::jlimit(0.0f, 1.0f, (db + 60.0f) / 60.0f);
        };

        // 閾値 (overThreshDb) を超えた区間だけ別色 (overCol) で塗り重ねる:
        //   Peak → -3dB 超を赤 (クリッピング近傍) / VU → 0VU 基準超を黄
        auto drawBar = [&](int x, int y, int width, float db,
                           juce::Colour col, float overThreshDb, juce::Colour overCol)
        {
            if (width <= 0) return;
            int lvW = (int)(norm(db) * width);
            if (lvW <= 0) return;
            g.setColour(col);
            g.fillRect(x, y, lvW, meterH);
            if (db > overThreshDb)
            {
                int overStart = (int)(norm(overThreshDb) * width);
                g.setColour(overCol);
                g.fillRect(x + overStart, y, lvW - overStart, meterH);
            }
        };

        // ステレオでも 1 本に集約 (L/R の最大値)
        auto drawMeter = [&](int y, float dbL, float dbR, juce::Colour col,
                             float overThreshDb, juce::Colour overCol)
        {
            g.setColour(juce::Colour(0xff1a1a1a));
            g.fillRect(meterX, y, meterW, meterH);
            drawBar(meterX, y, meterW, juce::jmax(dbL, dbR), col, overThreshDb, overCol);
        };

        // Peak メーター: 緑 (< -6dB) → 黄 (-6〜0dB の警告域) → 0 dBFS 到達で右端に赤クリップ表示。
        // 「赤 = 実際に割れた (クリップ)」「黄 = クリップ手前」で他 DAW / アプリ縦メーターと同基準
        // (MeterDraw::kYellowDb / kRedDb)。クリップは updateInputLevels が一定時間保持する。
        auto drawPeakMeter = [&](int y, float dbL, float dbR)
        {
            g.setColour(juce::Colour(0xff1a1a1a));
            g.fillRect(meterX, y, meterW, meterH);
            const int levelW  = (int)(norm(juce::jmax(dbL, dbR)) * meterW);
            if (levelW > 0)
            {
                const int yellowX = (int)(norm(MeterDraw::kYellowDb) * meterW);  // -6dB 位置
                g.setColour(AppColours::meterGreen);
                g.fillRect(meterX, y, juce::jmin(levelW, yellowX), meterH);      // 緑: 〜-6dB
                if (levelW > yellowX)                                            // 黄: -6dB〜
                {
                    g.setColour(AppColours::meterYellow);
                    g.fillRect(meterX + yellowX, y, levelW - yellowX, meterH);
                }
            }
            if (peakClipShown)                                                   // 赤: クリップ保持中
            {
                const int clipW = juce::jmax(3, meterW / 36);   // 右端の小さな赤ブロック
                g.setColour(AppColours::meterRed);
                g.fillRect(meterX + meterW - clipW, y, clipW, meterH);
            }
        };

        drawPeakMeter(peakY, inPeakL, inPeakR);
        drawMeter(vuY,   inVUL,   inVUR,   juce::Colour(0xff5599cc),
                  vuReferenceLevel, AppColours::meterYellow);

        // メータ上に基準となる縦線を引くヘルパ（モノ/ステレオ共通で 1 本）
        auto drawRefLine = [&](int y, float thresholdDb, juce::Colour col)
        {
            if (thresholdDb <= -60.0f || thresholdDb >= 0.0f) return;
            const float refNorm = juce::jlimit(0.0f, 1.0f, (thresholdDb + 60.0f) / 60.0f);
            const int refX = meterX + (int)(refNorm * meterW);
            g.setColour(col);
            g.drawLine((float) refX, (float) (y - 1),
                       (float) refX, (float) (y + meterH + 1), 1.0f);
        };

        // Peak の緑/黄 境界線（-6dB = クリップ手前の警告域の始まり）
        drawRefLine(peakY, MeterDraw::kYellowDb, AppColours::accent.withAlpha(0.7f));
        // 0 VU 基準線
        drawRefLine(vuY, vuReferenceLevel, AppColours::accent.withAlpha(0.7f));

        g.setColour(AppColours::textDim);
        g.setFont(juce::FontOptions(9.0f));
        // Vol/Pan と同じ x=6 / w=22 / centredLeft で並びを揃える
        g.drawText("Peak", 6, peakY - 1, 22, meterH + 2, juce::Justification::centredLeft);
        g.drawText("VU",   6, vuY - 1,   22, meterH + 2, juce::Justification::centredLeft);
    }

    if (collapsed)
    {
        // 折りたたみ時は末尾の区切り線のみ
        g.setColour(AppColours::separator);
        g.drawLine(0.0f, (float)totalH - 1.0f, (float)w, (float)totalH - 1.0f, 1.0f);
        return;
    }

    // 展開時のみメイン/レーン分割線
    g.setColour(AppColours::separator);
    g.drawLine(0.0f, (float)mainH - 1.0f,
               (float)w, (float)mainH - 1.0f, 1.0f);

    for (int li = 1; li < laneCount; ++li)
    {
        int subY = mainH + (li - 1) * laneH;
        int subH = laneH;

        g.setColour(AppColours::panelBg.darker(0.15f));
        g.fillRect(subLaneIndent, subY, w - subLaneIndent, subH);
        g.setColour(AppColours::background.withAlpha(0.8f));
        g.fillRect(4, subY, subLaneIndent - 4, subH);
        g.setColour(track.getColour().withAlpha(0.6f));
        g.fillRect(subLaneIndent - 3, subY + 5, 2, subH - 10);

        g.setColour(AppColours::textDim);
        g.setFont(juce::FontOptions(11.0f));  // Take番号も少し大きく
        g.drawText("Take " + juce::String(li),
                   subLaneIndent + 4, subY + (subH - 16) / 2, 80, 16,
                   juce::Justification::centredLeft);

        auto sRect = getLaneSoloBtnRect(li);
        auto* lane = track.getLane(li);
        bool lSoloed = (lane && lane->soloed);
        g.setColour(lSoloed ? juce::Colour(0xff44aaff) : AppColours::buttonBg);
        g.fillRoundedRectangle(sRect.toFloat(), 2.0f);
        g.setColour(lSoloed ? juce::Colours::black : AppColours::textDim);
        g.setFont(juce::FontOptions(11.0f));
        g.drawText("S", sRect, juce::Justification::centred);
        g.setColour(AppColours::separator);
        g.drawRoundedRectangle(sRect.toFloat(), 2.0f, 1.0f);

        // ── ↑ (このテイクを採用) ボタン ── S の左隣。
        // 活性 = アクセント色, 非活性 = dim グレー。クリック選択中 or 範囲選択中に活性化。
        auto pRect = getLanePromoteBtnRect(li);
        const bool canPromote = getLanePromoteEnabled && getLanePromoteEnabled(li);
        g.setColour(canPromote ? AppColours::accent : AppColours::buttonBg);
        g.fillRoundedRectangle(pRect.toFloat(), 2.0f);
        g.setColour(canPromote ? juce::Colours::black : AppColours::textDim.withAlpha(0.45f));
        g.setFont(juce::FontOptions(12.0f, juce::Font::bold));
        g.drawText(juce::String::charToString((juce::juce_wchar) 0x2191),  // ↑
                   pRect, juce::Justification::centred);
        g.setColour(AppColours::separator);
        g.drawRoundedRectangle(pRect.toFloat(), 2.0f, 1.0f);

        g.drawLine(0.0f, (float)(subY + subH - 1), (float)w, (float)(subY + subH - 1), 1.0f);
    }

    g.setColour(AppColours::separatorLight.withAlpha(0.5f));
    g.drawLine(4.0f, (float)getHeight() - 1.0f, (float)w - 4.0f, (float)getHeight() - 1.0f, 2.0f);
}

void TrackHeaderView::resized()
{
    // 既存コントロールは左寄せの controlsWidth（基本幅）に収める。
    // INS スロット列が global で開いていると header が広くなるが、
    // その追加幅は INS 列専用とし、コントロールは常に同じ位置に揃える。
    const bool slotsVisible = track.isInsertSlotsVisible();
    const int w = juce::jmin(getWidth(), controlsWidth);
    const int insAreaW = juce::jmax(0, getWidth() - controlsWidth);
    const int bW = 20, bH = 17, bY = 8, gap = 2;

    // メイン部の高さに応じて、収まらない行のコントロールを隠す。コントロールは固定 Y に
    // 置かれるため、足りない行を出すと半端に見切れる。これで「トラック名 (M/S/R/I) の行だけ」
    // まで縮められるようにする (paint() の各行描画も同じ mainH 基準でガードする)。
    const int  lanesForVis  = track.getLaneCount();
    const int  mainHForVis  = (track.isLanesCollapsed() || lanesForVis <= 1)
                                 ? getHeight() : track.getMainHeight();
    const bool showVolRow    = mainHForVis >= 44;   // Vol     (y=30..42)
    const bool showPanRevRow = mainHForVis >= 58;   // Pan/Rev (y=44..56)
    const bool showBottomRow = mainHForVis >= 96;   // 底部行  (y=80..95)

    int bx = w - bW - 4;
    monBtn.setBounds (bx, bY, bW, bH); bx -= bW + gap;
    recBtn.setBounds (bx, bY, bW, bH); bx -= bW + gap;
    soloBtn.setBounds(bx, bY, bW, bH); bx -= bW + gap;
    muteBtn.setBounds(bx, bY, bW, bH);
    const int badgeW = 36;  // "stereo" が収まる幅
    if (!track.isClickTrack())
    {
        int badgeX = bx - badgeW - gap;
        stereoBadge.setBounds(badgeX, bY, badgeW, bH);
        nameLabel.setBounds(6, bY, badgeX - 10, bH);
    }
    else
    {
        stereoBadge.setVisible(false);
        nameLabel.setBounds(6, bY, bx - 10, bH);
    }

    volSlider.setBounds(30, 30, w - 34, 12);
    {
        // Pan: 横幅を半分にして左寄せ。右半分は Rev (リバーブセンド) スライダー。
        const int rowW  = w - 34;
        const int panW  = rowW / 2;
        const int revLblW = 24;            // "Rev" ラベル分の余白
        const int revX  = 30 + panW + 6 + revLblW;
        const int revW  = juce::jmax(20, w - revX - 4);
        panSlider.setBounds(30, 44, panW, 12);
        revSlider.setBounds(revX, 44, revW, 12);
    }
    volSlider.setVisible(showVolRow);
    // フォルダトラックは表示設定 (showFolderPanRevIns) で Pan/Rev を隠せる
    panSlider.setVisible(showPanRevRow && folderExtrasOn());
    // アプリケーショントラックは Rev 非対応 (エンジンにリバーブ送り経路が無い) → 常に非表示
    revSlider.setVisible(showPanRevRow && folderExtrasOn() && !track.isAppCaptureTrack());

    // INS スロット枠 (paint() で描いた外枠の内側にチップを敷く)
    if (slotsVisible && insAreaW > 0)
    {
        const int totalH = getHeight();
        const int laneCount = track.getLaneCount();
        const bool collapsed = track.isLanesCollapsed() || laneCount <= 1;
        const int mainH = collapsed ? totalH : track.getMainHeight();
        // INS パネルは圧縮しない: スロットは常に固定高 (insSlotH) で配置し、メイン部の高さに
        // 収まらないスロットは「そのままのサイズで」非表示にする (paint() も同じ基準でクリップ)。
        const int insVisibleH = juce::jmin(insFrameH, mainH);

        const int frameX = controlsWidth + 4;
        const int frameW = getWidth() - controlsWidth - 8;
        const int innerY = 13;            // "INSERTS" ヘッダ分
        const int slotH  = insSlotH;      // 固定スロット高 (圧縮しない)
        const int padX   = 6;
        for (int i = 0; i < fxChips.size(); ++i)
        {
            const int chipY = innerY + i * slotH + 1;
            const int chipH = slotH - 2;
            // スロット全体が可視領域に収まる時だけ表示 (収まらない分は隠す)
            const bool fits = (chipY + chipH) <= insVisibleH;
            auto* sw = (i < fxBypassBtns.size() ? fxBypassBtns[i] : nullptr);
            fxChips[i]->setVisible(fits);
            if (sw) sw->setVisible(fits);
            if (fits)
            {
                int cx = frameX + padX;
                int cw = frameW - padX * 2;
                if (sw)
                {
                    // 左端にバイパススイッチ (隙間ゼロ = チップと一体の枠の左セグメント)
                    const int swW = 16;
                    sw->setBounds(cx, chipY, swW, chipH);
                    cx += swW;
                    cw -= swW;
                }
                fxChips[i]->setBounds(cx, chipY, cw, chipH);
            }
        }
    }
    else
    {
        for (auto* chip : fxChips) chip->setVisible(false);
        for (auto* sw : fxBypassBtns) if (sw) sw->setVisible(false);
    }

    // 底部: [TList] 左40%  |  In: [ComboBox] 右60%（メータ行ぶん下げる）
    const int botY = 80, botH = 15;
    const int tlistX = 8;  // 枠から少し離す（4→8）
    const int tlistW = (w - tlistX - 4) * 2 / 5;
    lanesBtn.setBounds(tlistX, botY, tlistW, botH);

    const int inStart  = tlistX + tlistW + 4;
    const int inLabelW = 22;
    inputLabel.setBounds(inStart, botY, inLabelW, botH);
    inputChBox.setBounds(inStart + inLabelW, botY, w - inStart - inLabelW - 6, botH);

    // 底部行は音声トラックのみ (MIDI/Click はコンストラクタで非表示。ここで再表示しない)。
    // フォルダは録音入力を持たないので In: は出さない (lanesBtn = Open/Close は出す)。
    // アプリケーショントラックは In: の位置にアプリ選択ボタンを出す (TList/入力は無し)
    if (track.isAppCaptureTrack())
    {
        lanesBtn.setVisible(false);
        inputLabel.setVisible(false);
        inputChBox.setVisible(false);
        appSelectBtn.setBounds(inStart, botY, w - inStart - 6, botH);
        appSelectBtn.setVisible(showBottomRow);
    }
    else if (!track.isMidiTrack() && !track.isClickTrack())
    {
        lanesBtn.setVisible(showBottomRow);
        inputLabel.setVisible(showBottomRow && !track.isFolderTrack());
        inputChBox.setVisible(showBottomRow && !track.isFolderTrack());
    }

    // クリックトラック専用レイアウト: 底部 [Sound][½][x2][ACC]
    if (track.isClickTrack())
    {
        clickSoundBox.setVisible(showBottomRow);
        clickAccentBtn.setVisible(showBottomRow);
        clickHalfBtn.setVisible(showBottomRow);
        clickDoubleBtn.setVisible(showBottomRow);
        const int accW = 30, halfW = 22, dblW = 26;
        const int rightMargin = 6, gap = 2;
        int rx = w - rightMargin;
        clickAccentBtn.setBounds(rx - accW, botY, accW, botH); rx -= accW + gap;
        clickDoubleBtn.setBounds(rx - dblW, botY, dblW, botH); rx -= dblW + gap;
        clickHalfBtn.setBounds  (rx - halfW, botY, halfW, botH); rx -= halfW + gap;
        clickSoundBox.setBounds(tlistX, botY, rx - tlistX, botH);
    }

    // MIDI トラック専用レイアウト: 底部 [Oct▼][Oct▲][±info][♭][♯]   [Wav]
    if (track.isMidiTrack())
    {
        octDownBtn.setVisible(showBottomRow);
        octUpBtn.setVisible(showBottomRow);
        semiDownBtn.setVisible(showBottomRow);
        semiUpBtn.setVisible(showBottomRow);
        midiInfoLabel.setVisible(showBottomRow);
        waveformBtn.setVisible(showBottomRow);
        const int leftMargin  = 8;
        const int rightMargin = 6;
        const int gap         = 2;
        const int infoW       = 28;
        const int octW        = 30;
        const int semiW       = 18;
        const int wavW        = 34;

        // 右端: 波形ボタン
        int rx = w - rightMargin;
        waveformBtn.setBounds(rx - wavW, botY, wavW, botH);

        // 左から: Oct▼ Oct▲ [info] ♭ ♯
        int lx = leftMargin;
        octDownBtn.setBounds(lx, botY, octW, botH); lx += octW + gap;
        octUpBtn  .setBounds(lx, botY, octW, botH); lx += octW + gap;
        midiInfoLabel.setBounds(lx, botY, infoW, botH); lx += infoW + gap;
        semiDownBtn.setBounds(lx, botY, semiW, botH); lx += semiW + gap;
        semiUpBtn  .setBounds(lx, botY, semiW, botH);
    }
}


// アプリケーショントラックの取り込み対象アプリ選択メニュー。開くたびに listAudioApps() で
// 「いま音を出しているアプリ」を列挙し直す (COM 列挙はクリック時のみ = 毎 refresh で払わない)。
// 保存済みの対象が現在鳴っていない場合も選択を維持できるよう「(停止中)」として補完する。
void TrackHeaderView::showAppSelectMenu()
{
    juce::PopupMenu m;
    juce::StringArray exes;
    const juce::String cur = track.getAppCaptureExe();
    for (const auto& app : AppAudioCapture::listAudioApps())
    {
        exes.add(app.executable);
        m.addItem(100 + exes.size() - 1, app.displayName, true,
                  app.executable.equalsIgnoreCase(cur));
    }
    if (cur.isNotEmpty() && !exes.contains(cur, /*ignoreCase*/ true))
    {
        exes.add(cur);
        m.addItem(100 + exes.size() - 1, cur + tr(u8" (停止中)"), true, true);
    }
    if (exes.isEmpty())
        m.addItem(1, tr(u8"(音を再生中のアプリがありません)"), false);
    m.addSeparator();
    m.addItem(2, tr(u8"選択を解除"), cur.isNotEmpty());

    m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&appSelectBtn),
        [safe = juce::Component::SafePointer<TrackHeaderView>(this), exes](int result)
        {
            if (safe == nullptr || result == 0) return;
            auto* self = safe.getComponent();
            if (result == 2)
                self->track.setAppCaptureExe({});
            else if (result >= 100 && result - 100 < exes.size())
                self->track.setAppCaptureExe(exes[result - 100]);
            else
                return;
            self->refresh();
            // dirty 化 (onTrackChanged 経由)。キャプチャの起動/切替は syncAppCaptureTracks
            // (20Hz) が exe 変化を検出して行う
            if (self->onChanged) self->onChanged();
        });
}
