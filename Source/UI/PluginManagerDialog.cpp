// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

#include "PluginManagerDialog.h"
#include "../Localisation.h"

PluginManagerDialog::PluginManagerDialog(PluginManager& mgr)
    : pm(mgr),
      progressBar(progressValue)
{
    titleLabel.setText(tr(u8"プラグイン管理"), juce::dontSendNotification);
    titleLabel.setFont(juce::FontOptions(16.0f, juce::Font::bold));
    titleLabel.setColour(juce::Label::textColourId, juce::Colours::white);
    addAndMakeVisible(titleLabel);

    statusLabel.setText("", juce::dontSendNotification);
    statusLabel.setFont(juce::FontOptions(11.0f));
    statusLabel.setColour(juce::Label::textColourId, juce::Colour(0xff9aa0a6));
    statusLabel.setMinimumHorizontalScale(0.7f);
    addAndMakeVisible(statusLabel);

    progressBar.setColour(juce::ProgressBar::backgroundColourId, juce::Colour(0xff242830));
    progressBar.setColour(juce::ProgressBar::foregroundColourId, juce::Colour(0xff2a78ff));
    addAndMakeVisible(progressBar);

    auto styleBtn = [](juce::TextButton& b)
    {
        b.setColour(juce::TextButton::buttonColourId,   juce::Colour(0xff3a3a3a));
        b.setColour(juce::TextButton::textColourOffId,  juce::Colours::white);
        b.setColour(juce::TextButton::textColourOnId,   juce::Colours::white);
    };
    styleBtn(scanVST3Btn);
    styleBtn(rescanAllBtn);
    styleBtn(cancelBtn);
    styleBtn(clearBtn);
    styleBtn(removeSelBtn);
    styleBtn(pathsBtn);
    rescanAllBtn.setTooltip(juce::String::fromUTF8(
        u8"既知のプラグインも含めて全て再スキャンします (バージョンアップ後の認識更新用)"));

    cancelBtn.setColour(juce::TextButton::buttonColourId, juce::Colour(0xffc44a4a));
    cancelBtn.setVisible(false);

    addAndMakeVisible(scanVST3Btn);
    addAndMakeVisible(rescanAllBtn);
    addAndMakeVisible(cancelBtn);
    addAndMakeVisible(clearBtn);
    addAndMakeVisible(removeSelBtn);
    addAndMakeVisible(pathsBtn);

    scanVST3Btn.onClick = [this]
    {
        for (int i = 0; i < pm.getFormatManager().getNumFormats(); ++i)
            if (auto* f = pm.getFormatManager().getFormat(i); f && f->getName() == "VST3")
                { startScan(*f, /*forceRescan*/ false); return; }
    };
    rescanAllBtn.onClick = [this]
    {
        // 既知プラグインも含め VST3 を強制再スキャン (バージョンアップ反映用)。
        for (int i = 0; i < pm.getFormatManager().getNumFormats(); ++i)
            if (auto* f = pm.getFormatManager().getFormat(i); f && f->getName() == "VST3")
                { startScan(*f, /*forceRescan*/ true); return; }
    };
    cancelBtn.onClick = [this] { cancelScan(); };
    clearBtn.onClick   = [this]
    {
        pm.getKnownPluginListRW().clear();
        pm.save();
        refreshTable();
    };
    removeSelBtn.onClick = [this]
    {
        auto rows = table.getSelectedRows();
        if (rows.isEmpty()) return;
        // sortedTypes は表示順なので、選択された行 index に対応する PluginDescription を集める
        std::vector<juce::PluginDescription> toRemove;
        for (int i = 0; i < (int)sortedTypes.size(); ++i)
            if (rows.contains(i))
                toRemove.push_back(sortedTypes.getReference(i));
        for (auto& d : toRemove)
            pm.getKnownPluginListRW().removeType(d);
        pm.save();
        refreshTable();
    };
    pathsBtn.onClick = [this]
    {
        // 簡易: 最初に VST3 を選んでパス編集
        for (int i = 0; i < pm.getFormatManager().getNumFormats(); ++i)
            if (auto* f = pm.getFormatManager().getFormat(i); f && f->getName() == "VST3")
                { promptForPath(*f); return; }
    };

    table.setColour(juce::TableListBox::backgroundColourId,     juce::Colour(0xff1f2125));
    table.setColour(juce::TableListBox::outlineColourId,        juce::Colour(0xff3a3d42));
    table.setColour(juce::TableListBox::textColourId,           juce::Colours::white);
    table.setOutlineThickness(1);
    auto& h = table.getHeader();
    const int sortableFlags = juce::TableHeaderComponent::defaultFlags;
    h.addColumn(tr(u8"名前"),         1, 220, 100, 600,
                sortableFlags | juce::TableHeaderComponent::sortedForwards);
    h.addColumn(tr(u8"フォーマット"), 2,  80,  80, 100, sortableFlags);
    h.addColumn(tr(u8"カテゴリ"),     3, 120, 100, 200, sortableFlags);
    h.addColumn(tr(u8"メーカー"),     4, 160, 100, 300, sortableFlags);
    addAndMakeVisible(table);

    setSize(800, 540);
    startTimerHz(20);
    refreshTable();
}

