// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

// MainComponent のダイアログ関連実装 (環境設定 / About / ショートカット / 書き出し /
// オーディオデバイス設定 / オーディオ/MIDI インポート)。
// MainComponent.cpp が肥大化したため分割。

#include "MainComponent.h"
#include "Localisation.h"
#include "Audio/AudioDeviceSettings.h"
#include "Export/ExportEngine.h"
#include "Export/ExportDialog.h"
#include "MIDI/MidiImporter.h"
#include "MIDI/MidiImportDialog.h"

juce::File MainComponent::doImportConversion(const juce::File& src,
                                             const std::function<bool(double)>& onProgress)
{
    double projectSr = appSettings.projectSampleRate;
    if (projectSr < 1.0) projectSr = audioEngine.getSampleRate();
    if (projectSr < 1.0) projectSr = 48000.0;

    // リサンプルが進捗の大半 (0..0.85) を占める。残りはメタデータ除去 + コピー。
    auto r = fileImporter.importFile(src, projectSr, appSettings.resampleOutputBits,
        [&onProgress](double p) { return onProgress ? onProgress(juce::jlimit(0.0, 0.85, p * 0.85)) : true; });
    if (!r.success)
    {
        if (! r.cancelled)   // キャンセル時はダイアログを出さない (ユーザの明示操作)
        {
            // doImportConversion はバックグラウンドスレッドからも呼ばれるため、
            // Component を作る AlertWindow は必ずメッセージスレッドへマーシャルする。
            const juce::String title = tr(u8"インポート失敗");
            const juce::String msg   = src.getFileName() + ": " + r.errorMessage;
            juce::MessageManager::callAsync([title, msg]
            {
                juce::AlertWindow::showAsync(juce::MessageBoxOptions()
                    .withIconType(juce::MessageBoxIconType::WarningIcon)
                    .withTitle(title)
                    .withMessage(msg)
                    .withButton("OK"), nullptr);
            });
        }
        return {};
    }
    if (onProgress) onProgress(0.85);

    // 取り込んだファイルは必ずプロジェクトの Audio/ 配下に置く。
    // 元ファイルが既に Audio/ 内であればそのまま、外部 / Cache 内ならコピー (リサンプル時は元の一時ファイルを削除)。
    auto audioDir = getProjectAudioFolder();
    auto resolved = r.file;
    if (!resolved.isAChildOf(audioDir))
    {
        auto destBase = audioDir.getChildFile(resolved.getFileName());
        auto dest     = destBase.existsAsFile() ? destBase.getNonexistentSibling() : destBase;

        // WAV かつ stripImportedMetadata が有効なら不要メタデータ (bext / iXML / smpl 等) を
        // 除去しながらコピーする。(リサンプル経由は新規 WAV を書き直しているのでメタデータは既に消えている)
        bool copied = false;
        if (! r.wasResampled
            && appSettings.stripImportedMetadata
            && resolved.hasFileExtension("wav"))
        {
            juce::String strerr;
            copied = fileImporter.copyStrippingMetadata(resolved, dest, strerr);
            // 失敗時は plain copy にフォールバック
        }
        if (! copied)
            copied = resolved.copyFileTo(dest);

        if (copied)
        {
            if (r.wasResampled) resolved.deleteFile();  // Cache 側に残った一時ファイルを片付け
            resolved = dest;
        }
    }
    if (onProgress) onProgress(1.0);
    return resolved;
}

std::vector<MainComponent::ImportedItem>
MainComponent::convertImportSourcesWithProgress(const juce::Array<juce::File>& sources)
{
    // 合計が小さければ一瞬で終わるので進捗ウィンドウを出さない (チラつき防止)。
    juce::int64 totalBytes = 0;
    for (auto& f : sources) totalBytes += f.getSize();
    constexpr juce::int64 kProgressMinBytes = 1500 * 1024;   // ~1.5 MB
    if (totalBytes < kProgressMinBytes)
        return convertImportSources(sources, {});

    // 複数ファイルでも「インポート中...」を 1 つだけ出し、全体進捗をまとめて表示する。
    struct BatchImportTask : juce::ThreadWithProgressWindow
    {
        MainComponent& mc;
        juce::Array<juce::File> srcs;
        std::vector<ImportedItem> result;
        BatchImportTask(MainComponent& m, juce::Array<juce::File> s)
            : juce::ThreadWithProgressWindow(tr(u8"インポート中..."), true, true),
              mc(m), srcs(std::move(s)) {}
        void run() override
        {
            result = mc.convertImportSources(srcs,
                [this](double overall, const juce::String& label)
                {
                    setProgress(juce::jlimit(0.0, 1.0, overall));
                    setStatusMessage(tr(u8"インポート中: ") + label);
                    return ! threadShouldExit();
                });
        }
    };
    BatchImportTask task(*this, sources);
    task.runThread();   // モーダル進捗。完了 (またはキャンセル) で返る
    return std::move(task.result);
}

