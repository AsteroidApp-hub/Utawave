// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

#include "TrackManager.h"
#include "../Localisation.h"
#include <algorithm>

TrackManager::TrackManager(juce::AudioFormatManager& fmt)
    : formatManager(fmt)
{}

Track* TrackManager::addTrack(const juce::String& name, bool stereo)
{
    juce::String n = name;
    if (n.isEmpty())
    {
        // 既存トラックを走査して "Track N" の最大 N + 1 を採番
        // (セッション横断のカウンタを持たないので、プロジェクトを開き直しても
        //  常に矛盾の無い番号が振られる)
        int maxN = 0;
        for (auto& t : tracks)
        {
            const auto& tn = t->getName();
            if (tn.startsWith("Track "))
            {
                const int v = tn.substring(6).getIntValue();
                if (v > maxN) maxN = v;
            }
        }
        n = "Track " + juce::String(maxN + 1);
    }
    auto track = std::make_unique<Track>(n, formatManager, thumbnailCache,
                                          Track::paletteColour(nextColourIndex++));
    track->setStereo(stereo);
    // 新規トラックは既存トラックの INS スロット表示状態を引き継ぐ。
    // (INS 表示中に追加したトラックだけ INS が隠れる、という不整合を防ぐ)
    bool insVisible = false;
    for (auto& t : tracks)
        if (t->isInsertSlotsVisible()) { insVisible = true; break; }
    track->setInsertSlotsVisible(insVisible);
    tracks.push_back(std::move(track));
    if (onChanged) onChanged();
    return tracks.back().get();
}

Track* TrackManager::addClickTrack()
{
    if (hasClickTrack()) return nullptr;
    auto* t = new Track("Click", formatManager, thumbnailCache);
    t->setClickTrack(true);
    // 既存トラックの INS スロット表示状態を引き継ぐ (addTrack と同じ。CLICK にも EQ 等を
    // 挿せるよう INS スロットを出す。表示中に追加した CLICK だけ INS が隠れる不整合を防ぐ)
    bool insVisible = false;
    for (auto& tr : tracks)
        if (tr->isInsertSlotsVisible()) { insVisible = true; break; }
    t->setInsertSlotsVisible(insVisible);
    tracks.push_back(std::unique_ptr<Track>(t));
    if (onChanged) onChanged();
    return t;
}

bool TrackManager::hasClickTrack() const
{
    for (auto& t : tracks) if (t->isClickTrack()) return true;
    return false;
}

Track* TrackManager::getClickTrack() const
{
    for (auto& t : tracks) if (t->isClickTrack()) return t.get();
    return nullptr;
}

bool TrackManager::hasMidiTrack() const
{
    for (auto& t : tracks) if (t->isMidiTrack()) return true;
    return false;
}

void TrackManager::moveTrack(int from, int to)
{
    if (from < 0 || from >= (int) tracks.size()) return;
    to = juce::jlimit(0, (int) tracks.size() - 1, to);
    if (from == to) return;
    auto t = std::move(tracks[(size_t) from]);
    tracks.erase(tracks.begin() + from);
    tracks.insert(tracks.begin() + to, std::move(t));
    if (onChanged) onChanged();
}

bool TrackManager::reorderTo(const std::vector<Track*>& desired)
{
    // desired が現在のトラック集合の純粋な並べ替えであることを、move する前に検証する
    // (数一致 + 現在の各トラックが desired に含まれる → 重複なしの permutation)。
    if (desired.size() != tracks.size()) return false;
    for (auto& up : tracks)
        if (std::find(desired.begin(), desired.end(), up.get()) == desired.end())
            return false;

    std::vector<std::unique_ptr<Track>> rebuilt;
    rebuilt.reserve(tracks.size());
    for (auto* want : desired)
        for (auto& up : tracks)
            if (up && up.get() == want) { rebuilt.push_back(std::move(up)); break; }

    tracks = std::move(rebuilt);
    if (onChanged) onChanged();
    return true;
}

void TrackManager::removeTrack(int index)
{
    if (index >= 0 && index < (int)tracks.size())
    {
        tracks.erase(tracks.begin() + index);
        if (onChanged) onChanged();
    }
}

std::unique_ptr<Track> TrackManager::extractTrack(int index)
{
    if (index < 0 || index >= (int)tracks.size()) return nullptr;
    auto t = std::move(tracks[(size_t)index]);
    tracks.erase(tracks.begin() + index);
    if (onChanged) onChanged();
    return t;
}

void TrackManager::insertTrack(int index, std::unique_ptr<Track> track)
{
    if (!track) return;
    const int idx = juce::jlimit(0, (int)tracks.size(), index);
    tracks.insert(tracks.begin() + idx, std::move(track));
    if (onChanged) onChanged();
}

int TrackManager::indexOf(const Track* t) const
{
    for (size_t i = 0; i < tracks.size(); ++i)
        if (tracks[i].get() == t) return (int)i;
    return -1;
}

