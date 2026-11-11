// SPDX-License-Identifier: GPLv3-or-later WITH Appstore-exception
// Copyright (C) 2020 Jesse Chappell
// Modifications Copyright (C) 2026 Jason Buecker (Random Planet)



#pragma once
#include <set>

#include "JuceHeader.h"
#include <thread>
#include <atomic>

#include "aoo/aoo.hpp"
#include "aoo/aoo_net.hpp"

#include <map>
#include <string>
#include <utility>

#include "MVerb.h"

#include "EffectParams.h"
#include "ChannelGroup.h"

#include "zitaRev.h"

#include "SoundboardChannelProcessor.h"

typedef MVerb<float> MVerbFloat;

namespace SonoAudio {
class Metronome;
}


#define MAX_PEERS 32
#define MAX_CHANGROUPS 64
#define DEFAULT_SERVER_PORT 10998
#define DEFAULT_HUB_SERVER_PORT 12999   // A SonoBus RP hub's signalling port when none is typed (hubs default to 12999/13000, off SonoBus's 10998-11000)
#define DEFAULT_SERVER_HOST "aoo.sonobus.net"


struct AooServerConnectionInfo
{
    AooServerConnectionInfo() {}
    
    ValueTree getValueTree() const;
    void setFromValueTree(const ValueTree & val);
    
    String userName;
    String userPassword;
    String groupName;
    String groupPassword;
    bool   groupIsPublic = false;
    String serverHost;
    int    serverPort;
    
    int64 timestamp; // milliseconds since 1970
};

struct AooPublicGroupInfo
{
    AooPublicGroupInfo() {}

    String groupName;
    int    activeCount = 0;

    int64 timestamp = 0; // milliseconds since 1970
};


struct SBChatEvent
{
    enum EventType {
        SelfType=0,
        UserType,
        SystemType
    };

    SBChatEvent() {};
    SBChatEvent(EventType type_, const String & group_,  const String & from_, const String &targets_, const String & tags_, const String & mesg_) :
    type(type_), group(group_), from(from_), targets(targets_), tags(tags_), message(mesg_)
    {}

    EventType type = UserType;
    String group;
    String from;
    String targets;
    String tags;
    String message;
};

// chat message storage, thread-safe
typedef Array<SBChatEvent, CriticalSection> SBChatEventList;



inline bool operator==(const AooServerConnectionInfo& lhs, const AooServerConnectionInfo& rhs) {
    // compare all except timestamp
     return (lhs.userName == rhs.userName
             && lhs.userPassword == rhs.userPassword
             && lhs.groupName == rhs.groupName
             && lhs.groupPassword == rhs.groupPassword
             && lhs.serverHost == rhs.serverHost
             && lhs.serverPort == rhs.serverPort
             );
}

inline bool operator!=(const AooServerConnectionInfo& lhs, const AooServerConnectionInfo& rhs){ return !(lhs == rhs); }

inline bool operator<(const AooServerConnectionInfo& lhs, const AooServerConnectionInfo& rhs)
{
    // default sorting most recent first
    return (lhs.timestamp > rhs.timestamp); 
}


//==============================================================================
/**
*/
class SonobusAudioProcessor  : public AudioProcessor, public AudioProcessorValueTreeState::Listener, public ChangeListener
{
public:
    //==============================================================================
    SonobusAudioProcessor();
    ~SonobusAudioProcessor();

    enum AutoNetBufferMode {
        AutoNetBufferModeOff = 0,
        AutoNetBufferModeAutoIncreaseOnly,
        AutoNetBufferModeAutoFull,
        AutoNetBufferModeInitAuto,
        // Measure-commit-pin ("Smart Pin"): MEASURE with the AutoFull machinery for a bounded window, COMMIT to
        // max(measured need, 8ms clean floor), then PIN: stop reacting to individual loss events; only sustained
        // underruns (>6/min over 60s) escalate one block. Must stay last: the mode persists as a plain int.
        AutoNetBufferModeMeasureCommitPin
    };
    
    enum AudioCodecFormatCodec { CodecPCM = 0, CodecOpus };

    enum ReverbModel {
        ReverbModelFreeverb = 0,
        ReverbModelMVerb,
        ReverbModelZita
    };
    
    // treated as bitmask options
    enum RecordFileOptions {
        RecordDefaultOptions = 0,
        RecordMix = 1,
        RecordSelf = 2,
        RecordMixMinusSelf = 4,
        RecordIndividualUsers = 8
    };
    
    enum RecordFileFormat {
        FileFormatDefault = 0,
        FileFormatAuto,
        FileFormatFLAC,
        FileFormatWAV,
        FileFormatOGG
    };

    enum PeerDisplayMode {
        PeerDisplayModeFull = 0,
        PeerDisplayModeMinimal
    };

    // Transport topology mode. P2P connects peers directly; HostedMix routes all audio through the hub.
    enum NetworkTopologyMode {
        NetworkTopologyP2P = 0,
        NetworkTopologyHostedMix = 1
    };

    // Hosted control transport boundary (transport-agnostic).
    enum HostedControlTransportMode {
        HostedControlTransportStub = 0,
        HostedControlTransportWebSocket = 1,
        HostedControlTransportHttp = 2
    };

    struct AudioCodecFormatInfo {
        AudioCodecFormatInfo() {}
        AudioCodecFormatInfo(int bitdepth_) : codec(CodecPCM), bitdepth(bitdepth_), min_preferred_blocksize(16)  { computeName(); }
        AudioCodecFormatInfo(int bitrate_, int complexity_, int signaltype, int minblocksize=120) :  codec(CodecOpus), bitrate(bitrate_), complexity(complexity_), signal_type(signaltype), min_preferred_blocksize(minblocksize) { computeName(); }
        void computeName();
        
        String name;
        AudioCodecFormatCodec codec;
        // PCM options
        int bitdepth = 2; // bytes
        // opus options
        int bitrate = 0;
        int complexity = 0;
        int signal_type = 0;
        int min_preferred_blocksize = 120;
    };

    struct LatInfo {
        String sourceName;
        String destName;
        float latencyMs = 0.0f; // one way latency from source->dest in ms
    };
    
    int32 getCurrSamplesPerBlock() const { return currSamplesPerBlock; }
    
    void changeListenerCallback (ChangeBroadcaster* source) override;

    static BusesProperties getDefaultLayout();

    //==============================================================================
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;

   #ifndef JucePlugin_PreferredChannelConfigurations
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
   #endif

    void processBlock (AudioBuffer<float>&, MidiBuffer&) override;

    //==============================================================================
    AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override;

    //==============================================================================
    const String getName() const override;

    bool acceptsMidi() const override;
    bool producesMidi() const override;
    bool isMidiEffect() const override;
    double getTailLengthSeconds() const override;

    //==============================================================================
    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram (int index) override;
    const String getProgramName (int index) override;
    void changeProgramName (int index, const String& newName) override;

    //bool canAddBus    (bool isInput) const override   { return (! isInput && getBusCount (false) < 10); }
    //bool canRemoveBus (bool isInput) const override   { return (! isInput && getBusCount (false) > 1); }
    
