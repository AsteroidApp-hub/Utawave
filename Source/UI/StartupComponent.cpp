// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

#include "StartupComponent.h"
#include "../Localisation.h"
#include "../Project/RecentProjects.h"
#include "../Project/AppPreferences.h"
#include "UtawaveLookAndFeel.h"

StartupComponent::StartupComponent(bool showAds)
    : adsEnabled(showAds)
{
    // 起動画面専用の TooltipWindow (無いと setTooltip / TooltipClient が機能しない)。
    // ツールチップは UtawaveLookAndFeel で描く (グローバル既定 LnF が無いため明示適用)。
    // アプリ全体の「ツールチップ表示」設定 (既定 ON) を尊重し、OFF なら作らない。
    if (AppPreferences::load().showTooltips)
    {
        static UtawaveLookAndFeel tooltipLnF;   // 全 TooltipWindow より長生き (関数ローカル static)
        tooltipWindow = std::make_unique<juce::TooltipWindow>(this);
        tooltipWindow->setLookAndFeel(&tooltipLnF);
    }

    titleLabel.setText("Utawave", juce::dontSendNotification);
    titleLabel.setFont(juce::FontOptions(28.0f, juce::Font::bold));
    titleLabel.setJustificationType(juce::Justification::centredLeft);
    titleLabel.setColour(juce::Label::textColourId, juce::Colours::white);
    addAndMakeVisible(titleLabel);

    subtitleLabel.setText(tr(u8"プロジェクトを作成または開く"),
                          juce::dontSendNotification);
    subtitleLabel.setFont(juce::FontOptions(13.0f));
    subtitleLabel.setColour(juce::Label::textColourId, juce::Colour(0xff9aa0a6));
    addAndMakeVisible(subtitleLabel);

    auto styleSection = [](juce::Label& l, const juce::String& text)
    {
        l.setText(text, juce::dontSendNotification);
        l.setFont(juce::FontOptions(15.0f, juce::Font::bold));
        l.setColour(juce::Label::textColourId, juce::Colours::white);
    };
    styleSection(newSectionLabel, tr(u8"新規プロジェクト"));
    styleSection(recentsLabel,    tr(u8"最近使ったプロジェクト"));
    addAndMakeVisible(newSectionLabel);
    addAndMakeVisible(recentsLabel);

    if (adsEnabled)
    {
        styleSection(adsLabel, tr(u8"広告"));
        addAndMakeVisible(adsLabel);
        adPanel = std::make_unique<AdPanel>();
        addAndMakeVisible(*adPanel);
        adPanel->load();
    }

    auto styleField = [](juce::Label& l, const juce::String& text)
    {
        l.setText(text, juce::dontSendNotification);
        l.setFont(juce::FontOptions(12.0f));
        l.setColour(juce::Label::textColourId, juce::Colour(0xffb0b6bd));
    };
    styleField(nameLabel,       tr(u8"プロジェクト名"));
    styleField(locationLabel,   tr(u8"保存先"));
    styleField(sampleRateLabel, tr(u8"サンプルレート"));
    styleField(bitDepthLabel,   tr(u8"ビット深度"));
    styleField(deviceLabel,     tr(u8"オーディオI/F"));
    addAndMakeVisible(nameLabel);
    addAndMakeVisible(locationLabel);
    addAndMakeVisible(sampleRateLabel);
    addAndMakeVisible(bitDepthLabel);
    addAndMakeVisible(deviceLabel);

    nameEditor.setText("Untitled", juce::dontSendNotification);
    nameEditor.setSelectAllWhenFocused(true);
    addAndMakeVisible(nameEditor);

    // 保存先は前回値 (AppPreferences::lastProjectLocation) を引き継ぐ。空なら既定へ。
    const auto savedLoc = AppPreferences::load().lastProjectLocation;
    juce::File initLoc = savedLoc.isNotEmpty() ? juce::File(savedLoc) : defaultProjectLocation();
    initLoc.createDirectory();
    locationEditor.setText(initLoc.getFullPathName(), juce::dontSendNotification);
    addAndMakeVisible(locationEditor);

    addAndMakeVisible(browseLocBtn);
    browseLocBtn.onClick = [this] { chooseLocation(); };

    // 「デフォルト」: 保存先を既定 (~/Documents/Utawave) に戻し、前回値もクリアする
    addAndMakeVisible(defaultLocBtn);
    defaultLocBtn.onClick = [this]
    {
        auto def = defaultProjectLocation();
        def.createDirectory();
        locationEditor.setText(def.getFullPathName(), juce::dontSendNotification);
        persistProjectLocation({});   // 空 = 既定へリセット (次回起動も既定)
    };

    // SR コンボの項目は、デバイス初期化後に populateSampleRateBox() で「そのデバイスが実際に
    // 対応する SR」だけを列挙する (getAvailableSampleRates)。非対応 SR を選べないようにして
    // 「選んだ SR をデバイスが出せず自動追従」という事故を未然に防ぐ。ここでは onChange のみ設定。
    sampleRateBox.onChange = [this] { srUserPicked = true; };  // 手動選択を記録 (以後デバイスに追従しない)
    addAndMakeVisible(sampleRateBox);

    bitDepthBox.addItem(tr(u8"16 bit"),       16);
    bitDepthBox.addItem(tr(u8"24 bit"),       24);
    bitDepthBox.addItem(tr(u8"32 bit float"), 32);
    // 前回作成したプロジェクトのビット深度を既定に引き継ぐ (AppPreferences)。SR は下の
    // syncSampleRateBoxToDevice で「前回 SR → 無ければデバイス SR」の順に決める。
    {
        const auto prefs = AppPreferences::load();
        lastProjectSrPref = prefs.lastProjectSampleRate;
        const int lastBd = prefs.lastProjectBitDepth;
        bitDepthBox.setSelectedId((lastBd == 16 || lastBd == 24 || lastBd == 32) ? lastBd : 32,
                                  juce::dontSendNotification);
    }
    addAndMakeVisible(bitDepthBox);

    deviceSummaryLabel.setFont(juce::FontOptions(12.0f));
    deviceSummaryLabel.setColour(juce::Label::textColourId, juce::Colours::white);
    deviceSummaryLabel.setColour(juce::Label::backgroundColourId, juce::Colour(0xff1f1f1f));
    deviceSummaryLabel.setBorderSize({ 4, 8, 4, 8 });
    deviceSummaryLabel.setMinimumHorizontalScale(0.7f);
    addAndMakeVisible(deviceSummaryLabel);
    addAndMakeVisible(deviceChangeBtn);
    deviceChangeBtn.onClick = [this] { showDeviceDialog(); };

    // 共有 XML から起動用 deviceManager を初期化（MainComponent と状態を共有）
    AudioDeviceSettings::initialise(startupDeviceManager, 2, 2);
    startupDeviceManager.addChangeListener(this);
    refreshDeviceLabel();
    populateSampleRateBox();   // デバイス対応 SR を列挙し、既定を選ぶ

    createBtn.onClick = [this] { createNewProject(); };
    createBtn.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff2a78ff));
    createBtn.setColour(juce::TextButton::textColourOnId,  juce::Colours::white);
    createBtn.setColour(juce::TextButton::textColourOffId, juce::Colours::white);
    addAndMakeVisible(createBtn);

    recentsList.setColour(juce::ListBox::backgroundColourId, juce::Colour(0xff202225));
    recentsList.setRowHeight(46);
    addAndMakeVisible(recentsList);

    openBtn.onClick = [this] { browseExisting(); };
    addAndMakeVisible(openBtn);

    // アップデート通知 (右上)。更新が見つかるまで非表示。右寄せの赤いリンクテキスト。
    // 幅はテキストに合わせて縮める (showUpdateBanner) ので、文字以外はクリック対象にならない。
    updateBanner.setColour(juce::HyperlinkButton::textColourId, juce::Colour(0xffff5252));
    updateBanner.setJustificationType(juce::Justification::centredRight);
    updateBanner.setFont(juce::FontOptions(11.5f, juce::Font::bold), false,
                         juce::Justification::centredRight);
    addChildComponent(updateBanner);   // 追加するが可視化はしない (クリックで URL を開く)

    refreshRecents();
    setSize(adsEnabled ? kWidthWithAds : kWidthNoAds, kHeight);

    startUpdateCheck();
}

