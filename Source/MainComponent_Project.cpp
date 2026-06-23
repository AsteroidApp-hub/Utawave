// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

// MainComponent のプロジェクトファイル管理 (保存/読込/自動保存/フォルダ管理) 実装。
// MainComponent.cpp が肥大化したため分割。

#include "MainComponent.h"
#include "Localisation.h"
#include "Project/RecentProjects.h"
#include "Project/BackupManager.h"
#include "VST/PluginChain.h"

// ── 自動保存 / 世代バックアップ ──────────────────────────────────────
juce::File MainComponent::getBackupDir() const
{
    juce::File dir;
    if (currentProjectFile.existsAsFile())
        dir = currentProjectFile.getParentDirectory().getChildFile("Backup");
    else if (untitledProjectDir != juce::File() && untitledProjectDir.isDirectory())
        dir = untitledProjectDir.getChildFile("Backup");
    else
        return {};
    dir.createDirectory();
    return dir;
}

juce::String MainComponent::backupBaseName() const
{
    // 保存済み = プロジェクト名(拡張子なし) / 未保存(Untitled) = 空文字。
    // 命名・列挙・間引き・復旧判定のロジックは BackupManager に集約している。
    return currentProjectFile.existsAsFile()
             ? currentProjectFile.getFileNameWithoutExtension()
             : juce::String();
}

void MainComponent::restartAutoSaveTimer()
{
    if (!autoSaveTimer) return;
    autoSaveTimer->stopTimer();
    const int mins = appSettings.autoSaveIntervalMinutes;
    if (mins > 0)
        autoSaveTimer->startTimer(mins * 60 * 1000);
}

void MainComponent::performAutoSave()
{
    if (!projectDirty) return;
    if (isRecording)  return;   // 録音中の I/O は避ける
    if (appSettings.autoSaveIntervalMinutes <= 0) return;

    // 保存用 State を一度だけ構築し、バックアップ保存と本体保存で共用する
    ProjectManager::State s;
    s.trackManager   = &trackManager;
    s.appSettings    = &appSettings;
    auto markers     = timelineView.getMarkers();
    s.markers        = const_cast<std::vector<Marker>*>(&markers);
    s.bpm            = &bpm;
    s.loopStartSecs  = &loopStartSecs;
    s.loopEndSecs    = &loopEndSecs;
    s.loopActive     = &loopActive;
    s.playheadSecs   = &playPosition;
    s.pluginManager  = &pluginManager;
    s.masterChain    = &audioEngine.getMasterChain();
    double autoPpb   = timelineView.getHorizontalZoom();
    s.pixelsPerBeat  = &autoPpb;

    // 1) 日時付きの世代バックアップを Backup/ に保存（本体保存より先に書く）。
    //    本体保存の途中でクラッシュした時だけ最新バックアップが本体より新しくなり、
    //    次回起動の復旧条件（最新バックアップ > 本体）を正しく満たすための順序。
    //    保存後に古い世代を maxBackups 個まで間引く。失敗は黙って次回再挑戦。
    bool savedAny = false;
    if (auto dir = getBackupDir(); dir != juce::File())
    {
        auto bak = BackupManager::datedFile(dir, backupBaseName(), juce::Time::getCurrentTime());
        if (ProjectManager::save(bak, s))
        {
            BackupManager::prune(dir, backupBaseName(), appSettings.maxBackups);
            savedAny = true;
        }
    }

    // 2) 保存済みプロジェクトなら本体ファイルも自動保存する。
    //    = 時間が来たら勝手に Cmd+S を押すのと同じ。保存し忘れによるデータ損失を防ぐ。
    //    バックアップ自体は消さず残す（クラッシュ時の保険）。未保存(Untitled)は
    //    実ファイルが無いのでバックアップのみ（Save As ダイアログは勝手に出さない）。
    if (currentProjectFile.existsAsFile())
    {
        if (ProjectManager::save(currentProjectFile, s))
        {
            clearProjectDirty();
            savedAny = true;
        }
    }

    // 自動保存できたことが分かるようステータスバーに通知 (手動保存の「保存しました」と同じ場所)
    if (savedAny)
        statusBar.setMessage(tr(u8"自動保存しました"), 3000);
}

