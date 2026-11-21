// SPDX-License-Identifier: GPLv3-or-later WITH Appstore-exception
// Copyright (C) 2026 Jason Buecker (Random Planet)
// SonoBus RP — Hub Server settings model + persistence.
//
// Every assignable hub parameter is settable and persistent from the hub GUI, so a bare launch comes up correctly.
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
// A bare launch (no script, env or args) runs the ship defaults: applySinkEnv() exports the SB_AOO_* values
// below, so sink.cpp sees them without a launcher. aooGovArmBlocks = 1 is exported on purpose: the sink's own
// auto value (-1 = max(2, ceil(T/2)) blocks, T = pin blocks) differs from it.

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
    double sinkBufferMs = 12.0;      // WAN inbound (ms)
    double sinkBufferLanMs = 1.33;   // 2 blocks
    std::string holdMode = "jam";   // retired: not a setting, not on the CLI; the hold has one policy
    // File path for stdout/stderr redirect (GUI apps launched
    // via `open` otherwise log to the void).
    std::string logFile;
#ifdef __APPLE__
    int webPort = 0;                // The Mac hub's own GUI carries the status page; web off unless set
#else
    int webPort = 18080;            // Web status page (0 = off)
#endif
    std::string webPassword;        // web page password (user name is ignored); empty = the page stays off
    std::string location;           // Free text shown on the web page, e.g. "City, ST"
    std::string dumpPeer; int dumpSecs = 0;   // diagnostic audio dump (CLI only, never persisted)
    // Paced fan-out emission (staggers per-listener sends
    // across the block interval; see SonobusHubServer::pacerThread).
    bool sendPace = true;
    bool paceDither = true;   // pacer phase dither
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
    // (send() costs far less CPU than sendto() at the hub's rate). Default ON.
    bool connectedSockets = true;

    // AOO sink knobs. These are exported to the process env
    // (SB_AOO_*) in main() because sink.cpp reads them with getenv() once per process.
    int aooDrain = 1;          // SB_AOO_DRAIN        depth governor on/off
    int aooGovArmBlocks = 1;   // SB_AOO_GOV_ARM_BLOCKS governor arm band (blocks); <1 = sink auto
    int aooSlackBlocks = 0;    // SB_AOO_SLACK_BLOCKS  hidden depth above the pin (blocks)
    int aooBlockqExtra = 8;    // SB_AOO_BLOCKQ_EXTRA  blockqueue headroom (blocks)
    int aooHandoff = 1;        // SB_AOO_HANDOFF       lead hand-off (audioqueue cap = target + bq)
    bool geoip = true;         // Look up each WAN player's city/state (ip-api.com) for the status page
    // Stage limits and the stage directory. Live: they apply on Save. `location` is the stage's city.
    int maxPlayers = 20;       // 0 = no limit
    int maxChannels = 12;      // per player; 0 = no limit
    bool listStage = false;    // the operator opts in
    std::string stageGenre = "Any";
    std::string stageDescription;
    std::string welcomeMessage;   // sent to the chat of each player who joins; several lines, up to kWelcomeMaxBytes; empty = none
    std::string stageCountry;  // two-letter country code for the listing, for example US; empty = not given
    std::string stageNames = "Automatic";   // player names in the listing: Automatic (shown when open, hidden when locked) / Show / Hide
    int forgetPlayersDays = 0; // previous players not seen for this many days are forgotten; 0 = never
    std::string stageDirectory = "https://stages.randomplanet.com/api/1/";   // not on the form
    std::string stageHubId, stageHubKey;    // this hub's identity at the directory, created on first start; never shown
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
// kind: 0 Text, 1 Bool, 2 Choice (choices comma-separated, canonical values), 3 Password (text, masked),
//       4 Text of several lines.
// hint: one line shown under a section header or under a text of several lines.
struct FormSpec { int col; const char* key; const char* label; int kind; const char* choices; const char* hint = nullptr; };
const std::vector<FormSpec>& formSpecs();

// The welcome message as it is kept: line ends as "\n", no blank ends, at most kWelcomeMaxBytes of whole characters.
constexpr size_t kWelcomeMaxBytes = 2000;
std::string tidyWelcome(const std::string& text);

