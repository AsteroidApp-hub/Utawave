// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

#include <JuceHeader.h>
#include "MainComponent.h"
#include "UI/StartupComponent.h"
#include "Localisation.h"
#include "Project/WindowState.h"
#include "Project/AppPreferences.h"
#include "Project/CrashReporter.h"
#include "VST/PluginScannerProcess.h"
#include "Mac/AppNap.h"

class UtawaveApplication : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override
    {
        return JUCE_APPLICATION_NAME_STRING;
    }

    const juce::String getApplicationVersion() override
    {
        return JUCE_APPLICATION_VERSION_STRING;
    }

    bool moreThanOneInstanceAllowed() override
    {
        // プラグインスキャナ子プロセスは同一バイナリの 2 個目のインスタンスとして
        // 起動されるため許可する (通常起動は従来どおり単一インスタンス)
        return getCommandLineParameters().contains(PluginScannerProcess::processUID());
    }

    void initialise(const juce::String& commandLine) override
    {
        // プラグインスキャナ子プロセスとして起動された場合は、通常の起動 (ウィンドウ /
        // クラッシュレポータ / 言語) を一切行わない。スキャン中のクラッシュは子プロセス内に
        // 隔離され、ここで CrashReporter を入れない (プラグイン由来のログで本体起動時に
        // 誤った送信確認を出さないため)
        if (commandLine.contains(PluginScannerProcess::processUID()))
        {
            scannerWorker = PluginScannerProcess::createWorkerIfInvoked(commandLine);
            if (scannerWorker == nullptr)
                quit();   // 親へ接続できない (親が既に終了等) → GUI を開かず終了
            return;
        }

        CrashReporter::install();   // クラッシュ時にスタックトレースをローカルへ保存
        // macOS の App Nap を無効化する。これをしないと App Nap が周期的にタイマー/VBlank 配信を
        // スロットルし、再生バー (onVBlank) が数秒ごとに一瞬止まって見える (音は実時間スレッドで無傷)。
        disableAppNap();
        Localisation::install(Localisation::getSavedLanguage());
        // 画面全体の表示倍率をアプリ設定から適用 (高解像度/大画面で小さく見える対策)。
        // ユーザーが未設定なら主ディスプレイの幅から自動決定する (1920 幅 → 125% など)。
        // ウィンドウ生成前に設定して、起動画面から正しい倍率で表示されるようにする。
        juce::Desktop::getInstance().setGlobalScaleFactor(
            (float) AppPreferences::load().resolvedUiScale());
        mainWindow.reset(new MainWindow(getApplicationName()));

        // 前回クラッシュのログが残っていれば、起動画面表示後に同意ダイアログを出す
        // (同意した時だけ送信する。詳細は CrashReporter.h)
        juce::MessageManager::callAsync([] { CrashReporter::offerPendingReports(); });
    }

    void shutdown() override
    {
        mainWindow = nullptr;
        scannerWorker = nullptr;
    }

    void systemRequestedQuit() override
    {
        // Cmd+Q / メニュー「Quit」/ ✕ ボタン、いずれの終了経路でも未保存変更があれば
        // 確認ダイアログを通してから終了する（ウィンドウサイズ保存も終了直前に行う）。
        if (mainWindow != nullptr)
        {
            if (auto* mc = dynamic_cast<MainComponent*>(mainWindow->getContentComponent()))
            {
                mc->confirmCloseIfDirty([this]
                {
                    if (mainWindow) mainWindow->persistWindowSizeIfMain();
                    quit();
                });
                return;   // ダイアログ応答（保存/破棄）後に quit。キャンセルなら終了しない
            }
            mainWindow->persistWindowSizeIfMain();
        }
        quit();
    }

    class MainWindow : public juce::DocumentWindow
    {
    public:
        MainWindow(juce::String name)
            : DocumentWindow(name,
                             juce::Colour(0xff1a1a1a),
                             DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar(true);
            setResizable(true, true);
            showStartup();
            setVisible(true);
        }

        void showStartup()
        {
            // 起動画面に戻る前に、メインウィンドウのサイズを保存しておく
            // (次回プロジェクトを開いた時に同じサイズで開けるように)
            persistWindowSizeIfMain();

            // 最大化のまま固定サイズの起動画面に戻ると枠が最大化のまま残るので解除する
            if (isFullScreen()) setFullScreen(false);

            // 広告枠はコンパイル時フラグ (UTAWAVE_ADS_ENABLED) が ON かつユーザー設定が ON の時だけ。
            // 公開ソースの既定はフラグ OFF なので従来どおり 2 列表示になる。
            const bool showAds = AppPreferences::load().adsEffective();
            auto* startup = new StartupComponent(showAds);
            startup->onProjectChosen = [this](const juce::File& f, double sr, int bits, bool isNew)
            {
                showMain(f, sr, bits, isNew);
            };
            setContentOwned(startup, true);
            setResizable(false, false);
            centreWithSize(showAds ? StartupComponent::kWidthWithAds
                                   : StartupComponent::kWidthNoAds,
                           StartupComponent::kHeight);
        }

        void showMain(const juce::File& projectFile, double sampleRate, int bitDepth, bool isNew)
        {
            auto* mc = new MainComponent();
            mc->onCloseProject = [this]
            {
                juce::MessageManager::callAsync([this] { showStartup(); });
            };
            mc->onNewProject = [this]
            {
                juce::MessageManager::callAsync([this] { showStartup(); });
            };
            setContentOwned(mc, true);
            setResizable(true, true);

            // 前回保存したウィンドウサイズを復元 (無ければデフォルト 1280x800)
            const auto ws = WindowState::load();
            centreWithSize(ws.width, ws.height);
#if JUCE_WINDOWS
            // Windows はプロジェクトを開いたら最大化で表示する。保存サイズの復元だと
            // 画面より大きい時にタイトルバーが上に食い込み、毎回手で直す手間があった。
            // 復元サイズは「元に戻す」(最大化解除) 時の枠として上で設定済み
            setFullScreen(true);
#endif

            if (isNew)
                mc->createNewProject(projectFile, sampleRate, bitDepth);
            else
                mc->openExistingProject(projectFile);
        }

        void closeButtonPressed() override
        {
            // ✕ ボタンも Cmd+Q と同じ終了経路へ委譲する。
            // 未保存確認・ウィンドウサイズ保存は systemRequestedQuit() がまとめて行う。
            juce::JUCEApplication::getInstance()->systemRequestedQuit();
        }

        // 現在のメインウィンドウサイズを WindowState に書き出す。
        // 起動画面 (StartupComponent) 表示中は固定サイズなので何もしない。
        // ※ resized() ベースで保存すると破棄シーケンス中の自動リサイズで
        //    既定値に上書きされてしまうため、確実な終了経路だけで明示的に呼ぶ。
        void persistWindowSizeIfMain()
        {
            if (dynamic_cast<MainComponent*>(getContentComponent()) == nullptr) return;
            // 最大化 (Windows) / フルスクリーン中のサイズは保存しない。
            // 保存すると次回の「元に戻す」枠まで画面いっぱいになってしまう
            if (isFullScreen()) return;
            WindowState ws;
            ws.width  = getWidth();
            ws.height = getHeight();
            ws.save();
        }

    private:
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainWindow)
    };

private:
    std::unique_ptr<MainWindow> mainWindow;
    // プラグインスキャナ子プロセスモード時のみ非 null (詳細は PluginScannerProcess.h)
    std::unique_ptr<juce::ChildProcessWorker> scannerWorker;
};

START_JUCE_APPLICATION(UtawaveApplication)
