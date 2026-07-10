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
// 録音は常に Lane 0。停止時は新録音を Lane 0 に置き (既存クリップはパンチイントリム)、
// 新録音自体だけを Take レーンへバックアップする。

void Track::startLiveRecording(double startPosSecs, double bufferLeadSecs)
{
    recordingStartPos  = startPosSecs;
    liveBufferLeadSecs = bufferLeadSecs;   // 負も可 (負のレイテンシ補正 = 内容を遅く描く)
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

    // Lane 0 の重なる既存クリップはテイクレーンへ退避しない (上塗り・要望 2026-07)。
    // 各録音は録音時に自分自身を backupToTakeLane (下) で 1 回バックアップしており
    // テイク履歴はそれで揃う。ここで重なりクリップを再退避すると、パンチインで
    // トリム済みの断片が (file, fileOffset, duration) 違いの部分クリップとして
    // テイクリストに混入していた (dedup 3 つ組をすり抜けるため)。

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

int Track::findFreeTakeLaneIndex(double start, double end, int minLaneIdx)
{
    if (lanes.empty())
        lanes.push_back(std::make_unique<Lane>());   // Lane 0 はテイクレーンにしない
    const int first = juce::jmax(1, minLaneIdx);
    for (int li = first; li < (int)lanes.size(); ++li)
        if (!lanes[(size_t)li]->overlaps(start, end)) return li;
    lanes.push_back(std::make_unique<Lane>());
    return (int)lanes.size() - 1;
}

AudioClip* Track::backupToTakeLane(const juce::File& file, double startPos, double dur,
                                    double fileOffset)
{
    const int dest = findFreeTakeLaneIndex(startPos, startPos + dur);
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
    // 閉じたフォルダ配下のトラックは行ごと非表示 (高さ 0)。TrackManager の
    // getTrackY / trackAtY / getTotalHeight とヘッダ/タイムラインのレイアウトが
    // すべてこの値を積算するため、ここで 0 を返すだけで全域が追従する。
    if (isHiddenByFolder())
        return 0;
    // メイン部 + レーン部（独立にリサイズ可能）
    int mainH = getMainHeight();
    if (lanesCollapsed || (int)lanes.size() <= 1)
        return mainH;
    int laneH = getLaneHeight();
    return mainH + ((int)lanes.size() - 1) * laneH;
}
