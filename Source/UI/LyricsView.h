// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

#pragma once
#include <JuceHeader.h>
#include "../Localisation.h"

// 歌詞表示ビュー: テキストをコピペで貼り付け、歌唱中に最前面の窓で読む用途。
// 常に編集可能 (読み取り専用の表示モードへの切替は廃止)。文字サイズを − / ＋ で調整する。
// テキストはプロジェクト (.uta) に保存され、文字サイズはアプリ全体設定 (AppPreferences) に保存される。
// 歌詞が空のときは .txt ファイルのドラッグ&ドロップで読み込める (子の TextEditor は
// FileDragAndDropTarget ではないため、JUCE が親の LyricsView へドロップをルーティングする)。
class LyricsView : public juce::Component,
                   public juce::FileDragAndDropTarget
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
        // 歌詞が空のときの薄字ガイド (D&D / 読み込み案内)。editor が body を覆うため
        // setTextToShowWhenEmpty で editor 自身に描かせる (未フォーカス + 空のときだけ表示)。
        editor.setTextToShowWhenEmpty(tr(u8"ここにテキストファイル (.txt) をドラッグ & ドロップ、または「.txt を読み込む」から開きます"),
                                      juce::Colours::white.withAlpha(0.35f));
        editor.setText(initialText, false);
        editor.onTextChange = [this] { if (onTextChanged) onTextChanged(editor.getText()); };
        addAndMakeVisible(editor);

        loadButton.setButtonText(tr(u8".txt を読み込む"));
        loadButton.setTooltip(tr(u8"テキストファイル (.txt) から歌詞を読み込む"));
        loadButton.onClick    = [this] { openFileToLoad(); };
        fontDownButton.setButtonText("A-");
        fontUpButton.setButtonText("A+");
        fontDownButton.onClick = [this] { changeFont(-kFontStep); };
        fontUpButton.onClick   = [this] { changeFont(+kFontStep); };
        fontDownButton.setTooltip(tr(u8"文字を小さく"));
        fontUpButton.setTooltip(tr(u8"文字を大きく"));
        addAndMakeVisible(loadButton);
        addAndMakeVisible(fontDownButton);
        addAndMakeVisible(fontUpButton);

        applyFont();
    }

    juce::String getText() const { return editor.getText(); }
    int getFontSize() const      { return fontSize_; }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(juce::Colour(0xff1a1a1a));
        g.setColour(juce::Colour(0xff2a2f35));
        g.fillRect(0, kBarHeight - 1, getWidth(), 1);

        // D&D 中はボディ周囲をアクセント枠でハイライト (editor が中央を覆うので外周だけ見える)
        if (dragHighlight_)
        {
            auto body = getLocalBounds();
            body.removeFromTop(kBarHeight);
            g.setColour(juce::Colour(0xff7a5acc));
            g.drawRoundedRectangle(body.reduced(3).toFloat(), 6.0f, 3.0f);
        }
    }

    void resized() override
    {
        auto bar = getLocalBounds().removeFromTop(kBarHeight).reduced(6, 4);
        // ボタン幅はテキスト実寸に合わせる (固定幅だと短い訳語で中央寄せの余白が目立つため)
        const auto font  = getLookAndFeel().getTextButtonFont(loadButton, bar.getHeight());
        const int  textW = juce::GlyphArrangement::getStringWidthInt(font, loadButton.getButtonText());
        loadButton.setBounds(bar.removeFromLeft(juce::jlimit(72, bar.getWidth() - 96, textW + 24)));
        fontUpButton.setBounds(bar.removeFromRight(44));
        bar.removeFromRight(4);
        fontDownButton.setBounds(bar.removeFromRight(44));

        auto body = getLocalBounds();
        body.removeFromTop(kBarHeight);
        editor.setBounds(body.reduced(8));
    }

    // ── FileDragAndDropTarget: 歌詞が空のときだけ .txt の D&D を受ける ──
    bool isInterestedInFileDrag(const juce::StringArray& files) override
    {
        if (editor.getText().isNotEmpty()) return false;   // 既に歌詞があるときは受けない
        return hasTextFile(files);
    }

    void fileDragEnter(const juce::StringArray& files, int, int) override
    {
        if (isInterestedInFileDrag(files)) { dragHighlight_ = true; repaint(); }
    }

    void fileDragExit(const juce::StringArray&) override
    {
        if (dragHighlight_) { dragHighlight_ = false; repaint(); }
    }

    void filesDropped(const juce::StringArray& files, int, int) override
    {
        dragHighlight_ = false;
        repaint();
        for (const auto& f : files)
        {
            if (isTextFile(f))
            {
                loadFromFile(juce::File(f));
                break;
            }
        }
    }

private:
    static bool isTextFile(const juce::String& path)
    {
        return path.endsWithIgnoreCase(".txt") || path.endsWithIgnoreCase(".text");
    }

    static bool hasTextFile(const juce::StringArray& files)
    {
        for (const auto& f : files)
            if (isTextFile(f)) return true;
        return false;
    }

    void loadFromFile(const juce::File& file)
    {
        if (! file.existsAsFile()) return;
        // loadFileAsString は BOM / UTF-8 / UTF-16 を判別して読む
        editor.setText(file.loadFileAsString(), true);   // sendNotification=true で onTextChanged を発火 (保存される)
        editor.moveCaretToTop(false);
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
                loadFromFile(file);
            });
    }

    juce::TextEditor editor;
    juce::TextButton loadButton;
    juce::TextButton fontDownButton, fontUpButton;
    std::unique_ptr<juce::FileChooser> chooser;
    int  fontSize_ { 16 };
    bool dragHighlight_ { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LyricsView)
};
