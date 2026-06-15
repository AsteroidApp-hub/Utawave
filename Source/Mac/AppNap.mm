// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

#include "AppNap.h"
#import <Foundation/Foundation.h>

// beginActivityWithOptions が返す activity token を static で保持し、プロセス寿命の間
// App Nap (とアイドルスリープ) を抑止する。endActivity は呼ばない (= 終了まで有効)。
// JUCE は非 ARC でビルドされるため、autorelease されないよう明示的に retain する。
static id<NSObject> sActivityToken = nil;

void disableAppNap()
{
    if (sActivityToken != nil) return;   // 二重呼び出しガード

    NSProcessInfo* pi = [NSProcessInfo processInfo];
    if (! [pi respondsToSelector:@selector(beginActivityWithOptions:reason:)])
        return;   // 非常に古い OS 等 (現行 macOS では常に存在)

    // NSActivityUserInitiated  : App Nap を防ぐ (ユーザー操作中とみなさせる)
    // NSActivityLatencyCritical: 低レイテンシ要求を明示 (オーディオ/再生 UI の周期スロットル回避)
    const NSActivityOptions opts = (NSActivityOptions)
        (NSActivityUserInitiated | NSActivityLatencyCritical);

    sActivityToken = [[pi beginActivityWithOptions:opts
                                            reason:@"Utawave playback/recording UI responsiveness"] retain];
}
