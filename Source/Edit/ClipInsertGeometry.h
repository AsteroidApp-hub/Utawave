// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave
#pragma once

#include <vector>
#include <algorithm>

// ============================================================================
// ClipInsertGeometry — クリップ「差し込み」時の重なり処理の純粋な幾何計算
// ----------------------------------------------------------------------------
// クリップ (ドラッグ移動 / ペースト) をレーンへ差し込むと、重なる既存クリップを
// カット/分割し、接合部に最小クロスフェードを作る。この「どう切ってどこに何 ms の
// フェードを置くか」の計算は、ドラッグ別トラック経路 (TimelineView_Mouse.cpp) と
// ペースト経路 (TimelineView_Edit.cpp) で同一だが、以前は各所にコピーされていて
// #M1 (左右対称フェード) の境界クランプを 1 箇所だけ取りこぼすバグが起きた。
//
// ここに JUCE 非依存の純関数として一本化し、ユニットテスト可能にする
// (ClipInsertGeometryTests)。録音 (Undo 記録) の作法は各呼び出し側が持つ。
// ============================================================================
namespace ClipInsertGeometry
{
    enum class OverlapKind
    {
        None,      // 重なりなし → 何もしない
        Covered,   // 既存が差し込み範囲に完全に覆われる → 削除
        LeftCut,   // 既存の右側が差し込みの頭に重なる → 既存を「左部分」に詰める
        RightCut,  // 既存の左側が差し込みの尾に重なる → 既存を「右部分」に詰める
        Split      // 既存が差し込みを内包 → 「左部分 (既存)」+「右部分 (新クリップ)」へ分割
    };

    struct Plan
    {
        OverlapKind kind { OverlapKind::None };

        // 左部分 (既存クリップを縮めて残す。LeftCut / Split で有効)
        double leftDuration { 0.0 };          // 既存クリップの新しい尺 ([cS, cS+leftDuration])
        double leftFadeOut  { 0.0 };          // 接合部のクロスフェード長 (0 = 隣接 = 呼び出し側で既定フェード)

        // 右部分 (RightCut=既存クリップを動かす / Split=新クリップを作る)
        double rightStart            { 0.0 }; // タイムライン開始位置
        double rightFileOffsetDelta  { 0.0 }; // 元 fileOffset に加える量 (常に >= 0)
        double rightDuration         { 0.0 };
        double rightFadeIn           { 0.0 }; // 接合部のクロスフェード長 (0 = 隣接)

        // 差し込みクリップ自身が両端に持つべきクロスフェード長
        double insFadeIn  { 0.0 };            // 差し込みの左端 (LeftCut / Split)
        double insFadeOut { 0.0 };            // 差し込みの右端 (RightCut / Split)
    };

    // cS,cE   : 既存クリップの [開始, 終了] (タイムライン秒)
    // insS,insE: 差し込みクリップの [開始, 終了]
    // insDur  : 差し込みクリップの尺 (フェード長クランプ用。通常 insE-insS だが呼び出し側の値を尊重)
    // kXf     : 望む最小クロスフェード長 (例 0.030)
    // eps     : 重なり判定のイプシロン (例 0.001 / 1e-4)
    inline Plan planInsertOverlap(double cS, double cE,
                                  double insS, double insE, double insDur,
                                  double kXf, double eps)
    {
        Plan p;

        // 重なりなし / 接触のみ
        if (cE <= insS + eps || cS >= insE - eps) { p.kind = OverlapKind::None; return p; }
        // 完全に覆われる
        if (cS >= insS - eps && cE <= insE + eps) { p.kind = OverlapKind::Covered; return p; }

        const double half = insDur * 0.5;

        if (cS < insS && cE > insE)            // 内包 → 分割
        {
            p.kind = OverlapKind::Split;

            // 左クロスフェード: 差し込み外側 (insS-cS) と内側 (cE-insS)、両尺の半分で抑える。
            // insS-cS で抑えるのが #M1 の肝 (左部分の残り尺 >= 2*xfL を保証し、setFade*Secs の
            // dur*0.5 クランプで片側だけ短くなる非対称を防ぐ)。
            const double xfL = std::min(std::min(kXf, insS - cS), std::min(cE - insS, half));
            const double xfR = std::min(std::min(kXf, cE - insE), half);

            const double aL = (xfL > 0.001) ? xfL : 0.0;
            p.leftDuration = std::max(0.01, (insS + aL) - cS);
            p.leftFadeOut  = aL;
            p.insFadeIn    = aL;

            const double aR = (xfR > 0.001) ? xfR : 0.0;
            const double rStart = insE - aR;
            p.rightStart           = rStart;
            p.rightFileOffsetDelta = rStart - cS;       // = (insE-aR)-cS > 0 (cS<insS<insE)
            p.rightDuration        = std::max(0.01, cE - rStart);
            p.rightFadeIn          = aR;
            p.insFadeOut           = aR;
        }
        else if (cS < insS)                    // 左隣 (既存の尾が差し込みの頭に重なる)
        {
            p.kind = OverlapKind::LeftCut;
            const double xfL = std::min(std::min(kXf, insS - cS), std::min(cE - insS, half));
            const double aL  = (xfL > 0.001) ? xfL : 0.0;
            p.leftDuration = std::max(0.01, (insS + aL) - cS);
            p.leftFadeOut  = aL;
            p.insFadeIn    = aL;
        }
        else                                   // 右隣 (既存の頭が差し込みの尾に重なる)
        {
            p.kind = OverlapKind::RightCut;
            const double xfR    = std::min(std::min(kXf, cE - insE), std::min(insE - cS, half));
            const double aR     = (xfR > 0.001) ? xfR : 0.0;
            const double nStart = insE - aR;
            p.rightStart           = nStart;
            p.rightFileOffsetDelta = nStart - cS;        // = (insE-aR)-cS >= 0 (cS>=insS, aR<=insE-cS)
            p.rightDuration        = std::max(0.01, cE - nStart);
            p.rightFadeIn          = aR;
            p.insFadeOut           = aR;
        }

        return p;
    }

    // 分割の「右部分」が引き継ぐゲインエンベロープ。元クリップ (クリップ先頭=0 のローカル秒) の
    // ポイントのうち splitLocal より後ろを (-splitLocal) シフトしてコピーし、境界 (時刻0) に
    // 分割点の dB を 1 点足す。ClipSplitAction の右クリップと同じ手法。
    // Pt は { double time; float dB; } 互換 (production では AudioClip の GainPoint)。
    template <class Pt>
    inline std::vector<Pt> shiftRightEnvelope(const std::vector<Pt>& src,
                                              double splitLocal, float dbAtSplit)
    {
        std::vector<Pt> out;
        if (src.empty()) return out;
        out.push_back({ 0.0, dbAtSplit });
        for (const auto& gp : src)
            if (gp.time > splitLocal)
                out.push_back({ gp.time - splitLocal, gp.dB });
        return out;
    }
}
