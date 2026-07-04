// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

#include "StatusBar.h"
#include "../Localisation.h"
#include "../AppColours.h"

#if JUCE_MAC || JUCE_IOS
 #include <mach/mach.h>
#elif JUCE_WINDOWS
 #include <windows.h>
 #include <psapi.h>
#endif

// プロセスの実使用メモリ (MB)。取得できない環境では 0 を返す。
static double getProcessMemoryMB()
{
   #if JUCE_MAC || JUCE_IOS
    mach_task_basic_info info {};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                  reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS)
        return (double) info.resident_size / (1024.0 * 1024.0);
   #elif JUCE_WINDOWS
    PROCESS_MEMORY_COUNTERS pmc {};
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
        return (double) pmc.WorkingSetSize / (1024.0 * 1024.0);
   #endif
    return 0.0;
}

StatusBar::StatusBar()
{
    startTimerHz(1);
}

StatusBar::~StatusBar()
{
    stopTimer();
}

void StatusBar::timerCallback()
{
    cpuUsage = getCpuPercent ? getCpuPercent() : 0.0;
    memoryMB = getProcessMemoryMB();
    if (message.isNotEmpty() && juce::Time::getMillisecondCounter() >= messageExpiresAtMs)
        message.clear();

    // 表示される値が変化したときだけ再描画する (何も変わらないなら repaint しない)。
    const double shownCpu     = juce::String(cpuUsage, 1).getDoubleValue();
    const double shownMemory  = juce::String(memoryMB, 0).getDoubleValue();
    const bool   messageShown = message.isNotEmpty();

    const bool changed = shownCpu     != lastShownCpu
                      || shownMemory  != lastShownMemory
                      || messageShown != lastMessageVisible
                      || message      != lastShownMessage;

    if (!changed)
        return;

    lastShownCpu       = shownCpu;
    lastShownMemory    = shownMemory;
    lastShownMessage   = message;
    lastMessageVisible = messageShown;

    repaint();
}

void StatusBar::setMessage(const juce::String& text, int durationMs)
{
    message = text;
    messageExpiresAtMs = juce::Time::getMillisecondCounter() + (juce::uint32)juce::jmax(0, durationMs);
    repaint();
}

void StatusBar::paint(juce::Graphics& g)
{
    g.fillAll(AppColours::panelBg);

    g.setColour(AppColours::separator);
    g.drawLine(0.0f, 0.0f, (float)getWidth(), 0.0f, 1.0f);

    g.setColour(AppColours::textDim);
    g.setFont(juce::FontOptions(11.0f));

    auto drawItem = [&](const juce::String& text, int x, int w)
    {
        g.drawText(text, x, 0, w, getHeight(), juce::Justification::centred);
        g.setColour(AppColours::separator);
        g.drawLine((float)(x + w), 2.0f, (float)(x + w), (float)(getHeight() - 2), 1.0f);
        g.setColour(AppColours::textDim);
    };

    int x = 8;

    drawItem(juce::String::formatted("CPU: %.1f%%", cpuUsage), x, 120);          x += 120;
    drawItem(juce::String::formatted("Memory: %.0f MB", memoryMB), x, 140);      x += 140;

    // ── 右端: ショートカット一覧へのヒント (クリックで開く) ──
    {
        // Mac は ⌘/ ・非 Mac は Ctrl+/ (メニューのショートカット列と同じ変換ヘルパに集約)
        const juce::String keyHint = platformMenuShortcut(juce::String::fromUTF8(u8"⌘/"));
        const juce::String hint = keyHint + "  " + tr(u8"ショートカット");
        const int hw = 138;
        helpHintBounds = juce::Rectangle<int>(getWidth() - hw, 0, hw, getHeight());

        // 左に区切り線
        g.setColour(AppColours::separator);
        g.drawLine((float) helpHintBounds.getX(), 2.0f,
                   (float) helpHintBounds.getX(), (float) (getHeight() - 2), 1.0f);

        g.setColour(helpHover ? AppColours::textBright : AppColours::textDim);
        g.setFont(juce::FontOptions(11.0f));
        g.drawText(hint, helpHintBounds.reduced(8, 0), juce::Justification::centred);
    }

    // ── ショートカットの左: 歌詞パッドを開くヒント (クリックで開く) ──
    {
        const int lw = 92;
        lyricsHintBounds = juce::Rectangle<int>(helpHintBounds.getX() - lw, 0, lw, getHeight());

        // 左に区切り線
        g.setColour(AppColours::separator);
        g.drawLine((float) lyricsHintBounds.getX(), 2.0f,
                   (float) lyricsHintBounds.getX(), (float) (getHeight() - 2), 1.0f);

        g.setColour(lyricsHover ? AppColours::textBright : AppColours::textDim);
        g.setFont(juce::FontOptions(11.0f));
        g.drawText(tr(u8"歌詞パッド"), lyricsHintBounds.reduced(8, 0), juce::Justification::centred);
    }

    // メッセージ域は左の固定項目と右端ヒント群の間。極端に狭い幅でも負にならないようクランプ。
    const int msgX = x + 8;
    const int msgW = juce::jmax(0, (lyricsHintBounds.getX() - 8) - msgX);

    // 波形ロード中は進捗を優先表示。完了 (remaining==0) 後は通常メッセージを表示。
    if (wfRemaining > 0)
    {
        const int loaded = juce::jmax(0, wfTotal - wfRemaining);
        g.setColour(AppColours::accent);
        g.setFont(juce::FontOptions(11.0f));
        g.drawText(tr(u8"波形を読み込み中… ")
                       + juce::String(loaded) + " / " + juce::String(wfTotal),
                   msgX, 0, msgW, getHeight(),
                   juce::Justification::centredLeft);
    }
    else if (message.isNotEmpty())
    {
        // 「Memory:」等の固定項目と同じ控えめな色で目立ちすぎないようにする
        g.setColour(AppColours::textDim);
        g.setFont(juce::FontOptions(11.0f));
        g.drawText(message, msgX, 0, msgW, getHeight(),
                   juce::Justification::centredLeft);
    }
}

void StatusBar::resized() {}

void StatusBar::mouseDown(const juce::MouseEvent& e)
{
    if (helpHintBounds.contains(e.getPosition()) && onHelpClicked)
        onHelpClicked();
    else if (lyricsHintBounds.contains(e.getPosition()) && onLyricsClicked)
        onLyricsClicked();
}

void StatusBar::mouseMove(const juce::MouseEvent& e)
{
    const bool overHelp   = helpHintBounds.contains(e.getPosition());
    const bool overLyrics = lyricsHintBounds.contains(e.getPosition());
    setMouseCursor((overHelp || overLyrics) ? juce::MouseCursor::PointingHandCursor
                                            : juce::MouseCursor::NormalCursor);
    if (overHelp != helpHover)
    {
        helpHover = overHelp;
        repaint(helpHintBounds);
    }
    if (overLyrics != lyricsHover)
    {
        lyricsHover = overLyrics;
        repaint(lyricsHintBounds);
    }
}

void StatusBar::mouseExit(const juce::MouseEvent&)
{
    if (helpHover)
    {
        helpHover = false;
        repaint(helpHintBounds);
    }
    if (lyricsHover)
    {
        lyricsHover = false;
        repaint(lyricsHintBounds);
    }
}
