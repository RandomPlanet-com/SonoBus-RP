// SPDX-License-Identifier: GPLv3-or-later WITH Appstore-exception
// Copyright (C) 2026 Jason Buecker (Random Planet)
#include "SonobusHubDirectory.h"
#include "SonobusHubServer.h"
#include <iostream>
#if JUCE_LINUX || JUCE_MAC
#include <sys/stat.h>
#endif

#ifndef SONOBUS_HUB_VERSION
#define SONOBUS_HUB_VERSION "dev"
#endif

namespace {
std::string jsonText(const std::string& s) { return juce::JSON::toString(juce::var(juce::String::fromUTF8(s.c_str())), true).toStdString(); }

// The directory may move: it can name a new address, accepted only as https on randomplanet.com.
bool acceptableForward(const juce::String& u) {
    if (!u.startsWith("https://")) return false;
    const juce::String host = u.fromFirstOccurrenceOf("https://", false, false).upToFirstOccurrenceOf("/", false, false).toLowerCase();
    return host == "randomplanet.com" || host.endsWith(".randomplanet.com");
}

#if JUCE_LINUX || JUCE_MAC
juce::String runCurl(const juce::StringArray& cmd, int& exitCode) {
    juce::ChildProcess cp;
    if (!cp.start(cmd, juce::ChildProcess::wantStdOut)) { exitCode = -1; return {}; }
    const auto out = cp.readAllProcessOutput();
    cp.waitForProcessToFinish(30000);
    exitCode = (int) cp.getExitCode();
    return out;
}
#endif
}

HubStageDirectory::HubStageDirectory(SonobusHubServer& hub, std::string baseUrl, std::string hubId, std::string hubKey, juce::File dataDir)
    : juce::Thread("stage directory"), mHub(hub), mBase(std::move(baseUrl)), mHubId(std::move(hubId)), mHubKey(std::move(hubKey)), mDataDir(dataDir) {
    if (!mBase.empty() && mBase.back() != '/') mBase += '/';
    // A forward received earlier wins over the built-in address.
    const auto saved = mDataDir.getChildFile("stage-directory.txt").loadFileAsString().trim();
    if (acceptableForward(saved)) mBase = saved.toStdString() + (saved.endsWithChar('/') ? "" : "/");
}

HubStageDirectory::~HubStageDirectory() {
    stopThread(20000);
}

bool HubStageDirectory::post(const std::string& path, const std::string& json, int& status, std::string& reply) {
    status = 0; reply.clear();
    const juce::String url = juce::String(mBase) + path;
#if JUCE_LINUX || JUCE_MAC
    // curl over IPv4, with the body in a private file so the key is not on a command line. IPv4 on purpose: the
    // directory tests the address the check-in comes from, and a hub's forwarded ports are IPv4.
    const juce::File body = mDataDir.getChildFile(".stage-post.json");
    body.replaceWithText(juce::String::fromUTF8(json.c_str()));
    ::chmod(body.getFullPathName().toRawUTF8(), 0600);
    int rc = 0;
    const juce::String out = runCurl({ "curl", "-4", "-sS", "--connect-timeout", "8", "--max-time", "15", "-H", "Content-Type: application/json",
                                       "--data-binary", "@" + body.getFullPathName(), "-w", "\n%{http_code}", url }, rc);
    body.deleteFile();
    if (rc != 0) return false;
    status = out.fromLastOccurrenceOf("\n", false, false).trim().getIntValue();
    reply = out.upToLastOccurrenceOf("\n", false, false).toStdString();
    return status > 0;
#else
    juce::URL u = juce::URL(url).withPOSTData(juce::String::fromUTF8(json.c_str()));
    int code = 0;
    std::unique_ptr<juce::InputStream> in(u.createInputStream(juce::URL::InputStreamOptions(juce::URL::ParameterHandling::inPostData)
                                                                  .withExtraHeaders("Content-Type: application/json")
                                                                  .withConnectionTimeoutMs(10000)
                                                                  .withStatusCode(&code)));
    if (in == nullptr) return false;
    reply = in->readEntireStreamAsString().toStdString();
    status = code;
    return status > 0;
#endif
}

