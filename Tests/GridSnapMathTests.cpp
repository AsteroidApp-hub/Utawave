// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

// Utawave — 変拍子/途中テンポ変更対応グリッドスナップ (GridSnapMath) のユニットテスト
//
// TimelineView::snapTime の実体 (Source/UI/GridSnapMath.h・JUCE 非依存の純関数) を検証する。
// 旧実装 (round(secs/unit)*unit の固定間隔・Bar=4 拍固定) は拍子変更後の小節頭に
// 吸着できなかった (2026-07 修正の回帰テスト)。描画グリッド (drawTrackRows) と同じ
// 歩進なので、ここで固定する時刻 = 画面に見えるグリッド線の位置。expect は ASCII。

#include <JuceHeader.h>
#include <cmath>
#include "../Source/UI/GridSnapMath.h"

namespace
{

class GridSnapMathTests : public juce::UnitTest
{
public:
    GridSnapMathTests() : juce::UnitTest("GridSnapMath (meter/tempo-aware snap)") {}

    static bool near(double a, double b) { return std::abs(a - b) < 1e-6; }

    void runTest() override
    {
        using namespace GridSnapMath;
        const std::vector<BpmChange>   noBpm;
        const std::vector<MeterChange> noMeter;

        beginTest("plain 4/4: parity with fixed-unit snapping");
        {
            // 120bpm / 4/4 のみ: 従来挙動と同じ結果になる (回帰なし)
            expect(near(snapToGrid(0.55, SnapMode::Quarter, 120.0, noBpm, 4, noMeter), 0.5),
                   "quarter snaps to nearest beat");
            expect(near(snapToGrid(1.9,  SnapMode::Bar,     120.0, noBpm, 4, noMeter), 2.0),
                   "bar snaps to bar head");
            expect(near(snapToGrid(0.35, SnapMode::QuarterT,120.0, noBpm, 4, noMeter), 1.0 / 3.0),
                   "quarter triplet grid");
            expect(near(snapToGrid(1.23, SnapMode::Off,     120.0, noBpm, 4, noMeter), 1.23),
                   "Off is identity");
            expect(near(snapToGrid(-0.5, SnapMode::Quarter, 120.0, noBpm, 4, noMeter), 0.0),
                   "negative clamps to 0");
        }

        beginTest("meter change: bar snap lands on real bar heads (5/4)");
        {
            // 120bpm (拍 0.5s)。bar1,2 = 4/4 (各 2s)、bar3 から 5/4 (2.5s)。
            // 小節頭: 0 / 2 / 4 / 6.5 / 9 ...
            const std::vector<MeterChange> meters { { 2, 5, 4 } };   // barIndex 2 = bar 3
            expect(near(snapToGrid(6.4, SnapMode::Bar, 120.0, noBpm, 4, meters), 6.5),
                   "snaps to 5/4 bar head (old fixed-unit gave 6.0)");
            expect(near(snapToGrid(4.4, SnapMode::Bar, 120.0, noBpm, 4, meters), 4.0),
                   "snaps back to bar 3 head");
            expect(near(snapToGrid(6.5, SnapMode::Bar, 120.0, noBpm, 4, meters), 6.5),
                   "exact bar head is stable");
            expect(near(snapToGrid(8.0, SnapMode::Bar, 120.0, noBpm, 4, meters), 9.0),
                   "bar 4 (5/4) head at 9.0");
        }

        beginTest("meter change: sub-bar grid restarts at each bar head");
        {
            const std::vector<MeterChange> meters { { 2, 5, 4 } };
            // bar3 = [4.0, 6.5) の 1/2 グリッド: 4.0 / 5.0 / 6.0、次の小節頭 6.5
            expect(near(snapToGrid(6.1, SnapMode::Half, 120.0, noBpm, 4, meters), 6.0),
                   "half grid inside 5/4 bar");
            expect(near(snapToGrid(6.3, SnapMode::Half, 120.0, noBpm, 4, meters), 6.5),
                   "odd remainder: next bar head wins over last half line");
            // 1/4 は拍そのもの: 6.3 は 6.0 / 6.5 (次の頭) の間 → 6.5
            expect(near(snapToGrid(6.3, SnapMode::Quarter, 120.0, noBpm, 4, meters), 6.5),
                   "quarter grid continuous across meter change");
        }

        beginTest("tempo change: grid follows the tempo map");
        {
            // 120bpm、t=2.0 (bar2 頭) から 60bpm。bar1 = [0,2)、bar2 = [2,6)、bar3 頭 = 6.0
            const std::vector<BpmChange> bpms { { 2.0, 60.0 } };
            expect(near(snapToGrid(4.5, SnapMode::Bar, 120.0, bpms, 4, noMeter), 6.0),
                   "bar head after slowdown (old fixed-unit gave 4.0)");
            expect(near(snapToGrid(4.4, SnapMode::Quarter, 120.0, bpms, 4, noMeter), 4.0),
                   "quarter = 1s beats after slowdown");
            expect(near(snapToGrid(2.45, SnapMode::Eighth, 120.0, bpms, 4, noMeter), 2.5),
                   "eighth interpolates inside the slow beat");
        }
    }
};

static GridSnapMathTests gridSnapMathTests;

} // namespace
