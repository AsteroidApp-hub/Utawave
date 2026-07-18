// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

// MainComponent のプラグイン関連処理 (Track / Master の追加・削除・編集ウィンドウ管理)
// および PianoRollWindow / PluginEditorWindow の実装。

#include "MainComponent.h"
#include "Localisation.h"
#include "VST/PluginChain.h"
#include "Audio/builtin/BuiltInFactory.h"  // 内蔵エフェクト (EQ/コンプ/リバーブ) の生成・メニュー注入
#include "Edit/PluginActions.h"   // プラグインチェーン操作の Undo アクション (テスト可能・共有)
#include "Edit/TrackActions.h"    // トラック追加/複製の Undo アクション (テスト可能・共有)
#include "UI/PluginManagerDialog.h"
#include "UI/PianoRollEditor.h"

using EditActions::PluginSlotAction;
using EditActions::PluginBypassAction;
using EditActions::PluginSwapAction;
using EditActions::PluginMoveAction;

// MainComponent デストラクタの実装は、OwnedArray<PluginEditorWindow / PianoRollWindow>
// の解体に完全型が必要なため、これらのクラス定義が見える本ファイルに置く。
MainComponent::~MainComponent()
{
    // VBlank コールバックは message thread で来る。破棄前にここで確実に外し、
    // onVBlank が参照する各メンバ (timelineView / audioEngine 等) より先に切り離す。
    vblankAttachment = {};
    // MIDI 入力コールバックは専用スレッドで来る。audioEngine 等のメンバ破棄より先に閉じる
    midiDeviceListConnection = juce::MidiDeviceListConnection();
    midiKeyboardInput.reset();
   #if JUCE_MAC
    juce::MenuBarModel::setMacMainMenu(nullptr);
   #endif
    // プラグインエディタは trackManager より先に閉じる必要がある（editor がプラグインを参照しているため）
    pluginEditorWindows.clear();
    stopTimer();
    if (autoSaveTimer) autoSaveTimer->stopTimer();
    removeKeyListener(this);
    if (isRecording) stopRecording();
    // アプリ音声取り込みを先に止める (キャプチャスレッド join + エンジンからリング登録解除)
    appCapture.stop(audioEngine);
    // 配信ミラーを先に止める (ミラーデバイス停止 + エンジンからリング登録解除の drain)
    streamMirror.stop(audioEngine);
    audioEngine.shutdown();
}

// ── PluginEditorWindow 実装 ──────────────────────────────────────────
// クラス宣言は MainComponent.h 内 (OwnedArray の解体に完全型が必要なため)。
MainComponent::PluginEditorWindow::PluginEditorWindow(
    juce::AudioPluginInstance& p,
    std::function<void(PluginEditorWindow*)> onCloseCb)
    : DocumentWindow(p.getName(), juce::Colour(0xff1a1a1a),
                     DocumentWindow::closeButton | DocumentWindow::minimiseButton),
      plugin(p),
      onClose(std::move(onCloseCb))
{
    setUsingNativeTitleBar(true);
    setAlwaysOnTop(true);   // 閉じるまで常に最前面

    if (auto* ed = plugin.createEditorIfNeeded())
    {
        editor = ed;
        setContentNonOwned(editor, true);
        // プラグイン側がリサイズ対応 (Melodyne 等) ならウィンドウも可変にする。制約は
        // エディタの constrainer へ委譲する (下限より小さくできない)。固定サイズのエディタは
        // 崩れるので従来どおり固定。(JUCE 公式 AudioPluginHost と同じ作法)
        setConstrainer(&constrainer);
        setResizable(editor->isResizable(), false);
    }
    else
    {
        setResizable(false, false);
        auto* lbl = new juce::Label({}, tr(u8"このプラグインは独自エディタを持っていません"));
        lbl->setSize(360, 80);
        lbl->setJustificationType(juce::Justification::centred);
        lbl->setColour(juce::Label::textColourId, juce::Colours::white);
        setContentOwned(lbl, true);
    }

    centreWithSize(getWidth(), getHeight());
    setVisible(true);
}

MainComponent::PluginEditorWindow::~PluginEditorWindow()
{
    // エディタを先に detach → editorBeingDeleted → delete の順で破棄
    if (editor != nullptr)
    {
        setContentNonOwned(nullptr, false);
        plugin.editorBeingDeleted(editor);
        delete editor;
        editor = nullptr;
    }
}

void MainComponent::PluginEditorWindow::closeButtonPressed()
{
    if (onClose) onClose(this);   // owner にこのウィンドウを破棄してもらう
}

void MainComponent::closePluginEditorFor(juce::AudioPluginInstance* plugin)
{
    for (int i = pluginEditorWindows.size(); --i >= 0;)
        if (pluginEditorWindows[i] && pluginEditorWindows[i]->getPlugin() == plugin)
            pluginEditorWindows.remove(i);   // OwnedArray が delete してくれる
}

