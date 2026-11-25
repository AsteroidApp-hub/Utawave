// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

// Utawave — 書き出し結果の比較テスト (音の回帰テスト)
//
// 代表的なシーン (トラック Vol/Pan・フェード・クリップゲイン・内蔵エフェクト・マスター INS・
// PDC・MIDI 内蔵シンセ・リバーブ送り・CLICK・フォルダバス) を組み立てて、
//   1. .uta へ保存 → 別の TrackManager / AudioEngine へ読み直す (保存/復元の経路も通す)
//   2. 読み直した状態を renderOfflineRange でオフライン書き出し
//   3. 保存前の状態を直接書き出した結果とサンプル単位で一致すること (保存/復元で音が変わらない)
//   4. 書き出し結果の「指紋」(256 サンプルごとの RMS / ピーク / 平均・L/R) を
//      Tests/golden/<scene>.txt の基準と許容誤差つきで比較する
//
// 指紋にしているのは、WAV そのものを基準にすると容量が大きく、CPU/OS 間の浮動小数の
// わずかな差で完全一致しないため。平均 (DC) を含めることで数サンプルの時間ずれも検出できる。
//
// 基準の作成/更新: 環境変数 UTAWAVE_UPDATE_GOLDEN=1 を付けてテストを実行すると
// 基準ファイルを書き直す (意図して音を変えた時だけ行い、差分をコミットする)。
// 不一致時は一時フォルダに実際の指紋と WAV を書き出してパスを表示する (耳で確認できる)。

#include <JuceHeader.h>
#include <cmath>
#include <functional>

#include "../Source/Audio/AudioEngine.h"
#include "../Source/Tracks/TrackManager.h"
#include "../Source/Tracks/Track.h"
#include "../Source/Tracks/AudioClip.h"
#include "../Source/Tracks/MidiClip.h"
#include "../Source/VST/PluginChain.h"
#include "../Source/Project/ProjectManager.h"
#include "../Source/Audio/builtin/BuiltInFactory.h"
#include "../Source/Audio/builtin/BuiltInEQ.h"
#include "../Source/Audio/builtin/BuiltInCompressor.h"
#include "../Source/Audio/builtin/BuiltInDelay.h"
#include "../Source/Audio/builtin/BuiltInReverb.h"
#include "../Source/Audio/builtin/BuiltInMaximizer.h"

