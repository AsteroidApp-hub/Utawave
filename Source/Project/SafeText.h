// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

#pragma once
#include <JuceHeader.h>

// 外部 (ネットワーク応答 / キャッシュファイル) から来たバイト列を文字列にする。
// 不正な UTF-8 を juce::String::fromUTF8 にそのまま渡すと、後の trim() 等が範囲外を読む
// (ファジングで検出)。サーバー障害・プロキシ・公衆 Wi-Fi のログイン画面などで壊れた応答は
// 普通に起きるため、不正なら空文字 (= データ無し扱い) にする。
namespace SafeText
{
inline juce::String fromUtf8OrEmpty(const void* data, size_t size)
{
    if (data == nullptr || size == 0 || size > (size_t) std::numeric_limits<int>::max())
        return {};
    auto* p = static_cast<const char*>(data);
    if (! juce::CharPointer_UTF8::isValidString(p, (int) size))
        return {};
    return juce::String::fromUTF8(p, (int) size);
}

inline juce::String fromUtf8OrEmpty(const juce::MemoryBlock& mb)
{
    return fromUtf8OrEmpty(mb.getData(), mb.getSize());
}

inline juce::String loadFileOrEmpty(const juce::File& f)
{
    juce::MemoryBlock mb;
    if (! f.loadFileAsData(mb)) return {};
    return fromUtf8OrEmpty(mb);
}
} // namespace SafeText