std::string HubStageDirectory::checkinJson() {
    const auto st = mHub.stageSettings();
    const auto snap = mHub.snapshotForUI();
    const bool locked = !snap.groupPassword.empty();
    const bool showNames = st.names == 1 || (st.names == 0 && !locked);
    std::string players, instruments;   // always sent; names_public says whether the public list may show them
    for (const auto& p : snap.peers) {
        players += (players.empty() ? "" : ",") + jsonText(juce::String::fromUTF8(p.userName.c_str()).substring(0, 30).toStdString());
        instruments += (instruments.empty() ? "" : ",") + jsonText(p.clientInstrument);
    }
#if JUCE_MAC
    const char* os = "Mac";
#elif JUCE_WINDOWS
    const char* os = "Windows";
#else
    const char* os = "Linux";
#endif
    std::string j = "{";
    j += "\"hub_id\":" + jsonText(mHubId) + ",\"hub_key\":" + jsonText(mHubKey);
    j += ",\"name\":" + jsonText(snap.groupName) + ",\"genre\":" + jsonText(st.genre) + ",\"description\":" + jsonText(st.description);
    j += ",\"location\":" + jsonText(st.city);
    if (!st.country.empty()) j += ",\"country\":" + jsonText(st.country);
    if (!snap.publicAddressHost.empty()) j += ",\"host\":" + jsonText(snap.publicAddressHost);   // a name the operator set; the directory checks it
    j += ",\"port\":" + std::to_string(snap.aooServerPort > 0 ? snap.aooServerPort : 12999);
    j += ",\"audio_port\":" + std::to_string(snap.publicAddressPort > 0 ? snap.publicAddressPort : (snap.localUdpPort > 0 ? snap.localUdpPort : 13000));
    j += std::string(",\"locked\":") + (locked ? "true" : "false");
    j += ",\"players\":[" + players + "],\"instruments\":[" + instruments + "],\"count\":" + std::to_string((int) snap.peers.size());
    j += std::string(",\"names_public\":") + (showNames ? "true" : "false");
    j += ",\"max_players\":" + std::to_string(st.maxPlayers) + ",\"max_channels\":" + std::to_string(st.maxChannels);
    j += ",\"version\":" + jsonText(SONOBUS_HUB_VERSION) + ",\"os\":" + jsonText(os);
    j += "}";
    return j;
}

void HubStageDirectory::leave() {
    if (!mListed) return;
    int status = 0; std::string reply;
    post("hub/leave", "{\"hub_id\":" + jsonText(mHubId) + ",\"hub_key\":" + jsonText(mHubKey) + "}", status, reply);
    mListed = false;
    std::cout << "[HUB-STAGES] listing removed\n";
}

void HubStageDirectory::run() {
    uint64_t seenGen = (uint64_t) -1;
    uint64_t seenMembers = mHub.stageMembersGeneration();
    int intervalS = 60;
    int64_t nextAt = 0;
    while (!threadShouldExit()) {
        const auto st = mHub.stageSettings();
        const uint64_t gen = mHub.stageSettingsGeneration();
        const int64_t now = juce::Time::currentTimeMillis();
        if (!st.list) {
            if (mListed) leave();
            seenGen = gen;
            wait(1000);
            continue;
        }
        // A join or leave shows in the directory within a few seconds; a burst of them is one check-in.
        if (const uint64_t members = mHub.stageMembersGeneration(); members != seenMembers) {
            seenMembers = members;
            nextAt = juce::jmin(nextAt, now + 3000);
        }
        if (gen != seenGen || now >= nextAt) {
            seenGen = gen;
            int status = 0; std::string reply;
            const bool reached = post("hub/checkin", checkinJson(), status, reply);
            auto v = juce::JSON::parse(juce::String::fromUTF8(reply.c_str()));
            auto* o = v.getDynamicObject();
            if (!reached || o == nullptr) {
                mHub.setStageListingStatus(3, "Directory not reachable" + (status > 0 ? " (HTTP " + std::to_string(status) + ")" : std::string()));
                intervalS = 60;
            } else {
                const bool listed = (bool) o->getProperty("listed");
                const std::string reason = o->getProperty("reason").toString().toStdString();
                const juce::String host = juce::String(mBase).fromFirstOccurrenceOf("://", false, false).upToFirstOccurrenceOf("/", false, false);
                if (listed) mHub.setStageListingStatus(1, "Listed at " + host.toStdString());
                else if (reason == "waiting for approval") mHub.setStageListingStatus(2, "Waiting for approval at " + host.toStdString());
                else mHub.setStageListingStatus(2, "Not listed: " + (reason.empty() ? std::string("refused") : reason));
                if (listed != mListed) std::cout << "[HUB-STAGES] " << (listed ? "listed" : "not listed: " + reason) << "\n";
                mListed = listed;
                const int next = (int) o->getProperty("next_checkin_s");
                intervalS = juce::jlimit(15, 600, next > 0 ? next : 60);
                const juce::String fwd = o->getProperty("forward").toString();
                if (fwd.isNotEmpty() && acceptableForward(fwd) && fwd.toStdString() != mBase) {
                    std::cout << "[HUB-STAGES] directory moved to " << fwd << "\n";
                    mDataDir.getChildFile("stage-directory.txt").replaceWithText(fwd);
                    mBase = fwd.toStdString() + (fwd.endsWithChar('/') ? "" : "/");
                }
            }
            nextAt = juce::Time::currentTimeMillis() + (int64_t) intervalS * 1000;
        }
        wait(1000);
    }
    if (mLeaveOnStop.load()) leave();
}