Track* TrackManager::duplicateTrack(int sourceIdx)
{
    if (sourceIdx < 0 || sourceIdx >= (int) tracks.size()) return nullptr;
    auto* src = tracks[(size_t) sourceIdx].get();
    if (!src || src->isClickTrack()) return nullptr;  // Click は複製不可

    // 名前: "(コピー)" を付与。同名が既にあれば連番
    auto baseName = src->getName() + tr(u8" (コピー)");
    auto nameUnique = [this](juce::String s) -> juce::String
    {
        int n = 1;
        juce::String candidate = s;
        while (true)
        {
            bool dup = false;
            for (auto& t : tracks)
                if (t->getName() == candidate) { dup = true; break; }
            if (!dup) return candidate;
            ++n;
            candidate = s + " " + juce::String(n);
        }
    };
    auto dst = std::make_unique<Track>(nameUnique(baseName), formatManager, thumbnailCache);

    // 基本プロパティをコピー (録音アーム・ソロは混乱を避けるため引き継がない)
    dst->setColour          (src->getColour());
    dst->setVolume          (src->getVolume());
    dst->setPan             (src->getPan());
    dst->setReverbSend      (src->getReverbSend());
    dst->setMuted           (src->isMuted());
    dst->setInputMonitor    (src->isInputMonitor());
    dst->setInputChannel    (src->getInputChannel());
    dst->setStereo          (src->isStereo());
    dst->setMidiTrack       (src->isMidiTrack());
    dst->setSynthWaveform   (src->getSynthWaveform());
    dst->setSynthEnabled    (src->isSynthEnabled());
    dst->setOctaveShift     (src->getOctaveShift());
    dst->setSemitoneTranspose(src->getSemitoneTranspose());
    dst->setClickSound      (src->getClickSound());
    dst->setClickAccent     (src->isClickAccent());
    dst->setClickRate       (src->getClickRate());
    dst->setCustomHeight    (src->getCustomHeight());
    dst->setCustomLaneHeight(src->getLaneHeight());
    dst->setInsertSlotsVisible(src->isInsertSlotsVisible());
    dst->setLanesCollapsed  (src->isLanesCollapsed());

    // オーディオクリップ: 全レーンの全クリップをコピー
    for (int li = 0; li < src->getLaneCount(); ++li)
    {
        auto* srcLane = src->getLane(li);
        if (!srcLane) continue;
        auto* dstLane = dst->ensureLane(li);
        dstLane->muted  = srcLane->muted.load();
        dstLane->soloed = srcLane->soloed.load();
        for (auto& srcClip : srcLane->clips)
        {
            if (!srcClip) continue;
            auto* nc = dstLane->addClip(srcClip->getFile(),
                                         srcClip->getStartPosition(),
                                         srcClip->getDuration(),
                                         formatManager, thumbnailCache);
            if (!nc) continue;
            nc->setFileOffset  (srcClip->getFileOffset());
            nc->setGain        (srcClip->getGain());
            if (srcClip->getName().isNotEmpty()) nc->setName(srcClip->getName());
            if (srcClip->hasCustomColour()) nc->setColour(srcClip->getColour());
            nc->setFadeInCurve (srcClip->getFadeInCurve());
            nc->setFadeOutCurve(srcClip->getFadeOutCurve());
            nc->setFadeInSecs  (srcClip->getFadeInSecs());
            nc->setFadeOutSecs (srcClip->getFadeOutSecs());
            for (auto& gp : srcClip->getGainPoints())
                nc->getGainPointsRW().push_back(gp);
        }
    }

    // MIDI クリップ
    for (int mi = 0; mi < src->getMidiClipCount(); ++mi)
    {
        auto* srcMc = src->getMidiClip(mi);
        if (!srcMc) continue;
        auto* nm = dst->addMidiClip(srcMc->getStartPosition(), srcMc->getDuration());
        if (!nm) continue;
        nm->setName(srcMc->getName());
        nm->setColour(srcMc->getColour());
        nm->setChannel(srcMc->getChannel());
        nm->getSequence().addSequence(srcMc->getSequence(), 0.0);
        nm->getSequence().updateMatchedPairs();
    }

    auto* dstPtr = dst.get();
    tracks.insert(tracks.begin() + sourceIdx + 1, std::move(dst));
    if (onChanged) onChanged();
    return dstPtr;
}

int TrackManager::getTrackY(int index) const
{
    int y = 0;
    for (int i = 0; i < index && i < (int)tracks.size(); ++i)
        y += tracks[(size_t)i]->getTotalHeight();
    return y;
}

int TrackManager::getTotalHeight() const
{
    int h = 0;
    for (auto& t : tracks)
        h += t->getTotalHeight();
    return h;
}

int TrackManager::trackAtY(int y) const
{
    int cur = 0;
    for (int i = 0; i < (int)tracks.size(); ++i)
    {
        cur += tracks[(size_t)i]->getTotalHeight();
        if (y < cur) return i;
    }
    return -1;
}