StartupComponent::~StartupComponent()
{
    if (updateCancel != nullptr)
        updateCancel->store(true);   // worker の結果配信を打ち切る
    startupDeviceManager.removeChangeListener(this);
    startupDeviceManager.closeAudioDevice();
}

void StartupComponent::startUpdateCheck()
{
    updateCancel = std::make_shared<std::atomic<bool>>(false);

    juce::Component::SafePointer<StartupComponent> safe(this);
    UpdateChecker::check(UpdateChecker::defaultVersionUrl(),
                         UpdateChecker::defaultDownloadPageUrl(), updateCancel,
        [safe](UpdateInfo info, bool ok) mutable
        {
            if (auto* self = safe.getComponent())
                if (ok && UpdateChecker::isNewerVersion(info.version,
                                                        UpdateChecker::currentAppVersion()))
                    self->showUpdateBanner(info);
        });
}

void StartupComponent::showUpdateBanner(const UpdateInfo& info)
{
    updateBanner.setButtonText(tr(u8"アップデートがあります") + "  " + info.tag + "   ↓");
    updateBanner.setURL(juce::URL(info.pageUrl));            // クリックで既定ブラウザがこの URL を開く
    updateBanner.setTooltip(tr(u8"ダウンロードページを開く"));  // setURL がツールチップを上書きするので後に
    updateBanner.setVisible(true);
    updateBanner.toFront(false);
    layoutUpdateBanner();                                    // テキスト幅で右端に再配置
}

