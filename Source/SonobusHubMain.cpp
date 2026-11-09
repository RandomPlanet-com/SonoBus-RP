// SPDX-License-Identifier: GPLv3-or-later WITH Appstore-exception
// SonoBus RP — Hub Server entry point.
//
// Default = GUI (juce_add_gui_app).
// --nogui still calls runHeadless() (pure pre-GUI code path, no JUCE GUI
// objects ever constructed). This branching plus CoutTee gating lets us
// bisect dispatch problems.
//
// Every hub parameter is now a field of
// HubLaunchArgs (SonobusHubSettings.h) and persists in
// hub_settings.json in hub_settings::dataDir(). Startup precedence:
//   CLI args > launch env (SB_AOO_* only) > settings file > built-in (ship) defaults
// resolved HERE, before the server is constructed; the SB_AOO_* keys are then
// setenv()'d in-process because the AOO sink reads them once per process.
//
// Usage: see printUsage() (`sonobus-hub --help`).

#include "SonobusHubServer.h"
#if !defined(SONOBUS_HUB_NOGUI)
#include "SonobusHubMainWindow.h"   // The Linux hub is headless-only (no X11 needed on a server)
#endif
#include "SonobusHubSettings.h"
#include "SonobusHubUpdater.h"
#include "SonobusHubGeo.h"
#include "SonobusHubWeb.h"   // Status page
#include <juce_core/system/juce_TargetPlatform.h>  // defines JUCE_MAC for the header below
#include "SonobusHubMacUtils.h"  // hubDisableAppNap; the upstream GPL CrossPlatformUtilsMac.mm is not compiled into the hub
#include "aoo/aoo.h"

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <set>
#include <string>
#include <thread>
#include <atomic>
#include <chrono>

#ifndef _WIN32
#include <sys/types.h>
#include <unistd.h>
#endif

static std::atomic<bool> gQuit{false};

// HubLaunchArgs lives in sb_hub_gui (SonobusHubSettings.h) since GUI and headless paths share it.
// Bring the types in via using-declarations so the rest of this file doesn't need namespace qualifiers.
using HubLaunchArgs = sb_hub_gui::HubLaunchArgs;
using HubSettingsMeta = sb_hub_gui::HubSettingsMeta;
namespace hs = sb_hub_gui::hub_settings;

static void signalHandler(int sig) {
    std::cout << "\n[HUB] Caught signal " << sig << ", shutting down...\n";
    gQuit = true;
}