void MainComponent::offerAutoSaveRecoveryIfNeeded(const juce::File& projectFile)
{
    // 最新の世代バックアップが本体ファイルより新しければ復旧を提案する。
    auto dir = getBackupDir();
    if (! BackupManager::shouldOfferRecovery(dir, backupBaseName(), projectFile))
        return;   // 本体が新しい / バックアップ無し = 提案しない。世代は履歴として残す（消さない）
    auto newest = BackupManager::newest(dir, backupBaseName());

    juce::AlertWindow::showAsync(
        juce::MessageBoxOptions()
            .withIconType(juce::MessageBoxIconType::QuestionIcon)
            .withTitle(tr(u8"自動保存から復旧"))
            .withMessage(tr(u8"前回終了時に未保存の変更が見つかりました。\n自動保存ファイルから復旧しますか?"))
            .withButton(tr(u8"復旧"))
            .withButton(tr(u8"破棄")),
        [this, projectFile, newest](int result)
        {
            if (result == 1)
            {
                // 最新バックアップを読み込み、本体ファイルへ保存して確定する。
                // 本体が最新になるので再プロンプトは出ない。世代バックアップは履歴として残す。
                if (loadProjectFrom(newest, /*isRecovery=*/true))
                {
                    currentProjectFile = projectFile;
                    saveProjectTo(projectFile, /*announce=*/false);  // 復旧の内部保存は通知しない
                    restartAutoSaveTimer();
                }
            }
            else
            {
                // 破棄: 本体ファイルの内容を維持。再プロンプト防止に更新時刻だけ現在へ進める
                // （世代バックアップは消さず履歴として残す）。
                projectFile.setLastModificationTime(juce::Time::getCurrentTime());
            }
        });
}

// ── プロジェクトフォルダ ────────────────────────────────────────────
juce::File MainComponent::getProjectFolder() const
{
    if (currentProjectFile.existsAsFile())
        return currentProjectFile.getParentDirectory();
    return untitledProjectDir;
}

juce::File MainComponent::getProjectAudioFolder() const
{
    auto f = getProjectFolder().getChildFile("Audio");
    f.createDirectory();
    return f;
}

juce::File MainComponent::getProjectCacheFolder() const
{
    auto f = getProjectFolder().getChildFile("Cache");
    f.createDirectory();
    return f;
}

juce::File MainComponent::getProjectExportFolder() const
{
    auto f = getProjectFolder().getChildFile("Export");
    f.createDirectory();
    return f;
}

void MainComponent::migrateAudioToProjectFolder(const juce::File& newProjectFile)
{
    auto newProjectDir = newProjectFile.getParentDirectory();
    auto newAudio = newProjectDir.getChildFile("Audio");
    auto newCache = newProjectDir.getChildFile("Cache");
    newAudio.createDirectory();
    newCache.createDirectory();

    // 録音/再生を停止して読み込みハンドルを解放
    bool wasPlaying = isPlaying;
    if (wasPlaying) stopTransport();
    audioEngine.clearPlayback();

    auto migrate = [&](AudioClip* clip) {
        auto src = clip->getFile();
        if (!src.existsAsFile()) return;
        // 既に新プロジェクトフォルダ配下なら何もしない
        if (src.isAChildOf(newProjectDir)) return;

        // Audio / Cache どちらに入れるか: 元ファイル名に "_<sr>Hz_" のパターンがあれば Cache、それ以外 Audio
        bool isCache = src.getFileName().containsIgnoreCase("Hz_");
        auto destDir = isCache ? newCache : newAudio;
        auto dest = destDir.getChildFile(src.getFileName());
        // 同名ファイルが既にあれば連番をふる
        for (int i = 1; dest.existsAsFile() && !dest.hasIdenticalContentTo(src); ++i)
        {
            dest = destDir.getChildFile(src.getFileNameWithoutExtension()
                                        + "_" + juce::String(i)
                                        + "." + src.getFileExtension().substring(1));
        }
        if (!dest.existsAsFile())
            src.copyFileTo(dest);
        clip->setFile(dest);
    };

    for (int ti = 0; ti < trackManager.getTrackCount(); ++ti)
    {
        auto* track = trackManager.getTrack(ti);
        for (int li = 0; li < track->getLaneCount(); ++li)
        {
            auto* lane = track->getLane(li);
            if (!lane) continue;
            for (auto& cp : lane->clips)
                if (cp) migrate(cp.get());
        }
    }
}

