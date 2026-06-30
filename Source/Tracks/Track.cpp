// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

#include "Track.h"
#include "../VST/PluginChain.h"

const juce::Colour Track::trackColours[8] = {
    juce::Colour(0xff3a6ea5),
    juce::Colour(0xff5aa55a),
    juce::Colour(0xffa55a5a),
    juce::Colour(0xffa5925a),
    juce::Colour(0xff7a5aa5),
    juce::Colour(0xff5a9ea5),
    juce::Colour(0xffa55a92),
    juce::Colour(0xff5a7aa5)
};

juce::Colour Track::paletteColour(int idx)
{
    return trackColours[((idx % 8) + 8) % 8];
}

bool Lane::overlaps(double start, double end) const
{
    for (auto& clip : clips)
        if (clip->getStartPosition() < end && clip->getEndPosition() > start)
            return true;
    return false;
}

AudioClip* Lane::addClip(const juce::File& file, double startPos, double dur,
                          juce::AudioFormatManager& fmt, juce::AudioThumbnailCache& cache)
{
    clips.push_back(std::make_unique<AudioClip>(file, startPos, dur, fmt, cache));
    return clips.back().get();
}

Track::Track(const juce::String& trackName, juce::AudioFormatManager& fmt,
             juce::AudioThumbnailCache& cache)
    : Track(trackName, fmt, cache, trackColours[0])  // 後方互換: TrackManager 経由でない場合は青固定
{
}

Track::Track(const juce::String& trackName, juce::AudioFormatManager& fmt,
             juce::AudioThumbnailCache& cache, juce::Colour initialColour)
    : name(trackName),
      colour(initialColour),
      formatManager(fmt),
      thumbnailCache(cache),
      pluginChain(std::make_unique<PluginChain>())
{
    lanes.push_back(std::make_unique<Lane>());
}

Track::~Track() = default;

// ── ライブ録音レーン管理 ──────────────────────────────────────────
// 録音は常に Lane 0。録音停止時、Lane 0 内で重なるクリップを sub-lane へ退避。

void Track::startLiveRecording(double startPosSecs, double bufferLeadSecs)
{
    recordingStartPos  = startPosSecs;
    liveBufferLeadSecs = juce::jmax(0.0, bufferLeadSecs);
    liveBuffer.reset();

    if (lanes.empty())
    {
        lanes.push_back(std::make_unique<Lane>());
        liveRecordingLaneIdx = 0;
        return;
    }

    // Lane 0 はそのまま（移動・削除しない）
    // パンチイン形式: 既存クリップの上に新規録音を重ねる
    liveRecordingLaneIdx = 0;
}

