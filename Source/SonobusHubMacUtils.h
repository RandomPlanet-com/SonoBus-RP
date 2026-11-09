// SPDX-License-Identifier: GPLv3-or-later WITH Appstore-exception
// Copyright (C) 2026 Jason Buecker (Random Planet)
// SonobusHubMacUtils — the hub's own macOS helpers (Objective-C++ in SonobusHubMacUtils.mm).
// Replaces the upstream GPL CrossPlatformUtilsMac.mm in the hub build so the hub links no GPL source.
#pragma once
#include <juce_core/system/juce_TargetPlatform.h>   // JUCE_MAC

#if JUCE_MAC
// Keep macOS from App-Napping the hub (it owns no audio device, so without this it is throttled the moment its window
// loses focus — recv thread demoted, hub sinks starve). Begins an NSProcessInfo activity
// (NSActivityUserInitiatedAllowingIdleSystemSleep | NSActivityLatencyCritical) that lives for the process.
void hubDisableAppNap();
#else
inline void hubDisableAppNap() {}
#endif
