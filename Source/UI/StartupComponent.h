// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

#pragma once
#include <JuceHeader.h>
#include "../Localisation.h"
#include "../Audio/AudioDeviceSettings.h"
#include "../Project/UpdateChecker.h"
#include "AdPanel.h"

// 円形リフレッシュ (2 本矢印) アイコンボタン。「保存先を既定に戻す」用。TextButton を継承して
// 背景は LookAndFeel を再利用 (隣の「…」ボタンと統一感)、上に 2 本の弧 + 矢尻を自前描画する。
class ResetIconButton : public juce::TextButton
{
public:
    void paintButton (juce::Graphics& g, bool over, bool down) override
    {
        getLookAndFeel().drawButtonBackground (g, *this,
            findColour (juce::TextButton::buttonColourId), over, down);

        auto bnd = getLocalBounds().toFloat();
        const float cx = bnd.getCentreX(), cy = bnd.getCentreY();
        const float r  = juce::jmin (bnd.getWidth(), bnd.getHeight()) * 0.28f;
        const float thick = juce::jmax (1.6f, r * 0.26f);
        const float hlen  = r * 0.85f;   // 矢尻の長さ (前方)
        const float hw    = r * 0.62f;   // 矢尻の半幅
        g.setColour (juce::Colours::white.withAlpha ((over || down) ? 1.0f : 0.9f));

        // 弧 (12 時 = 0°・時計回りに角度が増える)。ギャップを大きめに取り 2 本の矢印に見せる。
        auto arc = [&] (float fromDeg, float toDeg)
        {
            juce::Path p;
            p.addCentredArc (cx, cy, r, r, 0.0f,
                             juce::degreesToRadians (fromDeg), juce::degreesToRadians (toDeg), true);
            g.strokePath (p, juce::PathStrokeType (thick, juce::PathStrokeType::curved,
                                                          juce::PathStrokeType::rounded));
        };
        // 弧の終端に「塗りつぶし三角」の矢尻。進行方向 (時計回りの接線 = (cosθ, sinθ)) に尖らせる。
        auto arrowHead = [&] (float endDeg)
        {
            const float a = juce::degreesToRadians (endDeg);
            const juce::Point<float> base (cx + r * std::sin (a), cy - r * std::cos (a)); // 弧の端
            const juce::Point<float> fwd  (std::cos (a),  std::sin (a));   // 進行方向 (単位)
            const juce::Point<float> perp (-std::sin (a), std::cos (a));   // 直交
            juce::Path tri;
            tri.startNewSubPath (base + fwd  * hlen);   // 先端
            tri.lineTo         (base + perp * hw);
            tri.lineTo         (base - perp * hw);
            tri.closeSubPath();
            g.fillPath (tri);
        };

        arc (35.0f, 165.0f);   arrowHead (165.0f);   // 上の弧 → 右下で矢尻
        arc (215.0f, 345.0f);  arrowHead (345.0f);   // 下の弧 → 左上で矢尻
    }
};

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
    ResetIconButton  defaultLocBtn;   // 保存先を既定に戻す (円形リフレッシュアイコン)
    juce::ComboBox   sampleRateBox;
    juce::ComboBox   bitDepthBox;
    juce::Label      deviceSummaryLabel;
    juce::TextButton deviceChangeBtn { tr(u8"変更...") };
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
