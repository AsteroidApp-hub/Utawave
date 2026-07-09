// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

// Utawave — ClipInsertGeometry のユニットテスト
//
// クリップ「差し込み」(ドラッグ別トラック / ペースト) で重なる既存クリップをどう切り、
// 接合部に何 ms のクロスフェードを置くかの純粋な幾何計算。以前は各経路にコピーされ、
// #M1 (左右対称フェード) の境界クランプを 1 箇所取りこぼして「接合部レベルディップ」
// バグが出た。ここで一本化したロジックを純関数として網羅検証する:
//   重なり判定 (None/Covered) / 4 ケース (LeftCut/RightCut/Split) / #M1 対称性
//   (フェード長 <= 残り尺/2) / fileOffset が負にならない / エンベロープ右シフト /
//   範囲削除経路 (deleteSelectionRange が使う kXf=0: 切り口が範囲境界に一致・フェード 0)。
// ExportEngineTests.cpp が main() を持つので静的インスタンスを置くだけ。expect は ASCII。

#include <JuceHeader.h>
#include "../Source/Edit/ClipInsertGeometry.h"

namespace
{
using namespace ClipInsertGeometry;

static bool approxEq(double a, double b, double eps = 1e-9) { return std::abs(a - b) < eps; }

// エンベロープテスト用の GainPoint 互換構造体 (production は AudioClip::GainPoint)
struct TestPt { double time; float dB; };

class ClipInsertGeometryTests : public juce::UnitTest
{
public:
    ClipInsertGeometryTests() : juce::UnitTest("ClipInsertGeometry") {}

    void runTest() override
    {
        testNoOverlap();
        testCovered();
        testLeftCut();
        testRightCut();
        testSplit();
        testSymmetryInvariant();
        testThinClipNoAsymmetry();
        testFileOffsetNeverNegative();
        testEnvelopeShift();
        testRangeDeleteZeroXf();
    }

    // 各プランの「設定するフェードは新しい尺の半分以下」(=setFade*Secs の dur*0.5 クランプを
    // 受けない) を確認するヘルパ。これが #M1 対称性の機械的保証。
    void expectSymmetric(const Plan& p, const char* what)
    {
        if (p.kind == OverlapKind::LeftCut || p.kind == OverlapKind::Split)
        {
            expect(p.leftFadeOut <= p.leftDuration * 0.5 + 1e-9, what);
            expect(approxEq(p.leftFadeOut, p.insFadeIn), what);   // 既存とペースト側が同値 = 対称
        }
        if (p.kind == OverlapKind::RightCut || p.kind == OverlapKind::Split)
        {
            expect(p.rightFadeIn <= p.rightDuration * 0.5 + 1e-9, what);
            expect(approxEq(p.rightFadeIn, p.insFadeOut), what);
        }
    }

    void testNoOverlap()
    {
        beginTest("No overlap / touch only");
        // 既存 [0,2], 差し込み [3,5] → 重なりなし
        expect(planInsertOverlap(0.0, 2.0, 3.0, 5.0, 2.0, 0.030, 0.001).kind == OverlapKind::None);
        // 接触のみ (既存終端 == 差し込み開始)
        expect(planInsertOverlap(0.0, 3.0, 3.0, 5.0, 2.0, 0.030, 0.001).kind == OverlapKind::None);
        expect(planInsertOverlap(5.0, 7.0, 3.0, 5.0, 2.0, 0.030, 0.001).kind == OverlapKind::None);
    }

    void testCovered()
    {
        beginTest("Covered -> delete");
        // 既存 [3.0,4.0] が 差し込み [2.0,6.0] に完全に覆われる
        expect(planInsertOverlap(3.0, 4.0, 2.0, 6.0, 4.0, 0.030, 0.001).kind == OverlapKind::Covered);
        // 端が一致 (eps 内) でも Covered
        expect(planInsertOverlap(2.0, 6.0, 2.0, 6.0, 4.0, 0.030, 0.001).kind == OverlapKind::Covered);
    }

    void testLeftCut()
    {
        beginTest("LeftCut (existing tail overlaps insert head)");
        // 既存 [4,6], 差し込み [5,8], insDur=3
        auto p = planInsertOverlap(4.0, 6.0, 5.0, 8.0, 3.0, 0.030, 0.001);
        expect(p.kind == OverlapKind::LeftCut);
        // xfL = min(0.03, insS-cS=1, cE-insS=1, half=1.5) = 0.03
        expect(approxEq(p.leftDuration, (5.0 + 0.030) - 4.0));   // 1.03
        expect(approxEq(p.leftFadeOut, 0.030));
        expect(approxEq(p.insFadeIn, 0.030));
        expect(approxEq(p.insFadeOut, 0.0));                     // 右端は重ならない
        expectSymmetric(p, "LeftCut symmetric");
    }