static void printUsage(const char* progname) {
    std::cout << "SonoBus RP Hub Server — Centralized audio relay\n\n"
              << "Usage: " << progname << " [options]\n\n"
              << "Default behavior: launches a JUCE GUI window (Status + Settings tabs).\n"
              << "Use --nogui to retain headless (launchd-friendly) operation.\n\n"
              << "Settings file:\n"
              << "  Every option marked [S] persists in the hub's hub_settings.json\n"
              << "  (JSON; written by the GUI Settings tab — Save / Save & Restart hub — or by hand).\n"
              << "  Precedence: CLI args > launch env (SB_AOO_* only) > settings file > built-in defaults.\n"
              << "  The defaults printed below are the built-ins (the ship configuration): a bare\n"
              << "  launch with no args and no file comes up exactly like this.\n"
              << "  \"Save & Restart hub\" relaunches the process WITHOUT the [S] options from the\n"
              << "  original command line (so the saved file applies) and with SB_AOO_* cleared.\n"
              << "  --settings-file <path>  Use this file instead of the default location\n"
              << "  --no-settings           Ignore the settings file entirely (CLI + defaults only)\n\n"
              << "Session / network [S]:\n"
              << "  --server-port <port>  Port of the built-in AOO server — what players connect to (default: 12999)\n"
              << "  --public-address <ip> WAN IP/host advertised to peers (default: auto)\n"
              << "  --public-port <port>  Port advertised with --public-address (default: 13000)\n"
              << "  --udp-port <port>     Hub audio UDP port (default: 13000)\n"
              << "  --lan-subnet <cidr>   [experiment] override the LAN/WAN split (default: auto from the\n"
              << "                        default-route interface's address + netmask)\n"
              << "  --group <name>        Group to join (default: hub-test)\n"
              << "  --group-password <pw> Group password (default: none)\n\n"
              << "Audio [S]:\n"
              << "  --samplerate <rate>   Sample rate in Hz (default: 48000)\n"
              << "  --blocksize <size>    Audio block size in samples (16..256, default: 32)\n"
              << "  (codec is PCM 16-bit, hub user name is SonoBus-Hub; not options)\n\n"
              << "Fan-out / sink buffers [S]:\n"
              << "  --sink-buffer <ms>    Hub sink jitter buffer for WAN peers (default: 12)\n"
              << "  --sink-buffer-lan <ms> Sink buffer for LAN-subnet peers (default: 1.33;\n"
              << "                        <= 0 = LAN peers use --sink-buffer too)\n"
              << "  [experiment] = test-only flag: not persisted, not on the Settings tab\n"
              << "  --send-pace           [experiment] Pace per-listener fan-out sends across the block interval (default: ON)\n"
              << "  --no-send-pace        Plain immediate fan-out\n"
              << "  --no-pace-dither      Disable pacer phase dither (default: dither on)\n"
              << "  --pace-spread <pct>   Spread listener sends over this % of the block period (default: 50)\n"
              << "  --pace-min-slot-us <n> Floor for the pacer slot width in µs (default: 50; 0 disables)\n"
              << "  --cut-through         Emit each listener's block immediately from its own thread\n"
              << "                        (no pacer slot wait; overrides --send-pace; default off)\n"
              << "  --inline-listeners    Run per-listener fan-out on the audio thread (no listener RT\n"
              << "                        threads); refuses itself if peers x per-listener time > 60% of\n"
              << "                        P/2; at sub-ms blocks demotes --cut-through to paced (default off)\n"
              << "  --no-bundle           Disable OSC-bundle packing of each listener slot's feed\n"
              << "                        datagrams (default ON; active with --send-pace / --cut-through)\n"
              << "  --bundle-mtu <bytes>  UDP payload cap per bundle (default: 1400; fits 1500 MTU)\n"
              << "  --redundancy-lan N    Copies of each block sent to LAN listeners (default 1)\n"
              << "  --redundancy-wan N    Copies of each block sent to WAN listeners (default 1)\n"
              << "  --dscp | --no-dscp    Mark UDP datagrams DSCP EF (voice) (default: off)\n"
              << "  --connected-sockets   One connect()ed UDP send socket per listener, bound to the hub\n"
              << "                        port: send() instead of sendto() (3x cheaper per datagram);\n"
              << "                        the recv thread polls all of them (default: ON)\n"
              << "  --no-connected-sockets  Single shared UDP socket for every peer\n\n"
              << "AOO sink knobs [S] — exported to the process env before the first sink exists.\n"
              << "deps/aoo sink.cpp reads them ONCE per process (ahead of hub_taps.conf), so a\n"
              << "change needs a process relaunch (GUI: Save & Restart hub):\n"
              << "  --aoo-drain 0|1       SB_AOO_DRAIN depth governor (default 1)\n"
              << "  --aoo-arm-blocks N    SB_AOO_GOV_ARM_BLOCKS arm band in blocks; 0 = sink auto (default 1)\n"
              << "  --aoo-slack-blocks N  SB_AOO_SLACK_BLOCKS hidden depth above the pin (default 0)\n"
              << "  --aoo-blockq-extra N  SB_AOO_BLOCKQ_EXTRA blockqueue headroom in blocks (default 8)\n"
              << "  --aoo-handoff 0|1     SB_AOO_HANDOFF lead hand-off, audioqueue cap = target + bq (default 1)\n\n"
              << "Other:\n"
              << "  --web-port <port>     Web status page port (default 18080, 0 = off)\n"
              << "  --web-password <pw>   Protect the status page with a password (Basic auth)\n"
              << "  --location <text>     Shown on the web status page, e.g. \"Atlanta, GA\"\n"
              << "  --log-file <path>     Append stdout/stderr to a file [S] (needed for GUI mode —\n"
              << "                        `open`-launched apps otherwise drop logs; applies live from the GUI)\n"
              << "  --nogui               Run headless (no GUI window)\n"
              << "  --dump-peer <name>    Diagnostic: dump one peer's decoded audio (not persisted)\n"
              << "  --dump-secs <n>       Seconds to dump (default 30)\n"
              << "  --wait-for-pid <pid>  (internal) wait for a previous instance to exit before starting\n"
              << "  --setup               Ask for the group and passwords, save them, and exit\n"
              << "  --version             Show the version and exit\n"
              << "  --install-update <url> [sha256]  Install a package over this copy and exit\n"
              << "  --help                Show this help\n";
}

