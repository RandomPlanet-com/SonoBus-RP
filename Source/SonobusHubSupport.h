// SPDX-License-Identifier: GPLv3-or-later WITH Appstore-exception
// Copyright (C) 2026 Jason Buecker (Random Planet)
// SonobusHubSupport
// One zip of this hub's logs for support: its log, the logs of its other stages (main hub), its crash reports
// (Mac) and a short about.txt. No settings files; secrets in log text are masked. Never called from an audio thread.
#pragma once
#include <atomic>
#include <string>

#include "JuceHeader.h"

namespace hub_support {

struct Info {
    std::string stageName;
    juce::File logFile;                        // this hub's log; its rotated copy sits beside it
    int serverPort = 0, udpPort = 0, webPort = 0;
    long long uptimeSec = -1;                  // -1 = not known
    const std::atomic<bool>* cancel = nullptr; // set to true to end the work early
};

// One log line with its secrets replaced by ********: the p= value of a stage link, the value of any
// "...password..." field, and the value after a --...password option.
std::string maskSecrets(const std::string& line);

// SonoBus-Hub-logs-<stage>-<YYYYMMDD-HHMM>.zip
juce::String zipFileName(const std::string& stageName, juce::Time now = juce::Time::getCurrentTime());

// Writes the zip to `out`. False with a plain-words reason in `err` when nothing could be written.
bool writeLogsZip(const Info& info, juce::OutputStream& out, std::string& err);

} // namespace hub_support
