// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

#pragma once
#include <cmath>

/**
    RBJ Audio EQ Cookbook の双二次 (biquad) フィルタ。Transposed Direct Form II。
    内蔵 EQ 専用 (ゼロレイテンシー)。係数算出は係数変更時のみ、process は 1 サンプル。
    状態はチャンネル毎 (ステレオ 2ch) に独立。
*/
struct Biquad
{
    double b0 { 1.0 }, b1 { 0.0 }, b2 { 0.0 }, a1 { 0.0 }, a2 { 0.0 };
    float  z1[2] { 0.0f, 0.0f };
    float  z2[2] { 0.0f, 0.0f };

    void reset() noexcept { z1[0] = z1[1] = z2[0] = z2[1] = 0.0f; }

    inline float process(int ch, float x) noexcept
    {
        const float y = (float) (b0 * x) + z1[ch];
        z1[ch] = (float) (b1 * x) - (float) (a1 * y) + z2[ch];
        z2[ch] = (float) (b2 * x) - (float) (a2 * y);
        return y;
    }

    // a0 で正規化して係数を格納
    void normalise(double b0n, double b1n, double b2n, double a0, double a1n, double a2n) noexcept
    {
        const double inv = 1.0 / a0;
        b0 = b0n * inv; b1 = b1n * inv; b2 = b2n * inv;
        a1 = a1n * inv; a2 = a2n * inv;
    }

    void setHighpass(double sr, double freq, double Q) noexcept
    {
        const double w0 = 2.0 * 3.14159265358979323846 * clampFreq(freq, sr) / sr;
        const double cs = std::cos(w0), sn = std::sin(w0);
        const double alpha = sn / (2.0 * (Q > 0.01 ? Q : 0.01));
        const double b0n = (1.0 + cs) * 0.5;
        const double b1n = -(1.0 + cs);
        const double b2n = (1.0 + cs) * 0.5;
        const double a0  = 1.0 + alpha;
        const double a1n = -2.0 * cs;
        const double a2n = 1.0 - alpha;
        normalise(b0n, b1n, b2n, a0, a1n, a2n);
    }

    void setPeak(double sr, double freq, double gainDb, double Q) noexcept
    {
        const double A  = std::pow(10.0, gainDb / 40.0);
        const double w0 = 2.0 * 3.14159265358979323846 * clampFreq(freq, sr) / sr;
        const double cs = std::cos(w0), sn = std::sin(w0);
        const double alpha = sn / (2.0 * (Q > 0.01 ? Q : 0.01));
        const double b0n = 1.0 + alpha * A;
        const double b1n = -2.0 * cs;
        const double b2n = 1.0 - alpha * A;
        const double a0  = 1.0 + alpha / A;
        const double a1n = -2.0 * cs;
        const double a2n = 1.0 - alpha / A;
        normalise(b0n, b1n, b2n, a0, a1n, a2n);
    }

    void setLowShelf(double sr, double freq, double gainDb, double S) noexcept
    {
        const double A  = std::pow(10.0, gainDb / 40.0);
        const double w0 = 2.0 * 3.14159265358979323846 * clampFreq(freq, sr) / sr;
        const double cs = std::cos(w0), sn = std::sin(w0);
        const double beta = std::sqrt(A) / (S > 0.01 ? S : 0.01);
        const double am1 = A - 1.0, ap1 = A + 1.0;
        const double b0n = A * (ap1 - am1 * cs + beta * sn);
        const double b1n = 2.0 * A * (am1 - ap1 * cs);
        const double b2n = A * (ap1 - am1 * cs - beta * sn);
        const double a0  = ap1 + am1 * cs + beta * sn;
        const double a1n = -2.0 * (am1 + ap1 * cs);
        const double a2n = ap1 + am1 * cs - beta * sn;
        normalise(b0n, b1n, b2n, a0, a1n, a2n);
    }

    void setHighShelf(double sr, double freq, double gainDb, double S) noexcept
    {
        const double A  = std::pow(10.0, gainDb / 40.0);
        const double w0 = 2.0 * 3.14159265358979323846 * clampFreq(freq, sr) / sr;
        const double cs = std::cos(w0), sn = std::sin(w0);
        const double beta = std::sqrt(A) / (S > 0.01 ? S : 0.01);
        const double am1 = A - 1.0, ap1 = A + 1.0;
        const double b0n = A * (ap1 + am1 * cs + beta * sn);
        const double b1n = -2.0 * A * (am1 + ap1 * cs);
        const double b2n = A * (ap1 + am1 * cs - beta * sn);
        const double a0  = ap1 - am1 * cs + beta * sn;
        const double a1n = 2.0 * (am1 - ap1 * cs);
        const double a2n = ap1 - am1 * cs - beta * sn;
        normalise(b0n, b1n, b2n, a0, a1n, a2n);
    }

    // ナイキストの少し下までにクランプ (係数の発散防止)
    static double clampFreq(double f, double sr) noexcept
    {
        const double hi = sr * 0.49;
        if (f < 10.0) return 10.0;
        if (f > hi)   return hi;
        return f;
    }
};
