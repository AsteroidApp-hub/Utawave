// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

#pragma once
#include <JuceHeader.h>

class StatusBar : public juce::Component, private juce::Timer
{
public:
    StatusBar();
    ~StatusBar() override;

    void paint(juce::Graphics&) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseMove(const juce::MouseEvent&) override;
    void mouseExit(const juce::MouseEvent&) override;

    // 旧表示項目 (SR / bit / Tracks / Duration) は廃止。呼び出し側互換のため no-op で残す。
    void setSampleRate(int)    {}
    void setBitDepth(int)      {}
    void setTrackCount(int)    {}
    void setDuration(double)   {}
    void setMessage(const juce::String& text, int durationMs);
    // 波形ロードの進捗 (remaining>0 で「読み込み中 …/…」を常時表示、0 で消える)
    void setWaveformProgress(int remaining, int total)
    {
        wfRemaining = remaining; wfTotal = total; repaint();
    }

    // CPU 使用率 (%) を返すコールバック (オーディオスレッド負荷)
    std::function<double()> getCpuPercent;

    // 右端ヒント (ショートカット一覧) クリック時のコールバック
    std::function<void()> onHelpClicked;
    // ショートカットの左「歌詞パッド」クリック時のコールバック
    std::function<void()> onLyricsClicked;

private:
    void timerCallback() override;

    // 右端の「ショートカット一覧」ヒント領域 (paint で更新、当たり判定に使用)
    juce::Rectangle<int> helpHintBounds;
    bool helpHover { false };
    // その左の「歌詞パッド」ヒント領域
    juce::Rectangle<int> lyricsHintBounds;
    bool lyricsHover { false };

    double cpuUsage   { 0.0 };
    double memoryMB   { 0.0 };
    juce::String   message;
    juce::uint32   messageExpiresAtMs { 0 };
    int            wfRemaining { 0 };
    int            wfTotal     { 0 };

    // 直近に表示した値をキャッシュし、変化が無いときの repaint を抑制する
    double         lastShownCpu       { -1.0 };
    double         lastShownMemory    { -1.0 };
    juce::String   lastShownMessage;
    bool           lastMessageVisible { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(StatusBar)
};
