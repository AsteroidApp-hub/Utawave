// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

#pragma once
#include <JuceHeader.h>
#include <array>
#include <atomic>
#include <vector>

/**
    内蔵エフェクト (EQ / コンプ / リバーブ) の共通基底。

    既存の INS スロット (PluginChain) は juce::AudioPluginInstance しか保持しないため、
    内蔵 DSP も AudioPluginInstance を継承して「内蔵プラグイン」として振る舞わせる。
    これにより保存 (.uta の getPluginDescription + getStateInformation 経路)・並べ替え・
    バイパス・D&D・エディタ窓・モニター経由 INS がすべて VST3 と同じ仕組みに乗る。

    パラメータは UI スレッドが書き audio スレッドが読むため std::atomic<float> で公開する
    (InternalSynth と同じく自前・依存最小。juce_dsp には依存しない)。
    DSP 本体 (係数算出 / 1 サンプル処理) は派生クラスが実装する。
*/
class BuiltInEffect : public juce::AudioPluginInstance
{
public:
    // パラメータ 1 個のメタ情報 (UI のノブ生成 / シリアライズキーに使う)
    // 文字列は静的リテラルへの const char* で保持する。juce::String メンバに UTF-8 の
    // const char* を直接代入すると debug で juce_String.cpp が assert するため (既知の落とし穴)、
    // 表示時のみ tr() / fromUTF8 を通す。
    struct ParamInfo
    {
        const char* id;      // シリアライズキー (ASCII・言語非依存・固定。例 "thresholdDb")
        const char* label;   // UI ラベル (UTF-8 日本語キー・tr() 経由で英訳)
        const char* unit;    // "Hz" / "dB" / "ms" / "%" / ":1" / "" (ASCII)
        float minV  { 0.0f };
        float maxV  { 1.0f };
        float defV  { 0.0f };
        float skew  { 1.0f };  // ロータリーのスキュー (1=リニア、<1 で低域を広く)
    };

    struct Preset
    {
        const char* name;             // 表示名 (UTF-8 日本語キー・tr() 経由)
        std::vector<float> values;    // params と同順 (足りない分は据え置き)
    };

    static constexpr int kMaxParams = 24;

    // ─── 派生が実装する識別情報 ───
    virtual juce::String getIdentifier() const = 0;  // "utawave.eq" 等 (言語非依存・固定)
    virtual juce::int32  getUid()        const = 0;  // PluginDescription.uniqueId (固定)

    // ─── パラメータアクセス ───
    int getParamCount() const noexcept { return paramCount; }
    const ParamInfo& getParamInfo(int i) const { return params[(size_t) i]; }

    float getP(int i) const noexcept
    {
        return values[(size_t) i].load(std::memory_order_relaxed);
    }
    void setP(int i, float v) noexcept
    {
        if (i < 0 || i >= paramCount) return;
        const auto& pi = params[(size_t) i];
        values[(size_t) i].store(juce::jlimit(pi.minV, pi.maxV, v), std::memory_order_relaxed);
    }

    const std::vector<Preset>& getPresets() const noexcept { return presets; }
    void applyPreset(int idx)
    {
        if (idx < 0 || idx >= (int) presets.size()) return;
        const auto& pr = presets[(size_t) idx];
        for (size_t i = 0; i < pr.values.size() && (int) i < paramCount; ++i)
            setP((int) i, pr.values[i]);
    }

    // コンプの GR メータ用 (デフォルトは無し)。派生がオーバーライドする。
    virtual bool  hasReductionMeter() const { return false; }
    virtual float getReductionDb()    const { return 0.0f; }  // 0..+ (減衰量 dB・正値)

    // ─── AudioProcessor / AudioPluginInstance 定型 ───
    bool   acceptsMidi()  const override { return false; }
    bool   producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    bool hasEditor() const override { return true; }
    juce::AudioProcessorEditor* createEditor() override;   // BuiltInEffectEditor.cpp で定義

    int  getNumPrograms()    override { return 1; }
    int  getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}

    void fillInPluginDescription(juce::PluginDescription& d) const override
    {
        // d.name は createIdentifierString() に入る → 保存 ID になるので**ロケール非依存の
        // 固定文字列 (= getIdentifier())** にする。getName() (tr 済み) を入れると言語切替で
        // 保存 ID が変わり、別言語で開いた時に内蔵エフェクトが照合できず無言で消える。
        // 表示名はチップ/窓ともに getName() を使うので UI は従来どおりローカライズされる。
        d.name             = getIdentifier();
        d.descriptiveName  = getIdentifier();
        d.pluginFormatName = "Utawave";
        d.category         = "Effect";
        d.manufacturerName = "Utawave";
        d.version          = "1.0";
        d.fileOrIdentifier = getIdentifier();
        d.uniqueId         = getUid();
        d.deprecatedUid    = getUid();
        d.isInstrument     = false;
        d.numInputChannels  = 2;
        d.numOutputChannels = 2;
    }

    void getStateInformation(juce::MemoryBlock& mb) override
    {
        juce::XmlElement xml("UtawaveBuiltIn");
        xml.setAttribute("id",  getIdentifier());
        xml.setAttribute("ver", 1);
        for (int i = 0; i < paramCount; ++i)
            xml.setAttribute(params[(size_t) i].id, (double) getP(i));
        copyXmlToBinary(xml, mb);
    }

    void setStateInformation(const void* data, int size) override
    {
        if (auto xml = getXmlFromBinary(data, size))
            for (int i = 0; i < paramCount; ++i)
                if (xml->hasAttribute(params[(size_t) i].id))
                    setP(i, (float) xml->getDoubleAttribute(params[(size_t) i].id, getP(i)));
    }

protected:
    BuiltInEffect()
        : juce::AudioPluginInstance(BusesProperties()
              .withInput ("Input",  juce::AudioChannelSet::stereo(), true)
              .withOutput("Output", juce::AudioChannelSet::stereo(), true)) {}

    // 派生のコンストラクタから呼ぶ。params への push と default 値の格納を一括で行う。
    void addParam(ParamInfo pi)
    {
        if (paramCount >= kMaxParams) { jassertfalse; return; }
        values[(size_t) paramCount].store(pi.defV, std::memory_order_relaxed);
        params.push_back(std::move(pi));
        ++paramCount;
    }
    void addPreset(const char* name, std::vector<float> v)
    {
        presets.push_back({ name, std::move(v) });
    }

    std::vector<ParamInfo> params;
    std::vector<Preset>    presets;
    int paramCount { 0 };
    std::array<std::atomic<float>, kMaxParams> values;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(BuiltInEffect)
};