    //==============================================================================
    void getStateInformation (MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;


    void parameterChanged (const String &parameterID, float newValue) override;


    void getStateInformationWithOptions(MemoryBlock& destData, bool includecache=true, bool includeInputGroups = true, bool xmlformat=false);
    void setStateInformationWithOptions (const void* data, int sizeInBytes, bool includecache=true, bool includeInputGroups = true, bool xmlformat=false);

    bool saveCurrentAsDefaultPluginSettings();
    void resetDefaultPluginSettings();
    bool loadDefaultPluginSettings();
    
    AudioProcessorValueTreeState& getValueTreeState();

    static String paramInGain;
    static String paramInMonitorMonoPan;
    static String paramInMonitorPan1;
    static String paramInMonitorPan2;
    static String paramDry;
    static String paramWet;
    static String paramSendChannels;
    static String paramBufferTime;
    static String paramDefaultAutoNetbuf;
    static String paramDefaultNetbufMs;
    static String paramDefaultSendQual;
    static String paramMainSendMute;
    static String paramMainRecvMute;
    static String paramMetEnabled;
    static String paramMetGain;
    static String paramMetTempo;
    static String paramSendMetAudio;
    static String paramSendFileAudio;
    static String paramSendSoundboardAudio;
    static String paramHearLatencyTest;
    static String paramMetIsRecorded;
    static String paramMainReverbEnabled;
    static String paramMainReverbLevel;
    static String paramMainReverbSize;
    static String paramMainReverbDamping;
    static String paramMainReverbPreDelay;
    static String paramMainReverbModel;
    static String paramDynamicResampling;
    static String paramMainInMute;
    static String paramMainMonitorSolo;
    static String paramAutoReconnectLast;
    static String paramDefaultPeerLevel;
    static String paramSyncMetToHost;
    static String paramSyncMetToFilePlayback;
    static String paramInputReverbLevel;
    static String paramInputReverbSize;
    static String paramInputReverbDamping;
    static String paramInputReverbPreDelay;

    struct EndpointState;
    struct RemoteSink;
    struct RemoteSource;
    struct RemotePeer;

    int32_t handleSourceEvents(const aoo_event ** events, int32_t n, int32_t sourceId);
    int32_t handleSinkEvents(const aoo_event ** events, int32_t n, int32_t sinkId);
    int32_t handleServerEvents(const aoo_event ** events, int32_t n);
    int32_t handleClientEvents(const aoo_event ** events, int32_t n);

    // server stuff
    void startAooServer();
    void stopAooServer();
    
    // client stuff
    
    // if value is 0, the system will choose any available UDP port (default)
    void setUseSpecificUdpPort(int port);
    int getUseSpecificUdpPort() const { return mUseSpecificUdpPort; }
    
    bool connectToServer(const String & host, int port, const String & username, const String & passwd="");
    bool isConnectedToServer() const;
    // userRequested = the user chose to leave (not a quit, not a lost connection).
    bool disconnectFromServer(bool userRequested = true);
    // Whether the next launch should rejoin the last room: yes after a quit, crash or lost connection while in a
    // room, no after the user disconnected. Kept in a small file so it survives a crash.
    void sbMarkLaunchRejoin(bool inRoom);
    bool sbLaunchRejoinWanted() const;
    double getElapsedConnectedTime() const { return mSessionConnectionStamp > 0 ? (Time::getMillisecondCounterHiRes() - mSessionConnectionStamp) * 1e-3 : 0.0; }

    void setAutoconnectToGroupPeers(bool flag);
    bool getAutoconnectToGroupPeers() const { return mAutoconnectGroupPeers; }

    bool joinServerGroup(const String & group, const String & groupsecret = "", bool isPublic=false);
    bool leaveServerGroup(const String & group);
    String getCurrentJoinedGroup() const ;
    bool setWatchPublicGroups(bool flag);
    bool getWatchPublicGroups() const { return mWatchPublicGroups; }

    void setNetworkTopologyMode(NetworkTopologyMode mode);
    NetworkTopologyMode getNetworkTopologyMode() const { return static_cast<NetworkTopologyMode>(mNetworkTopologyMode.get()); }
    bool isHostedMixModeEnabled() const { return getNetworkTopologyMode() == NetworkTopologyHostedMix; }

    void setHubPeerUsername(const String& name) { mHubPeerUsername = name; sbApplyPeerFilter(); sbRefreshRelayedFlags(); }
    void sbApplyPeerFilter();
    void sbRefreshRelayedFlags();   // Recompute RemotePeer::relayedViaHub after a hub-name change
    void sbScheduleLaunchRetry();   // Retry the last connection every 30 s until it succeeds
    // Launch connect race: set by the app when -c/-g asks for a connect at launch; every
    // auto-reconnect-to-most-recent launch attempt bails while set. Cleared by sbLaunchConnectFailed().
    void setLaunchConnectRequested(bool flag) { mLaunchConnectRequested.store(flag); }
    bool isLaunchConnectRequested() const { return mLaunchConnectRequested.load(); }
    void sbLaunchConnectFailed(const char* why);
    // A user-initiated connect (Connect panel / recents) retries itself a few times when the aoo client refuses
    // connect() because it is still (dis)connecting, e.g. the auto-reconnect loop is mid-flight after a refused join.
    // Auto paths keep their own chains.
    bool connectToServerUser(const String & host, int port, const String & username, const String & passwd);
    int mUserConnectRetriesLeft = 0;
    std::atomic<bool> mLaunchConnectRequested { false };
    bool mLaunchRetryArmed = false;   // Hosted mode = UDP-peer only with the hub (see aoo iclient::set_peer_filter)
    String getHubPeerUsername() const { return mHubPeerUsername; }
    // The address the hub told us to share
    // (public host + AOO server port, from /hxhubhello args 3+4). Empty until
    // a hello from a hub that sends them.
    String getHubLinkHost() const { const ScopedLock sl(mHubLinkLock); return mHubLinkHost; }
    String getServerHostAsTyped() const;   // The hub address exactly as this player entered it (DNS name or IP)
    int    getServerPortAsTyped() const;   // The signaling port the player connected to (what a friend must type), not the AOO audio port
    void sbSendRecStateToHub();            // Tell the hub we started/stopped recording (it tells everyone)
    void sbSendMuteStateToHub();           // Tell the hub we muted/unmuted our send (it tells everyone)
    void sbServePortProbe(const String& hubIp, int64 token, int udpPort, int serverPort);   // Probe our hub's ports from outside on its request
    int    getHubLinkPort() const { const ScopedLock sl(mHubLinkLock); return mHubLinkPort; }
    // The hub announces its block size in the hello; device buffer sizes below it are greyed out in Setup Audio.
    // Default 32 until a hub says otherwise.
    static int  hubMinBlockSize() { return sHubBlockSize.load(std::memory_order_relaxed); }
    static int  hubSampleRateHint() { return sHubSampleRate.load(std::memory_order_relaxed); }
    static void setHubMinBlockSize(int n) { sHubBlockSize.store(n, std::memory_order_relaxed); }   // launch seed from the persisted value
    bool isHubPeer(const String& username) const { return isHostedMixModeEnabled() && username == mHubPeerUsername; }
    // In hosted mode the one upstream feed to the hub is always PCM 16-bit: codec choice is meaningless there and
    // Opus costs 5-7 ms. Returns the "PCM 16 bit" entry of the codec table (looked up, not hard-coded), or -1 if the
    // table is not built yet.
    int getHubUpstreamFormatIndex() const { return findFormatIndex(CodecPCM, 0, 2); }
    // LAN/WAN classification + Smart Pin per-path limits
    bool  sbPeerIsWan(const RemotePeer * peer) const;
    int  sbIsoLanBlocks() const;                  // SB_AOO_ISO_LAN_BLOCKS (0 = off)
    int  sbIsoWanBlocks() const;                  // SB_AOO_ISO_WAN_BLOCKS (0 = off)
    int  sbIsoBlocksFor(const RemotePeer * peer) const;
    bool sbIsoPeer(const RemotePeer * peer) const; // LAN peer under the isochronous sink
    bool sbAdaptWan() const;                       // SB_AOO_ADAPT_WAN=1 (WAN peers get the adaptive sink)
    bool sbAdaptLan() const;                       // LAN peers adapt too (default on; SB_AOO_ADAPT_LAN=0 = fixed iso)
    bool sbDynamicResamplingWanted() const;         // Hosted mode -> off unless SB_DYNRESAMP=1
    bool sbAdaptPeer(const RemotePeer * peer) const;
    // Hosted mode, unified buffer mode = Manual
    // -> the hub feed is PINNED at the slider value (iso + adaptive OFF for the sink); any Auto mode = adaptive.
    bool sbHostedManualPin() const;
    float sbHostedManualFloorMs() const;   // 2 blocks — the sink's own minimum
    float mcpFloorMsFor(const RemotePeer * peer) const;
    float mcpCeilingMsFor(const RemotePeer * peer) const;
    float mcpSanityMaxMsFor(const RemotePeer * peer) const;
    // The one floor a peer's receive buffer may never be applied below: Smart Pin floor (LAN/WAN) when the peer is in
    // Smart Pin mode, plus the hosted feed floor in HostedMix. Every set_buffersize path for a peer's sinks goes
    // through applyPeerSinkBufferMs, which clamps to this and returns the value it actually applied.
    float peerBufferFloorMs(const RemotePeer * peer) const;
    float applyPeerSinkBufferMs(RemotePeer * peer, float bufferMs, bool auxSinks = true);
    void  sbMcpEvlog(const char* ev, const RemotePeer* peer, float ms);

    // Hosted control-channel scaffolding (not on the audio path).
    bool hostedControlConnect(const String & host, int port);
    void hostedControlDisconnect();
    bool isHostedControlConnected() const { return mHostedControlConnected.get(); }
    String getHostedControlEndpoint() const;
    bool setHostedControlTransportMode(HostedControlTransportMode mode);
    HostedControlTransportMode getHostedControlTransportMode() const { return static_cast<HostedControlTransportMode>(mHostedControlTransportMode.get()); }
    bool canSwitchHostedControlTransport(String * reason = nullptr) const;
    bool canStartHostedControlConnect(String * reason = nullptr) const;
    bool canSendHostedControlCommand(String * reason = nullptr) const;
    bool hostedControlSendCommand(const String & command, const String & payloadJson = {});
    bool hostedControlSendCommandStub(const String & command, const String & payloadJson = {});
    bool hostedControlJoinRoom(const String & roomName, const String & participantName = {});
    bool hostedControlLeaveRoom(const String & roomName = {});
    bool hostedControlPublishUplinkStream(const String & participantName = {}, int channels = 2, int sampleRate = 48000);
    bool hostedControlSubscribeRoomMix(const String & roomName = {});
    bool hostedControlSetParticipantMix(const String & participantName, float gain, float pan, bool muted, bool mixMinusSelf = false);
    String getHostedControlCurrentRoomName() const;
    String getHostedControlParticipantListText() const;
    String getHostedControlTelemetryPanelText() const;
    int getHostedControlCommandStubCount() const { return mHostedControlCommandStubCount.get(); }
    String getHostedControlLastCommandStub() const;
    int getHostedControlBlockedSendCount() const { return mHostedControlBlockedSendCount; }
    int getHostedControlBlockedTransportSwitchCount() const { return mHostedControlBlockedTransportSwitchCount; }
    int getHostedControlWsQueuedSendCount() const { return mHostedControlWsQueuedSendCount; }
    int getHostedControlWsFlushBlockedEmptyCount() const { return mHostedControlWsFlushBlockedEmptyCount; }
    int getHostedControlWsFlushBlockedTransportCount() const { return mHostedControlWsFlushBlockedTransportCount; }
    int getHostedControlWsFlushBlockedDisconnectedCount() const { return mHostedControlWsFlushBlockedDisconnectedCount; }
    String getHostedControlDiagnosticsSummary() const;
    String getHostedControlDiagnosticsStructuredJson() const;
    String getHostedControlDiagnosticsStructuredJsonPretty() const;
    String getHostedControlDiagnosticsDebugCopyText() const;
    String getHostedControlDiagnosticsTriageBundleText() const;
    String getHostedControlDiagnosticsTriageBundleParityStatusText() const;
    String getHostedControlDiagnosticsTriageBundleFromStructuredJsonDebug() const;
    String getHostedControlDiagnosticsTriageBundleParityStatusFromStructuredJsonDebug() const;
    String getHostedControlConnectTimeoutTelemetryText() const;
    String getHostedControlReconnectTelemetryText() const;
    String getHostedControlReconnectWindowLabel() const;
    String getHostedControlTimingAndReconnectTelemetryBundleText() const;
    String getHostedControlTimingAndReconnectTelemetryBundleFromStructuredJsonDebug() const;
    String getHostedControlTimingTelemetryCompactLine() const;
    String getHostedWsDebugCopyGuardrailsCompactLine() const;
    String getHostedWsDebugCopyGuardrailsCompactLineFromStructuredJsonDebug() const;
    String getHostedWsDebugCopyGuardrailsActionLineFromStructuredJsonDebug() const;
    String getHostedWsDebugCopyGuardrailsCompactLineForDebugCopyAction() const;
    String getHostedWsGuardrailCopyLastEventLine() const;
    String getHostedControlDiagnosticsSummaryAndWsGuardrailsDebugCopyText() const;
    String getHostedControlQueuedCommandsJsonSnapshot() const;
    String getHostedControlQueuedPayloadsJsonSnapshot() const;
    String getHostedControlQueuedSnapshotParityFromStructuredJsonDebug() const;
    String getHostedControlQueuedSnapshotParityStatusText() const;
    String getHostedDiagnosticsStatusChipText() const;
    void setHostedDiagnosticsStatusChipText(const String & statusText);
    StringArray getHostedDiagnosticsSelfCheckRequiredKeys() const;
    std::pair<int, int> debugHostedReconnectJitterWindowForDelayMs(int baseDelayMs) const;
    String runHostedDiagnosticsSelfCheck() const;
    String runHostedDiagnosticsMigrationRoundTripSelfCheck() const;
    String runHostedDiagnosticsValueTreeMigrationSelfCheck() const;
    String runHostedReplayResyncRegressionSelfCheck() const;
    static String runHostedMixMinusSelfUnitTest();
    static String runHostedReconnectResyncUnitTest();
    static String runHostedParticipantJoinLeaveUnitTest();
    static String runHostedServerAuthoritativeRestoreUnitTest();
    static String runHostedRecordingLifecycleUnitTest();
    static String runHostedRapidReconnectMixConsistencyUnitTest();
    static String runHostedRecordingGainParityUnitTest();
    static String runHostedSessionLifecycleIntegrationTest();
    static String runHostedServerCrashGracefulDegradeTest();
    static String runHostedMalformedPayloadRejectionTest();
    static String runHostedRecordingTimeoutFinalizeTest();
    String runHostedDiagnosticsCompositeSelfCheck() const;
    String runHostedDiagnosticsCompositeSelfCheckSmoke() const;
    String getHostedDiagnosticsCompositeSmokeBundleText() const;
    String getHostedDiagnosticsCompositeSmokeBundleFromStructuredJsonDebug() const;
    void resetHostedControlDiagnosticsDebug();
    void resetHostedDiagnosticsStatusChipDebug();
    bool debugHostedControlFlushQueuedSendsScaffold();
    bool hostedControlApplyServerAckScaffold(const String & command, const String & payloadJson = {}, bool accepted = true);
    bool debugHostedControlApplyServerAckScaffold(const String & command, const String & payloadJson = {}, bool accepted = true);
    bool canFlushHostedQueuedSendsScaffold(String * reason = nullptr) const;
    bool canCopyHostedWsTimingReconnectTelemetry(String * reason = nullptr) const;
    bool canCopyHostedWsTriageBundleTelemetry(String * reason = nullptr) const;
    bool canCopyHostedWsCompositeSmokeBundleTelemetry(String * reason = nullptr) const;
    bool canCopyHostedWsQueuedSnapshotTelemetry(String * reason = nullptr) const;
    void noteHostedWsGuardrailDebugCopyOutcome(const String & actionName, bool allowed, const String & reason = {});
    bool canSimulateHostedWsConnectTimeout(String * reason = nullptr) const;
    bool debugHostedControlEvaluateWsConnectTimeoutScaffoldForElapsedMs(int elapsedMs, const String & triggerReason = "debug-sim") ;
    bool debugHostedControlInjectWsLifecycleState(int targetState, const String & error = {});
    String getHostedControlRecentEventsSummary() const;

    int getPublicGroupInfos(Array<AooPublicGroupInfo> & retarray);

    void addRecentServerConnectionInfo(const AooServerConnectionInfo & cinfo);
    void removeRecentServerConnectionInfo(int index);
    int getRecentServerConnectionInfos(Array<AooServerConnectionInfo> & retarray);
    void clearRecentServerConnectionInfos();

    bool setCurrentUsername(const String & name);
    String getCurrentUsername() const { return mCurrentUsername; }

    // peer stuff
    
    EndpointState * findOrAddEndpoint(const String & host, int port);
    EndpointState * findOrAddRawEndpoint(void * rawaddr);

    int getUdpLocalPort() const { return mUdpLocalPort; }
    IPAddress getLocalIPAddress() const { return mLocalIPAddress; }
    

    int getSendChannels() const { return mSendChannels.get(); }

    int connectRemotePeer(const String & host, int port, const String & username = "", const String & groupname = "",  bool reciprocate=true);
    bool disconnectRemotePeer(const String & host, int port, int32_t sourceId);
    bool disconnectRemotePeer(int index);
    bool removeRemotePeer(int index, bool sendblock=false);

    bool removeAllRemotePeers();
    
    int getNumberRemotePeers() const;

    void setRemotePeerLevelGain(int index, float levelgain);
    float getRemotePeerLevelGain(int index) const;

    void setRemotePeerChannelGain(int index, int changroup, float levelgain);
    float getRemotePeerChannelGain(int index, int changroup) const;

    void setRemotePeerChannelPan(int index, int changroup, int chan, float pan);
    float getRemotePeerChannelPan(int index, int changroup, int chan) const;

    void setRemotePeerChannelMuted(int index, int changroup, bool muted);
    bool getRemotePeerChannelMuted(int index, int changroup) const;

    void setRemotePeerChannelSoloed(int index, int changroup, bool soloed);
    bool getRemotePeerChannelSoloed(int index, int changroup) const;

    void setRemotePeerChannelReverbSend(int index, int changroup, float rgain);
    float getRemotePeerChannelReverbSend(int index, int changroup);

    void setRemotePeerPolarityInvert(int index, int changroup, bool invert);
    bool getRemotePeerPolarityInvert(int index, int changroup);


    bool isRemotePeerUserInGroup(const String & name) const;
    
    void setRemotePeerUserName(int index, const String & name);
    String getRemotePeerUserName(int index) const;

    // Info-only peers (HostedMix: other musicians, present for UI but no direct audio)
    bool getRemotePeerInfoOnly(int index) const;
    bool getRemotePeerHubLatency(int index, float & toHubMs, float & fromHubMs) const;
    bool getRemotePeerHubSendMuted(int index) const;   // That player muted their send (relayed by the hub)
    int64_t getRemotePeerHubUplinkLost(int index) const;   // Per-player uplink loss as seen by the hub (-1 unknown)
    bool getSelfHubLatency(float & toHubMs, float & fromHubMs, float & rttMs, int64_t & uplinkLost) const;
    int getSelfHubLatencyAgeS() const;   // Seconds since the hub's last latency report for us (-1 = never)

    // Per-listener overrides for the hub's mix-minus-self. Sends /sb/hxmute or /sb/hxgain to the hub peer; the hub
    // stores the override and applies it when computing this listener's personal mix. Both functions also cache the
    // value locally so the UI can reflect the user's choice.
    void setHubMixMuteForPeer(const String& senderUserName, bool muted);
    void setHubMixGainForPeer(const String& senderUserName, float gain);
    void setHubMixSoloForPeer(const String& senderUserName, bool soloed);
    bool getHubMixMuteForPeer(const String& senderUserName) const;
    float getHubMixGainForPeer(const String& senderUserName) const;
    bool getHubMixSoloForPeer(const String& senderUserName) const;
    bool isAnyHubMixSoloed() const;

    // Tell the hub to allocate a per-(listener, sender) audio feed, addressed to this client's oursink at
    // `ourIdForSender`. Sends /sb/hxfeedreq s:senderUserName i:ourIdForSender.
    std::map<String, uint32> mLastFeedReqMs;   // Per-sender dedup of /sb/hxfeedreq bursts
    void sendHubFeedReqForPeer(const String& senderUserName, int32_t ourIdForSender);

    // The hub learns which local oursink id carries `senderUserName`'s feed only from the one-shot /sb/hxfeedreq
    // above, and the AOO sink never re-requests a format for a compact-data source on its own. When a sender quits
    // and rejoins, the listener's RemotePeer is recreated (new oursink, possibly a new ourId) while the hub keeps the
    // deterministic feed source id; if the feedreq/format exchange is lost or lands on the stale binding, the row
    // stays 0ch and deaf until the listener relaunches. This re-checks the row a few times after (re)creation and,
    // while it has no format, re-invites, asks the source to resend /format and resends the feedreq (the hub re-binds
    // old ourId -> new via allocateOrUpdateFeed). A row is also treated as unbound while RemotePeer::hostedFeedStale
    // is set (hub disconnect seen, no fresh /format yet), so a hub restart recovers without a client relaunch.
    // Message thread only; no-op once recvChannels > 0 or the row is gone.
    void scheduleHostedFeedRebind(const String& senderUserName, int32_t ourIdForSender, int attempt = 0);

    // No-op stubs: listeners compute meters locally from the relayed feeds.
    void noteHubMixLevelForPeer(const String& senderUserName, float peakLin);
    float getHubMixLevelForPeer(const String& senderUserName) const;


    int getRemotePeerChannelGroupCount(int index) const;
    void setRemotePeerChannelGroupCount(int index, int count);

    int getRemotePeerRecvChannelCount(int index) const;

    //int getRemotePeerChannelGroupChannelCount(int index, int changroup) const;
    //void setRemotePeerChannelGroupChannelCount(int index, int changroup, int count);

    void setRemotePeerChannelGroupStartAndCount(int index, int changroup, int start, int count);
    bool getRemotePeerChannelGroupStartAndCount(int index, int changroup, int & retstart, int & retcount);

    void setRemotePeerChannelGroupDestStartAndCount(int index, int changroup, int start, int count);
    bool getRemotePeerChannelGroupDestStartAndCount(int index, int changroup, int & retstart, int & retcount);

    void setRemotePeerChannelGroupSendMainMix(int index, int changroup, bool mainmix);
    bool getRemotePeerChannelGroupSendMainMix(int index, int changroup);

    bool insertRemotePeerChannelGroup(int index, int atgroup, int chstart, int chcount);
    bool removeRemotePeerChannelGroup(int index, int atgroup);

    bool copyRemotePeerChannelGroup(int index, int fromgroup, int togroup);

    String getRemotePeerChannelGroupName(int index, int changroup) const;
    void setRemotePeerChannelGroupName(int index, int changroup, const String & name);


    int getRemotePeerNominalSendChannelCount(int index) const;
    void setRemotePeerNominalSendChannelCount(int index, int numchans);

    int getRemotePeerOverrideSendChannelCount(int index) const;
    void setRemotePeerOverrideSendChannelCount(int index, int numchans);

    int getRemotePeerActualSendChannelCount(int index) const;

    bool getRemotePeerViewExpanded(int index) const;
    void setRemotePeerViewExpanded(int index, bool expanded);

    bool getLayoutFormatChangedForRemotePeer(int index) const;
    void restoreLayoutFormatForRemotePeer(int index);


    void setRemotePeerBufferTime(int index, float bufferMs);
    float getRemotePeerBufferTime(int index) const;

    void setRemotePeerAutoresizeBufferMode(int index, AutoNetBufferMode flag);
    AutoNetBufferMode getRemotePeerAutoresizeBufferMode(int index, bool & initCompleted) const;

    // Unified receive-buffer policy for HostedMix. In hub topology every remote player's audio arrives over the same
    // network path (hub -> us), so one value + one auto-mode flag applies uniformly to all remote peer sinks. The
    // per-peer setters above delegate here when HostedMix is enabled.
    void setUnifiedReceiveBufferTime(float bufferMs);
    float getUnifiedReceiveBufferTime() const { return mUnifiedReceiveBufferMs; }
    float getHostedFeedDepthMs() const;
    float sbRowLiveDepthMs(const RemotePeer * remote) const;   // One row's live sink depth (ms), -1 = none
    void setUnifiedReceiveAutoresizeBufferMode(AutoNetBufferMode flag);
    AutoNetBufferMode getUnifiedReceiveAutoresizeBufferMode() const { return (AutoNetBufferMode) mUnifiedReceiveAutoMode; }

    // acceptable limit for drop rate in dropinstance/second, above which it will adjust the jitter buffer in Auto modes
    void setAutoresizeBufferDropRateThreshold(float);
    float getAutoresizeBufferDropRateThreshold() const { return mAutoresizeDropRateThresh; }


    bool getRemotePeerReceiveBufferFillRatio(int index, float & retratio, float & retstddev) const;
    float getRemotePeerResamplerBacklogMs(int index) const;  

    
    void setRemotePeerSendActive(int index, bool active);
    bool getRemotePeerSendActive(int index) const;

    void setRemotePeerRecvActive(int index, bool active);
    bool getRemotePeerRecvActive(int index) const;

    void setRemotePeerSendAllow(int index, bool allow, bool cached=false);
    bool getRemotePeerSendAllow(int index, bool cached=false) const;

    void setRemotePeerRecvAllow(int index, bool allow, bool cached=false);
    bool getRemotePeerRecvAllow(int index, bool cached=false) const;

    void setRemotePeerSoloed(int index, bool soloed);
    bool getRemotePeerSoloed(int index) const;

    
    int64_t getRemotePeerPacketsReceived(int index) const;
    int64_t getRemotePeerPacketsSent(int index) const;

    int64_t getRemotePeerBytesReceived(int index) const;
    int64_t getRemotePeerStreamBytesReceived(int index) const;   // This peer's OWN stream (per AOO source), not the shared hub socket
    bool getRemotePeerHubStats(int index, float & rttMs, float & upKbps) const;   // Their ping + uplink rate (hub-reported)
    int64_t getRemotePeerBytesSent(int index) const;

    int64_t getRemotePeerPacketsDropped(int index) const;
    int64_t getRemotePeerPacketsResent(int index) const;
    void    resetRemotePeerPacketStats(int index);

    bool getRemotePeerSafetyMuted(int index) const;
    bool getRemotePeerBlockedUs(int index) const;
    
    struct LatencyInfo
    {
        float pingMs = 0.0f;
        float totalRoundtripMs = 0.0f;
        float outgoingMs = 0.0f;
        float incomingMs = 0.0f;
        float jitterMs = 0.0f;
        bool isreal = false;
        bool estimated = false;
        bool legacy = false;
    };
    
    bool getRemotePeerLatencyInfo(int index, LatencyInfo & retinfo) const;

    bool startRemotePeerLatencyTest(int index, float durationsec = 1.0);
    bool stopRemotePeerLatencyTest(int index);
    bool isRemotePeerLatencyTestActive(int index);
    
    
    bool isAnyRemotePeerRecording() const;
    bool isRemotePeerRecording(int index) const;

    void setRemotePeerCompressorParams(int index, int changroup, SonoAudio::CompressorParams & params);
    bool getRemotePeerCompressorParams(int index, int changroup, SonoAudio::CompressorParams & retparams);

    void setRemotePeerExpanderParams(int index, int changroup, SonoAudio::CompressorParams & params);
    bool getRemotePeerExpanderParams(int index, int changroup, SonoAudio::CompressorParams & retparams);

    void setRemotePeerEqParams(int index, int changroup, SonoAudio::ParametricEqParams & params);
    bool getRemotePeerEqParams(int index, int changroup, SonoAudio::ParametricEqParams & retparams);

    bool getRemotePeerEffectsActive(int index, int changroup);



    void setInputCompressorParams(int changroup, SonoAudio::CompressorParams & params);
    bool getInputCompressorParams(int changroup, SonoAudio::CompressorParams & retparams);

    void setInputExpanderParams(int changroup, SonoAudio::CompressorParams & params);
    bool getInputExpanderParams(int changroup, SonoAudio::CompressorParams & retparams);

    void setInputLimiterParams(int changroup, SonoAudio::CompressorParams & params);
    bool getInputLimiterParams(int changroup, SonoAudio::CompressorParams & retparams);

    
    // input channel group stuff

    void setInputGroupCount(int count);
    int getInputGroupCount() const { return mInputChannelGroupCount; }

    void setInputGroupChannelStartAndCount(int changroup, int start, int count);
    bool getInputGroupChannelStartAndCount(int changroup, int & retstart, int & retcount);

    void setInputGroupChannelDestStartAndCount(int changroup, int start, int count);
    bool getInputGroupChannelDestStartAndCount(int changroup, int & retstart, int & retcount);


    // input monitor delay stuff
    void setInputMonitorDelayParams(int changroup, SonoAudio::DelayParams & params);
    bool getInputMonitorDelayParams(int changroup, SonoAudio::DelayParams & retparams);


    // pushes existing groups around
    bool insertInputChannelGroup(int atgroup, int chstart, int chcount);
    bool removeInputChannelGroup(int atgroup);
    bool moveInputChannelGroupTo(int atgroup, int togroup);

    void setInputGroupName(int changroup, const String & name);
    String getInputGroupName(int changroup);

    void setInputChannelPan(int changroup, int chan, float pan);
    float getInputChannelPan(int changroup, int chan);

    void setInputGroupGain(int changroup, float gain);
    float getInputGroupGain(int changroup);

    void setInputMonitor(int changroup, float mgain);
    float getInputMonitor(int changroup);

    void setInputGroupMuted(int changroup, bool muted);
    bool getInputGroupMuted(int changroup);

    void setInputGroupSoloed(int changroup, bool muted);
    bool getInputGroupSoloed(int changroup);


    void setInputReverbSend(int changroup, float rgain, bool input=false);
    float getInputReverbSend(int changroup, bool input=false);

    void setInputPolarityInvert(int changroup, bool invert);
    bool getInputPolarityInvert(int changroup);

    void setInputEqParams(int changroup, SonoAudio::ParametricEqParams & params);
    bool getInputEqParams(int changroup, SonoAudio::ParametricEqParams & retparams);
    
    
    int  getInputGroupGateState(int changroup) const { return (changroup >= 0 && changroup < MAX_CHANGROUPS) ? mInputChannelGroups[changroup].gateStatePub.load(std::memory_order_relaxed) : -1; }   // -1 off, 0 closed, 1 open
    bool getInputEffectsActive(int changroup) const { return mInputChannelGroups[changroup].params.compressorParams.enabled || mInputChannelGroups[changroup].params.expanderParams.enabled || mInputChannelGroups[changroup].params.eqParams.enabled || mInputChannelGroups[changroup].params.invertPolarity || (mInputChannelGroups[changroup].params.inReverbSend > 0.0f); }

    bool getInputMonitorEffectsActive(int changroup) const { return mInputChannelGroups[changroup].params.monitorDelayParams.enabled
        || (mInputChannelGroups[changroup].params.monReverbSend > 0.0f); }

    int getNumberAudioCodecFormats() const {  return mAudioFormats.size(); }

    void setDefaultAudioCodecFormat(int formatIndex);
    int getDefaultAudioCodecFormat() const { return mDefaultAudioFormatIndex; }

    void setChangingDefaultAudioCodecSetsExisting(bool flag) { mChangingDefaultAudioCodecChangesAll = flag; }
    bool getChangingDefaultAudioCodecSetsExisting() const { return mChangingDefaultAudioCodecChangesAll;}

    void setChangingDefaultRecvAudioCodecSetsExisting(bool flag) { mChangingDefaultRecvAudioCodecChangesAll = flag; }
    bool getChangingDefaultRecvAudioCodecSetsExisting() const { return mChangingDefaultRecvAudioCodecChangesAll;}

    
    String getAudioCodeFormatName(int formatIndex) const;
    bool getAudioCodeFormatInfo(int formatIndex, AudioCodecFormatInfo & retinfo) const;

    void setDefaultAutoresizeBufferMode(AutoNetBufferMode flag);
    AutoNetBufferMode getDefaultAutoresizeBufferMode() const { return (AutoNetBufferMode) defaultAutoNetbufMode; }
    
    bool getSendingFilePlaybackAudio() const { return mSendPlaybackAudio.get(); }

    bool getAutoReconnectToLast() const { return mAutoReconnectLast.get(); }

    bool getSyncMetToHost() const { return mSyncMetToHost.get(); }

    // misc settings
    bool getSlidersSnapToMousePosition() const { return mSliderSnapToMouse; }
    void setSlidersSnapToMousePosition(bool flag) {  mSliderSnapToMouse = flag; }

    bool getDisableKeyboardShortcuts() const { return mDisableKeyboardShortcuts; }
    void setDisableKeyboardShortcuts(bool flag) {  mDisableKeyboardShortcuts = flag; }

    bool getConfirmQuitDuringSession() const { return mConfirmQuitDuringSession; }
    void setConfirmQuitDuringSession(bool flag) { mConfirmQuitDuringSession = flag; }

    struct VideoLinkInfo
    {
        ValueTree getValueTree() const;
        void setFromValueTree(const ValueTree & val);

        enum {
            PushAndView = 0,
            PushOnly = 1,
            ViewOnly =2
        };


        bool roomMode = true;
        bool showNames = true;
        bool beDirector = false;
        bool screenShareMode = false;
        bool largeShare = false;
        int pushViewMode = PushAndView;
        String extraParams;
    };

    VideoLinkInfo & getVideoLinkInfo() { return mVideoLinkInfo; }


    // sets and gets the format we send out
    void setRemotePeerAudioCodecFormat(int index, int formatIndex);
    int getRemotePeerAudioCodecFormat(int index) const;

    // gets and requests the format that we are receiving from the remote end
    bool getRemotePeerReceiveAudioCodecFormat(int index, AudioCodecFormatInfo & retinfo) const;
    bool setRequestRemotePeerSendAudioCodecFormat(int index, int formatIndex);
    int getRequestRemotePeerSendAudioCodecFormat(int index) const; // -1 is no preferences
    
    int getRemotePeerSendPacketsize(int index) const;
    void setRemotePeerSendPacketsize(int index, int psize);

    int getRemotePeerOrderPriority(int index) const;
    void setRemotePeerOrderPriority(int index, int priority);

    // select by index or by peer, or don't specify for all
    void updateRemotePeerUserFormat(int index=-1, RemotePeer * onlypeer=nullptr);


    // danger
    foleys::LevelMeterSource * getRemotePeerRecvMeterSource(int index);
    foleys::LevelMeterSource * getRemotePeerSendMeterSource(int index);
    
    void setRemotePeerConnected(int index, bool active);
    bool getRemotePeerConnected(int index) const;
    
    bool getRemotePeerAddressInfo(int index, String & rethost, int & retport) const;

    bool getPatchMatrixValue(int srcindex, int destindex) const;
    void setPatchMatrixValue(int srcindex, int destindex, bool value);

    int getActiveSendChannelCount() const { return mActiveSendChannels; }

    foleys::LevelMeterSource & getInputMeterSource() { return inputMeterSource; }
    foleys::LevelMeterSource & getPostInputMeterSource() { return postinputMeterSource; }
    foleys::LevelMeterSource & getSendMeterSource() { return sendMeterSource; }
    foleys::LevelMeterSource & getOutputMeterSource() { return outputMeterSource; }

    foleys::LevelMeterSource & getFilePlaybackMeterSource() { return filePlaybackMeterSource; }
    foleys::LevelMeterSource & getMetronomeMeterSource() { return metMeterSource; }

    bool isAnythingRoutedToPeer(int index) const;
    
    bool isAnythingSoloed() const { return mAnythingSoloed.get(); }
    
    // IP block list
    bool isAddressBlocked(const String & ipaddr) const;
    void addBlockedAddress(const String & ipaddr);
    void removeBlockedAddress(const String & ipaddr);
    StringArray getAllBlockedAddresses() const;
    
    class ClientListener {
    public:
        virtual ~ClientListener() {}
        virtual void aooClientConnected(SonobusAudioProcessor *comp, bool success, const String & errmesg="") {}
        virtual void aooClientDisconnected(SonobusAudioProcessor *comp, bool success, const String & errmesg="") {}
        virtual void aooClientLoginResult(SonobusAudioProcessor *comp, bool success, const String & errmesg="") {}
        virtual void aooClientGroupJoined(SonobusAudioProcessor *comp, bool success, const String & group,  const String & errmesg="") {}
        virtual void aooClientGroupLeft(SonobusAudioProcessor *comp, bool success, const String & group, const String & errmesg="") {}
        virtual void aooClientPublicGroupModified(SonobusAudioProcessor *comp, const String & group, int count, const String & errmesg="") {}
        virtual void aooClientPublicGroupDeleted(SonobusAudioProcessor *comp, const String & group,  const String & errmesg="") {}
        virtual void aooClientPeerPendingJoin(SonobusAudioProcessor *comp, const String & group, const String & user) {}
        virtual void aooClientPeerJoined(SonobusAudioProcessor *comp, const String & group, const String & user) {}
        virtual void aooClientPeerJoinFailed(SonobusAudioProcessor *comp, const String & group, const String & user) {}
        virtual void aooClientPeerJoinBlocked(SonobusAudioProcessor *comp, const String & group, const String & user, const String & address, int port) {}
        virtual void aooClientPeerLeft(SonobusAudioProcessor *comp, const String & group, const String & user) {}
        virtual void aooClientError(SonobusAudioProcessor *comp, const String & errmesg) {}
        virtual void aooClientPeerChangedState(SonobusAudioProcessor *comp, const String & mesg) {}
        virtual void sbChatEventReceived(SonobusAudioProcessor *comp, const SBChatEvent & chatevent) {}
        virtual void peerRequestedLatencyMatch(SonobusAudioProcessor *comp, const String & username, float latency) {}
        virtual void peerBlockedInfoChanged(SonobusAudioProcessor *comp, const String & username, bool blocked) {}
        virtual void peerSuggestedNewGroup(SonobusAudioProcessor *comp, const String & username, const String & newgroup, const String & grouppass, bool isPublic, const StringArray & others) {}
    };
    
    void addClientListener(ClientListener * l) {
        clientListeners.add(l);
    }
    void removeClientListener(ClientListener * l) {
        clientListeners.remove(l);
    }
    
    // effects
    void setMainReverbEnabled(bool flag);
    bool getMainReverbEnabled() const { return mMainReverbEnabled.get(); }
    void  setMainReverbDryLevel(float level);
    float getMainReverbDryLevel() const;
    void  setMainReverbWetLevel(float level);
    float getMainReverbWetLevel() const { return mMainReverbLevel.get(); }
    void  setMainReverbSize(float value);
    float getMainReverbSize() const { return mMainReverbSize.get(); }
    void  setMainReverbPreDelay(float valuemsec);
    float getMainReverbPreDelay() const { return mMainReverbPreDelay.get(); }
    void setMainReverbModel(ReverbModel flag);
    ReverbModel getMainReverbModel() const { return (ReverbModel) mMainReverbModel.get(); }

    void  setInputReverbWetLevel(float level);
    float getInputReverbWetLevel() const { return mInputReverbLevel.get(); }
    void  setInputReverbSize(float value);
    float getInputReverbSize() const { return mInputReverbSize.get(); }
    void  setInputReverbPreDelay(float valuemsec);
    float getInputReverbPreDelay() const { return mInputReverbPreDelay.get(); }


    void setMetronomeMonitorDelayParams(SonoAudio::DelayParams & params);
    bool getMetronomeMonitorDelayParams(SonoAudio::DelayParams & retparams);
    void setMetronomeChannelDestStartAndCount(int start, int count);
    bool getMetronomeChannelDestStartAndCount(int & retstart, int & retcount);
    void setMetronomePan(float pan);
    float getMetronomePan() const;
    void setMetronomeGain(float gain);
    float getMetronomeGain() const;
    void setMetronomeMonitor(float mgain);
    float getMetronomeMonitor() const;


    void setFilePlaybackMonitorDelayParams(SonoAudio::DelayParams & params);
    bool getFilePlaybackMonitorDelayParams(SonoAudio::DelayParams & retparams);
    void setFilePlaybackDestStartAndCount(int start, int count);
    bool getFilePlaybackDestStartAndCount(int & retstart, int & retcount);
    void setFilePlaybackGain(float gain);
    float getFilePlaybackGain() const;
    void setFilePlaybackMonitor(float mgain);
    float getFilePlaybackMonitor() const;


    void setLinkMonitoringDelayTimes(bool flag) { mLinkMonitoringDelayTimes = flag; }
    bool getLinkMonitoringDelayTimes() const { return mLinkMonitoringDelayTimes; }

    double getMonitoringDelayTimeFromAvgPeerLatency(float scalar=1.0f);


    // recording stuff, if record options are RecordMixOnly, or RecordSelf  (only) the file refers to the name of the file
    //   otherwise it refers to a directory where the files will be recorded (and also the prefix for the recorded files)
    bool startRecordingToFile(const URL & recordLocation, const String & filename, URL & mainreturl, uint32 recordOptions=RecordDefaultOptions, RecordFileFormat fileformat=FileFormatDefault);
    bool stopRecordingToFile();
    bool isRecordingToFile();
    double getElapsedRecordTime() const { return mElapsedRecordSamples / getSampleRate(); }
    String getLastErrorMessage() const { return mLastError; }

    void setDefaultRecordingDirectory(const URL & recdir)  { mDefaultRecordDir = recdir; }
    URL getDefaultRecordingDirectory() const { return mDefaultRecordDir; }

    void setLastBrowseDirectory(String recdir)  { mLastBrowseDir = recdir; }
    String getLastBrowseDirectory() const { return mLastBrowseDir; }

    uint32 getDefaultRecordingOptions() const { return mDefaultRecordingOptions; }
    void setDefaultRecordingOptions(uint32 opts) { mDefaultRecordingOptions = opts; }

    RecordFileFormat getDefaultRecordingFormat() const { return mDefaultRecordingFormat; }
    void setDefaultRecordingFormat(RecordFileFormat fmt) { mDefaultRecordingFormat = fmt; }

    int getDefaultRecordingBitsPerSample() const { return mDefaultRecordingBitsPerSample; }
    void setDefaultRecordingBitsPerSample(int fmt) { mDefaultRecordingBitsPerSample = fmt; }

    bool getSelfRecordingPreFX() const { return mRecordInputPreFX; }
    void setSelfRecordingPreFX(bool flag) { mRecordInputPreFX = flag; }

    bool getSelfRecordingSilenceWhenMuted() const { return mRecordInputSilenceWhenMuted; }
    void setSelfRecordingSilenceWhenMuted(bool flag) { mRecordInputSilenceWhenMuted = flag; }

    bool getRecordFinishOpens() const { return mRecordFinishOpens; }
    void setRecordFinishOpens(bool flag) { mRecordFinishOpens = flag; }

    bool getReconnectAfterServerLoss() const { return mReconnectAfterServerLoss.get(); }
    void setReconnectAfterServerLoss(bool flag) { mReconnectAfterServerLoss = flag; }


    PeerDisplayMode getPeerDisplayMode() const { return mPeerDisplayMode; }
    void setPeerDisplayMode(PeerDisplayMode mode) { mPeerDisplayMode = mode; }

    // latency match stuff
    void beginLatencyMatchProcedure();
    bool isLatencyMatchProcedureReady();
    void sendLatencyMatchToAll(float latency);
    void getLatencyInfoList(Array<LatInfo> & retlist);
    void commitLatencyMatch(float latency);

    // invite peers to new group stuff
    void suggestNewGroupToPeers(const String & group, const String & groupPass, const StringArray & peernames, bool ispublic=false);

    void sendBlockedInfoMessage(EndpointState *endpoint, bool blocked);

    // playback stuff
    bool loadURLIntoTransport (const URL& audioURL);
    void clearTransportURL();
    URL getCurrentLoadedTransportURL () const { return mCurrTransportURL; }
    AudioTransportSource & getTransportSource() { return mTransportSource; }
    AudioFormatManager & getFormatManager() { return mFormatManager; }

    // chat
    bool sendChatEvent(const SBChatEvent & event);
    void setLastChatWidth(int width) { mLastChatWidth = width;}
    int getLastChatWidth() const { return mLastChatWidth; }
    void setLastUiScale(float scale) { mLastUiScale = scale; }
    float getLastUiScale() const { return mLastUiScale; }
    void setLastChatShown(bool shown) { mLastChatShown = shown; }
    bool getLastChatShown() const { return mLastChatShown; }
    void setChatUseFixedWidthFont(bool shown) { mChatUseFixedWidthFont = shown; }
    bool getChatUseFixedWidthFont() const { return mChatUseFixedWidthFont; }
    void setChatFontSizeOffset(int offset) { mChatFontSizeOffset = offset;}
    int getChatFontSizeOffset() const { return mChatFontSizeOffset; }
    Array<SBChatEvent, CriticalSection> & getAllChatEvents() { return mAllChatEvents; }

    // soundboard
    void setLastSoundboardWidth(int width) { mLastSoundboardWidth = width; }
    int getLastSoundboardWidth() const { return mLastSoundboardWidth; }
    void setLastSoundboardShown(bool shown) { mLastSoundboardShown = shown; }
    bool getLastSoundboardShown() const { return mLastSoundboardShown; }
    SoundboardChannelProcessor* getSoundboardProcessor() { return soundboardChannelProcessor.get(); }

    void setLastPluginBounds(juce::Rectangle<int> bounds) { mPluginWindowWidth = bounds.getWidth(); mPluginWindowHeight = bounds.getHeight();}
    juce::Rectangle<int> getLastPluginBounds() const { return juce::Rectangle<int>(0,0,mPluginWindowWidth, mPluginWindowHeight); }

    // support dir path
    File getSupportDir() const { return mSupportDir; }
    
    // language
    void setLanguageOverrideCode(const String & code) { mLangOverrideCode = code; }
    String getLanguageOverrideCode() const { return mLangOverrideCode; }

    void setUseUniversalFont(bool flag) { mUseUniversalFont = flag; }
    bool getUseUniversalFont() const { return mUseUniversalFont; }

private:
    //==============================================================================
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SonobusAudioProcessor)
    