juce::File dataDir();                    // settings, taps, stats and session history
// A settings file outside the default folder makes its folder this instance's data folder (log, taps,
// stats, directory files) and allows a second hub on the machine. Call once at startup.
bool useSettingsFolderAsDataDir(const juce::File& settingsFile);
bool hasOwnDataDir();
// True when this hub is a stage of a main hub: its settings file sits in that hub's stages/ folder, or its folder
// holds the marker a main hub wrote, or (Mac and Windows) it was started beside the hub of the default folder.
// Every other hub is a main hub, also one started with its own settings file: it keeps its stages in that folder.
bool isStage();
juce::File stageMainFolder();            // a stage: its main hub's data folder
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
// The stage limits and listing settings, live: call after every Save (window and web page).
void applyStageToHub(SonobusHubServer& hub, const HubLaunchArgs& a);
const char* stageGenres();   // comma list for the genre choice; the directory's list wins once it is known
// First start: give this hub its directory identity (kept in hub_settings.json). True = created, caller saves.
bool ensureStageIdentity(HubLaunchArgs& a);
// Resolve the AOO signaling endpoint the hub's own client connects to.
void resolveServerEndpoint(const HubLaunchArgs& a, std::string& host, int& port);

// ─── Other stages on this computer ───
// Extra hubs, each run by this same app with its own settings folder (--settings-file). A main hub (see isStage)
// lists them, makes new ones, starts and stops them; a stage manages nothing. "Default folder" below = the main
// hub's data folder: the default one, or the folder of the settings file a main hub was started with.
// The list = other_stages.json in that folder (stages added by path, and which stages were started) plus
// every stage folder under <default folder>/stages. New stage makes such a folder, named after the stage, with
// the main stage's settings and its own name, password, ports and log. Ports are one row: the main stage's pair
// (12999 / 13000), then 13001 / 13002, 13003 / 13004, ... so one range is forwarded for all stages. Delete stops the stage (its clean stop
// takes it out of the stage directory) and moves the folder to <default folder>/stages-deleted; moving it back
// brings the stage back. A stage added by path can only be taken off the list.
// Running = the stage's hub.pid names a live process of this app. Stop = a QUIT file in its folder, which the
// stage looks for once a second and then quits cleanly. On a Mac a stage with its own folder runs in the
// background (no Dock icon, no window); a SHOW file opens its window until it is closed. On Linux the stages are
// child processes of the main hub inside its service: they stop with it and come back
// with it. On every system the main hub, at its start, starts the stages again that were started and not stopped
// by the operator; before an update replaces the program it stops them all, so they come back on the new one.
// Each stage writes hub_status.json (players, bytes per second) every few seconds for the list.
// Control path only.
struct StagePlayer {                 // a player on another stage, as that stage reports it
    std::string user, instrument;
    long long joinedMs = 0;          // wall clock, ms
    bool lan = false;
};
struct OtherStage {
    juce::File file;                 // its hub_settings.json
    std::string group;               // stage name from that file
    bool locked = false;             // it has a stage password
    int serverPort = 0, udpPort = 0, webPort = 0;
    bool running = false;
    bool stopping = false;           // QUIT written, not gone yet
    bool managed = false;            // its folder is under <default folder>/stages: Delete, not Forget
    int moveServerPort = 0;          // a lower free pair of the port row starts here; 0 = none
    int players = -1;                // from its status file while it runs; -1 = not known
    double inBytesPerSec = -1.0, outBytesPerSec = -1.0;
    int portState = -1;              // its port check: 1 open, 2 closed, 0 or 3 no verdict; -1 = not known
    int portRuns = 0;                // port checks it has finished since its start
    std::string portText;            // the check's own line
    std::string problem;             // the file cannot be read, or the like
    std::vector<StagePlayer> who;    // its players, from its status file while it runs
};
struct NewStage {
    std::string name, password, city, region;   // city + state / province / region, both needed
    std::string webPassword;         // empty = the main hub's web password
    bool listed = false;             // in the stage directory
};
bool canManageOtherStages();         // the main hub only
bool canStartOtherStages();
std::vector<OtherStage> otherStages();
bool addOtherStage(const juce::File& settingsFile, std::string& err);
bool forgetOtherStage(const juce::File& settingsFile, std::string& err);   // a stopped stage added by path leaves the list; its files stay
bool startOtherStage(const juce::File& settingsFile, std::string& err);
bool stopOtherStage(const juce::File& settingsFile, std::string& err);
bool showOtherStage(const juce::File& settingsFile, std::string& err, bool settings = false);   // Mac and Windows: SHOW file, the stage brings its window forward (at its Status, or its Settings tab)
bool showMainHub(std::string& err);   // a stage: SHOW file in the main hub's folder, its window comes forward at Other stages
int mainHubWebPort();                // a stage: the web page port of its main hub, 0 = none
// Makes the folder and settings of a new stage, puts it on the list and starts it. `made` = what it got.
bool createStage(const NewStage& in, OtherStage& made, std::string& err);
// Stops it if it runs, then moves its folder to stages-deleted (after the stop, in the background, when it was running).
bool deleteStage(const juce::File& settingsFile, std::string& err);
// What the operator opens for players, one line for the main stage and every listed stage: the port ranges to
// forward on the router, or the firewall command on Linux.
std::string stagesForwardHelp();
// Moves a listed stage to the lowest free pair of the port row (main stage's ports + 2, + 4, ...); a running stage restarts.
bool moveStagePorts(const juce::File& settingsFile, std::string& err);
int stageCores();                    // processor cores of this computer
int stagesRunning();                 // this hub and every other stage that runs
// Empty, or why `extra` more stages are more than this computer should run (a warning, never a refusal).
std::string stageLoadWarning(int extra);
void writeInstancePid();             // a stage with its own folder: hub.pid there, once it holds the folder lock
void registerWithMainHub();          // a stage with its own folder: onto the main hub's Other stages list (once, at start)
void writeStageStatus(int players, double inBytesPerSec, double outBytesPerSec,
                      int portState, int portRuns, const std::string& portText,
                      const std::vector<StagePlayer>& who);   // a stage: hub_status.json
