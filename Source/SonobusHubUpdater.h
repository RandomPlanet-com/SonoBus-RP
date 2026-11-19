// SPDX-License-Identifier: GPLv3-or-later WITH Appstore-exception
// Copyright (C) 2026 Jason Buecker (Random Planet)
// SonobusHubUpdater: finds new hub releases and replaces this copy of the hub.
#pragma once
#include "JuceHeader.h"
#include <functional>

namespace sb_hub_gui {

struct HubUpdateZip {
    juce::String url, sha256;
    juce::int64 size = 0;
};

// What the update banners show. version = release name (e.g. 1.3.3), or the build when the server names none.
struct HubUpdateInfo {
    bool available = false;
    juce::String version, build, notesUrl, url;
};

// Asks the update server at launch and once a day after that.
class HubUpdateCheck : public juce::Thread {
public:
    HubUpdateCheck() : juce::Thread("hub-update-check") {}
    ~HubUpdateCheck() override { stopThread(5000); }
    void run() override;
    juce::String line() const { const juce::ScopedLock sl(mLock); return mLine; }
    juce::String url() const { const juce::ScopedLock sl(mLock); return mUrl; }
    juce::String latest() const { const juce::ScopedLock sl(mLock); return mLatest; }
    HubUpdateZip zip() const { const juce::ScopedLock sl(mLock); return mZip; }
    HubUpdateInfo info() const { const juce::ScopedLock sl(mLock); return mInfo; }
private:
    void checkOnce();
    juce::CriticalSection mLock;
    juce::String mLine, mUrl, mLatest;
    HubUpdateZip mZip;
    HubUpdateInfo mInfo;
};

// True when this copy can be replaced by the user it runs as.
bool hubCanUpdateInPlace();

// Downloads, verifies and swaps this copy. The running process keeps the old code until it restarts.
// progress receives a status line and returns false to cancel.
juce::Result hubInstallUpdate(const HubUpdateZip& zip, const std::function<bool(const juce::String&)>& progress);

} // namespace sb_hub_gui
