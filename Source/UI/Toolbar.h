// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

#pragma once
#include <JuceHeader.h>

class TransportBar : public juce::Component
{
public:
    TransportBar();
    ~TransportBar() override;

    void paint(juce::Graphics&) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent&) override;

    std::function<void()>     onPlay;
    std::function<void()>     onStop;
    std::function<void()>     onRecord;
    std::function<void()>     onRewind;
    std::function<void()>     onAudioSettings;
    std::function<void()>     onPreferences;   // 歯車ボタン
    std::function<void()>     onImport;        // 読み込み (オーディオ / MIDI を拡張子で判別)
    std::function<void()>     onExport;        // 書き出し
    std::function<void(bool)> onClipGainChanged;
    std::function<void(int)>    onSnapModeSelected;
    std::function<void(bool)>   onLoopToggle;
    std::function<void(int)>    onCountInChanged;   // 0/1/2/4 小節
    std::function<void(double)> onPreRollChanged;   // 0/1/2/3 秒
    std::function<void(int)>    onToolModeChanged;  // 0=Click, 1=Selection, 2=Both
    std::function<void(double)> onBpmChanged;
    std::function<void(bool)>   onMetronomeToggle;
    std::function<void()>       onMetronomeSettings;  // 右クリック等で設定パネルを開く

    void setToolMode(int mode);  // ボタンの点灯状態を更新

    void setSnapLabel(const juce::String& label, bool active);
    void setLoopActive(bool v);
    void setMetronomeActive(bool v);
    void setCountInBars(int bars);    // ボタンのラベル更新
    void setPreRollSecs(double secs); // ボタンのラベル更新

    void setPlaying(bool isPlaying);
    void setRecording(bool isRecording);
    void setBpm(double bpm);
    void setTimePosition(double seconds, int bars, int beats);

private:
    juce::TextButton rewindBtn;
    juce::TextButton stopBtn;
    juce::TextButton playBtn;
    juce::TextButton recBtn;
    juce::TextButton tapTempoBtn;
    juce::TextButton metronomeBtn;
    juce::TextButton audioSettingsBtn;
    juce::TextButton prefsBtn;
    juce::TextButton importBtn;
    juce::TextButton exportBtn;
    juce::TextButton gainBtn;
    juce::TextButton snapBtn;
    juce::TextButton loopBtn;
    juce::TextButton clickToolBtn;
    juce::TextButton linkToolBtn;
    juce::TextButton rangeToolBtn;    // グリッドスナップ
    juce::TextButton countInBtn;
    juce::TextButton preRollBtn;

    juce::Label bpmLabel;
    juce::Label barsBeatLabel;
    juce::Label timeLabel;

    double currentBpm    { 120.0 };
    bool   playing       { false };
    bool   recording     { false };
    int    transportRight  { 0 };
    int    bpmBlockRight   { 0 };
    int    audioBlockWidth { 64 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TransportBar)
};