    struct PeerStateCache
    {
        PeerStateCache();
        ValueTree getValueTree() const;
        void setFromValueTree(const ValueTree & val);

        String name;
        float netbuf = 10.0f;
        int   netbufauto;
        // Measure-commit-pin persisted pin (ms); 0 = no committed pin (start in MEASURE). Absent in old files, which
        // default to 0.
        float netbufmcppin = 0.0f;
        int   sendFormat = 4;

        float mainGain = 1.0f;
        SonoAudio::ChannelGroupParams channelGroupParams[MAX_CHANGROUPS];
        int numChanGroups = 1;
        SonoAudio::ChannelGroupParams channelGroupMultiParams[MAX_CHANGROUPS];
        int numMultiChanGroups = 0;
        bool modifiedChanGroups = false;
        int orderPriority = -1;
        // Row MUTE / SOLO as the player left them in hub mode = the per-listener hub-mix
        // override for this sender (what the row buttons toggle in HostedMix). Absent in old files -> false.
        // Never written for the hub's own row.
        bool hubMixMuted = false;
        bool hubMixSoloed = false;
    };

    // key is peer name
    typedef std::map<String, PeerStateCache>  PeerStateCacheMap;



    
    void initializeAoo(int udpPort=0);
    void cleanupAoo();
    