// ── 保存 ────────────────────────────────────────────────────────────
bool MainComponent::saveProjectTo(const juce::File& f, bool announce)
{
    // 保存先が現在のプロジェクトと異なる場合は、Audio/Cache を新フォルダへ移行
    const bool migrated = (currentProjectFile != f);
    if (migrated)
        migrateAudioToProjectFolder(f);

    ProjectManager::State s;
    s.trackManager   = &trackManager;
    s.appSettings    = &appSettings;
    auto markers     = timelineView.getMarkers();
    s.markers        = const_cast<std::vector<Marker>*>(&markers);
    s.bpm            = &bpm;
    s.loopStartSecs  = &loopStartSecs;
    s.loopEndSecs    = &loopEndSecs;
    s.loopActive     = &loopActive;
    s.playheadSecs   = &playPosition;
    s.pluginManager  = &pluginManager;
    s.masterChain    = &audioEngine.getMasterChain();
    double currentPpb = timelineView.getHorizontalZoom();
    s.pixelsPerBeat  = &currentPpb;
    if (!ProjectManager::save(f, s))
    {
        juce::AlertWindow::showAsync(juce::MessageBoxOptions()
            .withIconType(juce::MessageBoxIconType::WarningIcon)
            .withTitle(tr(u8"保存失敗"))
            .withMessage(tr(u8"プロジェクトの保存に失敗しました: ") + f.getFullPathName())
            .withButton("OK"), nullptr);
        return false;
    }
    currentProjectFile = f;
    clearProjectDirty();
    // 世代バックアップは履歴として残す（手動保存で本体の更新時刻が最新になるため、
    // 次回起動の復旧プロンプトは出ない）。
    // 波形ディスクキャッシュを保存 → 次回読み込み時に WAV デコードをスキップできる。
    trackManager.saveThumbnailCache(getProjectCacheFolder().getChildFile("thumbnails.bin"),
                                    getProjectAudioFolder());
    // 通常の Save では preparePlayback を呼ばない（重い再準備で音が一瞬途切れる）。
    // migrate で clearPlayback された場合のみ再準備する。
    if (migrated)
        audioEngine.preparePlayback(trackManager);
    timelineView.refresh();
    RecentProjects::add(f);
    // 保存できたことが分かるようステータスバーに通知 (波形読み込み完了と同じ場所)。
    // 明示保存 (Cmd+S / 別名で保存 / 閉じる確認の「保存」) のときだけ出す。
    if (announce)
        statusBar.setMessage(tr(u8"保存しました"), 3000);
    return true;
}

