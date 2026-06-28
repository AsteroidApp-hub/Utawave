// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

// MainComponent の環境設定ダイアログ実装 (showPreferences)。
// MainComponent_Dialogs.cpp から分割。

#include "MainComponent.h"
#include "Localisation.h"
#include "Audio/AudioDeviceSettings.h"
#include "Export/ExportEngine.h"
#include "Export/ExportDialog.h"
#include "MIDI/MidiImporter.h"
#include "MIDI/MidiImportDialog.h"

void MainComponent::showPreferences()
{
    class PrefsDlg : public juce::Component
    {
    public:
        juce::Label    languageLabel, bitsLabel, behaviorLabel, recLabel, autoSaveLabel,
                       backupCountLabel, vuRefLabel, loudnessLabel;
        juce::ComboBox languageCombo, bitsCombo, autoSaveCombo, backupCountCombo, vuRefCombo, loudnessCombo;
        std::function<void(int)>   onLanguageChanged;   // 1=日本語, 2=English
        juce::ToggleButton followSelBtn, retroBtn, rtzBtn, autoNormBtn, zoomMouseBtn, peakGuardBtn, zeroCrossBtn, stripMetaBtn;
        juce::ToggleButton showMidiExportBtn;   // 初期状態 / コールバックは showPreferences 側で設定 (アプリ全体設定)
        juce::ToggleButton midiPagingBtn;       // MIDI ピアノロールの自動ページング (アプリ全体設定。初期状態は showPreferences 側)
        juce::ToggleButton tooltipsBtn;         // ツールチップ表示 (アプリ全体設定。初期状態は showPreferences 側)
        juce::ToggleButton showAdsBtn;          // 起動画面の広告表示 (アプリ全体設定。初期状態は showPreferences 側で設定)
        juce::ToggleButton recCompBtn;          // 録音レイテンシ自動補正 (アプリ全体設定。初期状態は showPreferences 側)
        juce::ToggleButton monInsBtn;           // 入力モニターに INS を通す (アプリ全体設定。初期状態は showPreferences 側)
        juce::ToggleButton diskStreamBtn;       // ディスクストリーミング (アプリ全体設定。初期状態は showPreferences 側)
        juce::Label        recCompOffsetLabel;
        juce::Slider       recCompOffsetSlider; // 追加の手動オフセット (ms)
        juce::Label        exportLabel, startupLabel;
        const bool         adsUi { AppPreferences::adsCompiledIn() };  // 広告がコンパイル時有効な時だけ UI を出す
        juce::TextButton closeBtn, resetBtn;
        // 設定項目は縦に長いので Viewport でスクロールさせる (ウィンドウは従来の約半分の高さ)。
        // resetBtn / closeBtn だけはダイアログ下部に固定し、スクロールしなくても押せるようにする
        juce::Viewport  viewport;
        juce::Component content;
        std::function<void(int)>   onBitsChanged;
        std::function<void(bool)>  onFollowSelChanged;
        std::function<void(bool)>  onRetroChanged;
        std::function<void(bool)>  onRtzChanged;
        std::function<void(int)>   onAutoSaveChanged;
        std::function<void(int)>   onBackupCountChanged;
        std::function<void(float)> onVuRefChanged;
        std::function<void(float)> onLoudnessTargetChanged;
        std::function<void(bool)>  onAutoNormChanged;
        std::function<void(bool)>  onZoomMouseChanged;
        std::function<void(bool)>  onPeakGuardChanged;
        std::function<void(bool)>  onZeroCrossChanged;
        std::function<void(bool)>  onStripMetaChanged;
        std::function<void(bool)>  onShowMidiExportChanged;
        std::function<void(bool)>  onMidiPagingChanged;
        std::function<void(bool)>  onTooltipsChanged;
        std::function<void(bool)>  onShowAdsChanged;
        std::function<void(bool)>  onRecCompChanged;
        std::function<void(double)> onRecCompOffsetChanged;
        std::function<void(bool)>  onMonInsChanged;
        std::function<void(bool)>  onDiskStreamChanged;
        std::function<void()>      onResetDefaults;

        PrefsDlg(int curBits, bool curFollowSel, bool curRetro, bool curRtz,
                 int curAutoSaveMin, int curMaxBackups, float curVuRefDb, float curLoudnessTargetLufs,
                 bool curAutoNorm, bool curZoomMouse, bool curPeakGuard, bool curZeroCross,
                 bool curStripMeta)
        {
            auto setupLabel = [this](juce::Label& l, juce::String txt, float fontSize, juce::Colour col) {
                l.setText(txt, juce::dontSendNotification);
                l.setColour(juce::Label::textColourId, col);
                l.setFont(juce::FontOptions(fontSize));
                addAndMakeVisible(l);
            };
            setupLabel(languageLabel, tr(u8"言語 (Language)  ※再起動で反映"), 13.0f, juce::Colours::white);
            // 言語名は各言語の表記のまま (翻訳しない)
            languageCombo.addItem(juce::String::fromUTF8(u8"日本語"), 1);
            languageCombo.addItem("English", 2);
            languageCombo.addItem(juce::String::fromUTF8(u8"简体中文"), 3);
            languageCombo.addItem(juce::String::fromUTF8(u8"繁體中文"), 4);
            languageCombo.addItem(juce::String::fromUTF8(u8"한국어"), 5);
            {
                int curId = 1;
                switch (Localisation::getSavedLanguage())
                {
                    case Localisation::Language::English:            curId = 2; break;
                    case Localisation::Language::SimplifiedChinese:  curId = 3; break;
                    case Localisation::Language::TraditionalChinese: curId = 4; break;
                    case Localisation::Language::Korean:             curId = 5; break;
                    case Localisation::Language::Japanese:
                    default:                                         curId = 1; break;
                }
                languageCombo.setSelectedId(curId, juce::dontSendNotification);
            }
            languageCombo.setColour(juce::ComboBox::backgroundColourId, juce::Colour(0xff3a3a3a));
            languageCombo.setColour(juce::ComboBox::textColourId, juce::Colours::white);
            languageCombo.setColour(juce::ComboBox::arrowColourId, juce::Colours::white);
            languageCombo.setColour(juce::ComboBox::outlineColourId, juce::Colour(0xff555555));
            languageCombo.onChange = [this] {
                if (onLanguageChanged) onLanguageChanged(languageCombo.getSelectedId());
            };
            addAndMakeVisible(languageCombo);

            setupLabel(bitsLabel, tr(u8"インポート時のリサンプル出力"), 13.0f, juce::Colours::white);
            setupLabel(behaviorLabel, tr(u8"編集動作"), 13.0f, juce::Colours::white);
            setupLabel(recLabel,      tr(u8"録音フロー"), 13.0f, juce::Colours::white);
            setupLabel(autoSaveLabel, tr(u8"自動保存"), 13.0f, juce::Colours::white);
            setupLabel(backupCountLabel, tr(u8"バックアップを残す数 (古い世代から自動削除)"), 13.0f, juce::Colours::white);
            setupLabel(vuRefLabel,    tr(u8"VU メータ基準レベル (0 VU)"), 13.0f, juce::Colours::white);
            setupLabel(loudnessLabel, tr(u8"ラウドネス自動調整ターゲット"), 13.0f, juce::Colours::white);
            setupLabel(exportLabel,   tr(u8"書き出し"), 13.0f, juce::Colours::white);
            if (adsUi)
                setupLabel(startupLabel,  tr(u8"起動画面"), 13.0f, juce::Colours::white);

            bitsCombo.addItem("32-bit float", 32);
            bitsCombo.addItem("24-bit", 24);
            bitsCombo.setSelectedId(curBits == 24 ? 24 : 32, juce::dontSendNotification);
            bitsCombo.setColour(juce::ComboBox::backgroundColourId, juce::Colour(0xff3a3a3a));
            bitsCombo.setColour(juce::ComboBox::textColourId, juce::Colours::white);
            bitsCombo.setColour(juce::ComboBox::arrowColourId, juce::Colours::white);
            bitsCombo.setColour(juce::ComboBox::outlineColourId, juce::Colour(0xff555555));
            bitsCombo.onChange = [this] {
                if (onBitsChanged) onBitsChanged(bitsCombo.getSelectedId());
            };
            addAndMakeVisible(bitsCombo);

            // インポート時の不要メタデータ除去 (他 DAW のテンポ/ループ情報の流入防止)。既定 ON。
            stripMetaBtn.setButtonText(
                tr(u8"インポート時に不要なタグを削除する (テンポ等の埋め込み情報)"));
            stripMetaBtn.setToggleState(curStripMeta, juce::dontSendNotification);
            stripMetaBtn.setColour(juce::ToggleButton::textColourId, juce::Colours::white);
            stripMetaBtn.onClick = [this] {
                if (onStripMetaChanged) onStripMetaChanged(stripMetaBtn.getToggleState());
            };
            addAndMakeVisible(stripMetaBtn);

            followSelBtn.setButtonText(
                tr(u8"再生バーを選択先頭に追従させる (Insertion Follows Selection)"));
            followSelBtn.setToggleState(curFollowSel, juce::dontSendNotification);
            followSelBtn.setColour(juce::ToggleButton::textColourId, juce::Colours::white);
            followSelBtn.onClick = [this] {
                if (onFollowSelChanged) onFollowSelChanged(followSelBtn.getToggleState());
            };
            addAndMakeVisible(followSelBtn);

            rtzBtn.setButtonText(
                tr(u8"停止時に再生開始位置へ戻る (Return To Zero)"));
            rtzBtn.setToggleState(curRtz, juce::dontSendNotification);
            rtzBtn.setColour(juce::ToggleButton::textColourId, juce::Colours::white);
            rtzBtn.onClick = [this] {
                if (onRtzChanged) onRtzChanged(rtzBtn.getToggleState());
            };
            addAndMakeVisible(rtzBtn);

            zoomMouseBtn.setButtonText(platformShortcutText(
                tr(u8"Cmd+スクロール拡大の起点をマウス位置にする (OFF: 再生バー中央)")));
            zoomMouseBtn.setToggleState(curZoomMouse, juce::dontSendNotification);
            zoomMouseBtn.setColour(juce::ToggleButton::textColourId, juce::Colours::white);
            zoomMouseBtn.onClick = [this] {
                if (onZoomMouseChanged) onZoomMouseChanged(zoomMouseBtn.getToggleState());
            };
            addAndMakeVisible(zoomMouseBtn);

            zeroCrossBtn.setButtonText(
                tr(u8"クロスフェードをゼロクロス点でつなぐ"));
            zeroCrossBtn.setToggleState(curZeroCross, juce::dontSendNotification);
            zeroCrossBtn.setColour(juce::ToggleButton::textColourId, juce::Colours::white);
            zeroCrossBtn.onClick = [this] {
                if (onZeroCrossChanged) onZeroCrossChanged(zeroCrossBtn.getToggleState());
            };
            addAndMakeVisible(zeroCrossBtn);

            // MIDI ピアノロールの自動ページング (アプリ全体設定。初期状態は showPreferences 側)
            midiPagingBtn.setButtonText(
                tr(u8"MIDI 編集の再生バーがビュー外へ出たら自動でページ送りする"));
            midiPagingBtn.setColour(juce::ToggleButton::textColourId, juce::Colours::white);
            midiPagingBtn.onClick = [this] {
                if (onMidiPagingChanged) onMidiPagingChanged(midiPagingBtn.getToggleState());
            };
            addAndMakeVisible(midiPagingBtn);

            // ツールチップ表示 (アプリ全体設定。初期状態は showPreferences 側)。
            // 操作に慣れたら OFF にしてホバー時の説明を止められる。
            tooltipsBtn.setButtonText(
                tr(u8"ボタンの上にマウスを乗せたとき説明 (ツールチップ) を表示する"));
            tooltipsBtn.setColour(juce::ToggleButton::textColourId, juce::Colours::white);
            tooltipsBtn.onClick = [this] {
                if (onTooltipsChanged) onTooltipsChanged(tooltipsBtn.getToggleState());
            };
            addAndMakeVisible(tooltipsBtn);

            // 録音フロー
            retroBtn.setButtonText(platformShortcutText(
                tr(u8"再生中バックグラウンド録音 (遡及録音 Cmd+Shift+R で確定)")));
            retroBtn.setToggleState(curRetro, juce::dontSendNotification);
            retroBtn.setColour(juce::ToggleButton::textColourId, juce::Colours::white);
            retroBtn.onClick = [this] {
                if (onRetroChanged) onRetroChanged(retroBtn.getToggleState());
            };
            addAndMakeVisible(retroBtn);

            // 録音レイテンシ補正 (アプリ全体設定。初期状態 / 文言は showPreferences 側で設定)
            recCompBtn.setButtonText(
                tr(u8"録音をデバイスのレイテンシ分だけ自動で手前にずらす"));
            recCompBtn.setColour(juce::ToggleButton::textColourId, juce::Colours::white);
            recCompBtn.onClick = [this] {
                if (onRecCompChanged) onRecCompChanged(recCompBtn.getToggleState());
            };
            addAndMakeVisible(recCompBtn);

            setupLabel(recCompOffsetLabel, tr(u8"追加の録音補正 (ms, +で手前へ)"),
                       13.0f, juce::Colours::white);
            recCompOffsetSlider.setSliderStyle(juce::Slider::LinearBar);
            recCompOffsetSlider.setRange(-200.0, 300.0, 1.0);
            recCompOffsetSlider.setTextValueSuffix(" ms");
            recCompOffsetSlider.setColour(juce::Slider::trackColourId, juce::Colour(0xff3a5a3a));
            recCompOffsetSlider.setColour(juce::Slider::backgroundColourId, juce::Colour(0xff3a3a3a));
            recCompOffsetSlider.setColour(juce::Slider::textBoxTextColourId, juce::Colours::white);
            recCompOffsetSlider.onValueChange = [this] {
                if (onRecCompOffsetChanged) onRecCompOffsetChanged(recCompOffsetSlider.getValue());
            };
            addAndMakeVisible(recCompOffsetSlider);

            // 入力モニターに INS (インサート FX) を通すか (アプリ全体設定。初期状態は showPreferences 側)。
            // 空チェーンのトラックでは無影響。歌枠/配信で生声に EQ/Comp を掛けながら歌うための機能。
            monInsBtn.setButtonText(
                tr(u8"入力モニター中、トラックの INS (インサート FX) を返し音にも通す"));
            monInsBtn.setColour(juce::ToggleButton::textColourId, juce::Colours::white);
            monInsBtn.onClick = [this] {
                if (onMonInsChanged) onMonInsChanged(monInsBtn.getToggleState());
            };
            addAndMakeVisible(monInsBtn);

            // ディスクストリーミング (再生時の音声読み込みを先読みスレッドへ分離。アプリ全体設定)。
            // 既定 ON。多トラック/低速ディスクでの取りこぼし軽減。OFF で従来の同期読みへ戻せる。
            diskStreamBtn.setButtonText(
                tr(u8"ディスクストリーミングを使う (再生の読み込みを先読みして音切れを防ぐ)"));
            diskStreamBtn.setColour(juce::ToggleButton::textColourId, juce::Colours::white);
            diskStreamBtn.onClick = [this] {
                if (onDiskStreamChanged) onDiskStreamChanged(diskStreamBtn.getToggleState());
            };
            addAndMakeVisible(diskStreamBtn);

            // 自動保存: 無効 + 5 分刻み (5/10/15/20/25/30)
            // ID = minutes + 1 (無効=1, 5分=6, ...)
            autoSaveCombo.addItem(tr(u8"無効"),  1);
            autoSaveCombo.addItem(tr(u8"5 分"),  6);
            autoSaveCombo.addItem(tr(u8"10 分"), 11);
            autoSaveCombo.addItem(tr(u8"15 分"), 16);
            autoSaveCombo.addItem(tr(u8"20 分"), 21);
            autoSaveCombo.addItem(tr(u8"25 分"), 26);
            autoSaveCombo.addItem(tr(u8"30 分"), 31);
            auto minutesToId = [](int m) -> int {
                if (m <= 0)  return 1;
                int snapped = ((m + 2) / 5) * 5;  // 直近の 5 分刻みに丸め
                snapped = juce::jlimit(5, 30, snapped);
                return snapped + 1;
            };
            autoSaveCombo.setSelectedId(minutesToId(curAutoSaveMin), juce::dontSendNotification);
            autoSaveCombo.setColour(juce::ComboBox::backgroundColourId, juce::Colour(0xff3a3a3a));
            autoSaveCombo.setColour(juce::ComboBox::textColourId, juce::Colours::white);
            autoSaveCombo.setColour(juce::ComboBox::arrowColourId, juce::Colours::white);
            autoSaveCombo.setColour(juce::ComboBox::outlineColourId, juce::Colour(0xff555555));
            autoSaveCombo.onChange = [this] {
                int id = autoSaveCombo.getSelectedId();
                int mins = (id <= 1) ? 0 : (id - 1);
                if (onAutoSaveChanged) onAutoSaveChanged(mins);
            };
            addAndMakeVisible(autoSaveCombo);

            // バックアップ世代数: 5/10/20/30/50/100 個。ID = 個数そのもの。既定 20
            backupCountCombo.addItem(tr(u8"5 個"),   5);
            backupCountCombo.addItem(tr(u8"10 個"),  10);
            backupCountCombo.addItem(tr(u8"20 個"),  20);
            backupCountCombo.addItem(tr(u8"30 個"),  30);
            backupCountCombo.addItem(tr(u8"50 個"),  50);
            backupCountCombo.addItem(tr(u8"100 個"), 100);
            auto snapBackups = [](int n) -> int {
                const int opts[] = { 5, 10, 20, 30, 50, 100 };
                int best = 20, bestDiff = 1 << 30;
                for (int o : opts) { int d = (o > n) ? (o - n) : (n - o);
                                     if (d < bestDiff) { bestDiff = d; best = o; } }
                return best;
            };
            backupCountCombo.setSelectedId(snapBackups(curMaxBackups), juce::dontSendNotification);
            backupCountCombo.setColour(juce::ComboBox::backgroundColourId, juce::Colour(0xff3a3a3a));
            backupCountCombo.setColour(juce::ComboBox::textColourId, juce::Colours::white);
            backupCountCombo.setColour(juce::ComboBox::arrowColourId, juce::Colours::white);
            backupCountCombo.setColour(juce::ComboBox::outlineColourId, juce::Colour(0xff555555));
            backupCountCombo.onChange = [this] {
                if (onBackupCountChanged) onBackupCountChanged(backupCountCombo.getSelectedId());
            };
            addAndMakeVisible(backupCountCombo);

            // VU メータ基準レベル: -14 / -18 / -20 / -24 dBFS
            // ※ 規格名 (EBU/SMPTE 等) の括弧書きは付けない (誤解防止・数値のみ)
            // ID = abs(dB) (14, 18, 20, 24)
            vuRefCombo.addItem("-14 dBFS", 14);
            vuRefCombo.addItem("-18 dBFS", 18);
            vuRefCombo.addItem("-20 dBFS", 20);
            vuRefCombo.addItem("-24 dBFS", 24);
            auto vuRefToId = [](float dB) -> int {
                const int abs = (int) std::round(-dB);
                if (abs <= 16) return 14;
                if (abs <= 19) return 18;
                if (abs <= 22) return 20;
                return 24;
            };
            vuRefCombo.setSelectedId(vuRefToId(curVuRefDb), juce::dontSendNotification);
            vuRefCombo.setColour(juce::ComboBox::backgroundColourId, juce::Colour(0xff3a3a3a));
            vuRefCombo.setColour(juce::ComboBox::textColourId, juce::Colours::white);
            vuRefCombo.setColour(juce::ComboBox::arrowColourId, juce::Colours::white);
            vuRefCombo.setColour(juce::ComboBox::outlineColourId, juce::Colour(0xff555555));
            vuRefCombo.onChange = [this] {
                if (onVuRefChanged) onVuRefChanged(-(float) vuRefCombo.getSelectedId());
            };
            addAndMakeVisible(vuRefCombo);

            // ラウドネス自動調整ターゲット: -14 / -16 / -18 / -23 / -24 / -26 / -28 LUFS
            // ID = abs(LUFS) (14, 16, 18, 23, 24, 26, 28)
            // ※ 書き出し基準ではなくインポート時の取り込みレベル目安なので、
            //   配信規格名 (Spotify 等) の括弧書きは付けない (誤解防止)
            loudnessCombo.addItem("-14 LUFS", 14);
            loudnessCombo.addItem("-16 LUFS", 16);
            loudnessCombo.addItem("-18 LUFS", 18);
            loudnessCombo.addItem("-23 LUFS", 23);
            loudnessCombo.addItem("-24 LUFS", 24);
            loudnessCombo.addItem("-26 LUFS", 26);
            loudnessCombo.addItem("-28 LUFS", 28);
            auto loudnessToId = [](float lufs) -> int {
                const int abs = (int) std::round(-lufs);
                if (abs <= 15) return 14;
                if (abs <= 17) return 16;
                if (abs <= 20) return 18;
                if (abs <= 23) return 23;
                if (abs <= 25) return 24;
                if (abs <= 27) return 26;
                return 28;
            };
            loudnessCombo.setSelectedId(loudnessToId(curLoudnessTargetLufs), juce::dontSendNotification);
            loudnessCombo.setColour(juce::ComboBox::backgroundColourId, juce::Colour(0xff3a3a3a));
            loudnessCombo.setColour(juce::ComboBox::textColourId, juce::Colours::white);
            loudnessCombo.setColour(juce::ComboBox::arrowColourId, juce::Colours::white);
            loudnessCombo.setColour(juce::ComboBox::outlineColourId, juce::Colour(0xff555555));
            loudnessCombo.onChange = [this] {
                if (onLoudnessTargetChanged)
                    onLoudnessTargetChanged(-(float) loudnessCombo.getSelectedId());
            };
            addAndMakeVisible(loudnessCombo);

            autoNormBtn.setButtonText(
                tr(u8"インポート時にラウドネスを上記ターゲットへ自動調整"));
            autoNormBtn.setToggleState(curAutoNorm, juce::dontSendNotification);
            autoNormBtn.setColour(juce::ToggleButton::textColourId, juce::Colours::white);
            autoNormBtn.onClick = [this] {
                if (onAutoNormChanged) onAutoNormChanged(autoNormBtn.getToggleState());
            };
            addAndMakeVisible(autoNormBtn);

            peakGuardBtn.setButtonText(
                tr(u8"ピーク超過時に内部で減衰させて書き出す (クリッピング防止)"));
            peakGuardBtn.setToggleState(curPeakGuard, juce::dontSendNotification);
            peakGuardBtn.setColour(juce::ToggleButton::textColourId, juce::Colours::white);
            peakGuardBtn.onClick = [this] {
                if (onPeakGuardChanged) onPeakGuardChanged(peakGuardBtn.getToggleState());
            };
            addAndMakeVisible(peakGuardBtn);

            // MIDI 書き出しメニューの表示切替 (アプリ全体設定)。初期状態は showPreferences 側で設定する
            showMidiExportBtn.setButtonText(
                tr(u8"「MIDI を書き出す」をファイルメニューに表示"));
            showMidiExportBtn.setColour(juce::ToggleButton::textColourId, juce::Colours::white);
            showMidiExportBtn.onClick = [this] {
                if (onShowMidiExportChanged) onShowMidiExportChanged(showMidiExportBtn.getToggleState());
            };
            addAndMakeVisible(showMidiExportBtn);

            // 起動画面の広告表示切替 (広告がコンパイル時有効なビルドのみ表示。初期状態は showPreferences 側)
            if (adsUi)
            {
                showAdsBtn.setButtonText(
                    tr(u8"起動画面に広告を表示する (OFF で通信しません。次回起動で反映)"));
                showAdsBtn.setColour(juce::ToggleButton::textColourId, juce::Colours::white);
                showAdsBtn.onClick = [this] {
                    if (onShowAdsChanged) onShowAdsChanged(showAdsBtn.getToggleState());
                };
                addAndMakeVisible(showAdsBtn);
            }

            closeBtn.setButtonText(tr(u8"閉じる"));
            closeBtn.onClick = [this] {
                if (auto* dw = findParentComponentOfClass<juce::DialogWindow>())
                    dw->exitModalState(0);
            };
            addAndMakeVisible(closeBtn);

            resetBtn.setButtonText(tr(u8"デフォルトに戻す"));
            resetBtn.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff5a3a3a));
            resetBtn.setColour(juce::TextButton::textColourOffId, juce::Colours::white);
            resetBtn.onClick = [this]
            {
                juce::AlertWindow::showAsync(juce::MessageBoxOptions()
                    .withIconType(juce::MessageBoxIconType::QuestionIcon)
                    .withTitle(tr(u8"設定リセット"))
                    .withMessage(tr(u8"全ての環境設定をデフォルト値に戻します。よろしいですか？"))
                    .withButton(tr(u8"リセット"))
                    .withButton(tr(u8"キャンセル")),
                    [this](int r)
                    {
                        if (r != 1) return;
                        if (onResetDefaults) onResetDefaults();
                    });
            };
            addAndMakeVisible(resetBtn);

            // 設定項目 (resetBtn / closeBtn 以外の全子コンポーネント) をスクロール
            // コンテンツへ移す。addAndMakeVisible が親を付け替えるため、先に一覧を取る
            {
                juce::Array<juce::Component*> toMove;
                for (int i = 0; i < getNumChildComponents(); ++i)
                {
                    auto* c = getChildComponent(i);
                    if (c != &resetBtn && c != &closeBtn)
                        toMove.add(c);
                }
                for (auto* c : toMove)
                    content.addAndMakeVisible(c);
            }
            viewport.setViewedComponent(&content, /*deleteWhenRemoved*/ false);
            viewport.setScrollBarsShown(/*vertical*/ true, /*horizontal*/ false);
            addAndMakeVisible(viewport);

            setSize(480, 520);   // 従来 (940〜998) の約半分。項目はスクロールで見る
        }

        // 設定項目を幅 w で縦に並べ、コンテンツの総高さを返す (座標は content 相対)
        int layoutContent(int w)
        {
            int y = 14;
            languageLabel.setBounds(14, y, w - 28, 22); y += 26;
            languageCombo.setBounds(14, y, w - 28, 26); y += 36;
            bitsLabel.setBounds(14, y, w - 28, 22); y += 26;
            bitsCombo.setBounds(14, y, w - 28, 26); y += 32;
            stripMetaBtn.setBounds(14, y, w - 28, 24); y += 32;
            behaviorLabel.setBounds(14, y, w - 28, 22); y += 26;
            followSelBtn.setBounds(14, y, w - 28, 24); y += 28;
            rtzBtn.setBounds      (14, y, w - 28, 24); y += 28;
            zoomMouseBtn.setBounds(14, y, w - 28, 24); y += 28;
            zeroCrossBtn.setBounds(14, y, w - 28, 24); y += 28;
            midiPagingBtn.setBounds(14, y, w - 28, 24); y += 28;
            tooltipsBtn.setBounds(14, y, w - 28, 24); y += 32;
            recLabel.setBounds(14, y, w - 28, 22); y += 26;
            retroBtn.setBounds(14, y, w - 28, 24); y += 28;
            recCompBtn.setBounds(14, y, w - 28, 24); y += 26;
            recCompOffsetLabel.setBounds(14, y, 250, 24);
            recCompOffsetSlider.setBounds(270, y, w - 270 - 14, 24); y += 34;
            monInsBtn.setBounds(14, y, w - 28, 24); y += 28;
            diskStreamBtn.setBounds(14, y, w - 28, 24); y += 32;
            autoSaveLabel.setBounds(14, y, w - 28, 22); y += 26;
            autoSaveCombo.setBounds(14, y, w - 28, 26); y += 32;
            backupCountLabel.setBounds(14, y, w - 28, 22); y += 26;
            backupCountCombo.setBounds(14, y, w - 28, 26); y += 32;
            vuRefLabel.setBounds(14, y, w - 28, 22); y += 26;
            vuRefCombo.setBounds(14, y, w - 28, 26); y += 32;
            loudnessLabel.setBounds(14, y, w - 28, 22); y += 26;
            loudnessCombo.setBounds(14, y, w - 28, 26); y += 28;
            autoNormBtn.setBounds(14, y, w - 28, 24); y += 32;
            exportLabel.setBounds(14, y, w - 28, 22); y += 26;
            peakGuardBtn.setBounds(14, y, w - 28, 24); y += 28;
            showMidiExportBtn.setBounds(14, y, w - 28, 24); y += 32;
            if (adsUi)
            {
                startupLabel.setBounds(14, y, w - 28, 22); y += 26;
                showAdsBtn.setBounds(14, y, w - 28, 24); y += 32;
            }
            return y + 6;
        }

        enum { kFooterH = 46 };   // 下部の固定ボタン帯 (リセット / 閉じる)。ローカルクラスのため enum 定数

        void resized() override
        {
            viewport.setBounds(0, 0, getWidth(), getHeight() - kFooterH);
            // スクロールバー分を引いた幅でコンテンツを敷く (横スクロールを出さない)
            const int cw = getWidth() - viewport.getScrollBarThickness();
            content.setSize(cw, layoutContent(cw));
            resetBtn.setBounds(14, getHeight() - 34, 140, 26);
            closeBtn.setBounds(getWidth() - 100 - 14, getHeight() - 34, 100, 26);
        }

        // 全 UI コントロールの表示を curXxx の値に同期させる (Reset 用)
        void syncUiToValues(int curBits, bool curFollowSel, bool curRetro, bool curRtz,
                            int curAutoSaveMin, int curMaxBackups, float curVuRefDb, float curLoudnessLufs,
                            bool curAutoNorm, bool curZoomMouse, bool curPeakGuard, bool curZeroCross,
                            bool curStripMeta)
        {
            bitsCombo.setSelectedId(curBits == 24 ? 24 : 32, juce::dontSendNotification);
            followSelBtn.setToggleState(curFollowSel, juce::dontSendNotification);
            retroBtn   .setToggleState(curRetro, juce::dontSendNotification);
            rtzBtn     .setToggleState(curRtz,   juce::dontSendNotification);
            zoomMouseBtn.setToggleState(curZoomMouse, juce::dontSendNotification);
            zeroCrossBtn.setToggleState(curZeroCross, juce::dontSendNotification);
            stripMetaBtn.setToggleState(curStripMeta, juce::dontSendNotification);
            autoNormBtn.setToggleState(curAutoNorm, juce::dontSendNotification);
            peakGuardBtn.setToggleState(curPeakGuard, juce::dontSendNotification);

            // 自動保存: 5/10/15/20/25/30 分 もしくは 無効
            int asId = (curAutoSaveMin <= 0) ? 1
                       : juce::jlimit(5, 30, ((curAutoSaveMin + 2) / 5) * 5) + 1;
            autoSaveCombo.setSelectedId(asId, juce::dontSendNotification);

            // バックアップ世代数: 5/10/20/30/50/100 の最も近い値に合わせる
            {
                const int opts[] = { 5, 10, 20, 30, 50, 100 };
                int best = 20, bestDiff = 1 << 30;
                for (int o : opts) { int d = (o > curMaxBackups) ? (o - curMaxBackups) : (curMaxBackups - o);
                                     if (d < bestDiff) { bestDiff = d; best = o; } }
                backupCountCombo.setSelectedId(best, juce::dontSendNotification);
            }

            // VU ref: -14/-18/-20/-24
            int vuId = 18;
            {
                const int a = (int) std::round(-curVuRefDb);
                if      (a <= 16) vuId = 14;
                else if (a <= 19) vuId = 18;
                else if (a <= 22) vuId = 20;
                else              vuId = 24;
            }
            vuRefCombo.setSelectedId(vuId, juce::dontSendNotification);

            // Loudness: -14/-16/-18/-23/-24
            int lufsId = 24;
            {
                const int a = (int) std::round(-curLoudnessLufs);
                if      (a <= 15) lufsId = 14;
                else if (a <= 17) lufsId = 16;
                else if (a <= 20) lufsId = 18;
                else if (a <= 23) lufsId = 23;
                else              lufsId = 24;
            }
            loudnessCombo.setSelectedId(lufsId, juce::dontSendNotification);
        }
        void paint(juce::Graphics& g) override
        {
            g.fillAll(juce::Colour(0xff2a2a2a));
            // スクロール領域と固定ボタン帯の境界線
            g.setColour(juce::Colour(0xff444444));
            g.drawHorizontalLine(getHeight() - kFooterH, 0.0f, (float) getWidth());
        }
    };

    auto* dlg = new PrefsDlg(appSettings.resampleOutputBits,
                              appSettings.playheadFollowsSelection,
                              appSettings.retrospectiveEnabled,
                              appSettings.returnToStartOnStop,
                              appSettings.autoSaveIntervalMinutes,
                              appSettings.maxBackups,
                              appSettings.vuReferenceLevel,
                              appSettings.loudnessTargetLufs,
                              appSettings.autoNormalizeOnImport,
                              appSettings.zoomToMousePosition,
                              appSettings.exportPeakGuard,
                              appSettings.zeroCrossingFade,
                              appSettings.stripImportedMetadata);
    dlg->onLanguageChanged = [](int id) {
        Localisation::Language lang = Localisation::Language::Japanese;
        switch (id)
        {
            case 2: lang = Localisation::Language::English;            break;
            case 3: lang = Localisation::Language::SimplifiedChinese;  break;
            case 4: lang = Localisation::Language::TraditionalChinese; break;
            case 5: lang = Localisation::Language::Korean;             break;
            default: lang = Localisation::Language::Japanese;          break;
        }
        Localisation::saveLanguage(lang);  // アプリ全体設定 (プロジェクトではない)
        juce::AlertWindow::showAsync(juce::MessageBoxOptions()
            .withIconType(juce::MessageBoxIconType::InfoIcon)
            .withTitle(tr(u8"言語設定"))
            .withMessage(tr(u8"言語の変更は次回起動時に反映されます。"))
            .withButton("OK"), nullptr);
    };
    dlg->onBitsChanged = [this](int bits) {
        appSettings.resampleOutputBits = (bits == 24) ? 24 : 32;
        markProjectDirty();
    };
    dlg->onFollowSelChanged = [this](bool v) {
        appSettings.playheadFollowsSelection = v;
        timelineView.setAppSettings(appSettings);
        markProjectDirty();
    };
    dlg->onRetroChanged = [this](bool v) {
        appSettings.retrospectiveEnabled = v;
        markProjectDirty();
    };
    dlg->onRtzChanged = [this](bool v) {
        appSettings.returnToStartOnStop = v;
        markProjectDirty();
    };
    dlg->onAutoSaveChanged = [this](int mins) {
        appSettings.autoSaveIntervalMinutes = mins;
        restartAutoSaveTimer();
        markProjectDirty();
    };
    dlg->onBackupCountChanged = [this](int n) {
        appSettings.maxBackups = juce::jmax(1, n);
        markProjectDirty();
    };
    dlg->onVuRefChanged = [this](float dB) {
        appSettings.vuReferenceLevel = dB;
        masterPanel.setVuReferenceLevel(dB);
        trackHeaderPanel.setVuReferenceLevel(dB);
        markProjectDirty();
    };
    dlg->onLoudnessTargetChanged = [this](float lufs) {
        appSettings.loudnessTargetLufs = lufs;
        trackHeaderPanel.setLoudnessTargetLufs(lufs);
        // TimelineView は appSettings から直接読むので setAppSettings を呼ぶだけで反映
        timelineView.setAppSettings(appSettings);
        markProjectDirty();
    };
    dlg->onAutoNormChanged = [this](bool v) {
        appSettings.autoNormalizeOnImport = v;
        markProjectDirty();
    };
    dlg->onZoomMouseChanged = [this](bool v) {
        appSettings.zoomToMousePosition = v;
        timelineView.setAppSettings(appSettings);
        markProjectDirty();
    };
    dlg->onPeakGuardChanged = [this](bool v) {
        appSettings.exportPeakGuard = v;
        markProjectDirty();
    };
    dlg->onZeroCrossChanged = [this](bool v) {
        appSettings.zeroCrossingFade = v;
        timelineView.setAppSettings(appSettings);  // 手動クロスフェード (Xキー) が参照する
        markProjectDirty();
    };
    dlg->onStripMetaChanged = [this](bool v) {
        appSettings.stripImportedMetadata = v;
        markProjectDirty();
    };
    // MIDI 書き出しメニューの表示切替はアプリ全体設定 (プロジェクトではない)。
    // 即時に保存し、メニューを再構築して反映する (markProjectDirty は呼ばない)。
    dlg->showMidiExportBtn.setToggleState(appPrefs.showMidiExportMenu, juce::dontSendNotification);
    dlg->onShowMidiExportChanged = [this](bool v) {
        appPrefs.showMidiExportMenu = v;
        appPrefs.save();
        menuItemsChanged();   // ファイルメニューを再構築 (項目の表示/非表示を即反映)
    };
    // MIDI ピアノロールの自動ページング (アプリ全体設定)。即時保存し、開いている
    // ピアノロールへ即反映する (次に開く窓は openPianoRollFor で反映される)。
    dlg->midiPagingBtn.setToggleState(appPrefs.midiPagingEnabled, juce::dontSendNotification);
    dlg->onMidiPagingChanged = [this](bool v) {
        appPrefs.midiPagingEnabled = v;
        appPrefs.save();
        applyMidiPagingToOpenEditors();
    };
    // ツールチップ表示 (アプリ全体設定)。即時保存し、TooltipWindow を生成/破棄して即反映。
    dlg->tooltipsBtn.setToggleState(appPrefs.showTooltips, juce::dontSendNotification);
    dlg->onTooltipsChanged = [this](bool v) {
        appPrefs.showTooltips = v;
        appPrefs.save();
        applyTooltipVisibility();
    };
    // 起動画面の広告表示 (アプリ全体設定)。広告がコンパイル時有効なビルドのみ。即時保存。反映は次回起動画面表示時
    if (AppPreferences::adsCompiledIn())
    {
        dlg->showAdsBtn.setToggleState(appPrefs.showAds, juce::dontSendNotification);
        dlg->onShowAdsChanged = [this](bool v) {
            appPrefs.showAds = v;
            appPrefs.save();
        };
    }
    // 録音レイテンシ補正 (アプリ全体設定。ハードウェア依存のためプロジェクト設定ではない)。
    // 即時保存 + エンジンへ即反映 (次の録音開始から効く)
    {
        const double devMs = audioEngine.getDeviceRoundTripLatencySecs() * 1000.0;
        dlg->recCompBtn.setButtonText(
            tr(u8"録音をデバイスのレイテンシ分だけ自動で手前にずらす")
            + juce::String::formatted(" (%.1f ms)", devMs));
        dlg->recCompBtn.setToggleState(appPrefs.recLatencyAutoComp, juce::dontSendNotification);
        dlg->recCompOffsetSlider.setValue(appPrefs.recLatencyManualMs,
                                          juce::dontSendNotification);
        dlg->onRecCompChanged = [this](bool v) {
            appPrefs.recLatencyAutoComp = v;
            appPrefs.save();
            audioEngine.setRecordingLatencyComp(appPrefs.recLatencyAutoComp,
                                                appPrefs.recLatencyManualMs);
        };
        dlg->onRecCompOffsetChanged = [this](double ms) {
            appPrefs.recLatencyManualMs = ms;
            appPrefs.save();
            audioEngine.setRecordingLatencyComp(appPrefs.recLatencyAutoComp,
                                                appPrefs.recLatencyManualMs);
        };
    }
    // 入力モニターに INS を通す (アプリ全体設定)。即時保存 + モニタ状態を再同期して即反映。
    dlg->monInsBtn.setToggleState(appPrefs.monitorThroughInserts, juce::dontSendNotification);
    dlg->onMonInsChanged = [this](bool v) {
        appPrefs.monitorThroughInserts = v;
        appPrefs.save();
        syncInputMonitorStateToEngine();   // 返しのチェーン経路 (setMonitorChain) を即切替
    };
    // ディスクストリーミング (アプリ全体設定)。即時保存 + エンジンへ即反映 (次ブロックから従う)。
    dlg->diskStreamBtn.setToggleState(appPrefs.diskStreaming, juce::dontSendNotification);
    dlg->onDiskStreamChanged = [this](bool v) {
        appPrefs.diskStreaming = v;
        appPrefs.save();
        audioEngine.setDiskStreamingEnabled(v);
    };
    dlg->onResetDefaults = [this, dlg]
    {
        // AppSettings の各フィールドをデフォルト値 (構造体の初期化子) に揃える
        const AppSettings def;
        appSettings.resampleOutputBits       = def.resampleOutputBits;
        appSettings.playheadFollowsSelection = def.playheadFollowsSelection;
        appSettings.retrospectiveEnabled     = def.retrospectiveEnabled;
        appSettings.returnToStartOnStop      = def.returnToStartOnStop;
        appSettings.zoomToMousePosition      = def.zoomToMousePosition;
        appSettings.autoSaveIntervalMinutes  = def.autoSaveIntervalMinutes;
        appSettings.maxBackups               = def.maxBackups;
        appSettings.vuReferenceLevel         = def.vuReferenceLevel;
        appSettings.loudnessTargetLufs       = def.loudnessTargetLufs;
        appSettings.autoNormalizeOnImport    = def.autoNormalizeOnImport;
        appSettings.exportPeakGuard          = def.exportPeakGuard;
        appSettings.zeroCrossingFade         = def.zeroCrossingFade;
        appSettings.stripImportedMetadata    = def.stripImportedMetadata;

        // ランタイム反映
        timelineView.setAppSettings(appSettings);
        masterPanel.setVuReferenceLevel(appSettings.vuReferenceLevel);
        trackHeaderPanel.setVuReferenceLevel(appSettings.vuReferenceLevel);
        trackHeaderPanel.setLoudnessTargetLufs(appSettings.loudnessTargetLufs);
        restartAutoSaveTimer();

        // アプリ全体設定 (MIDI 書き出しメニュー / 広告表示 / 録音レイテンシ補正) も既定に戻す
        const AppPreferences defPrefs;
        appPrefs.showMidiExportMenu = defPrefs.showMidiExportMenu;
        appPrefs.showAds            = defPrefs.showAds;
        appPrefs.midiPagingEnabled  = defPrefs.midiPagingEnabled;
        appPrefs.recLatencyAutoComp = defPrefs.recLatencyAutoComp;
        appPrefs.recLatencyManualMs = defPrefs.recLatencyManualMs;
        appPrefs.monitorThroughInserts = defPrefs.monitorThroughInserts;
        appPrefs.diskStreaming      = defPrefs.diskStreaming;
        appPrefs.save();
        menuItemsChanged();
        applyMidiPagingToOpenEditors();
        audioEngine.setRecordingLatencyComp(appPrefs.recLatencyAutoComp,
                                            appPrefs.recLatencyManualMs);
        audioEngine.setDiskStreamingEnabled(appPrefs.diskStreaming);
        syncInputMonitorStateToEngine();   // モニタ FX 経路を既定 (ON) に戻す
        dlg->showMidiExportBtn.setToggleState(appPrefs.showMidiExportMenu, juce::dontSendNotification);
        dlg->midiPagingBtn.setToggleState(appPrefs.midiPagingEnabled, juce::dontSendNotification);
        if (AppPreferences::adsCompiledIn())
            dlg->showAdsBtn.setToggleState(appPrefs.showAds, juce::dontSendNotification);
        dlg->recCompBtn.setToggleState(appPrefs.recLatencyAutoComp, juce::dontSendNotification);
        dlg->recCompOffsetSlider.setValue(appPrefs.recLatencyManualMs, juce::dontSendNotification);
        dlg->monInsBtn.setToggleState(appPrefs.monitorThroughInserts, juce::dontSendNotification);
        dlg->diskStreamBtn.setToggleState(appPrefs.diskStreaming, juce::dontSendNotification);

        // ダイアログの UI を新しい値に同期
        dlg->syncUiToValues(appSettings.resampleOutputBits,
                            appSettings.playheadFollowsSelection,
                            appSettings.retrospectiveEnabled,
                            appSettings.returnToStartOnStop,
                            appSettings.autoSaveIntervalMinutes,
                            appSettings.maxBackups,
                            appSettings.vuReferenceLevel,
                            appSettings.loudnessTargetLufs,
                            appSettings.autoNormalizeOnImport,
                            appSettings.zoomToMousePosition,
                            appSettings.exportPeakGuard,
                            appSettings.zeroCrossingFade,
                            appSettings.stripImportedMetadata);
        markProjectDirty();
    };

    juce::DialogWindow::LaunchOptions opts;
    opts.content.setOwned(dlg);
    opts.dialogTitle = tr(u8"環境設定");
    opts.dialogBackgroundColour = juce::Colour(0xff2a2a2a);
    opts.escapeKeyTriggersCloseButton = true;
    opts.useNativeTitleBar = true;
    opts.resizable = false;
    opts.launchAsync();
}
