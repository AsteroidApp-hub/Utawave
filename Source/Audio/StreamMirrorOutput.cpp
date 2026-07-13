// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

#include "StreamMirrorOutput.h"
#include "AudioEngine.h"
#include "../Localisation.h"

std::unique_ptr<juce::AudioIODeviceType> StreamMirrorOutput::createDeviceType()
{
   #if JUCE_MAC
    return std::unique_ptr<juce::AudioIODeviceType>(
        juce::AudioIODeviceType::createAudioIODeviceType_CoreAudio());
   #elif JUCE_WINDOWS
    // 共有モード限定: 他アプリ (配信ソフト含む) と同じデバイスを同時に使える + OS の
    // オーディオセッションが生まれるのでアプリ音声キャプチャで拾える (排他/ASIO では不可)
    return std::unique_ptr<juce::AudioIODeviceType>(
        juce::AudioIODeviceType::createAudioIODeviceType_WASAPI(juce::WASAPIDeviceMode::shared));
   #else
    return nullptr;
   #endif
}

juce::StringArray StreamMirrorOutput::getOutputDeviceNames()
{
    auto type = createDeviceType();
    if (type == nullptr) return {};
    type->scanForDevices();
    return type->getDeviceNames(/*wantInputNames*/ false);
}

StreamMirrorOutput::~StreamMirrorOutput()
{
    if (device != nullptr)
    {
        device->stop();
        device->close();
    }
}

juce::String StreamMirrorOutput::start(const juce::String& deviceName, AudioEngine& engine)
{
    stop(engine);   // デバイス変更にも使うため、動いていれば一旦止める

    auto type = createDeviceType();
    if (type == nullptr)
        return tr(u8"この OS では配信ミラー出力を使えません");

    type->scanForDevices();
    const juce::StringArray names = type->getDeviceNames(/*wantInputNames*/ false);
    if (names.isEmpty())
        return tr(u8"出力デバイスが見つかりません");

    // 「OS 既定へのフォールバック」はしない (2026-07 撤去): 既定デバイス = メインで使っている
    // インターフェイスという環境が大半で、黙って同じ I/O へミラーすると二重聞こえ事故になる。
    // 未選択 / 繋ぎ直しで消えたデバイスは明示エラーにして、ユーザーに選び直させる
    if (deviceName.isEmpty() || !names.contains(deviceName))
        return tr(u8"ミラーの出力先デバイスが見つかりません。環境設定で選び直してください。");

    device.reset(type->createDevice(deviceName, {}));
    if (device == nullptr)
        return tr(u8"ミラー出力デバイスを開けませんでした") + ": " + deviceName;

    // SR はメインエンジンと同じものを優先 (リサンプル最小化)。無ければ 48k → 44.1k → 先頭。
    // 違っても reader のドリフト補正リサンプラが吸収するので動作はする。
    double sr = 0.0;
    {
        const auto rates  = device->getAvailableSampleRates();
        const double want = engine.getSampleRate();
        for (double cand : { want, 48000.0, 44100.0 })
        {
            for (auto r : rates)
                if (std::abs(r - cand) < 1.0) { sr = r; break; }
            if (sr > 0.0) break;
        }
        if (sr <= 0.0) sr = rates.isEmpty() ? 48000.0 : rates[0];
    }

    // バッファはデバイス既定 (低遅延の必要が無いので安定優先)
    juce::BigInteger outCh;
    outCh.setRange(0, 2, true);
    const juce::String err = device->open({}, outCh, sr, device->getDefaultBufferSize());
    if (err.isNotEmpty())
    {
        device.reset();
        return err;
    }

    // reader (ミラーデバイス側) より先にエンジンへ登録しても、reader は priming で
    // 目標水位まで待つだけなので順序の競合は無い
    ring = std::make_shared<StreamMirrorRing>();
    engine.setMirrorRing(ring);
    device->start(this);
    return {};
}

void StreamMirrorOutput::stop(AudioEngine& engine)
{
    if (device != nullptr)
    {
        device->stop();    // reader スレッド停止 (以降 ring を読む者はいない)
        device->close();
        device.reset();
    }
    if (ring != nullptr)
    {
        // 解除後もエンジンの退役リストが回収まで保持するので、ここで手放しても UAF にならない
        engine.setMirrorRing(nullptr);
        ring.reset();
    }
}

void StreamMirrorOutput::audioDeviceAboutToStart(juce::AudioIODevice* d)
{
    dstRate = d->getCurrentSampleRate();
    reader  = StreamMirrorReader();   // 溜め直しから開始
}

void StreamMirrorOutput::audioDeviceStopped() {}

void StreamMirrorOutput::audioDeviceIOCallbackWithContext(
    const float* const*, int,
    float* const* outputChannelData, int numOutputChannels, int numSamples,
    const juce::AudioIODeviceCallbackContext&)
{
    if (numOutputChannels <= 0 || outputChannelData == nullptr) return;

    // 3ch 以上のデバイスでは余りを無音に (ステレオのみ使う)
    for (int ch = 2; ch < numOutputChannels; ++ch)
        if (outputChannelData[ch] != nullptr)
            juce::FloatVectorOperations::clear(outputChannelData[ch], numSamples);

    float* L = outputChannelData[0];
    float* R = numOutputChannels > 1 ? outputChannelData[1] : nullptr;
    if (L == nullptr) return;

    if (ring != nullptr)
        reader.pull(*ring, L, R, numSamples, dstRate);
    else
    {
        juce::FloatVectorOperations::clear(L, numSamples);
        if (R != nullptr) juce::FloatVectorOperations::clear(R, numSamples);
    }
}
