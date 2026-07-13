// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

#pragma once
#include <JuceHeader.h>
#include "StreamMirrorRing.h"

class AudioEngine;

// ── 配信ミラー出力 ──
// 最終ミックス (いま耳に聞こえている音 = 再生 + INS を通ったモニタ返し) を、メインとは別の
// 出力デバイスへ同時に流す。主目的は配信: メイン出力が ASIO 等 (OS のオーディオセッションを
// バイパスする) で配信ソフトから見えない環境でも、ミラー側が WASAPI 共有 (Mac は CoreAudio)
// のセッションを作るので、配信ソフトのアプリ音声キャプチャや仮想デバイス経由で拾える。
// ループバック機能の無いオーディオインターフェースのユーザー向け。
//  - デバイスは Windows = WASAPI 共有モードのみ / macOS = CoreAudio (ASIO は列挙しない。
//    シングルクライアント占有・低遅延の必要も無いため)
//  - SR はメインエンジンと同じものを優先して開く (リサンプル最小化)。違っても
//    StreamMirrorReader のドリフト補正リサンプラが吸収する
//  - 遅延はおおむね一定 (リングの目標水位 ≈ 50ms + デバイスバッファ)。配信側の
//    同期オフセット設定で吸収できる
// 所有/操作は message thread のみ。エンジンへの登録は AudioEngine::setMirrorRing (shared_ptr
// 公開 + 解除時 drain) 経由なので、audio thread とのライフタイム競合は無い。
class StreamMirrorOutput : private juce::AudioIODeviceCallback
{
public:
    StreamMirrorOutput() = default;
    // 注意: エンジンからの登録解除は stop(engine) が行う。dtor はデバイスを閉じるだけなので、
    // 呼び出し側 (MainComponent) は破棄前に stop() を呼ぶこと (エンジン側の shared_ptr が
    // リングを延命するため呼び忘れても UAF にはならないが、無駄な push が続く)。
    ~StreamMirrorOutput() override;

    // 選択可能な出力デバイス名 (環境設定のコンボ用)。message thread から呼ぶ。
    static juce::StringArray getOutputDeviceNames();

    // 開始 (既に動いていれば止めてから)。deviceName は必須 (空 / 不在はエラー)。
    // 「OS 既定へのフォールバック」はしない — 既定 = メイン出力と同じ I/O の環境が多く、
    // 黙って同じデバイスへミラーすると二重聞こえ事故になるため。
    // 戻り値はエラーメッセージ (成功なら空文字)。
    juce::String start(const juce::String& deviceName, AudioEngine& engine);
    // 停止 + エンジンから登録解除。リングは shared_ptr 所有 + エンジン側退役リスト保持なので
    // ここで手放しても安全 (解放は message thread の回収時)。
    void stop(AudioEngine& engine);

    bool isRunning() const { return device != nullptr; }
    juce::String getActiveDeviceName() const { return device != nullptr ? device->getName() : juce::String(); }

private:
    static std::unique_ptr<juce::AudioIODeviceType> createDeviceType();

    void audioDeviceIOCallbackWithContext(const float* const* inputChannelData, int numInputChannels,
                                          float* const* outputChannelData, int numOutputChannels,
                                          int numSamples,
                                          const juce::AudioIODeviceCallbackContext&) override;
    void audioDeviceAboutToStart(juce::AudioIODevice* d) override;
    void audioDeviceStopped() override;

    std::unique_ptr<juce::AudioIODevice> device;
    std::shared_ptr<StreamMirrorRing>    ring;
    StreamMirrorReader                   reader;             // ミラーデバイススレッド専用
    double                               dstRate { 0.0 };    // aboutToStart で確定

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(StreamMirrorOutput)
};