// ── 読み込み ────────────────────────────────────────────────────────
bool MainComponent::loadProjectFrom(const juce::File& f, bool isRecovery)
{
    if (isPlaying) stopTransport();
    audioEngine.clearPlayback();
    // 別プロジェクトの読み込みは既存クリップ/トラックを全破棄するため、それらを指す Undo 履歴を
    // 破棄する (旧クリップへのダングリング = use-after-free を防ぐ。オーディオ/MIDI 両方に有効)。
    undoManager.clearUndoHistory();
    pluginEditorWindows.clear();   // 既存プラグインエディタを閉じる（古い Plugin が破棄される前に）
    // 既存マスターチェーンをクリア（新規読み込み前に古いプラグインを破棄）
    while (audioEngine.getMasterChain().getNumPlugins() > 0)
        audioEngine.getMasterChain().removePlugin(0);

    ProjectManager::State s;
    s.trackManager   = &trackManager;
    s.appSettings    = &appSettings;
    std::vector<Marker> markers;
    s.markers        = &markers;
    s.bpm            = &bpm;
    s.loopStartSecs  = &loopStartSecs;
    s.loopEndSecs    = &loopEndSecs;
    s.loopActive     = &loopActive;
    s.playheadSecs   = &playPosition;
    s.pluginManager  = &pluginManager;
    s.pluginSampleRate = audioEngine.getSampleRate() > 0 ? audioEngine.getSampleRate() : 48000.0;
    s.pluginBlockSize  = 512;
    s.masterChain    = &audioEngine.getMasterChain();
    double loadedPpb = timelineView.getHorizontalZoom();
    s.pixelsPerBeat  = &loadedPpb;
    std::vector<juce::String> missingFiles;
    s.missingFiles   = &missingFiles;
    // クリップ生成前に波形ディスクキャッシュを読み込む。各クリップの setSource が
    // キャッシュヒットすれば WAV デコードを省略でき、波形表示が爆速になる。
    trackManager.loadThumbnailCache(
        f.getParentDirectory().getChildFile("Cache").getChildFile("thumbnails.bin"),
        f.getParentDirectory().getChildFile("Audio"));
    if (!ProjectManager::load(f, s))
    {
        juce::AlertWindow::showAsync(juce::MessageBoxOptions()
            .withIconType(juce::MessageBoxIconType::WarningIcon)
            .withTitle(tr(u8"読み込み失敗"))
            .withMessage(tr(u8"プロジェクトを読み込めませんでした: ") + f.getFullPathName())
            .withButton("OK"), nullptr);
        return false;
    }
    currentProjectFile = f;
    clearProjectDirty();

    // UI と AudioEngine に反映
    toolbar.setBpm(bpm);
    timelineView.setBpm(bpm);
    timelineView.setMarkers(markers);
    timelineView.setLoopRange(loopStartSecs, loopEndSecs, loopActive);
    timelineView.setAppSettings(appSettings);
    timelineView.setPlayheadPosition(playPosition);
    timelineView.setHorizontalZoom(loadedPpb);  // 保存されていたタイムライン横ズームを復元
    audioEngine.setPosition(playPosition);
    audioEngine.setMetronomeBpm(bpm);
    audioEngine.setMetronomeBeatsPerBar(appSettings.meterNumerator);
    audioEngine.setLoopRange(loopStartSecs, loopEndSecs, loopActive);
    audioEngine.setAppSettings(appSettings);
    toolbar.setLoopActive(loopActive);
    toolbar.setCountInBars(appSettings.countInBars);
    toolbar.setPreRollSecs(appSettings.preRollSecs);
    // GRID 表示も復元した snapMode に同期 (スナップは効くのに表示が Off のままになるのを防ぐ)
    syncSnapLabelToSettings();
    trackHeaderPanel.refresh();
    timelineView.refresh();
    audioEngine.preparePlayback(trackManager);
    // 読み込んだ CLICK トラックの状態をメトロノームへ反映 (無ければ停止)
    syncClickTrackToEngine();
    statusBar.setTrackCount(trackManager.getTrackCount());
    // 読み込んだ insertSlotsVisible 状態に合わせてヘッダ幅を再計算
    resized();
    // マスターINSスロット表示状態 / 折りたたみ状態 / 時刻行・小節行表示も復元
    masterPanel.setInsertSlotsVisible(appSettings.masterInsertSlotsVisible);
    masterPanel.setCollapsed(appSettings.masterPanelCollapsed);
    masterPanel.setVuReferenceLevel(appSettings.vuReferenceLevel);
    trackHeaderPanel.setVuReferenceLevel(appSettings.vuReferenceLevel);
    trackHeaderPanel.setLoudnessTargetLufs(appSettings.loudnessTargetLufs);
    timelineView.getRuler().setTimeRowVisible(appSettings.rulerTimeRowVisible);
    timelineView.getRuler().setBarsRowVisible(appSettings.rulerBarsRowVisible);
    trackHeaderPanel.setRulerHeight(timelineView.getRuler().getDesiredHeight());
    timelineView.resized();
    resized();
    if (!isRecovery)
    {
        RecentProjects::add(f);
        // 自動保存の間隔がプロジェクトごとに違う場合があるので再起動
        restartAutoSaveTimer();
        // 復旧候補があれば確認ダイアログ
        offerAutoSaveRecoveryIfNeeded(f);
    }
    // オーディオデバイスをプロジェクトのサンプリングレートに追従させる
    applyProjectSampleRateToDevice();
    // 録音 WAV のビット深度もプロジェクトに合わせる
    recordingMgr.setBitDepth(appSettings.projectBitDepth);
    // ステータスバー表示もプロジェクト設定に追従
    statusBar.setSampleRate((int) appSettings.projectSampleRate);
    statusBar.setBitDepth(appSettings.projectBitDepth);
    // AudioThumbnail の非同期ロード完了を待って波形を再描画。
    // デコードが走った場合は updateWaveformLoadingStatus() の完了時にディスクキャッシュへ保存する。
    scheduleWaveformRefresh();

    // 欠損ファイルがあれば警告ダイアログ (最大 10 件まで列挙)
    if (!missingFiles.empty())
    {
        juce::String list;
        const int maxShow = juce::jmin((int) missingFiles.size(), 10);
        for (int i = 0; i < maxShow; ++i)
            list << "  • " << missingFiles[(size_t) i] << "\n";
        if ((int) missingFiles.size() > maxShow)
            list << tr(u8"  ... ほか ") << juce::String((int) missingFiles.size() - maxShow) << tr(u8" 件\n");
        juce::AlertWindow::showAsync(juce::MessageBoxOptions()
            .withIconType(juce::MessageBoxIconType::WarningIcon)
            .withTitle(tr(u8"欠損ファイル"))
            .withMessage(tr(u8"以下の音声ファイルが見つかりませんでした。\n"
                            u8"クリップは表示されますが音は出ません。\n"
                            u8"ファイルを <プロジェクト>/Audio/ に戻すと自動的に復活します。\n\n")
                         + list)
            .withButton("OK"), nullptr);
    }
    return true;
}

