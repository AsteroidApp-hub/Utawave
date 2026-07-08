// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

#include "PluginManager.h"
#include "PluginScannerProcess.h"

// ─────────────────────────────────────────────────────────
// PluginManager::ScanThread
// ─────────────────────────────────────────────────────────
class PluginManager::ScanThread : public juce::Thread
{
public:
    ScanThread(PluginManager& pm,
               std::function<void(double, juce::String)> progress,
               std::function<void()> done)
        : juce::Thread("PluginScan"),
          owner(pm),
          progressCb(std::move(progress)),
          doneCb(std::move(done))
    {}

    ~ScanThread() override { stopThread(5000); }

    void run() override
    {
        auto& fmgr = owner.getFormatManager();

        // フォーマット毎にスキャン
        const int total = fmgr.getNumFormats();
        for (int fi = 0; fi < total && !threadShouldExit(); ++fi)
        {
            auto* fmt = fmgr.getFormat(fi);
            if (fmt == nullptr) continue;

            auto paths = owner.getSearchPathsForFormat(fmt->getName());
            juce::PluginDirectoryScanner scanner(owner.getKnownPluginListRW(),
                                                  *fmt,
                                                  paths,
                                                  /*recursive*/ true,
                                                  PluginManager::getDeadMansPedalFile());

            juce::String currentFile;
            while (!threadShouldExit())
            {
                if (progressCb)
                {
                    const double p = (double)fi / juce::jmax(1, total)
                                   + scanner.getProgress() / juce::jmax(1, total);
                    juce::MessageManager::callAsync(
                        [cb = progressCb, p, msg = scanner.getNextPluginFileThatWillBeScanned()]
                        { cb(p, msg); });
                }
                if (!scanner.scanNextFile(/*dontRescanIfAlreadyInList*/ true, currentFile))
                    break;
            }
        }

        owner.save();

        if (doneCb)
            juce::MessageManager::callAsync([cb = doneCb] { cb(); });
    }

private:
    PluginManager& owner;
    std::function<void(double, juce::String)> progressCb;
    std::function<void()>                     doneCb;
};

// ─────────────────────────────────────────────────────────
// PluginManager
// ─────────────────────────────────────────────────────────
PluginManager::PluginManager() = default;
PluginManager::~PluginManager() { cancelScan(); }

void PluginManager::initialise()
{
    // VST3 のみ登録 (AU は JUCE_PLUGINHOST_AU=0 のためビルドに含まれない)。
    juce::addDefaultFormatsToManager(formatManager);

    propsOptions.applicationName     = "Utawave";
    propsOptions.filenameSuffix      = "settings";
    propsOptions.folderName          = "Utawave";
    propsOptions.osxLibrarySubFolder = "Application Support";
    props = std::make_unique<juce::PropertiesFile>(getStoreFile(), propsOptions);

    load();

    // スキャン中のプラグイン読み込みを別プロセスへ隔離する (クラッシュ耐性)。
    // クラッシュ/ハングしたプラグインは KnownPluginList のブラックリストに入り
    // plugin_list.xml に永続化される (以後のスキャンでスキップ)
    auto scanner = std::make_unique<OutOfProcessPluginScanner>();
    oopScanner = scanner.get();
    knownList.setCustomScanner(std::move(scanner));
}

void PluginManager::startScan(std::function<void(double, juce::String)> progress,
                              std::function<void()> done)
{
    cancelScan();
    if (oopScanner) oopScanner->resetAbortFlag();
    scanThread = std::make_unique<ScanThread>(*this, std::move(progress), std::move(done));
    scanThread->startThread();
}

void PluginManager::cancelScan()
{
    if (scanThread)
    {
        scanThread->signalThreadShouldExit();
        // 別プロセススキャナの応答待ちブロックを起こす (50ms 以内に空結果で抜ける)。
        // これをしないと子プロセスのスキャンが終わるまで stopThread が待たされる
        if (oopScanner) oopScanner->abortScan();
        scanThread->stopThread(5000);
        scanThread.reset();
    }
}

void PluginManager::abortOutOfProcessScan()
{
    if (oopScanner) oopScanner->abortScan();
}

void PluginManager::resetOutOfProcessScanAbortFlag()
{
    if (oopScanner) oopScanner->resetAbortFlag();
}

void PluginManager::save()
{
    auto xml = knownList.createXml();
    if (!xml) return;

    // アトミック保存: tmp に書いてから replaceFileIn で差し替える。
    // 書き込み中クラッシュで plugin_list.xml が破損して次回起動時に
    // 全プラグイン再スキャンになるのを防ぐ。
    const auto dst = getStoreFile();
    auto tmp = dst.getSiblingFile(dst.getFileName() + ".tmp");
    tmp.deleteFile();
    if (!xml->writeTo(tmp))
    {
        tmp.deleteFile();
        return;
    }
    if (dst.existsAsFile())
    {
        if (!tmp.replaceFileIn(dst))
            tmp.deleteFile();
    }
    else
    {
        if (!tmp.moveFileTo(dst))
            tmp.deleteFile();
    }
}

void PluginManager::load()
{
    auto xml = juce::XmlDocument::parse(getStoreFile());
    if (xml != nullptr)
        knownList.recreateFromXml(*xml);
}

juce::FileSearchPath PluginManager::getDefaultSearchPathsForFormat(const juce::String& formatName) const
{
    for (int i = 0; i < formatManager.getNumFormats(); ++i)
    {
        if (auto* fmt = formatManager.getFormat(i); fmt && fmt->getName() == formatName)
            return fmt->getDefaultLocationsToSearch();
    }
    return {};
}

juce::FileSearchPath PluginManager::getSearchPathsForFormat(const juce::String& formatName) const
{
    if (props)
    {
        auto stored = props->getValue("scanPaths." + formatName);
        if (stored.isNotEmpty())
            return juce::FileSearchPath(stored);
    }
    return getDefaultSearchPathsForFormat(formatName);
}

void PluginManager::setSearchPathsForFormat(const juce::String& formatName, const juce::FileSearchPath& p)
{
    if (props)
    {
        props->setValue("scanPaths." + formatName, p.toString());
        props->save();
    }
}

juce::File PluginManager::getStoreFile()
{
    auto root = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                    .getChildFile("Utawave");
    root.createDirectory();
    return root.getChildFile("plugin_list.xml");
}

juce::File PluginManager::getDeadMansPedalFile()
{
    auto root = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                    .getChildFile("Utawave");
    root.createDirectory();
    return root.getChildFile("plugin_crash.txt");
}
