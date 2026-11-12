// SPDX-License-Identifier: GPLv3-or-later WITH Appstore-exception
// Copyright (C) 2026 Jason Buecker (Random Planet)
// SonoBus RP Hub: listing this stage at the stage directory (stages.randomplanet.com).
// A background thread checks in at the interval the directory asks for, at once after a settings change, and
// removes the listing when the operator turns listing off or the hub quits. Control path only.
#pragma once
#include "JuceHeader.h"
#include <atomic>
#include <string>

class SonobusHubServer;

class HubStageDirectory : public juce::Thread {
public:
    HubStageDirectory(SonobusHubServer& hub, std::string baseUrl, std::string hubId, std::string hubKey, juce::File dataDir);
    ~HubStageDirectory() override;
    void run() override;
    void setLeaveOnStop(bool leave) { mLeaveOnStop = leave; }   // false for a hub restart: the stage stays listed

private:
    bool post(const std::string& path, const std::string& json, int& status, std::string& reply);
    void leave();
    std::string checkinJson();

    SonobusHubServer& mHub;
    std::string mBase, mHubId, mHubKey;
    juce::File mDataDir;
    bool mListed = false;
    std::atomic<bool> mLeaveOnStop{true};
};