    void doReceiveData();
    // Per-packet parse/dispatch, split out of doReceiveData so the recv thread can drain a burst per select() wake.
    // Returns true if the packet was an AOO message (the send thread is then notified once for the burst). A datagram
    // may be a standard OSC bundle ("#bundle\0" + timetag + [int32 BE size][message]...) from a bundling hub; each
    // element is dispatched through this same function (depth+1, nested bundles allowed once, cap 2).
    bool handleReceivedPacket(const char * buf, int nbytes, EndpointState * endpoint, int depth = 0);
    // Integer-keyed endpoint lookup (no String construction). ipv4 is the sender address in network byte order, port
    // in host order.
    EndpointState * findEndpointFast(uint32_t ipv4, int port);
    void doSendData();
    void handleEvents();
    // 10 s counter dump from the event thread (never RT threads)
    void reportNetDietStatsIfDue();

    bool handleOtherMessage(EndpointState * endpoint, const char *msg, int32_t n);

    int32_t sendPeerMessage(RemotePeer * peer, const char *msg, int32_t n);

    void handleRemotePeerInfoUpdate(RemotePeer * peer, const juce::var & infodata);
    void sendRemotePeerInfoUpdate(int peerindex = -1, RemotePeer * topeer = nullptr);

    enum HostedControlWsLifecycleState {
        HostedControlWsIdle = 0,
        HostedControlWsConnecting,
        HostedControlWsConnected,
        HostedControlWsError,
        HostedControlWsClosing
    };