// Parses argv onto `a` (already holding settings-file / default values) and
// records which JSON keys argv pinned. Returns false on error / --help.
static bool parseArgs(int argc, char* argv[], HubLaunchArgs& a, bool& nogui, std::set<std::string>& cliKeys) {
    nogui = false;
    auto pin = [&cliKeys](const char* key) { cliKeys.insert(key); };

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") { printUsage(argv[0]); return false; }
        else if (arg == "--setup") { }   // handled in main()
        else if (arg == "--nogui") nogui = true;
        else if (arg == "--group" && i + 1 < argc) { a.groupName = argv[++i]; pin("group"); }
        else if (arg == "--group-password" && i + 1 < argc) { a.groupPassword = argv[++i]; pin("group_password"); }
        else if (arg == "--udp-port" && i + 1 < argc) { a.udpPort = std::atoi(argv[++i]); pin("udp_port"); }
        else if (arg == "--samplerate" && i + 1 < argc) { a.sampleRate = std::atoi(argv[++i]); pin("samplerate"); }
        else if (arg == "--blocksize" && i + 1 < argc) { a.blockSize = std::atoi(argv[++i]); pin("blocksize"); }
        else if (arg == "--pace-spread" && i + 1 < argc) { a.paceSpread = std::atoi(argv[++i]); pin("pace_spread"); }   
        else if (arg == "--pace-min-slot-us" && i + 1 < argc) { a.paceMinSlotUs = std::atoi(argv[++i]); pin("pace_min_slot_us"); }
        else if (arg == "--server-port" && i + 1 < argc) { a.localServerPort = std::atoi(argv[++i]); pin("server_port"); }
        else if (arg == "--public-address" && i + 1 < argc) { a.publicAddressHost = argv[++i]; pin("public_address"); }
        else if (arg == "--port-check-helper" && i + 1 < argc) { a.portCheckHelper = argv[++i]; pin("port_check_helper"); }   
        else if (arg == "--public-port" && i + 1 < argc) { a.publicAddressPort = std::atoi(argv[++i]); pin("public_port"); }
        else if (arg == "--lan-subnet" && i + 1 < argc) { a.lanSubnetCidr = argv[++i]; pin("lan_subnet"); }
        else if (arg == "--sink-buffer" && i + 1 < argc) { a.sinkBufferMs = std::atof(argv[++i]); pin("sink_buffer_ms"); }
        else if (arg == "--sink-buffer-lan" && i + 1 < argc) { a.sinkBufferLanMs = std::atof(argv[++i]); pin("sink_buffer_lan_ms"); }
        else if (arg == "--send-pace") { a.sendPace = true; pin("send_pace"); }      // Paced fan-out
        else if (arg == "--no-send-pace") { a.sendPace = false; pin("send_pace"); }
        else if (arg == "--no-pace-dither") { a.paceDither = false; pin("pace_dither"); }   // A/B control for phase dither
        else if (arg == "--cut-through") { a.cutThrough = true; pin("cut_through"); }        // Listener-thread emission
        else if (arg == "--inline-listeners") { a.inlineListeners = true; pin("inline_listeners"); }   // (rt-split A)
        else if (arg == "--no-bundle") { a.bundle = false; pin("bundle"); }             // Hub diet v2
        else if (arg == "--bundle-mtu" && i + 1 < argc) { a.bundleMtu = std::atoi(argv[++i]); pin("bundle_mtu"); }
        else if (arg == "--redundancy-lan" && i + 1 < argc) { a.redundancyLan = std::atoi(argv[++i]); pin("redundancy_lan"); }   
        else if (arg == "--redundancy-wan" && i + 1 < argc) { a.redundancyWan = std::atoi(argv[++i]); pin("redundancy_wan"); }
        else if (arg == "--no-dscp") { a.dscp = false; pin("dscp"); }   
        else if (arg == "--dscp") { a.dscp = true; pin("dscp"); }
        else if (arg == "--connected-sockets") { a.connectedSockets = true; pin("connected_sockets"); }      
        else if (arg == "--no-connected-sockets") { a.connectedSockets = false; pin("connected_sockets"); }
        else if (arg == "--log-file" && i + 1 < argc) { a.logFile = argv[++i]; pin("log_file"); }
        else if (arg == "--web-port" && i + 1 < argc) { a.webPort = std::atoi(argv[++i]); pin("web_port"); }          
        else if (arg == "--web-password" && i + 1 < argc) { a.webPassword = argv[++i]; pin("web_password"); }
        else if (arg == "--location" && i + 1 < argc) { a.location = argv[++i]; pin("location"); }   
        // AOO sink knobs (were launchd env only)
        else if (arg == "--aoo-drain" && i + 1 < argc) { a.aooDrain = std::atoi(argv[++i]); pin("aoo_drain"); }
        else if (arg == "--aoo-arm-blocks" && i + 1 < argc) { a.aooGovArmBlocks = std::atoi(argv[++i]); pin("aoo_gov_arm_blocks"); }
        else if (arg == "--aoo-slack-blocks" && i + 1 < argc) { a.aooSlackBlocks = std::atoi(argv[++i]); pin("aoo_slack_blocks"); }
        else if (arg == "--aoo-blockq-extra" && i + 1 < argc) { a.aooBlockqExtra = std::atoi(argv[++i]); pin("aoo_blockq_extra"); }
        else if (arg == "--aoo-handoff" && i + 1 < argc) { a.aooHandoff = std::atoi(argv[++i]); pin("aoo_handoff"); }
        // consumed by the pre-pass in main(); accepted here so they aren't "unknown"
        else if (arg == "--settings-file" && i + 1 < argc) { ++i; }
        else if (arg == "--no-settings") {}
        else if (arg == "--wait-for-pid" && i + 1 < argc) { ++i; }
        else if (arg == "--dump-peer" && i + 1 < argc) a.dumpPeer = argv[++i];
        else if (arg == "--dump-secs" && i + 1 < argc) a.dumpSecs = std::atoi(argv[++i]);
        else {
            std::cerr << "Unknown option: " << arg << "\n";
            printUsage(argv[0]);
            return false;
        }
    }

    if (a.groupName.empty()) {
        std::cerr << "Error: --group is required (or set \"group\" in the settings file)\n\n";
        printUsage(argv[0]);
        return false;
    }
    if (auto problem = hs::validate(a); !problem.empty()) {
        std::cerr << "Error: " << problem << "\n\n";
        printUsage(argv[0]);
        return false;
    }
    return true;
}

