// SPDX-License-Identifier: GPLv3-or-later WITH Appstore-exception
// Copyright (C) 2020 Jesse Chappell
// Modifications Copyright (C) 2026 Jason Buecker (Random Planet)


#include "JuceHeader.h"
#include "SbDataPaths.h"
#include "VersionInfo.h"
#include <cstdlib>
#if SONOBUSPLUS_BUILD
#include "SonobusPlusVersion.h"
#endif

std::unique_ptr<VersionInfo> VersionInfo::fetchFromUpdateServer (const String& versionString)
{
    return fetch ("tags/" + versionString);
}

std::unique_ptr<VersionInfo> VersionInfo::fetchLatestFromUpdateServer()
{
    return fetch ("latest");
}

std::unique_ptr<InputStream> VersionInfo::createInputStreamForAsset (const Asset& asset, int& statusCode)
{
    URL downloadUrl (asset.url);
    StringPairArray responseHeaders;
    String extraHeaders;   
    extraHeaders += "Accept: application/octet-stream\r\n";
    
    String authuserpass = SystemStats::getEnvironmentVariable("GITUSERPASS", "");    
    if (authuserpass.isNotEmpty()) {
        extraHeaders += "Authorization: Basic " + Base64::toBase64(authuserpass) + "\r\n";
    }
    
    ;
    
    return std::unique_ptr<InputStream> (downloadUrl.createInputStream (URL::InputStreamOptions(URL::ParameterHandling::inAddress).withExtraHeaders(extraHeaders).withResponseHeaders(&responseHeaders).withStatusCode(&statusCode).withNumRedirectsToFollow(1)));
}

#if SONOBUSPLUS_BUILD
// Release versions are YYYY.MM.DD.HHMM (notarization time, Eastern); a dev build carries "-dev" after the same
// stamp. Compare the four numbers; equal stamps are not "newer" (a -dev build of the same stamp stays put).
static bool sbPlusStampNewer (const String& current, const String& candidate)
{
    auto nums = [] (const String& v) {
        auto t = StringArray::fromTokens (v.upToFirstOccurrenceOf ("-", false, false), ".", {});
        Array<int> out; for (int i = 0; i < 4; ++i) out.add (i < t.size() ? t[i].getIntValue() : 0); return out; };
    auto a = nums (current), b = nums (candidate);
    for (int i = 0; i < 4; ++i) { if (b[i] != a[i]) return b[i] > a[i]; }
    return false;
}
#endif

bool VersionInfo::isNewerVersionThanCurrent()
{
    jassert (versionString.isNotEmpty());
   #if SONOBUSPLUS_BUILD
    {
        const auto me = SonobusPlusVersion::get();
        if (me.version.isNotEmpty()) return sbPlusStampNewer (me.version, versionString);
        return versionString.containsChar ('.') && versionString.length() >= 15;   // unstamped dev build: any release is newer
    }
   #endif

    auto currentTokens = StringArray::fromTokens (ProjectInfo::versionString, ".", {});
    auto thisTokens    = StringArray::fromTokens (versionString, ".", {});

    jassert (thisTokens.size() >= 2 && thisTokens.size() >= 2);

    if (currentTokens[0].getIntValue() == thisTokens[0].getIntValue())
    {
        // check for 'b' in second token
        if (thisTokens[1].contains("b")) {            
            return currentTokens[1].compareIgnoreCase(thisTokens[1]); 
        }
        else {
            if (currentTokens[1].getIntValue() == thisTokens[1].getIntValue()) {
                if (currentTokens.size() > 2 && thisTokens.size() > 2) {
                    return currentTokens[2].getIntValue() < thisTokens[2].getIntValue();
                } else if (thisTokens.size() > 2) {
                    return true;
                } else {
                    return false;
                }
            }
            
            return currentTokens[1].getIntValue() < thisTokens[1].getIntValue();
        }
    }

    return currentTokens[0].getIntValue() < thisTokens[0].getIntValue();
}

