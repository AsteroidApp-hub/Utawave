// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

#include "MidiImporter.h"
#include "../Localisation.h"
#include <algorithm>
#include <cmath>
#include <vector>

namespace
{
    inline juce::uint32 beInt32(const juce::uint8* p)
    {
        return ((juce::uint32) p[0] << 24) | ((juce::uint32) p[1] << 16)
             | ((juce::uint32) p[2] << 8)  |  (juce::uint32) p[3];
    }

    inline void writeBeInt32(juce::MemoryBlock& mb, juce::uint32 v)
    {
        const juce::uint8 b[4] = { (juce::uint8)(v >> 24), (juce::uint8)(v >> 16),
                                   (juce::uint8)(v >> 8),  (juce::uint8) v };
        mb.append(b, 4);
    }

    inline void writeBeInt16(juce::MemoryBlock& mb, juce::uint16 v)
    {
        const juce::uint8 b[2] = { (juce::uint8)(v >> 8), (juce::uint8) v };
        mb.append(b, 2);
    }

    // 一部の SMF (例: Yamaha XF) は規定トラックの後に独自チャンク (XFIH/XFKM 等) を付与する。
    // JUCE の MidiFile::readFrom は宣言トラック数を読んだ後に末尾バイトが残ると false を返す
    // (`size == 0` を要求) ため、こうしたファイルが「解析できません」になる。MThd と MTrk
    // チャンクだけを抽出した最小 SMF を組み直して読み直すことで、末尾の独自チャンクを構造的に
    // 捨てて取り込めるようにする (JUCE の堅牢なトラック解析はそのまま使う)。
    // raw が標準的に解釈できれば out に再構成した SMF を書いて true、それ以外は false。
    bool extractStandardChunks(const juce::MemoryBlock& raw, juce::MemoryBlock& out)
    {
        const auto* data = static_cast<const juce::uint8*>(raw.getData());
        const size_t n = raw.getSize();
        if (data == nullptr || n < 14) return false;

        // MThd を探す (通常は先頭。RIFF ラップ等で前置されている場合も拾えるよう前方検索)
        size_t hdr = 0;
        bool found = false;
        const size_t limit = (n >= 4) ? (n - 4) : 0;
        for (size_t i = 0; i <= limit; ++i)
            if (data[i] == 'M' && data[i+1] == 'T' && data[i+2] == 'h' && data[i+3] == 'd')
            { hdr = i; found = true; break; }
        if (! found || hdr + 14 > n) return false;

        const juce::uint32 headerLen = beInt32(data + hdr + 4);
        const juce::uint16 origFormat = (juce::uint16) ((data[hdr+8]  << 8) | data[hdr+9]);
        const juce::uint16 division   = (juce::uint16) ((data[hdr+12] << 8) | data[hdr+13]);
        size_t pos = hdr + 8 + headerLen;   // ヘッダチャンク本体の直後
        if (pos > n) return false;

        // MTrk チャンクを全て収集 (id != MTrk のチャンクはスキップ)
        std::vector<std::pair<size_t, size_t>> trackChunks;  // (オフセット, 全長)
        while (pos + 8 <= n)
        {
            const bool isMtrk = (data[pos] == 'M' && data[pos+1] == 'T'
                              && data[pos+2] == 'r' && data[pos+3] == 'k');
            const juce::uint32 len = beInt32(data + pos + 4);
            const size_t whole = (size_t) 8 + len;
            if (pos + whole > n) break;   // 切り詰められたチャンク → ここで打ち切り
            if (isMtrk)
                trackChunks.push_back({ pos, whole });
            pos += whole;
        }
        if (trackChunks.empty()) return false;

        // format 0 は ntrks==1 が必須 (JUCE がそうチェックする)。収集トラックが複数なら 1 へ昇格。
        const size_t count = trackChunks.size();
        juce::uint16 format = origFormat;
        if (count != 1 && format == 0) format = 1;

        out.reset();
        out.append("MThd", 4);
        writeBeInt32(out, 6);
        writeBeInt16(out, format);
        writeBeInt16(out, (juce::uint16) count);
        writeBeInt16(out, division);
        for (const auto& tc : trackChunks)
            out.append(data + tc.first, tc.second);
        return true;
    }

    // チャンネル分割したパートのトラック名 = 楽器名 (General MIDI Level 1)。
    // 音色名は JUCE の juce::MidiMessage::getGMInstrumentName (range 外は nullptr) を使う
    // (汎用名なので商標問題なし)。drum (ch10) は "Drums"、program 未指定は "ch N" にフォールバック
    // (同じ楽器が複数チャンネルにあると同名になるが、ユーザー要望によりチャンネル番号は付けない)。
    juce::String midiPartName(int channel0, int program, bool drum)
    {
        if (drum) return "Drums";
        if (const char* gm = juce::MidiMessage::getGMInstrumentName(program);
            gm != nullptr && *gm != '\0')
            return juce::String(gm);
        return "ch " + juce::String(channel0 + 1);
    }
}

