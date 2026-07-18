// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

#pragma once
#include <JuceHeader.h>
#include <memory>
#include <vector>

class StreamMirrorRing;

// ── アプリ音声取り込み (指定アプリの音を OS からキャプチャして出力へ混ぜる・歌枠向け) ──
// ブラウザ (YouTube) やカラオケアプリの音を、仮想ケーブル無しで DAW の最終出力へ加算する。
// ASIO 出力 (ヘッドホン) に乗るので歌い手本人が聞け、配信ミラー (StreamMirror) の tap は
// コールバック末尾なので OBS 等にも自動で乗る。録音・書き出しには構造的に乗らない
// (AudioEngine::setAppCaptureVoices 参照)。設計は Docs/internal/PLAN_アプリ音声取り込み.md。
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
//    reader 側 (AudioEngine の AppCaptureVoice) への配線は呼び出し側 (MainComponent の
//    アプリケーショントラック reconcile) が getRing() 経由で行う。エンジンへの依存は無い
//  - 対象アプリの解決・再接続はキャプチャスレッド自身が行う (2 秒間隔の再試行ループ):
//    対象未起動でも start は成功し「待機」する / アプリ再起動で自動復帰する /
//    エラーダイアログは出さない (ブラウザ未起動は普通の状態)
class AppAudioCapture
{
public:
    AppAudioCapture();
    ~AppAudioCapture();   // キャプチャスレッドの join のみ (リングは shared_ptr 所有で安全)

    struct AppInfo
    {
        juce::String executable;    // 実行ファイル名 (設定保存キー・例 "chrome.exe")
        juce::String displayName;   // UI 表示名 (例 "Google Chrome (chrome.exe)")
        juce::uint32 pid = 0;       // 列挙時点の PID (キャプチャ時に名前から再解決するので参考値)
    };

    // この環境で取り込みを提供できるか (Windows = true / それ以外 = false)。UI の表示ゲート。
    static bool isSupported();

    // 音声セッションを持つアプリの列挙 (トラックヘッダのアプリ選択メニュー用・message thread)。
    // 全レンダーデバイス横断で集め、同一アプリ (同名 exe の最上位祖先) に dedupe する。
    // システム音・Utawave 自身は除外。
    static std::vector<AppInfo> listAudioApps();

    // 取り込み開始。executable は listAudioApps() の AppInfo::executable (空はエラー)。
    // preferredSampleRate = 要求キャプチャ SR (通常はエンジン SR。0 以下は 48k)。
    // 内部でリングを生成しキャプチャスレッドを起動する (getRing() で取り出して配線する)。
    // 対象プロセスが見つからなくても成功し「待機」する (見つかり次第キャプチャ開始)。
    // 既に動いていれば止めてから開始。戻り値はエラーメッセージ (成功なら空文字)。
    juce::String start(const juce::String& executable, double preferredSampleRate);

    // 停止 (キャプチャスレッド join)。リングは shared_ptr 共有なので、reader 側が
    // まだ持っていても安全 (writer 不在の枯渇無音になるだけ)。未開始なら no-op。
    void stop();

    // 現在のリング (start 中のみ非 null)。ソース SR はセッション確立時にキャプチャスレッドが
    // ring->reset(実フォーマット SR) で設定する (それまで srcRate = 0 で reader は無音)。
    std::shared_ptr<StreamMirrorRing> getRing() const;

    bool isRunning() const;                 // start 済みか (待機中も true)
    bool isReceiving() const;               // 直近 ~1 秒にキャプチャパケットを受信したか (状態表示用)
    juce::String getTargetExecutable() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AppAudioCapture)
};
