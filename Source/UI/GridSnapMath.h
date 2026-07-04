// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

#pragma once
#include <cmath>
#include <vector>
#include <algorithm>
#include "../AppSettings.h"

// 変拍子 / 途中テンポ変更対応のグリッドスナップ (JUCE 非依存・ヘッダオンリーの純関数)。
// TimelineView::drawTrackRows の背景グリッド線生成と**同じ歩進・同じ補間式**で
// 「小節頭 + 小節内サブグリッド」の時刻を求め、最も近い線へスナップする。
// 旧実装 (round(secs/unit)*unit の固定間隔・Bar=4 拍固定) は拍子変更があると
// 小節頭に吸着できなかった (5/4 や 6/4 の小節頭が 4 拍の倍数に乗らないため)。
// 条件を変える時は drawTrackRows のグリッド線生成と必ず両方直すこと (ずれると
// 「見えている線に吸着しない」バグに戻る)。RulerRangeMath と同じ作法で
// GridSnapMathTests が検証する。
namespace GridSnapMath
{

// TimelineRuler::bpmAt と同じ: 時刻 t で有効な BPM (changes は時刻昇順)
inline double bpmAtTime(double t, double baseBpm, const std::vector<BpmChange>& changes)
{
    double cur = baseBpm;
    for (auto& bc : changes)
    {
        if (bc.timeSec <= t) cur = bc.bpm;
        else break;
    }
    return cur;
}

// TimelineRuler::getMeterAtBar1 と同じ: 1-based 小節番号で有効な拍子の分子 (changes は barIndex 昇順)
inline int meterNumAtBar1(int bar1, int baseNum, const std::vector<MeterChange>& changes)
{
    int cur = baseNum;
    const int barIdx = bar1 - 1;
    for (auto& mc : changes)
    {
        if (mc.barIndex <= barIdx) cur = mc.numerator;
        else break;
    }
    return cur;
}

// secs を最寄りのグリッド線へスナップする。
//  - グリッドは各小節頭から unitBeats (音価の拍数) 刻みで張り、小節境界で必ず張り直す
//    (端数拍の小節でも次の小節頭が常に候補になる)
//  - SnapMode::Bar は小節頭のみ (拍子に依らず正しい小節頭に吸着する)
//  - SnapMode::Off は恒等
inline double snapToGrid(double secs, SnapMode mode, double baseBpm,
                         const std::vector<BpmChange>& bpmChanges,
                         int baseMeterNum, const std::vector<MeterChange>& meterChanges)
{
    if (mode == SnapMode::Off) return secs;
    if (secs <= 0.0) return 0.0;

    // グリッド単位を「拍数」で表す (音価なので BPM に依らず一定。drawTrackRows と同式)
    const double spbRef     = 60.0 / (baseBpm > 1.0 ? baseBpm : 1.0);
    const double unitBeats  = snapModeUnitSecs(mode, baseBpm) / (spbRef > 1e-9 ? spbRef : 1e-9);
    const bool   barOnly    = (mode == SnapMode::Bar);   // Bar の 4 拍固定値は使わない

    int bar = 1;
    double t = 0.0;
    std::vector<double> beatBounds;   // 小節内の各拍境界の時刻 (size n+1)
    while (bar <= 100000)
    {
        const int n = std::max(1, meterNumAtBar1(bar, baseMeterNum, meterChanges));
        const double barStart = t;
        beatBounds.clear();
        beatBounds.push_back(t);
        for (int beat = 0; beat < n; ++beat)
        {
            const double bp = bpmAtTime(t, baseBpm, bpmChanges);
            t += 60.0 / (bp > 1.0 ? bp : 1.0);
            beatBounds.push_back(t);
        }

        if (secs >= t && bar < 100000) { ++bar; continue; }

        // この小節内: 候補 = 小節頭 / 小節内サブグリッド / 次の小節頭
        double best = barStart;
        auto consider = [&](double cand)
        {
            if (std::abs(cand - secs) < std::abs(best - secs)) best = cand;
        };
        if (!barOnly && unitBeats > 1e-6)
        {
            for (double p = unitBeats; p < (double)n - 1e-6; p += unitBeats)
            {
                const int j = (int)p;                    // 拍インデックス
                if (j >= (int)beatBounds.size() - 1) break;
                const double frac = p - (double)j;       // 拍内の位置 (拍内は等速補間)
                consider(beatBounds[(size_t)j]
                         + frac * (beatBounds[(size_t)j + 1] - beatBounds[(size_t)j]));
            }
        }
        consider(t);   // 次の小節頭
        return best;
    }
    return secs;
}

} // namespace GridSnapMath