void MainComponent::saveProject()
{
    if (currentProjectFile.existsAsFile()) saveProjectTo(currentProjectFile);
    else                                     saveProjectAs();
}

// ── 新規作成 / 開く / 別名で保存 / 閉じる確認 ──────────────────────
bool MainComponent::createNewProject(const juce::File& projectFile,
                                     double sampleRate, int bitDepth)
{
    // フォルダが無ければ作成
    auto projectDir = projectFile.getParentDirectory();
    projectDir.createDirectory();
    projectDir.getChildFile("Audio").createDirectory();
    projectDir.getChildFile("Cache").createDirectory();

    // 既存セッションを停止・全トラック削除（新規スタート）
    if (isPlaying) stopTransport();
    audioEngine.clearPlayback();
    pluginEditorWindows.clear();   // 既存プラグインエディタを閉じる
    while (trackManager.getTrackCount() > 0)
        trackManager.removeTrack(0);
    // マスターチェーンもクリア
    while (audioEngine.getMasterChain().getNumPlugins() > 0)
        audioEngine.getMasterChain().removePlugin(0);
    // 全トラックを破棄したので、旧クリップを指す Undo 履歴も破棄する (ダングリング防止)
    undoManager.clearUndoHistory();

    // プロジェクト設定を反映
    appSettings.projectSampleRate = sampleRate;
    appSettings.projectBitDepth   = bitDepth;
    audioEngine.setAppSettings(appSettings);
    recordingMgr.setBitDepth(bitDepth);
    // オーディオデバイスをプロジェクトのサンプリングレートに追従させる
    applyProjectSampleRateToDevice();

    currentProjectFile = projectFile;

    // 初回保存（空プロジェクト）— 以降の録音/インポートはこのフォルダ配下に保存される
    // 新規作成の内部保存なので「保存しました」通知は出さない (announce=false)
    if (!saveProjectTo(projectFile, /*announce=*/false)) return false;

    statusBar.setTrackCount(trackManager.getTrackCount());
    statusBar.setSampleRate((int) sampleRate);
    statusBar.setBitDepth(bitDepth);
    restartAutoSaveTimer();
    return true;
}

