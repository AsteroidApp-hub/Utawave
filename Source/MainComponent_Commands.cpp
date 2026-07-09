// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

// MainComponent のメニュー・コマンドターゲット・キーボードショートカット実装。
// MainComponent.cpp が肥大化したため分割。クラス本体は MainComponent.h で宣言されている。

#include "MainComponent.h"
#include "Localisation.h"

// ── メニューバー ──────────────────────────────────────────────────────
juce::StringArray MainComponent::getMenuBarNames()
{
    return {
        tr(u8"ファイル"),
        tr(u8"ヘルプ")
    };
}

juce::PopupMenu MainComponent::getMenuForIndex(int topLevelMenuIndex, const juce::String& /*menuName*/)
{
    auto addItem = [](juce::PopupMenu& menu, int id, const juce::String& text, const juce::String& shortcut)
    {
        juce::PopupMenu::Item item;
        item.itemID                  = id;
        item.text                    = text;
        item.shortcutKeyDescription  = shortcut;
        menu.addItem(item);
    };

    juce::PopupMenu m;
    if (topLevelMenuIndex == 0)  // ファイル
    {
        addItem(m, 1, tr(u8"新規プロジェクト..."), "");
        addItem(m, 2, tr(u8"開く..."),                platformMenuShortcut(juce::String::fromUTF8(u8"⌘O")));
        m.addSeparator();
        addItem(m, 3, tr(u8"保存"),                    platformMenuShortcut(juce::String::fromUTF8(u8"⌘S")));
        addItem(m, 4, tr(u8"別名で保存..."),         platformMenuShortcut(juce::String::fromUTF8(u8"⇧⌘S")));
        {
            // プロジェクトの保存先フォルダを Finder / Explorer で開く。未保存 (保存先が無い) は非活性。
            juce::PopupMenu::Item item;
            item.itemID    = 11;
            item.text      = tr(u8"プロジェクトフォルダを開く");
            item.isEnabled = currentProjectFile.existsAsFile();
            m.addItem(item);
        }
        m.addSeparator();
        addItem(m, 9, tr(u8"オーディオを読み込む..."), platformMenuShortcut(juce::String::fromUTF8(u8"⌘I")));
        addItem(m, 8, tr(u8"MIDI を読み込む..."),     "");
        m.addSeparator();
        addItem(m, 6, tr(u8"書き出し..."),           platformMenuShortcut(juce::String::fromUTF8(u8"⌘E")));
        // 設定で ON のときだけ表示する MIDI 書き出し (普段使わない想定の機能)。
        // MIDI トラックが 1 つも無ければ非活性にする (メニューは開く度に再構築されるため
        // トラック追加/削除に追従する)。
        if (appPrefs.showMidiExportMenu)
        {
            juce::PopupMenu::Item item;
            item.itemID    = 10;
            item.text      = tr(u8"MIDI を書き出す...");
            item.isEnabled = trackManager.hasMidiTrack();
            m.addItem(item);
        }
        m.addSeparator();
        addItem(m, 200, tr(u8"歌詞パッドを表示..."), "");
        m.addSeparator();
        addItem(m, 7, tr(u8"プラグインを管理..."),  "");
        m.addSeparator();
        addItem(m, 5, tr(u8"プロジェクトを閉じる"), "");
    }
    else if (topLevelMenuIndex == 1)  // ヘルプ
    {
        addItem(m, 102, tr(u8"使い方ドキュメント..."), "");
        addItem(m, 101, tr(u8"ショートカット一覧..."), platformMenuShortcut(juce::String::fromUTF8(u8"⌘/")));
        m.addSeparator();
        addItem(m, 103, tr(u8"開発を支援する (寄付)..."), "");
        addItem(m, 100, tr(u8"Utawave について..."), "");
    }
    return m;
}

