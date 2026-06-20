// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

#include "ProjectManager.h"
#include "../VST/PluginChain.h"
#include "../VST/PluginManager.h"
#include "../VST/PluginChain.h"
#include "../Audio/builtin/BuiltInFactory.h"  // 内蔵エフェクトの復元 (format="Utawave")
#include "../Tracks/MidiClip.h"

namespace {
juce::String relPath(const juce::File& projectDir, const juce::File& f)
{
    if (!f.exists()) return f.getFullPathName();
    auto rel = f.getRelativePathFrom(projectDir);
    return rel.isEmpty() ? f.getFullPathName() : rel;
}

juce::File resolveFile(const juce::File& projectDir, const juce::String& path)
{
    if (juce::File::isAbsolutePath(path))
    {
        // 存在しない絶対パスを getChildFile に渡すと不正なパスに化けるため、
        // 絶対パスはそのまま返す (欠損していれば missingFiles 警告に元のパスが出る)
        return juce::File(path);
    }
    return projectDir.getChildFile(path);
}

// 破損/手編集されたプロジェクトの不正値でテンポ計算 (60/bpm 等) が壊れないよう
// ロード時に音楽的に意味のある範囲へクランプする
double sanitiseBpm(double bpm)   { return (bpm > 0.0 && bpm < 1000.0) ? bpm : 120.0; }
int    sanitiseMeter(int v)      { return juce::jlimit(1, 64, v); }

juce::String colorToHex(juce::Colour c)
{
    return juce::String::formatted("#%02x%02x%02x%02x",
        c.getAlpha(), c.getRed(), c.getGreen(), c.getBlue());
}
juce::Colour hexToColor(const juce::String& s)
{
    auto t = s.startsWithChar('#') ? s.substring(1) : s;
    juce::uint32 v = (juce::uint32)t.getHexValue64();
    if (t.length() == 6) v |= 0xff000000;
    return juce::Colour(v);
}

// 内蔵エフェクト (format="Utawave") を 1 件 chain へ復元する。knownList/formatManager に
// 依存しない (自己完結) ので PluginManager が無いヘッドレス/テスト経路でも復元できる。
// 戻り値 true = 内蔵として処理済み (呼び出し側は VST 経路に流さない) / false = 内蔵でない。
bool restoreBuiltInPlugin(const juce::XmlElement* pEl, PluginChain& chain)
{
    if (! BuiltInFactory::isBuiltInFormat(pEl->getStringAttribute("format")))
        return false;

    auto inst = BuiltInFactory::createFromIdentifierString(pEl->getStringAttribute("id"));
    if (inst == nullptr)
    {
        DBG("Built-in effect not found: " << pEl->getStringAttribute("id"));
        return true;   // 内蔵だが未知 → ここで処理済み扱い (VST 経路に誤って流さない)
    }
    const auto b64 = pEl->getStringAttribute("state");
    if (b64.isNotEmpty())
    {
        juce::MemoryBlock mb;
        if (mb.fromBase64Encoding(b64) && mb.getSize() > 0)
            inst->setStateInformation(mb.getData(), (int) mb.getSize());
    }
    const int slotIdx = pEl->getIntAttribute("slotIndex", chain.getNumPlugins());
    chain.insertPluginAt(slotIdx, std::move(inst));
    if (pEl->getIntAttribute("bypassed", 0) != 0)
        chain.setBypassed(slotIdx, true);
    return true;
}
} // namespace