// A "Save & Restart hub" child is spawned while the
// old instance is still alive. Wait here — before JUCE's single-instance lock
// and before any socket bind — until the old pid is gone, so its UDP/TCP ports
// are free and JUCE doesn't see "another instance is running".
static void waitForPidExit(long pid) {
#ifndef _WIN32
    if (pid <= 0 || pid == (long)getpid()) return;
    fprintf(stderr, "[HUB] relaunch: waiting for previous instance pid %ld to exit...\n", pid);
    for (int i = 0; i < 300; ++i) {   // 30 s cap
        if (kill((pid_t)pid, 0) != 0 && errno == ESRCH) {
            fprintf(stderr, "[HUB] relaunch: previous instance gone after ~%d ms\n", i * 100);
            std::this_thread::sleep_for(std::chrono::milliseconds(300));   // let the kernel release the ports
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    fprintf(stderr, "[HUB] relaunch: previous instance pid %ld still alive after 30 s — continuing anyway\n", pid);
#else
    (void)pid;
#endif
}

static constexpr int kExitNotSetUp = 78;

static std::string askLine(const char* prompt) {
    std::cout << prompt << std::flush;
    std::string line;
    std::getline(std::cin, line);
    return juce::String(line).trim().toStdString();
}

// First-run questions for a hub with no screen. Values given as flags are not asked again.
static int runSetup(HubLaunchArgs& a, const HubSettingsMeta& meta) {
    const bool gave = meta.cliKeys.count("group") != 0;
    if (!gave) a.groupName = askLine("Group name (players use it to join): ");
    if (a.groupName.empty()) { std::cerr << "A group name is required.\n"; return 1; }
    if (meta.cliKeys.count("group_password") == 0 && !gave) a.groupPassword = askLine("Group password (optional): ");
    if (meta.cliKeys.count("web_password") == 0 && !gave) a.webPassword = askLine("Web page password (needed to change settings from the web page): ");
    std::string err;
    if (!hs::save(meta.file, a, err)) { std::cerr << "Could not save settings: " << err << "\n"; return 1; }
    std::cout << "Saved " << meta.file.getFullPathName() << "\n";
    return 0;
}

static int runHeadless(HubLaunchArgs a, const juce::File& settingsFile) {   // By value — the web Settings page edits it
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    std::string serverHost;
    int serverPort = 0;
    hs::resolveServerEndpoint(a, serverHost, serverPort);

    SonobusHubServer hub;
    hs::applyToHub(hub, a);   // Shared with the GUI paths
    if (settingsFile != juce::File()) hub.setSessionsFile(settingsFile.getSiblingFile("hub_sessions.json").getFullPathName().toStdString());   // Recent sessions (+ their players) persist next to hub_settings.json

    std::cout << "=== SonoBus RP Hub Server (headless) ===\n"
              << "Server: " << serverHost << ":" << serverPort << "\n"
              << "User:   " << a.userName << "\n"
              << "Group:  " << a.groupName << "\n"
              << "Audio:  " << a.sampleRate << " Hz, " << a.blockSize << " samples, " << a.channels << " ch\n"
              << "Codec:  " << (a.useOpus ? "Opus " + std::to_string(a.opusBitrate / 1000) + " kbps/ch"
                                          : std::string("PCM 16-bit"))
              << "\n"
              << "Local:  Built-in AOO server on port " << a.localServerPort << "\n"
              << "=========================================\n\n";

    if (!hub.start(serverHost, serverPort, a.userName, a.groupName, a.groupPassword,
                   a.udpPort, a.sampleRate, a.blockSize, a.channels)) {
        std::cerr << "Failed to start hub server.\n";
        return 1;
    }

    SonobusHubWeb web(hub, a.webPort, a.webPassword);   // Web status page
    web.setLocation(a.location);
    // Settings section. "Save & Restart" ends this process cleanly; the systemd unit
    // (Restart=always) or the launcher brings it back with the saved file.
    web.setSettings(&a, settingsFile, [] { std::cout << "[HUB-WEB] Save & Restart — quitting for the supervisor to relaunch\n"; gQuit = true; });
    sb_hub_gui::HubUpdateCheck updateCheck;
    web.setUpdateCheck(&updateCheck);
    updateCheck.startThread();
    web.start();


    // Only log when the peer count changes.
    int lastPeers = -1;
    int logCheckTicks = 0;
    while (!gQuit.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        if (++logCheckTicks >= 600) {
            logCheckTicks = 0;
            if (!a.logFile.empty() && juce::File(juce::String::fromUTF8(a.logFile.c_str())).getSize() >= 50 * 1024 * 1024)
                hs::redirectLogFile(a.logFile);
        }
        int peers = hub.getNumConnectedPeers();
        if (peers != lastPeers) {
            std::cout << "[HUB] Active peers: " << peers << "\n";
            lastPeers = peers;
        }
    }

    web.stop();
    hub.stop();
    return 0;
}

int main(int argc, char* argv[]) {
    // Never die of SIGPIPE. On Darwin a UDP send raises it only when the SOCKET itself was killed under us
    // (SOF_DEFUNCT / SS_CANTSENDMORE: an NECP verdict, not a vanished peer; see healEpipeSockets). Complements the
    // SO_NOSIGPIPE settings in the server: a hub launched through LaunchServices otherwise dies silently (signal 13).
    signal(SIGPIPE, SIG_IGN);
    // Pre-pass for the options that must act before anything
    // else — the settings-file location and the relaunch hand-off wait.
    juce::File settingsFile = hs::defaultFile();
    bool noSettings = false;
    long waitPid = 0;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--settings-file" && i + 1 < argc)
            settingsFile = juce::File::getCurrentWorkingDirectory().getChildFile(juce::String(argv[++i]));
        else if (arg == "--no-settings") noSettings = true;
        else if (arg == "--wait-for-pid" && i + 1 < argc) waitPid = std::atol(argv[++i]);
    }
    for (int i = 1; i < argc; ++i)
        if (std::string(argv[i]) == "--version") {
            std::cout << "SonoBus RP Hub " << SONOBUS_HUB_VERSION << " build " << hubBuildId() << "\n";
            return 0;
        }
    for (int i = 1; i + 1 < argc; ++i)
        if (std::string(argv[i]) == "--install-update") {   // <url> [sha256]: installs a package over this copy, for headless hubs
            sb_hub_gui::HubUpdateZip zip;
            zip.url = argv[i + 1];
            if (i + 2 < argc) zip.sha256 = argv[i + 2];
            const juce::ScopedJuceInitialiser_GUI juce;
            const auto r = sb_hub_gui::hubInstallUpdate(zip, [](const juce::String& s) { std::cout << "[HUB-UPDATE] " << s << "\n"; return true; });
            std::cout << "[HUB-UPDATE] " << (r.wasOk() ? juce::String("installed") : r.getErrorMessage()) << "\n";
            return r.wasOk() ? 0 : 1;
        }
    waitForPidExit(waitPid);
    hs::migrateOldDataDir();

    // Hub reads ITS OWN taps file, never the client's (both can run on one Mac).
    // Override with SB_TAPS_FILE in the environment.
    if (const char* cur = getenv("SB_TAPS_FILE"); !cur || !cur[0]) {
        std::string hubTaps = hs::dataDir().getChildFile("hub_taps.conf").getFullPathName().toStdString();
        setenv("SB_TAPS_FILE", hubTaps.c_str(), 1);
        fprintf(stderr, "[HUB] taps file: %s\n", hubTaps.c_str());
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IOLBF, 0);
#if defined(__APPLE__)
    // NSActivityLatencyCritical | NSActivityUserInitiated for the process lifetime. The hub owns no audio
    // device, so without this macOS App-Naps it the moment its window loses focus: recv thread demoted,
    // hub sinks starve, relay collapses until refocus.
    hubDisableAppNap();   // Own implementation (SonobusHubMacUtils.mm), same NSProcessInfo activity
#endif
    // Hub sink concealment, default ON. A sink refill would otherwise forward a short silence hole to every
    // listener, a click in music. The PCM decoder's bounded PLC (2 ms full quality, fade to silence by 8 ms) fills it,
    // the same conceal the players use at the edge; a long WAN stall still goes quiet after 8 ms, so no spiral.
    // SB_HUB_PLC=0 (env or the hub taps file) turns it off for A/B. setenv here, in-process before the first
    // decoder exists, because launcher exports never cross open()/launchd into a GUI app.
    {
        bool plc_on = true;
        if (const char* e = std::getenv("SB_HUB_PLC"); e && e[0]) plc_on = std::atoi(e) > 0;
        else if (const char* tf = std::getenv("SB_TAPS_FILE"); tf && tf[0]){
            if (FILE* f = std::fopen(tf, "r")){ char line[256]; while (std::fgets(line, sizeof(line), f)){ if (!std::strncmp(line, "SB_HUB_PLC=", 11)){ plc_on = std::atoi(line + 11) > 0; break; } } std::fclose(f); }
        }
        if (!plc_on) setenv("SB_PLC_OFF", "1", 1);
        std::cout << "[HUB] concealment on hub sinks: " << (plc_on ? "ON" : "off (SB_PLC_OFF)") << "\n";
    }

    // ── Resolve settings: built-in defaults ← settings file ← CLI ──
    HubLaunchArgs a;            // member initializers = ship defaults
    HubSettingsMeta meta;
    meta.file = noSettings ? juce::File() : settingsFile;
    if (!noSettings) {
        std::string err;
        meta.loadedFromFile = hs::load(meta.file, a, err);
        meta.loadError = err;
    }

    bool nogui = false;
    bool setup = false;
    for (int i = 1; i < argc; ++i) if (std::string(argv[i]) == "--setup") setup = true;
    if (!parseArgs(argc, argv, a, nogui, meta.cliKeys)) return 1;
    hs::applyDefaults(a);

    if (setup) return runSetup(a, meta);
#if defined(SONOBUS_HUB_NOGUI)
    // Same rule as the Mac app: nothing is hosted until the operator has named the group.
    if (!noSettings && !meta.loadedFromFile && meta.loadError.empty() && meta.cliKeys.count("group") == 0) {
        std::cerr << "This hub has not been set up yet. Run: sonobus-hub --setup\n";
        return kExitNotSetUp;
    }
#endif

    // Redirect logging to a file when asked. Critical for GUI mode: `open`-launched apps have no tty,
    // so [HUB-JIT]/[HUB-ABUF] telemetry would vanish. Line-buffered so tail -f works live.
    if (!a.logFile.empty()) hs::redirectLogFile(a.logFile);

    if (noSettings) {
        std::cout << "[HUB-SETTINGS] settings file disabled (--no-settings)\n";
    } else {
        std::cout << "[HUB-SETTINGS] file: " << meta.file.getFullPathName()
                  << (meta.loadedFromFile ? " (loaded)"
                      : meta.loadError.empty() ? " (not found — built-in defaults + CLI)"
                                               : " (ERROR: " + meta.loadError + " — built-in defaults + CLI)")
                  << "\n";
    }
    if (!meta.cliKeys.empty()) {
        std::cout << "[HUB-SETTINGS] pinned by CLI this launch:";
        for (auto& k : meta.cliKeys) std::cout << " " << k;
        std::cout << "\n";
    }

    // SB_AOO_* → process env, BEFORE aoo_initialize()/any sink (sink.cpp
    // getenv()s them once per process). Pre-existing env (launchd) wins over
    // the file unless the CLI pinned the key; see applySinkEnv.
    hs::applySinkEnv(a, meta.cliKeys, meta.envKeys);

    aoo_initialize();

#if defined(SONOBUS_HUB_NOGUI)
    (void) meta; (void) argc; (void) argv;
    if (!nogui) std::cout << "[HUB] headless build: running without a GUI (--nogui implied)\n";
    return runHeadless(a, meta.file);
#else
    if (nogui) return runHeadless(a, meta.file);
    return sb_hub_gui::runGui(a, meta, argc, argv);
#endif
}
