// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

#pragma once
#include <JuceHeader.h>
#include "../Localisation.h"
#include "../VST/PluginManager.h"

/**
    JUCE 標準の PluginListComponent の代替実装。
    スキャン中にキャンセルすると、内部スレッドを強制停止して即時に終了する点が標準実装との違い。
    （標準実装は「現在のプラグイン完了待ち」になる）
*/
class PluginManagerDialog : public juce::Component,
                             public juce::TableListBoxModel,
                             private juce::Timer
{
public:
    explicit PluginManagerDialog(PluginManager& pm);
    ~PluginManagerDialog() override;

    void paint(juce::Graphics&) override;
    void resized() override;

    // TableListBoxModel
    int  getNumRows() override;
    void paintRowBackground(juce::Graphics&, int rowNumber, int width, int height, bool rowIsSelected) override;
    void paintCell(juce::Graphics&, int rowNumber, int columnId, int width, int height, bool rowIsSelected) override;
    void deleteKeyPressed(int lastRowSelected) override;
    void sortOrderChanged(int newSortColumnId, bool isForwards) override;

private:
    // forceRescan=true で 既知プラグインも改めて再スキャン (Steinberg等のバージョンアップ対応)
    void startScan(juce::AudioPluginFormat& fmt, bool forceRescan = false);
    void cancelScan();
    void refreshTable();

    void timerCallback() override;

    PluginManager&         pm;
    juce::TableListBox     table { "PluginTable", this };

    juce::Label            titleLabel;
    juce::Label            statusLabel;
    juce::ProgressBar      progressBar;
    double                 progressValue { 0.0 };
    juce::String           currentStatus;

    juce::TextButton       scanVST3Btn  { tr(u8"プラグインをスキャン") };
    juce::TextButton       rescanAllBtn { tr(u8"全て再スキャン") };
    juce::TextButton       cancelBtn    { tr(u8"停止") };
    juce::TextButton       clearBtn     { tr(u8"リストをクリア") };
    juce::TextButton       removeSelBtn { tr(u8"選択を削除") };
    juce::TextButton       pathsBtn     { tr(u8"検索パス...") };

    // ──── スキャンスレッド ─────────────────────────────────────
    class ScanThread : public juce::Thread
    {
    public:
        ScanThread(PluginManagerDialog& o,
                   juce::AudioPluginFormat& fmt,
                   juce::FileSearchPath paths,
                   juce::File deadMansPedalFile)
            : juce::Thread("PluginScan"),
              owner(o),
              scanner(o.pm.getKnownPluginListRW(), fmt, paths,
                      /*recursive*/ true, deadMansPedalFile,
                      /*allowAsync*/ true) {}

        // forceRescan=true なら既知プラグインも改めてスキャンする
        ScanThread(PluginManagerDialog& o,
                   juce::AudioPluginFormat& fmt,
                   juce::FileSearchPath paths,
                   juce::File deadMansPedalFile,
                   bool forceRescan)
            : juce::Thread("PluginScan"),
              owner(o),
              dontRescanIfAlreadyInList(! forceRescan),
              scanner(o.pm.getKnownPluginListRW(), fmt, paths,
                      /*recursive*/ true, deadMansPedalFile,
                      /*allowAsync*/ true) {}

        // メンバ scanner (PluginDirectoryScanner) の破棄より先に必ずスレッドを止める。
        // 暗黙 dtor だと基底 juce::Thread の停止より先に scanner が破棄され、走行中の run()
        // が破棄済み scanner (と scanFinished で解放される別プロセススキャナの応答待ち
        // mutex/condvar) を触って UAF になる (クラッシュレポート id19/id32 の最後の砦。
        // 通常は呼び出し側の abortOutOfProcessScan + 自然終了で即座に止まっている)
        ~ScanThread() override { stopThread(5000); }

        void run() override
        {
            juce::String current;
            while (! threadShouldExit())
            {
                {
                    const juce::ScopedLock sl(owner.statusLock);
                    owner.currentStatus = scanner.getNextPluginFileThatWillBeScanned();
                    owner.progressValue = scanner.getProgress();
                }
                if (! scanner.scanNextFile(dontRescanIfAlreadyInList, current))
                    break;
            }
        }

    private:
        PluginManagerDialog& owner;
        bool                 dontRescanIfAlreadyInList { true };
    public:
        juce::PluginDirectoryScanner scanner;
    };

    std::unique_ptr<ScanThread> scanThread;
    juce::CriticalSection statusLock;

    // 並び替え用の表示順スナップショット
    juce::Array<juce::PluginDescription> sortedTypes;
    int  sortColumn   { 1 };       // 1=名前 / 2=フォーマット / 3=カテゴリ / 4=メーカー
    bool sortForwards { true };
    void resortTypes();

    juce::FileSearchPath promptForPath(juce::AudioPluginFormat& fmt);

    std::unique_ptr<juce::FileChooser> fileChooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PluginManagerDialog)
};
