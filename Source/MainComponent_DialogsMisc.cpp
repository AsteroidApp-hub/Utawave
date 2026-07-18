// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

// MainComponent の About / ショートカット / オーディオ設定 / 書き出しダイアログ実装。
// MainComponent_Dialogs.cpp から分割。

#include "MainComponent.h"
#include "Localisation.h"
#include "Audio/AudioDeviceSettings.h"
#include "Export/ExportEngine.h"
#include "Export/ExportDialog.h"
#include "MIDI/MidiImporter.h"
#include "MIDI/MidiImportDialog.h"
#include "MIDI/MidiExporter.h"

// ─────────────────────────────────────────────────────────────────────────
//  Utawave について ダイアログ (バージョン + OSS ライセンス表記)
// ─────────────────────────────────────────────────────────────────────────
void MainComponent::showAboutDialog()
{
    class AboutDlg : public juce::Component
    {
    public:
        juce::Label       titleLabel, versionLabel, copyrightLabel, licenseHeader;
        juce::TextEditor  licenseText;
        juce::TextButton  closeBtn;

        AboutDlg()
        {

            titleLabel.setText("Utawave", juce::dontSendNotification);
            titleLabel.setFont(juce::FontOptions(28.0f, juce::Font::bold));
            titleLabel.setColour(juce::Label::textColourId, juce::Colours::white);
            titleLabel.setJustificationType(juce::Justification::centred);
            addAndMakeVisible(titleLabel);

            versionLabel.setText(tr(u8"バージョン ") + juce::String(ProjectInfo::versionString),
                                  juce::dontSendNotification);
            versionLabel.setFont(juce::FontOptions(13.0f));
            versionLabel.setColour(juce::Label::textColourId, juce::Colour(0xffb0b6bd));
            versionLabel.setJustificationType(juce::Justification::centred);
            addAndMakeVisible(versionLabel);

            copyrightLabel.setText(
                tr(u8"© 2025-2026 Utawave"),
                juce::dontSendNotification);
            copyrightLabel.setFont(juce::FontOptions(12.0f));
            copyrightLabel.setColour(juce::Label::textColourId, juce::Colour(0xff8a9097));
            copyrightLabel.setJustificationType(juce::Justification::centred);
            addAndMakeVisible(copyrightLabel);

            licenseHeader.setText(tr(u8"ライセンス情報 / 利用しているオープンソース"),
                                   juce::dontSendNotification);
            licenseHeader.setFont(juce::FontOptions(13.0f, juce::Font::bold));
            licenseHeader.setColour(juce::Label::textColourId, juce::Colours::white);
            addAndMakeVisible(licenseHeader);

            // ライセンス本文
            licenseText.setMultiLine(true);
            licenseText.setReadOnly(true);
            licenseText.setCaretVisible(false);
            licenseText.setScrollbarsShown(true);
            licenseText.setFont(juce::FontOptions(11.5f));
            licenseText.setColour(juce::TextEditor::backgroundColourId, juce::Colour(0xff1c1e22));
            licenseText.setColour(juce::TextEditor::textColourId,        juce::Colour(0xffd0d4d8));
            licenseText.setColour(juce::TextEditor::outlineColourId,     juce::Colour(0xff3a3d42));
            licenseText.setText(buildLicenseText(), juce::dontSendNotification);
            addAndMakeVisible(licenseText);

            closeBtn.setButtonText(tr(u8"閉じる"));
            closeBtn.onClick = [this] {
                if (auto* dw = findParentComponentOfClass<juce::DialogWindow>())
                    dw->exitModalState(0);
            };
            addAndMakeVisible(closeBtn);

            setSize(560, 540);
        }

        static juce::String buildLicenseText()
        {
            juce::String t;
            t << tr(u8"Utawave © 2025-2026\n");
            t << tr(u8"本アプリは AGPL v3 ライセンスの下で配布されています。\n")
              << tr(u8"ソースコードは公式サイトで公開しています。\n")
              << "  https://utawave.com\n\n";

            t << tr(u8"── 免責事項 ──\n")
              << tr(u8"本ソフトウェアは無料で「現状のまま」提供されます。万一、使用（または使用できないこと）により損害が生じた場合でも、作者は責任を負いかねます。大切なデータはバックアップしてください。(詳細は AGPL v3 第15条・第16条)\n\n");

            t << tr(u8"開発を応援する (寄付):\n")
              << "  " << tr(u8"https://donate.stripe.com/8x29ATdTI6g07vo49S38404") << "\n\n";

            t << tr(u8"── 利用しているサードパーティ ライブラリ ──\n\n");

            t << "JUCE Framework\n"
              << tr(u8"  Copyright (c) Raw Material Software Limited.\n")
              << tr(u8"  AGPL v3 (非商用 / オープンソース利用)\n\n");

            t << "LAME (libmp3lame)\n"
              << tr(u8"  Copyright (c) The LAME project (http://lame.sourceforge.net/).\n")
              << tr(u8"  GNU LGPL v2.0 or later\n\n");

            t << "r8brain-free-src\n"
              << tr(u8"  Copyright (c) Aleksey Vaneev.  MIT License\n\n");

            t << tr(u8"ASIO Interface Technology  (Windows ビルド時のみ、任意)\n")
              << tr(u8"  ASIO is a trademark and software of\n")
              << tr(u8"  Steinberg Media Technologies GmbH.\n")
              << tr(u8"  デュアルライセンス (Steinberg ASIO License または GPL v3)\n")
              << tr(u8"  本プロジェクトでは GPL v3 の選択肢で利用\n");

            return t;
        }

        void paint(juce::Graphics& g) override
        {
            g.fillAll(juce::Colour(0xff2a2c30));
        }

        void resized() override
        {
            auto b = getLocalBounds().reduced(16);

            titleLabel.setBounds(b.removeFromTop(40));
            b.removeFromTop(2);
            versionLabel.setBounds(b.removeFromTop(20));
            b.removeFromTop(2);
            copyrightLabel.setBounds(b.removeFromTop(20));
            b.removeFromTop(16);

            licenseHeader.setBounds(b.removeFromTop(20));
            b.removeFromTop(6);

            auto bottomRow = b.removeFromBottom(34);
            closeBtn.setBounds(bottomRow.removeFromRight(100));
            b.removeFromBottom(8);

            licenseText.setBounds(b);
        }
    };

    auto* dlg = new AboutDlg();
    juce::DialogWindow::LaunchOptions opts;
    opts.content.setOwned(dlg);
    opts.dialogTitle = tr(u8"Utawave について");
    opts.dialogBackgroundColour = juce::Colour(0xff2a2c30);
    opts.escapeKeyTriggersCloseButton = true;
    opts.useNativeTitleBar = true;
    opts.resizable = false;
    opts.launchAsync();
}

