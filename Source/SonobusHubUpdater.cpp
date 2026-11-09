// SPDX-License-Identifier: GPLv3-or-later WITH Appstore-exception
// Copyright (C) 2026 Jason Buecker (Random Planet)
// SonobusHubUpdater: see SonobusHubUpdater.h
#include "SonobusHubUpdater.h"
#include "SonobusHubGeo.h"
#include <cstdio>
#include <iostream>

namespace sb_hub_gui {

#if JUCE_MAC || JUCE_LINUX
static juce::String runTool(const juce::StringArray& cmd, int& exitCode);
#endif

static bool buildIsNewer(const juce::String& current, const juce::String& candidate) {
    auto nums = [](const juce::String& v) {
        auto t = juce::StringArray::fromTokens(v.upToFirstOccurrenceOf("-", false, false), ".", {});
        juce::Array<int> out;
        for (int i = 0; i < 4; ++i) out.add(i < t.size() ? t[i].getIntValue() : 0);
        return out;
    };
    const auto a = nums(current), b = nums(candidate);
    for (int i = 0; i < 4; ++i) if (b[i] != a[i]) return b[i] > a[i];
    return false;
}

void HubUpdateCheck::run() {
    while (!threadShouldExit()) {
        checkOnce();
        wait(24 * 60 * 60 * 1000);
    }
}

void HubUpdateCheck::checkOnce() {
   #if JUCE_MAC
    const char* const app = "sbplus-hub";
    const char* const packageSuffix = ".zip";
   #else
    const char* const app = "sbplus-hub-linux";
    const char* const packageSuffix = ".tar.gz";
   #endif
    const juce::String me(hubBuildId());
    juce::URL u(juce::String("https://randomplanet.com/rp-update.php?app=") + app + "&v=" + juce::URL::addEscapeChars(me, true)
                + "&os=" + juce::URL::addEscapeChars(juce::SystemStats::getOperatingSystemName(), true)
               #if JUCE_ARM
                + "&arch=arm64");
               #else
                + "&arch=x86_64");
               #endif
    if (const char* test = std::getenv("SB_HUB_UPDATE_CHECK_URL"); test && test[0]) u = juce::URL(test);   // test only
   #if JUCE_LINUX
    // The Linux hub is built without an HTTPS library, so requests go through curl.
    int rc = 0;
    const auto reply = runTool({ "curl", "-fsSL", "--max-time", "15", u.toString(true) }, rc);
    if (rc != 0 || threadShouldExit()) { std::cout << "[HUB-UPDATE] could not reach the update server\n"; return; }
   #else
    std::unique_ptr<juce::InputStream> in(u.createInputStream(juce::URL::InputStreamOptions(juce::URL::ParameterHandling::inAddress).withConnectionTimeoutMs(8000)));
    if (in == nullptr || threadShouldExit()) return;
    const auto reply = in->readEntireStreamAsString();
   #endif
    auto json = juce::JSON::parse(reply);
    auto* obj = json.getDynamicObject();
    if (obj == nullptr) return;
    const juce::String latest = obj->getProperty("tag_name").toString();
    if (latest.isEmpty()) return;
    juce::String url;
    HubUpdateZip zip;
    if (auto* assets = obj->getProperty("assets").getArray()) {
        if (assets->size() > 0) if (auto* a0 = (*assets)[0].getDynamicObject()) url = a0->getProperty("url").toString();
        for (const auto& a : *assets)
            if (auto* o = a.getDynamicObject(); o != nullptr && o->getProperty("name").toString().endsWith(packageSuffix)) {
                zip.url = o->getProperty("url").toString();
                zip.sha256 = o->getProperty("sha256").toString();
                zip.size = (juce::int64) o->getProperty("size");
            }
    }
    const bool available = me.isNotEmpty() && buildIsNewer(me, latest);
    {
        const juce::ScopedLock sl(mLock);
        if (available) { mLine = "Update available: SonoBus RP Hub " + latest + "  (click to download)"; mUrl = url; mLatest = latest; mZip = zip; }
        else { mLine = "Hub " + me + " is up to date"; mUrl.clear(); mLatest.clear(); mZip = {}; }
    }
    std::cout << "[HUB-UPDATE] installed " << me << ", server latest " << latest << (available ? " -> update available" : " -> up to date") << "\n";
}

#if JUCE_MAC || JUCE_LINUX
static juce::String runTool(const juce::StringArray& cmd, int& exitCode) {
    juce::ChildProcess cp;
    if (!cp.start(cmd, juce::ChildProcess::wantStdOut | juce::ChildProcess::wantStdErr)) { exitCode = -1; return {}; }
    const auto out = cp.readAllProcessOutput();
    cp.waitForProcessToFinish(120000);
    exitCode = (int) cp.getExitCode();
    return out;
}