    void addHostedControlDiagnosticEvent(const String & eventLine);
    String hostedControlTransportModeLabel(HostedControlTransportMode mode) const;
    String hostedControlWsLifecycleStateLabel(HostedControlWsLifecycleState state) const;
    void hostedControlSetWsLifecycleState(HostedControlWsLifecycleState state, const String & error = {});
    bool hostedControlEnsureWebSocketClientPlaceholder();
    void hostedControlReleaseWebSocketClientPlaceholder();
    std::pair<int, int> hostedControlComputeReconnectJitterWindowMs(int baseDelayMs) const;
    void hostedControlNoteWebSocketReconnectScheduled(const String & reason);
    bool hostedControlConnectWebSocketStub(const String & host, int port);
    void hostedControlDisconnectWebSocketStub();
    void hostedControlQueueSendWebSocketScaffold(const String & command, const String & payloadJson, const String & reason);
    bool hostedControlFlushQueuedSendsWebSocketScaffold(const String & triggerReason = {});
    bool hostedControlPrepareWebSocketConnectScaffold(const String & host, int port);
    bool hostedControlEvaluateWebSocketConnectTimeoutScaffold(const String & triggerReason = {});
    bool hostedControlSendCommandWebSocketStub(const String & command, const String & payloadJson);
    bool hostedControlReplayRoomStateScaffold(const String & triggerReason = {});
    bool hostedControlRequestStateResync(const String & reason = {});
    String hostedControlBuildParticipantListJson() const;
    String hostedControlBuildParticipantMixControlsSummaryText() const;
    String hostedControlBuildParticipantMixControlsJson() const;
    void hostedControlEmitParticipantListEvent(const String & reason);