    void testRightCut()
    {
        beginTest("RightCut (existing head overlaps insert tail)");
        // 既存 [6,10], 差し込み [5,7], insDur=2
        auto p = planInsertOverlap(6.0, 10.0, 5.0, 7.0, 2.0, 0.030, 0.001);
        expect(p.kind == OverlapKind::RightCut);
        // xfR = min(0.03, cE-insE=3, insE-cS=1, half=1) = 0.03
        expect(approxEq(p.rightStart, 7.0 - 0.030));            // 6.97
        expect(approxEq(p.rightFileOffsetDelta, 6.97 - 6.0));   // 0.97 (>0)
        expect(approxEq(p.rightDuration, 10.0 - 6.97));         // 3.03
        expect(approxEq(p.rightFadeIn, 0.030));
        expect(approxEq(p.insFadeOut, 0.030));
        expect(approxEq(p.insFadeIn, 0.0));
        expectSymmetric(p, "RightCut symmetric");
    }

    void testSplit()
    {
        beginTest("Split (existing contains insert)");
        // 既存 [0,10], 差し込み [3,5], insDur=2
        auto p = planInsertOverlap(0.0, 10.0, 3.0, 5.0, 2.0, 0.030, 0.001);
        expect(p.kind == OverlapKind::Split);
        // 左: xfL = min(0.03, 3, 7, 1) = 0.03
        expect(approxEq(p.leftDuration, (3.0 + 0.030) - 0.0));  // 3.03
        expect(approxEq(p.leftFadeOut, 0.030));
        expect(approxEq(p.insFadeIn, 0.030));
        // 右: xfR = min(0.03, cE-insE=5, half=1) = 0.03 → rStart = 5-0.03 = 4.97
        expect(approxEq(p.rightStart, 4.970));
        expect(approxEq(p.rightFileOffsetDelta, 4.970 - 0.0));  // 4.97 (連続継続)
        expect(approxEq(p.rightDuration, 10.0 - 4.970));        // 5.03
        expect(approxEq(p.rightFadeIn, 0.030));
        expect(approxEq(p.insFadeOut, 0.030));
        expectSymmetric(p, "Split symmetric");
        // 右部分が元クリップ末尾 cE にぴったり終わる (末尾を捨てない)
        expect(approxEq(p.rightStart + p.rightDuration, 10.0));
    }

    void testSymmetryInvariant()
    {
        beginTest("Symmetry invariant across many random-ish geometries");
        // 様々な配置で「設定フェード <= 残り尺/2」かつ「既存側 == ペースト側」を確認
        const double kXf = 0.030, eps = 0.001;
        const double starts[] = { 0.0, 1.0, 3.2, 9.31 };
        const double lens[]   = { 0.01, 0.04, 0.5, 2.0, 8.0 };
        for (double cS : starts)
            for (double cl : lens)
                for (double insS : starts)
                    for (double il : lens)
                    {
                        const double cE = cS + cl, insE = insS + il;
                        auto p = planInsertOverlap(cS, cE, insS, insE, il, kXf, eps);
                        expectSymmetric(p, "random geometry symmetric");
                    }
    }

    void testThinClipNoAsymmetry()
    {
        beginTest("Thin clip at paste boundary stays symmetric (#M1 regression)");
        // 回帰: 薄い既存 [10.00,10.05] に 差し込み [10.01,12.0] が重なる (LeftCut)。
        // 旧バグ: xfL を pStart-cS で抑えず 0.030 にすると leftDuration=0.04 で fadeOut が
        // 0.02 にクランプされ insFadeIn=0.030 と非対称になった。今は xfL=0.01 で対称。
        auto p = planInsertOverlap(10.00, 10.05, 10.01, 12.0, 1.99, 0.030, 0.001);
        expect(p.kind == OverlapKind::LeftCut);
        expect(approxEq(p.leftFadeOut, 0.010));                 // = pStart-cS で抑えられる
        expect(approxEq(p.insFadeIn, 0.010));
        expect(p.leftFadeOut <= p.leftDuration * 0.5 + 1e-9, "no asymmetric clamp");

        // 右側ミラー: 既存 [11.0,12.02] が差し込み [10,12] の尾に重なり、はみ出す尾 (cE-insE=0.02)
        // が薄い RightCut。xfR を cE-insE で抑えないと残り尺 0.05 に対し fadeIn=0.03 が 0.025 へ
        // クランプされ insFadeOut=0.03 と非対称になる。今は xfR=0.02 で対称。
        auto q = planInsertOverlap(11.0, 12.02, 10.0, 12.0, 2.0, 0.030, 0.001);
        expect(q.kind == OverlapKind::RightCut);
        expect(approxEq(q.rightFadeIn, 0.020));
        expect(q.rightFadeIn <= q.rightDuration * 0.5 + 1e-9, "right no asymmetric clamp");
        expect(approxEq(q.rightFadeIn, q.insFadeOut));
    }

    void testFileOffsetNeverNegative()
    {
        beginTest("rightFileOffsetDelta never negative");
        const double kXf = 0.030, eps = 0.001;
        const double starts[] = { 0.0, 0.005, 1.0, 5.5 };
        const double lens[]   = { 0.01, 0.05, 1.0, 4.0 };
        for (double cS : starts)
            for (double cl : lens)
                for (double insS : starts)
                    for (double il : lens)
                    {
                        const double cE = cS + cl, insE = insS + il;
                        auto p = planInsertOverlap(cS, cE, insS, insE, il, kXf, eps);
                        if (p.kind == OverlapKind::RightCut || p.kind == OverlapKind::Split)
                            expect(p.rightFileOffsetDelta >= -1e-9, "fileOffset delta >= 0");
                    }
    }

