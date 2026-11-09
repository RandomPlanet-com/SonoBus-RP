#include "SonobusPluginProcessor.h"
#include <iostream>

int main()
{
    const auto mixMinusSelf = SonobusAudioProcessor::runHostedMixMinusSelfUnitTest();
    const auto reconnectResync = SonobusAudioProcessor::runHostedReconnectResyncUnitTest();
    const auto participantJoinLeave = SonobusAudioProcessor::runHostedParticipantJoinLeaveUnitTest();
    const auto serverAuthoritativeRestore = SonobusAudioProcessor::runHostedServerAuthoritativeRestoreUnitTest();
    const auto recordingLifecycle = SonobusAudioProcessor::runHostedRecordingLifecycleUnitTest();
    const auto rapidReconnectMixConsistency = SonobusAudioProcessor::runHostedRapidReconnectMixConsistencyUnitTest();
    const auto recordingGainParity = SonobusAudioProcessor::runHostedRecordingGainParityUnitTest();

    const bool mixMinusSelfOk = mixMinusSelf.containsIgnoreCase("OK");
    const bool reconnectResyncOk = reconnectResync.containsIgnoreCase("OK");
    const bool participantJoinLeaveOk = participantJoinLeave.containsIgnoreCase("OK");
    const bool serverAuthoritativeRestoreOk = serverAuthoritativeRestore.containsIgnoreCase("OK");
    const bool recordingLifecycleOk = recordingLifecycle.containsIgnoreCase("OK");
    const bool rapidReconnectMixConsistencyOk = rapidReconnectMixConsistency.containsIgnoreCase("OK");
    const bool recordingGainParityOk = recordingGainParity.containsIgnoreCase("OK");

    std::cout << "[HostedPathUnitTests] mix-minus-self\n" << mixMinusSelf << "\n\n";
    std::cout << "[HostedPathUnitTests] reconnect/resync\n" << reconnectResync << "\n\n";
    std::cout << "[HostedPathUnitTests] participant join/leave\n" << participantJoinLeave << "\n\n";
    std::cout << "[HostedPathUnitTests] server-authoritative restore\n" << serverAuthoritativeRestore << "\n\n";
    std::cout << "[HostedPathUnitTests] recording lifecycle\n" << recordingLifecycle << "\n\n";
    std::cout << "[HostedPathUnitTests] rapid reconnect mix consistency\n" << rapidReconnectMixConsistency << "\n\n";
    std::cout << "[HostedPathUnitTests] recording gain parity\n" << recordingGainParity << "\n";

    if (!mixMinusSelfOk || !reconnectResyncOk || !participantJoinLeaveOk || !serverAuthoritativeRestoreOk || !recordingLifecycleOk || !rapidReconnectMixConsistencyOk || !recordingGainParityOk)
    {
        std::cerr << "[HostedPathUnitTests] FAILED\n";
        return 1;
    }

    std::cout << "[HostedPathUnitTests] PASSED\n";
    return 0;
}
