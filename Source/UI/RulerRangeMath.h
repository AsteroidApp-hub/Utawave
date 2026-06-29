// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

#pragma once
#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

// ルーラー範囲（ロケーター / ループ範囲を兼用）の純粋な幾何 / 選択ロジック。
// JUCE 非依存。描画 (TimelineRuler::paint) とヒット判定 (hitTestLoopHandle)、
// P キーの範囲設定 (TimelineView::getRangeForRulerFromSelection) が共有して drift を防ぐ。
namespace RulerRangeMath
{
// 時刻(秒) → ルーラー上の x 座標。ルーラーは固定 BPM 換算 (bpm/60 * pixelsPerBeat) で
// 時刻↔x を線形写像する。ロケーター帯の描画とヒット判定で同じ式を使う。
inline double timeToX(double secs, double bpm, double pixelsPerBeat, double scrollX)
{
    return secs * (bpm / 60.0) * pixelsPerBeat - scrollX;
}

// ルーラー範囲(ロケーター)の端ヒット判定。戻り: 1=左端 / 2=右端 / 0=外。
//  ・範囲が空 (lx2<=lx1) なら 0
//  ・小節バー行 [yBars, yBars+hBars) の外、または hBars<=0 なら 0 (着色域に一致)
//  ・どちらかの端から hitPx 以内なら近い方の端を返す (同距離は左端を優先)
//  ・中央(全体移動)は廃止したので 0 (範囲内の通常クリックは上下ズーム/シークに使う)
inline int loopHandleAt(double lx1, double lx2, int x, int y,
                        int yBars, int hBars, double hitPx)
{
    if (lx2 <= lx1) return 0;
    if (hBars <= 0 || y < yBars || y >= yBars + hBars) return 0;
    const double dL = std::abs((double) x - lx1);
    const double dR = std::abs((double) x - lx2);
    if (dL <= hitPx || dR <= hitPx) return (dL <= dR) ? 1 : 2;
    return 0;
}

// P キー: 範囲選択があればそれを、無ければクリップ span 群の和 [minStart,maxEnd] を返す。
// 戻り true で outStart/outEnd を埋める (end>start のときのみ)。どちらも無ければ false。
inline bool rangeForRuler(bool hasSelection, double selStart, double selEnd,
                          const std::vector<std::pair<double, double>>& clipSpans,
                          double& outStart, double& outEnd)
{
    if (hasSelection)
    {
        outStart = selStart;
        outEnd   = selEnd;
        return selEnd > selStart;
    }
    if (clipSpans.empty()) return false;
    double s = clipSpans.front().first;
    double e = clipSpans.front().second;
    for (const auto& sp : clipSpans)
    {
        s = std::min(s, sp.first);
        e = std::max(e, sp.second);
    }
    if (e <= s) return false;
    outStart = s;
    outEnd   = e;
    return true;
}
} // namespace RulerRangeMath