// ── トラック追加 / 複製の Undo (TrackAddAction) ──────────────────────
// t は追加済みであること (最初の perform は no-op)。undo は Track を破棄せずアクションが
// 所有して延命するため、redo で同一インスタンス (プラグイン/クリップ含む) が復帰する。
// この実装が本ファイルにあるのは、undo 時に閉じる PluginEditorWindow / PianoRollWindow の
// 完全型が必要なため (ヘッダの宣言コメント参照)。
void MainComponent::pushTrackAddUndo(Track* t, bool newTransaction)
{
    if (!t) return;
    if (newTransaction)
        undoManager.beginNewTransaction();
    undoManager.perform(new EditActions::TrackAddAction(
        trackManager, t,
        /*willRemove*/ [this](Track* tr)
        {
            if (!tr) return;
            // 開いているプラグインエディタを閉じる (deleteTracks と同じ手順)
            auto& chain = tr->getPluginChain();
            for (int i = 0; i < chain.getNumPlugins(); ++i)
                closePluginEditorFor(chain.getPlugin(i));
            // このトラックの MIDI クリップを開いているピアノロールも閉じる (dangling 防止)
            for (int wi = pianoRollWindows.size(); --wi >= 0;)
            {
                auto* w = pianoRollWindows[wi];
                if (!w) continue;
                for (int ci = 0; ci < tr->getMidiClipCount(); ++ci)
                    if (w->getClip() == tr->getMidiClip(ci))
                    {
                        pianoRollWindows.remove(wi);
                        break;
                    }
            }
            // Track* / AudioClip* の参照を再生スナップショットから切る (UAF バリア)
            audioEngine.clearPlayback();
        },
        /*onChange*/ [this]
        {
            // 消えた / 戻った index を参照しないよう選択をリセット (deleteTracks と同じ)
            selectedTrackIndex = -1;
            trackHeaderPanel.clearTrackSelection();
            trackHeaderPanel.refresh();
            timelineView.refresh();
            scheduleWaveformRefresh();
            audioEngine.invalidatePlayback();
            if (trackHeaderPanel.onTrackChanged) trackHeaderPanel.onTrackChanged();
            markProjectDirty();
        }));
}

void MainComponent::pushTrackDeleteUndo(std::vector<Track*> tracks, bool newTransaction)
{
    if (tracks.empty()) return;
    if (newTransaction)
        undoManager.beginNewTransaction();
    undoManager.perform(new EditActions::TrackDeleteAction(
        trackManager, std::move(tracks),
        /*willRemove*/ [this](Track* tr)
        {
            if (!tr) return;
            // 開いているプラグインエディタを閉じる (pushTrackAddUndo / deleteTracks と同じ手順)
            auto& chain = tr->getPluginChain();
            for (int i = 0; i < chain.getNumPlugins(); ++i)
                closePluginEditorFor(chain.getPlugin(i));
            // このトラックの MIDI クリップを開いているピアノロールも閉じる (dangling 防止)
            for (int wi = pianoRollWindows.size(); --wi >= 0;)
            {
                auto* w = pianoRollWindows[wi];
                if (!w) continue;
                for (int ci = 0; ci < tr->getMidiClipCount(); ++ci)
                    if (w->getClip() == tr->getMidiClip(ci))
                    {
                        pianoRollWindows.remove(wi);
                        break;
                    }
            }
            // Track* / AudioClip* の参照を再生スナップショットから切る (UAF バリア)
            audioEngine.clearPlayback();
        },
        /*onChange*/ [this]
        {
            // 削除 / 復元のたびに選択をリセットし UI と再生スナップショットを更新する
            selectedTrackIndex = -1;
            trackHeaderPanel.clearTrackSelection();
            trackHeaderPanel.refresh();
            timelineView.refresh();
            scheduleWaveformRefresh();
            audioEngine.invalidatePlayback();
            if (trackHeaderPanel.onTrackChanged) trackHeaderPanel.onTrackChanged();
            markProjectDirty();
        }));
}