    void handlePingEvent(EndpointState * endpoint, uint64_t tt1, uint64_t tt2, uint64_t tt3);

    void sendPingEvent(RemotePeer * peer);

    void updateSafetyMuting(RemotePeer * peer);

    void setupSourceFormat(RemotePeer * peer, aoo::isource * source, bool latencymode=false);
    bool formatInfoToAooFormat(const AudioCodecFormatInfo & info, int channels, aoo_format_storage & retformat);

    void setupSourceUserFormat(RemotePeer * peer, aoo::isource * source);

    
    RemotePeer *  findRemotePeer(EndpointState * endpoint, int32_t ourId);
    RemotePeer *  findRemotePeerByEchoId(EndpointState * endpoint, int32_t echoId);
    RemotePeer *  findRemotePeerByLatencyId(EndpointState * endpoint, int32_t latId);
    RemotePeer *  findRemotePeerByRemoteSourceId(EndpointState * endpoint, int32_t sourceId);
    RemotePeer *  findRemotePeerByRemoteSinkId(EndpointState * endpoint, int32_t sinkId);
    RemotePeer *  doAddRemotePeerIfNecessary(EndpointState * endpoint, int32_t ourId=AOO_ID_NONE, const String & username={}, const String & groupname={});
    // HostedMix: add a row representing another musician for UI purposes only — no AOO audio plumbing.
    RemotePeer *  doAddInfoOnlyPeerIfNecessary(EndpointState * endpoint, const String & username, const String & groupname);
    bool doRemoveRemotePeerIfNecessary(EndpointState * endpoint, int32_t ourId);

    // GC stale same-name peer rows bound to a different endpoint than the authoritative one. Endpoint identity is a
    // pointer, so a hub bounce can mint a fresh EndpointState for the same address; (endpoint,name) dedupe then
    // misses and a rejoining sender would grow a duplicate feed row.
    bool removeStaleSameNamePeers(const String & username, EndpointState * keepEndpoint);

    bool removeAllRemotePeersWithEndpoint(EndpointState * endpoint);

    void adjustRemoteSendMatrix(int index, bool removed);

    void commitCompressorParams(RemotePeer * peer, int changroup);
    void commitInputCompressorParams(int changroup);

    //void commitExpanderParams(RemotePeer * peer);
    void commitInputExpanderParams(int changroup);
    void commitInputLimiterParams(int changroup);

    void commitInputEqParams(int changroup);
    void commitInputMonitoringParams(int changroup);

    void updateDynamicResampling();

    void updateRemotePeerSendChannels(int index, RemotePeer * remote);

    void setupSourceFormatsForAll();
    ValueTree getSendUserFormatLayoutTree();

    void applyLayoutFormatToPeer(RemotePeer * remote, const ValueTree & valtree);
    void restoreLayoutFormatForPeer(RemotePeer * remote, bool resetmulti=false);


    int connectRemotePeerRaw(void * sockaddr, const String & username = "", const String & groupname = "", bool reciprocate=true);

    int findFormatIndex(AudioCodecFormatCodec codec, int bitrate, int bitdepth) const;
    // The send-format index actually applied to a peer's source: the hub peer (hosted mode) is pinned to PCM 16-bit
    // regardless of persisted/user-selected formatIndex; everyone else is unchanged.
    int effectiveSendFormatIndex(const RemotePeer * peer) const;

    void ensureBuffers(int samples);

    void commitCacheForPeer(RemotePeer * peer);
    bool findAndLoadCacheForPeer(RemotePeer * peer);
    void sbFillHubMixCache(PeerStateCache & cache) const;      // Copy this name's hub-mix mute/solo into its cache entry
    void sbPersistHubMixOverride(const String & senderUserName); // Cache the row MUTE/SOLO the moment it changes

    // Per-peer application bodies for buffer time / auto
    // mode. The public setRemotePeer* setters route through the unified
    // policy in HostedMix mode and call these directly in P2P mode.
    void applyRemotePeerBufferTime(int index, float bufferMs);
    void applyRemotePeerAutoresizeBufferMode(int index, AutoNetBufferMode flag);

    // Measure-commit-pin lifecycle (event thread; see
    // handleSourceEvents). Commit pins at max(measured need, floor);
    // escalate raises one block after sustained underruns while pinned.
    void commitMcpPinForPeer(RemotePeer * peer, double nowtimeMs);
    void escalateMcpPinForPeer(RemotePeer * peer, double nowtimeMs);
    // Read the Smart Pin taps (SB_MCP_FLOOR_MS, SB_MCP_RESET_PIN) once, before any state/cache load, so the cache and
    // unified-state loaders see them.
    void loadMcpTaps();
    
    void loadPeerCacheFromState();
    void storePeerCacheToState();

    void loadGlobalState();
    bool storeGlobalState();

    void handleLatInfo(const juce::var & obj);
    juce::var getAllLatInfo();
    void sendReqLatInfoToAll();

    void moveOldMisplacedFiles();
    
    bool reconnectToMostRecent();

    
    ListenerList<ClientListener> clientListeners;

    
    AudioSampleBuffer tempBuffer;
    AudioSampleBuffer mixBuffer;
    AudioSampleBuffer workBuffer;
    AudioSampleBuffer inputBuffer;
    AudioSampleBuffer monitorBuffer;
    AudioSampleBuffer sendWorkBuffer;
    AudioSampleBuffer inputPostBuffer;
    AudioSampleBuffer inputPreBuffer;
    AudioSampleBuffer fileBuffer;
    AudioSampleBuffer metBuffer;
    AudioSampleBuffer mainFxBuffer;
    AudioSampleBuffer inputRevBuffer;
    AudioSampleBuffer silentBuffer; // only ever has one channel
    int mTempBufferSamples = 0;
    int mTempBufferChannels = 0;
    
    Atomic<float>   mInGain    { 1.0 };
    Atomic<float>   mInMonMonoPan    {   0.0 };
    Atomic<float>   mInMonPan1    {   -1.0 };
    Atomic<float>   mInMonPan2    {   1.0 };
    Atomic<float>   mDry    { 0.0 };
    Atomic<float>   mWet    {   1.0 };
    Atomic<double>   mBufferTime     { 0.001 };
    Atomic<double>   mMaxBufferTime     { 1.0 };
    Atomic<bool>   mMainSendMute    {   false };
    Atomic<bool>   mMainRecvMute    {   false };
    Atomic<bool>   mMainInMute    {   false };
    Atomic<bool>   mMainMonitorSolo    {   false };
    Atomic<bool>   mMetEnabled  { false };
    Atomic<bool>   mSendMet  { false };
    Atomic<int>   mSendChannels  { 1 }; // 0 is match inputs, 1 is 1, etc
    Atomic<float>   mMetGain    { 0.5f };
    Atomic<double>   mMetTempo    { 100.0f };
    Atomic<bool>   mSendPlaybackAudio  { true };    // Default ON — loading a file/sample is to share it
    Atomic<bool>   mSendSoundboardAudio  { true };
    Atomic<bool>   mHearLatencyTest  { false };
    Atomic<bool>   mMetIsRecorded  { true };
    Atomic<bool>   mMainReverbEnabled  { false };
    Atomic<float>   mMainReverbLevel  { 1.0f };
    Atomic<float>   mMainReverbSize  { 0.15f };
    Atomic<float>   mMainReverbDamping  { 0.5f };
    Atomic<float>   mMainReverbPreDelay  { 20.0f }; // ms
    Atomic<int>   mMainReverbModel  { ReverbModelMVerb };
    Atomic<bool>   mDynamicResampling  { false };
    // Design intent: WAN-first ensemble play, so sessions recover automatically after an app, network or system blip.
    // Default on; the user can disable it via Options.
    Atomic<bool>   mAutoReconnectLast  { true };

    // One-shot flag set when the initial-launch auto-reconnect retry chain achieves its first successful connect.
    // After that, queued retry timers bail unconditionally, so they cannot trigger a fresh connectToServer and have
    // the hub's same-username eviction evict the just-established session. Stays true for the session; subsequent
    // server losses are handled by mReconnectTimer.
    std::atomic<bool> mInitialAutoReconnectDone { false };
    Atomic<float>   mDefUserLevel    { 1.0f };
    Atomic<bool>   mSyncMetToHost  { false };
    Atomic<bool>   mSyncMetStartToPlayback  { false };
    Atomic<bool>   mReconnectAfterServerLoss  { true };

    Atomic<float>   mInputReverbLevel  { 1.0f };
    Atomic<float>   mInputReverbSize  { 0.15f };
    Atomic<float>   mInputReverbDamping  { 0.5f };
    Atomic<float>   mInputReverbPreDelay  { 20.0f }; // ms

    float mLastInputGain    = 0.0f;
    float mLastDry    = 0.0f;
    float mLastWet    = 0.0f;
    float mLastInMonMonoPan = 0.0f;
    float mLastInMonPan1 = -1.0f;
    float mLastInMonPan2 = 1.0f;
    bool mLastMetEnabled = false;
    bool mLastMainReverbEnabled = false;
    bool mReverbParamsChanged = false;
    bool mLastHasMainFx = false;
    bool mLastInputReverbEnabled = false;
    bool mFreshInit = true;
    
    Atomic<bool>   mAnythingSoloed  { false };


    int defaultAutoNetbufMode = AutoNetBufferModeAutoFull;

    // HostedMix unified receive-buffer policy state (see setUnifiedReceiveBufferTime). One shared network path -> one
    // value + one auto mode for every remote sink. Persisted via extraTree. Default matches the hosted feed-buffer
    // floor (8 ms, the lowest clean value on LAN).
    float mUnifiedReceiveBufferMs = 8.0f;
    // Hosted-mode default is measure-commit-pin, because continuous AutoFull rises quickly and falls slowly. P2P uses
    // defaultAutoNetbufMode above.
    int mUnifiedReceiveAutoMode = AutoNetBufferModeMeasureCommitPin;
    // True once a hosted-mode measure-commit-pin cycle has committed: reconnecting peers then skip MEASURE and pin
    // straight at mUnifiedReceiveBufferMs. Cleared by any manual unified buffer/mode set. Persisted via extraTree.
    bool mUnifiedMcpPinned = false;

