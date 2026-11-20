// SPDX-License-Identifier: GPLv3-or-later WITH Appstore-exception
// Copyright (C) 2020 Jesse Chappell
// Modifications Copyright (C) 2026 Jason Buecker (Random Planet)


#pragma once

void getSafeAreaInsets(void * component, float & top, float & bottom, float & left, float & right);


#if JUCE_MAC

void disableAppNap();

#if JUCE_MAC
#include <functional>
// Ask macOS for microphone access BEFORE the audio device is opened.
// A brand-new app identity has no permission entry; opening an input device while the prompt is unanswered makes
// CoreAudio stall ~60 s and fail to start IO, and the player limps on with a broken device until relaunched.
// JUCE's mac RuntimePermissions is a stub (always "granted"), hence this. The callback runs on the message thread:
// immediately when already decided, after the user's answer otherwise.
void sbRequestMicrophoneAccess(std::function<void(bool granted)> done);

#include <string>
// Latency of the audio device(s) this process runs (in a plugin: the host's), input and output, each with its buffer,
// counted like the app's figure. Returns 1 = the devices of this process or of the process responsible for it (a
// plugin hosted out of process; macOS 14+), 2 = the one running device
// that matches the host's rate and buffer, 0 = not found. Message thread only.
// trace: what was looked up and found, for the log.
int sbProcessInterfaceLatency(double hostRate, int hostBlock, float & inMs, float & outMs, std::string & deviceName, std::string & trace);
// The host application's name ("Logic Pro"), also when the plugin runs in a helper process (AUHostingService).
std::string sbHostAppName();
#endif

#endif

#if JUCE_IOS
bool urlBookmarkToBinaryData(void * bookmark, const void * & retdata, size_t & retsize);

void *binaryDataToUrlBookmark(const void * data, size_t size);

juce::URL generateUpdatedURL (juce::URL& urlToUse);

#endif
