// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

// MainComponent の環境設定ダイアログ実装 (showPreferences)。
// MainComponent_Dialogs.cpp から分割。

#include "MainComponent.h"
#include "Localisation.h"
#include "Audio/AudioDeviceSettings.h"
#include "Audio/StreamMirrorOutput.h"
#include "Export/ExportEngine.h"
#include "Export/ExportDialog.h"
#include "MIDI/MidiImporter.h"
#include "MIDI/MidiImportDialog.h"

void MainComponent::showPreferences()
{
    class PrefsDlg : public juce::Component
    {
    public:
        juce::Label    languageLabel, uiScaleLabel, bitsLabel, behaviorLabel, recLabel, autoSaveLabel,
                       backupCountLabel, vuRefLabel, loudnessLabel;
        juce::ComboBox languageCombo, uiScaleCombo, bitsCombo, autoSaveCombo, backupCountCombo, vuRefCombo, loudnessCombo;
        std::function<void(double)> onUiScaleChanged;   // 画面の表示倍率 (アプリ全体設定。初期状態は showPreferences 側)
        std::function<void(int)>   onLanguageChanged;   // 1=日本語, 2=English
        juce::ToggleButton followSelBtn, rtzBtn, autoNormBtn, zoomMouseBtn, peakGuardBtn, zeroCrossBtn, stripMetaBtn;
        juce::ToggleButton showMidiExportBtn;   // 初期状態 / コールバックは showPreferences 側で設定 (アプリ全体設定)
        juce::ToggleButton exportDoneDlgBtn;    // 書き出し完了ダイアログ表示 (アプリ全体設定。初期状態は showPreferences 側)
        juce::ToggleButton midiPagingBtn;       // MIDI ピアノロールの自動ページング (アプリ全体設定。初期状態は showPreferences 側)
        juce::ToggleButton tooltipsBtn;         // ツールチップ表示 (アプリ全体設定。初期状態は showPreferences 側)
        juce::ToggleButton folderTracksBtn;     // フォルダトラック追加を有効化 (アプリ全体設定。初期状態は showPreferences 側)
        juce::ToggleButton folderExtrasBtn;     // フォルダに Pan/Rev/INS を表示 (アプリ全体設定。初期状態は showPreferences 側)
        juce::ToggleButton showAdsBtn;          // 起動画面の広告表示 (アプリ全体設定。初期状態は showPreferences 側で設定)
        juce::ToggleButton recCompBtn;          // 録音レイテンシ自動補正 (アプリ全体設定。初期状態は showPreferences 側)
        juce::ToggleButton retakeKeepBtn;       // Q リテイクでテイクを残す (アプリ全体設定。初期状態は showPreferences 側)
        juce::ToggleButton monInsBtn;           // 入力モニターに INS を通す (アプリ全体設定。初期状態は showPreferences 側)
        juce::ToggleButton diskStreamBtn;       // ディスクストリーミング (アプリ全体設定。初期状態は showPreferences 側)
        juce::ToggleButton multicoreBtn;        // オーディオのマルチコア処理 (アプリ全体設定。初期状態は showPreferences 側)
        juce::ToggleButton mirrorBtn;           // 配信ミラー出力 (アプリ全体設定。初期状態は showPreferences 側)
        juce::Label        mirrorDevLabel;
        juce::ComboBox     mirrorDevCombo;      // ミラーの出力先デバイス (1 = OS 既定、100+ = 実デバイス名)
        juce::StringArray  mirrorDevNames;      // combo の 100+i に対応するデバイス名
        juce::ToggleButton midiInBtn;           // MIDI キーボードを使用する (アプリ全体設定。初期状態は showPreferences 側)
        juce::Label        midiInDevLabel;
        juce::ComboBox     midiInDevCombo;      // MIDI 入力デバイス (100+i = 実デバイス名)
        juce::StringArray  midiInDevNames;      // combo の 100+i に対応するデバイス名
        // 「配信」見出しの隣に出す、同梱ヘルプの配信セクション (#streaming) へのリンク。
        // URL は設定しない (HyperlinkButton の URL 起動は file:// のフラグメントを保持できない
        // ため見た目だけ使い、遷移は onClick → MainComponent::openBundledHelp("streaming"))
        juce::HyperlinkButton streamGuideLink;
        juce::Label        recCompOffsetLabel;
        // ダブルクリックで数値を直接入力できるスライダー (追加の手動オフセット ms 用)
        struct TypeInSlider : juce::Slider
        {
            void mouseDoubleClick (const juce::MouseEvent&) override { showTextBox(); }
        };
        TypeInSlider       recCompOffsetSlider;
        // アプリケーショントラックの追加を有効化 (Windows のみ表示。初期状態は showPreferences 側)
        juce::ToggleButton appTracksBtn;
        juce::Label        exportLabel, startupLabel, streamLabel, midiInLabel;
        const bool         adsUi { AppPreferences::adsCompiledIn() };  // 広告がコンパイル時有効な時だけ UI を出す
        const bool         appCapUi { AppAudioCapture::isSupported() }; // アプリケーショントラックは対応環境 (Windows) だけ UI を出す
        juce::TextButton closeBtn, resetBtn;
        // 設定項目は縦に長いので Viewport でスクロールさせる (ウィンドウは従来の約半分の高さ)。
        // resetBtn / closeBtn だけはダイアログ下部に固定し、スクロールしなくても押せるようにする
        juce::Viewport  viewport;
        juce::Component content;
        std::function<void(int)>   onBitsChanged;
        std::function<void(bool)>  onFollowSelChanged;
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
        std::function<void(bool)>  onExportDoneDlgChanged;
        std::function<void(bool)>  onMidiPagingChanged;
        std::function<void(bool)>  onTooltipsChanged;
        std::function<void(bool)>  onFolderTracksChanged;
        std::function<void(bool)>  onFolderExtrasChanged;
        std::function<void(bool)>  onShowAdsChanged;
        std::function<void(bool)>  onRecCompChanged;
        std::function<void(double)> onRecCompOffsetChanged;
        std::function<void(bool)>  onRetakeKeepChanged;
        std::function<void(bool)>  onMonInsChanged;
        std::function<void(bool)>  onDiskStreamChanged;
        std::function<void(bool)>  onMulticoreChanged;
        std::function<void(bool)>          onMirrorChanged;
        std::function<void(juce::String)>  onMirrorDeviceChanged;   // 空 = OS 既定
        std::function<void(bool)>          onAppTracksChanged;
        std::function<void(bool)>          onMidiInChanged;
        std::function<void(juce::String)>  onMidiInDeviceChanged;   // 空 = 未選択
        std::function<void()>      onResetDefaults;

        PrefsDlg(int curBits, bool curFollowSel, bool curRtz,
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

            // 画面全体の表示倍率 (アプリ全体設定)。高解像度/大画面で小さく見えるときに拡大する。
            // ID = パーセント (100/110/125/150/175/200)。初期選択は showPreferences 側で設定。
            setupLabel(uiScaleLabel, tr(u8"画面の表示倍率 (文字やボタンが小さすぎる/大きすぎる時に調整)"),
                       13.0f, juce::Colours::white);
            uiScaleCombo.addItem("80%", 80);
            uiScaleCombo.addItem("90%", 90);
            uiScaleCombo.addItem(tr(u8"100% (等倍)"), 100);
            uiScaleCombo.addItem("110%", 110);
            uiScaleCombo.addItem("125%", 125);
            uiScaleCombo.addItem("150%", 150);
            uiScaleCombo.addItem("175%", 175);
            uiScaleCombo.addItem("200%", 200);
            uiScaleCombo.setColour(juce::ComboBox::backgroundColourId, juce::Colour(0xff3a3a3a));
            uiScaleCombo.setColour(juce::ComboBox::textColourId, juce::Colours::white);
            uiScaleCombo.setColour(juce::ComboBox::arrowColourId, juce::Colours::white);
            uiScaleCombo.setColour(juce::ComboBox::outlineColourId, juce::Colour(0xff555555));
            uiScaleCombo.onChange = [this] {
                if (onUiScaleChanged) onUiScaleChanged(uiScaleCombo.getSelectedId() / 100.0);
            };
            addAndMakeVisible(uiScaleCombo);

            setupLabel(bitsLabel, tr(u8"インポート時のリサンプル出力"), 13.0f, juce::Colours::white);
            setupLabel(behaviorLabel, tr(u8"編集動作"), 13.0f, juce::Colours::white);
            setupLabel(recLabel,      tr(u8"録音フロー"), 13.0f, juce::Colours::white);
            setupLabel(autoSaveLabel, tr(u8"自動保存"), 13.0f, juce::Colours::white);
            setupLabel(backupCountLabel, tr(u8"バックアップを残す数 (古い世代から自動削除)"), 13.0f, juce::Colours::white);
            setupLabel(vuRefLabel,    tr(u8"VU メータ基準レベル (0 VU)"), 13.0f, juce::Colours::white);
            setupLabel(loudnessLabel, tr(u8"ラウドネス自動調整ターゲット"), 13.0f, juce::Colours::white);
            setupLabel(exportLabel,   tr(u8"書き出し"), 13.0f, juce::Colours::white);
            setupLabel(streamLabel,   tr(u8"配信"), 13.0f, juce::Colours::white);
            // 配信ソフト連携の手順書リンク (同梱ヘルプの #streaming)。URL は showPreferences 側で設定
            streamGuideLink.setButtonText(tr(u8"(配信ソフト連携の手順を見る)"));
            streamGuideLink.setFont(juce::FontOptions(13.0f), false, juce::Justification::centredLeft);
            streamGuideLink.setColour(juce::HyperlinkButton::textColourId, juce::Colour(0xff7ab8e8));
            addAndMakeVisible(streamGuideLink);
            streamGuideLink.setVisible(false);   // URL が解決できた時だけ表示 (showPreferences 側)
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

            // フォルダトラック (アプリ全体設定。初期状態は showPreferences 側)。
            // ON にすると「+ トラック追加」メニューに「フォルダトラックを追加」が出る。
            folderTracksBtn.setButtonText(
                tr(u8"フォルダトラックを追加できるようにする"));
            folderTracksBtn.setColour(juce::ToggleButton::textColourId, juce::Colours::white);
            folderTracksBtn.onClick = [this] {
                if (onFolderTracksChanged) onFolderTracksChanged(folderTracksBtn.getToggleState());
            };
            addAndMakeVisible(folderTracksBtn);

            // フォルダトラックの Pan/Rev 表示 (表示のみの設定。値・効果は非表示でも生きる。
            // INS スロットは他トラックとレイアウトを揃えるため常に表示 = 設定対象外)
            folderExtrasBtn.setButtonText(
                tr(u8"Pan・Rev をフォルダトラックに表示する"));
            folderExtrasBtn.setColour(juce::ToggleButton::textColourId, juce::Colours::white);
            folderExtrasBtn.onClick = [this] {
                if (onFolderExtrasChanged) onFolderExtrasChanged(folderExtrasBtn.getToggleState());
            };
            addAndMakeVisible(folderExtrasBtn);

            // 録音フロー
            // 「再生中バックグラウンド録音 (遡及録音)」のトグルは UI から撤去 (初心者が迷うため・
            //  常時 ON 運用)。機能と Cmd+Shift+R は残しているので、要望があれば retroBtn を復活し
            //  appSettings.retrospectiveEnabled に再配線するだけで戻せる。

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
            recCompOffsetSlider.setRange(-300.0, 300.0, 1.0);
            recCompOffsetSlider.setTextValueSuffix(" ms");
            // 設定ページのスクロール中にホイールが値を変えてしまう誤操作を防ぐ
            // (無効化するとホイールは素通りして Viewport のスクロールに使われる)
            recCompOffsetSlider.setScrollWheelEnabled(false);
            recCompOffsetSlider.setTooltip(tr(u8"ダブルクリックで数値を入力"));
            recCompOffsetSlider.setColour(juce::Slider::trackColourId, juce::Colour(0xff3a5a3a));
            recCompOffsetSlider.setColour(juce::Slider::backgroundColourId, juce::Colour(0xff3a3a3a));
            recCompOffsetSlider.setColour(juce::Slider::textBoxTextColourId, juce::Colours::white);
            recCompOffsetSlider.onValueChange = [this] {
                if (onRecCompOffsetChanged) onRecCompOffsetChanged(recCompOffsetSlider.getValue());
            };
            addAndMakeVisible(recCompOffsetSlider);

            // Q (リテイク) で録り直す前のテイクを残すか (アプリ全体設定。初期状態は showPreferences 側)。
            // 既定 OFF = 完全破棄。ON でテイクレーンへ確定してから録り直す。
            retakeKeepBtn.setButtonText(
                tr(u8"Q (リテイク) で録り直す前のテイクをテイクリストに残す"));
            retakeKeepBtn.setColour(juce::ToggleButton::textColourId, juce::Colours::white);
            retakeKeepBtn.onClick = [this] {
                if (onRetakeKeepChanged) onRetakeKeepChanged(retakeKeepBtn.getToggleState());
            };
            addAndMakeVisible(retakeKeepBtn);

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

            // オーディオのマルチコア処理 (再生時のトラック描画を複数コアへ分散。アプリ全体設定)。
            // 既定 ON。多トラック/重いプラグインでスケール。OFF で単一スレッドへ (互換重視)。
            multicoreBtn.setButtonText(
                tr(u8"オーディオをマルチコアで処理する (多トラック/重いプラグインで軽くなる)"));
            multicoreBtn.setColour(juce::ToggleButton::textColourId, juce::Colours::white);
            multicoreBtn.onClick = [this] {
                if (onMulticoreChanged) onMulticoreChanged(multicoreBtn.getToggleState());
            };
            addAndMakeVisible(multicoreBtn);

            // 配信ミラー出力 (アプリ全体設定。初期状態は showPreferences 側)。
            // 最終ミックスを別の出力デバイスへ同時に流し、配信ソフトのアプリ音声キャプチャで
            // 拾えるようにする (メイン出力が ASIO 等でキャプチャできない環境向け)。
            mirrorBtn.setButtonText(
                tr(u8"配信ミラー出力を使う (いま聞こえている音を別の出力デバイスへも同時に流す)"));
            mirrorBtn.setColour(juce::ToggleButton::textColourId, juce::Colours::white);
            mirrorBtn.setTooltip(
                tr(u8"配信ソフトが音を拾えない環境 (ASIO 等) 向け。ミラー側の音は配信ソフトのアプリ音声キャプチャや仮想デバイス経由で拾えます。録音や書き出しには影響しません。"));
            mirrorBtn.onClick = [this] {
                if (onMirrorChanged) onMirrorChanged(mirrorBtn.getToggleState());
            };
            addAndMakeVisible(mirrorBtn);

            setupLabel(mirrorDevLabel,
                       tr(u8"ミラーの出力先デバイス (メインと同じデバイスを選ぶと二重に聞こえます)"),
                       13.0f, juce::Colours::white);
            // 候補の投入は showPreferences 側 (メイン出力デバイスの除外に audioEngine が要るため)。
            // 「既定の出力デバイス」の選択肢は廃止 (2026-07): 既定 = メインと同じ I/O の環境が
            // 大半で、ON にした瞬間に二重聞こえ事故になる導線だった。未選択の間は開始しない
            mirrorDevCombo.setTextWhenNothingSelected(tr(u8"デバイスを選択…"));
            mirrorDevCombo.setColour(juce::ComboBox::backgroundColourId, juce::Colour(0xff3a3a3a));
            mirrorDevCombo.setColour(juce::ComboBox::textColourId, juce::Colours::white);
            mirrorDevCombo.setColour(juce::ComboBox::arrowColourId, juce::Colours::white);
            mirrorDevCombo.setColour(juce::ComboBox::outlineColourId, juce::Colour(0xff555555));
            mirrorDevCombo.onChange = [this] {
                if (onMirrorDeviceChanged)
                {
                    const int id = mirrorDevCombo.getSelectedId();
                    onMirrorDeviceChanged(id >= 100 ? mirrorDevNames[id - 100] : juce::String());
                }
            };
            addAndMakeVisible(mirrorDevCombo);

            // アプリケーショントラック (Windows のみ・アプリ全体設定。初期状態は showPreferences 側)。
            // ON にすると「+ トラック追加」メニューに「アプリケーショントラックを追加」が出る。
            // 指定アプリ (ブラウザ / カラオケアプリ) の音をトラックとして混ぜられる (仮想ケーブル
            // 不要・録音/書き出しには乗らない)。OFF でも既存のアプリケーショントラックは動く
            // (追加導線が消えるだけ = enableFolderTracks と同じ思想)
            if (appCapUi)
            {
                appTracksBtn.setButtonText(
                    tr(u8"アプリケーショントラックを追加できるようにする (ブラウザ等の音をトラックとして混ぜる)"));
                appTracksBtn.setColour(juce::ToggleButton::textColourId, juce::Colours::white);
                appTracksBtn.setTooltip(
                    tr(u8"選んだアプリ (ブラウザやカラオケアプリ) の音をトラックとして取り込み、いま聞こえている音と配信に混ぜます。仮想ケーブルは不要です。録音や書き出しには入りません。"));
                appTracksBtn.onClick = [this] {
                    if (onAppTracksChanged) onAppTracksChanged(appTracksBtn.getToggleState());
                };
                addAndMakeVisible(appTracksBtn);
            }

            // MIDI 入力 (アプリ全体設定。初期状態と候補投入は showPreferences 側)。
            // 接続した MIDI キーボードで選択中の MIDI トラックの音源を弾けるようにする。
            // チャンネル指定は初心者に厳しいため設けない (全チャンネル受信)
            setupLabel(midiInLabel, tr(u8"MIDI入力"), 13.0f, juce::Colours::white);
            midiInBtn.setButtonText(
                tr(u8"MIDIキーボードを使用する (弾いた音を選択中の MIDI トラックで鳴らす)"));
            midiInBtn.setColour(juce::ToggleButton::textColourId, juce::Colours::white);
            midiInBtn.setTooltip(
                tr(u8"接続した MIDI キーボードで、選択中の MIDI トラックの音源 (内蔵シンセ / INS のプラグイン音源) をそのまま演奏できます。MIDI チャンネルは全て受信します。ピアノロールのステップ入力にも使えます。"));
            midiInBtn.onClick = [this] {
                if (onMidiInChanged) onMidiInChanged(midiInBtn.getToggleState());
            };
            addAndMakeVisible(midiInBtn);

            setupLabel(midiInDevLabel, tr(u8"MIDIキーボードのデバイス"),
                       13.0f, juce::Colours::white);
            midiInDevCombo.setTextWhenNothingSelected(tr(u8"デバイスを選択…"));
            midiInDevCombo.setColour(juce::ComboBox::backgroundColourId, juce::Colour(0xff3a3a3a));
            midiInDevCombo.setColour(juce::ComboBox::textColourId, juce::Colours::white);
            midiInDevCombo.setColour(juce::ComboBox::arrowColourId, juce::Colours::white);
            midiInDevCombo.setColour(juce::ComboBox::outlineColourId, juce::Colour(0xff555555));
            midiInDevCombo.onChange = [this] {
                if (onMidiInDeviceChanged)
                {
                    const int id = midiInDevCombo.getSelectedId();
                    onMidiInDeviceChanged(id >= 100 ? midiInDevNames[id - 100] : juce::String());
                }
            };
            addAndMakeVisible(midiInDevCombo);

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

            // 書き出し完了ダイアログの表示切替 (アプリ全体設定・2 ミックス / stems 共用)。初期状態は showPreferences 側
            exportDoneDlgBtn.setButtonText(tr(u8"書き出し完了後にダイアログを表示する"));
            exportDoneDlgBtn.setColour(juce::ToggleButton::textColourId, juce::Colours::white);
            exportDoneDlgBtn.onClick = [this] {
                if (onExportDoneDlgChanged) onExportDoneDlgChanged(exportDoneDlgBtn.getToggleState());
            };
            addAndMakeVisible(exportDoneDlgBtn);

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

            setSize(520, 560);   // 少し広め+高めにして余白を確保。項目はスクロールで見る
        }

        // 設定項目を幅 w で縦に並べ、コンテンツの総高さを返す (座標は content 相対)。
        // 窮屈にならないよう、行間・セクション間・左右マージンを広めに取る。
        int layoutContent(int w)
        {
            const int mx   = 22;          // 左右マージン (旧 14)
            const int cw   = w - mx * 2;  // コントロール幅
            const int gRow = 12;          // チェック行どうしの余白 (旧 4)
            const int gLbl = 7;           // ラベルと直下コントロールの余白
            const int gSec = 22;          // セクション見出しの前に空ける余白

            auto label = [&](juce::Label& l, int y)        { l.setBounds(mx, y, cw, 22); return y + 22 + gLbl; };
            auto check = [&](juce::ToggleButton& b, int y) { b.setBounds(mx, y, cw, 24); return y + 24 + gRow; };
            auto combo = [&](juce::ComboBox& c, int y)     { c.setBounds(mx, y, cw, 28); return y + 28 + gRow; };

            int y = 22;
            // ── 一般 (言語 / インポート) ──
            y = label(languageLabel, y);  y = combo(languageCombo, y);
            y = label(uiScaleLabel, y);   y = combo(uiScaleCombo, y);
            y = label(bitsLabel, y);      y = combo(bitsCombo, y);
            y = check(stripMetaBtn, y);

            // ── 編集動作 ──
            y += gSec; y = label(behaviorLabel, y);
            y = check(followSelBtn, y);
            y = check(rtzBtn, y);
            y = check(zoomMouseBtn, y);
            y = check(zeroCrossBtn, y);
            y = check(midiPagingBtn, y);
            y = check(tooltipsBtn, y);
            y = check(folderTracksBtn, y);
            y = check(folderExtrasBtn, y);

            // ── 録音フロー ──
            y += gSec; y = label(recLabel, y);
            y = check(recCompBtn, y);
            recCompOffsetLabel.setBounds(mx, y, 250, 24);
            recCompOffsetSlider.setBounds(mx + 256, y, cw - 256, 24); y += 24 + gRow;
            y = check(retakeKeepBtn, y);
            y = check(monInsBtn, y);
            y = check(diskStreamBtn, y);
            y = check(multicoreBtn, y);

            // ── 配信 ──
            // 見出しの隣 (括弧付き) に手順書リンクを置く。ラベルは実テキスト幅に縮めて
            // リンクをすぐ右へ (HyperlinkButton はバウンズ全体がヒット領域のため幅を文字に合わせる)
            y += gSec;
            {
                const int lw = juce::GlyphArrangement::getStringWidthInt(
                                   streamLabel.getFont(), streamLabel.getText());
                streamLabel.setBounds(mx, y, lw + 6, 22);
                streamGuideLink.changeWidthToFitText();
                streamGuideLink.setTopLeftPosition(mx + lw + 12, y);
                streamGuideLink.setSize(streamGuideLink.getWidth(), 22);
                y += 22 + gLbl;
            }
            y = check(mirrorBtn, y);
            y = label(mirrorDevLabel, y);
            y = combo(mirrorDevCombo, y);
            if (appCapUi)   // アプリケーショントラック (Windows のみ)
                y = check(appTracksBtn, y);

            // ── MIDI入力 ──
            y += gSec; y = label(midiInLabel, y);
            y = check(midiInBtn, y);
            y = label(midiInDevLabel, y);
            y = combo(midiInDevCombo, y);

            // ── 保存 / メータ / 音量 ──
            y += gSec; y = label(autoSaveLabel, y);    y = combo(autoSaveCombo, y);
            y = label(backupCountLabel, y);            y = combo(backupCountCombo, y);
            y = label(vuRefLabel, y);                  y = combo(vuRefCombo, y);
            y = label(loudnessLabel, y);               y = combo(loudnessCombo, y);
            y = check(autoNormBtn, y);

            // ── 書き出し ──
            y += gSec; y = label(exportLabel, y);
            y = check(peakGuardBtn, y);
            y = check(exportDoneDlgBtn, y);
            y = check(showMidiExportBtn, y);

            if (adsUi)
            {
                // ── 起動画面 ──
                y += gSec; y = label(startupLabel, y);
                y = check(showAdsBtn, y);
            }
            return y + 12;
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
        void syncUiToValues(int curBits, bool curFollowSel, bool curRtz,
                            int curAutoSaveMin, int curMaxBackups, float curVuRefDb, float curLoudnessLufs,
                            bool curAutoNorm, bool curZoomMouse, bool curPeakGuard, bool curZeroCross,
                            bool curStripMeta)
        {
            bitsCombo.setSelectedId(curBits == 24 ? 24 : 32, juce::dontSendNotification);
            followSelBtn.setToggleState(curFollowSel, juce::dontSendNotification);
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
    // 書き出し完了ダイアログの表示 (アプリ全体設定・2 ミックス / stems 共用)。即時保存のみ
    // (次の書き出しから反映。markProjectDirty は呼ばない)。
    dlg->exportDoneDlgBtn.setToggleState(appPrefs.showExportCompleteDialog, juce::dontSendNotification);
    dlg->onExportDoneDlgChanged = [this](bool v) {
        appPrefs.showExportCompleteDialog = v;
        appPrefs.save();
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
    // フォルダトラック追加の有効化 (アプリ全体設定)。即時保存のみ (「+ トラック追加」
    // メニューは開くたびに再構築されるので次に開いた時から反映される)。
    dlg->folderTracksBtn.setToggleState(appPrefs.enableFolderTracks, juce::dontSendNotification);
    dlg->onFolderTracksChanged = [this](bool v) {
        appPrefs.enableFolderTracks = v;
        appPrefs.save();
    };
    // フォルダの Pan/Rev 表示 (アプリ全体設定)。即時保存 + ヘッダへ即反映。
    dlg->folderExtrasBtn.setToggleState(appPrefs.showFolderPanRev, juce::dontSendNotification);
    dlg->onFolderExtrasChanged = [this](bool v) {
        appPrefs.showFolderPanRev = v;
        appPrefs.save();
        trackHeaderPanel.refresh();
    };
    // 画面の表示倍率 (アプリ全体設定。ハードウェア依存のためプロジェクト設定ではない)。
    // 即時に適用 (setGlobalScaleFactor) + 保存。次回起動でも Main.cpp が同じ倍率で復元する。
    auto uiScaleToId = [](double s) -> int {
        const int opts[] = { 80, 90, 100, 110, 125, 150, 175, 200 };
        const int pct = (int) std::round(s * 100.0);
        int best = 100, bestDiff = 1 << 30;
        for (int o : opts) { int d = std::abs(o - pct);
                             if (d < bestDiff) { bestDiff = d; best = o; } }
        return best;
    };
    // 初期選択は実効倍率 (ユーザー未設定なら自動判定値) を表示する。
    dlg->uiScaleCombo.setSelectedId(uiScaleToId(appPrefs.resolvedUiScale()), juce::dontSendNotification);
    dlg->onUiScaleChanged = [this](double scale) {
        appPrefs.uiScale = juce::jlimit(AppPreferences::minUiScale,
                                        AppPreferences::maxUiScale, scale);
        appPrefs.uiScaleUserSet = true;   // 以降は自動判定せずこの値を尊重
        appPrefs.save();
        juce::Desktop::getInstance().setGlobalScaleFactor((float) appPrefs.uiScale);
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
    // Q リテイクでテイクを残す (アプリ全体設定)。即時保存のみ (次の Q から効く)。
    dlg->retakeKeepBtn.setToggleState(appPrefs.retakeKeepsTake, juce::dontSendNotification);
    dlg->onRetakeKeepChanged = [this](bool v) {
        appPrefs.retakeKeepsTake = v;
        appPrefs.save();
    };
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
    // オーディオのマルチコア処理 (アプリ全体設定)。即時保存 + エンジンへ即反映 (次ブロックから従う)。
    dlg->multicoreBtn.setToggleState(appPrefs.multicoreAudio, juce::dontSendNotification);
    dlg->onMulticoreChanged = [this](bool v) {
        appPrefs.multicoreAudio = v;
        appPrefs.save();
        audioEngine.setMulticoreAudioEnabled(v);
    };
    // 配信ミラー出力 (アプリ全体設定)。即時保存 + ミラーデバイスの開始/停止を即反映。
    {
        // 「配信」見出し隣のリンク: 同梱ヘルプの配信セクション (#streaming) を既定ブラウザで開く。
        // 現在言語のヘルプへ言語別に飛ぶ。ヘルプが無い環境では非表示のまま。
        // setURL は使わない (URL 起動経路はフラグメントが落ちる・openBundledHelp のコメント参照)
        if (findBundledHelpFile().existsAsFile())
        {
            dlg->streamGuideLink.onClick = [this] { openBundledHelp("streaming"); };
            dlg->streamGuideLink.setVisible(true);
        }

        // ミラー出力先の候補を投入する。**メイン出力と同じデバイスは候補から外す**
        // (同じ I/O へのミラーは二重聞こえになるだけで正しい使い方が無いため・2026-07 要望)。
        // 名前で同一判定できるのは同じ API 同士のみ: Mac は CoreAudio 同士で常に比較可、
        // Windows はメインが標準ドライバ (Windows Audio 系) のときだけ。メインが ASIO のときは
        // ASIO ドライバ名と WASAPI デバイス名が別物で確実に対応付けられないため除外しない
        // (ラベルの「メインと同じデバイスを選ぶと二重に聞こえます」の注意書きでカバー)
        {
            dlg->mirrorDevNames = StreamMirrorOutput::getOutputDeviceNames();
            if (auto* mainDev = audioEngine.getDeviceManager().getCurrentAudioDevice())
            {
               #if JUCE_MAC
                const bool comparable = true;   // CoreAudio 同士
               #else
                const bool comparable = audioEngine.getDeviceManager()
                                            .getCurrentAudioDeviceType().startsWith("Windows Audio");
               #endif
                if (comparable)
                    dlg->mirrorDevNames.removeString(mainDev->getName());
            }
            for (int i = 0; i < dlg->mirrorDevNames.size(); ++i)
                dlg->mirrorDevCombo.addItem(dlg->mirrorDevNames[i], 100 + i);
        }

        dlg->mirrorBtn.setToggleState(appPrefs.streamMirrorEnabled, juce::dontSendNotification);
        dlg->mirrorDevCombo.setEnabled(appPrefs.streamMirrorEnabled);
        int selId = 0;   // 0 = 未選択 (「デバイスを選択…」表示・ミラーは開始しない)
        if (appPrefs.streamMirrorDevice.isNotEmpty())
        {
            const int idx = dlg->mirrorDevNames.indexOf(appPrefs.streamMirrorDevice);
            if (idx >= 0) selId = 100 + idx;
        }
        dlg->mirrorDevCombo.setSelectedId(selId, juce::dontSendNotification);
        dlg->onMirrorChanged = [this, dlg](bool v) {
            appPrefs.streamMirrorEnabled = v;
            appPrefs.save();
            dlg->mirrorDevCombo.setEnabled(v);
            // ON にしたのに出力先が未選択なら、コンボを開いて選択を促す (選んだ瞬間に開始される)
            if (v && dlg->mirrorDevCombo.getSelectedId() == 0)
            {
                dlg->mirrorDevCombo.showPopup();
                return;
            }
            applyStreamMirrorFromPrefs(/*showErrors*/ true);
        };
        dlg->onMirrorDeviceChanged = [this](juce::String dev) {
            appPrefs.streamMirrorDevice = dev;
            appPrefs.save();
            if (appPrefs.streamMirrorEnabled)
                applyStreamMirrorFromPrefs(/*showErrors*/ true);   // デバイス変更を即反映
        };
    }
    // アプリケーショントラック (Windows のみ・アプリ全体設定)。即時保存 (追加メニューの表示ゲート)。
    if (dlg->appCapUi)
    {
        dlg->appTracksBtn.setToggleState(appPrefs.enableAppCaptureTracks, juce::dontSendNotification);
        dlg->onAppTracksChanged = [this](bool v) {
            appPrefs.enableAppCaptureTracks = v;
            appPrefs.save();
        };
    }
    // MIDI 入力 (アプリ全体設定)。即時保存 + デバイスの開閉を即反映。
    {
        for (const auto& d : juce::MidiInput::getAvailableDevices())
            dlg->midiInDevNames.add(d.name);
        for (int i = 0; i < dlg->midiInDevNames.size(); ++i)
            dlg->midiInDevCombo.addItem(dlg->midiInDevNames[i], 100 + i);

        dlg->midiInBtn.setToggleState(appPrefs.midiInputEnabled, juce::dontSendNotification);
        dlg->midiInDevCombo.setEnabled(appPrefs.midiInputEnabled);
        int selId = 0;   // 0 = 未選択 (「デバイスを選択…」表示・開かない)
        if (appPrefs.midiInputDevice.isNotEmpty())
        {
            const int idx = dlg->midiInDevNames.indexOf(appPrefs.midiInputDevice);
            if (idx >= 0) selId = 100 + idx;
        }
        dlg->midiInDevCombo.setSelectedId(selId, juce::dontSendNotification);
        dlg->onMidiInChanged = [this, dlg](bool v) {
            appPrefs.midiInputEnabled = v;
            appPrefs.save();
            dlg->midiInDevCombo.setEnabled(v);
            // ON にしたのにデバイス未選択なら選択を促す。候補が 1 つだけなら自動選択
            // (MIDI キーボードは 1 台だけ繋いでいるのが普通なのでワンクリックで済ませる)
            if (v && dlg->midiInDevCombo.getSelectedId() == 0)
            {
                if (dlg->midiInDevNames.size() == 1)
                    dlg->midiInDevCombo.setSelectedId(100);   // onChange 経由で保存 + 適用される
                else
                    dlg->midiInDevCombo.showPopup();
                return;
            }
            applyMidiInputFromPrefs(/*showErrors*/ true);
        };
        dlg->onMidiInDeviceChanged = [this](juce::String dev) {
            appPrefs.midiInputDevice = dev;
            appPrefs.save();
            if (appPrefs.midiInputEnabled)
                applyMidiInputFromPrefs(/*showErrors*/ true);   // デバイス変更を即反映
        };
    }
    dlg->onResetDefaults = [this, dlg, uiScaleToId]
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
        appPrefs.showExportCompleteDialog = defPrefs.showExportCompleteDialog;
        appPrefs.showAds            = defPrefs.showAds;
        appPrefs.midiPagingEnabled  = defPrefs.midiPagingEnabled;
        appPrefs.recLatencyAutoComp = defPrefs.recLatencyAutoComp;
        appPrefs.recLatencyManualMs = defPrefs.recLatencyManualMs;
        appPrefs.retakeKeepsTake    = defPrefs.retakeKeepsTake;
        appPrefs.monitorThroughInserts = defPrefs.monitorThroughInserts;
        appPrefs.diskStreaming      = defPrefs.diskStreaming;
        appPrefs.multicoreAudio     = defPrefs.multicoreAudio;
        appPrefs.streamMirrorEnabled = defPrefs.streamMirrorEnabled;
        appPrefs.streamMirrorDevice  = defPrefs.streamMirrorDevice;
        appPrefs.enableAppCaptureTracks = defPrefs.enableAppCaptureTracks;
        appPrefs.midiInputEnabled    = defPrefs.midiInputEnabled;
        appPrefs.midiInputDevice     = defPrefs.midiInputDevice;
        appPrefs.uiScale            = defPrefs.uiScale;
        appPrefs.uiScaleUserSet     = defPrefs.uiScaleUserSet;   // 自動判定へ戻す
        appPrefs.save();
        juce::Desktop::getInstance().setGlobalScaleFactor((float) appPrefs.resolvedUiScale());
        menuItemsChanged();
        applyMidiPagingToOpenEditors();
        audioEngine.setRecordingLatencyComp(appPrefs.recLatencyAutoComp,
                                            appPrefs.recLatencyManualMs);
        audioEngine.setDiskStreamingEnabled(appPrefs.diskStreaming);
        audioEngine.setMulticoreAudioEnabled(appPrefs.multicoreAudio);
        applyStreamMirrorFromPrefs(/*showErrors*/ false);   // 既定 OFF へ (ミラー停止)
        applyMidiInputFromPrefs(/*showErrors*/ false);      // 既定 OFF へ (MIDI 入力を閉じる)
        syncInputMonitorStateToEngine();   // モニタ FX 経路を既定 (ON) に戻す
        dlg->showMidiExportBtn.setToggleState(appPrefs.showMidiExportMenu, juce::dontSendNotification);
        dlg->exportDoneDlgBtn.setToggleState(appPrefs.showExportCompleteDialog, juce::dontSendNotification);
        dlg->midiPagingBtn.setToggleState(appPrefs.midiPagingEnabled, juce::dontSendNotification);
        if (AppPreferences::adsCompiledIn())
            dlg->showAdsBtn.setToggleState(appPrefs.showAds, juce::dontSendNotification);
        dlg->recCompBtn.setToggleState(appPrefs.recLatencyAutoComp, juce::dontSendNotification);
        dlg->recCompOffsetSlider.setValue(appPrefs.recLatencyManualMs, juce::dontSendNotification);
        dlg->retakeKeepBtn.setToggleState(appPrefs.retakeKeepsTake, juce::dontSendNotification);
        dlg->monInsBtn.setToggleState(appPrefs.monitorThroughInserts, juce::dontSendNotification);
        dlg->diskStreamBtn.setToggleState(appPrefs.diskStreaming, juce::dontSendNotification);
        dlg->multicoreBtn.setToggleState(appPrefs.multicoreAudio, juce::dontSendNotification);
        dlg->mirrorBtn.setToggleState(appPrefs.streamMirrorEnabled, juce::dontSendNotification);
        dlg->mirrorDevCombo.setSelectedId(0, juce::dontSendNotification);   // 未選択へ戻す
        dlg->mirrorDevCombo.setEnabled(appPrefs.streamMirrorEnabled);
        if (dlg->appCapUi)
            dlg->appTracksBtn.setToggleState(appPrefs.enableAppCaptureTracks, juce::dontSendNotification);
        dlg->midiInBtn.setToggleState(appPrefs.midiInputEnabled, juce::dontSendNotification);
        dlg->midiInDevCombo.setSelectedId(0, juce::dontSendNotification);   // 未選択へ戻す
        dlg->midiInDevCombo.setEnabled(appPrefs.midiInputEnabled);
        dlg->uiScaleCombo.setSelectedId(uiScaleToId(appPrefs.resolvedUiScale()), juce::dontSendNotification);

        // ダイアログの UI を新しい値に同期
        dlg->syncUiToValues(appSettings.resampleOutputBits,
                            appSettings.playheadFollowsSelection,
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

// 配信ミラー出力の開始/停止を appPrefs に同期させる (起動時と環境設定変更時に呼ぶ)。
// 開始に失敗した場合は設定を OFF に戻さない (デバイスを繋ぎ直して再起動すれば復帰する)
void MainComponent::applyStreamMirrorFromPrefs(bool showErrors)
{
    // 出力先が未選択の間は開始しない (「既定の出力デバイス」廃止後の未選択状態。エラーにはしない
    // — 旧設定の「既定」(空) が残っているユーザーも起動時に黙って停止し、設定画面で選び直させる)
    if (!appPrefs.streamMirrorEnabled || appPrefs.streamMirrorDevice.isEmpty())
    {
        streamMirror.stop(audioEngine);
        return;
    }

    const juce::String err = streamMirror.start(appPrefs.streamMirrorDevice, audioEngine);
    if (err.isNotEmpty())
    {
        DBG("StreamMirror start failed: " << err);
        if (showErrors)
            juce::AlertWindow::showAsync(juce::MessageBoxOptions()
                .withIconType(juce::MessageBoxIconType::WarningIcon)
                .withTitle(tr(u8"配信ミラー出力"))
                .withMessage(tr(u8"配信ミラー出力を開始できませんでした。") + "\n" + err)
                .withButton("OK"), nullptr);
    }
}

// MIDI キーボード入力の開閉を appPrefs に同期させる (起動時・環境設定変更時・デバイス抜き差し時)。
// デバイスは名前で照合する (streamMirrorDevice と同じ作法。identifier は OS/再接続で変わりうる)
void MainComponent::applyMidiInputFromPrefs(bool showErrors)
{
    midiKeyboardInput.reset();   // 既存を閉じる (OFF / デバイス変更 / 開き直しの共通経路)
    if (appPrefs.midiInputEnabled && appPrefs.midiInputDevice.isNotEmpty())
    {
        for (const auto& d : juce::MidiInput::getAvailableDevices())
        {
            if (d.name != appPrefs.midiInputDevice) continue;
            midiKeyboardInput = juce::MidiInput::openDevice(d.identifier, &midiKeyboardCallback);
            break;
        }
        if (midiKeyboardInput != nullptr)
            midiKeyboardInput->start();
        else if (showErrors)
            juce::AlertWindow::showAsync(juce::MessageBoxOptions()
                .withIconType(juce::MessageBoxIconType::WarningIcon)
                .withTitle(tr(u8"MIDI入力"))
                .withMessage(tr(u8"MIDIキーボードを開けませんでした。デバイスの接続を確認してください。"))
                .withButton("OK"), nullptr);
    }
    // 接続中だけ MIDI トラックに R (録音アーム) ボタンを出す
    trackHeaderPanel.setMidiInputAvailable(midiKeyboardInput != nullptr);
    // 開いているピアノロールのステップ入力ボタンも接続状態に追従させる
    // (切断時はステップ入力中でも解除される)
    for (auto* w : pianoRollWindows)
        if (w != nullptr)
            if (auto* ed = w->getEditor())
                ed->setMidiInputAvailable(midiKeyboardInput != nullptr);
    updateLiveMidiTarget();
}

// ライブ MIDI の出力先トラックをエンジンへ publish する。優先順: (1) ステップ入力中の
// ピアノロールの編集対象トラック (置くクリップとモニタ音源を一致させる)、(2) 選択中の
// トラックが MIDI トラックならそこ、(3) 最初の MIDI トラック (1 本ならどこを選んでいても
// 鳴る)。デバイスが開いていない間は -1 (無効)。ついでにステップ入力の有無フラグ
// (stepInputWanted・MIDI スレッドの callAsync 事前判定) もここで更新する
void MainComponent::updateLiveMidiTarget()
{
    int  target     = -1;
    bool stepWanted = false;
    // dispatchStepInputNote と同じ順 (pianoRollWindows 先頭から) で最初のステップ入力中
    // エディタを探し、そのクリップを持つ MIDI トラックを解決する
    for (auto* w : pianoRollWindows)
    {
        if (w == nullptr) continue;
        auto* ed = w->getEditor();
        if (ed == nullptr || !ed->isStepInputActive()) continue;
        stepWanted = true;
        if (auto* clip = w->getClip())
            for (int i = 0; i < trackManager.getTrackCount() && target < 0; ++i)
                if (auto* t = trackManager.getTrack(i); t != nullptr && t->isMidiTrack())
                    for (int c = 0; c < t->getMidiClipCount(); ++c)
                        if (t->getMidiClip(c) == clip) { target = i; break; }
        break;
    }
    stepInputWanted.store(stepWanted);

    if (midiKeyboardInput == nullptr)
    {
        audioEngine.setLiveMidiTargetTrack(-1);
        return;
    }
    // Rec アーム済みの MIDI トラックがあればそこを優先する (録音先とモニタ音源を一致させる。
    // アーム = 「このトラックを弾く」意思表示なので、選択がどこにあっても鳴らす)
    if (target < 0)
        for (int i = 0; i < trackManager.getTrackCount(); ++i)
            if (auto* t = trackManager.getTrack(i);
                t != nullptr && t->isMidiTrack() && !t->isClickTrack() && t->isRecArmed())
            {
                target = i;
                break;
            }
    if (target < 0)
    {
        auto* sel = (selectedTrackIndex >= 0 && selectedTrackIndex < trackManager.getTrackCount())
                        ? trackManager.getTrack(selectedTrackIndex) : nullptr;
        if (sel != nullptr && sel->isMidiTrack())
            target = selectedTrackIndex;
        else
            for (int i = 0; i < trackManager.getTrackCount(); ++i)
                if (auto* t = trackManager.getTrack(i); t != nullptr && t->isMidiTrack())
                {
                    target = i;
                    break;
                }
    }
    audioEngine.setLiveMidiTargetTrack(target);
}
