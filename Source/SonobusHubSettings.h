// SPDX-License-Identifier: GPLv3-or-later WITH Appstore-exception
// Copyright (C) 2026 Jason Buecker (Random Planet)
// SonoBus RP — Hub Server settings model + persistence.
//
// ("every assignable hub parameter settable AND
// persistent from the hub GUI, so a bare Dock launch comes up correctly").
//
// One model — HubLaunchArgs — feeds three consumers: the CLI parser
// (SonobusHubMain.cpp), the JSON settings file (dataDir()/
// hub_settings.json) and the GUI Settings tab (SonobusHubMainWindow.cpp).
//
// Precedence at startup (resolved in main(), BEFORE the server is constructed):
//   CLI args  >  pre-existing launch env (SB_AOO_* keys only)  >  settings file  >  built-in defaults
//
// The five SB_AOO_* keys are read by the AOO sink (deps/aoo/lib/src/sink.cpp)
// via getenv() ONCE per process at first sink setup, ahead of the taps file. So
// main() must setenv() them from the resolved settings before any sink exists
// (see applySinkEnv). Changing them from the GUI needs a full process relaunch
// ("Save & Restart hub"); everything else is picked up by an in-process hub
// restart, and the log-file path applies live.
//
// Latency policy: nothing in this file is ever touched by the audio, listener
// or pacer threads. Load/save/apply run in main() or on the JUCE message thread.
//
// ─── launcher pins vs code defaults ────
// The the hub Mac hub was started by a launcher script (launchctl setenv SB_AOO_*
// + `open -g -n sonobus-hub.app --args ...`). Goal: a bare double-click of the
// App icon (no script, no env, no args) behaves identically. Audit of every
// value a launcher pinned against the code default:
//
//   launcher pin                      | code default (5257d247)             | now
//   ----------------------------------+-------------------------------------+---------------------
//   --local-server                    | localServer = true                  | same
//   --server 192.0.2.25 (hairpin)   | "aoo.sonobus.net" -> 127.0.0.1      | 127.0.0.1 kept; LAN/WAN
//                                     |   (resolveServerEndpoint)           |   addresses derived at
//                                     |                                     |   start (see below)
//   --server-port 10999               | localServerPort = 10999             | same
//   --connected-sockets               | connectedSockets = false            | CHANGED -> true
//   --pace-spread 50                  | paceSpread = 80                     | CHANGED -> 50
//   --sink-buffer-lan 1.33 (SBLAN)    | sinkBufferLanMs = 1.33              | same
//   --sink-buffer 12 (SBWAN)          | sinkBufferMs = 12                   | same (help text said 16: fixed)
//   --redundancy-lan 1 (SBREDLAN)     | redundancyLan = 1                   | same
//   --redundancy-wan 1 (SBREDWAN)     | redundancyWan = 1                   | same
//   --blocksize 32 (SBBLOCK)          | blockSize = 32                      | same
//   (SBNODSCP empty: no --no-dscp)    | dscp = false                        | same
//   (no --group)                      | groupName = "hub-test"              | same (hub_settings.json overlays)
//   env SB_AOO_DRAIN=1                | aooDrain = 1 (sink.cpp raw: 1)      | same
//   env SB_AOO_GOV_ARM_BLOCKS=1       | aooGovArmBlocks = 1                 | same. sink.cpp raw default -1 = auto:
//                                     |                                     |   H = max(2, ceil(T/2)) (T = pin blocks),
//                                     |                                     |   = 2 for the 2-block LAN pin, 2 for adaptive
//                                     |                                     |   sinks, 9 for an 18-block WAN pin; so
//                                     |                                     |   1 DIFFERS from auto -> keep exporting 1
//   env SB_AOO_SLACK_BLOCKS=0         | aooSlackBlocks = 0 (raw -1=auto->0) | same
//   env SB_AOO_BLOCKQ_EXTRA=8         | aooBlockqExtra = 8 (raw: 8)         | same
//   env SB_AOO_HANDOFF=1              | aooHandoff = 1 (raw: 1)             | same
//   env SB_HUB_ASRC_ON=0              | hub_asrc_enabled(): unset = 0       | same (not a setting; off by code)
//
// The SB_AOO_* rows are exported to the process env by applySinkEnv() from
// these defaults, so a no-env launch reaches sink.cpp with the same values the
// launcher's launchctl setenv did.