// ─────────────────────────────────────────────────────────────────────────
//  使い方ドキュメント (同梱 HTML を既定ブラウザで開く)
// ─────────────────────────────────────────────────────────────────────────
juce::File MainComponent::findBundledHelpFile() const
{
    // 同梱した help.html を探す。macOS は .app/Contents/Resources、Windows は実行ファイル隣。
    // (CMake で MACOSX_PACKAGE_LOCATION / POST_BUILD コピーにより配置される)
    const auto appFile = juce::File::getSpecialLocation(juce::File::currentApplicationFile);

    // 現在の言語のヘルプ (ja は基底 help.html)。無ければ ja へフォールバックする。
    juce::String langFile = "help.html";
    switch (Localisation::getSavedLanguage())
    {
        case Localisation::Language::English:            langFile = "help.en.html";      break;
        case Localisation::Language::SimplifiedChinese:  langFile = "help.zh-Hans.html"; break;
        case Localisation::Language::TraditionalChinese: langFile = "help.zh-Hant.html"; break;
        case Localisation::Language::Korean:             langFile = "help.ko.html";      break;
        case Localisation::Language::Japanese:
        default:                                         langFile = "help.html";         break;
    }

    // 検索ディレクトリ (mac=Resources / Win=exe隣の Docs/ → exe隣 / 同階層フォールバック)。
    // 配布 zip / インストーラはドキュメント類を Docs/ サブフォルダへ格納する (多言語 help で
    // ファイル数が増え、zip 直下が散らかるため)。旧配置 (exe 隣) も後方互換で探す。
    juce::Array<juce::File> dirs;
   #if JUCE_MAC
    dirs.add(appFile.getChildFile("Contents/Resources"));
   #endif
    dirs.add(appFile.getParentDirectory().getChildFile("Docs")); // Windows: exe 隣の Docs/
    dirs.add(appFile.getParentDirectory());                      // 旧配置: exe 隣 (後方互換)
    dirs.add(appFile);                                           // 同階層フォールバック

    // 各ディレクトリで「現在言語ファイル → ja の help.html」の順に探す。
    juce::StringArray names;
    names.add(langFile);
    names.addIfNotAlreadyThere("help.html");

    for (auto& d : dirs)
        for (auto& nm : names)
        {
            auto c = d.getChildFile(nm);
            if (c.existsAsFile())
                return c;
        }
    return {};
}

void MainComponent::showDocumentation()
{
    openBundledHelp({});
}

void MainComponent::openBundledHelp(const juce::String& anchor)
{
    const auto f = findBundledHelpFile();
    if (! f.existsAsFile())
    {
        juce::AlertWindow::showAsync(juce::MessageBoxOptions()
            .withIconType(juce::MessageBoxIconType::WarningIcon)
            .withTitle(tr(u8"使い方ドキュメント"))
            .withMessage(tr(u8"ドキュメントファイル (help.html) が見つかりませんでした。"))
            .withButton("OK"), nullptr);
        return;
    }

    if (anchor.isEmpty())
    {
        f.startAsProcess();   // 既定の HTML ハンドラ (ブラウザ) で開く
        return;
    }

    // #anchor 付きで開く。file:// URL を OS のオープン機構へ直接渡すとフラグメントが
    // 落ちる環境がある (macOS の LaunchServices / Windows の ShellExecute とも、URL でなく
    // ファイルとしてブラウザへ渡され # 以降が失われる)。そこで meta refresh の踏み台 HTML を
    // temp に書いてそれを開き、**ブラウザ自身に** #anchor 付きの遷移をさせる (ブラウザ内の
    // ナビゲーションならフラグメントは確実に効く)。万一 refresh がブロックされる環境向けに
    // 手動リンクも本文に置く
    const juce::String target = juce::URL(f).toString(false) + "#" + anchor;
    auto hop = juce::File::getSpecialLocation(juce::File::tempDirectory)
                   .getChildFile("utawave-help-goto.html");
    const bool ok = hop.replaceWithText(
        "<!doctype html><html><head><meta charset=\"utf-8\">"
        "<meta http-equiv=\"refresh\" content=\"0;url=" + target + "\">"
        "<title>Utawave Help</title></head>"
        "<body style=\"font-family:sans-serif;background:#222;color:#ddd\">"
        "<p><a style=\"color:#7ab8e8\" href=\"" + target + "\">Open the guide</a></p>"
        "</body></html>");
    if (ok) hop.startAsProcess();
    else    f.startAsProcess();   // temp に書けない環境はアンカー無しで開く (最悪でも本文には届く)
}