void StartupComponent::layoutUpdateBanner()
{
    if (! updateBanner.isVisible() || updateBannerArea.isEmpty())
        return;

    updateBanner.setBounds(updateBannerArea);     // まず領域全体に置いて高さを確定
    updateBanner.changeWidthToFitText();          // 幅をテキストに合わせる (左上基準・+6px)
    const int w = juce::jmin(updateBanner.getWidth(), updateBannerArea.getWidth());
    updateBanner.setBounds(updateBannerArea.getRight() - w, updateBannerArea.getY(),
                           w, updateBannerArea.getHeight());   // 右端へ寄せる
}

void StartupComponent::refreshRecents()
{
    recents = RecentProjects::load();
    recentsList.updateContent();
    recentsList.repaint();
}

StartupComponent::Columns StartupComponent::computeColumns() const
{
    // タイトル / サブタイトル分を上に空けたカード帯
    auto band = getLocalBounds().reduced(28).withTrimmedTop(64);
    const int gap = 22;

    Columns c;
    if (adsEnabled)
    {
        const int newW = 360;
        const int adW  = 244;
        c.newCol = band.removeFromLeft(newW);
        band.removeFromLeft(gap);
        c.adCol = band.removeFromRight(adW);
        band.removeFromRight(gap);
        c.recentsCol = band;
    }
    else
    {
        const int newW = band.getWidth() / 2 - gap / 2;
        c.newCol = band.removeFromLeft(newW);
        band.removeFromLeft(gap);
        c.recentsCol = band;
        c.adCol = {};
    }
    return c;
}

void StartupComponent::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xff1a1a1a));

    const auto cols = computeColumns();
    auto card = [&g](juce::Rectangle<int> r)
    {
        if (r.isEmpty()) return;
        g.setColour(juce::Colour(0xff242424));
        g.fillRoundedRectangle(r.toFloat(), 8.0f);
        g.setColour(juce::Colour(0xff2f2f2f));
        g.drawRoundedRectangle(r.toFloat(), 8.0f, 1.0f);
    };
    card(cols.newCol);
    card(cols.recentsCol);
    card(cols.adCol);
}