void MainComponent::pushTrackReorderUndo(std::vector<Track*> before, std::vector<Track*> after,
                                         std::vector<Track*> selected)
{
    undoManager.beginNewTransaction();
    undoManager.perform(new EditActions::TrackReorderAction(
        trackManager, std::move(before), std::move(after),
        /*onChange*/ [this, selected = std::move(selected)]
        {
            // undo / redo で順序が変わったときの UI 同期。reorderTo が onChanged
            // (= refreshTracks) を発火しパネル/タイムラインは追従する。
            trackHeaderPanel.refresh();
            timelineView.refresh();
            audioEngine.invalidatePlayback();
            if (trackHeaderPanel.onTrackChanged) trackHeaderPanel.onTrackChanged();
            // 移動したトラックを identity で選び直して選択状態を維持する。
            // (順序が変わると index がずれるので Track* → 現在 index へ解決し直す。
            //  Undo 非対応の削除で消えたトラックは indexOf<0 でスキップ = 安全)。
            std::vector<int> idx;
            for (auto* t : selected)
            {
                const int i = trackManager.indexOf(t);
                if (i >= 0) idx.push_back(i);
            }
            selectedTrackIndex = idx.empty() ? -1 : idx.front();
            trackHeaderPanel.setSelectedTracks(idx);
            markProjectDirty();
        }));
}

// ── PianoRollWindow 実装 ─────────────────────────────────────────────
MainComponent::PianoRollWindow::PianoRollWindow(
    MidiClip& mc, Track& tr, double bpm,
    double initialFocusTimeSec,
    std::function<void(PianoRollWindow*)> onCloseCb,
    std::function<void()> onChangedCb,
    std::function<void(int, float, bool)> onPreviewCb,
    std::function<void(double)> onSeekCb)
    : DocumentWindow("Piano Roll - " + (mc.getName().isNotEmpty() ? mc.getName() : tr.getName()),
                     juce::Colour(0xff1a1a1a),
                     DocumentWindow::closeButton | DocumentWindow::minimiseButton),
      clipPtr(&mc),
      editor(std::make_unique<PianoRollEditor>(mc, bpm, initialFocusTimeSec)),
      onClose(std::move(onCloseCb))
{
    setUsingNativeTitleBar(true);
    setResizable(true, false);
    setAlwaysOnTop(true);   // プラグインエディタと同様、閉じるまで常に最前面 (背面に隠れない)
    editor->setSize(900, 520);
    editor->onChanged     = std::move(onChangedCb);
    editor->onPreviewNote = std::move(onPreviewCb);
    editor->onSeek        = std::move(onSeekCb);
    setContentNonOwned(editor.get(), true);
    centreWithSize(getWidth(), getHeight());
    setVisible(true);
}

MainComponent::PianoRollWindow::~PianoRollWindow()
{
    setContentNonOwned(nullptr, false);
}

void MainComponent::PianoRollWindow::closeButtonPressed()
{
    if (onClose) onClose(this);
}

// ── LyricsWindow 実装 ────────────────────────────────────────────────
MainComponent::LyricsWindow::LyricsWindow(
    const juce::String& initialText, int fontSize,
    std::function<void(LyricsWindow*)> onCloseCb,
    std::function<void(const juce::String&)> onTextChangedCb,
    std::function<void(int)> onFontSizeChangedCb)
    : DocumentWindow(tr(u8"歌詞パッド"),
                     juce::Colour(0xff1a1a1a),
                     DocumentWindow::closeButton | DocumentWindow::minimiseButton),
      view(std::make_unique<LyricsView>(initialText, fontSize)),
      onClose(std::move(onCloseCb))
{
    setUsingNativeTitleBar(true);
    setResizable(true, false);
    setAlwaysOnTop(true);   // プラグインエディタ等と同様、歌唱中も背面に隠れない
    view->onTextChanged    = std::move(onTextChangedCb);
    view->onFontSizeChanged = std::move(onFontSizeChangedCb);
    view->setSize(460, 640);
    setContentNonOwned(view.get(), true);
    centreWithSize(getWidth(), getHeight());
    setVisible(true);
}

MainComponent::LyricsWindow::~LyricsWindow()
{
    setContentNonOwned(nullptr, false);
}

void MainComponent::LyricsWindow::closeButtonPressed()
{
    if (onClose) onClose(this);
}

void MainComponent::openLyricsWindow()
{
    // 1 枚だけ持つ。既に開いていれば前面へ。
    if (lyricsWindow)
    {
        lyricsWindow->toFront(true);
        return;
    }
    lyricsWindow = std::make_unique<LyricsWindow>(
        juce::String::fromUTF8(appSettings.lyrics.c_str()),
        appPrefs.lyricsFontSize,
        [this](LyricsWindow*) { lyricsWindow.reset(); },
        [this](const juce::String& text)
        {
            appSettings.lyrics = text.toStdString();
            markProjectDirty();
        },
        [this](int fontSize)
        {
            appPrefs.lyricsFontSize = fontSize;
            appPrefs.save();
        });
}

void MainComponent::toggleLyricsWindow()
{
    // 開いていれば閉じる / 閉じていれば開く (ステータスバーの「歌詞パッド」用)
    if (lyricsWindow) lyricsWindow.reset();
    else              openLyricsWindow();
}