    // Measure-commit-pin floor (ms), the clean threshold by listening (8 ms clean, 6 near, 4 edge). The commit value
    // is never pinned below this.
    float mMcpPinFloorMs = 2.67f;   // tested default
    // SB_MCP_RESET_PIN=1 for this process: every persisted pin (per-peer cache entries and the hosted unified flag)
    // is discarded at load, so every peer re-MEASUREs from the floor this launch.
    bool  mMcpResetPinTap = false;

    bool mChangingDefaultAudioCodecChangesAll = false;
    bool mChangingDefaultRecvAudioCodecChangesAll = false;

    RangedAudioParameter * mDefaultAutoNetbufModeParam;
    RangedAudioParameter * mTempoParameter;

    int mUseSpecificUdpPort = 0;

    bool mLinkMonitoringDelayTimes = true;

    // acceptable limit for drop rate in dropinstance/second
    // above which it will adjust the jitter buffer in Auto modes
    float mAutoresizeDropRateThresh = 0.5f;

    bool hasInitializedInMonPanners = false;
    
    int maxBlockSize = 4096;

    int mSinkBlocksize = 1024;
    int mSourceBlocksize = 1024;
    
    int currSamplesPerBlock = 256;
    int lastSamplesPerBlock = 256;

    int blocksizeCounter = -1;
    Atomic<bool> mNeedsSampleSetup  { false };

    float meterRmsWindow = 0.0f;
    
    int lastInputChannels = 0;
    int lastOutputChannels = 0;

    int mLastChatWidth = 250;
    float mLastUiScale = 1.0f;
    bool mLastChatShown = false;
    bool mChatUseFixedWidthFont = false;
    int mChatFontSizeOffset = 0;
    // chat message storage, thread-safe
    SBChatEventList mAllChatEvents;

    int mLastSoundboardWidth = 250;
    bool mLastSoundboardShown = false;

    int mPluginWindowWidth = 800;
    int mPluginWindowHeight = 600;


    int mActiveSendChannels = 0;

    PeerDisplayMode mPeerDisplayMode = PeerDisplayModeFull;
    
    PeerStateCacheMap mPeerStateCacheMap;
    
    // top level meter sources
    foleys::LevelMeterSource inputMeterSource;
    foleys::LevelMeterSource postinputMeterSource;
    foleys::LevelMeterSource sendMeterSource;
    foleys::LevelMeterSource outputMeterSource;
    foleys::LevelMeterSource filePlaybackMeterSource;
    foleys::LevelMeterSource metMeterSource;

    // AOO stuff
    aoo::isource::pointer mAooDummySource;

    aoo::net::iserver::pointer mAooServer;
    aoo::net::iclient::pointer mAooClient;

    std::unique_ptr<EndpointState> mServerEndpoint;
    
    bool mAutoconnectGroupPeers = true;
    bool mIsConnectedToServer = false;
    Atomic<int> mServerConnectInFlight { 0 };
    String mCurrentJoinedGroup;
    double mSessionConnectionStamp = 0.0;
    bool mWatchPublicGroups = false;
    // Default to HostedMix: SonoBus RP is hub-centric and WAN-primary. P2P only works for LAN-LAN scenarios where
    // peer address advertisements resolve; WAN clients can't reach LAN peers' RFC1918 addresses. HostedMix routes all
    // audio through the hub via feedreqs, which works regardless.
    Atomic<int> mNetworkTopologyMode { NetworkTopologyHostedMix };
    String mHubPeerUsername { "SonoBus-Hub" };
    std::atomic<uint32_t> mHubLinkIpv4 { 0 };   // The address WE reach the hub at (network byte order) — relayed-row LAN/WAN profile
    bool mSendAllDefaultsMigrated = false;   
    CriticalSection mHubLinkLock;            // Written on the UDP recv thread, read by the GUI
    static std::atomic<int> sHubBlockSize;   // From the hub hello (arg 5); 32 until told
    static std::atomic<int> sHubSampleRate;  // Hello arg 6
public:
    static std::function<void()> sHubBlockChanged;   // Message-thread hook, refreshes the audio settings panel
private:
    String mHubLinkHost;
    int    mHubLinkPort = 0;

    // Cache of per-peer overrides this client has set for the hub mix. Key is the sender's userName (the peer being
    // controlled in our mix). The cache lets the UI render the user's choice on rebuild and re-send the override if
    // the hub peer reconnects. Read/written on the message thread; protected by the same mCoreLock as
    // mPeerStateCacheMap.
    struct HubMixOverrideCache { bool muted = false; float gain = 1.0f; bool soloed = false; bool mutedBeforeMuteAll = false; /* Restored when Mute Everyone is released */ };
    std::map<String, HubMixOverrideCache> mHubMixOverrides;
    // 
    Atomic<int>      mInternalRecvMuteToggle { 0 };   // 1 while the block-size "reset all incoming" trick flips mastrecvmute (not a user mute)
    std::set<String> mMuteAllHubMuted;                 // senders WE hub-muted for Mute Everyone (restored on release; row edits during it opt out)
    bool             mInMuteAllApply = false;
    Atomic<int> mHostedControlTransportMode { HostedControlTransportStub };
    Atomic<int> mHostedControlConnected { 0 };
    Atomic<int> mHostedControlCommandStubCount { 0 };
    String mHostedControlHost;
    int mHostedControlPort = 0;
    String mHostedControlLastCommand;
    Atomic<int> mHostedControlWsScaffoldCommandCount { 0 };
    Atomic<int> mHostedControlWsLifecycleState { HostedControlWsIdle };
    Atomic<int> mHostedControlWsClientPlaceholderOwned { 0 };
    String mHostedControlWsLastError;
    int64 mHostedControlWsLastStateChangeMs = 0;
    int64 mHostedControlLastCommandAtMs = 0;
    int mHostedControlBlockedSendCount = 0;
    int mHostedControlBlockedTransportSwitchCount = 0;
    int mHostedControlWsReconnectAttemptCount = 0;
    int mHostedControlWsReconnectDelayMs = 0;
    int mHostedControlWsReconnectJitterMinMs = 0;
    int mHostedControlWsReconnectJitterMaxMs = 0;
    int64 mHostedControlWsReconnectScheduledAtMs = 0;
    int64 mHostedControlWsConnectStartedAtMs = 0;
    int mHostedControlWsConnectTimeoutMs = 0;
    int mHostedControlWsQueuedSendCount = 0;
    int mHostedControlWsFlushCount = 0;
    int mHostedControlWsFlushBlockedEmptyCount = 0;
    int mHostedControlWsFlushBlockedTransportCount = 0;
    int mHostedControlWsFlushBlockedDisconnectedCount = 0;
    String mHostedControlWsLastQueuedReason;
    String mHostedControlWsLastFlushReason;
    int64 mHostedControlWsLastFlushAtMs = 0;
    StringArray mHostedControlWsQueuedCommands;
    StringArray mHostedControlWsQueuedPayloadSnapshots;
    struct HostedParticipantMixControlState {
        float gain = 1.0f;
        float pan = 0.0f;
        bool muted = false;
        bool mixMinusSelf = false;
    };

    String mHostedControlCurrentRoomName;
    StringArray mHostedControlParticipantNames;
    std::map<String, HostedParticipantMixControlState> mHostedControlParticipantMixControls;
    StringArray mHostedControlReplayPendingAckKeys;
    bool mHostedControlRoomMixPublished = false;
    bool mHostedControlRoomMixSubscribed = false;
    String mHostedControlSubscribedMixRoom;
    double mHostedControlTelemetryRttMs = 0.0;
    double mHostedControlTelemetryPacketLossPct = 0.0;
    double mHostedControlTelemetryJitterMs = 0.0;
    String mHostedDiagnosticsStatusChipText { "Self-check: n/a" };
    StringArray mHostedControlDiagnosticEvents;
    static constexpr int kHostedControlMaxDiagnosticEvents = 8;
    static constexpr int kHostedControlMaxQueuedSendScaffold = 16;
    String mCurrentUsername;
    // Our own to/from-hub legs as the hub computes them (/sb/hxlat self entry)
    float mSelfHubLatToMs = -1.0f, mSelfHubLatFromMs = -1.0f, mSelfHubRttMs = -1.0f;
    int64_t mSelfHubUplinkLost = -1;
    uint32 mSelfHubLatStampMs = 0;
    // /sb/hxlat accounting, universal + cheap (recv thread counts,
    // the 10-s health tick reads). Age of the last APPLIED self entry rides /sb/hxbuf so the hub can show it per player.
    std::atomic<uint32_t> mHxlatRx { 0 }, mHxlatSelf { 0 }, mHxlatPeer { 0 }, mHxlatUnmatched { 0 };
    uint32_t mHxlatRxLast = 0, mHxlatSelfLast = 0, mHxlatPeerLast = 0, mHxlatUnmatchedLast = 0; int mHxlatTick = 0;
    std::map<String, int64_t> mSbDropsLast; uint64_t mSbLostSiteLast[8] = {0};   // [SB-DROPS] 30-s per-row + per-reason deltas
    std::atomic<uint32_t> mHxlatLastRxStampMs { 0 };
    char mHxlatLastUnmatched[48] = {0};

    double mPrevSampleRate = 0.0;
    Atomic<bool> mPendingUnmute {false}; // jlc
    uint32 mPendingUnmuteAtStamp = 0;

    // we will add sinks for any peer we invite, as part of a RemoteSource
    
    
    std::unique_ptr<DatagramSocket> mUdpSocket;
    // RECV thread tight RT contract. Values are
    // computed next to netRtOptions in initializeAoo() and applied by
    // RecvThread::run() on its own thread via thread_policy_set (macOS). See
    // the comment block at netRtOptions. Tap SB_RECV_TIGHT=0 disables.
    bool   mRecvTightEnabled       = false;
    double mRecvTightPeriodMs      = 0.0;
    double mRecvTightComputationMs = 0.0;
    double mRecvTightConstraintMs  = 0.0;
    int mUdpLocalPort;
    IPAddress mLocalIPAddress;
    
    class SendThread;
    class RecvThread;
    class EventThread;
    class ServerThread;
    class ClientThread;
    
    CriticalSection  mEndpointsLock;
    ReadWriteLock    mCoreLock;
    CriticalSection  mClientLock;
    CriticalSection  mSourceFormatLock;

    OwnedArray<EndpointState> mEndpoints;

    OwnedArray<RemotePeer> mRemotePeers;

    // ── recv-thread caches ────────────────────
    // Everything in this block is touched ONLY by the recv thread (single
    // writer/reader, no locking), except the atomics which the event thread
    // reads for the 10 s report. mEndpoints entries are never removed while
    // the recv thread runs (only cleanupAoo() clears them, after stopThread),
    // so the cached EndpointState* cannot dangle; it is reset in cleanupAoo().
    EndpointState * mRecvLastEndpoint = nullptr;
    uint32_t        mRecvLastAddr = 0;   // network byte order
    int             mRecvLastPort = 0;   // host order

    // (EndpointState*, salt) -> index into mRemotePeers of the sink that last
    // accepted a compact ("/d") data packet with that salt. The index is
    // bounds-checked under mCoreLock's read lock and the sink's own
    // handle_message() return value is the validator, so a stale entry can
    // only cost one extra parse attempt (falls back to the full scan).
    struct SinkDispatchEntry {
        EndpointState * ep = nullptr;
        int32_t salt = 0;
        int peerIndex = -1;
        const void * peer = nullptr;   // Identity of the cached peer (list indexes shift on join/leave)
    };
    static constexpr int kSinkDispatchSlots = 32; // power of two
    SinkDispatchEntry mSinkDispatch[kSinkDispatchSlots];

