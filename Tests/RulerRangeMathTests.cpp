// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

// Utawave — RulerRangeMath のユニットテスト
//
// ルーラー範囲(ロケーター / ループ範囲を兼用)の純粋な幾何 / 選択ロジック:
//   timeToX     … 時刻↔x の固定 BPM 線形写像 (描画とヒット判定で共有 = drift 防止)
//   loopHandleAt … 端ヒット (1=左端 / 2=右端 / 0=外)・近い端優先・小節バー行限定・
//                  中央(全体移動)は廃止 = 0
//   rangeForRuler … P キー (範囲選択優先・無ければクリップ span 群の和)
// main() は ExportEngineTests.cpp が持つので静的インスタンスを置くだけ。expect は ASCII。

#include <JuceHeader.h>
#include "../Source/UI/RulerRangeMath.h"

namespace
{
using namespace RulerRangeMath;

class RulerRangeMathTests : public juce::UnitTest
{
public:
    RulerRangeMathTests() : juce::UnitTest("RulerRangeMath") {}

    void runTest() override
    {
        beginTest("timeToX: linear map (bpm/60 * ppb) with scroll");
        {
            // bpm=120 -> bps=2; ppb=80 -> 160 px/sec
            expectWithinAbsoluteError(timeToX(0.0, 120.0, 80.0, 0.0),   0.0, 1e-9);
            expectWithinAbsoluteError(timeToX(1.0, 120.0, 80.0, 0.0), 160.0, 1e-9);
            expectWithinAbsoluteError(timeToX(1.0, 120.0, 80.0, 60.0),100.0, 1e-9);  // scroll shifts left
            expectWithinAbsoluteError(timeToX(1.0,  60.0, 80.0, 0.0),  80.0, 1e-9);  // bpm scales
        }

        beginTest("loopHandleAt: empty / inverted range -> 0");
        {
            expectEquals(loopHandleAt(100.0, 100.0, 100, 50, 44, 20, 6.0), 0);
            expectEquals(loopHandleAt(200.0, 100.0, 150, 50, 44, 20, 6.0), 0);
        }

        beginTest("loopHandleAt: only inside the bars row [yBars, yBars+hBars)");
        {
            const double lx1 = 100.0, lx2 = 300.0;
            const int yBars = 44, hBars = 20;
            expectEquals(loopHandleAt(lx1, lx2, 100, 43, yBars, hBars, 6.0), 0);   // above bars row
            expectEquals(loopHandleAt(lx1, lx2, 100, 64, yBars, hBars, 6.0), 0);   // below (e.g. Time row)
            expectEquals(loopHandleAt(lx1, lx2, 100, 44, yBars, hBars, 6.0), 1);   // top edge of bars row
            expectEquals(loopHandleAt(lx1, lx2, 100, 63, yBars, hBars, 6.0), 1);   // bottom of bars row
            expectEquals(loopHandleAt(lx1, lx2, 100, 50, yBars, 0,    6.0), 0);    // bars row hidden
        }

        beginTest("loopHandleAt: edges within hit radius, nearest wins");
        {
            const double lx1 = 100.0, lx2 = 300.0;
            const int yBars = 44, hBars = 20, y = 50;
            expectEquals(loopHandleAt(lx1, lx2, 100, y, yBars, hBars, 6.0), 1);  // exactly on left
            expectEquals(loopHandleAt(lx1, lx2, 106, y, yBars, hBars, 6.0), 1);  // 6px from left (inclusive)
            expectEquals(loopHandleAt(lx1, lx2, 107, y, yBars, hBars, 6.0), 0);  // 7px -> miss -> 0 (no middle)
            expectEquals(loopHandleAt(lx1, lx2, 300, y, yBars, hBars, 6.0), 2);  // exactly on right
            expectEquals(loopHandleAt(lx1, lx2, 295, y, yBars, hBars, 6.0), 2);  // near right
        }

        beginTest("loopHandleAt: middle (whole-range move) is gone -> 0");
        {
            expectEquals(loopHandleAt(100.0, 300.0, 200, 50, 44, 20, 6.0), 0);
        }

        beginTest("loopHandleAt: tie distance breaks to the left edge");
        {
            // lx1=100, lx2=110, x=105 -> dL=5, dR=5 -> tie -> left(1)
            expectEquals(loopHandleAt(100.0, 110.0, 105, 50, 44, 20, 6.0), 1);
        }

        beginTest("rangeForRuler: selection takes priority over clips");
        {
            double s = 0, e = 0;
            std::vector<std::pair<double, double>> clips { { 1.0, 2.0 } };
            expect(rangeForRuler(true, 5.0, 9.0, clips, s, e));
            expectWithinAbsoluteError(s, 5.0, 1e-9);
            expectWithinAbsoluteError(e, 9.0, 1e-9);
        }

        beginTest("rangeForRuler: no selection -> union [minStart, maxEnd] of clip spans");
        {
            double s = 0, e = 0;
            std::vector<std::pair<double, double>> clips { { 3.0, 4.0 }, { 1.0, 2.5 }, { 3.5, 6.0 } };
            expect(rangeForRuler(false, 0.0, 0.0, clips, s, e));
            expectWithinAbsoluteError(s, 1.0, 1e-9);
            expectWithinAbsoluteError(e, 6.0, 1e-9);
        }

        beginTest("rangeForRuler: nothing available -> false");
        {
            double s = -1, e = -1;
            std::vector<std::pair<double, double>> none;
            expect(! rangeForRuler(false, 0.0, 0.0, none, s, e));
        }

        beginTest("rangeForRuler: degenerate selection (start==end) -> false");
        {
            double s = 0, e = 0;
            std::vector<std::pair<double, double>> none;
            expect(! rangeForRuler(true, 5.0, 5.0, none, s, e));
        }
    }
};

static RulerRangeMathTests rulerRangeMathTests;
} // namespace
