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
        p.recLatencyManualMs = juce::jlimit(-500.0, 500.0,
            xml->getDoubleAttribute("recLatencyManualMs", p.recLatencyManualMs));
        p.monitorThroughInserts = xml->getBoolAttribute("monitorThroughInserts", p.monitorThroughInserts);
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
    xml.setAttribute("monitorThroughInserts", monitorThroughInserts);
    const bool ok = xml.writeTo(getStoreFile());
    if (!ok)
        DBG("AppPreferences::save failed (disk full / permission?): "
            << getStoreFile().getFullPathName());
    return ok;
}