static juce::Result downloadAndCheck(const HubUpdateZip& zip, const juce::File& dest, juce::StringArray shaTool,
                                     const std::function<bool(const juce::String&)>& progress) {
    using R = juce::Result;
    if (!progress("Downloading...")) return R::fail("Cancelled");
   #if JUCE_LINUX
    {
        int rc = 0;
        runTool({ "curl", "-fsSL", "--max-time", "600", "-o", dest.getFullPathName(), zip.url }, rc);
        if (rc != 0) return R::fail("Download failed");
    }
   #else
    {
        int status = 0;
        auto in = juce::URL(zip.url).createInputStream(juce::URL::InputStreamOptions(juce::URL::ParameterHandling::inAddress)
                                                           .withConnectionTimeoutMs(15000).withStatusCode(&status));
        if (in == nullptr || (status != 200 && !zip.url.startsWith("file:"))) return R::fail("Download failed (" + juce::String(status) + ")");
        juce::FileOutputStream out(dest);
        if (!out.openedOk()) return R::fail("Could not write the download");
        juce::int64 total = 0;
        for (;;) {
            const auto n = out.writeFromInputStream(*in, 65536);
            if (n <= 0) break;
            total += n;
            if (!progress("Downloading... " + juce::File::descriptionOfSizeInBytes(total))) return R::fail("Cancelled");
        }
    }
   #endif
    if (zip.size > 0 && dest.getSize() != zip.size) return R::fail("Download size mismatch");
    if (zip.sha256.isNotEmpty()) {
        if (!progress("Verifying download...")) return R::fail("Cancelled");
        int rc = 0;
        shaTool.add(dest.getFullPathName());
        const auto line = runTool(shaTool, rc);
        if (rc != 0 || !line.startsWithIgnoreCase(zip.sha256.trim())) return R::fail("Download checksum mismatch");
    }
    return R::ok();
}
#endif

#if JUCE_MAC
static const char* const kTeamId = "NJXTYJNUF5";

static juce::File currentBundle() { return juce::File::getSpecialLocation(juce::File::currentApplicationFile); }

static juce::String plistValue(const juce::File& app, const juce::String& key) {
    const auto xml = juce::parseXML(app.getChildFile("Contents/Info.plist"));
    if (xml == nullptr) return {};
    if (auto* dict = xml->getChildByName("dict"))
        for (auto* k = dict->getFirstChildElement(); k != nullptr; k = k->getNextElement())
            if (k->hasTagName("key") && k->getAllSubText() == key)
                if (auto* v = k->getNextElement()) return v->getAllSubText();
    return {};
}
#endif

bool hubCanUpdateInPlace() {
#if JUCE_MAC
    const auto app = currentBundle();
    if (!app.isDirectory() || !app.hasFileExtension(".app")) return false;
    const auto path = app.getFullPathName();
    if (path.startsWith("/private/var/folders") || path.startsWith("/Volumes/")) return false;   // translocated, or still on the DMG
    return app.getParentDirectory().hasWriteAccess() && app.hasWriteAccess();
#elif JUCE_LINUX
    const auto exe = juce::File::getSpecialLocation(juce::File::currentExecutableFile);
    return exe.existsAsFile() && exe.getParentDirectory().hasWriteAccess();
#else
    return false;
#endif
}