void MainComponent::propagatePlayheadToPianoRolls(double playheadSecs)
{
    for (auto* w : pianoRollWindows)
        if (w && w->getClip() && w->getEditor())
        {
            const double rel = playheadSecs - w->getClip()->getStartPosition();
            // setPlayheadPosition が再生バーの帯だけを部分 repaint する。
            // 全面 repaint は 60Hz 更新では無駄なので張らない。
            w->getEditor()->setPlayheadPosition(rel);
        }
}

void MainComponent::applyMidiPagingToOpenEditors()
{
    for (auto* w : pianoRollWindows)
        if (w)
            if (auto* ed = w->getEditor())
                ed->setPagingEnabled(appPrefs.midiPagingEnabled);
}

// MIDI キーボード入力 (MIDI スレッドで来る)。UI/trackManager には触れず、AudioEngine の
// ライブ MIDI キューへ積むだけ。ノートは message thread へ転送してステップ入力にも渡す
void MainComponent::MidiKeyboardCallback::handleIncomingMidiMessage(
    juce::MidiInput*, const juce::MidiMessage& m)
{
    auto* mc = owner;
    if (mc == nullptr) return;
    if (m.isActiveSense() || m.isMidiClock()
        || m.isMidiStart() || m.isMidiStop() || m.isMidiContinue())
        return;
    mc->audioEngine.pushLiveMidi(m);
    // MIDI 録音キャプチャ: タイムライン秒でタイムスタンプして積む。エンジン位置は
    // 「今生成中」の位置 = 聞こえている音より出力レイテンシ分先なので、演奏者が
    // 聞こえた音に合わせて弾いた意図位置 (= その分手前) へ補正する
    if (mc->midiCaptureActive.load())
    {
        auto stamped = m;
        stamped.setTimeStamp(juce::jmax(0.0,
            mc->audioEngine.getCurrentPositionSeconds()
            - mc->audioEngine.getOutputLatencySecs()));
        const juce::ScopedLock sl(mc->midiCaptureLock);
        if (mc->capturedMidi.size() < 200000)   // 暴走ガード (~1 時間の連打でも届かない)
            mc->capturedMidi.push_back(std::move(stamped));
    }
    // ステップ入力中のピアノロールが無ければ message thread へ転送しない
    // (ライブ演奏の毎ノートで無駄な callAsync を post しないための事前判定)
    if (m.isNoteOnOrOff() && mc->stepInputWanted.load())
    {
        const int   note = m.getNoteNumber();
        const float vel  = m.getFloatVelocity();
        const bool  on   = m.isNoteOn();   // velocity 0 の note-on は off 扱い (isNoteOn は false)
        auto safe = ownerSafe;             // master は ctor (message thread) 作成済み・コピーは安全
        juce::MessageManager::callAsync([safe, note, vel, on]
        {
            if (auto* self = safe.getComponent())
                self->dispatchStepInputNote(note, vel, on);
        });
    }
}

// ステップ入力が有効な開いているピアノロールへ MIDI キーボードのノートを渡す (先頭 1 枚のみ)
void MainComponent::dispatchStepInputNote(int note, float velocity, bool isOn)
{
    for (auto* w : pianoRollWindows)
        if (w != nullptr)
            if (auto* ed = w->getEditor())
                if (ed->isStepInputActive())
                {
                    ed->handleStepInputMidi(note, velocity, isOn);
                    return;
                }
}