MidiImporter::ImportResult MidiImporter::load(const juce::File& smfFile)
{
    ImportResult result;
    if (!smfFile.existsAsFile())
    {
        result.error = tr(u8"ファイルが見つかりません");
        return result;
    }

    juce::FileInputStream stream(smfFile);
    if (!stream.openedOk())
    {
        result.error = tr(u8"ファイルを開けません");
        return result;
    }

    juce::MidiFile mf;
    if (!mf.readFrom(stream))
    {
        // 末尾に独自チャンク (Yamaha XF の XFIH/XFKM 等) を持つ SMF は JUCE が余剰バイトを
        // 嫌って false を返す。MThd + MTrk だけを抽出し直して読み直す (上記コメント参照)。
        bool recovered = false;
        juce::MemoryBlock raw, cleaned;
        stream.setPosition(0);
        if (stream.readIntoMemoryBlock(raw) && extractStandardChunks(raw, cleaned))
        {
            juce::MemoryInputStream cleanedStream(cleaned, false);
            recovered = mf.readFrom(cleanedStream);
        }
        if (! recovered)
        {
            result.error = tr(u8"SMF を解析できません");
            return result;
        }
    }

    // ── 拍子マップは「ティック」のまま収集する（秒へ変換する前）。
    //    拍子変化の小節番号 (bar index) は譜面構造そのものでテンポに依存しない。
    //    秒へ変換してから initialBpm で拍数換算すると、途中にテンポ変化があると小節番号が
    //    ずれてしまう (旧実装のバグ)。ティックから直接求めればテンポ非依存で正確になる。 ──
    const short  timeFormat = mf.getTimeFormat();
    const double ppq = (timeFormat > 0) ? (double) timeFormat : 0.0;  // 正なら PPQ、負は SMPTE
    {
        struct MeterTick { double tick; int num; int den; };
        std::vector<MeterTick> meterTicks;
        for (int t = 0; t < mf.getNumTracks(); ++t)
            if (auto* seq = mf.getTrack(t))
                for (int i = 0; i < seq->getNumEvents(); ++i)
                {
                    const auto& m = seq->getEventPointer(i)->message;
                    if (m.isTimeSignatureMetaEvent())
                    {
                        int n = 4, d = 4;
                        m.getTimeSignatureInfo(n, d);
                        meterTicks.push_back({ m.getTimeStamp(),
                                               juce::jmax(1, n), juce::jmax(1, d) });
                    }
                }
        std::sort(meterTicks.begin(), meterTicks.end(),
                  [](const MeterTick& a, const MeterTick& b) { return a.tick < b.tick; });

        // 同一ティックの重複 (複数トラックに同じ拍子が入る等) は後勝ちで 1 件に統合
        std::vector<MeterTick> uniq;
        for (const auto& mt : meterTicks)
        {
            if (! uniq.empty() && std::abs(uniq.back().tick - mt.tick) < 1.0e-6)
                uniq.back() = mt;
            else
                uniq.push_back(mt);
        }

        // 先頭の拍子イベント (通常ティック 0) を曲頭の拍子とする
        size_t startIdx = 0;
        double prevTick = 0.0;
        int    prevNum  = result.meterNumerator;   // 既定 4 (拍子イベントが無い場合)
        if (! uniq.empty() && uniq[0].tick <= 1.0e-6)
        {
            result.meterNumerator   = uniq[0].num;
            result.meterDenominator = uniq[0].den;
            prevNum  = uniq[0].num;
            prevTick = uniq[0].tick;
            startIdx = 1;
        }

        // 残りを meterChanges (0-based barIndex) へ。アプリのモデルは
        // 「1 拍 = 四分音符・1 小節 = numerator 拍」(AppSettings::getMeterAtBar / beatsAtTime と一致)
        // なので 1 小節 = numerator × PPQ ティック。これは MidiExporter::barStartTicks の逆変換で、
        // 書き出し → 読み込みの往復が一致する。
        if (ppq > 0.0)
        {
            double cumBars = 0.0;
            for (size_t i = startIdx; i < uniq.size(); ++i)
            {
                const double ticksPerBar = ppq * (double) juce::jmax(1, prevNum);
                cumBars += (uniq[i].tick - prevTick) / ticksPerBar;
                const int barIndex = juce::jmax(0, juce::roundToInt(cumBars));
                result.meterChanges.push_back({ barIndex, { uniq[i].num, uniq[i].den } });
                prevTick = uniq[i].tick;
                prevNum  = uniq[i].num;
            }
        }
    }

    // ティック→秒変換（テンポマップを反映）
    mf.convertTimestampTicksToSeconds();

    // ── テンポ / マーカーを秒で収集（変換後）──
    // 最初のテンポを initialBpm に、それ以降を tempoChanges に積む。
    // (SMF Type 1 ではテンポマップは通常トラック 0 にあるが、念のため全トラック走査)
    {
        bool firstTempoSeen = false;
        for (int t = 0; t < mf.getNumTracks(); ++t)
            if (auto* seq = mf.getTrack(t))
                for (int i = 0; i < seq->getNumEvents(); ++i)
                {
                    const auto& m = seq->getEventPointer(i)->message;
                    if (m.isTempoMetaEvent())
                    {
                        const double secsPerQuarter = m.getTempoSecondsPerQuarterNote();
                        if (secsPerQuarter > 0.0)
                        {
                            const double bpm = 60.0 / secsPerQuarter;
                            if (! firstTempoSeen) { result.initialBpm = bpm; firstTempoSeen = true; }
                            else                   result.tempoChanges.push_back({ m.getTimeStamp(), bpm });
                        }
                    }
                    else if (m.isTextMetaEvent() && m.getMetaEventType() == 0x06)  // Marker
                    {
                        result.markers.push_back({ m.getTimeStamp(), m.getTextFromTextMetaEvent() });
                    }
                }
    }

    double maxEnd = 0.0;

    for (int t = 0; t < mf.getNumTracks(); ++t)
    {
        auto* seq = mf.getTrack(t);
        if (seq == nullptr) continue;

        juce::String baseName = "Track " + juce::String(t + 1);

        // 演奏イベントを MIDI チャンネル単位に振り分ける。
        // Type 0 (全パートが 1 MTrk に同居) や、複数チャンネルを含む Type 1 トラックを、
        // チャンネルごとに別トラックへ分割する (各楽器が独立した Utawave トラックになる)。
        // 単一チャンネルのトラックは従来どおり 1 トラックのまま (名前も尊重)。
        std::array<juce::MidiMessageSequence, 16> perCh;
        std::array<int, 16> noteCount{};
        std::array<int, 16> firstProgram;
        firstProgram.fill(-1);

        for (int i = 0; i < seq->getNumEvents(); ++i)
        {
            const auto& m = seq->getEventPointer(i)->message;

            // トラック名 Meta (このトラック全体の基準名)
            if (m.isTrackNameEvent() || (m.isTextMetaEvent() && m.getMetaEventType() == 0x03))
            {
                auto n = m.getTextFromTextMetaEvent().trim();
                if (n.isNotEmpty()) baseName = n;
                continue;
            }

            // 演奏イベントだけをチャンネル別に取り込む
            if (m.isNoteOnOrOff() || m.isPitchWheel() || m.isController()
                || m.isProgramChange() || m.isAftertouch() || m.isChannelPressure()
                || m.isSustainPedalOn() || m.isSustainPedalOff())
            {
                const int ch = m.getChannel() - 1;   // 1..16 -> 0..15 (0 は非チャンネルメッセージ)
                if (ch < 0 || ch >= 16) continue;
                perCh[(size_t)ch].addEvent(m);  // タイムスタンプは秒 (convertTimestampTicksToSeconds 済み)
                if (m.isNoteOn()) ++noteCount[(size_t)ch];
                if (m.isProgramChange() && firstProgram[(size_t)ch] < 0)
                    firstProgram[(size_t)ch] = m.getProgramChangeNumber();
                maxEnd = juce::jmax(maxEnd, m.getTimeStamp());
            }
        }

        int channelsWithNotes = 0;
        for (int ch = 0; ch < 16; ++ch)
            if (noteCount[(size_t)ch] > 0) ++channelsWithNotes;

        for (int ch = 0; ch < 16; ++ch)
        {
            if (noteCount[(size_t)ch] <= 0) continue;   // ノートの無いチャンネルは取り込まない

            ImportedTrack out;
            out.sequence = perCh[(size_t)ch];
            out.sequence.updateMatchedPairs();          // Note On/Off の対応付け
            out.primaryChannel  = ch;
            out.isDrum          = (ch == 9);
            out.numNoteOnEvents = noteCount[(size_t)ch];
            // 1 チャンネルだけのトラックはトラック名を尊重 (Type 1 の通常ケース)。
            // 複数チャンネルが同居していた場合はチャンネル + 楽器名で各パートを区別する。
            out.name = (channelsWithNotes <= 1)
                           ? baseName
                           : midiPartName(ch, firstProgram[(size_t)ch], out.isDrum);
            result.tracks.push_back(std::move(out));
        }
    }

    result.endTimeSecs = maxEnd;
    result.ok = true;
    return result;
}