namespace
{
constexpr double kSR        = 48000.0;
constexpr int    kBlock     = 256;      // 指紋のブロック長 (≈5.3ms)
constexpr float  kAbsTol    = 2.0e-4f;  // 指紋の許容誤差 (絶対)
constexpr float  kRelTol    = 2.0e-3f;  // 指紋の許容誤差 (基準値に対する相対)

juce::File goldenDir()
{
    auto env = juce::SystemStats::getEnvironmentVariable("UTAWAVE_GOLDEN_DIR", {});
    if (env.isNotEmpty()) return juce::File(env);
   #ifdef UTAWAVE_TESTS_DIR
    return juce::File(juce::String::fromUTF8(UTAWAVE_TESTS_DIR)).getChildFile("golden");
   #else
    return juce::File::getCurrentWorkingDirectory().getChildFile("Tests/golden");
   #endif
}

bool updateMode()
{
    return juce::SystemStats::getEnvironmentVariable("UTAWAVE_UPDATE_GOLDEN", {}) == "1";
}

bool writeFloatWav(const juce::File& f, double sr, const juce::AudioBuffer<float>& buf)
{
    juce::WavAudioFormat waf;
    using SF = juce::AudioFormatWriterOptions::SampleFormat;
    auto wopts = juce::AudioFormatWriterOptions{}
                     .withSampleRate(sr)
                     .withNumChannels(buf.getNumChannels())
                     .withBitsPerSample(32)
                     .withSampleFormat(SF::floatingPoint);
    f.getParentDirectory().createDirectory();
    f.deleteFile();
    auto fos = std::make_unique<juce::FileOutputStream>(f);
    if (!fos->openedOk()) return false;
    std::unique_ptr<juce::OutputStream> os = std::move(fos);
    std::unique_ptr<juce::AudioFormatWriter> w(waf.createWriterFor(os, wopts));
    if (!w) return false;
    return w->writeFromAudioSampleBuffer(buf, 0, buf.getNumSamples());
}

// 決定論的なソース音源 (乱数を使わず数式だけで作る = どの環境でも同じ)
juce::AudioBuffer<float> makeSource(int numCh, double secs, const std::function<float(int ch, double t)>& fn)
{
    const int n = (int) std::lround(secs * kSR);
    juce::AudioBuffer<float> b(numCh, n);
    for (int ch = 0; ch < numCh; ++ch)
        for (int i = 0; i < n; ++i)
            b.setSample(ch, i, fn(ch, i / kSR));
    return b;
}

constexpr double kTwoPi = 6.283185307179586;

// ───────────────────────── 指紋 ─────────────────────────

struct Fingerprint
{
    int numSamples { 0 };
    // ブロックごとに [rmsL, peakL, meanL, rmsR, peakR, meanR]
    std::vector<std::array<float, 6>> blocks;
};

Fingerprint fingerprintOf(const juce::AudioBuffer<float>& b)
{
    Fingerprint fp;
    fp.numSamples = b.getNumSamples();
    for (int s = 0; s < fp.numSamples; s += kBlock)
    {
        const int n = juce::jmin(kBlock, fp.numSamples - s);
        std::array<float, 6> v {};
        for (int ch = 0; ch < 2; ++ch)
        {
            const float* p = b.getReadPointer(juce::jmin(ch, b.getNumChannels() - 1)) + s;
            double sq = 0.0, sum = 0.0; float pk = 0.0f;
            for (int i = 0; i < n; ++i)
            {
                sq += (double) p[i] * p[i];
                sum += p[i];
                pk = juce::jmax(pk, std::abs(p[i]));
            }
            v[(size_t) ch * 3 + 0] = (float) std::sqrt(sq / n);
            v[(size_t) ch * 3 + 1] = pk;
            v[(size_t) ch * 3 + 2] = (float) (sum / n);
        }
        fp.blocks.push_back(v);
    }
    return fp;
}

juce::String serialise(const Fingerprint& fp, const juce::String& scene)
{
    juce::String s;
    s << "# Utawave render golden v1 (scene: " << scene << ")\n"
      << "# regenerate: UTAWAVE_UPDATE_GOLDEN=1 UtawaveTests\n"
      << "samples " << fp.numSamples << "\n"
      << "block " << kBlock << "\n"
      << "# rmsL peakL meanL rmsR peakR meanR\n";
    for (auto& v : fp.blocks)
    {
        for (size_t k = 0; k < v.size(); ++k)
        {
            char tmp[32];
            std::snprintf(tmp, sizeof(tmp), "%.7g", (double) v[k]);
            s << (k ? " " : "") << tmp;
        }
        s << "\n";
    }
    return s;
}

bool parse(const juce::String& text, Fingerprint& fp)
{
    juce::StringArray lines;
    lines.addLines(text);
    int block = 0;
    for (auto& raw : lines)
    {
        auto line = raw.trim();
        if (line.isEmpty() || line.startsWithChar('#')) continue;
        if (line.startsWith("samples ")) { fp.numSamples = line.fromFirstOccurrenceOf(" ", false, false).getIntValue(); continue; }
        if (line.startsWith("block "))   { block = line.fromFirstOccurrenceOf(" ", false, false).getIntValue(); continue; }
        juce::StringArray tok;
        tok.addTokens(line, " ", {});
        tok.removeEmptyStrings();
        if (tok.size() != 6) return false;
        std::array<float, 6> v {};
        for (int k = 0; k < 6; ++k) v[(size_t) k] = tok[k].getFloatValue();
        fp.blocks.push_back(v);
    }
    return block == kBlock && fp.numSamples > 0 && ! fp.blocks.empty();
}

// ───────────────────────── シーン ─────────────────────────

struct Scene
{
    juce::AudioFormatManager fmt;
    TrackManager tm { fmt };
    AudioEngine engine;
    AppSettings settings;
    std::vector<Marker> markers;
    double bpm { 120.0 }, loopS { 0.0 }, loopE { 0.0 }, head { 0.0 }, ppb { 100.0 };
    bool loopAct { false };

    Scene() { fmt.registerBasicFormats(); }

