// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

#pragma once
#include <JuceHeader.h>
#include <memory>
#include <vector>

class AudioEngine;

// ── アプリ音声取り込み (指定アプリの音を OS からキャプチャして出力へ混ぜる・歌枠向け) ──
// ブラウザ (YouTube) やカラオケアプリの音を、仮想ケーブル無しで DAW の最終出力へ加算する。
// ASIO 出力 (ヘッドホン) に乗るので歌い手本人が聞け、配信ミラー (StreamMirror) の tap は
// コールバック末尾なので OBS 等にも自動で乗る。録音・書き出しには構造的に乗らない
// (AudioEngine::setAppCaptureRing 参照)。設計は Docs/internal/PLAN_アプリ音声取り込み.md。
//
// 実装は Windows のみ (WASAPI プロセスループバック =
// AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK・Windows 10 2004+・本アプリは Win11+)。
// macOS は CoreAudio がマルチクライアントでこの問題自体が起きにくいためスタブ
// (isSupported() = false・UI 非表示)。ファイル分割は GlobalKeyMonitor と同じ作法
// (AppAudioCapture_win.cpp / AppAudioCapture_stub.cpp)。
//
// スレッドモデル:
//  - 本クラスの所有/操作 (start/stop/listAudioApps) は message thread のみ
//  - キャプチャは専用スレッド (Impl 所有) が行い、StreamMirrorRing (SPSC) へ push する。
//    reader は AudioEngine の audio thread (setAppCaptureRing で登録)
//  - 対象アプリの解決・再接続はキャプチャスレッド自身が行う (2 秒間隔の再試行ループ):
//    対象未起動でも start は成功し「待機」する / アプリ再起動で自動復帰する /
//    エラーダイアログは出さない (ブラウザ未起動は普通の状態)
class AppAudioCapture
{
public:
    AppAudioCapture();
    // dtor はキャプチャスレッドの join のみ。エンジンからの解除は呼び出し側が破棄前に
    // stop(engine) で行う契約 (StreamMirrorOutput と同じ。AudioEngine* を保持しないので
    // メンバ宣言順への暗黙依存が無い。解除し忘れてもリングは shared_ptr 所有で UAF にならない)
    ~AppAudioCapture();

    struct AppInfo
    {
        juce::String executable;    // 実行ファイル名 (設定保存キー・例 "chrome.exe")
        juce::String displayName;   // UI 表示名 (例 "Google Chrome (chrome.exe)")
        juce::uint32 pid = 0;       // 列挙時点の PID (キャプチャ時に名前から再解決するので参考値)
    };

    // この環境で取り込みを提供できるか (Windows = true / それ以外 = false)。UI の表示ゲート。
    static bool isSupported();

    // 音声セッションを持つアプリの列挙 (環境設定のコンボ用・message thread)。
    // 全レンダーデバイス横断で集め、同一アプリ (同名 exe の最上位祖先) に dedupe する。
    // システム音・Utawave 自身は除外。
    static std::vector<AppInfo> listAudioApps();

    // 取り込み開始。executable は listAudioApps() の AppInfo::executable (空はエラー)。
    // engine.setAppCaptureRing() でリングを登録し、キャプチャスレッドを起動する。
    // 対象プロセスが見つからなくても成功し「待機」する (見つかり次第キャプチャ開始)。
    // 既に動いていれば止めてから開始。戻り値はエラーメッセージ (成功なら空文字)。
    juce::String start(const juce::String& executable, AudioEngine& engine);

    // 停止 + エンジンから登録解除。リングは shared_ptr 所有 + エンジン側退役リスト保持なので
    // ここで手放しても安全 (StreamMirrorOutput::stop と同じ作法)。未開始なら no-op。
    void stop(AudioEngine& engine);

    bool isRunning() const;                 // start 済みか (待機中も true)
    bool isReceiving() const;               // 直近 ~1 秒にキャプチャパケットを受信したか (状態表示用)
    juce::String getTargetExecutable() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AppAudioCapture)
};
