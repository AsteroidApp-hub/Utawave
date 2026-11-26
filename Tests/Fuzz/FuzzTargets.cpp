// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

// Utawave — ファジング対象の実装
//
// どのターゲットも「本物のアプリが外部データを読む時と同じ関数」を呼ぶ。シードは実装の
// 書き出し関数 (ProjectManager::save / JUCE の各 writer / MidiFile / getStateInformation) で
// その場生成するので、フォーマットが変わってもシードが古くならない。

#include "FuzzTargets.h"

#include <cmath>
#include <cstring>
#include <unordered_map>

#include "../../Source/Audio/AudioEngine.h"
#include "../../Source/Audio/AudioFileImporter.h"
#include "../../Source/Audio/builtin/BuiltInEffect.h"
#include "../../Source/Audio/builtin/BuiltInFactory.h"
#include "../../Source/Export/Mp3EncoderWriter.h"
#include "../../Source/MIDI/MidiImporter.h"
#include "../../Source/Project/AdService.h"
#include "../../Source/Project/ProjectManager.h"
#include "../../Source/Project/SafeText.h"
#include "../../Source/Project/UpdateChecker.h"
#include "../../Source/Tracks/AudioClip.h"
#include "../../Source/Tracks/MidiClip.h"
#include "../../Source/Tracks/Track.h"
#include "../../Source/Tracks/TrackManager.h"
#include "../../Source/VST/PluginChain.h"

namespace utafuzz
{
namespace
{
constexpr double kSR = 48000.0;

Bytes toBytes(const juce::MemoryBlock& mb)
{
    auto* p = static_cast<const uint8_t*>(mb.getData());
    return Bytes(p, p + mb.getSize());
}

Bytes toBytes(const juce::String& s)
{
    const char* p = s.toRawUTF8();
    return Bytes(p, p + std::strlen(p));
}

Bytes fileBytes(const juce::File& f)
{
    juce::MemoryBlock mb;
    f.loadFileAsData(mb);
    return toBytes(mb);
}

void writeBytes(const juce::File& f, const uint8_t* data, size_t size)
{
    f.replaceWithData(data, size);
}

juce::String asString(const uint8_t* data, size_t size)
{
    // アプリがネットワーク応答/キャッシュを文字列にするのと同じ変換 (不正な UTF-8 は空になる)
    return SafeText::fromUtf8OrEmpty(data, size);
}

void requireFinite(const juce::AudioBuffer<float>& b, const char* what)
{
    for (int ch = 0; ch < b.getNumChannels(); ++ch)
    {
        const float* p = b.getReadPointer(ch);
        for (int i = 0; i < b.getNumSamples(); ++i)
            if (! std::isfinite(p[i]))
                fuzzFail(what);
    }
}

juce::AudioBuffer<float> tone(int numCh, int numSamples, double hz)
{
    juce::AudioBuffer<float> b(numCh, numSamples);
    for (int ch = 0; ch < numCh; ++ch)
        for (int i = 0; i < numSamples; ++i)
            b.setSample(ch, i, (float) (0.4 * std::sin(6.283185307179586 * hz * (ch + 1) * i / kSR)));
    return b;
}

// JUCE の writer で音声ファイルを作る (シード生成用)
bool writeAudio(juce::AudioFormat& fmt, const juce::File& f, const juce::AudioBuffer<float>& buf,
                int bits, bool isFloat, const std::unordered_map<juce::String, juce::String>& meta = {})
{
    f.deleteFile();
    auto fos = std::make_unique<juce::FileOutputStream>(f);
    if (! fos->openedOk()) return false;
    std::unique_ptr<juce::OutputStream> os = std::move(fos);
    using SF = juce::AudioFormatWriterOptions::SampleFormat;
    auto opts = juce::AudioFormatWriterOptions{}
                    .withSampleRate(kSR)
                    .withNumChannels(buf.getNumChannels())
                    .withBitsPerSample(bits)
                    .withSampleFormat(isFloat ? SF::floatingPoint : SF::integral)
                    .withMetadataValues(meta);
    std::unique_ptr<juce::AudioFormatWriter> w(fmt.createWriterFor(os, opts));
    if (w == nullptr) return false;
    return w->writeFromAudioSampleBuffer(buf, 0, buf.getNumSamples());
}

// JUCE は 64bit WAV を書けないため手書きする (アプリは自前で読む = 重要な攻撃面)
Bytes wav64(int channels, int frames, bool isFloat)
{
    juce::MemoryOutputStream m;
    const int bytesPerFrame = channels * 8;
    const int dataBytes = frames * bytesPerFrame;
    m.write("RIFF", 4); m.writeInt(36 + dataBytes); m.write("WAVE", 4);
    m.write("fmt ", 4); m.writeInt(16);
    m.writeShort((short) (isFloat ? 3 : 1)); m.writeShort((short) channels);
    m.writeInt((int) kSR); m.writeInt((int) kSR * bytesPerFrame);
    m.writeShort((short) bytesPerFrame); m.writeShort(64);
    m.write("data", 4); m.writeInt(dataBytes);
    for (int i = 0; i < frames; ++i)
        for (int ch = 0; ch < channels; ++ch)
        {
            const double v = 0.3 * std::sin(0.05 * i * (ch + 1));
            if (isFloat) m.writeDouble(v);
            else         m.writeInt64((juce::int64) (v * 9.2e18));
        }
    return toBytes(m.getMemoryBlock());
}

juce::AudioFormatManager& formats()
{
    static juce::AudioFormatManager fm;
    static bool init = false;
    if (! init) { fm.registerBasicFormats(); init = true; }
    return fm;
}

// 音声ファイルを「再生と同じ読み方」で読む: reader を作り、最初と途中から少しずつ読む
void decodeLikePlayback(const juce::File& f)
{
    std::unique_ptr<juce::AudioFormatReader> r(formats().createReaderFor(f));
    if (r == nullptr) return;
    juce::AudioBuffer<float> buf(2, 1024);
    const juce::int64 len = r->lengthInSamples;
    const juce::int64 starts[] = { 0, len / 2, len - 100, len + 1000, -50 };
    for (auto s : starts)
        r->read(&buf, 0, buf.getNumSamples(), s, true, true);
}

// ───────────────────────── .uta (プロジェクトファイル) ─────────────────────────

juce::File utaDir() { return workDir().getChildFile("uta"); }

void ensureUtaAudio()
{
    auto audio = utaDir().getChildFile("Audio").getChildFile("tone.wav");
    if (! audio.existsAsFile())
    {
        audio.getParentDirectory().createDirectory();
        juce::WavAudioFormat wav;
        writeAudio(wav, audio, tone(1, 24000, 440.0), 32, true);
    }
}

struct ProjectState
{
    juce::AudioFormatManager& fm = formats();
    TrackManager tm { fm };
    AppSettings settings;
    std::vector<Marker> markers;
    double bpm { 120.0 }, loopS { 0.0 }, loopE { 0.0 }, head { 0.0 }, ppb { 100.0 };
    bool loopAct { false };
    std::vector<juce::String> missing;

