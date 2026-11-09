// SPDX-License-Identifier: GPLv3-or-later WITH Appstore-exception
// Copyright (C) 2026 Jason Buecker (Random Planet)
// SonoBus RP release identity. Stamped into Info.plist at notarization:
//   CFBundleShortVersionString = marketing version (1.0.0, from CMake SONOBUS_PLUS_VERSION),
//   CFBundleVersion = the date build id (YYYY.MM.DD.HHMM[-dev]), SBPlusCommit = source commit,
//   SBPlusUpstreamBase = upstream SonoBus version.
// The updater compares the date build (the update server's tag_name is a date stamp). Bundles with the date in
// CFBundleShortVersionString are still understood. Finder Get Info, the Options page and About SonoBus RP read the same values.
#pragma once

#include "JuceHeader.h"

namespace SonobusPlusVersion
{
    struct Info {
        String version;       // DATE build id, e.g. 2026.10.05.1551  (empty for an unstamped dev build) — what the updater compares
        String marketing;     // e.g. 1.0.0 (CFBundleShortVersionString; compile-time SONOBUS_PLUS_VERSION when unstamped)
        String commit;        // e.g. bcf6b667
        String upstreamBase;  // e.g. 1.7.2b02
        String buildDate;     // compile date of this binary
    };

    inline Info get()
    {
        Info info;
        info.buildDate = String(__DATE__);
       #ifdef SONOBUS_PLUS_VERSION
        info.marketing = SONOBUS_PLUS_VERSION;
       #elif defined(SONOBUS_HUB_VERSION)
        info.marketing = SONOBUS_HUB_VERSION;
       #endif
        auto isStamp = [](const String& v){ return v.length() >= 15 && v.matchesWildcard("????.??.??.????*", true); };
        String shortV, bundleV;
       #if JUCE_MAC
        auto plist = File::getSpecialLocation(File::currentApplicationFile).getChildFile("Contents/Info.plist");
        if (auto xml = parseXML(plist)) {
            if (auto* dict = xml->getChildByName("dict")) {
                for (auto* e = dict->getFirstChildElement(); e != nullptr; e = e->getNextElement()) {
                    if (!e->hasTagName("key")) continue;
                    auto* val = e->getNextElement();
                    if (val == nullptr || !val->hasTagName("string")) continue;
                    const auto key = e->getAllSubText();
                    if (key == "SBPlusCommit") info.commit = val->getAllSubText();
                    else if (key == "SBPlusUpstreamBase") info.upstreamBase = val->getAllSubText();
                    else if (key == "CFBundleShortVersionString") shortV = val->getAllSubText();
                    else if (key == "CFBundleVersion") bundleV = val->getAllSubText();
                }
            }
        }
        if (isStamp(bundleV)) { info.version = bundleV; if (shortV.isNotEmpty() && !isStamp(shortV)) info.marketing = shortV; }   // new style
        else if (isStamp(shortV)) info.version = shortV;                                                                        // older bundles
        else if (shortV.isNotEmpty()) info.marketing = shortV;                                                                  // unstamped bundle
       #else
        // No Info.plist: the build id and commit arrive as compile-time defines (CMake SONOBUS_PLUS_BUILD /
        // SONOBUS_PLUS_COMMIT).
       #ifdef SONOBUS_PLUS_BUILD
        { const String b(SONOBUS_PLUS_BUILD); if (isStamp(b)) info.version = b; }
       #endif
       #ifdef SONOBUS_PLUS_COMMIT
        { const String c(SONOBUS_PLUS_COMMIT); if (c.isNotEmpty() && c != "unknown") info.commit = c; }
       #endif
       #endif
       #ifdef SONOBUS_UPSTREAM_BASE
        if (info.upstreamBase.isEmpty()) info.upstreamBase = SONOBUS_UPSTREAM_BASE;   // the SonoBus base this fork tracks (1.7.2)
       #endif
        if (info.commit.isEmpty()) {
            // Unstamped dev build: no release identity; keep the upstream base, drop any build id.
            if (info.upstreamBase.isEmpty()) info.upstreamBase = info.version.isNotEmpty() ? info.version : String(ProjectInfo::versionString);
            info.version = {};
        }
        if (info.upstreamBase.isEmpty()) info.upstreamBase = ProjectInfo::versionString;
        return info;
    }

    // One-line form for the Options page.
    inline String displayString()
    {
        auto i = get();
        const String mk = i.marketing.isNotEmpty() ? i.marketing : String("1.0");
        if (i.version.isEmpty()) return "SonoBus RP " + mk + " (dev build" + (i.commit.isNotEmpty() ? ", " + i.commit : String()) + ")";
        return "SonoBus RP " + mk + " (" + i.version + (i.commit.isNotEmpty() ? ", " + i.commit : String()) + ")";
    }
}
