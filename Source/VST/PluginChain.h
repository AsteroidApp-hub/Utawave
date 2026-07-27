// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

#pragma once
#include <JuceHeader.h>

/**
    トラック単位のインサートエフェクトチェーン。
    複数のプラグインを直列に通し、入力バッファをそのまま加工する。
*/
class PluginChain : private juce::AudioProcessorListener,
                    private juce::AsyncUpdater
{
public:
    PluginChain();
    ~PluginChain() override;

    // プラグイン出力の安全上限 (絶対値・+18dBFS 相当)。processBlock が各プラグインの後で
    // これを超えるサンプル / NaN / Inf を検出したブロックだけを修復する (正常時は検出スキャンのみ)。
    // プラグインが UI 操作 (オーバーサンプリング切替等) の内部再構成中に未初期化バッファ由来の
    // 巨大値や NaN を吐いても、爆音がスピーカーへ直行したり後段 (リバーブバス / PDC 遅延ライン /
    // 他プラグイン) の内部状態を汚染したりしないための安全網。正規の信号がプラグイン間で
    // +18dBFS を超えることは実用上無く、ゴミは桁違い (1e10 等) なので誤クランプは起きない。
    static constexpr float kMaxPluginSample = 8.0f;

    // ─── プラグイン管理（メッセージスレッドから呼び出す） ───
    void   addPlugin(std::unique_ptr<juce::AudioPluginInstance> plugin);
    // 指定スロット index にプラグインを挿入。
    // slotIdx までの隙間は空スロットで埋まる（空スロットは音声処理ではスキップされる）
    void   insertPluginAt(int slotIdx, std::unique_ptr<juce::AudioPluginInstance> plugin);
    void   removePlugin(int index);
    // プラグインインスタンスを破棄せず取り出す（D&D 移動など、別チェーンに移すために使う）。
    // スロットは空になり末尾の空スロットは掃除される。返り値が空（nullptr）の場合は何もしない。
    std::unique_ptr<juce::AudioPluginInstance> extractPlugin(int index);
    void   movePlugin(int from, int to);
    void   swapSlots(int a, int b);
    int    getNumPlugins() const;
    // ロックを取らずにスロット数を読む (audio thread のガード用)。値は getNumPlugins() と一致する。
    int    getActivePluginCountAtomic() const noexcept
    {
        return activePluginCount.load(std::memory_order_acquire);
    }
    juce::AudioPluginInstance* getPlugin(int index) const;

    // ─── オーディオ処理 ───
    void prepareToPlay(double sampleRate, int blockSize);
    void releaseResources();

    // オフライン書き出し (バウンス) 用にプラグインの nonRealtime フラグを切り替えて再 prepare する。
    // JUCE のオフラインレンダー契約は「setNonRealtime → prepareToPlay」の順。オーバーサンプリング /
    // ルックアヘッド / レンダリング品質モードを持つプラグインは、realtime 前提で確保したバッファのまま
    // 書き出しで叩かれると過走して null/低位アドレス書き込みでクラッシュすることがある。書き出し前に
    // offline=true で呼んで再構成し、書き出し後に offline=false で realtime へ復帰させる。
    // setNonRealtime を効かせるには prepareToPlay の前に設定する必要があるため必ず再 prepare する
    // (isPreparedFor ガードは通さない)。**入力モニター中のチェーンには使わないこと** — releaseResources +
    // 再確保が chainLock 保持下で audio thread をブロックし (優先度逆転)、プラグイン状態もリセットされて
    // モニターがグリッチする。呼び出し側 (renderOfflineRange) がモニターチェーンを除外する。
    void setOfflineRenderMode(bool offline, double sampleRate, int blockSize);
    /** buffer をプラグインチェーン全体に通す（in-place）。
        @param playHead 各プラグインへ供給する再生位置情報 (Melodyne 等の transport 同期用)。
                        nullptr なら設定しない（書き出し以外の経路や transport 不要なテスト用）。 */
    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi,
                      juce::AudioPlayHead* playHead = nullptr);

    // バイパス（個別プラグイン）
    void setBypassed(int index, bool bypassed);
    bool isBypassed(int index) const;

    // 合計遅延サンプル（PDC 用）。現状は呼び出し側で参照のみ
    int getTotalLatencySamples() const;

    // 指定 SR / blockSize で既に prepareToPlay 済みか。モニター経路 (setMonitorChain) と
    // 書き出し経路 (renderOfflineRange・別スレッド) が、同設定での二重 prepare
    // (プラグイン状態リセット = グリッチ) を避けるための軽量判定。prepared* は書き出しの
    // バックグラウンドスレッドからも読まれる (prepareToPlay の message thread 書き込みと競合し
    // うる) ため、(SR, blockSize) を 1 組として preparedLock 下で読み書きする — 独立した
    // atomic 2 本だと 2 つの store の間に読んで「新 SR + 旧 blockSize」の混合ペアを観測しうる
    // (混合ペアがクエリ値に一致すると必要な prepare がスキップされる)。読み手は message /
    // export thread のみ (audio thread からは呼ばない) なので SpinLock で問題ない。
    bool isPreparedFor(double sr, int bs) const noexcept
    {
        const auto [psr, pbs] = getPrepared();
        return psr == sr && pbs == bs;
    }

    // 変更通知
    std::function<void()> onChainChanged;

    // プラグインが setLatencySamples でレイテンシを変えた時にメッセージスレッドで呼ばれる
    // (AudioProcessorListener::audioProcessorChanged の latencyChanged を AsyncUpdater で
    // メッセージスレッドへ集約)。AudioEngine が invalidatePlayback (PDC 再構築) を配線する。
    // これが無いとプラグインのモード切替 (ルックアヘッド ON/OFF 等) 後に trackDelays が
    // stale なまま整合がずれ続ける。
    std::function<void()> onLatencyChanged;