#pragma once

#include "JuceHeader.h"

#include <set>
#include <string>
#include <utility>   // Std::pair (session snapshot)
#include <vector>

class SonobusHubServer;

namespace sb_hub_gui {

// ─── Hub launch arguments / persisted settings ────────────────────────────────
// Member defaults ARE the ship defaults. CLI flags and the settings file overlay them.
// Fields marked [CLI/experiment] are not settings: not in hub_settings.json, not on the Settings tab. Their member
// default is the ship value; only a CLI flag can move them for an experiment. The built-in AOO server is the only
// mode, the user name is fixed ("SonoBus-Hub"), the codec is PCM 16-bit, lan_subnet is auto-detected.
struct HubLaunchArgs {
    std::string serverHost = "127.0.0.1";        // not a setting: the hub's own client always dials the built-in server
    int serverPort = 10998;                      // external AOO server port (ignored when localServer)
    std::string userName = "SonoBus-Hub";
    std::string groupName = "hub-test";
    std::string groupPassword;
    int udpPort = 13000;  // Fixed default so hub bounces don't orphan clients; off SonoBus's own 11000
    int sampleRate = 48000;
    int blockSize = 32;
    int channels = 2;
    int opusBitrate = 128000;
    bool useOpus = false;           // ship default: PCM 16-bit
    bool localServer = true;        // ship default: built-in AOO signaling server
    int localServerPort = 12999;   // Off SonoBus's own 10999 (stock standalone's local server)
    std::string publicAddressHost;  // empty = auto
    std::string portCheckHelper = "sonobus-rp-hub-check.randomplanet.com:10999";   // Outside port checker, a DNS name so the server can move
    int publicAddressPort = 13000;   // Follows udpPort
    std::string lanSubnetCidr;      // empty = auto (default-route interface); --lan-subnet CLI override only
    // Per-peer sink jitter-buffer tuning. Base value
    // applies to every peer (WAN-safe). The LAN value, when >0 AND lanSubnetCidr
    // is set, applies only to peers whose address matches the LAN CIDR — so
    // remote/WAN peers can never be under-buffered by accident.
    double sinkBufferMs = 12.0;      // WAN inbound 12 (was 16)
    double sinkBufferLanMs = 1.33;   // 2 blocks — hub conceals 317→46 per 15 min, p50 unchanged
    std::string holdMode = "jam";   // RETIRED not a setting, not on the CLI; the hold has one policy (0/min). Live.
    // File path for stdout/stderr redirect (GUI apps launched
    // via `open` otherwise log to the void).
    std::string logFile;
#ifdef __APPLE__
    int webPort = 0;                // The Mac hub's own GUI carries the status page; web off unless set
#else
    int webPort = 18080;            // Web status page (0 = off)
#endif
    std::string webPassword;        // optional Basic auth password (user name is ignored)
    std::string location;           // Free text shown on the web page, e.g. "Atlanta, GA (cloud)"
    std::string dumpPeer; int dumpSecs = 0;   // diagnostic audio dump (CLI only, never persisted)
    // Paced fan-out emission (staggers per-listener sends
    // across the block interval; see SonobusHubServer::pacerThread).
    bool sendPace = true;
    bool paceDither = true;   // Evening: pacer phase dither
    int  paceSpread = 50;     // % of the block period the pacer spreads sends over (5..95)
    int  paceMinSlotUs = 50;  // Absolute floor for the pacer slot in µs (0..500)
    // --cut-through — each listener thread emits its own
    // bundled block right after the barrier (no pacer slot wait). Overrides
    // --send-pace. Default OFF.
    bool cutThrough = false;
    // --inline-listeners — the audio thread
    // pumps every listener's feeds itself (no per-listener RT threads, no
    // barrier). Measured-budget governor falls back to threads. Default OFF.
    bool inlineListeners = false;
    // OSC-bundle packing of each listener
    // slot's feed datagrams (--no-bundle / --bundle-mtu N). Active under
    // --send-pace (pacer) or --cut-through (listener threads).
    bool bundle = true;
    int bundleMtu = 1400;
    // Per-peer redundancy. LAN listeners get
    // redundancyLan copies of each block, WAN listeners redundancyWan.
    int redundancyLan = 1;
    int redundancyWan = 1;
    bool dscp = false;         // --dscp enables, --no-dscp disables (default off)
    // --connected-sockets: one connected UDP send socket per listener
    // (send() is 3x cheaper than sendto() at the hub's rate). Default ON.
    bool connectedSockets = true;