// ─────────────────────────────────────────────────────────────────────────
//  ショートカット一覧 ダイアログ
// ─────────────────────────────────────────────────────────────────────────
void MainComponent::showShortcutsDialog()
{
    // トグル: 既に開いていれば閉じる (ステータスバー / Cmd+/ 再操作で閉じられる)
    if (shortcutsWindow) { shortcutsWindow.reset(); return; }

    struct Entry { const char* key; const char* action; };
    struct Section { const char* title; std::vector<Entry> entries; };

    class ShortcutsView : public juce::Component
    {
    public:
        std::vector<Section> sections;
        explicit ShortcutsView(std::vector<Section> s) : sections(std::move(s)) {}

        void paint(juce::Graphics& g) override
        {
            g.fillAll(juce::Colour(0xff1c1e22));

            const int padX = 16, padY = 12;
            const int keyW = 180;       // 左カラム (キー) 幅
            int y = padY;


            for (auto& sec : sections)
            {
                // セクションタイトル
                g.setColour(juce::Colour(0xff7aaef0));
                g.setFont(juce::FontOptions(13.5f, juce::Font::bold));
                g.drawText(tr(sec.title), padX, y, getWidth() - padX * 2, 20,
                           juce::Justification::centredLeft);
                y += 22;

                // 区切り線
                g.setColour(juce::Colour(0xff3a3d42));
                g.drawHorizontalLine(y, (float) padX, (float) (getWidth() - padX));
                y += 6;

                // 各エントリ (キー | アクション)
                g.setFont(juce::FontOptions(12.5f));
                for (auto& e : sec.entries)
                {
                    g.setColour(juce::Colour(0xfff0c060));  // キーは黄色寄り
                    g.drawText(platformShortcutText(tr(e.key)), padX + 4, y, keyW, 18,
                               juce::Justification::centredLeft);
                    g.setColour(juce::Colour(0xffd0d4d8));   // アクションは白寄り
                    g.drawText(tr(e.action), padX + keyW + 8, y,
                               getWidth() - padX * 2 - keyW - 8, 18,
                               juce::Justification::centredLeft);
                    y += 20;
                }
                y += 10;
            }

            preferredHeight = y + padY;
        }
        int preferredHeight { 0 };
    };

    class ShortcutsDlg : public juce::Component
    {
    public:
        juce::Viewport      viewport;
        std::unique_ptr<ShortcutsView> view;
        juce::TextButton    closeBtn;

        ShortcutsDlg(std::vector<Section> sections)
        {
            view = std::make_unique<ShortcutsView>(std::move(sections));
            // 1 度 paint させて preferredHeight を確定
            view->setSize(620, 1200);
            {
                juce::Image dummy(juce::Image::ARGB, 1, 1, true);
                juce::Graphics g(dummy);
                view->paint(g);
            }
            view->setSize(620, juce::jmax(400, view->preferredHeight));

            viewport.setViewedComponent(view.get(), false);
            viewport.setScrollBarsShown(true, false);
            addAndMakeVisible(viewport);

            closeBtn.setButtonText(tr(u8"閉じる"));
            closeBtn.onClick = [this] {
                // 非モーダルの保持窓なので closeButtonPressed 経由で閉じる (onClose → member reset)
                if (auto* dw = findParentComponentOfClass<juce::DocumentWindow>())
                    dw->closeButtonPressed();
            };
            addAndMakeVisible(closeBtn);

            setSize(640, 600);
        }
        void paint(juce::Graphics& g) override { g.fillAll(juce::Colour(0xff2a2c30)); }
        void resized() override
        {
            auto b = getLocalBounds().reduced(8);
            auto bottom = b.removeFromBottom(34);
            closeBtn.setBounds(bottom.removeFromRight(100));
            b.removeFromBottom(6);
            viewport.setBounds(b);
            if (view) view->setSize(viewport.getMaximumVisibleWidth(),
                                     juce::jmax(viewport.getHeight(), view->preferredHeight));
        }
    };

    std::vector<Section> sections = {
        { u8"トランスポート", {
            { u8"Space",                  u8"再生 / 停止" },
            { u8"S",                      u8"停止" },
            { u8"R",                      u8"録音 開始 / 停止 (パンチイン対応)" },
            { u8"Q",                      u8"録音中: テイクを破棄して録り直し (リテイク)" },
            // Cmd+Shift+R (遡及録音を確定) はキーバインドとして残しているが、初心者が
            // 迷わないよう一覧からは非表示 (要望次第で復活)。実装は MainComponent_Commands.cpp
            { u8"L",                      u8"ループ再生 ON / OFF" },
            { u8"Option+C",               u8"メトロノーム (CLICK) ON / OFF" },
            { u8"P",                      u8"範囲選択/選択クリップをルーラー範囲に設定" },
            { u8"[ / ]",                  u8"選択トラックの縦幅 (最大↔標準 / 最小↔標準)" },
            { u8"Shift+Enter",            u8"曲頭〜再生位置を範囲選択" },
            { u8"Home / 0 / Return",      u8"先頭へ戻る" },
        }},
        { u8"ファイル", {
            { u8"Cmd+O",                  u8"プロジェクトを開く" },
            { u8"Cmd+S",                  u8"保存" },
            { u8"Cmd+Shift+S",            u8"別名で保存" },
            { u8"Cmd+I",                  u8"オーディオファイル取り込み" },
            { u8"Cmd+E",                  u8"書き出し" },
            { u8"Cmd+,",                  u8"環境設定" },
        }},
        { u8"編集", {
            { u8"Cmd+Z / Cmd+Shift+Z (Cmd+Y)", u8"取り消し / やり直し" },
            { u8"Cmd+A",                  u8"クリップ全選択" },
            { u8"Cmd+C / Cmd+X / Cmd+V",  u8"コピー / カット / ペースト" },
            { u8"Cmd+D",                  u8"複製" },
            { u8"Delete / Backspace",     u8"選択クリップ / 範囲内の波形 / クロスフェード削除" },
            { u8"← / →",                  u8"クリップを左右にナッジ" },
            { u8"Shift+← / →",            u8"クリップを大きくナッジ" },
            { u8"F",                      u8"選択範囲で fade-in / fade-out" },
            { u8"X",                      u8"選択範囲でクロスフェード" },
            { u8"Alt+クリック",           u8"クリップ分割 (波形 / MIDI のカット)" },
            { u8"Cmd+ドラッグ",           u8"クリップ移動を左右固定 (上下=トラック移動のみ)" },
            { u8"Cmd+端ドラッグ",         u8"リサイズ中グリッドスナップを一時解除" },
            { u8"Cmd+クリック",           u8"プラグインをバイパス (INS チップ)" },
        }},
        { u8"トラック", {
            { u8"Shift+R",                u8"録音アーム トグル" },
            { u8"Shift+S",                u8"ソロ トグル" },
            { u8"Shift+M",                u8"ミュート トグル" },
            { u8"Shift+Option+クリック",   u8"S / M ボタンで選択トラックを一括ソロ/ミュート" },
            { u8"Shift+I",                u8"入力モニター トグル" },
            { u8"Shift+T",                u8"テイクレーン (TList) 表示切替" },
            { u8"↑ / ↓",                  u8"選択トラックを上 / 下へ移動" },
            { u8"Option+↑ / ↓",           u8"選択範囲のフォーカスレーンを移動" },
            { u8"Shift+↑",                u8"テイクから Lane 0 へ採用" },
        }},
        { u8"マーカー / ナビゲーション", {
            { u8"N / B",                  u8"1 小節 進む / 戻る" },
            { u8"+ / −",                  u8"再生バーを進む / 戻る" },
            { u8"Shift+N / Shift+B",      u8"次 / 前のマーカーへジャンプ" },
        }},
        { u8"グリッド (スナップ)", {
            { u8"Shift+0",                u8"グリッド Off" },
            { u8"Shift+1",                u8"1/1 (小節)" },
            { u8"Shift+2",                u8"1/2 (二分音符)" },
            { u8"Shift+3",                u8"1/4 (四分音符)" },
            { u8"Shift+4",                u8"1/8 (八分音符)" },
            { u8"Shift+5",                u8"1/16 (十六分音符)" },
            { u8"Shift+6",                u8"1/32 (三十二分音符)" },
            { u8"Shift+7",                u8"1/4 三連" },
            { u8"Shift+8",                u8"1/8 三連" },
            { u8"Shift+9",                u8"1/16 三連" },
        }},
        { u8"表示・ナビゲーション", {
            { u8"Cmd+スクロール",         u8"横ズーム (再生バー中央 / マウス位置)" },
            { u8"Shift+Option+スクロール",u8"波形振幅 縦ズーム" },
            { u8"Shift+F",                u8"プロジェクト全体ビューに合わせる" },
        }},
        { u8"ルーラー", {
            { u8"Cmd+クリック (テンポ列)",   u8"テンポ変更を挿入" },
            { u8"Cmd+クリック (拍子列)",     u8"拍子変更を挿入" },
            { u8"Cmd+ホールド + ルーラー",   u8"ペンシルツール (記入準備)" },
            { u8"右クリック (マーカー / テンポ / 拍子上)", u8"該当の削除メニュー" },
        }},
        { u8"ヘルプ", {
            { u8"Cmd+/",                  u8"このショートカット一覧を表示" },
        }},
    };

    // 非モーダルの窓で表示し member で保持する。これにより (1) ステータスバーの「ショートカット」
    // 再クリックでトグルして閉じられ、(2) 一覧を見ながら本体を操作できる。閉じる経路 (OS の×/
    // 「閉じる」ボタン/Escape) はすべて closeButtonPressed → onClose → reset に集約する。
    class ShortcutsWin : public juce::DialogWindow
    {
    public:
        std::function<void()> onClose;
        ShortcutsWin()
            : juce::DialogWindow(tr(u8"ショートカット一覧"), juce::Colour(0xff2a2c30),
                                 /*escapeKeyCloses*/ true, /*addToDesktop*/ true) {}
        void closeButtonPressed() override { if (onClose) onClose(); }
        // 既定の escapeKeyPressed は setVisible(false) するだけで member が残りトグルがズレる。
        // Escape も他の閉じる経路 (× / 閉じるボタン) と同じく closeButtonPressed → reset に通す。
        bool escapeKeyPressed() override { closeButtonPressed(); return true; }
    };

    auto* w = new ShortcutsWin();
    w->setContentOwned(new ShortcutsDlg(std::move(sections)), true);
    w->setUsingNativeTitleBar(true);
    w->setResizable(true, false);
    w->centreWithSize(w->getWidth(), w->getHeight());
    w->onClose = [this] { shortcutsWindow.reset(); };  // 全ての閉じる経路を member reset に集約
    w->setVisible(true);
    shortcutsWindow.reset(w);
}

