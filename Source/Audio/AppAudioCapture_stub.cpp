// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

// アプリ音声取り込みの非 Windows スタブ (GlobalKeyMonitor_stub と同じ作法)。
// macOS は CoreAudio がマルチクライアントで「ASIO 占有でブラウザ音が聞けない」問題自体が
// 起きにくいため未実装 (isSupported() = false で UI 非表示)。将来 macOS 対応するなら
// CoreAudio process tap (14.4+) 等を別ファイルで実装する (PLAN_アプリ音声取り込み.md 参照)。

#include "AppAudioCapture.h"
#include "../Localisation.h"

struct AppAudioCapture::Impl {};

AppAudioCapture::AppAudioCapture() = default;
AppAudioCapture::~AppAudioCapture() = default;

bool AppAudioCapture::isSupported() { return false; }

std::vector<AppAudioCapture::AppInfo> AppAudioCapture::listAudioApps() { return {}; }

juce::String AppAudioCapture::start(const juce::String&, AudioEngine&)
{
    return tr(u8"この環境ではアプリ音声の取り込みに対応していません");
}

void AppAudioCapture::stop(AudioEngine&) {}

bool AppAudioCapture::isRunning() const   { return false; }
bool AppAudioCapture::isReceiving() const { return false; }
juce::String AppAudioCapture::getTargetExecutable() const { return {}; }