    ProjectManager::State state(std::vector<juce::String>* missing = nullptr)
    {
        ProjectManager::State s;
        s.trackManager = &tm; s.appSettings = &settings; s.markers = &markers;
        s.bpm = &bpm; s.loopStartSecs = &loopS; s.loopEndSecs = &loopE;
        s.loopActive = &loopAct; s.playheadSecs = &head; s.pixelsPerBeat = &ppb;
        s.pluginManager = nullptr;
        s.pluginSampleRate = kSR;
        s.pluginBlockSize = 512;
        s.masterChain = &engine.getMasterChain();
        s.missingFiles = missing;
        return s;
    }

    juce::AudioBuffer<float> render(double secs, bool includeClick)
    {
        engine.preparePlayback(tm);
        juce::AudioBuffer<float> out;
        engine.renderOfflineRange(0.0, secs, out, {}, {}, false, includeClick);
        return out;
    }
};

template <typename T>
T* insertBuiltIn(PluginChain& chain, int slot, const juce::String& id)
{
    auto fx = BuiltInFactory::create(id);
    auto* raw = dynamic_cast<T*>(fx.get());
    if (raw == nullptr) return nullptr;
    chain.insertPluginAt(slot, std::unique_ptr<juce::AudioPluginInstance>(fx.release()));
    return raw;
}

AudioClip* addSourceClip(Track* t, const juce::File& audioDir, const juce::String& name,
                         const juce::AudioBuffer<float>& src, double start, double dur)
{
    auto f = audioDir.getChildFile(name + ".wav");
    if (! writeFloatWav(f, kSR, src)) return nullptr;
    auto* c = t->addClip(f, start, dur);
    if (c) { c->setFadeInSecs(0.0); c->setFadeOutSecs(0.0); }
    return c;
}

struct SceneDef
{
    const char* name;
    double      seconds;
    bool        includeClick;
    // シーンを組み立てる (audioDir に音源を置く)。false でセットアップ失敗
    std::function<bool(Scene&, const juce::File& audioDir)> build;
};

std::vector<SceneDef> sceneDefs()
{
    std::vector<SceneDef> v;

    // 1. 基本ミックス: Vol/Pan・フェード・クリップゲイン・ゲインエンベロープ・fileOffset・ミュート
    v.push_back({ "mix_basic", 3.0, false, [] (Scene& s, const juce::File& dir)
    {
        auto srcA = makeSource(1, 3.0, [] (int, double t)
            { return (float) (0.5 * std::sin(kTwoPi * 220.0 * t) * (0.6 + 0.4 * std::sin(kTwoPi * 1.5 * t))); });
        auto srcB = makeSource(2, 3.0, [] (int ch, double t)
            { return ch == 0 ? (float) (0.3 * std::sin(kTwoPi * 330.0 * t) + 0.1 * std::sin(kTwoPi * 990.0 * t))
                             : (float) (0.3 * std::sin(kTwoPi * 440.0 * t) + 0.1 * std::sin(kTwoPi * 1320.0 * t)); });
        auto srcC = makeSource(1, 1.0, [] (int, double) { return 0.7f; });

        auto* a = s.tm.addTrack("A", false);
        a->setVolume(-3.0f); a->setPan(-0.4f);
        auto* ca = addSourceClip(a, dir, "a", srcA, 0.25, 2.5);
        if (! ca) return false;
        ca->setFadeInSecs(0.2); ca->setFadeOutSecs(0.3); ca->setGain(0.8f);
        ca->getGainPointsRW() = { { 0.5, 0.0f }, { 1.2, -9.0f }, { 2.0, 3.0f } };

        auto* b = s.tm.addTrack("B", true);
        b->setVolume(0.0f); b->setPan(0.5f);
        auto* cb = addSourceClip(b, dir, "b", srcB, 0.0, 2.0);
        if (! cb) return false;
        cb->setFileOffset(0.5); cb->setFadeOutSecs(0.05);

        auto* m = s.tm.addTrack("Muted", false);      // ミュートは書き出しに乗らない
        m->setMuted(true);
        return addSourceClip(m, dir, "c", srcC, 0.0, 1.0) != nullptr;
    } });

    // 2. 内蔵エフェクトのチェーン (EQ → コンプ → ディレイ → リバーブ) + マスターにマキシマイザー (PDC)
    v.push_back({ "fx_chain", 3.0, false, [] (Scene& s, const juce::File& dir)
    {
        // 0.5 秒ごとの減衰するバースト (コンプ/ディレイ/リバーブの動きが指紋に出る)
        auto src = makeSource(1, 3.0, [] (int, double t)
        {
            const double ph = std::fmod(t, 0.5);
            return (float) (0.8 * std::exp(-ph * 12.0) * std::sin(kTwoPi * 330.0 * t)
                          + 0.2 * std::exp(-ph * 30.0) * std::sin(kTwoPi * 3100.0 * t));
        });
        auto* t = s.tm.addTrack("Vocal", false);
        t->setVolume(-2.0f);
        if (! addSourceClip(t, dir, "burst", src, 0.0, 3.0)) return false;

        auto& chain = t->getPluginChain();
        auto* eq = insertBuiltIn<BuiltInEQ>(chain, 0, "utawave.eq");
        auto* cp = insertBuiltIn<BuiltInCompressor>(chain, 1, "utawave.comp");
        auto* dl = insertBuiltIn<BuiltInDelay>(chain, 2, "utawave.delay");
        auto* rv = insertBuiltIn<BuiltInReverb>(chain, 3, "utawave.reverb");
        auto* mx = insertBuiltIn<BuiltInMaximizer>(s.engine.getMasterChain(), 0, "utawave.maximizer");
        if (! (eq && cp && dl && rv && mx)) return false;
        eq->setP(BuiltInEQ::HpfHz, 120.0f);
        eq->setP(BuiltInEQ::MidDb, 4.0f);
        eq->setP(BuiltInEQ::AirDb, -3.0f);
        cp->setP(BuiltInCompressor::ThresholdDb, -18.0f);
        cp->setP(BuiltInCompressor::Ratio, 4.0f);
        dl->setP(BuiltInDelay::Sync, 0.0f);
        dl->setP(BuiltInDelay::TimeMs, 150.0f);
        dl->setP(BuiltInDelay::Feedback, 35.0f);
        dl->setP(BuiltInDelay::Mix, 30.0f);
        rv->setP(BuiltInReverb::MixPct, 25.0f);
        mx->setP(BuiltInMaximizer::DriveDb, 6.0f);
        mx->setP(BuiltInMaximizer::CeilingDb, -1.0f);
        return true;
    } });

    // 3. MIDI トラック (内蔵シンセ): 波形違い・移調・和音
    v.push_back({ "midi_synth", 2.5, false, [] (Scene& s, const juce::File&)
    {
        auto addMidi = [&] (const char* name, int wave, int transpose, float pan,
                            std::initializer_list<std::tuple<int, double, double, int>> notes)
        {
            auto* t = s.tm.addTrack(name, false, -1, true);
            if (! t) return false;
            t->setMidiTrack(true);
            t->setSynthEnabled(true);
            t->setSynthWaveform(wave);
            t->setSemitoneTranspose(transpose);
            t->setVolume(0.0f); t->setPan(pan);
            auto* mc = t->addMidiClip(0.0, 2.0);
            if (! mc) return false;
            auto& seq = mc->getSequence();
            for (auto& [note, on, off, vel] : notes)
            {
                seq.addEvent(juce::MidiMessage::noteOn(1, note, (juce::uint8) vel), on);
                seq.addEvent(juce::MidiMessage::noteOff(1, note), off);
            }
            seq.updateMatchedPairs();
            return true;
        };
        return addMidi("Saw", 1, 2, -0.3f, { { 60, 0.1, 0.9, 100 }, { 64, 0.1, 0.9, 90 }, { 67, 1.0, 1.8, 110 } })
            && addMidi("Sine", 0, 0, 0.3f, { { 72, 0.3, 1.2, 80 }, { 76, 1.25, 1.9, 120 } })
            && addMidi("Square", 2, -12, 0.0f, { { 48, 0.5, 1.5, 70 } });
    } });

    // 4. フォルダバス (INS + Vol) + 子トラックのリバーブ送り + CLICK (メトロノーム)
    v.push_back({ "folder_send_click", 3.0, true, [] (Scene& s, const juce::File& dir)
    {
        auto src1 = makeSource(1, 3.0, [] (int, double t) { return (float) (0.4 * std::sin(kTwoPi * 262.0 * t)); });
        auto src2 = makeSource(1, 3.0, [] (int, double t)
            { return (float) (0.3 * std::sin(kTwoPi * 392.0 * t) * (t < 1.5 ? 1.0 : 0.0)); });

        auto* c1 = s.tm.addTrack("Child1", false);
        auto* c2 = s.tm.addTrack("Child2", false);
        if (! addSourceClip(c1, dir, "c1", src1, 0.0, 3.0)) return false;
        if (! addSourceClip(c2, dir, "c2", src2, 0.0, 3.0)) return false;
        c1->setPan(-0.5f);
        c2->setPan(0.6f);
        c2->setReverbSend(0.5f);            // ノートが切れた後もテールが残る

        auto* folder = s.tm.addFolderTrack();
        if (! folder) return false;
        folder->setVolume(-4.0f);
        c1->setFolderParent(folder);
        c2->setFolderParent(folder);
        s.tm.normalizeFolderContiguity();
        auto* eq = insertBuiltIn<BuiltInEQ>(folder->getPluginChain(), 0, "utawave.eq");
        if (! eq) return false;
        eq->setP(BuiltInEQ::LowDb, -6.0f);

        auto* click = s.tm.addClickTrack();
        if (! click) return false;
        click->setVolume(-6.0f);
        return true;
    } });

    // 5. PDC: 片方のトラックだけにルックアヘッド (マキシマイザー)。立ち上がりがずれないこと
    v.push_back({ "pdc_align", 2.0, false, [] (Scene& s, const juce::File& dir)
    {
        // 0.25 秒ごとの短いクリック列 (時間ずれが平均・ピーク位置に直接出る)
        auto clicks = makeSource(1, 2.0, [] (int, double t)
        {
            const double ph = std::fmod(t, 0.25);
            return ph < 0.004 ? (float) (0.5 * std::sin(kTwoPi * 1000.0 * t)) : 0.0f;
        });
        auto* dry = s.tm.addTrack("Dry", false);
        auto* wet = s.tm.addTrack("Lookahead", false);
        dry->setPan(-1.0f);
        wet->setPan(1.0f);
        if (! addSourceClip(dry, dir, "dry", clicks, 0.0, 2.0)) return false;
        if (! addSourceClip(wet, dir, "wet", clicks, 0.0, 2.0)) return false;
        auto* mx = insertBuiltIn<BuiltInMaximizer>(wet->getPluginChain(), 0, "utawave.maximizer");
        if (! mx) return false;
        mx->setP(BuiltInMaximizer::DriveDb, 0.0f);
        mx->setP(BuiltInMaximizer::CeilingDb, 0.0f);
        return true;
    } });

    return v;
}
} // namespace