void MainComponent::menuItemSelected(int menuItemID, int /*topLevelMenuIndex*/)
{
    // 書き出し中はメニュー操作を無視する。macOS のメインメニューショートカット (Cmd+O 等) は
    // モーダル中も発火し得るため、「開く/閉じる」でのプロジェクト差し替え → トラック破棄 →
    // 書き出しスレッドの UAF を塞ぐ (MainComponent.h の exportInProgress 参照)
    if (exportInProgress) return;
    switch (menuItemID)
    {
        case 1: confirmCloseIfDirty([this] { if (onNewProject)   onNewProject(); });   break;
        case 2: confirmCloseIfDirty([this] { openProject(); });                       break;
        case 3: saveProject();       break;
        case 4: saveProjectAs();     break;
        case 5: confirmCloseIfDirty([this] { if (onCloseProject) onCloseProject(); }); break;
        case 6: showExportDialog(); break;
        case 7: showPluginManager(); break;
        case 8: showImportMidiDialog(); break;
        case 9: showImportDialog(); break;
        case 10: showMidiExportDialog(); break;
        case 11:  // プロジェクトの保存先フォルダを開く。メニューの非活性化と同じ条件 (保存済み) で
                  // ガードし、未保存時に内部の untitled/temp フォルダを開かないようにする。
            if (currentProjectFile.existsAsFile())
                if (auto dir = getProjectFolder(); dir.isDirectory())
                    dir.startAsProcess();
            break;
        case 100: showAboutDialog(); break;
        case 101: showShortcutsDialog(); break;
        case 102: showDocumentation(); break;
        case 200: openLyricsWindow(); break;
        case 103: juce::URL(tr(u8"https://donate.stripe.com/8x29ATdTI6g07vo49S38404")).launchInDefaultBrowser(); break;
        default: break;
    }
}

// ── ApplicationCommandTarget ─────────────────────────────────────────
void MainComponent::getAllCommands(juce::Array<juce::CommandID>& commands)
{
    commands.add(cmdPlayPause);
    commands.add(cmdStop);
    commands.add(cmdRecord);
    commands.add(cmdExport);
    commands.add(cmdSave);
    commands.add(cmdOpen);
}

void MainComponent::getCommandInfo(juce::CommandID commandID, juce::ApplicationCommandInfo& info)
{
    switch (commandID)
    {
        case cmdPlayPause:
            info.setInfo(tr(u8"再生/停止"), tr(u8"トランスポートの再生切替"), "Transport", 0);
            info.addDefaultKeypress(juce::KeyPress::spaceKey, 0);
            break;
        case cmdStop:
            info.setInfo(tr(u8"停止"), tr(u8"停止"), "Transport", 0);
            info.addDefaultKeypress('s', 0);
            break;
        case cmdRecord:
            info.setInfo(tr(u8"録音"), tr(u8"録音切替"), "Transport", 0);
            info.addDefaultKeypress('r', 0);
            break;
        case cmdExport:
            info.setInfo(tr(u8"書き出し..."), tr(u8"書き出し"), "File", 0);
            info.addDefaultKeypress('e', juce::ModifierKeys::commandModifier);
            break;
        case cmdSave:
            info.setInfo(tr(u8"保存"), tr(u8"プロジェクト保存"), "File", 0);
            info.addDefaultKeypress('s', juce::ModifierKeys::commandModifier);
            break;
        case cmdOpen:
            info.setInfo(tr(u8"開く..."), tr(u8"プロジェクトを開く"), "File", 0);
            info.addDefaultKeypress('o', juce::ModifierKeys::commandModifier);
            break;
        default: break;
    }
}

bool MainComponent::perform(const InvocationInfo& info)
{
    // 書き出し中は transport / ファイル操作を無視する (MainComponent.h の exportInProgress 参照)
    if (exportInProgress) return true;
    switch (info.commandID)
    {
        case cmdPlayPause: togglePlay();        return true;
        case cmdStop:      stopTransport();     return true;
        case cmdRecord:    toggleRecord();      return true;
        case cmdExport:    showExportDialog();  return true;
        case cmdSave:      saveProject();       return true;
        case cmdOpen:      openProject();       return true;
        default: return false;
    }
}