void MainComponent::showAudioSettings()
{
    auto sel = std::make_unique<juce::AudioDeviceSelectorComponent>(
        audioEngine.getDeviceManager(), 0, 2, 2, 2, false, false, false, false);
    sel->setSize(480, 380);

    // プロジェクトのサンプリングレートが設定されている場合は、
    // ダイアログ内の Sample rate コンボボックスを非活性化（プロジェクトに追従させるため）
    auto lockSampleRateUi = [](juce::Component* root)
    {
        if (root == nullptr) return;
        std::function<void(juce::Component*)> walk = [&](juce::Component* c)
        {
            // Sample rate コンボボックスは項目が "<rate> Hz" 形式
            if (auto* cb = dynamic_cast<juce::ComboBox*>(c))
            {
                for (int i = 0; i < cb->getNumItems(); ++i)
                {
                    if (cb->getItemText(i).endsWithIgnoreCase("hz"))
                    {
                        cb->setEnabled(false);
                        cb->setTooltip(juce::String::fromUTF8(
                            u8"サンプリングレートはプロジェクト設定により固定されています"));
                        break;
                    }
                }
            }
            // "Sample rate:" ラベルもグレーアウト
            if (auto* lbl = dynamic_cast<juce::Label*>(c))
            {
                if (lbl->getText().containsIgnoreCase("sample rate"))
                    lbl->setColour(juce::Label::textColourId,
                                   juce::Colours::grey.withAlpha(0.6f));
            }
            for (int i = 0; i < c->getNumChildComponents(); ++i)
                walk(c->getChildComponent(i));
        };
        walk(root);
    };

    // セレクタをホルダで包み、ダイアログを閉じた時にプロジェクト SR をデバイスへ再適用する。
    // デバイスを切り替えられても projectSampleRate == 実デバイス SR に揃え直す安全網
    // (SR コンボはロック済みだが、デバイス切替で SR が変わる経路を塞ぐ)。
    class Holder : public juce::Component
    {
    public:
        std::unique_ptr<juce::AudioDeviceSelectorComponent> sel;
        std::function<void()> onDestroy;
        explicit Holder(std::unique_ptr<juce::AudioDeviceSelectorComponent> s) : sel(std::move(s))
        {
            addAndMakeVisible(sel.get());
            setSize(sel->getWidth(), sel->getHeight());
        }
        ~Holder() override
        {
            sel.reset();               // 先にセレクタを破棄 (デバイスリスナーを外す)
            if (onDestroy) onDestroy(); // その後で SR を再適用
        }
        void resized() override { if (sel) sel->setBounds(getLocalBounds()); }
    };
    auto* holder = new Holder(std::move(sel));
    holder->onDestroy = [safe = juce::Component::SafePointer<MainComponent>(this)]
    {
        if (auto* self = safe.getComponent()) self->applyProjectSampleRateToDevice();
    };

    if (appSettings.projectSampleRate > 0.0)
    {
        // ComboBox は構築直後に項目が populated されないことがあるため、
        // 同期で 1 回試した後、非同期でも 1 回試す
        lockSampleRateUi(holder);
        juce::Component::SafePointer<juce::Component> safe(holder);
        juce::MessageManager::callAsync([safe, lockSampleRateUi]
        {
            if (auto* c = safe.getComponent()) lockSampleRateUi(c);
        });
    }

    juce::DialogWindow::LaunchOptions opts;
    opts.content.setOwned(holder);
    opts.dialogTitle                  = "Audio Settings";
    opts.dialogBackgroundColour       = AppColours::panelBg;
    opts.escapeKeyTriggersCloseButton = true;
    opts.useNativeTitleBar            = true;
    opts.resizable                    = true;
    opts.launchAsync();
}