void StartupComponent::resized()
{
    auto r = getLocalBounds().reduced(28);

    titleLabel.setBounds(r.removeFromTop(34));
    subtitleLabel.setBounds(r.removeFromTop(20));

    const auto cols = computeColumns();

    // アップデート通知リンク: ヘッダ帯 (タイトル+サブタイトルの高さ) の右側、右カラムの
    // 左端からコンテンツ右端まで。タイトル文字は左側にあるので水平には重ならない。
    // 実際のリンクはこの領域の右端へテキスト幅で寄せる (layoutUpdateBanner)。
    {
        auto band = getLocalBounds().reduced(28).removeFromTop(54);
        band.setLeft(cols.recentsCol.getX());
        // サブタイトルの高さに合わせて少し下げる (右寄せ・小さめのリンク)
        updateBannerArea = band.reduced(0, 9).translated(0, 16);
        layoutUpdateBanner();
    }
    auto leftCol  = cols.newCol.reduced(20);
    auto rightCol = cols.recentsCol.reduced(20);

    // 左カラム
    newSectionLabel.setBounds(leftCol.removeFromTop(28));
    leftCol.removeFromTop(8);

    auto field = [&leftCol](juce::Label& lbl, juce::Component& c, int h = 28)
    {
        lbl.setBounds(leftCol.removeFromTop(18));
        c.setBounds(leftCol.removeFromTop(h));
        leftCol.removeFromTop(12);
    };

    field(nameLabel, nameEditor);

    {
        locationLabel.setBounds(leftCol.removeFromTop(18));
        auto row = leftCol.removeFromTop(28);
        defaultLocBtn.setBounds(row.removeFromRight(92));
        row.removeFromRight(6);
        browseLocBtn.setBounds(row.removeFromRight(72));
        row.removeFromRight(6);
        locationEditor.setBounds(row);
        leftCol.removeFromTop(12);
    }

    auto row2 = leftCol.removeFromTop(46);
    auto half = row2.getWidth() / 2 - 6;
    {
        auto col = row2.removeFromLeft(half);
        sampleRateLabel.setBounds(col.removeFromTop(18));
        sampleRateBox.setBounds(col.removeFromTop(28));
    }
    row2.removeFromLeft(12);
    {
        auto col = row2;
        bitDepthLabel.setBounds(col.removeFromTop(18));
        bitDepthBox.setBounds(col.removeFromTop(28));
    }
    leftCol.removeFromTop(12);

    // オーディオI/F 行
    {
        deviceLabel.setBounds(leftCol.removeFromTop(18));
        auto row = leftCol.removeFromTop(28);
        deviceChangeBtn.setBounds(row.removeFromRight(80));
        row.removeFromRight(6);
        deviceSummaryLabel.setBounds(row);
    }

    createBtn.setBounds(leftCol.removeFromBottom(40));

    // 中央カラム (最近)
    recentsLabel.setBounds(rightCol.removeFromTop(28));
    rightCol.removeFromTop(8);
    openBtn.setBounds(rightCol.removeFromBottom(34));
    rightCol.removeFromBottom(10);
    recentsList.setBounds(rightCol);

    // 右カラム (広告)
    if (adsEnabled && adPanel != nullptr)
    {
        auto adCol = cols.adCol.reduced(16);
        adsLabel.setBounds(adCol.removeFromTop(28));
        adCol.removeFromTop(6);
        adPanel->setBounds(adCol);
    }
}

juce::File StartupComponent::defaultProjectLocation()
{
    return juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("Utawave");
}

void StartupComponent::persistProjectLocation(const juce::String& path)
{
    // 保存先を AppPreferences に記憶する (空 = 既定へリセット)。他項目を壊さないよう load→変更→save。
    auto p = AppPreferences::load();
    p.lastProjectLocation = path;
    p.save();
}

void StartupComponent::chooseLocation()
{
    juce::File current(locationEditor.getText());
    if (!current.isDirectory())
        current = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory);

    fileChooser = std::make_unique<juce::FileChooser>(
        tr(u8"保存先フォルダを選択"), current, "*", true);

    fileChooser->launchAsync(juce::FileBrowserComponent::openMode
                            | juce::FileBrowserComponent::canSelectDirectories,
        [this](const juce::FileChooser& fc)
        {
            auto f = fc.getResult();
            if (f.isDirectory())
            {
                locationEditor.setText(f.getFullPathName(), juce::dontSendNotification);
                persistProjectLocation(f.getFullPathName());   // 選んだ保存先を記憶 (次回起動も維持)
            }
        });
}

