// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

#include "AppPreferences.h"

bool AppPreferences::adsCompiledIn()
{
   #if defined(UTAWAVE_ADS_ENABLED) && (UTAWAVE_ADS_ENABLED)
    return true;
   #else
    return false;   // 公開ソース / 通常ビルドの既定
   #endif
}

int AppPreferences::autoScaleDisplayWidth()
{
    // 接続中の全ディスプレイのうち最も広い論理幅を採用する。ノート内蔵 (狭い) が主で
    // 外部の大画面 (広い) がある構成でも、大画面基準で倍率を決められるようにする。
    //
    // 重要: JUCE は全プラットフォームで totalArea を現在の globalScaleFactor で割って報告する
    // (macOS: frame/masterScale、Windows: updateToLogical / masterScale)。そのため
    // setGlobalScaleFactor で拡大した後にそのまま読むと幅が縮んで見え (1920→1536)、自動判定が
    // 崩れる (適用はされるのに設定画面が 100% に戻る等)。globalScaleFactor を掛け戻して、
    // 自前スケールの有無に依らない「OS 論理幅」を復元する (OS の DPI スケールは残る)。
    const double g = juce::Desktop::getInstance().getGlobalScaleFactor();
    int widest = 0;
    for (auto& d : juce::Desktop::getInstance().getDisplays().displays)
        widest = juce::jmax(widest, d.totalArea.getWidth());
    return (int) std::round(widest * g);
}

double AppPreferences::autoUiScaleForMainDisplay()
{
    // totalArea は論理幅 (physical / display.scale)。Retina の Mac (物理 2940 / scale 2 = 1470) は
    // 1.0、Windows 100% スケールの 1920 幅モニタは 1.25 になる。しきい値は「見た目のサイズ感」基準。
    const int w = autoScaleDisplayWidth();
    if (w >= 3200) return 2.0;    // 4K を等倍表示している等 (非常に細かい)
    if (w >= 2560) return 1.5;    // 1440p クラス
    if (w >= 1920) return 1.25;   // 1080p / 1200p クラス (要望: 1920 → 125%)
    if (w >= 1600) return 1.1;
    return 1.0;                    // ~1470 の Mac 開発機など
}

juce::File AppPreferences::getStoreFile()
{
    auto root = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                    .getChildFile("Utawave");
    root.createDirectory();
    return root.getChildFile("app_prefs.xml");
}

AppPreferences AppPreferences::load()
{
    AppPreferences p;
    if (auto xml = juce::XmlDocument::parse(getStoreFile()))
    {
        p.showMidiExportMenu = xml->getBoolAttribute("showMidiExportMenu", p.showMidiExportMenu);
        p.showAds            = xml->getBoolAttribute("showAds", p.showAds);
        p.midiPagingEnabled  = xml->getBoolAttribute("midiPagingEnabled", p.midiPagingEnabled);
        p.recLatencyAutoComp = xml->getBoolAttribute("recLatencyAutoComp", p.recLatencyAutoComp);
        p.recLatencyManualMs = juce::jlimit(-300.0, 300.0,
            xml->getDoubleAttribute("recLatencyManualMs", p.recLatencyManualMs));
        p.retakeKeepsTake    = xml->getBoolAttribute("retakeKeepsTake", p.retakeKeepsTake);
        p.monitorThroughInserts = xml->getBoolAttribute("monitorThroughInserts", p.monitorThroughInserts);
        p.showTooltips       = xml->getBoolAttribute("showTooltips", p.showTooltips);
        p.enableFolderTracks = xml->getBoolAttribute("enableFolderTracks", p.enableFolderTracks);
        p.showFolderPanRev   = xml->getBoolAttribute("showFolderPanRev", p.showFolderPanRev);
        p.showExportCompleteDialog = xml->getBoolAttribute("showExportCompleteDialog", p.showExportCompleteDialog);
        p.lastProjectSampleRate = xml->getDoubleAttribute("lastProjectSampleRate", p.lastProjectSampleRate);
        p.lastProjectBitDepth   = xml->getIntAttribute("lastProjectBitDepth", p.lastProjectBitDepth);
        p.lastProjectLocation   = xml->getStringAttribute("lastProjectLocation", p.lastProjectLocation);
        p.diskStreaming      = xml->getBoolAttribute("diskStreaming", p.diskStreaming);
        p.multicoreAudio     = xml->getBoolAttribute("multicoreAudio", p.multicoreAudio);
        p.streamMirrorEnabled = xml->getBoolAttribute("streamMirrorEnabled", p.streamMirrorEnabled);
        p.streamMirrorDevice  = xml->getStringAttribute("streamMirrorDevice", p.streamMirrorDevice);
        // 範囲は LyricsView::kMinFont/kMaxFont と一致させる (UI へ依存させないため数値で持つ)
        p.lyricsFontSize     = juce::jlimit(12, 96,
            xml->getIntAttribute("lyricsFontSize", p.lyricsFontSize));
        p.uiScale            = juce::jlimit(minUiScale, maxUiScale,
            xml->getDoubleAttribute("uiScale", p.uiScale));
        p.uiScaleUserSet     = xml->getBoolAttribute("uiScaleUserSet", p.uiScaleUserSet);
    }
    return p;
}

bool AppPreferences::save() const
{
    juce::XmlElement xml("Preferences");
    xml.setAttribute("showMidiExportMenu", showMidiExportMenu);
    xml.setAttribute("showAds", showAds);
    xml.setAttribute("midiPagingEnabled", midiPagingEnabled);
    xml.setAttribute("recLatencyAutoComp", recLatencyAutoComp);
    xml.setAttribute("recLatencyManualMs", recLatencyManualMs);
    xml.setAttribute("retakeKeepsTake", retakeKeepsTake);
    xml.setAttribute("monitorThroughInserts", monitorThroughInserts);
    xml.setAttribute("showTooltips", showTooltips);
    xml.setAttribute("enableFolderTracks", enableFolderTracks);
    xml.setAttribute("showFolderPanRev", showFolderPanRev);
    xml.setAttribute("showExportCompleteDialog", showExportCompleteDialog);
    xml.setAttribute("lastProjectSampleRate", lastProjectSampleRate);
    xml.setAttribute("lastProjectBitDepth", lastProjectBitDepth);
    xml.setAttribute("lastProjectLocation", lastProjectLocation);
    xml.setAttribute("diskStreaming", diskStreaming);
    xml.setAttribute("multicoreAudio", multicoreAudio);
    xml.setAttribute("streamMirrorEnabled", streamMirrorEnabled);
    xml.setAttribute("streamMirrorDevice", streamMirrorDevice);
    xml.setAttribute("lyricsFontSize", lyricsFontSize);
    xml.setAttribute("uiScale", uiScale);
    xml.setAttribute("uiScaleUserSet", uiScaleUserSet);
    const bool ok = xml.writeTo(getStoreFile());
    if (!ok)
        DBG("AppPreferences::save failed (disk full / permission?): "
            << getStoreFile().getFullPathName());
    return ok;
}