    // stats: relaxed atomics, RT threads store, event thread reads
    std::atomic<uint64_t> mNetStatRecvWakes { 0 };        // select() wakes that yielded >=1 packet
    std::atomic<uint64_t> mNetStatRecvPackets { 0 };      // datagrams read
    std::atomic<uint64_t> mNetStatRecvMaxBurst { 0 };     // largest packets-per-wake seen in the interval
    std::atomic<uint64_t> mNetStatEndpointCacheMiss { 0 };// sender != last endpoint (took the scan)
    std::atomic<uint64_t> mNetStatEndpointStringPath { 0 };// fell through to String/findOrAddEndpoint
    std::atomic<uint64_t> mNetStatDispatchHits { 0 };     // compact packet accepted by cached sink
    std::atomic<uint64_t> mNetStatDispatchMisses { 0 };   // compact packet needed the full sink scan
    std::atomic<uint64_t> mNetStatSendWakes { 0 };        // doSendData() calls
    std::atomic<uint64_t> mNetStatSendPasses { 0 };       // inner while(didsomething) iterations
    std::atomic<uint64_t> mNetStatDispatchDups { 0 };     // cached sink rejected = redundancy dup/late; full scan skipped
    // OSC-bundle datagrams from a bundling hub
    std::atomic<uint64_t> mNetStatBundlesRecv { 0 };      // "#bundle" datagrams received
    std::atomic<uint64_t> mNetStatBundleMsgs { 0 };       // messages unpacked from them
    std::atomic<uint64_t> mNetStatBundleBad { 0 };        // malformed bundles / elements dropped silently
    double   mNetStatLastReportMs = 0;
    uint64_t mNetStatPrev[13] = { 0,0,0,0,0,0,0,0,0,0,0,0,0 };
    int      mNetStatEnabled = -1; // -1 unknown, 0 off, 1 on (SB_NETDIET_STATS)


    Array<AooServerConnectionInfo> mRecentConnectionInfos;
    CriticalSection  mRecentsLock;
    
    AooServerConnectionInfo mPendingReconnectInfo;
    bool mPendingReconnect = false;
    bool mRecoveringFromServerLoss = false;
    
    class ServerReconnectTimer : public Timer
    {
    public:
        ServerReconnectTimer(SonobusAudioProcessor & proc) : processor(proc) {
        }
        
        void timerCallback() override;

        SonobusAudioProcessor & processor;
        // Ticks spent waiting on a pending reconnect that
        // never produced a CONNECT event (watchdog for silent no-op paths).
        int pendingStuckTicks = 0;
    };

    ServerReconnectTimer mReconnectTimer;

    // SB_DUMP_NETBUF telemetry: conf-gated (sb_taps.conf) low-rate logger. Every 2 s appends each remote peer's
    // netbuf ms + receive-buffer fill ratio/stddev to a file. Own thread, GUI-grade getters only; audio thread
    // untouched.
    // Audio health: audio-callback count (RT thread, one relaxed increment) + CoreAudio overloads (JUCE getXRunCount,
    // read by the editor timer). Logged every 10 s to stderr + the sink evlog as
    // 'callbacks'/'callbacks_missing'/'xrun' so skipped callbacks can be correlated with governor splices. Zero cost
    // when no editor timer runs.
    std::atomic<uint64_t> mSbAudioCallbacks{0};
    std::atomic<uint64_t> mSbNonFiniteInputs{0};   // NaN/Inf input samples scrubbed in processBlock
    std::atomic<uint64_t> mSbAudioFrames{0}; uint64_t mSbAudioFramesLast = 0;   // Frames actually delivered (short callbacks?)
    uint64_t mSbAudioCbLast = 0; int mSbXrunLast = 0; double mSbAudioHealthLastMs = 0;
    std::map<std::string, uint32_t> mSbAutoLogSeq;   // Last [SB-AUTO] decision seq logged, keyed by sink+source (survives row re-creation)
    int64_t mSbRxUnderrunsLast = 0, mSbRxPopsLast = 0;   // Receive-health deltas for the 10-s hub report
    // processBlock's wait for mCoreLock (read): max per health tick + count of waits > 1 ms. Diagnostic for
    // audio-thread stalls that trigger emergency drains on all sources at once.
    std::atomic<uint32_t> mSbLockWaitMaxUs { 0 };
    // Receive-thread lateness = recv-thread handling time minus the kernel's arrival timestamp,
    // per packet; reset every health tick. Tells whether "late" packets were late on the wire or late in the app.
    std::atomic<uint32_t> mSbRxLatMaxUs { 0 }, mSbRxLatOver2ms { 0 }, mSbRxPackets { 0 }, mSbRxMaxPerWake { 0 };
    std::atomic<uint64_t> mSbCbLastNs { 0 }; std::atomic<uint32_t> mSbCbGapMaxUs { 0 }, mSbCbGapOver2ms { 0 };   // Audio-callback interval jitter
    std::atomic<uint32_t> mSbLockWaitsOver1ms { 0 };
public:
    void sbAudioHealthTick(int xrunsTotal, double sr, int bufSize);
private:
    std::thread mNetbufLogThread;
    std::atomic<bool> mNetbufLogStop{false};
    void maybeStartNetbufLogger();
    
    CriticalSection  mRemotesLock;

    std::map<String,AooPublicGroupInfo> mPublicGroupInfos;
    CriticalSection  mPublicGroupsLock;

    
    
    Array<AudioCodecFormatInfo> mAudioFormats;
    // Design intent: PCM 16-bit (index 8) is the default send format. Trades bandwidth (~1.5 Mbps/peer/dir stereo)
    // for zero codec delay and bit-exact-within-16-bit quality vs Opus 96k/ch (index 4 in stock SonoBus).
    int mDefaultAudioFormatIndex = 8;
    
    RangedAudioParameter * mDefaultAudioFormatParam;




    CriticalSection  mLatInfoLock;
    Array<LatInfo> mLatInfoList;


    void initFormats();
    
    bool mRemoteSendMatrix[MAX_PEERS][MAX_PEERS];
    
    
    void notifySendThread() {
        mNeedSendSentinel += 1;
        mSendWaitable.signal();
    }
    
    WaitableEvent  mSendWaitable;
    Atomic<int>   mNeedSendSentinel  { 0 };


    std::unique_ptr<SendThread> mSendThread;
    std::unique_ptr<RecvThread> mRecvThread;
    std::unique_ptr<EventThread> mEventThread;
    std::unique_ptr<ServerThread> mServerThread;
    std::unique_ptr<ClientThread> mClientThread;


    // Input channelgroups
    SonoAudio::ChannelGroup mInputChannelGroups[MAX_CHANGROUPS];
    int mInputChannelGroupCount = 0;

    // Effects
    std::unique_ptr<Reverb> mMainReverb;
    Reverb::Parameters mMainReverbParams;
    MVerbFloat mMReverb;
    zitaRev mZitaReverb;
    MapUI  mZitaControl;

    ReverbModel mLastReverbModel = ReverbModelMVerb;

    // input reverb
    MVerbFloat mInputReverb;


    // met and playback channel groups
    SonoAudio::ChannelGroup  mMetChannelGroup;
    SonoAudio::ChannelGroup  mFilePlaybackChannelGroup;

    float _lastfplaygain = 0.0f;

    // and a replicant one for recording purposes
    SonoAudio::ChannelGroup  mRecMetChannelGroup;
    SonoAudio::ChannelGroup  mRecFilePlaybackChannelGroup;

    
    // recording stuff
    
    uint32 mDefaultRecordingOptions = RecordMix;
    RecordFileFormat mDefaultRecordingFormat = FileFormatFLAC;
    int mDefaultRecordingBitsPerSample = 16;
    bool mRecordInputPreFX = true;
    bool mRecordInputSilenceWhenMuted = true;
    bool mRecordFinishOpens = true;
    URL mDefaultRecordDir;
    String mLastError;
    int mSelfRecordChannels = 2;
    int mActiveInputChannels = 2;
    String mLastBrowseDir;

    std::atomic<bool> writingPossible = { false };
    std::atomic<bool> userWritingPossible = { false };
    int totalRecordingChannels = 2;
    int64 mElapsedRecordSamples = 0;
    int64 mHostedRecordingStartedAtMs = 0;
    String mHostedRecordingSessionId;
    uint32 mHostedRecordingLastOptions = 0;
    URL mHostedRecordingOutputDir;
    StringArray mHostedStemParticipants;
    StringArray mHostedStemFiles;
    StringArray mHostedStemChannels;
    int mHostedStemExpectedSampleRate = 0;
    int mHostedStemExpectedBitsPerSample = 16;
    std::unique_ptr<TimeSliceThread> recordingThread;
    std::unique_ptr<AudioFormatWriter::ThreadedWriter> threadedMixWriter;
    std::unique_ptr<AudioFormatWriter::ThreadedWriter> threadedMixMinusWriter;
    OwnedArray<AudioFormatWriter::ThreadedWriter> threadedSelfWriters;
    int  mSelfRecordChans[MAX_CHANGROUPS] { 0 };

    CriticalSection writerLock;
    std::atomic<AudioFormatWriter::ThreadedWriter*> activeMixWriter { nullptr };
    std::atomic<AudioFormatWriter::ThreadedWriter*> activeMixMinusWriter { nullptr };
    std::atomic<AudioFormatWriter::ThreadedWriter*> activeSelfWriters[MAX_CHANGROUPS] { nullptr };

    // playing stuff
    AudioTransportSource mTransportSource;
    std::unique_ptr<AudioFormatReaderSource> mCurrentAudioFileSource;
    AudioFormatManager mFormatManager;
    TimeSliceThread mDiskThread  { "audio file reader" };
    URL mCurrTransportURL;
    bool mTransportWasPlaying = false;

    // soundboard
    std::unique_ptr<SoundboardChannelProcessor> soundboardChannelProcessor;

    // metronome
    std::unique_ptr<SonoAudio::Metronome> mMetronome;
   
    // misc
    bool mSliderSnapToMouse = true;
    bool mDisableKeyboardShortcuts = false;
    bool mConfirmQuitDuringSession = true;
    VideoLinkInfo mVideoLinkInfo;
    
    File mSupportDir;
    
    // global config
    ValueTree   mGlobalState;
    
    String mLangOverrideCode;
    bool mUseUniversalFont = false;

    // main state
    AudioProcessorValueTreeState mState;
    UndoManager                  mUndoManager;

};


inline bool operator==(const SonobusAudioProcessor::LatInfo& lhs, const SonobusAudioProcessor::LatInfo& rhs) {
    // compare all except timestamp
     return (lhs.sourceName == rhs.sourceName
             && lhs.destName == rhs.destName
             );
}

inline bool operator!=(const SonobusAudioProcessor::LatInfo& lhs, const SonobusAudioProcessor::LatInfo& rhs){ return !(lhs == rhs); }

inline bool operator<(const SonobusAudioProcessor::LatInfo& lhs, const SonobusAudioProcessor::LatInfo& rhs)
{
    // default sorting alpha
    return (lhs.sourceName.compareIgnoreCase(rhs.sourceName) < 0);
}