    ProjectManager::State state(PluginChain* master)
    {
        ProjectManager::State s;
        s.trackManager = &tm; s.appSettings = &settings; s.markers = &markers;
        s.bpm = &bpm; s.loopStartSecs = &loopS; s.loopEndSecs = &loopE;
        s.loopActive = &loopAct; s.playheadSecs = &head; s.pixelsPerBeat = &ppb;
        s.pluginManager = nullptr; s.pluginSampleRate = kSR; s.pluginBlockSize = 512;
        s.masterChain = master; s.missingFiles = &missing;
        return s;
    }
};

void runUta(const uint8_t* data, size_t size)
{
    ensureUtaAudio();
    auto f = utaDir().getChildFile("fuzz.uta");
    writeBytes(f, data, size);

    AudioEngine engine;
    {
        ProjectState p;
        auto st = p.state(&engine.getMasterChain());
        if (ProjectManager::load(f, st))
        {
            // 読み込んだ値でそのまま鳴らす (壊れた値が再生/書き出しで暴れないか)
            engine.preparePlayback(p.tm);
            juce::AudioBuffer<float> out;
            const double head = std::isfinite(p.head) ? juce::jlimit(0.0, 600.0, p.head) : 0.0;
            engine.renderOfflineRange(0.0, 0.1, out);
            requireFinite(out, "uta: render from t=0 produced NaN/Inf");
            engine.renderOfflineRange(head, head + 0.1, out, {}, {}, true);
            requireFinite(out, "uta: pre-fader render at playhead produced NaN/Inf");
            // 保存し直せること (読めたプロジェクトは必ず書ける)
            ProjectManager::save(utaDir().getChildFile("resaved.uta"), st);
        }
        engine.clearPlayback();   // TrackManager の破棄前に再生スナップショットを手放す (本体と同じ作法)
    }
    engine.getMasterChain().releaseResources();
}

std::vector<Bytes> seedsUta()
{
    ensureUtaAudio();
    std::vector<Bytes> out;
    AudioEngine engine;
    ProjectState p;
    auto audio = utaDir().getChildFile("Audio").getChildFile("tone.wav");

    auto* a = p.tm.addTrack("Vocal", true);
    a->setVolume(-3.0f); a->setPan(0.25f); a->setReverbSend(0.3f);
    if (auto* c = a->addClip(audio, 0.05, 0.4))
    {
        c->setFadeInSecs(0.02); c->setFadeOutSecs(0.05); c->setGain(0.8f); c->setFileOffset(0.01);
        c->getGainPointsRW() = { { 0.0, -3.0f }, { 0.2, 2.0f } };
    }
    if (auto fx = BuiltInFactory::create("utawave.eq"))
        a->getPluginChain().insertPluginAt(0, std::unique_ptr<juce::AudioPluginInstance>(fx.release()));
    if (auto fx = BuiltInFactory::create("utawave.comp"))
        a->getPluginChain().insertPluginAt(1, std::unique_ptr<juce::AudioPluginInstance>(fx.release()));

    auto* m = p.tm.addTrack("Melody", false, -1, true);
    m->setSynthEnabled(true); m->setSemitoneTranspose(3);
    if (auto* mc = m->addMidiClip(0.0, 1.0))
    {
        mc->getSequence().addEvent(juce::MidiMessage::noteOn(1, 64, (juce::uint8) 100), 0.0);
        mc->getSequence().addEvent(juce::MidiMessage::controllerEvent(1, 1, 64), 0.1);
        mc->getSequence().addEvent(juce::MidiMessage::pitchWheel(1, 9000), 0.2);
        mc->getSequence().addEvent(juce::MidiMessage::noteOff(1, 64), 0.5);
        mc->getSequence().updateMatchedPairs();
    }
    auto* folder = p.tm.addFolderTrack();
    auto* child = p.tm.addTrack("Child", false);
    if (folder && child) { child->setFolderParent(folder); p.tm.normalizeFolderContiguity(); }
    p.tm.addClickTrack();
    if (auto fx = BuiltInFactory::create("utawave.maximizer"))
        engine.getMasterChain().insertPluginAt(0, std::unique_ptr<juce::AudioPluginInstance>(fx.release()));

    p.settings.bpmChanges   = { BpmChange { 1.0, 140.0 } };
    p.settings.meterChanges = { MeterChange { 2, 3, 4 } };
    p.settings.lyrics = "la la\nla";
    p.markers = { { 0.5, "Verse", juce::Colours::red } };
    p.loopS = 0.1; p.loopE = 0.6; p.loopAct = true; p.head = 0.2;

    auto f = utaDir().getChildFile("seed.uta");
    if (ProjectManager::save(f, p.state(&engine.getMasterChain())))
        out.push_back(fileBytes(f));
    engine.clearPlayback();

    // 最小の旧形式 (旧ルートタグ互換経路)
    out.push_back(toBytes(juce::String(
        "<?xml version=\"1.0\"?><TrakovaProject version=\"1.0\"><Settings bpm=\"90\"/>"
        "<Tracks><Track name=\"T\"><Lane><Clip file=\"Audio/tone.wav\" start=\"0\" duration=\"0.3\"/>"
        "</Lane></Track></Tracks></TrakovaProject>")));
    return out;
}

// ───────────────────────── WAV (自前パーサ + メタデータ除去 + 取り込み) ─────────────────────────

void importLikeApp(const juce::File& src)
{
    AudioFileImporter imp(formats());
    auto cache = workDir().getChildFile("importCache");
    cache.createDirectory();
    imp.getCacheFolderCb = [cache] { return cache; };
    // SR が極端な (壊れた) ヘッダはリサンプル出力が巨大になりうるため、取り込みは
    // 実在しうる範囲のときだけ行う (ヘッダ解析・デコード自体は全入力で行う)
    std::unique_ptr<juce::AudioFormatReader> probe(formats().createReaderFor(src));
    if (probe != nullptr && probe->sampleRate >= 8000.0 && probe->sampleRate <= 384000.0)
    {
        probe.reset();
        auto r = imp.importFile(src, kSR);
        if (r.success && r.wasResampled) r.file.deleteFile();
    }
    for (auto& f : cache.findChildFiles(juce::File::findFiles, false))
        f.deleteFile();
}

void runWav(const uint8_t* data, size_t size)
{
    auto src = workDir().getChildFile("fuzz.wav");
    writeBytes(src, data, size);

    auto info = AudioFileImporter::peekWavFormat(src);
    (void) info;
    juce::String err;
    if (AudioFileImporter::needsHighBitTranscode(src))
        AudioFileImporter::transcodeHighBitWavToFloat(src, workDir().getChildFile("t64.wav"), err);

    AudioFileImporter imp(formats());
    imp.copyStrippingMetadata(src, workDir().getChildFile("stripped.wav"), err);
    decodeLikePlayback(src);
    importLikeApp(src);
}

std::vector<Bytes> seedsWav()
{
    std::vector<Bytes> out;
    juce::WavAudioFormat wav;
    auto f = workDir().getChildFile("seed.wav");

    std::unordered_map<juce::String, juce::String> meta;   // bext / iXML / smpl / LIST-INFO を全部載せる (除去経路を通す)
    meta["bwav description"] = "desc";
    meta["bwav originator"] = "orig";
    meta["IXML_VERSION"] = "1.6";
    meta["tempo"] = "120";
    meta["NumSampleLoops"] = "1";
    meta["Loop0Start"] = "100";
    meta["Loop0End"] = "900";
    meta["IART"] = "artist";
    meta["ICMT"] = "comment";

    struct V { int ch; int bits; bool fl; bool withMeta; };
    for (auto v : { V{ 1, 16, false, false }, V{ 2, 24, false, true }, V{ 2, 32, true, false },
                    V{ 1, 8, false, false }, V{ 4, 24, false, false } })
        if (writeAudio(wav, f, tone(v.ch, 600, 440.0), v.bits, v.fl, v.withMeta ? meta : std::unordered_map<juce::String, juce::String>{}))
            out.push_back(fileBytes(f));
    out.push_back(wav64(2, 300, true));
    out.push_back(wav64(1, 300, false));
    return out;
}

// ───────────────────────── 各種音声形式のデコード (先頭 1 バイトで拡張子を選ぶ) ─────────────────────────

const char* const kDecodeExts[] = { ".wav", ".aiff", ".flac", ".ogg", ".mp3" };
constexpr int kNumDecodeExts = (int) (sizeof(kDecodeExts) / sizeof(kDecodeExts[0]));

void runDecode(const uint8_t* data, size_t size)
{
    if (size < 1) return;
    auto src = workDir().getChildFile(juce::String("fuzz") + kDecodeExts[data[0] % kNumDecodeExts]);
    writeBytes(src, data + 1, size - 1);
    decodeLikePlayback(src);
    importLikeApp(src);
}

std::vector<Bytes> seedsDecode()
{
    std::vector<Bytes> out;
    auto add = [&] (int extIdx, const juce::File& f)
    {
        if (! f.existsAsFile()) return;
        Bytes b { (uint8_t) extIdx };
        auto body = fileBytes(f);
        b.insert(b.end(), body.begin(), body.end());
        out.push_back(std::move(b));
    };
    auto buf = tone(2, 4800, 330.0);
    juce::WavAudioFormat wav;       auto fw = workDir().getChildFile("seed_d.wav");
    juce::AiffAudioFormat aiff;     auto fa = workDir().getChildFile("seed_d.aiff");
    juce::FlacAudioFormat flac;     auto ff = workDir().getChildFile("seed_d.flac");
    juce::OggVorbisAudioFormat ogg; auto fo = workDir().getChildFile("seed_d.ogg");
    auto fm = workDir().getChildFile("seed_d.mp3");
    if (writeAudio(wav, fw, buf, 24, false))   add(0, fw);
    if (writeAudio(aiff, fa, buf, 16, false))  add(1, fa);
    if (writeAudio(flac, ff, buf, 16, false))  add(2, ff);
    if (writeAudio(ogg, fo, buf, 16, false))   add(3, fo);
    if (Mp3EncoderWriter::encodeBuffer(buf, kSR, 128, fm)) add(4, fm);
    return out;
}

// ───────────────────────── MIDI (SMF) ─────────────────────────

void runMidi(const uint8_t* data, size_t size)
{
    auto f = workDir().getChildFile("fuzz.mid");
    writeBytes(f, data, size);
    auto r = MidiImporter::load(f);
    if (! r.ok) return;
    AppSettings s;
    MidiImporter::applyTempoMeterToSettings(r, s);
    // 取り込んだテンポ/拍子でルーラー計算 (ゼロ除算・無限ループが無いこと)
    for (double t : { 0.0, 1.0, 10.0, 100.0 })
    {
        int bar = 0, beat = 0;
        s.barAndBeatAtTime(t, bar, beat);
        if (! std::isfinite(s.beatsAtTime(t)) || ! std::isfinite(s.bpmAtTime(t)))
            fuzzFail("midi: tempo map gives non-finite beats/bpm");
    }
    for (auto& t : r.tracks)
        (void) t;
}

std::vector<Bytes> seedsMidi()
{
    std::vector<Bytes> out;
    for (int variant = 0; variant < 2; ++variant)
    {
        juce::MidiFile mf;
        if (variant == 0) mf.setTicksPerQuarterNote(960);
        else              mf.setSmpteTimeFormat(25, 40);
        juce::MidiMessageSequence conductor;
        conductor.addEvent(juce::MidiMessage::tempoMetaEvent(500000), 0);
        conductor.addEvent(juce::MidiMessage::timeSignatureMetaEvent(3, 4), 0);
        conductor.addEvent(juce::MidiMessage::tempoMetaEvent(400000), 1920);
        conductor.addEvent(juce::MidiMessage::textMetaEvent(6, "Chorus"), 960);
        mf.addTrack(conductor);
        juce::MidiMessageSequence t;
        t.addEvent(juce::MidiMessage::textMetaEvent(3, "Lead"), 0);
        for (int i = 0; i < 8; ++i)
        {
            t.addEvent(juce::MidiMessage::noteOn(variant == 0 ? 1 : 10, 60 + i, (juce::uint8) 90), i * 480);
            t.addEvent(juce::MidiMessage::noteOff(variant == 0 ? 1 : 10, 60 + i), i * 480 + 400);
        }
        t.addEvent(juce::MidiMessage::controllerEvent(1, 64, 127), 100);
        t.addEvent(juce::MidiMessage::pitchWheel(1, 12000), 200);
        t.updateMatchedPairs();
        mf.addTrack(t);
        juce::MemoryOutputStream m;
        mf.writeTo(m, 1);
        out.push_back(toBytes(m.getMemoryBlock()));
    }
    return out;
}

// ───────────────────────── 内蔵エフェクトの保存状態 (.uta の state="...") ─────────────────────────

const char* const kBuiltIns[] = { "utawave.eq", "utawave.comp", "utawave.deesser", "utawave.gate",
                                  "utawave.maximizer", "utawave.delay", "utawave.reverb", "utawave.kerovoice" };
constexpr int kNumBuiltIns = (int) (sizeof(kBuiltIns) / sizeof(kBuiltIns[0]));

void runBuiltInState(const uint8_t* data, size_t size)
{
    if (size < 1) return;
    auto fx = BuiltInFactory::create(kBuiltIns[data[0] % kNumBuiltIns]);
    if (fx == nullptr) return;
    fx->setStateInformation(data + 1, (int) (size - 1));
    fx->setPlayConfigDetails(2, 2, kSR, 512);
    fx->prepareToPlay(kSR, 512);
    juce::MidiBuffer midi;
    for (int block = 0; block < 6; ++block)
    {
        auto buf = tone(2, 512, 220.0 + 100.0 * block);
        fx->processBlock(buf, midi);
        requireFinite(buf, "builtin_state: effect output became NaN/Inf after loading state");
    }
    fx->releaseResources();
}

std::vector<Bytes> seedsBuiltInState()
{
    std::vector<Bytes> out;
    for (int i = 0; i < kNumBuiltIns; ++i)
        if (auto fx = BuiltInFactory::create(kBuiltIns[i]))
        {
            juce::MemoryBlock mb;
            fx->getStateInformation(mb);
            Bytes b { (uint8_t) i };
            auto body = toBytes(mb);
            b.insert(b.end(), body.begin(), body.end());
            out.push_back(std::move(b));
        }
    return out;
}

// ───────────────────────── ネットワークから受け取る JSON ─────────────────────────

void runAdsJson(const uint8_t* data, size_t size)
{
    auto ads = AdService::parseAds(asString(data, size));
    if ((int) ads.size() > AdService::kMaxAds) fuzzFail("ads_json: more than kMaxAds parsed");
    juce::Random rng(1234);
    for (const char* lang : { "ja", "en", "zh-Hans", "" })
    {
        auto sel = AdService::selectAdsForLanguage(ads, lang, AdService::kMaxAds);
        AdService::arrangeForDisplay(sel, 15, rng);
        if (sel.size() > 15) fuzzFail("ads_json: arrangeForDisplay exceeded maxCount");
    }
    // キャッシュへ書いて読み戻す (起動時はキャッシュ経由でも同じデータを読む)
    auto dir = workDir().getChildFile("adcache");
    AdService::saveCache(dir, "ja", asString(data, size), ads);
    AdService::loadCache(dir, "ja");
}

std::vector<Bytes> seedsAdsJson()
{
    return {
        toBytes(juce::String(R"({"ads":[{"id":"a1","title":"Title","body":"Body","imageUrl":"https://e.x/a.png",)"
                             R"("linkUrl":"https://e.x/","lang":"ja","pinned":true},)"
                             R"({"id":"a2","title":"B","lang":"all"},{"id":"a3","imageUrl":"https://e.x/b.png","lang":"en"}]})")),
        toBytes(juce::String(R"([{"id":"x","title":"T"}])")),
    };
}

void runVersionJson(const uint8_t* data, size_t size)
{
    auto info = UpdateChecker::parseVersionInfo(asString(data, size));
    UpdateChecker::isNewerVersion(info.version, "1.0.6");
    UpdateChecker::isNewerVersion("1.0.6", info.version);
    UpdateChecker::normaliseVersion(asString(data, juce::jmin<size_t>(size, 64)));
}

std::vector<Bytes> seedsVersionJson()
{
    return {
        toBytes(juce::String(R"({"version":"v1.2.3","url":"https://utawave.com/download"})")),
        toBytes(juce::String(R"({"version":"Utawave-v0.10.0-beta","notes":["a","b"]})")),
    };
}

const std::vector<std::string> kNumberDict = {
    "0", "-1", "1e308", "-1e308", "nan", "NaN", "inf", "-inf", "1e-320", "2147483647", "-2147483648",
    "4294967296", "99999999999999999999", "0.0000001", "", " ", "\xff\xfe", "&amp;", "&#0;", "]]>",
};
} // namespace

const std::vector<Target>& allTargets()
{
    static const std::vector<Target> targets = [] {
        auto xmlDict = kNumberDict;
        for (const char* t : { "<Track>", "</Track>", "<Lane>", "<Clip ", "<Plugin ", "<Settings ", "<Marker ",
                               "<MidiClip ", "<Event ", "<Lyrics>", "<Master>", "<Tracks>", "/>",
                               "start=\"", "duration=\"", "gain=\"", "fileOffset=\"", "slotIndex=\"",
                               "state=\"", "folderParent=\"", "bpm=\"", "numerator=\"", "file=\"../../",
                               "format=\"Utawave\"", "id=\"Utawave-EQ\"" })
            xmlDict.push_back(t);
        auto jsonDict = kNumberDict;
        for (const char* t : { "{", "}", "[", "]", ":", ",", "\"ads\"", "\"lang\"", "\"pinned\":true",
                               "\"version\"", "\"url\"", "null", "true", "\\u0000", "\\ud800" })
            jsonDict.push_back(t);
        std::vector<std::string> binDict = { "RIFF", "WAVE", "fmt ", "data", "LIST", "INFO", "bext", "iXML",
                                             "smpl", "ds64", "RF64", "FORM", "AIFF", "COMM", "SSND", "fLaC",
                                             "OggS", "ID3", "MThd", "MTrk", "\xff\xfb", "\xff\x51\x03",
                                             "\xff\x58\x04", "\xfe\xff", "\x00\x00\x00\x00", "\xff\xff\xff\xff" };
        return std::vector<Target> {
            { "uta",           ".uta project load + render + resave",            runUta,          seedsUta,          xmlDict,     true,  64 * 1024 },
            { "wav",           "WAV header parse / 64-bit transcode / metadata strip / import", runWav, seedsWav, binDict,     false, 64 * 1024 },
            { "audio_decode",  "WAV/AIFF/FLAC/Ogg/MP3 decode + import",         runDecode,       seedsDecode,       binDict,     false, 64 * 1024 },
            { "midi",          "SMF import + tempo map",                         runMidi,         seedsMidi,         binDict,     false, 16 * 1024 },
            { "builtin_state", "built-in effect setStateInformation + process", runBuiltInState, seedsBuiltInState, xmlDict,     false, 4 * 1024 },
            { "ads_json",      "ad feed JSON parse + selection + cache",        runAdsJson,      seedsAdsJson,      jsonDict,    true,  16 * 1024 },
            { "version_json",  "update-check JSON parse + version compare",     runVersionJson,  seedsVersionJson,  jsonDict,    true,  4 * 1024 },
        };
    }();
    return targets;
}
} // namespace utafuzz