void MainComponent::openPianoRollFor(MidiClip* clip, Track* track)
{
    if (!clip || !track) return;
    // 既に開いていればそれを前面に
    for (auto* w : pianoRollWindows)
        if (w && w->getClip() == clip)
        {
            w->toFront(true);
            return;
        }
    // ピアノロールを開いた時点での再生バー位置 (クリップ先頭からの相対秒)。
    // 再生バーがクリップ範囲外なら 0 (クリップ先頭) にフォーカスする
    // (クリップ外へスクロールして空白だけ見える状態を防ぐ)。
    double playheadInClip = playPosition - clip->getStartPosition();
    if (playheadInClip < 0.0 || playheadInClip > clip->getDuration())
        playheadInClip = 0.0;

    // プレビュー用にトラック index を取得 (内蔵シンセは track index ベース)
    int trackIdx = -1;
    for (int i = 0; i < trackManager.getTrackCount(); ++i)
        if (trackManager.getTrack(i) == track) { trackIdx = i; break; }

    auto* win = new PianoRollWindow(
        *clip, *track, bpm,
        playheadInClip,
        [this](PianoRollWindow* self)
        {
            for (int i = pianoRollWindows.size(); --i >= 0;)
                if (pianoRollWindows[i] == self) { pianoRollWindows.remove(i); break; }
        },
        [this]
        {
            markProjectDirty();
            timelineView.repaint();
        },
        [this, trackIdx](int note, float vel, bool isOn)
        {
            if (trackIdx < 0) return;
            audioEngine.previewMidiNote(trackIdx, note, vel, isOn);
        },
        [this, clip](double secsInClip)
        {
            // ピアノロールのルーラークリック → グローバルプレイヘッドを移動
            seekTo(clip->getStartPosition() + secsInClip);
        });
    // PianoRoll の Undo をメイン UndoManager と統合する (#36)
    // → Cmd+Z でメイン側の他の編集 (クリップ移動等) と一貫した履歴になる
    if (auto* ed = win->getEditor())
    {
        ed->setUndoManager(&undoManager);
        // 自動ページング (再生バーがビュー外へ出たら次ページへ) をアプリ設定から反映
        ed->setPagingEnabled(appPrefs.midiPagingEnabled);
        // ピアノロール内の FOLLOW ボタンでの切替をアプリ設定へ書き戻して永続化し、
        // 開いている他のピアノロール (と環境設定の次回表示) にも反映する
        ed->onFollowToggled = [this](bool v)
        {
            appPrefs.midiPagingEnabled = v;
            appPrefs.save();
            applyMidiPagingToOpenEditors();
        };
        // ピアノロール窓専用のツールチップ表示 (アプリ設定 showTooltips に追従)
        ed->setTooltipsEnabled(appPrefs.showTooltips);
        // ステップ入力ボタンは MIDI キーボード接続中のみ表示 (未接続では入力手段が無い)
        ed->setMidiInputAvailable(midiKeyboardInput != nullptr);
        // Velocity 領域の高さ (境界ドラッグで調整可) をアプリ設定から適用し、変更を保存する
        ed->setVelocityAreaHeight(appPrefs.pianoRollVelocityH);
        ed->onVelocityAreaResized = [this](int h)
        {
            appPrefs.pianoRollVelocityH = h;
            appPrefs.save();
        };
        // Space = 再生/停止。ピアノロール (独立ウィンドウ) にフォーカスがある間も再生できるよう、
        // メイン側のトランスポートへ委譲する (Windows で再生できない問題の対策・macOS は
        // GlobalKeyMonitor 経由なのでこの経路は実質未使用だが害はない)。
        ed->onTogglePlay = [this] { togglePlay(); };
        // undo/redo で MidiClip が書き換わったときに、現在開いている
        // (作成元と異なる可能性のある) Editor を見つけて再描画させる経路。
        ed->setExternalReloadCallback([this](MidiClip* changedClip)
        {
            for (auto* w : pianoRollWindows)
                if (w && w->getClip() == changedClip)
                    if (auto* curEd = w->getEditor())
                        curEd->reloadNotesFromClip();
            // タイムライン側の MIDI クリップ描画も更新する
            timelineView.repaint();
            markProjectDirty();
        });
        // ピアノロール起点の undo/redo 後、タイムラインの選択生ポインタをクリアする
        // (共有 UndoManager で AudioClip が破棄された場合の UAF 防止。メイン経路の
        //  keyPressed が呼ぶ clearSelectionsAfterExternalEdit と同じ後始末)
        ed->setExternalUndoRedoCallback([this]
        {
            timelineView.clearSelectionsAfterExternalEdit();
        });
    }
    pianoRollWindows.add(win);
}

// ── プラグイン Undo: チェーン解決 ────────────────────────────────────
PluginChain* MainComponent::resolveChainForUndo(Track* track)
{
    if (track == nullptr) return &audioEngine.getMasterChain();
    return trackStillExists(track) ? &track->getPluginChain() : nullptr;
}

std::function<PluginChain*()> MainComponent::makeChainResolver(Track* track)
{
    return [this, track]() -> PluginChain* { return resolveChainForUndo(track); };
}

void MainComponent::swapTrackPluginsUndoable(int trackIdx, int a, int b)
{
    auto* t = trackManager.getTrack(trackIdx);
    if (!t) return;
    undoManager.beginNewTransaction();
    undoManager.perform(new PluginSwapAction(
        makeChainResolver(t), a, b, [this] { markProjectDirty(); }));
}

void MainComponent::swapMasterPluginsUndoable(int a, int b)
{
    undoManager.beginNewTransaction();
    undoManager.perform(new PluginSwapAction(
        makeChainResolver(nullptr), a, b, [this] { markProjectDirty(); }));
}

void MainComponent::togglePluginBypassUndoable(int trackIdx, int slot)
{
    Track* t = (trackIdx < 0) ? nullptr : trackManager.getTrack(trackIdx);
    if (trackIdx >= 0 && t == nullptr) return;
    auto* c = resolveChainForUndo(t);
    if (c == nullptr) return;
    const bool before = c->isBypassed(slot);
    undoManager.beginNewTransaction();
    undoManager.perform(new PluginBypassAction(
        makeChainResolver(t), slot, before, !before, [this] { markProjectDirty(); }));
}