class RenderGoldenTests : public juce::UnitTest
{
public:
    RenderGoldenTests() : juce::UnitTest("Render golden (audio regression)", "Utawave") {}

    void runTest() override
    {
        // 不一致時の実際の WAV を残す場所。CI は UTAWAVE_RENDER_OUT で artifact 用フォルダを指定する
        auto outEnv = juce::SystemStats::getEnvironmentVariable("UTAWAVE_RENDER_OUT", {});
        // (開始時に丸ごと消すので、指定フォルダ自体ではなくその下の専用サブフォルダを使う)
        tempRoot = (outEnv.isNotEmpty() ? juce::File(outEnv)
                                        : juce::File::getSpecialLocation(juce::File::tempDirectory))
                       .getChildFile("UtawaveRenderGolden");
        tempRoot.deleteRecursively();
        tempRoot.createDirectory();

        for (auto& def : sceneDefs())
            runScene(def);

        if (! keepTemp) tempRoot.deleteRecursively();
    }

private:
    juce::File tempRoot;
    bool keepTemp { false };

    void runScene(const SceneDef& def)
    {
        beginTest(juce::String("render golden: ") + def.name);

        auto projDir = tempRoot.getChildFile(def.name);
        auto audioDir = projDir.getChildFile("Audio");
        audioDir.createDirectory();
        auto projFile = projDir.getChildFile(juce::String(def.name) + ".uta");

        // 1. 組み立て → 保存前の状態を直接書き出し
        Scene a;
        if (! def.build(a, audioDir)) { expect(false, "scene setup failed"); return; }
        auto direct = a.render(def.seconds, def.includeClick);
        expect(ProjectManager::save(projFile, a.state()), "save .uta");

        // 2. 読み直して書き出し (保存/復元の経路を通した音 = 実アプリで開いた時の音)
        Scene b;
        std::vector<juce::String> missing;
        auto st = b.state(&missing);
        expect(ProjectManager::load(projFile, st), "load .uta");
        expect(missing.empty(), "no missing audio after load");
        auto loaded = b.render(def.seconds, def.includeClick);

        const int expectedLen = (int) std::lround(def.seconds * kSR);
        expect(loaded.getNumChannels() == 2 && loaded.getNumSamples() == expectedLen,
               "rendered 2ch, expected length");

        // 3. 保存/復元で音が変わらない (同じ環境の同じ処理なのでほぼ完全一致する)
        float maxDiff = 0.0f;
        if (direct.getNumSamples() == loaded.getNumSamples())
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < loaded.getNumSamples(); ++i)
                    maxDiff = juce::jmax(maxDiff, std::abs(direct.getSample(ch, i) - loaded.getSample(ch, i)));
        else
            maxDiff = 1.0f;
        expect(maxDiff < 1.0e-5f, "save/load round trip renders identically (max diff "
                                      + juce::String(maxDiff) + ")");