std::unique_ptr<VersionInfo> VersionInfo::fetch (const String& endpoint)
{
   #if SONOBUSPLUS_BUILD
    // SonoBus RP updates come from the project's own server, never from upstream SonoBus. The endpoint answers in the
    // GitHub release JSON shape (tag_name / body / assets). What we send: app, our version, macOS version, CPU
    // architecture; nothing else.
    ignoreUnused (endpoint);
    const auto me = SonobusPlusVersion::get();
    const String ver = me.version.isNotEmpty() ? me.version : (me.upstreamBase + "-dev");
    // SB_UPDATE_URL (env or sb_taps.conf) points the check at a
    // TEST appcast endpoint (same JSON shape) so the self-updater can be exercised without touching the live server.
    String base ("https://randomplanet.com/rp-update.php");
    {
        String o;
        if (const char* e = std::getenv ("SB_UPDATE_URL"); e && e[0]) o = e;
        if (o.isEmpty())
        {
            String tapsPath;
            if (const char* f = std::getenv ("SB_TAPS_FILE"); f && f[0]) tapsPath = f;
            else tapsPath = sbTapsPath();
            if (tapsPath.isNotEmpty())
            {
                StringArray lines; File (tapsPath).readLines (lines);
                for (auto& l : lines) if (l.startsWith ("SB_UPDATE_URL=")) o = l.fromFirstOccurrenceOf ("=", false, false).trim();
            }
        }
        if (o.isNotEmpty()) base = o;
    }
    URL latestVersionURL (base
                          + (base.containsChar ('?') ? "&" : "?")
                         #if JUCE_LINUX
                          + "app=sbplus-client-linux&v="   // Linux players get the Linux package, not the Mac one
                         #else
                          + "app=sbplus-client&v="
                         #endif
                          + URL::addEscapeChars (ver, true)
                          + "&os=" + URL::addEscapeChars (SystemStats::getOperatingSystemName(), true)
                         #if JUCE_ARM
                          + "&arch=arm64");    // the running slice of the universal binary
                         #else
                          + "&arch=x86_64");
                         #endif
   #else
    URL latestVersionURL ("https://api.github.com/repos/sonosaurus/sonobus/releases/" + endpoint);
   #endif
    String extraHeaders;      
    String authuserpass = SystemStats::getEnvironmentVariable("GITUSERPASS", "");
    
    if (authuserpass.isNotEmpty()) {
        extraHeaders = "Authorization: Basic " + Base64::toBase64(authuserpass) + "\r\n";
    }

    std::unique_ptr<InputStream> inStream (latestVersionURL.createInputStream (URL::InputStreamOptions(URL::ParameterHandling::inAddress).withExtraHeaders(extraHeaders)));

    if (inStream == nullptr)
        return nullptr;

    auto content = inStream->readEntireStreamAsString();
    auto latestReleaseDetails = JSON::parse (content);

    auto* json = latestReleaseDetails.getDynamicObject();

    if (json == nullptr)
        return nullptr;

    auto versionString = json->getProperty ("tag_name").toString();

    if (versionString.isEmpty())
        return nullptr;

    auto* assets = json->getProperty ("assets").getArray();

    if (assets == nullptr)
        return nullptr;

    auto releaseNotes = json->getProperty ("body").toString();
    std::vector<VersionInfo::Asset> parsedAssets;

    for (auto& asset : *assets)
    {
        if (auto* assetJson = asset.getDynamicObject())
        {
            parsedAssets.push_back ({ assetJson->getProperty ("name").toString(),
                                      assetJson->getProperty ("url").toString(),
                                      assetJson->getProperty ("sha256").toString(),                 // Zip assets carry sha256 + size
                                      (int64) assetJson->getProperty ("size") });
            jassert (parsedAssets.back().name.isNotEmpty());
            jassert (parsedAssets.back().url.isNotEmpty());
        }
        else
        {
            jassertfalse;
        }
    }

    return std::unique_ptr<VersionInfo> (new VersionInfo ({ versionString, releaseNotes, std::move (parsedAssets) }));
}
