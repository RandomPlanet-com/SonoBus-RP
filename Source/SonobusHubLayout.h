// SPDX-License-Identifier: GPLv3-or-later WITH Appstore-exception
// Channel names out of a player's /sb/clayinfo blob (a JUCE ValueTree written to a stream by
// SonobusAudioProcessor::getSendUserFormatLayoutTree: children with "name", "numchan", "chanstart"). One entry per
// channel so the hub page / Mac GUI can list "1 Vocal, 2 Guitar 1, 3 Guitar 2, …" on hover. Control path only.
#pragma once
#include <string>
#include <vector>

std::vector<std::string> hubParseChannelNames(const char* blob, size_t size);