void MainComponent::importAudioFilesAtDrop(const juce::Array<juce::File>& sources,
                                           double dropTime, int targetTrackIdx)
{
    auto items = convertImportSourcesWithProgress(sources);
    if (items.empty()) return;

    // 配置ルール:
    //  ・単一ファイルを既存トラックへドロップ → そのトラックの Lane 0 のドロップ位置へ置く
    //    (テイクレーンへ退避せず、波形を置いた位置にそのまま置く)
    //  ・複数ファイル → マルチトラック取り込みとみなし、全て新規トラックへ取り込む
    //    (各ファイルを別トラックの先頭に揃えて並べる)
    //  ・ドロップ先がクリック / MIDI トラック等で無効な場合も新規トラックへ
    const bool multiTrackImport = items.size() > 1;

    Track* targetTrack = nullptr;
    if (!multiTrackImport
        && targetTrackIdx >= 0 && targetTrackIdx < trackManager.getTrackCount())
    {
        Track* t = trackManager.getTrack(targetTrackIdx);
        if (t && !t->isClickTrack() && !t->isMidiTrack())
            targetTrack = t;
    }

    bool added = false;
    for (auto& it : items)
    {
        if (targetTrack)
        {
            // 既存トラックの Lane 0 のドロップ位置へ (頭固定にせずドロップ位置を尊重)
            targetTrack->addClipToLane0(it.file, juce::jmax(0.0, dropTime), it.dur);
            if (it.hasVol) targetTrack->setVolume(it.volDb);
            added = true;
        }
        else if (Track* t = trackManager.addTrack(it.name, it.stereo))
        {
            // 新規トラックは先頭 (0 秒) に揃える (カラオケ音源 / マルチトラックの定番)
            t->addClip(it.file, 0.0, it.dur);
            if (it.hasVol) t->setVolume(it.volDb);   // ラウドネス調整 (変換時に測定済み)
            added = true;
        }
    }
    timelineView.refresh();
    trackHeaderPanel.refresh();
    audioEngine.preparePlayback(trackManager);
    if (added) scheduleWaveformRefresh();
    if (added) markProjectDirty();
}

void MainComponent::moveClipToNewTrack(const EditActions::ClipParams& p, bool stereo,
                                       Lane* srcLane, AudioClip* srcClip)
{
    if (!srcLane || !srcClip) return;

    // 新規トラックを作成 (クリップ名 / ソースのモノ・ステレオを継承)
    juce::String name = p.name.isNotEmpty() ? p.name
                        : p.file.getFileNameWithoutExtension();
    Track* nt = trackManager.addTrack(name, stereo);
    if (!nt) return;
    Lane* destLane = nt->getLane(0);
    if (!destLane) return;

    // editChangeCb 相当: refresh + 再生スナップショット無効化 (再生中も他トラックを止めない)
    auto onChange = [this]
    {
        markProjectDirty();
        trackHeaderPanel.refresh();
        timelineView.refresh();
        audioEngine.invalidatePlayback();
    };

    // 1 トランザクションに: トラック追加 → ソースから削除 → 新トラック Lane 0 へ追加。
    // undo は逆順 (ClipAdd 取消 → ClipDelete 取消 = ソースへ復帰 → TrackAdd 取消 = トラック除去)。
    undoManager.beginNewTransaction("Move to New Track");
    pushTrackAddUndo(nt, /*newTransaction=*/false);
    undoManager.perform(new EditActions::ClipDeleteAction(srcLane, srcClip, onChange));
    undoManager.perform(new EditActions::ClipAddAction(
        destLane, p, nt->getFormatManager(), nt->getThumbnailCache(), onChange));

    trackHeaderPanel.refresh();
    timelineView.refresh();
    audioEngine.preparePlayback(trackManager);
    scheduleWaveformRefresh();
    statusBar.setTrackCount(trackManager.getTrackCount());
    markProjectDirty();
}

