// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

// Utawave — ファジング対象 (外部から入ってくるデータを読む処理) の定義
//
// 各ターゲットは「任意のバイト列を 1 つ受け取って処理する関数」+「正しい入力の種 (シード)」+
// 「変異に使う辞書」を持つ。ドライバ (FuzzMain.cpp) はシードを壊した入力を大量に流し込み、
// ASan/UBSan のクラッシュ・タイムアウト・ターゲット自身が検出した異常 (fuzzCheck) を報告する。

#pragma once
#include <JuceHeader.h>
#include <cstdint>
#include <string>
#include <vector>

namespace utafuzz
{
using Bytes = std::vector<uint8_t>;

struct Target
{
    const char* name;
    const char* description;
    void (*run)(const uint8_t* data, size_t size);
    std::vector<Bytes> (*seeds)();
    std::vector<std::string> dictionary;   // 挿入/置換に使う「効きそうな」トークン
    bool   textual;                         // true: "..." で囲まれた値の差し替え変異も行う (XML/JSON)
    size_t maxLen;                          // 入力サイズの上限
};

const std::vector<Target>& allTargets();

// ターゲット内で「クラッシュではないがバグ」(出力が NaN 等) を見つけた時に呼ぶ。
// ドライバが現在の入力を保存して異常終了する。
[[noreturn]] void fuzzFail(const char* what);

// ターゲットが使う作業フォルダ (ドライバが起動時に用意する)
juce::File workDir();
} // namespace utafuzz