        // 無音でないこと (シーンが壊れて全部無音になった基準を作らないため)
        expect(loaded.getMagnitude(0, 0, loaded.getNumSamples()) > 0.01f, "render is not silent");
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < loaded.getNumSamples(); ++i)
                if (! std::isfinite(loaded.getSample(ch, i))) { expect(false, "render has NaN/Inf"); return; }

        // 4. 基準と比較
        auto fp = fingerprintOf(loaded);
        auto goldenFile = goldenDir().getChildFile(juce::String(def.name) + ".txt");

        if (updateMode())
        {
            goldenFile.getParentDirectory().createDirectory();
            expect(goldenFile.replaceWithText(serialise(fp, def.name)), "write golden");
            logMessage("  golden updated: " + goldenFile.getFullPathName());
            return;
        }

        Fingerprint ref;
        if (! goldenFile.existsAsFile() || ! parse(goldenFile.loadFileAsString(), ref))
        {
            expect(false, "golden missing or unreadable: " + goldenFile.getFullPathName()
                          + " (run with UTAWAVE_UPDATE_GOLDEN=1 to create)");
            return;
        }

        int mismatches = 0;
        juce::String firstReport;
        if (ref.numSamples != fp.numSamples || ref.blocks.size() != fp.blocks.size())
        {
            ++mismatches;
            firstReport = "length differs: golden " + juce::String(ref.numSamples)
                        + " / actual " + juce::String(fp.numSamples);
        }
        else
        {
            static const char* kNames[] = { "rmsL", "peakL", "meanL", "rmsR", "peakR", "meanR" };
            for (size_t bi = 0; bi < fp.blocks.size(); ++bi)
                for (size_t k = 0; k < 6; ++k)
                {
                    const float r = ref.blocks[bi][k], x = fp.blocks[bi][k];
                    if (std::abs(r - x) > kAbsTol + kRelTol * std::abs(r))
                    {
                        if (mismatches++ < 5)
                            firstReport << "\n    t=" << juce::String(bi * kBlock / kSR, 4) << "s "
                                        << kNames[k] << " golden " << r << " actual " << x;
                    }
                }
        }

        if (mismatches > 0)
        {
            keepTemp = true;
            auto actualTxt = projDir.getChildFile("actual_fingerprint.txt");
            auto actualWav = projDir.getChildFile("actual.wav");
            actualTxt.replaceWithText(serialise(fp, def.name));
            writeFloatWav(actualWav, kSR, loaded);
            logMessage("  " + juce::String(mismatches) + " fingerprint values differ:" + firstReport
                       + "\n  actual render: " + actualWav.getFullPathName()
                       + "\n  if the change is intended, rerun with UTAWAVE_UPDATE_GOLDEN=1 and commit Tests/golden/");
        }
        expect(mismatches == 0, juce::String("render matches golden: ") + def.name);
    }
};

static RenderGoldenTests renderGoldenTests;