// ── Track プラグインチェーン ─────────────────────────────────────────
void MainComponent::removePluginFromTrack(int trackIdx, int slotIdx)
{
    auto* t = trackManager.getTrack(trackIdx);
    if (!t) return;
    if (!t->getPluginChain().getPlugin(slotIdx)) return;

    // 削除を Undo 対応で実行 (インスタンスはアクションが延命・破棄せず保持)。
    // willRemove でエディタを閉じる。インスタンスは破棄しないので callAsync は不要。
    undoManager.beginNewTransaction();
    undoManager.perform(new PluginSlotAction(
        makeChainResolver(t), slotIdx, /*afterInstance*/ nullptr,
        [this] { markProjectDirty(); },
        [this](juce::AudioPluginInstance* p) { closePluginEditorFor(p); }));
}

void MainComponent::handlePluginDropAcrossTracks(int srcTrack, int srcSlot,
                                                 int dstTrack, int dstSlot, bool copy)
{
    // trackIdx == -1 はマスターチェーン。Track* で解決 (resolver は apply 時に生存確認)。
    Track* srcT = (srcTrack < 0) ? nullptr : trackManager.getTrack(srcTrack);
    Track* dstT = (dstTrack < 0) ? nullptr : trackManager.getTrack(dstTrack);
    if ((srcTrack >= 0 && srcT == nullptr) || (dstTrack >= 0 && dstT == nullptr)) return;

    auto* srcChainPtr = resolveChainForUndo(srcT);
    auto* dstChainPtr = resolveChainForUndo(dstT);
    if (!srcChainPtr || !dstChainPtr) return;
    auto* srcPlugin = srcChainPtr->getPlugin(srcSlot);
    if (!srcPlugin) return;

    auto onChange  = [this] { markProjectDirty(); };
    auto willClose = [this](juce::AudioPluginInstance* p) { closePluginEditorFor(p); };

    if (copy)
    {
        // 元プラグインの description と state を取得して、同じ種類の新インスタンスを生成
        auto desc = srcPlugin->getPluginDescription();
        juce::MemoryBlock state;
        srcPlugin->getStateInformation(state);

        const double sr = audioEngine.getSampleRate() > 0 ? audioEngine.getSampleRate() : 48000.0;
        const int    bs = 512;
        auto& fmgr      = pluginManager.getFormatManager();

        juce::String err;
        std::unique_ptr<juce::AudioPluginInstance> instance;
        if (BuiltInFactory::isBuiltInFormat(desc.pluginFormatName))
            // 内蔵エフェクトは formatManager に登録が無いので (fileOrIdentifier は安定 ID)
            // ファクトリで複製する。state は下の setStateInformation でコピーされる。
            instance = BuiltInFactory::create(desc.fileOrIdentifier);
        else
            instance = fmgr.createPluginInstance(desc, sr, bs, err);

        if (!instance)
        {
            // 内蔵エフェクトの複製失敗時は err が空なのでフォールバック文言を出す
            juce::AlertWindow::showAsync(juce::MessageBoxOptions()
                .withIconType(juce::MessageBoxIconType::WarningIcon)
                .withTitle(tr(u8"プラグインのコピーに失敗"))
                .withMessage(err.isNotEmpty() ? err : tr(u8"エフェクトを複製できませんでした"))
                .withButton("OK"), nullptr);
            return;
        }
        if (state.getSize() > 0)
            instance->setStateInformation(state.getData(), (int) state.getSize());

        // コピー = dst スロットへの追加 (dst が埋まっていれば退避して undo で戻す)。
        // ホスト側バイパス状態は getStateInformation に含まれないので、コピー元が
        // バイパス中ならコピー先も同じバイパス状態で作る (afterBypass で引き継ぐ)。
        const bool srcBypassed = srcChainPtr->isBypassed(srcSlot);
        undoManager.beginNewTransaction();
        undoManager.perform(new PluginSlotAction(
            makeChainResolver(dstT), dstSlot, std::move(instance), onChange, willClose,
            srcBypassed));
        return;
    }

    // 移動: src から取り出して dst の dstSlot へ (エディタ閉じ・dst 退避はアクション内で処理)
    undoManager.beginNewTransaction();
    undoManager.perform(new PluginMoveAction(
        makeChainResolver(srcT), srcSlot, makeChainResolver(dstT), dstSlot, onChange, willClose));
}