// ── キーボードショートカット ─────────────────────────────────────────
bool MainComponent::keyPressed(const juce::KeyPress& key, juce::Component*)
{
    // 書き出し中は全ショートカットを無視する。Undo でトラックが破棄されると書き出し側の
    // bouncedChains (生ポインタ) が解放済み PluginChain を触る UAF になる
    if (exportInProgress) return true;

    // Cmd+Z / Cmd+Shift+Z = Undo / Redo、Cmd+Y = Redo (Windows 標準・Mac でも受理)
    if (key.getModifiers().isCommandDown() && (key.getKeyCode() == 'z' || key.getKeyCode() == 'Z'))
    {
        const bool did = key.getModifiers().isShiftDown() ? undoManager.redo()
                                                          : undoManager.undo();
        // 構造編集の Undo/Redo はクリップを破棄/再生成するため、タイムラインの選択中
        // 生ポインタ (clip / crossfade) が解放済みを指す可能性がある。実際に巻き戻した
        // ときだけクリアして直後の Delete 等での UAF を防ぐ (対象が無い空打ちでは選択を消さない)。
        if (did) timelineView.clearSelectionsAfterExternalEdit();
        return true;
    }
    if (key.getModifiers().isCommandDown() && !key.getModifiers().isShiftDown()
        && (key.getKeyCode() == 'y' || key.getKeyCode() == 'Y'))
    {
        if (undoManager.redo()) timelineView.clearSelectionsAfterExternalEdit();
        return true;
    }

    // Cmd+, = 環境設定（Mac標準）
    if (key.getModifiers().isCommandDown()
        && (key.getKeyCode() == ',' || key.getTextCharacter() == ','))
    {
        showPreferences();
        return true;
    }

    // Cmd+/ または Cmd+? = ショートカット一覧 (チートシート) を表示。
    // macOS は Shift を含めた charactersIgnoringModifiers から keyCode を作るため
    // Cmd+Shift+/ の keyCode は '?' になる ('/' は Cmd+/)。Command 押下時は
    // textCharacter が 0 にされる macOS では下の textCharacter 判定は Windows/Linux 用の保険。
    if (key.getModifiers().isCommandDown()
        && (key.getKeyCode() == '/'
            || key.getKeyCode() == '?'
            || key.getTextCharacter() == '/'
            || key.getTextCharacter() == '?'))
    {
        showShortcutsDialog();
        return true;
    }

    // Cmd+E = 書き出し
    if (key.getModifiers().isCommandDown()
        && (key.getKeyCode() == 'e' || key.getKeyCode() == 'E'))
    {
        showExportDialog();
        return true;
    }

    // Shift+Enter = 0 〜 現在の再生位置 を範囲選択
    if (key.getModifiers().isShiftDown() && !key.getModifiers().isCommandDown()
        && key.getKeyCode() == juce::KeyPress::returnKey)
    {
        double start = 0.0;
        double end   = juce::jmax(0.001, playPosition);
        if (end > start)
            // これは「範囲選択」操作なので選択範囲だけを更新（ルーラー範囲 / ループとは独立）
            timelineView.setSelectionRange(start, end, true);
        return true;
    }

    // P = 範囲選択 / 選択クリップ(波形)の範囲を「ルーラー範囲(ループ)」に設定
    if (!key.getModifiers().isCommandDown() && !key.getModifiers().isShiftDown()
        && !key.getModifiers().isAltDown()
        && (key.getKeyCode() == 'p' || key.getKeyCode() == 'P'))
    {
        // 録音中のルーラー範囲変更は禁止 (ロケーター操作 / LOOP トグルと同じ凍結ルール)
        if (isRecording) return true;
        double s = 0.0, e = 0.0;
        if (timelineView.getRangeForRulerFromSelection(s, e) && e > s)
        {
            loopStartSecs = s;
            loopEndSecs   = e;
            timelineView.setRulerRange(loopStartSecs, loopEndSecs, loopActive);
            audioEngine.setLoopRange(loopStartSecs, loopEndSecs, loopActive);
        }
        return true;
    }

    // Cmd+S = 保存、Cmd+Shift+S = 別名で保存、Cmd+O = 開く
    if (key.getModifiers().isCommandDown()
        && (key.getKeyCode() == 's' || key.getKeyCode() == 'S'))
    {
        if (key.getModifiers().isShiftDown()) saveProjectAs();
        else                                    saveProject();
        return true;
    }
    if (key.getModifiers().isCommandDown() && !key.getModifiers().isShiftDown()
        && (key.getKeyCode() == 'o' || key.getKeyCode() == 'O'))
    {
        openProject();
        return true;
    }

    // Cmd+A / Cmd+C / Cmd+V / Cmd+X / Cmd+D / Cmd+I
    if (key.getModifiers().isCommandDown() && !key.getModifiers().isShiftDown())
    {
        int kc = key.getKeyCode();
        if (kc == 'i' || kc == 'I') { showImportDialog();                    return true; }
        if (kc == 'a' || kc == 'A') { timelineView.selectAllClips();         return true; }
        if (kc == 'c' || kc == 'C') { timelineView.copySelectedClips();      return true; }
        if (kc == 'x' || kc == 'X') { timelineView.cutSelectedClips();       return true; }
        if (kc == 'v' || kc == 'V')
        {
            Track* t = (selectedTrackIndex >= 0
                        && selectedTrackIndex < trackManager.getTrackCount())
                       ? trackManager.getTrack(selectedTrackIndex) : nullptr;
            timelineView.pasteAtPlayhead(t);
            return true;
        }
        if (kc == 'd' || kc == 'D') { timelineView.duplicateSelectedClips(); return true; }
    }

    // Option+C (Win: Alt+C) = メトロノーム (CLICK) のオン/オフ
    if (key.getModifiers().isAltDown()
        && !key.getModifiers().isCommandDown() && !key.getModifiers().isShiftDown()
        && (key.getKeyCode() == 'c' || key.getKeyCode() == 'C'))
    {
        const bool on = !toolbar.isMetronomeActive();
        audioEngine.setMetronomeEnabled(on);
        toolbar.setMetronomeActive(on);
        return true;
    }

    // Cmd+Shift+R = 遡及録音を確定（再生中バックグラウンド録音を採用）
    if (key.getModifiers().isCommandDown() && key.getModifiers().isShiftDown()
        && (key.getKeyCode() == 'r' || key.getKeyCode() == 'R'))
    {
        commitRetrospective();
        return true;
    }

    // ── Shift + キー: 選択トラックのパラメータをトグル ──
    if (key.getModifiers().isShiftDown() && selectedTrackIndex >= 0)
    {
        auto* t = trackManager.getTrack(selectedTrackIndex);
        if (t)
        {
            int kc = key.getKeyCode();
            if (kc == 'r' || kc == 'R')
            {
                t->setRecArmed(!t->isRecArmed());
                trackHeaderPanel.refresh();
                return true;
            }
            if (kc == 's' || kc == 'S')
            {
                // テイクレーン (lane > 0) にフォーカスがあればそのレーンの Solo を切替
                if (timelineView.toggleFocusLaneSolo())
                {
                    audioEngine.preparePlayback(trackManager);
                    return true;
                }
                // ボタン経由と同じく Undo 対応にする (applyTrackEditUndoable が
                // refresh / onTrackChanged / invalidatePlayback まで担う)。キー経由だけ
                // 非対応だと、後続の full-snapshot Undo と状態が食い違う。
                applyTrackEditUndoable(t, [t] { t->setSoloed(!t->isSoloed()); });
                return true;
            }
            if (kc == 'm' || kc == 'M')
            {
                applyTrackEditUndoable(t, [t] { t->setMuted(!t->isMuted()); });
                return true;
            }
            if (kc == 'i' || kc == 'I')
            {
                t->setInputMonitor(!t->isInputMonitor());
                syncInputMonitorStateToEngine();
                trackHeaderPanel.refresh();
                return true;
            }
            if (kc == 't' || kc == 'T')
            {
                t->setLanesCollapsed(!t->isLanesCollapsed());
                trackHeaderPanel.refresh();
                timelineView.refresh();
                return true;
            }
        }
    }

    // Shift+T（選択トラックなし）でも選択トラックがあれば TList トグル
    if ((key.getKeyCode() == 't' || key.getKeyCode() == 'T')
        && key.getModifiers().isShiftDown()
        && selectedTrackIndex >= 0)
        return true; // 上のブロックで処理済み

    // F = fade-in / fade-out のみ作成
    // X = crossfade のみ作成 (波形が重なっている場合のみ)
    if (!key.getModifiers().isCommandDown() && !key.getModifiers().isShiftDown()
        && !key.getModifiers().isAltDown())
    {
        if (key.getKeyCode() == 'f' || key.getKeyCode() == 'F')
        {
            timelineView.applyCrossfadeToSelection(TimelineView::FadeOpMode::FadesOnly);
            return true;
        }
        if (key.getKeyCode() == 'x' || key.getKeyCode() == 'X')
        {
            timelineView.applyCrossfadeToSelection(TimelineView::FadeOpMode::CrossfadeOnly);
            return true;
        }
    }

    // Delete / Backspace = 選択クロスフェード / 範囲選択内の波形 / 選択クリップ群を削除
    if (key == juce::KeyPress::deleteKey || key == juce::KeyPress::backspaceKey)
    {
        if (timelineView.hasSelectedCrossfade())
        {
            timelineView.deleteSelectedCrossfade();
            return true;
        }
        // 範囲選択があれば範囲内の波形だけを削除 (クリップ全体削除より優先)。
        // 範囲ドラッグはクリップ自体も選択状態にするため、先に判定しないと
        // 「範囲を選んで Delete」でクリップが丸ごと消えてしまう。
        if (timelineView.hasSelectionRange())
        {
            timelineView.deleteSelectionRange();
            return true;
        }
        if (timelineView.hasSelectedClip())
        {
            timelineView.deleteSelectedClips();
            return true;
        }
        if (timelineView.hasSelectedMidiClip())
        {
            timelineView.deleteSelectedMidiClip();
            return true;
        }
    }

    // ← → = 選択クリップ群をナッジ
    if (key == juce::KeyPress::leftKey || key == juce::KeyPress::rightKey)
    {
        if (timelineView.hasSelectedClip())
        {
            double nudge = key.getModifiers().isShiftDown() ? 0.1 : 0.01;
            if (key == juce::KeyPress::leftKey) nudge = -nudge;
            timelineView.nudgeSelectedClips(nudge);
            return true;
        }
    }

    // ↑ ↓ = 選択範囲のフォーカスレーンを上下に移動
    // Shift+↑ = フォーカスレーンの選択範囲を録音レーン (Lane 0) にコピペ
    // 注: KeyPress::operator==(int) は修飾キーありで false を返すため keyCode で比較
    {
        const int kc = key.getKeyCode();
        const bool isUp   = (kc == juce::KeyPress::upKey);
        const bool isDown = (kc == juce::KeyPress::downKey);
        if (isUp || isDown)
        {
            if (key.getModifiers().isShiftDown() && isUp)
            {
                if (timelineView.copySelectionRangeToRecLane())
                {
                    trackHeaderPanel.refresh();
                    audioEngine.preparePlayback(trackManager);
                    return true;
                }
                return false;
            }
            if (!key.getModifiers().isAnyModifierKeyDown()
                && timelineView.hasSelectionRange())
            {
                int delta = isUp ? -1 : 1;
                if (timelineView.moveSelectionFocusLane(delta))
                    return true;
            }
        }
    }

    // N / B: 1 小節 進む / 戻る (小節グリッドにスナップ)
    // Shift+N / Shift+B: 次 / 前のマーカーへジャンプ
    {
        const bool isN = (key.getKeyCode() == 'n' || key.getKeyCode() == 'N');
        const bool isB = (key.getKeyCode() == 'b' || key.getKeyCode() == 'B');
        if ((isN || isB)
            && !key.getModifiers().isCommandDown()
            && !key.getModifiers().isAltDown())
        {
            const double cur = audioEngine.getCurrentPositionSeconds();
            double t = cur;
            bool doSeek = false;

            if (key.getModifiers().isShiftDown())
            {
                // マーカー移動
                doSeek = isN ? timelineView.jumpToNextMarker(cur, t)
                             : timelineView.jumpToPrevMarker(cur, t);
            }
            else
            {
                // 1 小節移動: ルーラーと同じ小節グリッド (barAtTime / barStartSecs) で動かす。
                // 旧実装の barLengthSecs() は一様な小節長で拍子分母も使う前提だったため、ルーラーが
                // 描く実際の小節線 (分母を使わず 1 拍 = 四分音符・1 小節 = numerator 拍、テンポ/拍子
                // 変化も反映) とずれ、初回の移動が 1 小節ぶんにならないことがあった。表示中の小節線へ揃える。
                auto& ruler = timelineView.getRuler();
                const int    curBar1     = ruler.barAtTime(cur);              // cur を含む 1-based 小節
                const double curBarStart = ruler.barStartSecs(curBar1);
                if (isN)
                    t = ruler.barStartSecs(curBar1 + 1);                      // 次の小節頭
                else
                    t = (std::abs(cur - curBarStart) < 1e-3)
                            ? ruler.barStartSecs(juce::jmax(1, curBar1 - 1))  // 小節頭にいる → 前の小節
                            : curBarStart;                                    // 小節途中 → 現在小節の頭
                doSeek = true;
            }

            if (doSeek)
                seekTo(t);
            return true;
        }
    }

    // ; (JIS では「+」キーの位置) / + : 再生バーを進む、 − : 戻る。
    //   実際の移動は onVBlank の連続スクラブ (updatePlayheadScrub) がキー押下を毎フレーム
    //   見て、押下時間に応じた可変速で滑らかに動かす。ここではキーを消費して既定動作
    //   (ビープ/文字入力) を抑えるだけ。テンキー +/− も拾う。
    {
        const juce::juce_wchar tc = key.getTextCharacter();
        const int kc = key.getKeyCode();
        const bool fwd  = (tc == ';') || (tc == '+') || (kc == juce::KeyPress::numberPadAdd);
        const bool back = (tc == '-')               || (kc == juce::KeyPress::numberPadSubtract);
        if ((fwd || back)
            && !key.getModifiers().isCommandDown()
            && !key.getModifiers().isAltDown())
            return true;
    }

    // L: ループ再生 ON/OFF をトグル (LOOP ボタンと同じ動作)
    if ((key.getKeyCode() == 'l' || key.getKeyCode() == 'L')
        && !key.getModifiers().isAnyModifierKeyDown())
    {
        if (toolbar.onLoopToggle) toolbar.onLoopToggle(!loopActive);
        return true;
    }

    // Shift+1〜9: グリッド (Snap) モードを切替 (1=1/1, 2=1/2, ..., 9=1/16 三連)
    // Windows は WM_CHAR から Shift で変換された記号 (!"#$%&'() 等) が来るので
    // JIS / US 両配列の Shift+数字 → 元の数字 を逆引きする。
    if (key.getModifiers().isShiftDown()
        && !key.getModifiers().isCommandDown()
        && !key.getModifiers().isAltDown()
        && !key.getModifiers().isCtrlDown())
    {
        int kc = key.getKeyCode();
        int digit = -1;
        if (kc >= '0' && kc <= '9')
        {
            digit = kc - '0';          // macOS / JIS Windows はここで拾える (0 含む)
        }
        else
        {
            switch (kc)                // Windows の Shift 変換記号
            {
                case '!':  digit = 1; break;  // JIS=US
                case '"':  digit = 2; break;  // JIS
                case '@':  digit = 2; break;  // US
                case '#':  digit = 3; break;
                case '$':  digit = 4; break;
                case '%':  digit = 5; break;
                case '&':  digit = 6; break;  // JIS (US は Shift+7 だが優先度低)
                case '^':  digit = 6; break;  // US
                case '\'': digit = 7; break;  // JIS
                case '(':  digit = 8; break;  // JIS (US は Shift+9 だが優先度低)
                case '*':  digit = 8; break;  // US
                case ')':  digit = 9; break;  // JIS (US は Shift+0 だが優先度低)
            }
        }
        if (digit >= 0 && digit <= 9)
        {
            if (toolbar.onSnapModeSelected) toolbar.onSnapModeSelected(digit);
            return true;
        }
    }

    // [ ] : 選択トラックの縦幅トグル ([ = 最大↔標準 / ] = 最小↔標準)。複数選択中は全選択トラックへ
    // 同じ高さを適用する (ドラッグ追従と同じ作法)。ループ範囲設定はルーラー横ドラッグへ移管した。
    if (key == juce::KeyPress('[') || key == juce::KeyPress(']'))
    {
        // 対象: ヘッダで複数選択されていれば全選択、無ければ主選択トラック単体。
        // getTrack は std::vector の operator[] (境界チェック無し) なので、stale な選択 index
        // (トラック削除後など) で範囲外を渡さないよう、ここで [0, trackCount) に絞る。
        const int trackCount = trackManager.getTrackCount();
        const auto& sel = trackHeaderPanel.getSelectedTrackIndices();
        std::vector<int> scope;
        for (int i : sel)
            if (i >= 0 && i < trackCount) scope.push_back(i);
        if (scope.empty() && selectedTrackIndex >= 0 && selectedTrackIndex < trackCount)
            scope.push_back(selectedTrackIndex);
        if (scope.empty()) return false;  // 有効な選択トラックが無ければ何もしない

        // 目標高さは主選択 (無ければ先頭) の現在高さで決め、全対象を同じ高さへ揃える。
        const int refIdx = (selectedTrackIndex >= 0 && selectedTrackIndex < trackCount
                            && sel.count(selectedTrackIndex)) ? selectedTrackIndex : scope.front();
        auto* refT = trackManager.getTrack(refIdx);
        if (refT == nullptr) return false;
        const int cur = refT->getMainHeight();
        int target;
        if (key == juce::KeyPress('['))
            target = (cur >= Track::maxHeight) ? Track::defaultHeight : Track::maxHeight;
        else
            target = (cur <= Track::minHeight) ? Track::defaultHeight : Track::minHeight;

        for (int i : scope)
            if (auto* t = trackManager.getTrack(i)) t->setCustomHeight(target);
        // ドラッグリサイズと同じ全更新 (markDirty + ヘッダ/タイムライン再レイアウト)
        if (trackHeaderPanel.onTrackChanged) trackHeaderPanel.onTrackChanged();
        return true;
    }

    if (key == juce::KeyPress::spaceKey)  { togglePlay();     return true; }
    if (key == juce::KeyPress('s'))       { stopTransport();  return true; }
    if (key == juce::KeyPress('r'))       { toggleRecord();   return true; }
    // Q = リテイク: 録音中のみ、今のテイクを破棄して R 押下位置から録り直す。
    // 録音中でなければ消費しない (Q で通常の録音開始はさせない)
    if (key == juce::KeyPress('q') && isRecording) { retakeRecording(); return true; }
    // Shift+F = 全体フィット（末尾 + 2 小節がビューに収まる）
    if ((key.getKeyCode() == 'f' || key.getKeyCode() == 'F')
        && key.getModifiers().isShiftDown())
    {
        timelineView.zoomToFitAll();
        return true;
    }
    if (key == juce::KeyPress::returnKey ||
        key == juce::KeyPress('0') ||
        key == juce::KeyPress(juce::KeyPress::numberPad0) ||
        key == juce::KeyPress::homeKey)
    {
        stopTransport();
        audioEngine.rewind();
        playPosition = 0.0;
        toolbar.setTimePosition(0.0, 1, 1);
        timelineView.setPlayheadPosition(0.0);
        propagatePlayheadToPianoRolls(0.0);
        return true;
    }
    return false;
}