void MainComponent::showExportDialog()
{
    // プラグインの遅延復元が残っていたら先に確定する (チェーンの欠けたバウンスを防ぐ)
    flushPendingPluginRestores();

    // 1. Context を集める
    ExportDialog::Context ctx;

    // プロジェクト末尾 = 全クリップの最大 endPosition
    double projectEnd = 0.0;
    for (int ti = 0; ti < trackManager.getTrackCount(); ++ti)
    {
        auto* track = trackManager.getTrack(ti);
        if (!track || track->isClickTrack()) continue;
        for (int li = 0; li < track->getLaneCount(); ++li)
        {
            auto* lane = track->getLane(li);
            if (!lane) continue;
            for (auto& cp : lane->clips)
                if (cp) projectEnd = juce::jmax(projectEnd, cp->getEndPosition());
        }
    }
    if (projectEnd <= 0.0) projectEnd = 60.0;
    ctx.projectEndSec = projectEnd;

    // ルーラー範囲書き出し: ルーラー横ドラッグで指定した範囲（ループ範囲を兼用）を使う
    if (loopEndSecs > loopStartSecs + 0.001)
    {
        ctx.rulerAvailable = true;
        ctx.rulerStartSec  = loopStartSecs;
        ctx.rulerEndSec    = loopEndSecs;
        const int sb = timelineView.getRuler().barAtTime(loopStartSecs);
        const int eb = timelineView.getRuler().barAtTime(loopEndSecs);
        ctx.rulerRangeDesc = tr(u8"小節") + " " + juce::String(sb)
                             + juce::String::fromUTF8(u8" 〜 ") + juce::String(eb);
    }

    // 小節範囲書き出し: 1-based 小節番号 → 開始秒 の変換と、末尾小節（デフォルト終了値）
    ctx.barToSec      = [this](int bar1) { return timelineView.getRuler().barStartSecs(bar1); };
    ctx.projectEndBar = timelineView.getRuler().barAtTime(projectEnd);

    ctx.projectSr   = (appSettings.projectSampleRate > 0.0)
                          ? appSettings.projectSampleRate
                          : audioEngine.getSampleRate();
    ctx.projectBits = appSettings.projectBitDepth;

    // 既定の出力先 = <Project>/Export/
    ctx.defaultBaseName = currentProjectFile.existsAsFile()
                              ? currentProjectFile.getFileNameWithoutExtension()
                              : juce::String("Export");
    ctx.defaultFolder   = getProjectExportFolder();

    // トラック一覧（クリックトラックとフォルダトラックを除く。フォルダはクリップを持たない
    // グループバスで、子トラックを選べばそのフォルダの INS/Vol は自動で乗る）
    for (int ti = 0; ti < trackManager.getTrackCount(); ++ti)
    {
        auto* track = trackManager.getTrack(ti);
        if (!track || track->isClickTrack() || track->isFolderTrack()
            || track->isAppCaptureTrack()) continue;   // アプリトラックは書き出しに乗らない
        ExportDialog::TrackInfo ti2;
        ti2.index             = ti;
        ti2.name              = track->getName();
        ti2.colour            = track->getColour();
        ti2.isStereoByDefault = track->isStereo();
        ctx.tracks.push_back(ti2);
    }

    // 2. ダイアログを開く
    auto* dialog = new ExportDialog(ctx);
    dialog->setSize(560, 680);

    juce::DialogWindow::LaunchOptions opts;
    opts.content.setOwned(dialog);
    opts.dialogTitle                  = tr(u8"書き出し");
    opts.dialogBackgroundColour       = juce::Colour(0xff1a1a1a);
    opts.escapeKeyTriggersCloseButton = true;
    opts.useNativeTitleBar            = true;
    opts.resizable                    = false;
    auto* w = opts.launchAsync();

    dialog->onCancel = [w]
    {
        if (w) w->exitModalState(0);
    };
    dialog->onExport = [this, w](const ExportEngine::Options& optionsIn)
    {
        // 書き出し本体はメンバ関数 runExport へ委譲する。この onExport クロージャは ExportDialog が
        // 所有しており、下の exitModalState でダイアログ破棄が (非同期に) 予約される → 書き出し中の
        // runThread (入れ子モーダル) でメッセージが回るとこのクロージャが解放される。ラムダ本体で
        // captured this を触り続けると解放後に freed メモリを読む (v1.0.0 で書き出し完了時に UAF
        // クラッシュ)。メンバ関数なら this は呼び出し時の明示レシーバ (スタック) で安定し、クロージャ
        // 解放後の captured this 参照を避けられる。exitModalState は非同期削除なので、この直後の
        // runExport 呼び出し (this の読み出し) はまだクロージャ生存下 = 安全。
        if (w) w->exitModalState(0);
        runExport(optionsIn);
    };
}