PluginManagerDialog::~PluginManagerDialog()
{
    if (scanThread)
    {
        scanThread->signalThreadShouldExit();
        // 別プロセススキャナの応答待ちを起こす (50ms 以内に空結果で抜ける)。これを呼ばないと
        // 重い/ハングした子プロセススキャン中はスレッドが exit フラグを見られず下の wait が
        // タイムアウトし、走行中スレッドの真下で scanner (PluginDirectoryScanner) が破棄されて
        // 応答待ち中の Subprocess (mutex/condvar) ごと解放 = UAF クラッシュだった
        // (クラッシュレポート id19/id32・MSVCP140 条件変数内部で read AV)
        pm.abortOutOfProcessScan();
        scanThread->waitForThreadToExit(10000);
        scanThread.reset();
        pm.save();
    }
    stopTimer();
}

void PluginManagerDialog::startScan(juce::AudioPluginFormat& fmt, bool forceRescan)
{
    cancelScan();   // 念のため
    // 前回の停止/破棄で abort されたままだと全プラグインが空結果になるため必ず戻す
    // (PluginManager::startScan と同じ作法)
    pm.resetOutOfProcessScanAbortFlag();
    auto paths = pm.getSearchPathsForFormat(fmt.getName());
    scanThread = std::make_unique<ScanThread>(*this, fmt, paths,
                                              PluginManager::getDeadMansPedalFile(),
                                              forceRescan);
    scanThread->startThread();

    scanVST3Btn .setEnabled(false);
    rescanAllBtn.setEnabled(false);
    clearBtn    .setEnabled(false);
    removeSelBtn.setEnabled(false);
    pathsBtn    .setEnabled(false);
    cancelBtn   .setVisible(true);

    statusLabel.setText(forceRescan
        ? tr(u8"全プラグイン再スキャン開始...")
        : tr(u8"スキャン開始..."),
        juce::dontSendNotification);
}

void PluginManagerDialog::cancelScan()
{
    if (scanThread != nullptr && scanThread->isThreadRunning())
    {
        // 協調キャンセル: フラグを立てて自然終了を待つ (強制 kill はプラグイン破棄中の
        // クラッシュリスクがあるため使わない)。別プロセススキャナの応答待ちも起こすことで、
        // 重い/ハングした子スキャン中でも 50ms 以内に現在のプラグインを空結果で抜けて
        // すぐ止まる (中断したプラグインはブラックリスト化されず次回スキャンで再走査される)
        scanThread->signalThreadShouldExit();
        pm.abortOutOfProcessScan();
        cancelBtn.setEnabled(false);
        statusLabel.setText(juce::String::fromUTF8(
                u8"停止中... 現在のプラグイン処理完了を待っています（数秒かかります）"),
            juce::dontSendNotification);
        // 後処理は timerCallback で thread 終了を検知して行う
        return;
    }
    // スキャン中でなければボタン状態だけ戻す
    scanVST3Btn .setEnabled(true);
    rescanAllBtn.setEnabled(true);
    clearBtn    .setEnabled(true);
    removeSelBtn.setEnabled(true);
    pathsBtn    .setEnabled(true);
    cancelBtn   .setVisible(false);
    cancelBtn   .setEnabled(true);
    statusLabel.setText("", juce::dontSendNotification);
    progressValue = 0.0;
    refreshTable();
}

void PluginManagerDialog::refreshTable()
{
    resortTypes();
    table.updateContent();
    table.repaint();
}

void PluginManagerDialog::resortTypes()
{
    sortedTypes = pm.getKnownPluginList().getTypes();
    auto key = [this](const juce::PluginDescription& d) -> juce::String
    {
        switch (sortColumn)
        {
            case 2: return d.pluginFormatName;
            case 3: return d.category;
            case 4: return d.manufacturerName;
            case 1:
            default: return d.name;
        }
    };
    // juce::Array はソート可能だが std::sort と互換 — Array::data でアクセス
    std::sort(sortedTypes.begin(), sortedTypes.end(),
        [this, &key](const juce::PluginDescription& a, const juce::PluginDescription& b)
        {
            const auto cmp = key(a).compareIgnoreCase(key(b));
            if (cmp != 0) return sortForwards ? (cmp < 0) : (cmp > 0);
            return sortForwards ? (a.name.compareIgnoreCase(b.name) < 0)
                                : (a.name.compareIgnoreCase(b.name) > 0);
        });
}

