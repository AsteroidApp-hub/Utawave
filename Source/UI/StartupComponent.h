// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

#pragma once
#include <JuceHeader.h>
#include "../Localisation.h"
#include "../Audio/AudioDeviceSettings.h"
#include "../Project/UpdateChecker.h"
#include "AdPanel.h"

class StartupComponent : public juce::Component,
                         public juce::ListBoxModel,
                         private juce::ChangeListener
{
public:
    // showAds: 右端に広告枠を表示するか (アプリ全体設定 AppPreferences::showAds に対応)。
    explicit StartupComponent(bool showAds = true);
    ~StartupComponent() override;

    // 起動ウィンドウの推奨サイズ。広告枠の有無で横幅が変わる。
    static constexpr int kWidthWithAds = 1080;
    static constexpr int kWidthNoAds   = 820;
    static constexpr int kHeight       = 520;

    // file: 新規時は <location>/<name>/<name>.uta、既存時は選択された .uta
    // sr / bits: 新規時のみ意味を持つ。既存読み込み時はプロジェクトファイル側の値を使用
    std::function<void(const juce::File& file, double sampleRate, int bitDepth, bool isNew)> onProjectChosen;

    void paint(juce::Graphics&) override;
    void resized() override;

    // ListBoxModel
    int getNumRows() override;
    void paintListBoxItem(int rowNumber, juce::Graphics& g,
                          int width, int height, bool rowIsSelected) override;
    void listBoxItemDoubleClicked(int row, const juce::MouseEvent&) override;

private:
    void chooseLocation();
    // 新規プロジェクト保存先の既定 (~/Documents/Utawave) と、前回値の永続化。
    static juce::File defaultProjectLocation();
    void persistProjectLocation(const juce::String& path);   // 空 = 既定へリセット
    void browseExisting();
    void createNewProject();
    void refreshRecents();
    void showDeviceDialog();
    // プロジェクト作成前に、アプリ全体設定 (AppPreferences) + 言語 + 表示倍率だけを設定できる
    // 独立ダイアログ (本体の環境設定はプロジェクト/エンジンに依存するため起動画面では開けない)。
    void showStartupPreferences();
    void refreshDeviceLabel();
    // 新規プロジェクトの既定サンプルレートを、現在のオーディオデバイスの SR に合わせる
    // (Windows 等でデバイス既定と食い違って音切れするのを防ぐ)。ユーザーが手動で選んだら尊重する。
    // 現在のデバイスが対応する SR だけをコンボに列挙する (getAvailableSampleRates)。
    // 非対応 SR を選べなくして「選んだ SR をデバイスが出せず自動追従」する事故を防ぐ。
    void populateSampleRateBox();
    void syncSampleRateBoxToDevice();
    bool srUserPicked { false };   // サンプルレートをユーザーが手動選択したか
    double lastProjectSrPref { 0.0 };   // 前回作成した SR (AppPreferences 由来、0=未設定)。既定に引き継ぐ

    // アップデート通知 (右上のリンク)。GitHub の最新リリース / タグを非同期取得し、現在版より
    // 新しければリンクを表示する。クリックでリリースページを既定ブラウザで開く。
    void startUpdateCheck();
    void showUpdateBanner(const UpdateInfo& info);
    // リンクをテキスト幅に合わせて配置可能領域 (updateBannerArea) の右端へ寄せる。
    // 文字以外がクリック対象にならないよう、ヒット領域をテキスト幅に限定する。
    void layoutUpdateBanner();

    // 3 カラム (左:新規 / 中央:最近 / 右:広告) の枠 (角丸カード) を算出する。
    // 広告無効時は ad が空で、左右 2 カラムになる。paint / resized で共用。
    struct Columns { juce::Rectangle<int> newCol, recentsCol, adCol; };
    Columns computeColumns() const;

    const bool adsEnabled;

    // 左カラム（新規）
    juce::Label    titleLabel;
    juce::Label    subtitleLabel;
    juce::Label    newSectionLabel;
    juce::Label    nameLabel, locationLabel, sampleRateLabel, bitDepthLabel, deviceLabel;
    juce::TextEditor nameEditor;
    juce::TextEditor locationEditor;
    juce::TextButton browseLocBtn  { tr(u8"参照...") };
    juce::TextButton defaultLocBtn { tr(u8"デフォルト") };   // 保存先を既定に戻す
    juce::ComboBox   sampleRateBox;
    juce::ComboBox   bitDepthBox;
    juce::Label      deviceSummaryLabel;
    juce::TextButton deviceChangeBtn { tr(u8"変更...") };
    juce::TextButton settingsBtn     { tr(u8"環境設定...") };   // 作成前にアプリ全体設定を開く
    juce::TextButton createBtn       { tr(u8"作成") };

    // 起動画面用の AudioDeviceManager（保存済み状態を共有）
    juce::AudioDeviceManager startupDeviceManager;

    // ChangeListener
    void changeListenerCallback(juce::ChangeBroadcaster*) override;

    // 中央カラム（最近 / 開く）
    juce::Label      recentsLabel;
    juce::ListBox    recentsList   { "Recents", this };
    juce::TextButton openBtn       { tr(u8"別の場所から開く...") };

    // 右カラム（広告）。adsEnabled の時だけ生成する。
    juce::Label                adsLabel;
    std::unique_ptr<AdPanel>   adPanel;

    juce::Array<juce::File> recents;
    std::unique_ptr<juce::FileChooser> fileChooser;

    // アップデート通知 (新しい版がある時だけ可視・右寄せの赤いリンクテキスト)
    juce::HyperlinkButton     updateBanner;
    juce::Rectangle<int>      updateBannerArea;   // リンクを右寄せ配置する領域 (右端基準)
    UpdateChecker::CancelFlag updateCancel;

    // 起動画面は MainComponent とは別に表示されるため、ツールチップ (広告タイトルの全文など) を
    // 出すには専用の TooltipWindow が要る。AppPreferences::showTooltips が ON のときだけ生成する。
    std::unique_ptr<juce::TooltipWindow> tooltipWindow;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(StartupComponent)
};
