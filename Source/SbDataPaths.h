// SPDX-License-Identifier: GPLv3-or-later WITH Appstore-exception
// Copyright (C) 2026 Jason Buecker (Random Planet)
// Where the player keeps its files. Plain C++ so the audio library can use it too.
#pragma once
#include <cstdlib>
#include <string>

inline std::string sbHomeDir() {
    const char* home = std::getenv("HOME");
    if (!home || !home[0]) home = std::getenv("USERPROFILE");
    return home ? std::string(home) : std::string();
}

// Settings-type files such as sb_taps.conf.
inline std::string sbDataDir() {
#if defined(__APPLE__)
    return sbHomeDir() + "/Library/Application Support/SonoBus RP";
#elif defined(_WIN32)
    const char* appdata = std::getenv("APPDATA");
    return (appdata ? std::string(appdata) : sbHomeDir()) + "\\SonoBus RP";
#else
    return sbHomeDir() + "/.config/SonoBus RP";
#endif
}

inline std::string sbLogDir() {
#if defined(__APPLE__)
    return sbHomeDir() + "/Library/Logs/SonoBus RP";
#else
    return sbDataDir();
#endif
}

inline std::string sbTapsPath() { return sbDataDir() + "/sb_taps.conf"; }
inline std::string sbOldTapsPath() { return sbHomeDir() + "/SonoBus-Plus/sb_taps.conf"; }