void StartupComponent::browseExisting()
{
    juce::File startDir = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
                              .getChildFile("Utawave");
    if (!startDir.isDirectory())
        startDir = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory);

    fileChooser = std::make_unique<juce::FileChooser>(
        // 新拡張子のみ (ProjectManager::openWildcard と同義。include を増やさないため直書き)
        tr(u8"プロジェクトを開く"), startDir, "*.uta", true);

    fileChooser->launchAsync(juce::FileBrowserComponent::openMode
                            | juce::FileBrowserComponent::canSelectFiles,
        [this](const juce::FileChooser& fc)
        {
            auto f = fc.getResult();
            if (f.existsAsFile() && onProjectChosen)
            {
                startupDeviceManager.closeAudioDevice();
                onProjectChosen(f, 0.0, 0, false);
            }
        });
}

void StartupComponent::createNewProject()
{
    auto name = nameEditor.getText().trim();
    if (name.isEmpty()) name = "Untitled";
    name = juce::File::createLegalFileName(name);

    juce::File loc(locationEditor.getText().trim());
    if (!loc.isDirectory())
    {
        if (!loc.createDirectory())
        {
            juce::AlertWindow::showAsync(juce::MessageBoxOptions()
                .withIconType(juce::MessageBoxIconType::WarningIcon)
                .withTitle(tr(u8"保存先エラー"))
                .withMessage(tr(u8"保存先フォルダを作成できませんでした"))
                .withButton("OK"), nullptr);
            return;
        }
    }

    auto projectDir  = loc.getChildFile(name);
    auto projectFile = projectDir.getChildFile(name + ".uta");

    if (projectDir.exists() && !projectDir.isDirectory())
    {
        juce::AlertWindow::showAsync(juce::MessageBoxOptions()
            .withIconType(juce::MessageBoxIconType::WarningIcon)
            .withTitle(tr(u8"作成できません"))
            .withMessage(tr(u8"同名のファイルが既に存在します"))
            .withButton("OK"), nullptr);
        return;
    }
    if (projectFile.existsAsFile())
    {
        juce::AlertWindow::showAsync(juce::MessageBoxOptions()
            .withIconType(juce::MessageBoxIconType::QuestionIcon)
            .withTitle(tr(u8"既存のプロジェクト"))
            .withMessage(tr(u8"同名のプロジェクトが既に存在します。\n別の名前を指定してください。"))
            .withButton("OK"), nullptr);
        return;
    }

    projectDir.createDirectory();
    projectDir.getChildFile("Audio").createDirectory();
    projectDir.getChildFile("Cache").createDirectory();

    double sr = (double)juce::jmax(1, sampleRateBox.getSelectedId());
    int    bd = juce::jmax(16, bitDepthBox.getSelectedId());

    // 手入力の保存先も次回へ引き継ぐ (既定と同じなら記憶せず既定扱いのまま)。
    if (loc == defaultProjectLocation())
        persistProjectLocation({});
    else
        persistProjectLocation(loc.getFullPathName());

    if (onProjectChosen)
    {
        startupDeviceManager.closeAudioDevice();
        onProjectChosen(projectFile, sr, bd, true);
    }
}

int StartupComponent::getNumRows()
{
    return recents.size();
}

void StartupComponent::paintListBoxItem(int rowNumber, juce::Graphics& g,
                                        int width, int height, bool rowIsSelected)
{
    if (rowNumber < 0 || rowNumber >= recents.size()) return;
    auto& f = recents.getReference(rowNumber);

    if (rowIsSelected)
        g.fillAll(juce::Colour(0xff2a78ff).withAlpha(0.2f));

    g.setColour(juce::Colours::white);
    g.setFont(juce::FontOptions(13.0f, juce::Font::bold));
    g.drawText(f.getFileNameWithoutExtension(),
               12, 4, width - 24, 20,
               juce::Justification::centredLeft, true);

    g.setColour(juce::Colour(0xff8a8f95));
    g.setFont(juce::FontOptions(10.5f));
    g.drawText(f.getParentDirectory().getFullPathName(),
               12, 24, width - 24, 16,
               juce::Justification::centredLeft, true);
}

void StartupComponent::listBoxItemDoubleClicked(int row, const juce::MouseEvent&)
{
    if (row < 0 || row >= recents.size()) return;
    auto f = recents.getReference(row);
    if (!f.existsAsFile())
    {
        RecentProjects::remove(f);
        refreshRecents();
        return;
    }
    if (onProjectChosen)
    {
        startupDeviceManager.closeAudioDevice();
        onProjectChosen(f, 0.0, 0, false);
    }
}

