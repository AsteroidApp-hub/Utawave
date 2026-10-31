// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave
//
// クリップのタイムストレッチの位置計算 (純関数・JUCE 非依存)。
// 「元ファイル (source) 上の区間」を基準に、倍率から新しいクリップ配置とレンダー区間を求める。
// 再ストレッチ / 100% 戻しは常に source 基準で計算するので、何度伸ばしても位置がずれない。

#pragma once
#include <algorithm>
#include <cmath>

namespace ClipStretchMath
{
    constexpr double kMinRatio = 0.5;
    constexpr double kMaxRatio = 2.0;
    // レンダー区間の前後余白 (source 秒)。伸ばした後の軽いトリム戻しに使える
    constexpr double kRenderMarginSecs = 2.0;

    // source 上の区間 (クリップが鳴らしている元音声の範囲)
    struct SourceSpan { double start { 0.0 }; double duration { 0.0 }; };

    // 現在のクリップ → source 上の区間。stretched=false なら現ファイル = source
    inline SourceSpan spanOf(double fileOffset, double duration,
                             bool stretched, double ratio, double stretchSrcStart)
    {
        if (! stretched || ratio <= 0.0) return { fileOffset, duration };
        return { stretchSrcStart + fileOffset / ratio, duration / ratio };
    }

    inline double clampRatio(double r) { return std::clamp(r, kMinRatio, kMaxRatio); }

    // ドラッグで決めた新しい尺 → 倍率 (source 基準・範囲クランプ)
    inline double ratioForDuration(const SourceSpan& s, double newDuration)
    {
        if (s.duration <= 0.0) return 1.0;
        return clampRatio(newDuration / s.duration);
    }

    // レンダーする source 区間 (前後に余白・ファイル長でクランプ)
    struct Region { double start { 0.0 }; double duration { 0.0 }; };
    inline Region renderRegion(const SourceSpan& s, double sourceLength,
                               double margin = kRenderMarginSecs)
    {
        const double a = std::max(0.0, s.start - margin);
        const double b = std::min(sourceLength > 0.0 ? sourceLength : s.start + s.duration + margin,
                                  s.start + s.duration + margin);
        return { a, std::max(0.0, b - a) };
    }

    // 新しいクリップ配置。anchorEnd=true なら右端 (oldEnd) を固定 (左下角のドラッグ)、
    // false なら左端 (oldStart) を固定 (右下角のドラッグ)。
    // fileOffset はレンダーファイル (region.start が t=0・ratio 倍の時間軸) 上の位置
    struct Placement { double startPos { 0.0 }; double duration { 0.0 }; double fileOffset { 0.0 }; };
    inline Placement place(const SourceSpan& s, double ratio, const Region& region,
                           double oldStart, double oldEnd, bool anchorEnd)
    {
        Placement p;
        p.duration   = s.duration * ratio;
        p.fileOffset = std::max(0.0, (s.start - region.start) * ratio);
        p.startPos   = anchorEnd ? std::max(0.0, oldEnd - p.duration) : oldStart;
        return p;
    }

    // 倍率 1 (伸縮なし) に戻す時の配置: source をそのまま参照する
    inline Placement placeUnstretched(const SourceSpan& s, double oldStart, double oldEnd, bool anchorEnd)
    {
        Placement p;
        p.duration   = s.duration;
        p.fileOffset = s.start;
        p.startPos   = anchorEnd ? std::max(0.0, oldEnd - p.duration) : oldStart;
        return p;
    }

    inline bool isUnity(double ratio) { return std::abs(ratio - 1.0) < 1.0e-4; }
}