bool MainComponent::openExistingProject(const juce::File& projectFile)
{
    return loadProjectFrom(projectFile);
}

void MainComponent::saveProjectAs(std::function<void(bool)> onDone)
{
    fileChooser = std::make_unique<juce::FileChooser>(
        tr(u8"プロジェクトを保存"),
        currentProjectFile.existsAsFile()
            ? currentProjectFile.getParentDirectory()
            : juce::File::getSpecialLocation(juce::File::userDocumentsDirectory),
        "*" + ProjectManager::fileExtension());
    fileChooser->launchAsync(juce::FileBrowserComponent::saveMode
                              | juce::FileBrowserComponent::canSelectFiles
                              | juce::FileBrowserComponent::warnAboutOverwriting,
        [this, onDone](const juce::FileChooser& fc) {
            auto f = fc.getResult();
            if (f == juce::File()) { if (onDone) onDone(false); return; }
            if (!f.hasFileExtension(ProjectManager::fileExtension()))
                f = f.withFileExtension(ProjectManager::fileExtension());
            const bool ok = saveProjectTo(f);
            if (onDone) onDone(ok);
        });
}

void MainComponent::confirmCloseIfDirty(std::function<void()> onProceed)
{
    if (!projectDirty) { if (onProceed) onProceed(); return; }

    juce::AlertWindow::showAsync(
        juce::MessageBoxOptions()
            .withIconType(juce::MessageBoxIconType::QuestionIcon)
            .withTitle(tr(u8"変更を保存しますか?"))
            .withMessage(tr(u8"このプロジェクトには未保存の変更があります。\n保存しますか?"))
            .withButton(tr(u8"保存"))
            .withButton(tr(u8"保存しない"))
            .withButton(tr(u8"キャンセル")),
        [this, onProceed](int result)
        {
            // result: 1=Save, 2=Don't Save, 3=Cancel
            if (result == 1)
            {
                if (currentProjectFile.existsAsFile())
                {
                    if (saveProjectTo(currentProjectFile))
                        if (onProceed) onProceed();
                }
                else
                {
                    // 未保存の新規プロジェクト → Save As ダイアログを経由
                    saveProjectAs([onProceed](bool savedOk)
                    {
                        if (savedOk && onProceed) onProceed();
                    });
                }
            }
            else if (result == 2)
            {
                // 保存しない: 在メモリの変更だけ破棄。世代バックアップは履歴として残す
                // （本体ファイルは変更していないので次回起動で復旧プロンプトは出ない）。
                if (onProceed) onProceed();
            }
            // result == 3 (Cancel) → 何もしない
        });
}

void MainComponent::openProject()
{
    fileChooser = std::make_unique<juce::FileChooser>(
        tr(u8"プロジェクトを開く"),
        currentProjectFile.existsAsFile()
            ? currentProjectFile.getParentDirectory()
            : juce::File::getSpecialLocation(juce::File::userDocumentsDirectory),
        ProjectManager::openWildcard());
    fileChooser->launchAsync(juce::FileBrowserComponent::openMode
                              | juce::FileBrowserComponent::canSelectFiles,
        [this](const juce::FileChooser& fc) {
            auto f = fc.getResult();
            if (f.existsAsFile()) loadProjectFrom(f);
        });
}
