// SPDX-License-Identifier: GPLv3-or-later WITH Appstore-exception
// Copyright (C) 2026 Jason Buecker (Random Planet)
#include "SonobusPluginProcessor.h"
#include <iostream>

int main()
{
    const auto crash = SonobusAudioProcessor::runHostedServerCrashGracefulDegradeTest();
    const auto malformed = SonobusAudioProcessor::runHostedMalformedPayloadRejectionTest();
    const auto timeoutFinalize = SonobusAudioProcessor::runHostedRecordingTimeoutFinalizeTest();

    const bool ok = crash.containsIgnoreCase("OK")
                    && malformed.containsIgnoreCase("OK")
                    && timeoutFinalize.containsIgnoreCase("OK");

    std::cout << "[HostedPathErrorHandlingTests] server crash\n" << crash << "\n\n";
    std::cout << "[HostedPathErrorHandlingTests] malformed payload\n" << malformed << "\n\n";
    std::cout << "[HostedPathErrorHandlingTests] timeout finalize\n" << timeoutFinalize << "\n";

    if (!ok)
    {
        std::cerr << "[HostedPathErrorHandlingTests] FAILED\n";
        return 1;
    }

    std::cout << "[HostedPathErrorHandlingTests] PASSED\n";
    return 0;
}