// ── Master プラグインチェーン ────────────────────────────────────────
void MainComponent::addPluginToMaster(int slotIdx)
{
    auto& knownList = pluginManager.getKnownPluginListRW();
    auto& fmgr      = pluginManager.getFormatManager();

    juce::PopupMenu menu;
    {
        juce::PopupMenu builtin;
        BuiltInFactory::appendMenu(builtin);
        menu.addSubMenu("Utawave", builtin);
        menu.addSeparator();
    }
    juce::KnownPluginList::addToMenu(menu, knownList.getTypes(),
                                     juce::KnownPluginList::sortByManufacturer);

    menu.showMenuAsync(juce::PopupMenu::Options(),
        [this, slotIdx, &knownList, &fmgr](int result)
        {
            if (result <= 0) return;

            // 内蔵エフェクト (予約 ID 域)
            if (auto builtinInst = BuiltInFactory::createFromMenuId(result))
            {
                auto& mc = audioEngine.getMasterChain();
                const int targetSlot = (slotIdx >= 0) ? slotIdx : mc.getNumPlugins();
                undoManager.beginNewTransaction();
                undoManager.perform(new PluginSlotAction(
                    makeChainResolver(nullptr), targetSlot, std::move(builtinInst),
                    [this] { markProjectDirty(); },
                    [this](juce::AudioPluginInstance* p) { closePluginEditorFor(p); }));
                openMasterPluginEditor(targetSlot);
                return;
            }

            const int typeIdx = juce::KnownPluginList::getIndexChosenByMenu(knownList.getTypes(), result);
            if (typeIdx < 0) return;
            auto desc = knownList.getTypes()[typeIdx];

            const double sr  = audioEngine.getSampleRate() > 0 ? audioEngine.getSampleRate() : 48000.0;
            const int    bs  = 512;

            juce::String err;
            std::unique_ptr<juce::AudioPluginInstance> instance(
                fmgr.createPluginInstance(desc, sr, bs, err));

            if (!instance)
            {
                juce::AlertWindow::showAsync(juce::MessageBoxOptions()
                    .withIconType(juce::MessageBoxIconType::WarningIcon)
                    .withTitle(tr(u8"プラグインの読込に失敗"))
                    .withMessage(err)
                    .withButton("OK"), nullptr);
                return;
            }

            auto& mc = audioEngine.getMasterChain();
            const int targetSlot = (slotIdx >= 0) ? slotIdx : mc.getNumPlugins();
            undoManager.beginNewTransaction();
            undoManager.perform(new PluginSlotAction(
                makeChainResolver(nullptr), targetSlot, std::move(instance),
                [this] { markProjectDirty(); },
                [this](juce::AudioPluginInstance* p) { closePluginEditorFor(p); }));
            openMasterPluginEditor(targetSlot);
        });
}

void MainComponent::openMasterPluginEditor(int slotIdx)
{
    auto* plugin = audioEngine.getMasterChain().getPlugin(slotIdx);
    if (!plugin) return;

    for (auto* w : pluginEditorWindows)
        if (w && w->getPlugin() == plugin) { w->toFront(true); return; }

    auto* w = new PluginEditorWindow(*plugin,
        [this](PluginEditorWindow* self) { pluginEditorWindows.removeObject(self); });
    pluginEditorWindows.add(w);

    auto* mappings = commandManager.getKeyMappings();
    w->addKeyListener(mappings);
    std::function<void(juce::Component*)> attach = [&](juce::Component* c)
    {
        if (c == nullptr) return;
        c->addKeyListener(mappings);
        for (auto* child : c->getChildren()) attach(child);
    };
    if (auto* content = w->getContentComponent())
    {
        content->setWantsKeyboardFocus(true);
        attach(content);
    }
}

void MainComponent::removeMasterPlugin(int slotIdx)
{
    if (!audioEngine.getMasterChain().getPlugin(slotIdx)) return;
    undoManager.beginNewTransaction();
    undoManager.perform(new PluginSlotAction(
        makeChainResolver(nullptr), slotIdx, /*afterInstance*/ nullptr,
        [this] { markProjectDirty(); },
        [this](juce::AudioPluginInstance* p) { closePluginEditorFor(p); }));
}

void MainComponent::handlePluginDropFromTrackToMaster(int srcTrack, int srcSlot,
                                                     int dstSlot, bool copy)
{
    // dstTrack = -1 (マスター) として汎用ハンドラに委譲
    handlePluginDropAcrossTracks(srcTrack, srcSlot, /*dstTrack=*/-1, dstSlot, copy);
}

