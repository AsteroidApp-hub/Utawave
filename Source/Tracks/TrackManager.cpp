// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

#include "TrackManager.h"
#include "../Localisation.h"
#include <algorithm>

TrackManager::TrackManager(juce::AudioFormatManager& fmt)
    : formatManager(fmt)
{}

Track* TrackManager::addTrack(const juce::String& name, bool stereo, int insertAfter, bool midi)
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
    Track* ptr = track.get();
    // ヘッダビューは onChanged() で isMidiTrack() を読んで作られるので、その前に確定させる。
    if (midi) ptr->setMidiTrack(true);
    if (insertAfter >= 0 && insertAfter < (int) tracks.size())
        tracks.insert(tracks.begin() + insertAfter + 1, std::move(track));
    else
        tracks.push_back(std::move(track));
    if (onChanged) onChanged();
    return ptr;
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

Track* TrackManager::addFolderTrack(int insertAfter)
{
    // "Folder N" 採番 (addTrack の "Track N" と同じ方式)
    int maxN = 0;
    for (auto& t : tracks)
    {
        const auto& tn = t->getName();
        if (tn.startsWith("Folder "))
        {
            const int v = tn.substring(7).getIntValue();
            if (v > maxN) maxN = v;
        }
    }
    auto track = std::make_unique<Track>("Folder " + juce::String(maxN + 1),
                                         formatManager, thumbnailCache,
                                         Track::paletteColour(nextColourIndex++));
    track->setStereo(true);
    bool insVisible = false;
    for (auto& t : tracks)
        if (t->isInsertSlotsVisible()) { insVisible = true; break; }
    track->setInsertSlotsVisible(insVisible);
    Track* ptr = track.get();
    // ヘッダビューは onChanged() で isFolderTrack() を読んで作られるので、その前に確定させる
    ptr->setFolderTrack(true);
    if (insertAfter >= 0 && insertAfter < (int) tracks.size())
        tracks.insert(tracks.begin() + insertAfter + 1, std::move(track));
    else
        tracks.push_back(std::move(track));
    if (onChanged) onChanged();
    return ptr;
}

bool TrackManager::hasFolderTrack() const
{
    for (auto& t : tracks) if (t->isFolderTrack()) return true;
    return false;
}

Track* TrackManager::addAppCaptureTrack(int insertAfter)
{
    // "App N" 採番 (addFolderTrack の "Folder N" と同じ方式)
    int maxN = 0;
    for (auto& t : tracks)
    {
        const auto& tn = t->getName();
        if (tn.startsWith("App "))
        {
            const int v = tn.substring(4).getIntValue();
            if (v > maxN) maxN = v;
        }
    }
    auto track = std::make_unique<Track>("App " + juce::String(maxN + 1),
                                         formatManager, thumbnailCache,
                                         Track::paletteColour(nextColourIndex++));
    track->setStereo(true);
    Track* ptr = track.get();
    // INS はアプリ音声に効かないため常に非表示 (toggleAllInsertSlots 側も対象外にする)。
    // ヘッダビューは onChanged() で isAppCaptureTrack() を読んで作られるので先に確定させる
    ptr->setInsertSlotsVisible(false);
    ptr->setAppCaptureTrack(true);
    if (insertAfter >= 0 && insertAfter < (int) tracks.size())
        tracks.insert(tracks.begin() + insertAfter + 1, std::move(track));
    else
        tracks.push_back(std::move(track));
    if (onChanged) onChanged();
    return ptr;
}

int TrackManager::folderRunEnd(int folderIdx) const
{
    if (folderIdx < 0 || folderIdx >= (int) tracks.size()) return folderIdx;
    auto* f = tracks[(size_t) folderIdx].get();
    if (!f || !f->isFolderTrack()) return folderIdx + 1;
    int i = folderIdx + 1;
    while (i < (int) tracks.size() && tracks[(size_t) i]->getFolderParent() == f)
        ++i;
    return i;
}