AudioClip* Track::finishLiveRecording(const juce::File& file, double startPos, double dur,
                                      double fileOffset)
{
    if (liveRecordingLaneIdx < 0 || lanes.empty())
    {
        liveRecordingLaneIdx = -1;
        return nullptr;
    }

    double endPos = startPos + dur;

    // Lane 0 の重なるクリップを Take レーンにコピー（バックアップ）
    // ※ Lane 0 からは移動しない → パンチイン連続性を維持
    // ※ (ファイル, fileOffset, 尺) が完全一致する Take レーンクリップがあればスキップ。
    //    (ファイル名だけで判定すると、同一録音を分割して fileOffset 違いで複数配置している
    //     ケースで片方しかバックアップされないため、3 タプルで比較する)
    {
        constexpr double kEps = 1e-4;
        auto alreadyBackedUp = [&](const AudioClip& src) -> bool
        {
            for (int li = 1; li < (int)lanes.size(); ++li)
                for (auto& c : lanes[(size_t)li]->clips)
                    if (c->getFile() == src.getFile()
                        && std::abs(c->getFileOffset() - src.getFileOffset()) < kEps
                        && std::abs(c->getDuration()   - src.getDuration())   < kEps)
                        return true;
            return false;
        };

        std::vector<AudioClip*> toBackup;
        for (auto& c : lanes[0]->clips)
            if (c->getStartPosition() < endPos && c->getEndPosition() > startPos
                && !alreadyBackedUp(*c))   // バックアップ済みはスキップ
                toBackup.push_back(c.get());

        for (auto* orig : toBackup)
        {
            double cs = orig->getStartPosition(), ce = orig->getEndPosition();
            int dest = -1;
            for (int li = 1; li < (int)lanes.size(); ++li)
                if (!lanes[(size_t)li]->overlaps(cs, ce)) { dest = li; break; }
            if (dest < 0) { lanes.push_back(std::make_unique<Lane>()); dest = (int)lanes.size() - 1; }
            auto* bk = lanes[(size_t)dest]->addClip(orig->getFile(), cs, ce - cs,
                                                    formatManager, thumbnailCache);
            // 退避クリップは元クリップの忠実なコピーにする。特に fileOffset を
            // 引き継がないと、分割やパンチイン由来で fileOffset>0 の元クリップが
            // ファイル先頭 (offset 0) から描画/再生されて波形が元と食い違う
            // (テイクに入った波形だけ違って見える原因)。dedup も (file, fileOffset,
            // duration) で行うため、正しい fileOffset を入れないと重複退避も起きる。
            if (bk)
            {
                bk->setFileOffset  (orig->getFileOffset());
                bk->setGain        (orig->getGain());
                if (orig->getName().isNotEmpty()) bk->setName(orig->getName());
                if (orig->hasCustomColour()) bk->setColour(orig->getColour());
                bk->setFadeInCurve (orig->getFadeInCurve());
                bk->setFadeOutCurve(orig->getFadeOutCurve());
                bk->setFadeInSecs  (orig->getFadeInSecs());
                bk->setFadeOutSecs (orig->getFadeOutSecs());
                for (auto& gp : orig->getGainPoints())
                    bk->getGainPointsRW().push_back(gp);
            }
        }
    }

    // Lane 0 に新クリップを追加（既存クリップと重なってOK = パンチイン）
    auto* clip = lanes[0]->addClip(file, startPos, dur, formatManager, thumbnailCache);
    // 録音直後のファイルはサムネイルキャッシュが古い/未完なので必ず再読込
    if (clip)
    {
        if (fileOffset > 0.0) clip->setFileOffset(fileOffset);
        clip->refreshThumbnail();
    }

    // 既存クリップをトリムして境界に最小クロスフェードを作成
    trimAndCrossfadeOnLane0(clip, startPos, dur);

    // 新しいクリップ自体もTakeレーンにバックアップ
    // （テイク選びで最新録音も参照できるように）
    if (auto* bk = backupToTakeLane(file, startPos, dur, fileOffset)) bk->refreshThumbnail();

    liveRecordingLaneIdx = -1;
    return clip;
}

void Track::cancelLiveRecording()
{
    // キャンセル時は何もしない（Lane 0 の既存クリップはそのまま）
    liveRecordingLaneIdx = -1;
}