void StartupComponent::refreshDeviceLabel()
{
    auto s = AudioDeviceSettings::getDeviceSummary(startupDeviceManager);
    deviceSummaryLabel.setText(s.isEmpty()
                                   ? tr(u8"未選択")
                                   : s,
                               juce::dontSendNotification);
}

void StartupComponent::showDeviceDialog()
{
    auto* sel = new juce::AudioDeviceSelectorComponent(
        startupDeviceManager, 0, 2, 2, 2, false, false, false, false);
    sel->setSize(480, 380);

    juce::DialogWindow::LaunchOptions opts;
    opts.content.setOwned(sel);
    opts.dialogTitle                  = tr(u8"オーディオI/F の設定");
    opts.dialogBackgroundColour       = juce::Colour(0xff1a1a1a);
    opts.escapeKeyTriggersCloseButton = true;
    opts.useNativeTitleBar            = true;
    opts.resizable                    = true;
    opts.launchAsync();
}

void StartupComponent::populateSampleRateBox()
{
    // 現在のデバイスが対応する SR のみを列挙する。手動選択が残っていれば控えて復元する。
    const int keep = srUserPicked ? sampleRateBox.getSelectedId() : 0;

    juce::Array<double> rates;
    if (auto* dev = startupDeviceManager.getCurrentAudioDevice())
        rates = dev->getAvailableSampleRates();
    if (rates.isEmpty())   // デバイス無し/取得不可はフォールバックの標準リスト
        rates = { 44100.0, 48000.0, 88200.0, 96000.0, 192000.0 };

    sampleRateBox.clear(juce::dontSendNotification);
    for (auto r : rates)
    {
        const int sr = (int) r;
        if (sr > 0 && sampleRateBox.indexOfItemId(sr) < 0)
            sampleRateBox.addItem(juce::String(sr) + " Hz", sr);
    }

    // 手動選択が新しいリストにも在れば維持、無ければ既定を選び直す (前回SR→デバイスSR→先頭)。
    if (keep > 0 && sampleRateBox.indexOfItemId(keep) >= 0)
    {
        sampleRateBox.setSelectedId(keep, juce::dontSendNotification);
    }
    else
    {
        srUserPicked = false;          // 選べなくなった手動選択は破棄
        syncSampleRateBoxToDevice();   // 前回SR / デバイスSR を既定に
        if (sampleRateBox.getSelectedId() <= 0 && sampleRateBox.getNumItems() > 0)
            sampleRateBox.setSelectedId(sampleRateBox.getItemId(0), juce::dontSendNotification);  // 先頭
    }
}

void StartupComponent::syncSampleRateBoxToDevice()
{
    if (srUserPicked) return;   // 手動選択を尊重
    // 優先: 前回作成したプロジェクトの SR (引き継ぎ) → 無ければデバイスの現在 SR。
    int sr = (int) lastProjectSrPref;
    if (sr <= 0)
    {
        double devSr = startupDeviceManager.getAudioDeviceSetup().sampleRate;
        if (auto* dev = startupDeviceManager.getCurrentAudioDevice())
            if (dev->getCurrentSampleRate() > 0.0) devSr = dev->getCurrentSampleRate();
        sr = (int) devSr;
    }
    if (sr <= 0) return;
    // コンボに該当 SR がある時だけ既定として選ぶ (無ければ現状維持)。onChange が
    // srUserPicked を立てないよう dontSendNotification で設定する。
    for (int i = 0; i < sampleRateBox.getNumItems(); ++i)
        if (sampleRateBox.getItemId(i) == sr)
        {
            sampleRateBox.setSelectedId(sr, juce::dontSendNotification);
            return;
        }
}

void StartupComponent::changeListenerCallback(juce::ChangeBroadcaster*)
{
    AudioDeviceSettings::saveState(startupDeviceManager);
    refreshDeviceLabel();
    populateSampleRateBox();   // デバイス変更で対応 SR リストと既定を更新 (手動選択は可能なら維持)
}