bool ProjectManager::save(const juce::File& projectFile, const State& s)
{
    if (!s.trackManager || !s.appSettings) return false;
    auto projectDir = projectFile.getParentDirectory();

    auto root = std::make_unique<juce::XmlElement>("UtawaveProject");
    root->setAttribute("version", "1.0");

    // Transport
    auto* transport = root->createNewChildElement("Transport");
    transport->setAttribute("bpm", s.bpm ? *s.bpm : 120.0);
    transport->setAttribute("initialBpm", s.appSettings->initialBpm);
    transport->setAttribute("playheadSecs", s.playheadSecs ? *s.playheadSecs : 0.0);

    // View (タイムライン横ズーム等の表示状態)
    if (s.pixelsPerBeat)
    {
        auto* view = root->createNewChildElement("View");
        view->setAttribute("pixelsPerBeat", *s.pixelsPerBeat);
    }
    auto* bpmChanges = transport->createNewChildElement("BpmChanges");
    for (auto& bc : s.appSettings->bpmChanges)
    {
        auto* e = bpmChanges->createNewChildElement("BpmChange");
        e->setAttribute("timeSec", bc.timeSec);
        e->setAttribute("bpm",     bc.bpm);
    }

    // Meter
    auto* meter = root->createNewChildElement("Meter");
    meter->setAttribute("numerator",   s.appSettings->meterNumerator);
    meter->setAttribute("denominator", s.appSettings->meterDenominator);
    auto* meterChanges = meter->createNewChildElement("MeterChanges");
    for (auto& mc : s.appSettings->meterChanges)
    {
        auto* e = meterChanges->createNewChildElement("MeterChange");
        e->setAttribute("barIndex",    mc.barIndex);
        e->setAttribute("numerator",   mc.numerator);
        e->setAttribute("denominator", mc.denominator);
    }

    // Loop
    if (s.loopActive && s.loopStartSecs && s.loopEndSecs)
    {
        auto* loop = root->createNewChildElement("Loop");
        loop->setAttribute("active",    *s.loopActive ? 1 : 0);
        loop->setAttribute("startSecs", *s.loopStartSecs);
        loop->setAttribute("endSecs",   *s.loopEndSecs);
    }

    // Markers
    if (s.markers)
    {
        auto* markersEl = root->createNewChildElement("Markers");
        for (auto& m : *s.markers)
        {
            auto* e = markersEl->createNewChildElement("Marker");
            e->setAttribute("time",   m.time);
            e->setAttribute("name",   m.name);
            e->setAttribute("colour", colorToHex(m.colour));
        }
    }

    // App Settings
    auto* settings = root->createNewChildElement("Settings");
    settings->setAttribute("countInBars",       s.appSettings->countInBars);
    settings->setAttribute("preRollSecs",       s.appSettings->preRollSecs);
    settings->setAttribute("retrospectiveEnabled", s.appSettings->retrospectiveEnabled ? 1 : 0);
    settings->setAttribute("playheadFollowsSelection", s.appSettings->playheadFollowsSelection ? 1 : 0);
    settings->setAttribute("autoCrossfade",     s.appSettings->autoCrossfade ? 1 : 0);
    settings->setAttribute("zeroCrossingFade",  s.appSettings->zeroCrossingFade ? 1 : 0);
    settings->setAttribute("crossfadeDuration", s.appSettings->crossfadeDuration);
    settings->setAttribute("showClipGain",      s.appSettings->showClipGain ? 1 : 0);
    settings->setAttribute("snapMode",          (int)s.appSettings->snapMode);
    settings->setAttribute("useMarkerColors",   s.appSettings->useMarkerColors ? 1 : 0);
    settings->setAttribute("toolMode",          (int)s.appSettings->toolMode);
    settings->setAttribute("resampleOutputBits", s.appSettings->resampleOutputBits);
    settings->setAttribute("projectSampleRate", s.appSettings->projectSampleRate);
    settings->setAttribute("projectBitDepth",   s.appSettings->projectBitDepth);
    settings->setAttribute("masterInsertSlotsVisible", s.appSettings->masterInsertSlotsVisible ? 1 : 0);
    settings->setAttribute("masterPanelCollapsed",     s.appSettings->masterPanelCollapsed ? 1 : 0);
    settings->setAttribute("rulerTimeRowVisible",      s.appSettings->rulerTimeRowVisible ? 1 : 0);
    settings->setAttribute("rulerBarsRowVisible",      s.appSettings->rulerBarsRowVisible ? 1 : 0);
    settings->setAttribute("autoSaveIntervalMinutes",  s.appSettings->autoSaveIntervalMinutes);
    settings->setAttribute("maxBackups",               s.appSettings->maxBackups);
    settings->setAttribute("vuReferenceLevel",         (double) s.appSettings->vuReferenceLevel);
    settings->setAttribute("loudnessTargetLufs",       (double) s.appSettings->loudnessTargetLufs);
    settings->setAttribute("returnToStartOnStop",      s.appSettings->returnToStartOnStop ? 1 : 0);
    settings->setAttribute("autoNormalizeOnImport",    s.appSettings->autoNormalizeOnImport ? 1 : 0);
    settings->setAttribute("stripImportedMetadata",    s.appSettings->stripImportedMetadata ? 1 : 0);
    settings->setAttribute("zoomToMousePosition",      s.appSettings->zoomToMousePosition ? 1 : 0);
    settings->setAttribute("exportPeakGuard",          s.appSettings->exportPeakGuard ? 1 : 0);

    // Tracks
    auto* tracks = root->createNewChildElement("Tracks");
    for (int ti = 0; ti < s.trackManager->getTrackCount(); ++ti)
    {
        auto* tr = s.trackManager->getTrack(ti);
        auto* trackEl = tracks->createNewChildElement("Track");
        trackEl->setAttribute("name",            tr->getName());
        trackEl->setAttribute("colour",          colorToHex(tr->getColour()));
        trackEl->setAttribute("volume",          tr->getVolume());
        trackEl->setAttribute("pan",             tr->getPan());
        trackEl->setAttribute("reverbSend",      tr->getReverbSend());
        trackEl->setAttribute("muted",           tr->isMuted() ? 1 : 0);
        trackEl->setAttribute("soloed",          tr->isSoloed() ? 1 : 0);
        trackEl->setAttribute("recArmed",        tr->isRecArmed() ? 1 : 0);
        trackEl->setAttribute("inputMonitor",    tr->isInputMonitor() ? 1 : 0);
        trackEl->setAttribute("inputChannel",    tr->getInputChannel());
        trackEl->setAttribute("isStereo",        tr->isStereo() ? 1 : 0);
        trackEl->setAttribute("isClickTrack",    tr->isClickTrack() ? 1 : 0);
        trackEl->setAttribute("clickSound",      tr->getClickSound());
        trackEl->setAttribute("clickAccent",     tr->isClickAccent() ? 1 : 0);
        trackEl->setAttribute("clickRate",       tr->getClickRate());
        trackEl->setAttribute("customHeight",    tr->getCustomHeight());
        trackEl->setAttribute("customLaneHeight",tr->getLaneHeight());
        trackEl->setAttribute("lanesCollapsed",  tr->isLanesCollapsed() ? 1 : 0);
        trackEl->setAttribute("insertSlotsVisible", tr->isInsertSlotsVisible() ? 1 : 0);
        trackEl->setAttribute("isMidiTrack",     tr->isMidiTrack() ? 1 : 0);
        if (tr->isMidiTrack())
        {
            trackEl->setAttribute("synthWaveform", tr->getSynthWaveform());
            trackEl->setAttribute("synthEnabled",  tr->isSynthEnabled() ? 1 : 0);
            trackEl->setAttribute("octaveShift",       tr->getOctaveShift());
            trackEl->setAttribute("semitoneTranspose", tr->getSemitoneTranspose());

            auto* midiEl = trackEl->createNewChildElement("MidiClips");
            for (int ci = 0; ci < tr->getMidiClipCount(); ++ci)
            {
                auto* clip = tr->getMidiClip(ci);
                if (!clip) continue;
                auto* mEl = midiEl->createNewChildElement("MidiClip");
                mEl->setAttribute("startPosition", clip->getStartPosition());
                mEl->setAttribute("duration",      clip->getDuration());
                mEl->setAttribute("name",          clip->getName());
                mEl->setAttribute("colour",        colorToHex(clip->getColour()));
                mEl->setAttribute("channel",       clip->getChannel());
                // シーケンスは MIDI 1.0 のバイト列を Base64 で保存
                juce::MidiFile mf;
                mf.setTicksPerQuarterNote(960);
                juce::MidiMessageSequence copy = clip->getSequence();
                // タイムスタンプ（秒）→ ticks に変換: 120BPM 固定相当（保存内では秒→tick 変換も可だが
                // ここでは秒のままを「擬似的に 1秒=480tick として保存」のシンプル化を行わず、
                // データ可搬性のため独自 XML として時刻+ノート情報を保存）
                for (int i = 0; i < copy.getNumEvents(); ++i)
                {
                    const auto& m = copy.getEventPointer(i)->message;
                    auto* eEl = mEl->createNewChildElement("Ev");
                    eEl->setAttribute("t", m.getTimeStamp());
                    auto data = m.getRawData();
                    juce::MemoryBlock mb(data, (size_t)m.getRawDataSize());
                    eEl->setAttribute("d", mb.toBase64Encoding());
                }
            }
        }

        // プラグインチェーン
        {
            auto& chain = tr->getPluginChain();
            const int nfx = chain.getNumPlugins();
            if (nfx > 0)
            {
                auto* pluginsEl = trackEl->createNewChildElement("Plugins");
                for (int pi = 0; pi < nfx; ++pi)
                {
                    auto* plugin = chain.getPlugin(pi);
                    if (!plugin) continue;
                    auto desc = plugin->getPluginDescription();
                    auto* pEl = pluginsEl->createNewChildElement("Plugin");
                    pEl->setAttribute("slotIndex", pi);
                    pEl->setAttribute("id",        desc.createIdentifierString());
                    pEl->setAttribute("name",      desc.name);
                    pEl->setAttribute("manufacturer", desc.manufacturerName);
                    pEl->setAttribute("format",    desc.pluginFormatName);
                    pEl->setAttribute("bypassed",  chain.isBypassed(pi) ? 1 : 0);

                    juce::MemoryBlock mb;
                    plugin->getStateInformation(mb);
                    if (mb.getSize() > 0)
                        pEl->setAttribute("state", mb.toBase64Encoding());
                }
            }
        }

        auto* lanesEl = trackEl->createNewChildElement("Lanes");
        for (int li = 0; li < tr->getLaneCount(); ++li)
        {
            auto* lane = tr->getLane(li);
            auto* laneEl = lanesEl->createNewChildElement("Lane");
            laneEl->setAttribute("muted",  lane->muted  ? 1 : 0);
            laneEl->setAttribute("soloed", lane->soloed ? 1 : 0);
            for (auto& clipPtr : lane->clips)
            {
                auto* c = clipPtr.get();
                auto* clipEl = laneEl->createNewChildElement("Clip");
                clipEl->setAttribute("file",          relPath(projectDir, c->getFile()));
                clipEl->setAttribute("startPosition", c->getStartPosition());
                clipEl->setAttribute("duration",      c->getDuration());
                clipEl->setAttribute("fileOffset",    c->getFileOffset());
                clipEl->setAttribute("gain",          c->getGain());
                clipEl->setAttribute("name",          c->getName());
                clipEl->setAttribute("colour",        colorToHex(c->getColour()));
                clipEl->setAttribute("customColour",  c->hasCustomColour() ? 1 : 0);
                clipEl->setAttribute("fadeIn",        c->getFadeInSecs());
                clipEl->setAttribute("fadeOut",       c->getFadeOutSecs());
                clipEl->setAttribute("fadeInCurve",   (int)c->getFadeInCurve());
                clipEl->setAttribute("fadeOutCurve",  (int)c->getFadeOutCurve());

                if (!c->getGainPoints().empty())
                {
                    auto* gp = clipEl->createNewChildElement("GainPoints");
                    for (auto& p : c->getGainPoints())
                    {
                        auto* pe = gp->createNewChildElement("Point");
                        pe->setAttribute("time", p.time);
                        pe->setAttribute("dB",   p.dB);
                    }
                }
            }
        }
    }

    // ── マスターインサートチェーン ──
    if (s.masterChain != nullptr)
    {
        auto& chain = *s.masterChain;
        const int nfx = chain.getNumPlugins();
        if (nfx > 0)
        {
            auto* masterEl = root->createNewChildElement("Master");
            auto* pluginsEl = masterEl->createNewChildElement("Plugins");
            for (int pi = 0; pi < nfx; ++pi)
            {
                auto* plugin = chain.getPlugin(pi);
                if (!plugin) continue;
                auto desc = plugin->getPluginDescription();
                auto* pEl = pluginsEl->createNewChildElement("Plugin");
                pEl->setAttribute("slotIndex",    pi);
                pEl->setAttribute("id",           desc.createIdentifierString());
                pEl->setAttribute("name",         desc.name);
                pEl->setAttribute("manufacturer", desc.manufacturerName);
                pEl->setAttribute("format",       desc.pluginFormatName);
                pEl->setAttribute("bypassed",     chain.isBypassed(pi) ? 1 : 0);
                juce::MemoryBlock mb;
                plugin->getStateInformation(mb);
                if (mb.getSize() > 0)
                    pEl->setAttribute("state", mb.toBase64Encoding());
            }
        }
    }

    // アトミック保存: 一時ファイルに書き出してから差し替える。
    // 書き込み中にクラッシュ/電源断/ディスクフルが起きても本体ファイルは破壊されない。
    auto tmpFile = projectFile.getSiblingFile(projectFile.getFileName() + ".tmp");
    tmpFile.deleteFile();
    if (!root->writeTo(tmpFile))
    {
        DBG("ProjectManager::save: writeTo failed (disk full / permission?): "
            << tmpFile.getFullPathName());
        tmpFile.deleteFile();
        return false;
    }

    // 既存プロジェクトファイルがあれば replaceFileIn でアトミック上書き、
    // 初回保存 (まだファイルが無い) なら単純に moveFileTo で配置する。
    if (projectFile.existsAsFile())
    {
        if (!tmpFile.replaceFileIn(projectFile))
        {
            tmpFile.deleteFile();
            return false;
        }
    }
    else
    {
        if (!tmpFile.moveFileTo(projectFile))
        {
            tmpFile.deleteFile();
            return false;
        }
    }
    return true;
}