void PluginManagerDialog::sortOrderChanged(int newSortColumnId, bool isForwards)
{
    sortColumn   = newSortColumnId;
    sortForwards = isForwards;
    resortTypes();
    table.updateContent();
    table.repaint();
}

void PluginManagerDialog::timerCallback()
{
    if (scanThread)
    {
        juce::String s;
        {
            const juce::ScopedLock sl(statusLock);
            s = currentStatus;
        }
        statusLabel.setText(s.isEmpty() ? tr(u8"スキャン中...")
                                       : tr(u8"検証中: ") + s,
                            juce::dontSendNotification);

        if (! scanThread->isThreadRunning())
        {
            scanThread.reset();
            pm.save();
            scanVST3Btn .setEnabled(true);
            rescanAllBtn.setEnabled(true);
            clearBtn    .setEnabled(true);
            removeSelBtn.setEnabled(true);
            pathsBtn    .setEnabled(true);
            cancelBtn   .setVisible(false);
            statusLabel.setText(tr(u8"スキャン完了"), juce::dontSendNotification);
            progressValue = 0.0;
            refreshTable();
        }
    }
}

juce::FileSearchPath PluginManagerDialog::promptForPath(juce::AudioPluginFormat& fmt)
{
    auto current = pm.getSearchPathsForFormat(fmt.getName());

    // 単純に 1 つのフォルダだけ追加する簡易 UI
    fileChooser = std::make_unique<juce::FileChooser>(
        tr(u8"スキャンするフォルダを追加"),
        juce::File::getSpecialLocation(juce::File::userHomeDirectory),
        "*", true);
    fileChooser->launchAsync(juce::FileBrowserComponent::openMode
                            | juce::FileBrowserComponent::canSelectDirectories,
        [this, &fmt](const juce::FileChooser& fc)
        {
            auto f = fc.getResult();
            if (!f.isDirectory()) return;
            auto paths = pm.getSearchPathsForFormat(fmt.getName());
            paths.add(f);
            paths.removeRedundantPaths();
            pm.setSearchPathsForFormat(fmt.getName(), paths);
        });
    return current;
}

// ──── レイアウト / 描画 ──────────────────────────────────────────────────

void PluginManagerDialog::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xff181a1e));
}

void PluginManagerDialog::resized()
{
    auto r = getLocalBounds().reduced(14);
    titleLabel.setBounds(r.removeFromTop(24));
    r.removeFromTop(8);

    auto btnRow = r.removeFromTop(28);
    scanVST3Btn .setBounds(btnRow.removeFromLeft(140));
    btnRow.removeFromLeft(6);
    rescanAllBtn.setBounds(btnRow.removeFromLeft(120));
    btnRow.removeFromLeft(12);
    cancelBtn   .setBounds(btnRow.removeFromLeft(90));
    btnRow.removeFromLeft(12);
    pathsBtn    .setBounds(btnRow.removeFromLeft(110));
    btnRow.removeFromLeft(6);
    removeSelBtn.setBounds(btnRow.removeFromLeft(110));
    btnRow.removeFromLeft(6);
    clearBtn    .setBounds(btnRow.removeFromLeft(140));
    r.removeFromTop(8);

    auto statusRow = r.removeFromTop(22);
    statusLabel .setBounds(statusRow.removeFromLeft(getWidth() / 2));
    statusRow.removeFromLeft(8);
    progressBar.setBounds(statusRow);
    r.removeFromTop(6);

    table.setBounds(r);
}

// ──── TableListBoxModel ─────────────────────────────────────────────────

int PluginManagerDialog::getNumRows()
{
    return (int)sortedTypes.size();
}

void PluginManagerDialog::paintRowBackground(juce::Graphics& g, int rowNumber, int width, int height, bool rowIsSelected)
{
    if (rowIsSelected)
        g.fillAll(juce::Colour(0xff2a78ff).withAlpha(0.25f));
    else if (rowNumber % 2)
        g.fillAll(juce::Colour(0xff22252a));

    juce::ignoreUnused(width, height);
}

void PluginManagerDialog::paintCell(juce::Graphics& g, int rowNumber, int columnId,
                                    int width, int height, bool /*rowIsSelected*/)
{
    if (rowNumber < 0 || rowNumber >= sortedTypes.size()) return;
    const auto& d = sortedTypes.getReference(rowNumber);

    g.setColour(juce::Colours::white);
    g.setFont(juce::FontOptions(12.0f));
    juce::String text;
    switch (columnId)
    {
        case 1: text = d.name; break;
        case 2: text = d.pluginFormatName; break;
        case 3: text = d.category; break;
        case 4: text = d.manufacturerName; break;
        default: break;
    }
    g.drawText(text, 6, 0, width - 12, height, juce::Justification::centredLeft, true);
}

void PluginManagerDialog::deleteKeyPressed(int /*lastRowSelected*/)
{
    removeSelBtn.onClick();
}