// Asks a running stage to check its ports now (a CHECK file in its folder; the result comes with its status file).
bool checkStagePorts(const juce::File& settingsFile, std::string& err);
void stageQuitByOperator();          // a stage with its own folder, quit in its own window: not started again by the main hub
void restartStartedStages();         // main hub, at start: the stages that were started and not stopped run again
void stopChildStages();              // Linux main hub, at exit: its stages stop with it (they stay marked as started)
int stopStagesForUpdate();           // main hub, before the program is replaced: every stage stops, marked as started
// Crash watchdog of the main hub: a started stage that is gone without having stopped in order (no STOPPED file in
// its folder) is started again after 5 s, the second time within 10 minutes after 30 s; the third crash within 10
// minutes is left stopped ("crashed 3 times, not restarted") until Start is pressed. Control path, own thread.
void startStageWatchdog();
void stopStageWatchdog();
void pauseStageWatchdog(bool paused);
void noteStageStoppedInOrder();      // a stage, at every end in order: the file STOPPED in its folder

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

// Whole-line log writes. After this, std::cout and std::cerr collect each thread's text and pass complete lines to
// stdout in one write: lines from different threads cannot run into each other, and both streams share one buffer.
// Call once at the start of main().
void installLineLog();

// Range/sanity check. Returns an empty string when OK, else a human-readable
// description of the first problem. load() rejects files that fail this;
// the GUI refuses to Save them.
std::string validate(const HubLaunchArgs& a);
// "Public address" as typed: empty = fine, else why it cannot work (an address inside a private network).
// Checked when the operator saves; a value already in a settings file only gets a warning in the log.
std::string publicAddressProblem(const std::string& host);
// The stage's place is one stored text, "City, ST" ("Chicago, IL", "Toronto, ON", "Frankfurt, HE"); the forms show
// it as City and State / Province / Region (form keys location_city, location_region). Both are needed when the
// operator saves; a value already in a settings file is never rejected at load. For the United States and Canada
// (stage_country US / CA) the region is one of the two-letter codes; elsewhere any text.
void splitLocation(const std::string& location, std::string& city, std::string& region);
std::string joinLocation(const std::string& city, const std::string& region);
const char* regionCodes(const std::string& country);   // comma list for US and CA, else ""
void normaliseLocation(HubLaunchArgs& a);               // trims, upper-case code for US and CA
std::string locationProblem(const HubLaunchArgs& a);   // empty = fine, else what to enter

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

// Write only the stage password to the settings file (empty = no password); every other saved value stays as it is.
// `live` gets the new password too, so the next hub restart uses it. The running hub keeps its password until then.
bool saveStagePassword(const juce::File& f, HubLaunchArgs& live, const std::string& password, std::string& err);

// Canonical text form of one field (bools → "1"/"0", codec → "pcm"/"opus").
juce::String getValueString(const HubLaunchArgs& a, const std::string& key);
// Parse `text` into one field. Returns false (model untouched) on a parse or
// range error. Used by the GUI's text fields.
bool setValueFromString(HubLaunchArgs& a, const std::string& key, const juce::String& text);

} // namespace hub_settings
} // namespace sb_hub_gui