juce::Result hubInstallUpdate(const HubUpdateZip& zip, const std::function<bool(const juce::String&)>& progress) {
    using R = juce::Result;
#if JUCE_MAC
    if (!hubCanUpdateInPlace()) return R::fail("This copy of the hub cannot update itself. Use the installer instead.");
    const auto current = currentBundle();
    const auto parent = current.getParentDirectory();
    const auto work = parent.getChildFile(".sbrp-hub-update");
    work.deleteRecursively();
    if (!work.createDirectory()) return R::fail("Could not create " + work.getFullPathName());
    struct Cleanup { juce::File dir; ~Cleanup() { dir.deleteRecursively(); } } cleanup{ work };

    const auto zipFile = work.getChildFile("update.zip");
    if (auto r = downloadAndCheck(zip, zipFile, { "/usr/bin/shasum", "-a", "256" }, progress); r.failed()) return r;

    int rc = 0;
    if (!progress("Unpacking...")) return R::fail("Cancelled");
    const auto stage = work.getChildFile("stage");
    stage.createDirectory();
    runTool({ "/usr/bin/ditto", "-x", "-k", zipFile.getFullPathName(), stage.getFullPathName() }, rc);
    if (rc != 0) return R::fail("Unpack failed");
    juce::File staged;
    for (const auto& e : stage.findChildFiles(juce::File::findDirectories, false))
        if (e.hasFileExtension(".app")) { staged = e; break; }
    if (staged == juce::File()) return R::fail("No app in the download");

    // Everything is verified before the installed copy is touched.
    if (!progress("Verifying signature...")) return R::fail("Cancelled");
    runTool({ "/usr/bin/codesign", "--verify", "--deep", "--strict", staged.getFullPathName() }, rc);
    if (rc != 0) return R::fail("Signature check failed");
    const auto info = runTool({ "/usr/bin/codesign", "-dv", "--verbose=2", staged.getFullPathName() }, rc);
    if (!info.contains(juce::String("TeamIdentifier=") + kTeamId)) return R::fail("Not signed by Random Planet");
    const auto gate = runTool({ "/usr/sbin/spctl", "-a", "-t", "exec", "-vv", staged.getFullPathName() }, rc);
    const char* allow = std::getenv("SB_UPDATE_ALLOW_UNNOTARIZED");   // test only: dev builds are signed but not notarized
    if ((rc != 0 || !gate.contains("accepted")) && !(allow && allow[0] == '1')) return R::fail("Notarization check failed");
    if (plistValue(staged, "CFBundleIdentifier") != plistValue(current, "CFBundleIdentifier")) return R::fail("This download is a different app");
    const auto newBuild = plistValue(staged, "CFBundleVersion"), curBuild = plistValue(current, "CFBundleVersion");
    if (!buildIsNewer(curBuild, newBuild)) return R::fail("The download (" + newBuild + ") is not newer than this hub (" + curBuild + ")");
    if (!progress("Installing...")) return R::fail("Cancelled");

    const auto previous = parent.getChildFile(current.getFileName() + ".previous");
    previous.deleteRecursively();
    if (!current.moveFileTo(previous)) return R::fail("Could not set the current app aside");
    if (!staged.moveFileTo(current)) {
        previous.moveFileTo(current);
        return R::fail("Could not move the new app into place. The current hub was restored.");
    }
    current.setLastModificationTime(juce::Time::getCurrentTime());
    return R::ok();
#elif JUCE_LINUX
    if (!hubCanUpdateInPlace()) return R::fail("This copy of the hub cannot update itself. Use the installer instead.");
    // Linux packages carry no signature, so the checksum from the update server is mandatory.
    if (zip.sha256.isEmpty()) return R::fail("The update has no checksum");
    const auto current = juce::File::getSpecialLocation(juce::File::currentExecutableFile);
    const auto parent = current.getParentDirectory();
    const auto work = parent.getChildFile(".sbrp-hub-update");
    work.deleteRecursively();
    if (!work.createDirectory()) return R::fail("Could not create " + work.getFullPathName());
    struct Cleanup { juce::File dir; ~Cleanup() { dir.deleteRecursively(); } } cleanup{ work };

    const auto package = work.getChildFile("update.tar.gz");
    if (auto r = downloadAndCheck(zip, package, { "/usr/bin/sha256sum" }, progress); r.failed()) return r;

    int rc = 0;
    if (!progress("Unpacking...")) return R::fail("Cancelled");
    runTool({ "/bin/tar", "-xzf", package.getFullPathName(), "-C", work.getFullPathName() }, rc);
    if (rc != 0) return R::fail("Unpack failed");
    const auto staged = work.getChildFile("sonobus-hub/sonobus-hub");
    if (!staged.existsAsFile()) return R::fail("No hub in the download");

    // The new binary must run here before the installed copy is touched.
    if (!progress("Checking the new hub...")) return R::fail("Cancelled");
    const auto version = runTool({ staged.getFullPathName(), "--version" }, rc).trim();
    if (rc != 0 || !version.startsWith("SonoBus RP Hub")) return R::fail("The new hub does not run on this system");
    const auto newBuild = version.fromLastOccurrenceOf(" ", false, false);
    const juce::String curBuild(hubBuildId());
    if (!buildIsNewer(curBuild, newBuild)) return R::fail("The download (" + newBuild + ") is not newer than this hub (" + curBuild + ")");
    if (!progress("Installing...")) return R::fail("Cancelled");

    const auto previous = parent.getChildFile(current.getFileName() + ".previous");
    previous.deleteFile();
    if (!current.copyFileTo(previous)) return R::fail("Could not keep a copy of the current hub");
    if (::rename(staged.getFullPathName().toRawUTF8(), current.getFullPathName().toRawUTF8()) != 0)
        return R::fail("Could not move the new hub into place");
    return R::ok();
#else
    juce::ignoreUnused(zip, progress);
    return R::fail("In-place update is not available on this system");
#endif
}

} // namespace sb_hub_gui