std::vector<MainComponent::ImportedItem> MainComponent::convertImportSources(
    const juce::Array<juce::File>& sources,
    const std::function<bool(double, const juce::String&)>& report)
{
    std::vector<ImportedItem> out;
    const int n = sources.size();
    for (int i = 0; i < n; ++i)
    {
        const auto& src = sources[i];
        const juce::String label = src.getFileName()
            + (n > 1 ? (" (" + juce::String(i + 1) + "/" + juce::String(n) + ")") : juce::String());
        if (report && ! report((double) i / n, label)) break;   // キャンセル

        // 変換 (リサンプル / メタ除去 / コピー)。within-file 進捗を全体進捗へマップ。
        auto resolved = doImportConversion(src, [&report, &label, i, n](double p)
        {
            return report ? report(((double) i + juce::jlimit(0.0, 1.0, p)) / n, label) : true;
        });
        if (resolved == juce::File()) continue;   // 失敗 / キャンセル

        std::unique_ptr<juce::AudioFormatReader> rd(
            trackManager.getFormatManager().createReaderFor(resolved));
        if (!rd) continue;

        ImportedItem it;
        it.name   = src.getFileNameWithoutExtension();
        it.file   = resolved;
        it.dur    = (double) rd->lengthInSamples / juce::jmax(1.0, rd->sampleRate);
        it.stereo = (rd->numChannels >= 2);
        if (appSettings.autoNormalizeOnImport)
        {
            float volDb = 0.0f;
            if (computeLoudnessTargetVol(resolved, 0.0, it.dur, volDb))
            {
                it.hasVol = true;
                it.volDb  = volDb;
            }
        }
        out.push_back(std::move(it));
    }
    return out;
}