bool ProjectManager::load(const juce::File& projectFile, State& s)
{
    if (!s.trackManager || !s.appSettings) return false;
    if (!projectFile.existsAsFile()) return false;

    auto xml = juce::XmlDocument::parse(projectFile);
    // 旧名 (Trakova) 時代に保存されたプロジェクト (TrakovaProject ルート) も受理する。保存は常に UtawaveProject
    if (!xml || !(xml->hasTagName("UtawaveProject") || xml->hasTagName("TrakovaProject"))) return false;
    auto projectDir = projectFile.getParentDirectory();

    // 既存トラックを全削除
    while (s.trackManager->getTrackCount() > 0)
        s.trackManager->removeTrack(0);

    // Transport
    if (auto* transport = xml->getChildByName("Transport"))
    {
        if (s.bpm) *s.bpm = sanitiseBpm(transport->getDoubleAttribute("bpm", 120.0));
        s.appSettings->initialBpm = sanitiseBpm(transport->getDoubleAttribute("initialBpm", 120.0));
        if (s.playheadSecs) *s.playheadSecs = transport->getDoubleAttribute("playheadSecs", 0.0);
        s.appSettings->bpmChanges.clear();
        if (auto* bpmCh = transport->getChildByName("BpmChanges"))
        {
            for (auto* e : bpmCh->getChildIterator())
            {
                BpmChange bc;
                bc.timeSec = e->getDoubleAttribute("timeSec", 0.0);
                bc.bpm     = sanitiseBpm(e->getDoubleAttribute("bpm", 120.0));
                s.appSettings->bpmChanges.push_back(bc);
            }
        }
    }

    // View (タイムライン横ズーム)
    if (s.pixelsPerBeat)
    {
        if (auto* view = xml->getChildByName("View"))
            *s.pixelsPerBeat = view->getDoubleAttribute("pixelsPerBeat", 80.0);
        else
            *s.pixelsPerBeat = 80.0;  // 古いプロジェクトは既定値で開く
    }

    // Meter
    if (auto* meter = xml->getChildByName("Meter"))
    {
        s.appSettings->meterNumerator   = sanitiseMeter(meter->getIntAttribute("numerator",   4));
        s.appSettings->meterDenominator = sanitiseMeter(meter->getIntAttribute("denominator", 4));
        s.appSettings->meterChanges.clear();
        if (auto* mch = meter->getChildByName("MeterChanges"))
        {
            for (auto* e : mch->getChildIterator())
            {
                MeterChange mc;
                mc.barIndex    = e->getIntAttribute("barIndex",    0);
                mc.numerator   = sanitiseMeter(e->getIntAttribute("numerator",   4));
                mc.denominator = sanitiseMeter(e->getIntAttribute("denominator", 4));
                s.appSettings->meterChanges.push_back(mc);
            }
        }
    }

    // Loop
    if (auto* loop = xml->getChildByName("Loop"))
    {
        if (s.loopStartSecs) *s.loopStartSecs = loop->getDoubleAttribute("startSecs", 0.0);
        if (s.loopEndSecs)   *s.loopEndSecs   = loop->getDoubleAttribute("endSecs",   0.0);
        if (s.loopActive)    *s.loopActive    = loop->getIntAttribute("active", 0) != 0;
    }

    // Markers
    if (s.markers)
    {
        s.markers->clear();
        if (auto* markersEl = xml->getChildByName("Markers"))
        {
            for (auto* e : markersEl->getChildIterator())
            {
                Marker m;
                m.time   = e->getDoubleAttribute("time", 0.0);
                m.name   = e->getStringAttribute("name");
                m.colour = hexToColor(e->getStringAttribute("colour"));
                s.markers->push_back(m);
            }
        }
    }

    // Settings
    if (auto* settings = xml->getChildByName("Settings"))
    {
        // フォールバック値は AppSettings 構造体の初期値と一致させる。
        // (XML に属性が無い古い/未保存プロジェクトを開いた時に
        //  「新規作成 → 設定変更 → 保存 → 開き直し」で挙動が変わるのを防ぐ)
        const AppSettings def {};
        s.appSettings->countInBars              = settings->getIntAttribute("countInBars",       def.countInBars);
        s.appSettings->preRollSecs              = settings->getDoubleAttribute("preRollSecs",    def.preRollSecs);
        s.appSettings->retrospectiveEnabled     = settings->getIntAttribute("retrospectiveEnabled", def.retrospectiveEnabled ? 1 : 0) != 0;
        s.appSettings->playheadFollowsSelection = settings->getIntAttribute("playheadFollowsSelection", def.playheadFollowsSelection ? 1 : 0) != 0;
        s.appSettings->autoCrossfade            = settings->getIntAttribute("autoCrossfade",     def.autoCrossfade ? 1 : 0) != 0;
        s.appSettings->zeroCrossingFade         = settings->getIntAttribute("zeroCrossingFade",  def.zeroCrossingFade ? 1 : 0) != 0;
        s.appSettings->crossfadeDuration        = settings->getDoubleAttribute("crossfadeDuration", def.crossfadeDuration);
        s.appSettings->showClipGain             = settings->getIntAttribute("showClipGain",      def.showClipGain ? 1 : 0) != 0;
        s.appSettings->snapMode                 = (SnapMode)settings->getIntAttribute("snapMode", (int) def.snapMode);
        s.appSettings->useMarkerColors          = settings->getIntAttribute("useMarkerColors",   def.useMarkerColors ? 1 : 0) != 0;
        s.appSettings->toolMode                 = (ToolMode)settings->getIntAttribute("toolMode", (int) def.toolMode);
        s.appSettings->resampleOutputBits       = settings->getIntAttribute("resampleOutputBits", def.resampleOutputBits);
        s.appSettings->projectSampleRate        = settings->getDoubleAttribute("projectSampleRate", def.projectSampleRate);
        s.appSettings->projectBitDepth          = settings->getIntAttribute("projectBitDepth", def.projectBitDepth);
        s.appSettings->masterInsertSlotsVisible = settings->getIntAttribute("masterInsertSlotsVisible", def.masterInsertSlotsVisible ? 1 : 0) != 0;
        s.appSettings->masterPanelCollapsed     = settings->getIntAttribute("masterPanelCollapsed", def.masterPanelCollapsed ? 1 : 0) != 0;
        s.appSettings->rulerTimeRowVisible      = settings->getIntAttribute("rulerTimeRowVisible", def.rulerTimeRowVisible ? 1 : 0) != 0;
        s.appSettings->rulerBarsRowVisible      = settings->getIntAttribute("rulerBarsRowVisible", def.rulerBarsRowVisible ? 1 : 0) != 0;
        s.appSettings->autoSaveIntervalMinutes  = settings->getIntAttribute("autoSaveIntervalMinutes", def.autoSaveIntervalMinutes);
        s.appSettings->maxBackups               = settings->getIntAttribute("maxBackups", def.maxBackups);
        s.appSettings->vuReferenceLevel         = (float) settings->getDoubleAttribute("vuReferenceLevel", def.vuReferenceLevel);
        s.appSettings->loudnessTargetLufs       = (float) settings->getDoubleAttribute("loudnessTargetLufs", def.loudnessTargetLufs);
        s.appSettings->returnToStartOnStop      = settings->getIntAttribute("returnToStartOnStop", def.returnToStartOnStop ? 1 : 0) != 0;
        s.appSettings->autoNormalizeOnImport    = settings->getIntAttribute("autoNormalizeOnImport", def.autoNormalizeOnImport ? 1 : 0) != 0;
        s.appSettings->stripImportedMetadata    = settings->getIntAttribute("stripImportedMetadata", def.stripImportedMetadata ? 1 : 0) != 0;
        s.appSettings->zoomToMousePosition      = settings->getIntAttribute("zoomToMousePosition", def.zoomToMousePosition ? 1 : 0) != 0;
        s.appSettings->exportPeakGuard          = settings->getIntAttribute("exportPeakGuard", def.exportPeakGuard ? 1 : 0) != 0;
    }

    // Tracks
    if (auto* tracks = xml->getChildByName("Tracks"))
    {
        for (auto* trackEl : tracks->getChildIterator())
        {
            const bool isClick = trackEl->getIntAttribute("isClickTrack", 0) != 0;
            const bool isStereo = trackEl->getIntAttribute("isStereo", 0) != 0;

            Track* tr = nullptr;
            if (isClick) tr = s.trackManager->addClickTrack();
            else         tr = s.trackManager->addTrack(trackEl->getStringAttribute("name"), isStereo);
            if (!tr) continue;

            tr->setName(trackEl->getStringAttribute("name", "Track"));
            tr->setColour(hexToColor(trackEl->getStringAttribute("colour")));
            tr->setVolume((float)trackEl->getDoubleAttribute("volume", 0.0));
            tr->setPan   ((float)trackEl->getDoubleAttribute("pan", 0.0));
            tr->setReverbSend((float)trackEl->getDoubleAttribute("reverbSend", 0.0));
            tr->setMuted   (trackEl->getIntAttribute("muted",        0) != 0);
            tr->setSoloed  (trackEl->getIntAttribute("soloed",       0) != 0);
            tr->setRecArmed(trackEl->getIntAttribute("recArmed",     0) != 0);
            tr->setInputMonitor(trackEl->getIntAttribute("inputMonitor", 0) != 0);
            tr->setInputChannel(trackEl->getIntAttribute("inputChannel", 0));
            tr->setStereo  (isStereo);
            if (isClick)
            {
                tr->setClickSound(trackEl->getIntAttribute("clickSound", 0));
                tr->setClickAccent(trackEl->getIntAttribute("clickAccent", 1) != 0);
                tr->setClickRate(trackEl->getIntAttribute("clickRate", 0));
            }
            tr->setCustomHeight(trackEl->getIntAttribute("customHeight",     Track::defaultHeight));
            tr->setCustomLaneHeight(trackEl->getIntAttribute("customLaneHeight", Track::laneHeight));
            tr->setLanesCollapsed(trackEl->getIntAttribute("lanesCollapsed", 1) != 0);
            tr->setInsertSlotsVisible(trackEl->getIntAttribute("insertSlotsVisible", 0) != 0);
            tr->setMidiTrack(trackEl->getIntAttribute("isMidiTrack", 0) != 0);
            if (tr->isMidiTrack())
            {
                tr->setSynthWaveform(trackEl->getIntAttribute("synthWaveform", 1));   // 既定: Saw
                tr->setSynthEnabled (trackEl->getIntAttribute("synthEnabled", 1) != 0);
                tr->setOctaveShift       (trackEl->getIntAttribute("octaveShift", 0));
                tr->setSemitoneTranspose (trackEl->getIntAttribute("semitoneTranspose", 0));

                if (auto* midiEl = trackEl->getChildByName("MidiClips"))
                {
                    for (auto* mEl : midiEl->getChildIterator())
                    {
                        if (!mEl->hasTagName("MidiClip")) continue;
                        const double startP = mEl->getDoubleAttribute("startPosition", 0.0);
                        const double dur    = mEl->getDoubleAttribute("duration", 0.0);
                        auto* clip = tr->addMidiClip(startP, dur);
                        if (!clip) continue;
                        clip->setName(mEl->getStringAttribute("name"));
                        clip->setColour(hexToColor(mEl->getStringAttribute("colour")));
                        clip->setChannel(mEl->getIntAttribute("channel", 0));

                        auto& seq = clip->getSequence();
                        for (auto* eEl : mEl->getChildIterator())
                        {
                            if (!eEl->hasTagName("Ev")) continue;
                            const double t = eEl->getDoubleAttribute("t", 0.0);
                            const auto b64 = eEl->getStringAttribute("d");
                            juce::MemoryBlock mb;
                            if (mb.fromBase64Encoding(b64) && mb.getSize() > 0)
                            {
                                juce::MidiMessage msg((const juce::uint8*)mb.getData(),
                                                       (int)mb.getSize(), t);
                                seq.addEvent(msg);
                            }
                            else if (b64.isNotEmpty())
                            {
                                // 破損データの黙殺はデバッグ困難なので痕跡を残す
                                DBG("ProjectManager::load: corrupt MIDI event data skipped (t="
                                    << t << ")");
                            }
                        }
                        seq.updateMatchedPairs();
                    }
                }
            }

            // プラグインチェーンの復元。内蔵エフェクトは自己完結なので PluginManager 不要、
            // VST3 のみ PluginManager (knownList/formatManager) を要する。
            if (auto* pluginsEl = trackEl->getChildByName("Plugins"))
            {
                const bool havePM = (s.pluginManager != nullptr);
                // KnownPluginList::getTypes() は値で返るため、一度ローカルにコピーしてから検索する。
                // 以前は一時 Array の要素アドレスを保持 → ループ終了で dangling pointer になり VST3 ロード時にクラッシュしていた。
                const juce::Array<juce::PluginDescription> types =
                    havePM ? s.pluginManager->getKnownPluginListRW().getTypes()
                           : juce::Array<juce::PluginDescription>();

                for (auto* pEl : pluginsEl->getChildIterator())
                {
                    if (!pEl->hasTagName("Plugin")) continue;

                    // 内蔵エフェクトは PluginManager に依らず復元する
                    if (restoreBuiltInPlugin(pEl, tr->getPluginChain()))
                        continue;

                    if (!havePM) continue;   // VST3 は PluginManager 必須

                    auto& fmgr      = s.pluginManager->getFormatManager();
                    const auto idStr = pEl->getStringAttribute("id");

                    {
                        juce::PluginDescription matchDesc;
                        bool found = false;
                        for (const auto& d : types)
                            if (d.createIdentifierString() == idStr) { matchDesc = d; found = true; break; }
                        // ID 完全一致が無ければ名前で再検索
                        if (!found)
                        {
                            const auto wantedName = pEl->getStringAttribute("name");
                            for (const auto& d : types)
                                if (d.name == wantedName) { matchDesc = d; found = true; break; }
                        }
                        if (!found)
                        {
                            DBG("Plugin not found: " << idStr);
                            continue;
                        }

                        juce::String err;
                        std::unique_ptr<juce::AudioPluginInstance> instance(
                            fmgr.createPluginInstance(matchDesc,
                                                       s.pluginSampleRate,
                                                       s.pluginBlockSize,
                                                       err));
                        if (!instance) { DBG("Failed to load plugin: " << err); continue; }

                        // 状態の復元
                        const auto b64 = pEl->getStringAttribute("state");
                        if (b64.isNotEmpty())
                        {
                            juce::MemoryBlock mb;
                            if (mb.fromBase64Encoding(b64) && mb.getSize() > 0)
                                instance->setStateInformation(mb.getData(), (int)mb.getSize());
                        }

                        // 保存時のスロット位置に復元（古いファイルで未保存なら末尾）
                        const int slotIdx = pEl->getIntAttribute("slotIndex",
                                                tr->getPluginChain().getNumPlugins());
                        tr->getPluginChain().insertPluginAt(slotIdx, std::move(instance));
                        if (pEl->getIntAttribute("bypassed", 0) != 0)
                            tr->getPluginChain().setBypassed(slotIdx, true);
                    }
                }
            }

            // Lanes / Clips
            int laneIdx = 0;
            if (auto* lanesEl = trackEl->getChildByName("Lanes"))
            {
                for (auto* laneEl : lanesEl->getChildIterator())
                {
                    auto* lane = tr->ensureLane(laneIdx);
                    lane->muted  = laneEl->getIntAttribute("muted",  0) != 0;
                    lane->soloed = laneEl->getIntAttribute("soloed", 0) != 0;
                    for (auto* clipEl : laneEl->getChildIterator())
                    {
                        if (!clipEl->hasTagName("Clip")) continue;
                        const auto relPath = clipEl->getStringAttribute("file");
                        auto file = resolveFile(projectDir, relPath);
                        // 欠損ファイル: クリップは UI 表示用に作成するが、警告対象として記録する
                        // (相対パスは保ったままなので、後でファイルが復活すれば自動的に再生される)
                        if (!file.existsAsFile() && s.missingFiles != nullptr)
                            s.missingFiles->push_back(relPath);
                        double startPos = clipEl->getDoubleAttribute("startPosition", 0.0);
                        double dur      = clipEl->getDoubleAttribute("duration",      0.0);
                        auto* clip = lane->addClip(file, startPos, dur,
                                                    tr->getFormatManager(),
                                                    tr->getThumbnailCache());
                        if (!clip) continue;
                        clip->setFileOffset(clipEl->getDoubleAttribute("fileOffset", 0.0));
                        clip->setGain((float)clipEl->getDoubleAttribute("gain", 1.0));
                        clip->setName(clipEl->getStringAttribute("name", file.getFileNameWithoutExtension()));
                        if (clipEl->getIntAttribute("customColour", 0) != 0)
                            clip->setColour(hexToColor(clipEl->getStringAttribute("colour")));
                        clip->setFadeInSecs (clipEl->getDoubleAttribute("fadeIn",  0.010));
                        clip->setFadeOutSecs(clipEl->getDoubleAttribute("fadeOut", 0.010));
                        clip->setFadeInCurve ((FadeCurve)clipEl->getIntAttribute("fadeInCurve",  0));
                        clip->setFadeOutCurve((FadeCurve)clipEl->getIntAttribute("fadeOutCurve", 0));
                        if (auto* gp = clipEl->getChildByName("GainPoints"))
                        {
                            auto& pts = clip->getGainPointsRW();
                            pts.clear();
                            for (auto* pe : gp->getChildIterator())
                            {
                                GainPoint p;
                                p.time = pe->getDoubleAttribute("time", 0.0);
                                p.dB   = (float)pe->getDoubleAttribute("dB", 0.0);
                                pts.push_back(p);
                            }
                        }
                    }
                    ++laneIdx;
                }
            }
        }
    }

    // ── マスターインサートチェーンの復元 ──
    // 内蔵エフェクトは自己完結なので PluginManager 不要、VST3 のみ PluginManager を要する。
    if (s.masterChain != nullptr)
    {
        if (auto* masterEl = xml->getChildByName("Master"))
        {
            if (auto* pluginsEl = masterEl->getChildByName("Plugins"))
            {
                const bool havePM = (s.pluginManager != nullptr);
                const juce::Array<juce::PluginDescription> types =
                    havePM ? s.pluginManager->getKnownPluginListRW().getTypes()
                           : juce::Array<juce::PluginDescription>();

                for (auto* pEl : pluginsEl->getChildIterator())
                {
                    if (!pEl->hasTagName("Plugin")) continue;

                    // 内蔵エフェクトは PluginManager に依らず復元する
                    if (restoreBuiltInPlugin(pEl, *s.masterChain))
                        continue;

                    if (!havePM) continue;   // VST3 は PluginManager 必須

                    auto& fmgr      = s.pluginManager->getFormatManager();
                    const auto idStr = pEl->getStringAttribute("id");

                    juce::PluginDescription matchDesc;
                    bool found = false;
                    for (const auto& d : types)
                        if (d.createIdentifierString() == idStr) { matchDesc = d; found = true; break; }
                    if (!found)
                    {
                        const auto wantedName = pEl->getStringAttribute("name");
                        for (const auto& d : types)
                            if (d.name == wantedName) { matchDesc = d; found = true; break; }
                    }
                    if (!found)
                    {
                        DBG("Master plugin not found: " << idStr);
                        continue;
                    }

                    juce::String err;
                    std::unique_ptr<juce::AudioPluginInstance> instance(
                        fmgr.createPluginInstance(matchDesc, s.pluginSampleRate,
                                                   s.pluginBlockSize, err));
                    if (!instance) { DBG("Failed to load master plugin: " << err); continue; }

                    const auto b64 = pEl->getStringAttribute("state");
                    if (b64.isNotEmpty())
                    {
                        juce::MemoryBlock mb;
                        if (mb.fromBase64Encoding(b64) && mb.getSize() > 0)
                            instance->setStateInformation(mb.getData(), (int)mb.getSize());
                    }

                    const int slotIdx = pEl->getIntAttribute("slotIndex",
                                            s.masterChain->getNumPlugins());
                    s.masterChain->insertPluginAt(slotIdx, std::move(instance));
                    if (pEl->getIntAttribute("bypassed", 0) != 0)
                        s.masterChain->setBypassed(slotIdx, true);
                }
            }
        }
    }

    return true;
}