    void testRangeDeleteZeroXf()
    {
        beginTest("Range delete path (kXf=0): cuts land exactly on range bounds, no fades");
        // TimelineView::deleteSelectionRange は kXf=0 で呼ぶ (削除は接合相手がいないので
        // クロスフェードを作らない)。切り口が範囲境界に正確に一致し、プランのフェードが
        // すべて 0 になることを固定する。
        const double eps = 1e-4;

        // LeftCut: 既存 [4,6], 範囲 [5,8] → 左部分は範囲開始ちょうどで終わる
        auto p = planInsertOverlap(4.0, 6.0, 5.0, 8.0, 3.0, 0.0, eps);
        expect(p.kind == OverlapKind::LeftCut);
        expect(approxEq(p.leftDuration, 1.0), "left part ends exactly at range start");
        expect(approxEq(p.leftFadeOut, 0.0) && approxEq(p.insFadeIn, 0.0), "no crossfade");

        // RightCut: 既存 [6,10], 範囲 [5,7] → 右部分は範囲終端ちょうどから始まり、
        // fileOffset 前進量 = タイムライン前進量 (内容が動かない)
        auto q = planInsertOverlap(6.0, 10.0, 5.0, 7.0, 2.0, 0.0, eps);
        expect(q.kind == OverlapKind::RightCut);
        expect(approxEq(q.rightStart, 7.0), "right part starts exactly at range end");
        expect(approxEq(q.rightFileOffsetDelta, 1.0), "fileOffset delta == timeline shift");
        expect(approxEq(q.rightDuration, 3.0), "right part keeps tail length");
        expect(approxEq(q.rightFadeIn, 0.0) && approxEq(q.insFadeOut, 0.0), "no crossfade");

        // Split: 既存 [0,10], 範囲 [3,5] → 隙間 [3,5] がぴったり空く
        auto s = planInsertOverlap(0.0, 10.0, 3.0, 5.0, 2.0, 0.0, eps);
        expect(s.kind == OverlapKind::Split);
        expect(approxEq(s.leftDuration, 3.0), "left ends at range start");
        expect(approxEq(s.rightStart, 5.0), "tail starts at range end");
        expect(approxEq(s.rightFileOffsetDelta, 5.0), "tail fileOffset delta == rightStart - clipStart");
        expect(approxEq(s.rightStart + s.rightDuration, 10.0), "tail ends at original clip end");
        expect(approxEq(s.leftFadeOut, 0.0) && approxEq(s.rightFadeIn, 0.0)
               && approxEq(s.insFadeIn, 0.0) && approxEq(s.insFadeOut, 0.0), "no crossfade");

        // 最小尺クランプ: 端ぎりぎりの範囲でも left/right は 0.01 未満にならない
        auto m = planInsertOverlap(0.0, 10.0, 0.005, 9.995, 9.99, 0.0, eps);
        expect(m.kind == OverlapKind::Split);
        expect(m.leftDuration  >= 0.01 - 1e-9, "left duration clamped to minimum");
        expect(m.rightDuration >= 0.01 - 1e-9, "right duration clamped to minimum");

        // 接触のみ (範囲端 == クリップ端) は触らない
        expect(planInsertOverlap(0.0, 2.0, 2.0, 5.0, 3.0, 0.0, eps).kind == OverlapKind::None);
        expect(planInsertOverlap(5.0, 7.0, 2.0, 5.0, 3.0, 0.0, eps).kind == OverlapKind::None);
    }

    void testEnvelopeShift()
    {
        beginTest("shiftRightEnvelope rebases points");
        // 空はそのまま空
        expect(shiftRightEnvelope<TestPt>({}, 1.0, 0.0f).empty());

        std::vector<TestPt> src = { {0.0, -12.0f}, {1.0, -6.0f}, {2.0, 0.0f}, {3.0, -3.0f} };
        // splitLocal=1.5, 境界 dB=-9 → 期待: {0,-9}, {2-1.5=0.5,0}, {3-1.5=1.5,-3}
        auto out = shiftRightEnvelope<TestPt>(src, 1.5, -9.0f);
        expect(out.size() == 3);
        expect(approxEq(out[0].time, 0.0) && approxEq(out[0].dB, -9.0));
        expect(approxEq(out[1].time, 0.5) && approxEq(out[1].dB, 0.0));
        expect(approxEq(out[2].time, 1.5) && approxEq(out[2].dB, -3.0));
        // splitLocal がちょうどある点 (time==splitLocal) は > 判定で除外 (境界点に集約)
        auto out2 = shiftRightEnvelope<TestPt>(src, 2.0, -1.0f);
        expect(out2.size() == 2);                                // {0,-1}, {3-2=1,-3}
        expect(approxEq(out2[1].time, 1.0) && approxEq(out2[1].dB, -3.0));
    }
};

static ClipInsertGeometryTests clipInsertGeometryTests;
}
