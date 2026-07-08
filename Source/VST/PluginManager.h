// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

#pragma once
#include <JuceHeader.h>

class OutOfProcessPluginScanner;

/**
    VST3 プラグインのスキャンとリスト管理を担当するクラス。
    KnownPluginList をユーザデータ領域に永続化する。
    実際のプラグイン読み込みは別プロセス (OutOfProcessPluginScanner) で行い、
    クラッシュするプラグインがあっても本体を巻き込まない。
*/
class PluginManager
{
public:
    PluginManager();
    ~PluginManager();

    // 初期化: フォーマットマネージャに VST3 を登録し、保存済みリストを読み込み
    void initialise();

    // バックグラウンドスレッドでスキャンを開始（既存のリストはマージ）
    // progress 0..1, statusText は現在処理中のファイル
    void startScan(std::function<void(double, juce::String)> progress = {},
                   std::function<void()> done = {});

    // スキャン中なら true
    bool isScanning() const { return scanThread != nullptr; }
    // スキャンの中止
    void cancelScan();

    // 別プロセススキャナの応答待ちを中断 / 再開する (PluginManagerDialog の停止・破棄用)。
    // abort でブロック中の findPluginTypesFor が 50ms 以内に「空結果の成功」で抜けるため、
    // スキャンスレッドを自然終了させられる。これを呼ばずにスレッドを破棄すると、応答待ち中の
    // Subprocess (mutex/condvar) が先に解放されて UAF クラッシュになる (クラッシュレポート id19/id32)。
    // abort したまま次のスキャンを始めると全プラグインが空結果になるので、開始側は必ず reset を呼ぶ
    void abortOutOfProcessScan();
    void resetOutOfProcessScanAbortFlag();

    // 既知のプラグイン一覧
    const juce::KnownPluginList& getKnownPluginList() const { return knownList; }
    juce::KnownPluginList&        getKnownPluginListRW()    { return knownList; }

    juce::AudioPluginFormatManager& getFormatManager() { return formatManager; }

    // 永続化
    void save();
    void load();

    // 検索パス
    juce::FileSearchPath getDefaultSearchPathsForFormat(const juce::String& formatName) const;
    juce::FileSearchPath getSearchPathsForFormat(const juce::String& formatName) const;
    void setSearchPathsForFormat(const juce::String& formatName, const juce::FileSearchPath& p);

    static juce::File getStoreFile();
    static juce::File getDeadMansPedalFile();

private:
    juce::AudioPluginFormatManager formatManager;
    juce::KnownPluginList          knownList;

    juce::PropertiesFile::Options propsOptions;
    std::unique_ptr<juce::PropertiesFile> props;

    // バックグラウンドスキャン用
    class ScanThread;
    std::unique_ptr<ScanThread> scanThread;

    // 別プロセススキャナ (所有は knownList の CustomScanner。abort 連携用の生ポインタ)
    OutOfProcessPluginScanner* oopScanner { nullptr };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PluginManager)
};
