// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

#pragma once
#include <JuceHeader.h>
#include "../Localisation.h"

// 歌詞表示ビュー: テキストをコピペで貼り付け、歌唱中に最前面の窓で読む用途。
// 編集モード (貼り付け / 編集) ↔ 表示モード (読み取り専用・大きな文字) を上部ボタンで
// 切り替え、文字サイズを − / ＋ で調整する。テキストはプロジェクト (.uta) に保存され、
// 文字サイズはアプリ全体設定 (AppPreferences) に保存される。
class LyricsView : public juce::Component
{
public:
    static constexpr int kMinFont   = 12;
    static constexpr int kMaxFont   = 96;
    static constexpr int kFontStep  = 2;
    static constexpr int kBarHeight = 34;

    // テキストが編集されたときの通知 (プロジェクトを dirty にする)
    std::function<void(const juce::String&)> onTextChanged;
    // 文字サイズが変わったときの通知 (アプリ全体設定へ永続化)
    std::function<void(int)> onFontSizeChanged;

    LyricsView(const juce::String& initialText, int fontSize)
        : fontSize_(juce::jlimit(kMinFont, kMaxFont, fontSize))
    {
        setOpaque(true);

        editor.setMultiLine(true, true);
        editor.setReturnKeyStartsNewLine(true);
        editor.setScrollbarsShown(true);
        editor.setColour(juce::TextEditor::backgroundColourId,      juce::Colour(0xff14181c));
        editor.setColour(juce::TextEditor::textColourId,            juce::Colours::white);
        editor.setColour(juce::TextEditor::outlineColourId,         juce::Colours::transparentBlack);
        editor.setColour(juce::TextEditor::focusedOutlineColourId,  juce::Colours::transparentBlack);
        editor.setColour(juce::TextEditor::highlightColourId,       juce::Colour(0xff3a5a78));
        editor.setText(initialText, false);
        editor.onTextChange = [this] { if (onTextChanged) onTextChanged(editor.getText()); };
        addAndMakeVisible(editor);

        modeButton.onClick    = [this] { setEditMode(!editing); };
        loadButton.setButtonText(tr(u8"読み込み"));
        loadButton.setTooltip(tr(u8"テキストファイルから歌詞を読み込む"));
        loadButton.onClick    = [this] { openFileToLoad(); };
        fontDownButton.setButtonText("A-");
        fontUpButton.setButtonText("A+");
        fontDownButton.onClick = [this] { changeFont(-kFontStep); };
        fontUpButton.onClick   = [this] { changeFont(+kFontStep); };
        fontDownButton.setTooltip(tr(u8"文字を小さく"));
        fontUpButton.setTooltip(tr(u8"文字を大きく"));
        addAndMakeVisible(modeButton);
        addAndMakeVisible(loadButton);
        addAndMakeVisible(fontDownButton);
        addAndMakeVisible(fontUpButton);

        applyFont();
        // 内容があれば読みやすい表示モードで、空なら貼り付けやすい編集モードで開始
        setEditMode(initialText.isEmpty());
    }

    juce::String getText() const { return editor.getText(); }
    int getFontSize() const      { return fontSize_; }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(juce::Colour(0xff1a1a1a));
        g.setColour(juce::Colour(0xff2a2f35));
        g.fillRect(0, kBarHeight - 1, getWidth(), 1);
    }

    void resized() override
    {
        auto bar = getLocalBounds().removeFromTop(kBarHeight).reduced(6, 4);
        modeButton.setBounds(bar.removeFromLeft(104));
        bar.removeFromLeft(6);
        loadButton.setBounds(bar.removeFromLeft(88));
        fontUpButton.setBounds(bar.removeFromRight(44));
        bar.removeFromRight(4);
        fontDownButton.setBounds(bar.removeFromRight(44));

        auto body = getLocalBounds();
        body.removeFromTop(kBarHeight);
        editor.setBounds(body.reduced(8));
    }

private:
    void setEditMode(bool shouldEdit)
    {
        editing = shouldEdit;
        editor.setReadOnly(!editing);
        editor.setCaretVisible(editing);
        // 表示モードでは編集できないが、選択・スクロールは可能なまま
        modeButton.setButtonText(editing ? tr(u8"表示モード") : tr(u8"編集モード"));
        modeButton.setTooltip(editing ? tr(u8"大きな文字で読む表示に切り替える")
                                      : tr(u8"歌詞を貼り付け / 編集する"));
        if (editing)
            editor.grabKeyboardFocus();
    }

    void changeFont(int delta)
    {
        const int next = juce::jlimit(kMinFont, kMaxFont, fontSize_ + delta);
        if (next == fontSize_) return;
        fontSize_ = next;
        applyFont();
        if (onFontSizeChanged) onFontSizeChanged(fontSize_);
    }

    void applyFont()
    {
        editor.applyFontToAllText(juce::Font((float) fontSize_));
    }

    void openFileToLoad()
    {
        chooser = std::make_unique<juce::FileChooser>(
            tr(u8"歌詞テキストを読み込む"),
            juce::File::getSpecialLocation(juce::File::userDocumentsDirectory),
            "*.txt;*.text");
        chooser->launchAsync(
            juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
            [this, safe = juce::Component::SafePointer<LyricsView>(this)](const juce::FileChooser& fc)
            {
                // ファイル選択中に窓を閉じる / プロジェクトを閉じると LyricsView が破棄され得る
                // (mac の非同期パネルはアプリをブロックしない)。生 this の参照前に生存確認する。
                if (safe == nullptr) return;
                auto file = fc.getResult();
                if (file == juce::File()) return;   // キャンセル
                // loadFileAsString は BOM / UTF-8 / UTF-16 を判別して読む
                const auto text = file.loadFileAsString();
                editor.setText(text, true);         // sendNotification=true で onTextChanged を発火 (保存される)
                editor.moveCaretToTop(false);
                // 読み込んだ歌詞はそのまま読む用途が多いので表示モードへ
                setEditMode(false);
            });
    }

    juce::TextEditor editor;
    juce::TextButton modeButton;
    juce::TextButton loadButton;
    juce::TextButton fontDownButton, fontUpButton;
    std::unique_ptr<juce::FileChooser> chooser;
    int  fontSize_ { 16 };
    bool editing { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LyricsView)
};