void MainComponent::importMidiFromFile(const juce::File& file, double dropTimeOverride)
{
    auto result = MidiImporter::load(file);
    if (!result.ok)
    {
        juce::AlertWindow::showAsync(juce::MessageBoxOptions()
            .withIconType(juce::MessageBoxIconType::WarningIcon)
            .withTitle(tr(u8"読み込み失敗"))
            .withMessage(result.error)
            .withButton("OK"), nullptr);
        return;
    }
    auto sharedResult = std::make_shared<MidiImporter::ImportResult>(std::move(result));
    auto* dlg = new MidiImportDialog(*sharedResult);
    dlg->setSize(520, 400);

    juce::DialogWindow::LaunchOptions opts;
    opts.content.setOwned(dlg);
    opts.dialogTitle = tr(u8"MIDI を読み込む");
    opts.dialogBackgroundColour = juce::Colour(0xff181a1e);
    opts.escapeKeyTriggersCloseButton = true;
    opts.useNativeTitleBar = true;
    opts.resizable = false;
    auto* window = opts.launchAsync();

    dlg->onClose = [this, sharedResult, window, dropTimeOverride](MidiImportDialog::Result r)
    {
        if (window) window->exitModalState(0);
        if (!r.accepted || r.selectedTrackIndices.empty()) return;

        // ダイアログの配置選択を最優先で尊重する。
        // 「先頭に配置」を選んだら必ず 0 秒。「現在の再生位置に配置」を選んだ場合、
        //   D&D 経由なら drop した位置、それ以外なら playhead を使う。
        double placeAt = 0.0;
        if (r.placement == MidiImportDialog::Placement::AtPlayhead)
            placeAt = (dropTimeOverride >= 0.0)
                          ? dropTimeOverride
                          : audioEngine.getCurrentPositionSeconds();

        // インポート全体 (トラック追加 + テンポ/拍子/マーカー) を 1 つの Undo に束ねる
        const MusicState beforeMusic = captureMusicState();
        undoManager.beginNewTransaction();

        for (int trackIdx : r.selectedTrackIndices)
        {
            if (trackIdx < 0 || trackIdx >= (int)sharedResult->tracks.size()) continue;
            const auto& src = sharedResult->tracks[(size_t)trackIdx];

            auto* track = trackManager.addTrack(src.name.isNotEmpty() ? src.name : ("MIDI " + juce::String(trackIdx + 1)),
                                                /*stereo=*/true, /*insertAfter=*/-1, /*midi=*/true);
            if (!track) continue;
            // MIDI トラックの内蔵シンセ出力は大きめなので、デフォルトで -14 dB に設定
            track->setVolume(-14.0f);

            double endT = 0.0;
            for (int i = 0; i < src.sequence.getNumEvents(); ++i)
                endT = juce::jmax(endT, src.sequence.getEventPointer(i)->message.getTimeStamp());

            auto* clip = track->addMidiClip(placeAt, endT + 0.5);
            if (clip)
            {
                clip->setName(src.name);
                clip->setChannel(src.primaryChannel);
                clip->getSequence() = src.sequence;
            }
            pushTrackAddUndo(track, /*newTransaction=*/false);
        }

        if (r.importTempoMeter)
        {
            // 途中変化も含めて取り込む (純関数・MidiIoTests が検証)
            MidiImporter::applyTempoMeterToSettings(*sharedResult, appSettings);
            bpm = appSettings.initialBpm;
            toolbar.setBpm(bpm);
            timelineView.setBpm(bpm);
            audioEngine.setMetronomeBpm(bpm);
            audioEngine.setMetronomeBeatsPerBar(appSettings.meterNumerator);
        }
        if (r.importMarkers)
            for (const auto& mk : sharedResult->markers)
                timelineView.addMarkerAtTime(mk.first, mk.second);

        // テンポ/拍子/マーカーの変化を同じトランザクションへ積む。SnapshotAction の
        // perform() が applyMusicState(after) を呼び、bpmChanges / meterChanges を
        // timelineView (ルーラー/グリッド) と audioEngine (メトロノーム経路) へ伝搬する。
        // 旧実装は appSettings へ代入するだけで伝搬を忘れており、途中の拍子/テンポ変更が
        // 「保存して開き直すまで表示に反映されない」バグがあった
        {
            const MusicState afterMusic = captureMusicState();
            if (!(beforeMusic == afterMusic))
                undoManager.perform(new EditActions::SnapshotAction<MusicState>(
                    beforeMusic, afterMusic,
                    [this](const MusicState& s) { applyMusicState(s); }));
        }

        trackHeaderPanel.refresh();
        timelineView.refresh();
        audioEngine.preparePlayback(trackManager);
        statusBar.setTrackCount(trackManager.getTrackCount());
        markProjectDirty();
        // ダイアログ閉じた後、キーボードフォーカスを MainComponent へ戻す
        // （Space / Enter ショートカットが効くようにする）
        grabKeyboardFocus();
    };
}

void MainComponent::showImportMidiDialog()
{
    fileChooser = std::make_unique<juce::FileChooser>(
        tr(u8"MIDI ファイルを選択"),
        juce::File::getSpecialLocation(juce::File::userMusicDirectory),
        "*.mid;*.midi");
    int flags = juce::FileBrowserComponent::openMode
              | juce::FileBrowserComponent::canSelectFiles;
    fileChooser->launchAsync(flags, [this](const juce::FileChooser& fc) {
        auto file = fc.getResult();
        if (file == juce::File()) return;
        importMidiFromFile(file, /*dropTimeOverride=*/ -1.0);
    });
}

// 取り込みピッカーの音声ワイルドカード。対応拡張子は OS のデコーダで変わる (WAV/AIFF/MP3/
// FLAC/Ogg は共通、m4a/aac/caf は macOS の CoreAudio、wma は Windows の Media Foundation)。
// JUCE の各 AudioFormat が canHandleFile で受ける拡張子に合わせ、読めない形式をピッカーに
// 出さない (「選べるのに失敗」を防ぐ)。showImportDialog / showImportAnyDialog で共用しドリフトを防ぐ。
static juce::String audioImportWildcard()
{
    juce::String w = "*.wav;*.aif;*.aiff;*.mp3;*.flac;*.ogg";
   #if JUCE_MAC
    w += ";*.m4a;*.aac;*.caf";
   #elif JUCE_WINDOWS
    w += ";*.wma";
   #endif
    return w;
}