void MainComponent::runExport(const ExportEngine::Options& optionsIn)
{
        // ピーク超過保護フラグは環境設定からセット
        ExportEngine::Options options = optionsIn;
        options.peakGuard = appSettings.exportPeakGuard;

        // 再生中なら停止
        if (isPlaying) stopTransport();

        // 最新のトラック構成で再生クリップを準備（録音直後でも書き出せるように）
        audioEngine.preparePlayback(trackManager);

        // 書き出すジョブのリスト（mix-down=1件、stems=選択トラック数件）を構築
        struct Job { ExportEngine::Options opts; juce::String label; };
        std::vector<Job> jobs;

        const char* extStr = ".wav";
        if (options.format == ExportEngine::Format::AIFF) extStr = ".aiff";
        else if (options.format == ExportEngine::Format::MP3) extStr = ".mp3";
        auto folder = options.file;     // 新仕様: file は常にフォルダ
        folder.createDirectory();

        const juce::String projectStem = currentProjectFile.existsAsFile()
                                             ? currentProjectFile.getFileNameWithoutExtension()
                                             : juce::String("Export");

        if (options.stems)
        {
            for (int trackIdx : options.selectedTrackIndices)
            {
                auto* track = trackManager.getTrack(trackIdx);
                if (!track || track->isClickTrack()) continue;

                auto safeName = juce::File::createLegalFileName(track->getName());
                if (safeName.isEmpty()) safeName = "Track" + juce::String(trackIdx + 1);

                Job job;
                job.opts = options;
                job.opts.stems = false;
                job.opts.selectedTrackIndices = { trackIdx };
                // トラック毎の Mono/Stereo 指定
                auto chIt = options.trackChannelsMap.find(trackIdx);
                job.opts.numChannels = (chIt != options.trackChannelsMap.end())
                                           ? juce::jlimit(1, 2, chIt->second)
                                           : 2;
                // トラック毎の Pre/Post 指定
                auto faIt = options.trackPreFaderMap.find(trackIdx);
                job.opts.preFader = (faIt != options.trackPreFaderMap.end())
                                         ? faIt->second
                                         : false;
                auto outF = folder.getChildFile(safeName + extStr);
                if (options.autoRename && outF.existsAsFile())
                    outF = outF.getNonexistentSibling(true);
                job.opts.file  = outF;
                job.label      = track->getName();
                jobs.push_back(std::move(job));
            }

            if (jobs.empty())
            {
                juce::AlertWindow::showAsync(juce::MessageBoxOptions()
                    .withIconType(juce::MessageBoxIconType::WarningIcon)
                    .withTitle(tr(u8"書き出し対象なし"))
                    .withMessage(tr(u8"書き出せるトラックがありません"))
                    .withButton("OK"), nullptr);
                return;
            }
        }
        else
        {
            Job job;
            job.opts       = options;
            job.opts.stems = false;
            // mix-down: 選択トラックだけをミックスして 1 ファイルに。
            // エンジンは明示的なトラック指定があると Mute を無視する (ステムでミュート中の
            // トラックも単体で書き出せるようにするため) ので、2 ミックスでは「聞こえている
            // ものが書き出される」ようここでミュート中トラックを除外する
            auto& inc = job.opts.selectedTrackIndices;
            inc.erase(std::remove_if(inc.begin(), inc.end(), [this](int ti)
                      {
                          auto* t = trackManager.getTrack(ti);
                          if (t == nullptr) return false;
                          if (t->isMuted()) return true;
                          // フォルダのミュートは子へ継承 (再生と同じ「聞こえているもの」規則)
                          if (auto* f = t->getFolderParent())
                              if (f->isMuted()) return true;
                          return false;
                      }), inc.end());
            if (inc.empty())
            {
                // 空リストをエンジンへ渡すと「フィルタ無し = 全トラック」になるため必ずここで止める
                juce::AlertWindow::showAsync(juce::MessageBoxOptions()
                    .withIconType(juce::MessageBoxIconType::WarningIcon)
                    .withTitle(tr(u8"書き出し対象なし"))
                    .withMessage(tr(u8"選択されたトラックがすべてミュートされています。\nミュートを解除するか、他のトラックを選択してください。"))
                    .withButton("OK"), nullptr);
                return;
            }
            // CLICK トラックが再生で鳴っている状態 (存在・非ミュート・ソロ規則通過) なら
            // 2 ミックスへ混ぜる (要望 2026-07)。ダイアログのトラック一覧は CLICK を出さない
            // ため index では渡せず、専用フラグで指示する (stems は従来どおり混ぜない)
            {
                bool anySoloForClick = false;
                for (int ti = 0; ti < trackManager.getTrackCount(); ++ti)
                    if (auto* t = trackManager.getTrack(ti); t != nullptr && t->isSoloed())
                    { anySoloForClick = true; break; }
                auto* clickT = trackManager.getClickTrack();
                job.opts.includeClick = clickT != nullptr && !clickT->isMuted()
                                        && !(anySoloForClick && !clickT->isSoloed());
            }
            auto base = options.baseName.isEmpty() ? projectStem : options.baseName;
            auto outF = folder.getChildFile(base + extStr);
            if (options.autoRename && outF.existsAsFile())
                outF = outF.getNonexistentSibling(true);
            job.opts.file = outF;
            job.label     = base;
            jobs.push_back(std::move(job));
        }

        struct ExportTask : juce::ThreadWithProgressWindow
        {
            AudioEngine&  engine;
            TrackManager& trackManager;
            std::vector<Job> jobs;
            int completed { 0 };
            bool ok { true };
            juce::String err;

            // 実時間レンダリング時にトラック状態を一時的に変更するため、元状態を退避しておく
            struct SavedTrackState { float volume; float pan; bool muted; };
            std::vector<SavedTrackState> savedStates;

            ExportTask(AudioEngine& e, TrackManager& tm, std::vector<Job> j)
                : juce::ThreadWithProgressWindow(tr(u8"書き出し中..."), true, true),
                  engine(e), trackManager(tm), jobs(std::move(j)) {}

            void saveTrackStates()
            {
                savedStates.clear();
                for (int i = 0; i < trackManager.getTrackCount(); ++i)
                {
                    auto* t = trackManager.getTrack(i);
                    savedStates.push_back({ t->getVolume(), t->getPan(), t->isMuted() });
                }
            }
            void restoreTrackStates()
            {
                for (int i = 0; i < (int)savedStates.size() && i < trackManager.getTrackCount(); ++i)
                {
                    auto* t = trackManager.getTrack(i);
                    t->setVolume(savedStates[(size_t)i].volume);
                    t->setPan   (savedStates[(size_t)i].pan);
                    t->setMuted (savedStates[(size_t)i].muted);
                }
            }
            void applyRealtimeFilter(const Job& job)
            {
                const auto& inc = job.opts.selectedTrackIndices;
                for (int i = 0; i < trackManager.getTrackCount(); ++i)
                {
                    auto* t = trackManager.getTrack(i);
                    if (t->isClickTrack()) { t->setMuted(true); continue; }
                    // フォルダはミュートしない (ミュートすると Mute 継承で書き出し対象の
                    // 子まで黙る。フォルダは選択リストに出ないため include に決してならない)。
                    // 子側の setMuted だけで対象を制御できる (含まれない子はミュート =
                    // フォルダバスに何も入らない)。復元は restoreTrackStates が担う
                    if (t->isFolderTrack()) { t->setMuted(false); continue; }
                    // アプリケーショントラックは書き出しに乗らない (ライブ専用)。ユーザーの
                    // ミュート状態を書き出し準備で触らない (触ると書き出し中の実聴が変わる)
                    if (t->isAppCaptureTrack()) continue;
                    const bool include = std::find(inc.begin(), inc.end(), i) != inc.end()
                                            || inc.empty();
                    t->setMuted(!include);
                    if (include && job.opts.preFader)
                    {
                        t->setVolume(0.0f);
                        t->setPan(0.0f);
                    }
                }
            }

            void run() override
            {
                const bool anyRealtime = std::any_of(jobs.begin(), jobs.end(),
                    [](const Job& j) { return j.opts.realtime; });
                if (anyRealtime) saveTrackStates();

                const int n = (int)jobs.size();
                for (int i = 0; i < n; ++i)
                {
                    if (threadShouldExit()) { ok = false; break; }
                    auto& j = jobs[(size_t)i];
                    setStatusMessage(tr(u8"書き出し中: ") + j.label
                                     + " (" + juce::String(i + 1) + "/" + juce::String(n) + ")");

                    if (j.opts.realtime)
                    {
                        // ジョブ毎に Mute / Vol / Pan を退避状態へ戻し、必要分だけ再適用
                        restoreTrackStates();
                        applyRealtimeFilter(j);
                        juce::Thread::sleep(60);   // 状態変更がオーディオスレッドに反映されるのを少し待つ
                    }

                    juce::String e;
                    bool r = ExportEngine::render(engine, j.opts,
                        [this, i, n](double p)
                        {
                            setProgress(((double)i + p) / (double)n);
                        },
                        [this] { return threadShouldExit(); },
                        &e);
                    if (!r)
                    {
                        ok = false;
                        err = e;
                        break;
                    }
                    completed = i + 1;
                }

                if (anyRealtime) restoreTrackStates();
            }
        };

        // 完了メッセージ用に最後に書き出すファイル名を控える
        juce::File lastWritten = options.file;
        if (!jobs.empty()) lastWritten = jobs.back().opts.file;

        auto* task = new ExportTask(audioEngine, trackManager, std::move(jobs));
        // 書き出し中はキー/メニュー操作 (transport / Undo / プロジェクト遷移) を無視する。
        // モーダル中も GlobalKeyMonitor とメニューショートカットは素通りするため (MainComponent.h
        // の exportInProgress 参照)。runThread が返れば例外/早期 return でも必ず false へ戻る
        const juce::ScopedValueSetter<bool> exportGuard(exportInProgress, true);
        const bool finished = task->runThread();    // モーダルで完走を待つ
        if (!finished) { task->ok = false; }
        bool ok       = task->ok;
        auto err      = task->err;
        int  done     = task->completed;
        bool stems    = options.stems;
        auto folderPath = options.file;
        delete task;

        if (ok)
        {
            if (options.revealAfter)
            {
                // 出力先フォルダ自体を開く。書き出したファイルの親 = 出力フォルダ
                // (stems・ミックスダウンとも全ファイルが同じ出力フォルダに入る)。
                // revealToUser はファイルを選択表示するだけで環境によってはフォルダが
                // 前面に来ないため、フォルダを直接開く (Finder / Explorer)。
                auto outFolder = lastWritten.getParentDirectory();
                if (outFolder.isDirectory())
                    outFolder.startAsProcess();
            }

            // 完了ダイアログは環境設定「書き出し完了ダイアログを表示」(2 ミックス / stems 共用) が
            // ON のときだけ出す。出力フォルダを開く (revealAfter) はこの設定とは独立。
            if (appPrefs.showExportCompleteDialog)
            {
                const juce::String msg = stems
                    ? (tr(u8"トラック書き出しが完了しました（")
                       + juce::String(done) + tr(u8" ファイル）：\n")
                       + folderPath.getFullPathName())
                    : (tr(u8"書き出しが完了しました:\n") + lastWritten.getFullPathName());

                // 「次回から表示しない」チェック付きダイアログ。チェックすると設定を OFF にする
                // (環境設定のトグルと同じ appPrefs.showExportCompleteDialog を共有)。
                class ExportDoneContent : public juce::Component
                {
                public:
                    juce::TextEditor   msgBox;
                    juce::ToggleButton dontShowBtn;
                    juce::TextButton   okBtn;
                    std::function<void(bool)> onClose;   // (次回から非表示か)

                    explicit ExportDoneContent(const juce::String& message)
                    {
                        msgBox.setMultiLine(true, true);
                        msgBox.setReadOnly(true);
                        msgBox.setScrollbarsShown(false);
                        msgBox.setCaretVisible(false);
                        msgBox.setColour(juce::TextEditor::backgroundColourId, juce::Colour(0xff2a2a2a));
                        msgBox.setColour(juce::TextEditor::outlineColourId,    juce::Colours::transparentBlack);
                        msgBox.setColour(juce::TextEditor::textColourId,       juce::Colours::white);
                        msgBox.setText(message, false);
                        addAndMakeVisible(msgBox);

                        dontShowBtn.setButtonText(tr(u8"次回から表示しない"));
                        dontShowBtn.setColour(juce::ToggleButton::textColourId, juce::Colours::white);
                        addAndMakeVisible(dontShowBtn);

                        okBtn.setButtonText("OK");
                        addAndMakeVisible(okBtn);
                        okBtn.onClick = [this] {
                            if (onClose) onClose(dontShowBtn.getToggleState());
                            if (auto* dw = findParentComponentOfClass<juce::DialogWindow>())
                                dw->exitModalState(0);
                        };

                        setSize(480, 170);
                    }
                    void paint(juce::Graphics& g) override { g.fillAll(juce::Colour(0xff2a2a2a)); }
                    void resized() override
                    {
                        auto r = getLocalBounds().reduced(16);
                        okBtn.setBounds(r.removeFromBottom(30).removeFromRight(90));
                        r.removeFromBottom(10);
                        dontShowBtn.setBounds(r.removeFromBottom(26));
                        r.removeFromBottom(10);
                        msgBox.setBounds(r);
                    }
                };

                auto* content = new ExportDoneContent(msg);
                // SafePointer はラムダ外のローカルに取ってから捕捉する。onExport ラムダ ([this,w]) の
                // 中で init-capture に this を書くと MSVC が this を外側クロージャと解釈して落ちるため
                // (Clang は通るが移植性のため)。
                juce::Component::SafePointer<MainComponent> safeThis(this);
                content->onClose = [safeThis](bool dontShow)
                {
                    if (auto* self = safeThis.getComponent())
                        if (dontShow) { self->appPrefs.showExportCompleteDialog = false; self->appPrefs.save(); }
                };

                juce::DialogWindow::LaunchOptions opts;
                opts.content.setOwned(content);
                opts.dialogTitle = tr(u8"書き出し完了");
                opts.dialogBackgroundColour = juce::Colour(0xff2a2a2a);
                opts.escapeKeyTriggersCloseButton = true;
                opts.useNativeTitleBar = true;
                opts.resizable = false;
                opts.launchAsync();
            }
        }
        else if (err.isNotEmpty())
        {
            juce::AlertWindow::showAsync(juce::MessageBoxOptions()
                .withIconType(juce::MessageBoxIconType::WarningIcon)
                .withTitle(tr(u8"書き出し失敗"))
                .withMessage(err)
                .withButton("OK"), nullptr);
        }
}


