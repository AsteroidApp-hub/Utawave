// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

#pragma once

namespace builtin
{
    /** ソフトニーのゲインリダクション量 (dB, >=0) を返す。
        overDb = 検出レベル − スレッショルド、slope = 1 − 1/ratio、kneeDb = ニー幅。
        ニー幅内は 2 次補間で滑らかに立ち上げ、超えたら直線。コンプ/ディエッサーで共用
        (式を 1 箇所に集約し、ニー曲線の調整がエフェクト間でドリフトしないようにする)。 */
    inline float softKneeReductionDb(float overDb, float slope, float kneeDb) noexcept
    {
        const float halfKnee = kneeDb * 0.5f;
        if (overDb <= -halfKnee) return 0.0f;
        if (overDb >=  halfKnee) return slope * overDb;
        const float x = overDb + halfKnee;   // 0..kneeDb
        return slope * (x * x) / (2.0f * kneeDb);
    }
}