std::vector<Track*> TrackManager::getFolderChildren(const Track* folder) const
{
    std::vector<Track*> out;
    if (folder == nullptr) return out;
    for (auto& t : tracks)
        if (t && t->getFolderParent() == folder)
            out.push_back(t.get());
    return out;
}

bool TrackManager::normalizeFolderContiguity()
{
    // (1) 親参照の整合: 消えた親 / フォルダ自身・Click が親を持つ (非対応) を解消
    bool changedParents = false;
    for (auto& t : tracks)
    {
        if (!t) continue;
        auto* p = t->getFolderParent();
        if (p == nullptr) continue;
        const bool parentAlive = indexOf(p) >= 0 && p->isFolderTrack();
        if (!parentAlive || t->isFolderTrack() || t->isClickTrack() || t->isAppCaptureTrack())
        {
            t->setFolderParent(nullptr);
            changedParents = true;
        }
    }

    // (2) 並び: 子は親フォルダの直後に相対順のまま連続させる
    std::vector<Track*> desired;
    desired.reserve(tracks.size());
    for (auto& t : tracks)
    {
        if (!t) continue;
        if (t->getFolderParent() != nullptr) continue;   // 子は親の直後で emit する
        desired.push_back(t.get());
        if (t->isFolderTrack())
            for (auto& c : tracks)
                if (c && c->getFolderParent() == t.get())
                    desired.push_back(c.get());
    }

    bool same = desired.size() == tracks.size();
    if (same)
        for (size_t i = 0; i < tracks.size(); ++i)
            if (tracks[i].get() != desired[i]) { same = false; break; }
    if (same)
        return changedParents;

    reorderTo(desired);   // onChanged() は reorderTo が発火する
    return true;
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

Track* TrackManager::duplicateTrack(int sourceIdx, bool includeTakeLanes)
{
    if (sourceIdx < 0 || sourceIdx >= (int) tracks.size()) return nullptr;
    auto* src = tracks[(size_t) sourceIdx].get();
    if (!src || src->isClickTrack()) return nullptr;   // Click は複製不可
    if (src->isFolderTrack())        return nullptr;   // フォルダは複製不可 (子は複製対象外のため)
    if (src->isAppCaptureTrack())    return nullptr;   // アプリトラックは複製不可 (同一アプリの二重取り込みになる)

    // 名前: 末尾に連番 "(1)" "(2)" … を付与 (空きの最小番号)。
    // 既に "名前 (N)" 形式なら末尾の番号を剥がして基底名にし、番号だけ繰り上げる。
    juce::String baseName = src->getName();
    {
        auto trimmed = baseName.trimEnd();
        if (trimmed.endsWithChar (')'))
        {
            auto open = trimmed.lastIndexOfChar ('(');
            if (open > 0 && trimmed[open - 1] == ' ')
            {
                auto inner = trimmed.substring (open + 1, trimmed.length() - 1);
                if (inner.isNotEmpty() && inner.containsOnly ("0123456789"))
                    baseName = trimmed.substring (0, open).trimEnd();
            }
        }
    }
    auto nameUnique = [this, &baseName]() -> juce::String
    {
        for (int n = 1; ; ++n)
        {
            juce::String candidate = baseName + " (" + juce::String(n) + ")";
            bool dup = false;
            for (auto& t : tracks)
                if (t->getName() == candidate) { dup = true; break; }
            if (!dup) return candidate;
        }
    };
    auto dst = std::make_unique<Track>(nameUnique(), formatManager, thumbnailCache);

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
    // フォルダ所属は引き継ぐ (子の複製は同じフォルダ内に入る。挿入位置 sourceIdx+1 は
    // 元トラックの直後 = フォルダのラン内なので連続性も保たれる)
    dst->setFolderParent    (src->getFolderParent());

    // オーディオクリップ: 全レーンの全クリップをコピー。
    // includeTakeLanes=false なら Lane 0 (= メインレーン) のみで、テイクレーンは複製しない。
    const int laneLimit = includeTakeLanes ? src->getLaneCount() : 1;
    for (int li = 0; li < laneLimit; ++li)
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