private:
    // ─── AudioProcessorListener (プラグインからの変更通知) ───
    // audioProcessorChanged はプラグイン次第で audio thread からも呼ばれうるため、ここでは
    // triggerAsyncUpdate だけ行い、コールバック実行はメッセージスレッド (handleAsyncUpdate) に寄せる。
    void audioProcessorChanged(juce::AudioProcessor*, const ChangeDetails& details) override
    {
        if (details.latencyChanged)
            triggerAsyncUpdate();
    }
    void audioProcessorParameterChanged(juce::AudioProcessor*, int, float) override {}
    void handleAsyncUpdate() override
    {
        if (onLatencyChanged) onLatencyChanged();
    }
    struct Slot
    {
        std::unique_ptr<juce::AudioPluginInstance> plugin;
        bool bypassed { false };
    };

    // 1 プラグインをチェーンの I/O 規約で prepare し直す共通シーケンス。**chainLock 保持下で呼ぶこと**。
    // releaseResources → disableNonMainBuses → setPlayConfigDetails(2,2) → prepareToPlay の順序には
    // 微妙な制約がある (一部プラグインは prepare 前の releaseResources が必須)。prepareToPlay と
    // setOfflineRenderMode の両方がこれを使い、順序がドリフトしないようにする。
    static void prepareSlot(Slot* s, double sampleRate, int blockSize);

    juce::OwnedArray<Slot>      slots;
    // processBlock() (audio thread) でも取得するブロッキングロック。プラグインの
    // 追加/削除/並べ替えは UI 側の低頻度操作のため実用上問題ないが、原則としては妥協
    // (#R-10)。lock-free 化は Phase 2 の課題 (AudioEngine の playbackLock と同様)。
    mutable juce::CriticalSection chainLock;
    // chainLock 下でスロット構成を変えるたびに slots.size() を release-store する。
    // audio thread はこれを acquire-load してロック無しで「処理対象あり」を判定する。
    std::atomic<int> activePluginCount { 0 };
    // 書き出しの別スレッドからも isPreparedFor 経由で読まれるため、(SR, blockSize) を
    // 1 組として preparedLock 下で読み書きする (torn read / 混合ペア観測の回避)。
    // 直接触らず setPrepared / getPrepared を経由すること。
    mutable juce::SpinLock preparedLock;
    double preparedSampleRate { 0.0 };
    int    preparedBlockSize  { 0 };

    void setPrepared(double sr, int bs) noexcept
    {
        const juce::SpinLock::ScopedLockType l(preparedLock);
        preparedSampleRate = sr;
        preparedBlockSize  = bs;
    }
    std::pair<double, int> getPrepared() const noexcept
    {
        const juce::SpinLock::ScopedLockType l(preparedLock);
        return { preparedSampleRate, preparedBlockSize };
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PluginChain)
};