// ─────────────────────────────────────────────────────────────────────────
//  MIDI (SMF) 書き出し ダイアログ
//  設定 (環境設定) で「MIDI を書き出すメニューを表示」を ON にした時だけ使われる。
//  歌い手用途では多用しない想定のため、保存先を選ぶだけのシンプルなフローにする。
// ─────────────────────────────────────────────────────────────────────────
void MainComponent::showMidiExportDialog()
{
    // 書き出せる MIDI ノートが 1 つも無ければ、空ファイルを作らずに案内して終了
    int midiNoteCount = 0;
    for (int ti = 0; ti < trackManager.getTrackCount(); ++ti)
    {
        auto* track = trackManager.getTrack(ti);
        if (!track || !track->isMidiTrack()) continue;
        for (int ci = 0; ci < track->getMidiClipCount(); ++ci)
        {
            auto* clip = track->getMidiClip(ci);
            if (!clip) continue;
            const auto& seq = clip->getSequence();
            for (int i = 0; i < seq.getNumEvents(); ++i)
                if (seq.getEventPointer(i)->message.isNoteOn()) ++midiNoteCount;
        }
    }
    if (midiNoteCount == 0)
    {
        juce::AlertWindow::showAsync(juce::MessageBoxOptions()
            .withIconType(juce::MessageBoxIconType::InfoIcon)
            .withTitle(tr(u8"MIDI を書き出す"))
            .withMessage(tr(u8"書き出せる MIDI ノートがありません"))
            .withButton("OK"), nullptr);
        return;
    }

    const juce::String stem = currentProjectFile.existsAsFile()
                                  ? currentProjectFile.getFileNameWithoutExtension()
                                  : juce::String("Export");
    auto folder = getProjectExportFolder();
    folder.createDirectory();
    const auto defaultFile = folder.getChildFile(stem + ".mid");

    fileChooser = std::make_unique<juce::FileChooser>(tr(u8"MIDI を書き出す"), defaultFile, "*.mid");
    const auto flags = juce::FileBrowserComponent::saveMode
                     | juce::FileBrowserComponent::canSelectFiles
                     | juce::FileBrowserComponent::warnAboutOverwriting;
    fileChooser->launchAsync(flags, [this](const juce::FileChooser& fc)
    {
        auto file = fc.getResult();
        if (file == juce::File()) return;   // キャンセル
        if (file.getFileExtension().isEmpty())
            file = file.withFileExtension("mid");

        const auto result = MidiExporter::save(file, trackManager, appSettings);
        if (result.ok)
        {
            juce::AlertWindow::showAsync(juce::MessageBoxOptions()
                .withIconType(juce::MessageBoxIconType::InfoIcon)
                .withTitle(tr(u8"書き出し完了"))
                .withMessage(tr(u8"書き出しが完了しました:\n") + file.getFullPathName())
                .withButton("OK"), nullptr);
        }
        else
        {
            juce::AlertWindow::showAsync(juce::MessageBoxOptions()
                .withIconType(juce::MessageBoxIconType::WarningIcon)
                .withTitle(tr(u8"書き出し失敗"))
                .withMessage(result.error.isNotEmpty() ? result.error : tr(u8"書き出しに失敗しました"))
                .withButton("OK"), nullptr);
        }
    });
}