void MainComponent::showImportDialog()
{
    fileChooser = std::make_unique<juce::FileChooser>(
        tr(u8"オーディオファイルを選択"),
        juce::File::getSpecialLocation(juce::File::userMusicDirectory),
        audioImportWildcard());

    int flags = juce::FileBrowserComponent::openMode
              | juce::FileBrowserComponent::canSelectFiles
              | juce::FileBrowserComponent::canSelectMultipleItems;

    fileChooser->launchAsync(flags, [this](const juce::FileChooser& fc) {
        placeImportedAudioFiles(fc.getResults());
    });
}

// 選択済みオーディオファイルを変換 (進捗付き) してプロジェクト先頭へ配置する。
// Cmd+I (showImportDialog) とツールバー IMPORT (showImportAnyDialog) で共用。
void MainComponent::placeImportedAudioFiles(const juce::Array<juce::File>& audioFiles)
{
    if (audioFiles.isEmpty()) return;

    // ── 変換フェーズ ── 複数選択は「インポート中...」を 1 つだけ出してまとめて変換する。
    auto items = convertImportSourcesWithProgress(audioFiles);

    // ── 配置フェーズ (メッセージスレッド) ──
    // インポートしたクリップは常にプロジェクト先頭 (再生バー 0 秒) に配置する。
    // 歌い手用途では「カラオケ音源を頭から並べる」が定番フローのため。
    const double dropTime = 0.0;
    bool added = false;
    for (auto& it : items)
    {
        Track* t = (selectedTrackIndex >= 0
                    && selectedTrackIndex < trackManager.getTrackCount())
                   ? trackManager.getTrack(selectedTrackIndex) : nullptr;
        if (!t || t->isClickTrack() || t->isFolderTrack() || t->isAppCaptureTrack())
            t = trackManager.addTrack(it.name, it.stereo);
        if (t)
        {
            t->addClip(it.file, dropTime, it.dur);
            if (it.hasVol) t->setVolume(it.volDb);   // ラウドネス調整 (変換時に測定済み)
            added = true;
        }
    }
    timelineView.refresh();
    trackHeaderPanel.refresh();
    audioEngine.preparePlayback(trackManager);
    // AudioThumbnail はバックグラウンドで非同期にロードされるので、
    // 完了するまで定期的に repaint をかけないと大きいファイルで波形が途中までしか描かれない。
    if (added) scheduleWaveformRefresh();
    if (added) markProjectDirty();
}

// ツールバー IMPORT ボタン: オーディオ + MIDI を 1 つのピッカーで選び、拡張子で振り分ける。
// 音声は placeImportedAudioFiles へまとめて、MIDI は importMidiFromFile へ個別に渡す
// (D&D の onImportAudioFiles / onImportMidi と同じ仕分け方)。
void MainComponent::showImportAnyDialog()
{
    const juce::String wildcard = audioImportWildcard() + ";*.mid;*.midi";

    fileChooser = std::make_unique<juce::FileChooser>(
        tr(u8"読み込むファイルを選択 (オーディオ / MIDI)"),
        juce::File::getSpecialLocation(juce::File::userMusicDirectory),
        wildcard);

    int flags = juce::FileBrowserComponent::openMode
              | juce::FileBrowserComponent::canSelectFiles
              | juce::FileBrowserComponent::canSelectMultipleItems;

    fileChooser->launchAsync(flags, [this](const juce::FileChooser& fc) {
        auto results = fc.getResults();
        if (results.isEmpty()) return;

        juce::Array<juce::File> audioFiles, midiFiles;
        for (auto& f : results)
        {
            const auto ext = f.getFileExtension().toLowerCase();
            if (ext == ".mid" || ext == ".midi") midiFiles.add(f);
            else                                 audioFiles.add(f);
        }
        placeImportedAudioFiles(audioFiles);                 // 空なら no-op
        for (auto& m : midiFiles) importMidiFromFile(m, -1.0);
    });
}

