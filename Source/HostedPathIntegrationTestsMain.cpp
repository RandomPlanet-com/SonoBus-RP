#include "SonobusPluginProcessor.h"
#include <iostream>

int main()
{
    const auto lifecycle = SonobusAudioProcessor::runHostedSessionLifecycleIntegrationTest();
    const bool ok = lifecycle.containsIgnoreCase("OK");

    std::cout << "[HostedPathIntegrationTests] full lifecycle\n" << lifecycle << "\n";

    if (!ok)
    {
        std::cerr << "[HostedPathIntegrationTests] FAILED\n";
        return 1;
    }

    std::cout << "[HostedPathIntegrationTests] PASSED\n";
    return 0;
}
