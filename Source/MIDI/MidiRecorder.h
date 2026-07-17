// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

#pragma once
#include <JuceHeader.h>
#include <vector>
#include <algorithm>

// MIDI キーボード録音のキャプチャ列 → クリップシーケンス変換 (純関数・GUI/デバイス非依存)。
// キャプチャ列はタイムライン秒のタイムスタンプ付き MidiMessage (ループ録音のラップで
// 時刻順とは限らない)。ノートは (channel, note) ごとに on/off をペアリングし、
// off が来ないまま録音が終わったノート (押しっぱなし) は stopPos で閉じる。
// テストは Tests/RecordingTests.cpp (testMidiRecorderBuildSequence)。
namespace MidiRecorder
{
    // キャプチャ列からノートの時間範囲 [最初の note-on, 最後のノート終端] を求める。
    // ノートが 1 つも無ければ false (クリップを作らない判定に使う)。
    // 押しっぱなしノートの終端は stopPos。
    inline bool noteSpan(const std::vector<juce::MidiMessage>& events, double stopPos,
                         double& outFirstOn, double& outLastEnd)
    {
        bool any = false;
        double firstOn = 0.0, lastEnd = 0.0;
        // (channel-1)*128 + note → 押下中か
        std::vector<bool> held((size_t) 16 * 128, false);
        bool anyHeld = false;
        for (const auto& m : events)
        {
            if (m.isNoteOn())
            {
                const double t = m.getTimeStamp();
                if (!any || t < firstOn) firstOn = t;
                if (!any || t > lastEnd) lastEnd = t;
                any = true;
                const int idx = (m.getChannel() - 1) * 128 + m.getNoteNumber();
                if (idx >= 0 && idx < (int) held.size()) { held[(size_t) idx] = true; anyHeld = true; }
            }
            else if (m.isNoteOff())
            {
                const int idx = (m.getChannel() - 1) * 128 + m.getNoteNumber();
                if (idx >= 0 && idx < (int) held.size() && held[(size_t) idx])
                {
                    held[(size_t) idx] = false;
                    lastEnd = juce::jmax(lastEnd, m.getTimeStamp());
                }
            }
        }
        if (!any) return false;
        // 押しっぱなしのまま終わったノートがあれば終端は stopPos まで届く
        if (anyHeld)
            for (bool h : held)
                if (h) { lastEnd = juce::jmax(lastEnd, stopPos); break; }
        outFirstOn = firstOn;
        outLastEnd = juce::jmax(lastEnd, firstOn);
        return true;
    }

    // キャプチャ列をクリップ相対シーケンスへ変換する。
    //  - clipStartAbs / clipEndAbs: クリップのタイムライン範囲 (小節スナップ済みを渡す)
    //  - stopPos: 録音停止位置 (押しっぱなしノートをここで閉じる)
    //  - ノートは範囲内へクランプ (最小長 0.01s)。範囲外に完全に出るノートは捨てる
    //  - ノート以外 (CC / ピッチベンド等) は範囲内のものをそのまま通す
    inline juce::MidiMessageSequence buildClipSequence(std::vector<juce::MidiMessage> events,
                                                       double clipStartAbs, double clipEndAbs,
                                                       double stopPos)
    {
        juce::MidiMessageSequence seq;
        const double dur = clipEndAbs - clipStartAbs;
        if (dur <= 0.0) return seq;

        // ループ録音のラップで時刻順とは限らないため先に整列する (安定ソートで同時刻の
        // 順序 = 到着順を保つ。同時刻の off→on の並びを壊さない)
        std::stable_sort(events.begin(), events.end(),
                         [](const juce::MidiMessage& a, const juce::MidiMessage& b)
                         { return a.getTimeStamp() < b.getTimeStamp(); });

        // (channel-1)*128 + note → 開いているノートの開始時刻 (abs) と velocity
        struct Open { double onAbs; float vel; bool active; };
        std::vector<Open> open((size_t) 16 * 128, Open { 0.0, 0.0f, false });

        auto emitNote = [&](int channel1, int note, double onAbs, double offAbs, float vel)
        {
            // クリップ範囲へクランプ (範囲外に完全に出るノートは捨てる)
            const double on  = juce::jmax(onAbs,  clipStartAbs);
            const double off = juce::jmin(offAbs, clipEndAbs);
            if (off - on < 0.001) return;
            auto nOn  = juce::MidiMessage::noteOn (channel1, note, juce::jlimit(0.0f, 1.0f, vel));
            auto nOff = juce::MidiMessage::noteOff(channel1, note);
            nOn.setTimeStamp (on  - clipStartAbs);
            nOff.setTimeStamp(off - clipStartAbs);
            seq.addEvent(nOn);
            seq.addEvent(nOff);
        };

        for (const auto& m : events)
        {
            if (m.isNoteOn())
            {
                const int idx = (m.getChannel() - 1) * 128 + m.getNoteNumber();
                if (idx < 0 || idx >= (int) open.size()) continue;
                auto& o = open[(size_t) idx];
                // 同一ノートの再押下 (off 欠落): 前のノートをここで閉じてから開き直す
                if (o.active)
                    emitNote(m.getChannel(), m.getNoteNumber(), o.onAbs, m.getTimeStamp(), o.vel);
                o = Open { m.getTimeStamp(), m.getFloatVelocity(), true };
            }
            else if (m.isNoteOff())
            {
                const int idx = (m.getChannel() - 1) * 128 + m.getNoteNumber();
                if (idx < 0 || idx >= (int) open.size()) continue;
                auto& o = open[(size_t) idx];
                if (!o.active) continue;   // 対応する on の無い迷子 off は無視
                emitNote(m.getChannel(), m.getNoteNumber(), o.onAbs, m.getTimeStamp(), o.vel);
                o.active = false;
            }
            else
            {
                // CC / ピッチベンド等は範囲内のものをそのまま通す (クリップ相対時刻へ)
                const double t = m.getTimeStamp();
                if (t < clipStartAbs || t >= clipEndAbs) continue;
                auto copy = m;
                copy.setTimeStamp(t - clipStartAbs);
                seq.addEvent(copy);
            }
        }

        // 押しっぱなしのまま録音が終わったノートは停止位置 (クリップ末尾まで) で閉じる
        for (int idx = 0; idx < (int) open.size(); ++idx)
        {
            const auto& o = open[(size_t) idx];
            if (!o.active) continue;
            emitNote(idx / 128 + 1, idx % 128, o.onAbs, juce::jmin(stopPos, clipEndAbs), o.vel);
        }

        seq.sort();
        seq.updateMatchedPairs();
        return seq;
    }
}