    // AOO sink knobs. These are exported to the process env
    // (SB_AOO_*) in main() because sink.cpp reads them with getenv() once per process.
    int aooDrain = 1;          // SB_AOO_DRAIN        depth governor on/off
    int aooGovArmBlocks = 1;   // SB_AOO_GOV_ARM_BLOCKS governor arm band (blocks); <1 = sink auto
    int aooSlackBlocks = 0;    // SB_AOO_SLACK_BLOCKS  hidden depth above the pin (blocks)
    int aooBlockqExtra = 8;    // SB_AOO_BLOCKQ_EXTRA  blockqueue headroom (blocks)
    int aooHandoff = 1;        // SB_AOO_HANDOFF       lead hand-off (audioqueue cap = target + bq)
    bool geoip = true;         // Look up each WAN player's city/state (ip-api.com) for the status page
};

// ─── Per-launch metadata (where values came from) ─────────────────────────────
struct HubSettingsMeta {
    juce::File file;                 // resolved settings path; invalid/empty when disabled (--no-settings)
    bool loadedFromFile = false;     // file existed and parsed
    std::string loadError;           // non-empty = file present but unusable (kept defaults)
    std::set<std::string> cliKeys;   // JSON keys pinned by argv this launch (CLI wins on this launch only)
    std::set<std::string> envKeys;   // aoo_* keys pinned by a pre-existing SB_AOO_* env var
};

namespace hub_settings {

// What it takes for an edit of a given key to become effective.
enum class Tier {
    Live,      // applies on Save (message thread, no audio-path work)
    Restart,   // in-process hub restart ("Restart Hub" button) or Save & Restart
    Relaunch   // full process relaunch (AOO sink reads SB_AOO_* env once per process)
};

struct FieldInfo {
    const char* key;      // JSON key (== CLI option name with '-' → '_')
    const char* cliFlag;  // CLI option name(s) for display
    Tier tier;
};
const std::vector<FieldInfo>& fields();
Tier tierForKey(const std::string& key);

// ONE form layout shared by the Mac GUI Settings tab and the
// web page's Settings section (so a new parameter is added in exactly one place).
// key == nullptr → section header. col 0 = left, 1 = right.
// kind: 0 Text, 1 Bool, 2 Choice (choices comma-separated, canonical values), 3 Password (text, masked).
struct FormSpec { int col; const char* key; const char* label; int kind; const char* choices; };
const std::vector<FormSpec>& formSpecs();

juce::File dataDir();                    // settings, taps, stats and session history
juce::File defaultLogFile();
void applyDefaults(HubLaunchArgs& a);    // fills values that depend on the machine
void migrateOldDataDir();                // call once, before anything reads dataDir()
const char* tipFor(const std::string& key);
void rotateLogIfLarge(const std::string& path);

// dataDir()/hub_settings.json (directory created on save).
juce::File defaultFile();

// Overlay every key present in the file onto `a` (missing keys keep their
// current value). Returns false + err when the file exists but is unusable;
// a missing file is NOT an error (returns false, err empty).
bool load(const juce::File& f, HubLaunchArgs& a, std::string& err);
// Serialize `a` (dumpPeer/dumpSecs excluded) and write it. Creates the dir.
bool save(const juce::File& f, const HubLaunchArgs& a, std::string& err);
juce::String toJson(const HubLaunchArgs& a);

// Export the five SB_AOO_* keys into the process environment (setenv, in
// process, before any sink exists). A pre-existing non-empty env var WINS
// unless that key is in cliKeys (CLI beats env); winners are reported in
// envKeys and copied back into `a` so the GUI shows the effective value.
void applySinkEnv(HubLaunchArgs& a, const std::set<std::string>& cliKeys,
                  std::set<std::string>& envKeys);
// Remove the five SB_AOO_* keys from the environment (so a relaunched child
// re-derives them from the settings file).
void clearSinkEnv();

// Configure a freshly constructed SonobusHubServer from `a` (everything that
// is set BEFORE start()). Shared by headless, GUI initialise and GUI restart.
void applyToHub(SonobusHubServer& hub, const HubLaunchArgs& a);
// Resolve the AOO signaling endpoint the hub's own client connects to.
void resolveServerEndpoint(const HubLaunchArgs& a, std::string& host, int& port);

// Build the argv for a "Save & Restart hub" relaunch: keeps argv[0] and the
// pass-through options (--nogui, --settings-file, --no-settings, --dump-peer,
// --dump-secs), DROPS every settings-covered option (so the saved file is
// what the child applies), and appends "--wait-for-pid <waitPid>".
std::vector<std::string> relaunchArgv(const std::vector<std::string>& argv, long waitPid);
// Spawn the current executable with `argv` (argv[0] replaced by the resolved
// executable path). Inherited descriptors other than stdio are closed in the
// child so the parent's sockets don't keep the ports bound. Returns false + err
// on failure; on success the caller should quit cleanly.
bool relaunchProcess(const std::vector<std::string>& argv, std::string& err);

// Append stdout/stderr to `path` (line-buffered). Used at startup (--log-file)
// and by the GUI's live apply of a changed log_file. Message thread / main() only.
void redirectLogFile(const std::string& path);

// Range/sanity check. Returns an empty string when OK, else a human-readable
// description of the first problem. load() rejects files that fail this;
// the GUI refuses to Save them.
std::string validate(const HubLaunchArgs& a);

// Compare two models field-by-field; returns the JSON keys that differ.
std::vector<std::string> diffKeys(const HubLaunchArgs& x, const HubLaunchArgs& y);

// "Recent sessions". sessionSnapshot = every persisted key (fields order) -> its canonical
// text (getValueString), handed to SonobusHubServer::setSessionSettingsSnapshot() by applyToHub() so the session the
// hub opens at start() remembers the settings it ran with. applySession overlays a stored session onto `a`:
//   - only keys that exist in formSpecs() are applied (unknown/retired keys from an old file are ignored),
//   - group / group_password come from `group` / `password` (empty password allowed = open group), never from the map,
//   - web_port / web_password keep their current value unless the snapshot carries them (and a BLANK stored
//     web_password never wipes the current one, a stored web_port of 0 never switches a running page off — the page
//     must stay reachable),
//   - validate() must pass. Returns false + err (model untouched) when group is empty, a value fails to parse or
//     validation fails; `applied` = number of keys written.
std::vector<std::pair<std::string, std::string>> sessionSnapshot(const HubLaunchArgs& a);
bool applySession(HubLaunchArgs& a, const std::string& group, const std::string& password,
                  const std::vector<std::pair<std::string, std::string>>& settings, int& applied, std::string& err);

// Canonical text form of one field (bools → "1"/"0", codec → "pcm"/"opus").
juce::String getValueString(const HubLaunchArgs& a, const std::string& key);
// Parse `text` into one field. Returns false (model untouched) on a parse or
// range error. Used by the GUI's text fields.
bool setValueFromString(HubLaunchArgs& a, const std::string& key, const juce::String& text);

} // namespace hub_settings
} // namespace sb_hub_gui