void Track::trimAndCrossfadeOnLane0(AudioClip* newClip, double startPos, double dur)
{
    if (lanes.empty() || newClip == nullptr) return;
    const double endPos = startPos + dur;
    constexpr double kPunchXfade = 0.030;
    const double newHalf = dur * 0.5;
    // 各境界で左右対称なクロスフェードにする (#M1)。隣接ピースと newClip の両方に同値を入れ、
    // setFadeXxxSecs の独立クランプ (duration*0.5) で左右非対称にならないようにする。
    // (短いクリップ/ピースだと片側だけ縮められて描画の対称な X と実音がずれる)
    double leftFade  = juce::jmin(kPunchXfade, newHalf);   // newClip 左端 (= 左隣との境界)
    double rightFade = juce::jmin(kPunchXfade, newHalf);   // newClip 右端 (= 右隣との境界)

    auto& lane0 = lanes[0]->clips;
    std::vector<std::unique_ptr<AudioClip>> splitPieces;

    for (auto it = lane0.begin(); it != lane0.end(); )
    {
        auto* c = it->get();
        if (c == newClip) { ++it; continue; }

        double cs = c->getStartPosition();
        double ce = c->getEndPosition();

        if (cs >= endPos || ce <= startPos) { ++it; continue; }

        if (cs >= startPos && ce <= endPos)
        {
            it = lane0.erase(it);
        }
        else if (cs < startPos && ce > endPos)
        {
            const double rightTrimStart = juce::jmax(startPos, endPos - kPunchXfade);
            double rightFO = c->getFileOffset() + (rightTrimStart - cs);
            auto right = std::make_unique<AudioClip>(
                             c->getFile(), rightTrimStart, ce - rightTrimStart,
                             formatManager, thumbnailCache);
            right->setFileOffset(rightFO);
            right->setName(c->getName());
            // 元クリップがカスタム色を持つ場合のみ引き継ぐ。
            // setColour は無条件で customColour=true を立てるので、
            // 元がトラック色追従 (customColour=false) のときに呼ぶと
            // デフォルト色 (青) が固定化されてしまうため。
            if (c->hasCustomColour()) right->setColour(c->getColour());
            const double rf = juce::jmin(kPunchXfade, right->getDuration() * 0.5, newHalf);
            right->setFadeInSecs(rf);
            rightFade = juce::jmin(rightFade, rf);   // newClip 右端と同値に
            splitPieces.push_back(std::move(right));

            const double leftEnd = juce::jmin(startPos + kPunchXfade, ce);
            c->setDuration(juce::jmax(0.01, leftEnd - cs));
            const double lf = juce::jmin(kPunchXfade, c->getDuration() * 0.5, newHalf);
            c->setFadeOutSecs(lf);
            leftFade = juce::jmin(leftFade, lf);     // newClip 左端と同値に
            if (c->getDuration() <= 0.01) it = lane0.erase(it);
            else                          ++it;
        }
        else if (cs < startPos)
        {
            const double newCEnd = juce::jmin(ce, startPos + kPunchXfade);
            c->setDuration(juce::jmax(0.01, newCEnd - cs));
            const double lf = juce::jmin(kPunchXfade, c->getDuration() * 0.5, newHalf);
            c->setFadeOutSecs(lf);
            leftFade = juce::jmin(leftFade, lf);
            if (c->getDuration() <= 0.01) it = lane0.erase(it);
            else                          ++it;
        }
        else
        {
            const double newCStart = juce::jmax(cs, endPos - kPunchXfade);
            double newFO = c->getFileOffset() + (newCStart - cs);
            c->setFileOffset(newFO);
            c->setStartPosition(newCStart);
            c->setDuration(juce::jmax(0.01, ce - newCStart));
            const double rf = juce::jmin(kPunchXfade, c->getDuration() * 0.5, newHalf);
            c->setFadeInSecs(rf);
            rightFade = juce::jmin(rightFade, rf);
            if (c->getDuration() <= 0.01) it = lane0.erase(it);
            else                          ++it;
        }
    }

    for (auto& sp : splitPieces)
        lane0.push_back(std::move(sp));

    // 新クリップの両端フェードは各境界の対称値 (隣が無ければ既定 jmin(kPunchXfade, dur/2))
    newClip->setFadeInSecs(leftFade);
    newClip->setFadeOutSecs(rightFade);
}

AudioClip* Track::backupToTakeLane(const juce::File& file, double startPos, double dur,
                                    double fileOffset)
{
    int dest = -1;
    for (int li = 1; li < (int)lanes.size(); ++li)
        if (!lanes[(size_t)li]->overlaps(startPos, startPos + dur)) { dest = li; break; }
    if (dest < 0)
    {
        lanes.push_back(std::make_unique<Lane>());
        dest = (int)lanes.size() - 1;
    }
    auto* clip = lanes[(size_t)dest]->addClip(file, startPos, dur, formatManager, thumbnailCache);
    if (clip && fileOffset > 0.0) clip->setFileOffset(fileOffset);
    return clip;
}

AudioClip* Track::addClip(const juce::File& file, double startPos, double dur)
{
    for (auto& lane : lanes)
    {
        if (!lane->overlaps(startPos, startPos + dur))
            return lane->addClip(file, startPos, dur, formatManager, thumbnailCache);
    }
    lanes.push_back(std::make_unique<Lane>());
    return lanes.back()->addClip(file, startPos, dur, formatManager, thumbnailCache);
}

AudioClip* Track::addClipToLane0(const juce::File& file, double startPos, double dur)
{
    if (lanes.empty())
        lanes.push_back(std::make_unique<Lane>());
    return lanes[0]->addClip(file, startPos, dur, formatManager, thumbnailCache);
}

int Track::getTotalHeight() const
{
    // メイン部 + レーン部（独立にリサイズ可能）
    int mainH = getMainHeight();
    if (lanesCollapsed || (int)lanes.size() <= 1)
        return mainH;
    int laneH = getLaneHeight();
    return mainH + ((int)lanes.size() - 1) * laneH;
}
