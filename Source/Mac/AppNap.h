// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

#pragma once

/**
    macOS の App Nap を無効化する (起動時に一度だけ呼ぶ)。

    App Nap はアプリを「アイドル気味」と判断すると、タイマー / ディスプレイ更新 (VBlank) の
    配信を周期的にスロットルする。これにより、メッセージスレッドで駆動される再生バー
    (`MainComponent::onVBlank`) が数秒ごとに一瞬止まって見える (オーディオは実時間スレッドで
    App Nap 免除なので音は途切れない)。DAW は再生 / 録音中に UI を止めたくないため、起動時に
    `NSProcessInfo beginActivityWithOptions:` の activity token を取得してプロセス寿命の間
    App Nap を抑止する。

    非 macOS では何もしない (スタブ)。
*/
void disableAppNap();

// NSView のバッキングレイヤを同期描画 (drawsAsynchronously=NO) に強制する。
// JUCE は macOS でレイヤを drawsAsynchronously=YES にするため、AppKit が
// ディスプレイリンク同期で CA トランザクションをフラッシュする際に
// CA::Transaction::commit が非同期描画の完了を DispatchGroup の cond_wait で
// メッセージスレッド上で待ち、描画が重い/詰まると再生バー等の UI が数百 ms 固まる。
// これを NO にすると描画がインラインで走り「非同期描画の完了待ち」が消える。
// nsViewHandle は ComponentPeer::getNativeHandle() の戻り値 (NSView*)。
void forceSyncLayerDrawing(void* nsViewHandle);
