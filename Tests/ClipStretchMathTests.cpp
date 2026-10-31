// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

// Utawave — クリップのタイムストレッチ位置計算 (ClipStretchMath) のユニットテスト
//
// 再ストレッチ / 100% 戻しが常に元ファイル基準で計算され、何度伸ばしても位置が
// ずれないこと (= 元ファイル上の区間が保存されること) を固定する。expect は ASCII。

#include <JuceHeader.h>
#include <cmath>
#include "../Source/Edit/ClipStretchMath.h"

namespace
{

class ClipStretchMathTests : public juce::UnitTest
{
public:
    ClipStretchMathTests() : juce::UnitTest("ClipStretchMath (time-stretch geometry)") {}

    static bool near(double a, double b) { return std::abs(a - b) < 1e-9; }

    void runTest() override
    {
        using namespace ClipStretchMath;

        beginTest("spanOf: unstretched clip is its own file region");
        {
            auto s = spanOf(3.0, 2.0, false, 1.0, 0.0);
            expect(near(s.start, 3.0) && near(s.duration, 2.0), "span = fileOffset/duration");
        }

        beginTest("ratio clamp and ratioForDuration");
        {
            const SourceSpan s { 3.0, 2.0 };
            expect(near(ratioForDuration(s, 2.5), 1.25), "2.5 / 2.0 = 1.25");
            expect(near(ratioForDuration(s, 0.1), kMinRatio), "clamped to min 0.5");
            expect(near(ratioForDuration(s, 10.0), kMaxRatio), "clamped to max 2.0");
            expect(near(ratioForDuration({ 0.0, 0.0 }, 1.0), 1.0), "zero span -> 1.0");
        }

        beginTest("renderRegion: margins, clamped to file bounds");
        {
            auto r = renderRegion({ 3.0, 2.0 }, 60.0, 2.0);
            expect(near(r.start, 1.0) && near(r.duration, 6.0), "[1, 7]");
            auto head = renderRegion({ 0.5, 1.0 }, 60.0, 2.0);
            expect(near(head.start, 0.0) && near(head.duration, 3.5), "start clamped at 0");
            auto tail = renderRegion({ 58.0, 1.5 }, 60.0, 2.0);
            expect(near(tail.start, 56.0) && near(tail.duration, 4.0), "end clamped at file length");
        }

        beginTest("place: right-corner drag keeps start, left-corner drag keeps end");
        {
            const SourceSpan s { 3.0, 2.0 };
            const auto r = renderRegion(s, 60.0, 2.0);   // start 1.0
            auto p = place(s, 1.5, r, 10.0, 12.0, false);
            expect(near(p.startPos, 10.0), "start fixed");
            expect(near(p.duration, 3.0), "duration x1.5");
            expect(near(p.fileOffset, (3.0 - 1.0) * 1.5), "offset in rendered timeline");

            auto q = place(s, 0.5, r, 10.0, 12.0, true);
            expect(near(q.duration, 1.0), "duration x0.5");
            expect(near(q.startPos, 11.0), "end fixed at 12");
            auto clampLeft = place(s, 2.0, r, 1.0, 2.0, true);
            expect(clampLeft.startPos >= 0.0, "start never negative");
        }

        beginTest("re-stretch round trip keeps the source span (no drift)");
        {
            // 元: file 上 [3, 5] を 1.25 倍にレンダー → そのクリップから span を戻すと [3, 5]
            const SourceSpan s { 3.0, 2.0 };
            const auto r1 = renderRegion(s, 60.0);
            const auto p1 = place(s, 1.25, r1, 10.0, 12.0, false);
            const auto back1 = spanOf(p1.fileOffset, p1.duration, true, 1.25, r1.start);
            expect(near(back1.start, 3.0) && near(back1.duration, 2.0), "span recovered after x1.25");

            // さらに 0.8 倍へ (元基準) → 100% 戻し
            const auto r2 = renderRegion(back1, 60.0);
            const auto p2 = place(back1, 0.8, r2, p1.startPos, p1.startPos + p1.duration, false);
            const auto back2 = spanOf(p2.fileOffset, p2.duration, true, 0.8, r2.start);
            expect(near(back2.start, 3.0) && near(back2.duration, 2.0), "span recovered after x0.8");

            const auto u = placeUnstretched(back2, 10.0, 10.0 + p2.duration, false);
            expect(near(u.fileOffset, 3.0) && near(u.duration, 2.0) && near(u.startPos, 10.0),
                   "100% restores the original file offset and length");
        }

        beginTest("split piece of a stretched clip maps back to the right source position");
        {
            // 伸縮済みクリップを分割した右片 (fileOffset が進んだだけ) も同じ式で戻せる
            const SourceSpan s { 3.0, 2.0 };
            const auto r = renderRegion(s, 60.0);
            const auto p = place(s, 1.5, r, 10.0, 12.0, false);
            const double splitLocal = 1.2;   // タイムライン上でクリップ先頭から 1.2s
            const auto right = spanOf(p.fileOffset + splitLocal, p.duration - splitLocal, true, 1.5, r.start);
            expect(near(right.start, 3.0 + splitLocal / 1.5), "right piece source start");
            expect(near(right.start + right.duration, 5.0), "right piece ends at source end");
        }

        beginTest("isUnity");
        {
            expect(isUnity(1.0) && isUnity(1.00001) && ! isUnity(1.01), "unity tolerance 1e-4");
        }
    }
};

static ClipStretchMathTests clipStretchMathTests;
}