// ── Track プラグイン追加 / エディタオープン ──────────────────────────
void MainComponent::addPluginToTrack(int trackIdx, int slotIdx)
{
    auto* track = trackManager.getTrack(trackIdx);
    if (!track) return;

    auto& knownList = pluginManager.getKnownPluginListRW();
    auto& fmgr      = pluginManager.getFormatManager();

    juce::PopupMenu menu;
    {
        // 先頭に内蔵エフェクトのメーカー「Utawave」サブメニューを固定 (見つけやすさ優先)
        juce::PopupMenu builtin;
        BuiltInFactory::appendMenu(builtin);
        menu.addSubMenu("Utawave", builtin);
        menu.addSeparator();
    }
    juce::KnownPluginList::addToMenu(menu, knownList.getTypes(),
                                     juce::KnownPluginList::sortByManufacturer);

    menu.showMenuAsync(juce::PopupMenu::Options(),
        [this, trackIdx, slotIdx, &knownList, &fmgr](int result)
        {
            if (result <= 0) return;

            // 内蔵エフェクト (予約 ID 域)
            if (auto builtinInst = BuiltInFactory::createFromMenuId(result))
            {
                auto* tr2 = trackManager.getTrack(trackIdx);
                if (!tr2) return;
                const int targetSlot = (slotIdx >= 0)
                                           ? slotIdx
                                           : tr2->getPluginChain().getNumPlugins();
                undoManager.beginNewTransaction();
                undoManager.perform(new PluginSlotAction(
                    makeChainResolver(tr2), targetSlot, std::move(builtinInst),
                    [this] { markProjectDirty(); },
                    [this](juce::AudioPluginInstance* p) { closePluginEditorFor(p); }));
                openPluginEditor(trackIdx, targetSlot);
                return;
            }

            const int typeIdx = juce::KnownPluginList::getIndexChosenByMenu(knownList.getTypes(), result);
            if (typeIdx < 0) return;
            auto desc = knownList.getTypes()[typeIdx];

            const double sr  = audioEngine.getSampleRate() > 0 ? audioEngine.getSampleRate() : 48000.0;
            const int    bs  = 512;

            juce::String err;
            std::unique_ptr<juce::AudioPluginInstance> instance(
                fmgr.createPluginInstance(desc, sr, bs, err));

            if (!instance)
            {
                juce::AlertWindow::showAsync(juce::MessageBoxOptions()
                    .withIconType(juce::MessageBoxIconType::WarningIcon)
                    .withTitle(tr(u8"プラグインの読込に失敗"))
                    .withMessage(err)
                    .withButton("OK"), nullptr);
                return;
            }

            auto* track = trackManager.getTrack(trackIdx);
            if (!track) return;
            // slotIdx 指定があればその位置に、無ければ末尾に
            const int targetSlot = (slotIdx >= 0)
                                       ? slotIdx
                                       : track->getPluginChain().getNumPlugins();
            undoManager.beginNewTransaction();
            undoManager.perform(new PluginSlotAction(
                makeChainResolver(track), targetSlot, std::move(instance),
                [this] { markProjectDirty(); },
                [this](juce::AudioPluginInstance* p) { closePluginEditorFor(p); }));
            openPluginEditor(trackIdx, targetSlot);
        });
}

void MainComponent::openPluginEditor(int trackIdx, int slotIdx)
{
    auto* track = trackManager.getTrack(trackIdx);
    if (!track) return;
    auto* plugin = track->getPluginChain().getPlugin(slotIdx);
    if (!plugin) return;

    // すでに開いていればフォーカスを当てるだけ
    for (auto* w : pluginEditorWindows)
        if (w && w->getPlugin() == plugin) { w->toFront(true); return; }

    auto* w = new PluginEditorWindow(*plugin,
        [this](PluginEditorWindow* self) { pluginEditorWindows.removeObject(self); });
    pluginEditorWindows.add(w);

    // プラグイン GUI 内部の子コンポーネントにフォーカスが当たっていても
    // スペース/トランスポートキーが効くよう、エディタ階層に再帰的に KeyListener を仕込む
    auto* mappings = commandManager.getKeyMappings();
    w->addKeyListener(mappings);

    std::function<void(juce::Component*)> attach = [&](juce::Component* c)
    {
        if (c == nullptr) return;
        c->addKeyListener(mappings);
        for (auto* child : c->getChildren())
            attach(child);
    };
    if (auto* content = w->getContentComponent())
    {
        content->setWantsKeyboardFocus(true);
        attach(content);
    }
}

void MainComponent::showPluginManager()
{
    auto* dlg = new PluginManagerDialog(pluginManager);
    dlg->setSize(800, 540);

    juce::DialogWindow::LaunchOptions opts;
    opts.content.setOwned(dlg);
    opts.dialogTitle                  = tr(u8"プラグイン管理");
    opts.dialogBackgroundColour       = juce::Colour(0xff181a1e);
    opts.escapeKeyTriggersCloseButton = true;
    opts.useNativeTitleBar            = true;
    opts.resizable                    = true;
    opts.launchAsync();
}
