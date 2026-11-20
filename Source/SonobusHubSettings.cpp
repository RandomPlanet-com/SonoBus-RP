// SPDX-License-Identifier: GPLv3-or-later WITH Appstore-exception
// Copyright (C) 2026 Jason Buecker (Random Planet)
// SonoBus RP — Hub Server settings persistence (see SonobusHubSettings.h).

#include "SonobusHubCompat.h"
#include "SonobusHubSettings.h"
#include <cctype>
#include "SonobusHubServer.h"

#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <random>
#include <variant>

#ifndef _WIN32
#include <signal.h>
#include <unistd.h>
#endif
#if defined(__APPLE__)
#include <fcntl.h>
#include <libproc.h>
#include <sys/wait.h>
#endif
#include <algorithm>
#include <chrono>
#include <functional>
#include <mutex>
#include <thread>
#if defined(__APPLE__)
#include <spawn.h>
#endif
#if !defined(_WIN32)
#include <fcntl.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
extern char** environ;
#endif

namespace sb_hub_gui {
namespace hub_settings {

// ─── Field table ──────────────────────────────────────────────────────────────
// One row per persisted key. `member` binds the JSON key to the model field;
// std::monostate marks the one special case (codec ↔ useOpus).
namespace {

using StrM  = std::string HubLaunchArgs::*;
using IntM  = int         HubLaunchArgs::*;
using DblM  = double      HubLaunchArgs::*;
using BoolM = bool        HubLaunchArgs::*;
using Member = std::variant<std::monostate, StrM, IntM, DblM, BoolM>;

struct Binding {
    FieldInfo info;
    Member member;
};

const std::vector<Binding>& bindings() {
    // The loader ignores keys it does not know, with an "unknown key" warning; Save drops them.
    static const std::vector<Binding> b = {
        // ── session & network ──
        { { "server_port",        "--server-port",                     Tier::Restart }, &HubLaunchArgs::localServerPort },
        { { "public_address",     "--public-address",                  Tier::Restart }, &HubLaunchArgs::publicAddressHost },
        { { "public_port",        "--public-port",                     Tier::Restart }, &HubLaunchArgs::publicAddressPort },
        { { "port_check_helper",  "--port-check-helper",               Tier::Restart }, &HubLaunchArgs::portCheckHelper },   
        { { "udp_port",           "--udp-port",                        Tier::Restart }, &HubLaunchArgs::udpPort },
        { { "group",              "--group",                           Tier::Restart }, &HubLaunchArgs::groupName },
        { { "group_password",     "--group-password",                  Tier::Restart }, &HubLaunchArgs::groupPassword },
        // ── audio ──
        { { "samplerate",         "--samplerate",                      Tier::Restart }, &HubLaunchArgs::sampleRate },
        { { "blocksize",          "--blocksize",                       Tier::Restart }, &HubLaunchArgs::blockSize },
        // ── fan-out & sink buffers ──
        { { "sink_buffer_ms",     "--sink-buffer",                     Tier::Restart }, &HubLaunchArgs::sinkBufferMs },
        { { "sink_buffer_lan_ms", "--sink-buffer-lan",                 Tier::Restart }, &HubLaunchArgs::sinkBufferLanMs },
        { { "pace_spread",        "--pace-spread",                     Tier::Restart }, &HubLaunchArgs::paceSpread },
        { { "redundancy_wan",     "--redundancy-wan",                  Tier::Restart }, &HubLaunchArgs::redundancyWan },
        // ── AOO sink env (read once per process by deps/aoo sink.cpp) ──
        { { "aoo_drain",          "--aoo-drain",                       Tier::Relaunch }, &HubLaunchArgs::aooDrain },
        { { "aoo_gov_arm_blocks", "--aoo-arm-blocks",                  Tier::Relaunch }, &HubLaunchArgs::aooGovArmBlocks },
        { { "aoo_slack_blocks",   "--aoo-slack-blocks",                Tier::Relaunch }, &HubLaunchArgs::aooSlackBlocks },
        { { "aoo_blockq_extra",   "--aoo-blockq-extra",                Tier::Relaunch }, &HubLaunchArgs::aooBlockqExtra },
        { { "aoo_handoff",        "--aoo-handoff",                     Tier::Relaunch }, &HubLaunchArgs::aooHandoff },
        // ── logging ──
        { { "log_file",           "--log-file",                        Tier::Live },    &HubLaunchArgs::logFile },
        // ── web status page ──
        { { "web_port",           "--web-port",                        Tier::Restart }, &HubLaunchArgs::webPort },
        { { "web_password",       "--web-password",                    Tier::Restart }, &HubLaunchArgs::webPassword },
        { { "location",           "--location",                        Tier::Live    }, &HubLaunchArgs::location },
        { { "geoip",              "--geoip",                           Tier::Live    }, &HubLaunchArgs::geoip },   
        // ── stage limits and the stage directory ──
        { { "max_players",        "--max-players",                     Tier::Live    }, &HubLaunchArgs::maxPlayers },
        { { "max_channels",       "--max-channels",                    Tier::Live    }, &HubLaunchArgs::maxChannels },
        { { "forget_players_days", "--forget-players-days",            Tier::Live    }, &HubLaunchArgs::forgetPlayersDays },
        { { "list_stage",         "--list-stage",                      Tier::Live    }, &HubLaunchArgs::listStage },
        { { "stage_genre",        "--stage-genre",                     Tier::Live    }, &HubLaunchArgs::stageGenre },
        { { "stage_description",  "--stage-description",               Tier::Live    }, &HubLaunchArgs::stageDescription },
        { { "stage_country",      "--stage-country",                   Tier::Live    }, &HubLaunchArgs::stageCountry },
        { { "stage_names",        "--stage-names",                     Tier::Live    }, &HubLaunchArgs::stageNames },
        { { "stage_directory",    "--stage-directory",                 Tier::Restart }, &HubLaunchArgs::stageDirectory },
        { { "stage_hub_id",       "--stage-hub-id",                    Tier::Restart }, &HubLaunchArgs::stageHubId },
        { { "stage_hub_key",      "--stage-hub-key",                   Tier::Restart }, &HubLaunchArgs::stageHubKey },
    };
    return b;
}

const Binding* findBinding(const std::string& key) {
    for (auto& b : bindings()) if (key == b.info.key) return &b;
    return nullptr;
}

// env name ↔ aoo_* key
struct EnvKey { const char* key; const char* env; int HubLaunchArgs::* member; };
const EnvKey kEnvKeys[] = {
    { "aoo_drain",          "SB_AOO_DRAIN",          &HubLaunchArgs::aooDrain },
    { "aoo_gov_arm_blocks", "SB_AOO_GOV_ARM_BLOCKS", &HubLaunchArgs::aooGovArmBlocks },
    { "aoo_slack_blocks",   "SB_AOO_SLACK_BLOCKS",   &HubLaunchArgs::aooSlackBlocks },
    { "aoo_blockq_extra",   "SB_AOO_BLOCKQ_EXTRA",   &HubLaunchArgs::aooBlockqExtra },
    { "aoo_handoff",        "SB_AOO_HANDOFF",        &HubLaunchArgs::aooHandoff },
};

bool parseBoolText(const juce::String& t, bool& out) {
    auto s = t.trim().toLowerCase();
    if (s == "1" || s == "true" || s == "yes" || s == "on")  { out = true;  return true; }
    if (s == "0" || s == "false" || s == "no" || s == "off") { out = false; return true; }
    return false;
}

bool varToBool(const juce::var& v, bool& out) {
    if (v.isBool())   { out = (bool)v; return true; }
    if (v.isInt() || v.isInt64() || v.isDouble()) { out = ((int)v) != 0; return true; }
    if (v.isString()) return parseBoolText(v.toString(), out);
    return false;
}

bool varToInt(const juce::var& v, int& out) {
    if (v.isInt() || v.isInt64() || v.isDouble() || v.isBool()) { out = (int)v; return true; }
    if (v.isString()) {
        auto s = v.toString().trim();
        if (s.isEmpty() || !s.containsOnly("-0123456789")) return false;
        out = s.getIntValue(); return true;
    }
    return false;
}

bool varToDouble(const juce::var& v, double& out) {
    if (v.isInt() || v.isInt64() || v.isDouble()) { out = (double)v; return true; }
    if (v.isString()) {
        auto s = v.toString().trim();
        if (s.isEmpty() || !s.containsOnly("-+.0123456789eE")) return false;
        out = s.getDoubleValue(); return true;
    }
    return false;
}

} // namespace

const std::vector<FieldInfo>& fields() {
    static const std::vector<FieldInfo> f = [] {
        std::vector<FieldInfo> out;
        for (auto& b : bindings()) out.push_back(b.info);
        return out;
    }();
    return f;
}

const std::vector<FormSpec>& formSpecs() {
    static const std::vector<FormSpec> f = {
        { 0, nullptr,              "Session & network",                 0, nullptr },
        { 0, "server_port",        "Server port", 0, nullptr },
        { 0, "public_address",     "Public address",      0, nullptr },
        { 0, "public_port",        "Public port",                       0, nullptr },
        { 0, "udp_port",           "Hub audio UDP port",                0, nullptr },
        { 0, "group",              "Stage name",                        0, nullptr },
        { 0, "group_password",     "Stage password",                    3, nullptr },
        { 0, nullptr,              "Stage limits",                      0, nullptr },
        { 0, "max_players",        "Most players",                      0, nullptr },
        { 0, "max_channels",       "Most channels per player",          0, nullptr },
        { 0, "forget_players_days", "Forget players not seen for (days)", 0, nullptr },
        { 0, nullptr,              "Stage directory",                   0, nullptr },
        { 0, "list_stage",         "List this stage",                   1, nullptr },
        { 0, "location_city",      "City",                              0, nullptr },
        { 0, "location_region",    "State / Province / Region",         0, nullptr },
        { 0, "stage_country",      "Country (two letters)",             0, nullptr },
        { 0, "stage_genre",        "Genre",                             2, "Any,Rock/Pop,Jazz/Blues,Funk/Soul/R&B,Country/Folk,Classical/Choral,Electronic,Worship" },
        { 0, "stage_description",  "Description",                       0, nullptr },
        { 0, "stage_names",        "Player names in the listing",       2, "Automatic,Show,Hide" },

        // The right column: the ordinary sections first, the advanced group last.
        { 1, nullptr,              "Audio",                             0, nullptr },
        { 1, "samplerate",         "Sample rate (Hz)",                  2, "44100,48000,88200,96000" },
        { 1, "blocksize",          "Block size (samples)",              2, "16,32,64,128,256" },
        { 1, nullptr,              "Logging",                           0, nullptr },
        { 1, "log_file",           "Log file",                     0, nullptr },
        { 1, nullptr,              "Web page",                          0, nullptr },   
        { 1, "web_port",           "Web page port",    0, nullptr },
        { 1, "web_password",       "Web password", 3, nullptr },
        { 1, "geoip",              "Player locations", 1, nullptr },   

        { 1, nullptr,              "For advanced users only",           0, nullptr,
          "Change these only if you know why. The defaults are right for almost every stage." },
        // No WAN sink buffer row: WAN players start at the lowest hold and are measured.
        { 1, "sink_buffer_lan_ms", "LAN sink buffer (ms)", 0, nullptr },
        { 1, "pace_spread",        "Pacer spread (%)",  0, nullptr },
        { 1, "redundancy_wan",     "WAN redundancy (copies)",           2, "1,2,3,4" },
        { 1, "aoo_drain",          "Depth governor",            1, nullptr },
        { 1, "aoo_gov_arm_blocks", "Governor arm band (blocks)", 0, nullptr },
        { 1, "aoo_slack_blocks",   "Slack above pin (blocks)",          0, nullptr },
        { 1, "aoo_blockq_extra",   "Blockqueue extra (blocks)",         0, nullptr },
        { 1, "aoo_handoff",        "Lead hand-off",                     1, nullptr },
    };
    return f;
}

// The stage's place is kept as one text, "City, ST"; the forms show it as two fields.
static bool isLocationPart(const std::string& key) { return key == "location_city" || key == "location_region"; }

Tier tierForKey(const std::string& key) {
    if (isLocationPart(key)) return Tier::Live;
    if (auto* b = findBinding(key)) return b->info.tier;
    return Tier::Restart;   // unknown → conservative
}

static juce::File homeDir() { return juce::File::getSpecialLocation(juce::File::userHomeDirectory); }

static juce::File& instanceDirRef() { static juce::File d; return d; }
static juce::File& instanceFileRef() { static juce::File f; return f; }   // its settings file (for the main hub's list)

static juce::File defaultDataDir() {
#if JUCE_MAC
    return homeDir().getChildFile("Library/Application Support/SonoBus RP Hub");
#elif JUCE_WINDOWS
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory).getChildFile("SonoBus RP Hub");
#else
    return homeDir().getChildFile(".config/SonoBus RP Hub");
#endif
}

juce::File dataDir() {
    if (instanceDirRef() != juce::File()) return instanceDirRef();
    return defaultDataDir();
}

// A hub is a STAGE of a main hub when (1) its settings file sits in that hub's <data folder>/stages/<name>/, or
// (2) its folder holds the marker file MAIN_HUB with the main hub's data folder (written when a main hub takes a
// stage that lives elsewhere onto its list), or (3) on a Mac or Windows computer, where one main hub with a window
// runs the others, it was started with a settings file outside the default folder while the default folder has a
// hub. Every other hub is a main hub, also one that a service starts with its own settings file: its stages live in
// the folder of that file.
static juce::File& stageOwnerRef() { static juce::File d; return d; }
static const char* const kStageMarker = "MAIN_HUB";
static juce::File markedMainHub(const juce::File& stageDir) {
    const juce::String marked = stageDir.getChildFile(kStageMarker).loadFileAsString().trim();
    if (marked.isEmpty() || !juce::File::isAbsolutePath(marked)) return {};
    const juce::File d(marked);
    return d.isDirectory() && d != stageDir ? d : juce::File();
}
static juce::File findStageOwner(const juce::File& settingsFile) {
    const juce::File dir = settingsFile.getParentDirectory();
    if (dir.getParentDirectory().getFileName() == "stages") return dir.getParentDirectory().getParentDirectory();
    if (const juce::File d = markedMainHub(dir); d != juce::File()) return d;
#if defined(__APPLE__) || defined(_WIN32)
    if (defaultDataDir().getChildFile("hub_settings.json").existsAsFile()) return defaultDataDir();
#endif
    return {};
}

bool useSettingsFolderAsDataDir(const juce::File& settingsFile) {
    const juce::File dir = settingsFile.getParentDirectory();
    if (settingsFile == juce::File() || dir == defaultDataDir()) return false;   // default folder: one hub per machine
    instanceDirRef() = dir;
    instanceFileRef() = settingsFile;
    stageOwnerRef() = findStageOwner(settingsFile);
    return true;
}

bool hasOwnDataDir() { return instanceDirRef() != juce::File(); }
bool isStage() { return stageOwnerRef() != juce::File(); }
juce::File stageMainFolder() { return stageOwnerRef(); }
// The data folder of the main hub: this hub's own, or its main hub's when this is a stage.
static juce::File mainDataDir() { return isStage() ? stageOwnerRef() : dataDir(); }

juce::File defaultFile() { return dataDir().getChildFile("hub_settings.json"); }

static juce::File machineLogFile() {
#if JUCE_MAC
    return homeDir().getChildFile("Library/Logs/SonoBus RP Hub/hub.log");
#else
    return defaultDataDir().getChildFile("hub.log");
#endif
}

juce::File defaultLogFile() {
    if (hasOwnDataDir()) return dataDir().getChildFile("hub.log");   // each instance logs into its own folder
    return machineLogFile();
}

void applyDefaults(HubLaunchArgs& a) {
    if (a.logFile.find("/SonoBus-Plus/hub/") != std::string::npos) a.logFile.clear();   // that folder has moved
    // A settings file copied from the main hub names the main log; an instance with its own folder never shares it.
    if (hasOwnDataDir() && juce::File::isAbsolutePath(juce::String::fromUTF8(a.logFile.c_str()))
        && juce::File(juce::String::fromUTF8(a.logFile.c_str())) == machineLogFile()) a.logFile.clear();
    if (a.logFile.empty()) a.logFile = defaultLogFile().getFullPathName().toStdString();
}

// One-time move from the folder older hubs used.
void migrateOldDataDir() {
    const juce::File oldDir = homeDir().getChildFile("SonoBus-Plus").getChildFile("hub");
    const juce::File newDir = defaultDataDir();
    if (!oldDir.isDirectory() || newDir.exists()) return;
    newDir.getParentDirectory().createDirectory();
    // A hub running from inside the old folder must not move itself, so only its data goes.
    if (juce::File::getSpecialLocation(juce::File::currentExecutableFile).isAChildOf(oldDir)) {
        newDir.createDirectory();
        for (const char* name : { "hub_settings.json", "hub_taps.conf", "hub_sessions.json", "stats.csv" })
            if (const auto f = oldDir.getChildFile(name); f.existsAsFile()) f.moveFileTo(newDir.getChildFile(name));
        return;
    }
    if (!oldDir.moveFileTo(newDir)) {
        std::cerr << "[HUB-SETTINGS] could not move " << oldDir.getFullPathName() << " to " << newDir.getFullPathName() << "\n";
        return;
    }
    const juce::File oldRoot = oldDir.getParentDirectory();
    if (oldRoot.getNumberOfChildFiles(juce::File::findFilesAndDirectories) == 0) oldRoot.deleteRecursively();
}

const char* tipFor(const std::string& key) {
    static const std::map<std::string, const char*> tips = {
        { "server_port",        "The port players connect to. Forward it on your router for players outside your network." },
        { "public_address",     "The address outside players use to reach this hub. Detected automatically. Type a domain or address only to override it." },
        { "public_port",        "The outside port for hub audio. Normally the same as the hub audio UDP port." },
        { "udp_port",           "The UDP port that carries audio. Forward it on your router for players outside your network." },
        { "group",              "The name of the stage players join." },
        { "group_password",     "Optional. Players must enter this password to join. A listed stage with a password shows as locked." },
        { "max_players",        "The most players this stage takes at once. 0 = no limit. Each player's audio goes to every other player, so the hub's upload grows quickly with the player count." },
        { "forget_players_days", "Previous players not seen for this many days leave the list. 0 = never (the list keeps the last 1000 players). Removed players stay removed." },
        { "max_channels",       "The most channels one player may send. A stereo pair counts as 2. Extra channels are not passed on. 0 = no limit." },
        { "list_stage",         "Shows this stage at stages.randomplanet.com so players can find it. Its name, genre, description, city and this hub's public address are shown. Player names are sent to the directory; on a locked stage only the directory's operator sees them. Needs the city, and the ports forwarded." },
        { "stage_genre",        "The genre shown in the stage directory." },
        { "stage_description",  "One line shown in the stage directory, up to 160 characters." },
        { "stage_country",      "The two-letter country code shown in the stage directory, for example US, CA, GB or DE. Players can sort and filter the stage list by it." },
        { "stage_names",        "Who sees player names in the stage directory. Automatic shows them when the stage has no password and hides them from the public when it has one. The directory's operator always sees them." },
        { "samplerate",         "The sample rate of the session. Every player follows the hub." },
        { "blocksize",          "Audio block size. Smaller blocks mean lower latency and a heavier load on every machine." },
        { "log_file",           "Where the hub writes its log. It starts a new file when this one reaches 50 MB." },
        { "web_port",           "Port for the hub's web page. 0 turns the page off. The page also needs a Web password." },
        { "web_password",       "Password for the hub's web page. Without one the web page stays off." },
        { "location",           "The stage's city, shown on the web page and in the stage directory. Required to list the stage." },
        { "location_city",      "The stage's city, shown on the web page and in the stage directory." },
        { "location_region",    "The state, province or region of that city. For the United States and Canada the two-letter code, for example GA or ON; elsewhere as you write it, for example HE or Hessen." },
        { "geoip",              "Shows each outside player's city and state. Their public IP address is sent to ip-api.com for the lookup." },
        { "sink_buffer_lan_ms", "How much audio the hub holds for each player on your local network. Blank uses the default of 1.33 ms." },
        { "pace_spread",        "Spreads the hub's outgoing packets across this share of each audio block." },
        { "redundancy_wan",     "How many copies of each packet go to outside players. More copies survive loss but use more bandwidth." },
        { "aoo_drain",          "Removes extra buffered audio when a player's buffer grows, which keeps latency down." },
        { "aoo_gov_arm_blocks", "How far a buffer may rise before the depth governor acts. 0 is automatic." },
        { "aoo_slack_blocks",   "Extra blocks the depth governor allows above the target." },
        { "aoo_blockq_extra",   "Extra capacity in the hub's receive queue, in blocks." },
        { "aoo_handoff",        "Lets the hub pass audio on as soon as it arrives, without waiting for the next block." },
    };
    auto it = tips.find(key);
    return it == tips.end() ? "" : it->second;
}

// ─── location: "City, ST" ─────────────────────────────────────────────────────
void splitLocation(const std::string& location, std::string& city, std::string& region) {
    const juce::String l = juce::String::fromUTF8(location.c_str()).trim();
    const int comma = l.lastIndexOfChar(',');
    city = (comma < 0 ? l : l.substring(0, comma)).trim().toStdString();
    region = comma < 0 ? std::string() : l.substring(comma + 1).trim().toStdString();
}
std::string joinLocation(const std::string& cityIn, const std::string& regionIn) {
    const juce::String city = juce::String::fromUTF8(cityIn.c_str()).replaceCharacter(',', ' ').trim();   // the comma divides the two parts
    const juce::String region = juce::String::fromUTF8(regionIn.c_str()).replaceCharacter(',', ' ').trim();
    return (region.isEmpty() ? city : city + ", " + region).toStdString();
}
const char* regionCodes(const std::string& country) {
    const juce::String c = juce::String(country).trim().toUpperCase();
    if (c == "US") return "AL,AK,AZ,AR,CA,CO,CT,DE,DC,FL,GA,HI,ID,IL,IN,IA,KS,KY,LA,ME,MD,MA,MI,MN,MS,MO,MT,NE,NV,NH,NJ,NM,NY,NC,ND,OH,OK,OR,PA,RI,SC,SD,TN,TX,UT,VT,VA,WA,WV,WI,WY,PR,GU,VI,AS,MP";
    if (c == "CA") return "AB,BC,MB,NB,NL,NS,NT,NU,ON,PE,QC,SK,YT";
    return "";
}
void normaliseLocation(HubLaunchArgs& a) {
    std::string city, region;
    splitLocation(a.location, city, region);
    if (regionCodes(a.stageCountry)[0] != 0) region = juce::String::fromUTF8(region.c_str()).toUpperCase().toStdString();
    a.location = joinLocation(city, region);
}
std::string locationProblem(const HubLaunchArgs& a) {
    std::string city, region;
    splitLocation(a.location, city, region);
    if (city.empty()) return "Enter the city";
    if (region.empty()) return "Enter the state, province or region";
    const juce::String codes(regionCodes(a.stageCountry));
    if (codes.isNotEmpty() && !juce::StringArray::fromTokens(codes, ",", "").contains(juce::String::fromUTF8(region.c_str()).toUpperCase()))
        return juce::String(a.stageCountry).trim().toUpperCase() == "US" ? "State: enter the two-letter code, for example GA"
                                                                        : "Province: enter the two-letter code, for example ON";
    return {};
}

// ─── string accessors ─────────────────────────────────────────────────────────
juce::String getValueString(const HubLaunchArgs& a, const std::string& key) {
    if (isLocationPart(key)) {
        std::string city, region;
        splitLocation(a.location, city, region);
        return juce::String::fromUTF8(key == "location_city" ? city.c_str() : region.c_str());
    }
    auto* b = findBinding(key);
    if (!b) return {};
    if (std::holds_alternative<std::monostate>(b->member)) return a.useOpus ? "opus" : "pcm";
    return std::visit([&a](auto m) -> juce::String {
        using M = decltype(m);
        if constexpr (std::is_same_v<M, StrM>)  return juce::String(a.*m);
        else if constexpr (std::is_same_v<M, IntM>)  return juce::String(a.*m);
        else if constexpr (std::is_same_v<M, DblM>)  return juce::String(a.*m, 3).trimCharactersAtEnd("0").trimCharactersAtEnd(".");
        else if constexpr (std::is_same_v<M, BoolM>) return (a.*m) ? "1" : "0";
        else return juce::String();
    }, b->member);
}

bool setValueFromString(HubLaunchArgs& a, const std::string& key, const juce::String& text) {
    if (isLocationPart(key)) {
        std::string city, region;
        splitLocation(a.location, city, region);
        const std::string t = text.replaceCharacter(',', ' ').trim().toStdString();
        if (key == "location_city") city = t; else region = t;
        a.location = city.empty() && !region.empty() ? ", " + region : joinLocation(city, region);   // a region typed before the city is kept
        return true;
    }
    auto* b = findBinding(key);
    if (!b) return false;
    if (std::holds_alternative<std::monostate>(b->member)) {
        auto s = text.trim().toLowerCase();
        if (s == "pcm")  { a.useOpus = false; return true; }
        if (s == "opus") { a.useOpus = true;  return true; }
        return false;
    }
    return std::visit([&a, &text](auto m) -> bool {
        using M = decltype(m);
        if constexpr (std::is_same_v<M, StrM>) { a.*m = text.trim().toStdString(); return true; }
        else if constexpr (std::is_same_v<M, IntM>) {
            auto s = text.trim();
            // A BLANK field means "the default", never 0 (0 in the LAN sink field = "use WAN")
            if (s.isEmpty()) { a.*m = HubLaunchArgs{}.*m; return true; }
            if (!s.containsOnly("-0123456789")) return false;
            a.*m = s.getIntValue(); return true;
        }
        else if constexpr (std::is_same_v<M, DblM>) {
            auto s = text.trim();
            if (s.isEmpty()) { a.*m = HubLaunchArgs{}.*m; return true; }   // blank = default (e.g. LAN sink 1.33)
            if (!s.containsOnly("-+.0123456789eE")) return false;
            a.*m = s.getDoubleValue(); return true;
        }
        else if constexpr (std::is_same_v<M, BoolM>) { bool v; if (!parseBoolText(text, v)) return false; a.*m = v; return true; }
        else return false;
    }, b->member);
}

std::string publicAddressProblem(const std::string& hostRaw) {
    const std::string host = juce::String::fromUTF8(hostRaw.c_str()).trim().toStdString();
    if (host.empty()) return {};
    int a = 0, b = 0, c = 0, d = 0; char extra = 0;
    const bool literal = std::sscanf(host.c_str(), "%d.%d.%d.%d%c", &a, &b, &c, &d, &extra) == 4
                         && a >= 0 && a <= 255 && b >= 0 && b <= 255 && c >= 0 && c <= 255 && d >= 0 && d <= 255;
    const bool inside = literal ? (a == 10 || (a == 172 && b >= 16 && b <= 31) || (a == 192 && b == 168) || a == 127 || (a == 169 && b == 254) || a == 0)
                                : juce::String(host).equalsIgnoreCase("localhost");
    if (!inside) return {};
    return "Public address " + host + " is an address inside a private network, so players outside your network cannot reach it. "
           "Leave the field empty (the hub finds its public address itself) or enter your public internet address or domain name.";
}

std::string validate(const HubLaunchArgs& a) {
    auto port = [](int p, const char* what, bool allowZero) -> std::string {
        if (p < (allowZero ? 0 : 1) || p > 65535) return std::string(what) + " must be " + (allowZero ? "0..65535" : "1..65535");
        return {};
    };
    std::string e;
    if (!(e = port(a.serverPort, "port", false)).empty()) return e;
    if (!(e = port(a.localServerPort, "server_port", false)).empty()) return e;
    if (!(e = port(a.udpPort, "udp_port", true)).empty()) return e;
    if (!(e = port(a.publicAddressPort, "public_port", true)).empty()) return e;
    if (!(e = port(a.webPort, "web_port", true)).empty()) return e;   // 0 = web page off
    if (a.groupName.empty()) return "group must not be empty";
    if (a.userName.empty()) return "user must not be empty";
    if (a.sampleRate < 8000 || a.sampleRate > 192000) return "samplerate must be 8000..192000";
    if (a.blockSize < 1 || a.blockSize > 4096) return "blocksize must be 1..4096";
    if (a.paceSpread < 5 || a.paceSpread > 95) return "pace_spread must be 5..95 (percent of block period)";
    if (a.paceMinSlotUs < 0 || a.paceMinSlotUs > 500) return "pace_min_slot_us must be 0..500";
    if (a.channels < 1 || a.channels > HUB_MAX_CHANNELS_PER_PEER) return "channels must be 1.." + std::to_string(HUB_MAX_CHANNELS_PER_PEER);
    if (a.opusBitrate < 6000) return "opus_bitrate must be >= 6000";
    if (!(a.sinkBufferMs > 0.0)) return "sink_buffer_ms must be > 0";
    if (a.bundleMtu < 256 || a.bundleMtu > 65507) return "bundle_mtu must be 256..65507";
    if (a.redundancyLan < 1 || a.redundancyLan > 4) return "redundancy_lan must be 1..4";
    if (a.redundancyWan < 1 || a.redundancyWan > 4) return "redundancy_wan must be 1..4";
    if (a.aooDrain < 0 || a.aooDrain > 1) return "aoo_drain must be 0 or 1";
    if (a.aooHandoff < 0 || a.aooHandoff > 1) return "aoo_handoff must be 0 or 1";
    if (a.aooGovArmBlocks < 0 || a.aooGovArmBlocks > 64) return "aoo_gov_arm_blocks must be 0..64 (0 = sink auto)";
    if (a.maxPlayers < 0 || a.maxPlayers > 999) return "Most players must be 0..999 (0 = no limit)";
    if (a.forgetPlayersDays < 0 || a.forgetPlayersDays > 3650) return "Forget players must be 0..3650 days (0 = never)";
    if (a.maxChannels < 0 || a.maxChannels > HUB_MAX_CHANNELS_PER_PEER) return "Most channels per player must be 0.." + std::to_string(HUB_MAX_CHANNELS_PER_PEER) + " (0 = no limit)";
    if (a.stageDescription.size() > 160) return "Description must be 160 characters or fewer";
    if (!a.stageCountry.empty() && !(a.stageCountry.size() == 2 && std::isalpha((unsigned char) a.stageCountry[0]) && std::isalpha((unsigned char) a.stageCountry[1])))
        return "Country must be a two-letter code, for example US";
    if (a.stageNames != "Automatic" && a.stageNames != "Show" && a.stageNames != "Hide") return "Player names in the listing must be Automatic, Show or Hide";
    if (a.listStage && juce::String::fromUTF8(a.location.c_str()).trim().isEmpty()) return "Enter the city to list this stage";
    if (a.aooSlackBlocks < 0 || a.aooSlackBlocks > 64) return "aoo_slack_blocks must be 0..64";
    if (a.aooBlockqExtra < 1 || a.aooBlockqExtra > 256) return "aoo_blockq_extra must be 1..256";
    return {};
}

bool saveStagePassword(const juce::File& f, HubLaunchArgs& live, const std::string& password, std::string& err) {
    const std::string pw = juce::String::fromUTF8(password.c_str()).trim().toStdString();
    HubLaunchArgs m = live;   // no usable file yet: save what is running
    if (f.existsAsFile()) {
        HubLaunchArgs fileModel;
        std::string e;
        if (load(f, fileModel, e)) { applyDefaults(fileModel); m = fileModel; }
    }
    m.groupPassword = pw;
    if (!save(f, m, err)) return false;
    live.groupPassword = pw;
    return true;
}

std::vector<std::string> diffKeys(const HubLaunchArgs& x, const HubLaunchArgs& y) {
    std::vector<std::string> out;
    for (auto& b : bindings())
        if (getValueString(x, b.info.key) != getValueString(y, b.info.key)) out.push_back(b.info.key);
    return out;
}

// ─── JSON ─────────────────────────────────────────────────────────────────────
juce::String toJson(const HubLaunchArgs& a) {
    auto* obj = new juce::DynamicObject();
    juce::var root(obj);   // takes ownership (ReferenceCountedObject)
    obj->setProperty("_schema", 1);
    obj->setProperty("_comment", "SonoBus RP hub settings. Precedence: CLI args > launch env (aoo_*) > this file > built-in defaults. "
                                 "Edit from the hub GUI Settings tab (Save / Save & Restart hub) or by hand; see `sonobus-hub --help`.");
    for (auto& b : bindings()) {
        const juce::Identifier id(b.info.key);
        if (std::holds_alternative<std::monostate>(b.member)) { obj->setProperty(id, a.useOpus ? "opus" : "pcm"); continue; }
        std::visit([&](auto m) {
            using M = decltype(m);
            if constexpr (std::is_same_v<M, StrM>)       obj->setProperty(id, juce::String(a.*m));
            else if constexpr (std::is_same_v<M, IntM>)  obj->setProperty(id, (int)(a.*m));
            else if constexpr (std::is_same_v<M, DblM>)  obj->setProperty(id, (double)(a.*m));
            else if constexpr (std::is_same_v<M, BoolM>) obj->setProperty(id, (bool)(a.*m));
        }, b.member);
    }
    return juce::JSON::toString(root, false);
}

bool load(const juce::File& f, HubLaunchArgs& a, std::string& err) {
    err.clear();
    if (f == juce::File() || !f.existsAsFile()) return false;
    juce::var root;
    {
        auto text = f.loadFileAsString();
        auto res = juce::JSON::parse(text, root);
        if (res.failed()) { err = "parse error: " + res.getErrorMessage().toStdString(); return false; }
    }
    auto* obj = root.getDynamicObject();
    if (!obj) { err = "top level is not a JSON object"; return false; }

    // Parse into a scratch copy so a bad value leaves `a` untouched.
    HubLaunchArgs tmp = a;
    std::string bad;
    for (auto& b : bindings()) {
        const juce::Identifier id(b.info.key);
        if (!obj->hasProperty(id)) continue;
        const juce::var& v = obj->getProperty(id);
        bool ok = true;
        if (std::holds_alternative<std::monostate>(b.member)) {
            ok = setValueFromString(tmp, b.info.key, v.toString());
        } else {
            ok = std::visit([&](auto m) -> bool {
                using M = decltype(m);
                if constexpr (std::is_same_v<M, StrM>)       { tmp.*m = v.toString().toStdString(); return true; }
                else if constexpr (std::is_same_v<M, IntM>)  { int x; if (!varToInt(v, x)) return false; tmp.*m = x; return true; }
                else if constexpr (std::is_same_v<M, DblM>)  { double x; if (!varToDouble(v, x)) return false; tmp.*m = x; return true; }
                else if constexpr (std::is_same_v<M, BoolM>) { bool x; if (!varToBool(v, x)) return false; tmp.*m = x; return true; }
                else return false;
            }, b.member);
        }
        if (!ok) { if (!bad.empty()) bad += ", "; bad += b.info.key; }
    }
    // The port checker has no user setting, so a saved address from an older version must not pin it.
    tmp.portCheckHelper = HubLaunchArgs().portCheckHelper;
    // Unknown keys: warn (typo guard), never fatal.
    for (auto& nv : obj->getProperties()) {
        auto k = nv.name.toString();
        if (k.startsWith("_") || findBinding(k.toStdString())) continue;
        std::cerr << "[HUB-SETTINGS] unknown key '" << k << "' in " << f.getFullPathName() << " (ignored)\n";
    }
    if (!bad.empty()) { err = "bad value for: " + bad; return false; }
    if (auto problem = validate(tmp); !problem.empty()) { err = "invalid: " + problem; return false; }
    a = tmp;
    return true;
}

bool save(const juce::File& f, const HubLaunchArgs& a, std::string& err) {
    err.clear();
    if (f == juce::File()) { err = "settings file disabled (--no-settings)"; return false; }
    auto dir = f.getParentDirectory();
    if (!dir.exists()) {
        auto r = dir.createDirectory();
        if (r.failed()) { err = "cannot create " + dir.getFullPathName().toStdString() + ": " + r.getErrorMessage().toStdString(); return false; }
    }
    // Atomic-ish: write a temp sibling then move over (replaceFileIn).
    juce::TemporaryFile tmp(f);
    if (!tmp.getFile().replaceWithText(toJson(a) + "\n")) { err = "write failed: " + tmp.getFile().getFullPathName().toStdString(); return false; }
    if (!tmp.overwriteTargetFileWithTemporary()) { err = "rename failed: " + f.getFullPathName().toStdString(); return false; }
    return true;
}

// ─── SB_AOO_* env export ──────────────────────────────────────────────────────
void applySinkEnv(HubLaunchArgs& a, const std::set<std::string>& cliKeys, std::set<std::string>& envKeys) {
    for (auto& ek : kEnvKeys) {
        const bool cliPinned = cliKeys.count(ek.key) > 0;
        const char* cur = getenv(ek.env);
        if (!cliPinned && cur && cur[0]) {
            // Launch env wins over the settings file (mirror the CLI rule);
            // reflect it into the model so the GUI shows the effective value.
            a.*(ek.member) = atoi(cur);
            envKeys.insert(ek.key);
            fprintf(stdout, "[HUB-SETTINGS] %s=%s (launch env)\n", ek.env, cur);
            continue;
        }
        const std::string v = std::to_string(a.*(ek.member));
        setenv(ek.env, v.c_str(), 1);
        fprintf(stdout, "[HUB-SETTINGS] %s=%s (%s)\n", ek.env, v.c_str(), cliPinned ? "cli" : "settings");
    }
}

void clearSinkEnv() {
    for (auto& ek : kEnvKeys) unsetenv(ek.env);
}

// ─── hub application ──────────────────────────────────────────────────────────
void applyToHub(SonobusHubServer& hub, const HubLaunchArgs& a) {
    hub.setOpusEnabled(a.useOpus);
    hub.setOpusBitrate(a.opusBitrate);
    hub.setRunLocalServer(true, a.localServerPort);   // Always the built-in server
    if (!a.publicAddressHost.empty()) hub.setPublicAddressOverride(a.publicAddressHost, a.publicAddressPort);
    if (const std::string problem = publicAddressProblem(a.publicAddressHost); !problem.empty()) std::cerr << "[HUB-SETTINGS] WARNING: " + problem + "\n";
    hub.setPortCheckHelper(a.portCheckHelper);   
    if (!a.lanSubnetCidr.empty()) hub.setLanSubnet(a.lanSubnetCidr);
    hub.setSinkBufferMs(a.sinkBufferMs);          // Latency tuning
    hub.setSinkBufferLanMs(a.sinkBufferLanMs);
    hub.setSendPacing(a.sendPace);                // Paced fan-out
    hub.setPaceDither(a.paceDither);
    hub.setPaceSpreadPct(a.paceSpread);
    hub.setPaceMinSlotUs(a.paceMinSlotUs);
    hub.setCutThrough(a.cutThrough);              // Cut-through emission
    hub.setInlineListeners(a.inlineListeners);
    hub.setBundling(a.bundle);
    hub.setBundleMtu(a.bundleMtu);
    hub.setRedundancy(a.redundancyLan, a.redundancyWan);   // Per-peer redundancy
    hub.setDscp(a.dscp);
    hub.setConnectedSockets(a.connectedSockets);   // Per-listener connected send sockets
    if (!a.dumpPeer.empty()) hub.setDumpPeer(a.dumpPeer, a.dumpSecs > 0 ? a.dumpSecs : 30);
    hub.setSessionSettingsSnapshot(sessionSnapshot(a));   // Recent sessions remember the settings in force
    applyStageToHub(hub, a);
}

void applyStageToHub(SonobusHubServer& hub, const HubLaunchArgs& a) {
    SonobusHubServer::StageSettings st;
    st.maxPlayers = a.maxPlayers;
    st.maxChannels = a.maxChannels;
    st.list = a.listStage;
    st.genre = a.stageGenre.empty() ? "Any" : a.stageGenre;
    st.description = a.stageDescription;
    st.country = juce::String::fromUTF8(a.stageCountry.c_str()).trim().toUpperCase().toStdString();
    st.city = juce::String::fromUTF8(a.location.c_str()).trim().toStdString();
    st.names = a.stageNames == "Show" ? 1 : a.stageNames == "Hide" ? 2 : 0;
    hub.applyStageSettings(st);
    hub.setPreviousPlayersForgetDays(a.forgetPlayersDays);
}

const char* stageGenres() { return "Any,Rock/Pop,Jazz/Blues,Funk/Soul/R&B,Country/Folk,Classical/Choral,Electronic,Worship"; }

bool ensureStageIdentity(HubLaunchArgs& a) {
    if (!a.stageHubId.empty() && a.stageHubKey.size() >= 16) return false;
    if (a.stageHubId.empty()) a.stageHubId = juce::Uuid().toDashedString().toStdString();
    std::random_device rd;   // the system's random source
    const char* abc = "abcdefghijkmnopqrstuvwxyzABCDEFGHJKLMNPQRSTUVWXYZ23456789";
    std::string k;
    for (int i = 0; i < 40; ++i) k += abc[rd() % 57];
    a.stageHubKey = k;
    return true;
}

// ─── recent sessions ────────────────────────────────────
std::vector<std::pair<std::string, std::string>> sessionSnapshot(const HubLaunchArgs& a) {
    std::vector<std::pair<std::string, std::string>> out;
    for (auto& f : fields()) {
        const std::string k = f.key;
        if (k == "stage_hub_id" || k == "stage_hub_key" || k == "stage_directory") continue;   // identity, not a session setting
        out.emplace_back(f.key, getValueString(a, f.key).toStdString());
    }
    return out;
}

bool applySession(HubLaunchArgs& a, const std::string& group, const std::string& password,
                  const std::vector<std::pair<std::string, std::string>>& settings, int& applied, std::string& err) {
    err.clear(); applied = 0;
    if (group.empty()) { err = "session has no group name"; return false; }
    HubLaunchArgs n = a;   // scratch copy: a bad value leaves `a` untouched
    for (const auto& kv : settings) {
        const std::string& key = kv.first;
        bool inForm = false;
        for (const auto& sp : formSpecs()) if (sp.key && key == sp.key) { inForm = true; break; }
        if (!inForm) continue;                                        // retired/unknown key from an old file
        if (key == "group" || key == "group_password") continue;      // set explicitly below
        // Host INFRASTRUCTURE keys are not part of a session — applying
        // an old session must not put the hub's old public address / ports / web page / log path back.
        if (key == "public_address" || key == "public_port" || key == "server_port" || key == "udp_port" || key == "web_port"
            || key == "web_password" || key == "port_check_helper" || key == "log_file" || key == "location" || key == "geoip") continue;
        if (key == "web_password" && kv.second.empty()) continue;     // never lock the page out with a blank stored password
        if (key == "web_port" && kv.second == "0" && a.webPort > 0) continue;   // a session that ran without the page must not switch it off under the operator
        if (!setValueFromString(n, key, juce::String::fromUTF8(kv.second.c_str()))) { err = "bad value for " + key + ": '" + kv.second + "'"; return false; }
        ++applied;
    }
    n.groupName = group;
    n.groupPassword = password;
    if (auto problem = validate(n); !problem.empty()) { err = "invalid: " + problem; return false; }
    a = n;
    return true;
}

void resolveServerEndpoint(const HubLaunchArgs& a, std::string& host, int& port) {
    // The built-in AOO server is the only supported mode —
    // the hub's own client always dials loopback on the server port.
    host = "127.0.0.1";
    port = a.localServerPort;
}

void rotateLogIfLarge(const std::string& path) {
    constexpr int64_t kMaxLogBytes = 50 * 1024 * 1024;
    const juce::File logFile(juce::String::fromUTF8(path.c_str()));
    if (logFile.getSize() < kMaxLogBytes) return;
    logFile.moveFileTo(juce::File(logFile.getFullPathName() + ".1"));
}

namespace {
// One thread's unfinished line. Written out, with a line end, if the thread ends first.
struct ThreadLine {
    std::string text;
    ~ThreadLine() { if (!text.empty()) { text += '\n'; fwrite(text.data(), 1, text.size(), stdout); } }
};
class LineLogBuf : public std::streambuf {
public:
    explicit LineLogBuf(bool flushEachLine) : mFlush(flushEachLine) {}
protected:
    int_type overflow(int_type c) override {
        if (!traits_type::eq_int_type(c, traits_type::eof())) { const char ch = traits_type::to_char_type(c); put(&ch, 1); }
        return traits_type::not_eof(c);
    }
    std::streamsize xsputn(const char* p, std::streamsize n) override { put(p, n); return n; }
    int sync() override { return fflush(stdout) == 0 ? 0 : -1; }
private:
    void put(const char* p, std::streamsize n) {
        thread_local ThreadLine line;
        std::string& t = line.text;
        t.append(p, (size_t) n);
        const auto end = t.rfind('\n');
        if (end == std::string::npos) return;
        fwrite(t.data(), 1, end + 1, stdout);   // one locked write: whole lines only
        if (mFlush) fflush(stdout);
        t.erase(0, end + 1);
    }
    const bool mFlush;
};
}

void installLineLog() {
    // Never destroyed: threads may still log while the program shuts down.
    static LineLogBuf* out = new LineLogBuf(false);
    static LineLogBuf* err = new LineLogBuf(true);   // an error line reaches the file at once
    std::cout.rdbuf(out);
    std::cerr.rdbuf(err);
    std::cerr.unsetf(std::ios::unitbuf);   // the line end flushes, not every piece
}

void redirectLogFile(const std::string& path) {
    if (path.empty()) return;
    juce::File(juce::String::fromUTF8(path.c_str())).getParentDirectory().createDirectory();
    rotateLogIfLarge(path);
    // `open`-launched apps have no tty, so [HUB-JIT]/[HUB-ABUF]
    // telemetry would vanish without this. Line-buffered so tail -f works.
#if defined(_WIN32)
    if (freopen(path.c_str(), "a", stdout)) setvbuf(stdout, NULL, _IOFBF, 1 << 16);   // see hub_start_log_flusher
    // Everything the hub prints goes through stdout (see installLineLog). stderr keeps its own buffer for the few
    // direct writers (the audio library's event lines); the flusher writes both out as whole lines.
    if (freopen(path.c_str(), "a", stderr)) setvbuf(stderr, NULL, _IOFBF, 1 << 16);
#else
    if (freopen(path.c_str(), "a", stdout)) setvbuf(stdout, NULL, _IOLBF, 0);
    if (freopen(path.c_str(), "a", stderr)) setvbuf(stderr, NULL, _IOLBF, 0);
#endif
    std::cout << "[HUB] logging to " << path << "\n";
}

// ─── relaunch ─────────────────────────────────────────────────────────────────
std::vector<std::string> relaunchArgv(const std::vector<std::string>& argv, long waitPid) {
    const int argc = (int)argv.size();
    // Settings-covered options are DROPPED so the saved file is what the child
    // applies (CLI would otherwise win and make the GUI edit a no-op).
    static const std::set<std::string> dropWithValue = {
        "--server", "--port", "--user", "--group", "--group-password", "--udp-port",
        "--samplerate", "--blocksize", "--pace-spread", "--pace-min-slot-us", "--channels", "--opus-bitrate", "--server-port",
        "--public-address", "--public-port", "--lan-subnet", "--sink-buffer", "--sink-buffer-lan",
        "--bundle-mtu", "--redundancy-lan", "--redundancy-wan", "--log-file",
        "--aoo-drain", "--aoo-arm-blocks", "--aoo-slack-blocks", "--aoo-blockq-extra", "--aoo-handoff",
        "--wait-for-pid",
    };
    static const std::set<std::string> dropFlag = {
        "--pcm", "--opus", "--local-server", "--no-local-server",
        "--send-pace", "--no-send-pace", "--no-pace-dither", "--cut-through", "--inline-listeners",
        "--no-bundle", "--dscp", "--no-dscp", "--connected-sockets", "--no-connected-sockets",
    };
    static const std::set<std::string> keepWithValue = { "--settings-file", "--dump-peer", "--dump-secs" };

    std::vector<std::string> out;
    if (argc > 0) out.push_back(argv[0]);
    for (int i = 1; i < argc; ++i) {
        std::string s = argv[i];
        if (dropWithValue.count(s)) { if (i + 1 < argc) ++i; continue; }
        if (dropFlag.count(s)) continue;
        if (keepWithValue.count(s)) { out.push_back(s); if (i + 1 < argc) out.push_back(argv[++i]); continue; }
        out.push_back(s);   // --nogui, --no-settings, anything else
    }
    out.push_back("--wait-for-pid");
    out.push_back(std::to_string(waitPid));
    return out;
}

bool relaunchProcess(const std::vector<std::string>& argvIn, std::string& err) {
    err.clear();
#ifdef _WIN32
    const juce::String exe = juce::File::getSpecialLocation(juce::File::currentExecutableFile).getFullPathName();
    if (exe.isEmpty()) { err = "cannot resolve current executable path"; return false; }
    // One command line with the usual quoting rules (backslashes doubled only in front of a quote).
    auto quote = [](const juce::String& arg) {
        if (arg.isNotEmpty() && !arg.containsAnyOf(" \t\"")) return arg;
        juce::String out = "\"";
        int slashes = 0;
        for (auto c : arg) {
            if (c == '\\') { ++slashes; continue; }
            if (c == '"') out << juce::String::repeatedString("\\", slashes * 2 + 1) << "\"";
            else out << juce::String::repeatedString("\\", slashes) << juce::String::charToString(c);
            slashes = 0;
        }
        return out + juce::String::repeatedString("\\", slashes * 2) + "\"";
    };
    juce::String cmd = quote(exe);
    for (size_t i = 1; i < argvIn.size(); ++i) cmd << " " << quote(juce::String::fromUTF8(argvIn[i].c_str()));
    std::cout << "[HUB] relaunch: " << cmd << "\n";
    std::wstring wcmd(cmd.toWideCharPointer());
    STARTUPINFOW si; memset(&si, 0, sizeof(si)); si.cb = sizeof(si);
    PROCESS_INFORMATION pi; memset(&pi, 0, sizeof(pi));
    // Nothing is inherited: the new process binds the same ports once this one is gone.
    if (!CreateProcessW(exe.toWideCharPointer(), &wcmd[0], nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        err = "CreateProcess failed, error " + std::to_string((unsigned long) GetLastError());
        return false;
    }
    std::cout << "[HUB] relaunched as pid " << (unsigned long) pi.dwProcessId << "\n";
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
#else
    const std::string exe = juce::File::getSpecialLocation(juce::File::currentExecutableFile)
                                .getFullPathName().toStdString();
    if (exe.empty()) { err = "cannot resolve current executable path"; return false; }

    std::vector<std::string> args = argvIn;
    if (args.empty()) args.push_back(exe); else args[0] = exe;
    std::vector<char*> cargv;
    cargv.reserve(args.size() + 1);
    for (auto& s : args) cargv.push_back(s.data());
    cargv.push_back(nullptr);

    std::cout << "[HUB] relaunch:";
    for (auto& s : args) std::cout << " " << s;
    std::cout << "\n";

  #if defined(__APPLE__)
    // posix_spawn with CLOEXEC_DEFAULT: every descriptor except the ones we
    // explicitly inherit (stdio) is closed in the child — the parent's bound
    // UDP/TCP sockets must not survive into the child or its own bind() of the
    // same ports fails with EADDRINUSE after we exit.
    posix_spawnattr_t attr;
    posix_spawn_file_actions_t fa;
    posix_spawnattr_init(&attr);
    posix_spawn_file_actions_init(&fa);
    sigset_t allSigs, noSigs;
    sigfillset(&allSigs);
    sigemptyset(&noSigs);
    posix_spawnattr_setsigdefault(&attr, &allSigs);
    posix_spawnattr_setsigmask(&attr, &noSigs);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_CLOEXEC_DEFAULT | POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK);
    posix_spawn_file_actions_addinherit_np(&fa, 0);
    posix_spawn_file_actions_addinherit_np(&fa, 1);
    posix_spawn_file_actions_addinherit_np(&fa, 2);
    pid_t pid = 0;
    const int rc = posix_spawn(&pid, exe.c_str(), &fa, &attr, cargv.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&attr);
    if (rc != 0) { err = std::string("posix_spawn: ") + strerror(rc); return false; }
    std::cout << "[HUB] relaunched as pid " << (long)pid << "\n";
    return true;
  #else
    pid_t pid = fork();
    if (pid < 0) { err = std::string("fork: ") + strerror(errno); return false; }
    if (pid == 0) {
        // Child: only async-signal-safe calls until execv.
        long maxfd = sysconf(_SC_OPEN_MAX);
        if (maxfd < 0 || maxfd > 65536) maxfd = 65536;
        for (int fd = 3; fd < (int)maxfd; ++fd) close(fd);
        execv(exe.c_str(), cargv.data());
        _exit(127);
    }
    std::cout << "[HUB] relaunched as pid " << (long)pid << "\n";
    return true;
  #endif
#endif
}


// ─── Other stages on this computer ───────────────────────────────────────────
static juce::File otherStagesFile() { return mainDataDir().getChildFile("other_stages.json"); }
static juce::File stagesRoot() { return mainDataDir().getChildFile("stages"); }
static juce::File deletedStagesRoot() { return mainDataDir().getChildFile("stages-deleted"); }
// The main hub's own settings file (a main hub's functions only).
static juce::File mainSettingsFile() { return instanceFileRef() != juce::File() ? instanceFileRef() : defaultFile(); }
// A stage that lives outside stages/ carries its main hub's data folder in the marker file.
static void markStageOfThisHub(const juce::File& stageFile) {
    const juce::File dir = stageFile.getParentDirectory();
    if (dir.getParentDirectory() == stagesRoot() || markedMainHub(dir) == mainDataDir()) return;
    dir.getChildFile(kStageMarker).replaceWithText(mainDataDir().getFullPathName() + "\n", false, false, "\n");
}
static bool isManagedStage(const juce::File& file) { return file.getParentDirectory().getParentDirectory() == stagesRoot(); }

bool canManageOtherStages() { return !isStage(); }
bool canStartOtherStages() { return canManageOtherStages(); }

static bool sameFile(const juce::File& a, const juce::File& b) { return a.getFullPathName() == b.getFullPathName(); }
// Same file, also when one path goes through a link (the list must never hold a stage twice).
static bool samePlace(const juce::File& a, const juce::File& b) {
    if (sameFile(a, b)) return true;
#if !defined(_WIN32)
    char ra[PATH_MAX], rb[PATH_MAX];
    if (::realpath(a.getFullPathName().toRawUTF8(), ra) && ::realpath(b.getFullPathName().toRawUTF8(), rb)) return std::strcmp(ra, rb) == 0;
#endif
    return false;
}

struct StageEntry {
    juce::File file;
    bool run = false;   // started and not stopped by the operator
};

// What other_stages.json holds.
static std::vector<StageEntry> savedStageEntries() {
    std::vector<StageEntry> out;
    const juce::var root = juce::JSON::parse(otherStagesFile().loadFileAsString());
    if (const auto* arr = root.getProperty("stages", juce::var()).getArray())
        for (const auto& v : *arr) {
            const juce::String path = v.getProperty("file", juce::var()).toString();
            if (path.isEmpty() || !juce::File::isAbsolutePath(path)) continue;
            StageEntry e;
            e.file = juce::File(path);
            e.run = (bool) v.getProperty("run", false);
            out.push_back(e);
        }
    return out;
}
// The list: the saved entries (a stage folder under stages/ that is gone is left out) and every other stage
// folder under stages/.
static std::vector<StageEntry> stageEntries() {
    std::vector<StageEntry> out;
    for (const auto& e : savedStageEntries())
        if (!isManagedStage(e.file) || e.file.existsAsFile()) out.push_back(e);
    juce::Array<juce::File> dirs = stagesRoot().findChildFiles(juce::File::findDirectories, false);
    dirs.sort();
    for (const auto& d : dirs) {
        const juce::File f = d.getChildFile("hub_settings.json");
        if (!f.existsAsFile()) continue;
        bool have = false;
        for (const auto& e : out) if (samePlace(e.file, f)) { have = true; break; }
        if (!have) { StageEntry e; e.file = f; out.push_back(e); }
    }
    return out;
}
static bool saveStageEntries(const std::vector<StageEntry>& list, std::string& err) {
    juce::Array<juce::var> arr;
    for (const auto& e : list) {
        auto* o = new juce::DynamicObject();
        o->setProperty("file", e.file.getFullPathName());
        if (e.run) o->setProperty("run", true);
        arr.add(juce::var(o));
    }
    auto* root = new juce::DynamicObject();
    root->setProperty("_schema", 1);
    root->setProperty("stages", arr);
    const juce::File f = otherStagesFile();
    f.getParentDirectory().createDirectory();
    if (!f.replaceWithText(juce::JSON::toString(juce::var(root)) + "\n", false, false, "\n")) { err = "could not write " + f.getFullPathName().toStdString(); return false; }
    return true;
}
// One writer at a time, in this process and across the stages (each adds itself at its start). `change` returns
// false when there is nothing to save.
static bool editStageList(const std::function<bool(std::vector<StageEntry>&)>& change, std::string& err) {
    static std::mutex m;
    std::lock_guard<std::mutex> lk(m);
    juce::InterProcessLock lock("SonoBusRPHub_other_stages");
    if (!lock.enter(2000)) { err = "the other stages list is busy"; return false; }
    auto list = savedStageEntries();
    if (!change(list)) return true;
    return saveStageEntries(list, err);
}
static void markStageStarted(const juce::File& file, bool run) {
    std::string err;
    editStageList([&](std::vector<StageEntry>& list) {
        for (auto& e : list) if (samePlace(e.file, file)) { if (e.run == run) return false; e.run = run; return true; }
        if (!run) return false;
        StageEntry e; e.file = file; e.run = true;
        list.push_back(e);
        return true;
    }, err);
    if (!err.empty()) std::cout << "[HUB-STAGES] " << err << "\n";
}

// The process named in a stage's hub.pid, if it is alive and is this app (a reused pid is not).
static long livePid(const juce::File& dir) {
    const long pid = dir.getChildFile("hub.pid").loadFileAsString().trim().getLargeIntValue();
    if (pid <= 0) return 0;
    const juce::String self = juce::File::getSpecialLocation(juce::File::currentExecutableFile).getFileName();
#if defined(_WIN32)
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD) pid);
    if (!h) return 0;
    DWORD code = 0; wchar_t path[1024]; DWORD n = 1024;
    const bool alive = GetExitCodeProcess(h, &code) && code == STILL_ACTIVE;
    const bool same = QueryFullProcessImageNameW(h, 0, path, &n) && juce::File(juce::String(path)).getFileName().equalsIgnoreCase(self);
    CloseHandle(h);
    return alive && same ? pid : 0;
#else
    if (::kill((pid_t) pid, 0) != 0 && errno != EPERM) return 0;
  #if defined(__APPLE__)
    char path[PROC_PIDPATHINFO_MAXSIZE] = {0};
    if (proc_pidpath((int) pid, path, sizeof(path)) <= 0) return 0;
    return juce::File(juce::String::fromUTF8(path)).getFileName() == self ? pid : 0;
  #else
    const juce::String exe = juce::File("/proc/" + juce::String(pid) + "/exe").getLinkedTarget().getFileName();
    return exe == self || exe == self + " (deleted)" ? pid : 0;   // "(deleted)": the program file was replaced by an update
  #endif
#endif
}

static OtherStage describeStage(const StageEntry& e) {
    OtherStage st;
    st.file = e.file;
    st.managed = isManagedStage(e.file);
    HubLaunchArgs a;
    std::string err;
    if (!e.file.existsAsFile()) st.problem = "settings file not found";
    else if (!load(e.file, a, err)) st.problem = err.empty() ? "settings file cannot be read" : err;
    else { st.group = a.groupName; st.serverPort = a.localServerPort; st.udpPort = a.udpPort; st.webPort = a.webPassword.empty() ? 0 : a.webPort; }
    const juce::File dir = e.file.getParentDirectory();
    st.running = livePid(dir) > 0;
    st.stopping = st.running && dir.getChildFile("QUIT").existsAsFile();
    if (st.running) {   // its own figures, a few seconds old at most
        const juce::var s = juce::JSON::parse(dir.getChildFile("hub_status.json").loadFileAsString());
        const juce::int64 t = (juce::int64) s.getProperty("t", 0);
        if (t > 0 && std::llabs((long long) (juce::Time::currentTimeMillis() / 1000 - t)) <= 20) {
            st.players = (int) s.getProperty("players", -1);
            st.inBytesPerSec = (double) s.getProperty("in", -1.0);
            st.outBytesPerSec = (double) s.getProperty("out", -1.0);
            st.portState = (int) s.getProperty("ports", -1);
            st.portRuns = (int) s.getProperty("portRuns", 0);
            st.portText = s.getProperty("portText", juce::var()).toString().toStdString();
        }
    }
    return st;
}

std::vector<OtherStage> otherStages() {
    std::vector<OtherStage> out;
    if (!canManageOtherStages()) return out;
    for (const auto& e : stageEntries()) out.push_back(describeStage(e));
    // A stage above a free pair of the row (main stage's ports + 2, + 4, ...) can move down to it.
    HubLaunchArgs mine;
    std::string ignored;
    load(mainSettingsFile(), mine, ignored);
    for (auto& st : out) {
        if (st.serverPort <= 0) continue;
        std::set<int> used = { mine.localServerPort, mine.udpPort };
        for (const auto& o : out) if (&o != &st && o.serverPort > 0) used.insert({ o.serverPort, o.udpPort });
        for (int p = mine.localServerPort + 2; p < st.serverPort && p + 1 <= 65535; p += 2)
            if (!used.count(p) && !used.count(p + 1)) { st.moveServerPort = p; break; }
    }
    return out;
}

static bool listed(const juce::File& file) {
    for (const auto& e : stageEntries()) if (samePlace(e.file, file)) return true;
    return false;
}

bool addOtherStage(const juce::File& fileIn, std::string& err) {
    err.clear();
    if (!canManageOtherStages()) { err = "only the main hub keeps a list of other stages"; return false; }
    const juce::File file = fileIn.isDirectory() ? fileIn.getChildFile("hub_settings.json") : fileIn;
    if (!file.existsAsFile()) { err = "no such file: " + file.getFullPathName().toStdString(); return false; }
    if (file.getParentDirectory() == mainDataDir()) { err = "that is this hub's own settings file"; return false; }
    HubLaunchArgs a, mine;
    if (!load(file, a, err)) { if (err.empty()) err = "not a hub settings file"; return false; }
    std::string ignored;
    load(mainSettingsFile(), mine, ignored);   // this hub's ports (defaults when it has no file yet)
    if (a.localServerPort == mine.localServerPort || a.udpPort == mine.udpPort) {
        err = "that stage uses this hub's ports (" + std::to_string(mine.localServerPort) + " / " + std::to_string(mine.udpPort) + "); give it its own ports first";
        return false;
    }
    if (listed(file)) { err = "that stage is in the list already"; return false; }
    if (stageEntries().size() >= 16) { err = "the list holds 16 stages"; return false; }
    if (!editStageList([&](std::vector<StageEntry>& list) { StageEntry e; e.file = file; list.push_back(e); return true; }, err)) return false;
    markStageOfThisHub(file);   // from its next start it runs as a stage of this hub
    std::cout << "[HUB-STAGES] added " << file.getFullPathName() << " ('" << a.groupName << "')\n";
    return true;
}

bool forgetOtherStage(const juce::File& file, std::string& err) {
    err.clear();
    if (!listed(file)) { err = "that stage is not in the list"; return false; }
    if (isManagedStage(file)) { err = "this hub made that stage: delete it instead"; return false; }
    if (livePid(file.getParentDirectory()) > 0) { err = "stop the stage first"; return false; }
    if (!editStageList([&](std::vector<StageEntry>& list) {
            const auto n = list.size();
            list.erase(std::remove_if(list.begin(), list.end(), [&](const StageEntry& e) { return samePlace(e.file, file); }), list.end());
            return list.size() != n;
        }, err)) return false;
    file.getSiblingFile(kStageMarker).deleteFile();   // no stage of this hub any more
    std::cout << "[HUB-STAGES] forgot " << file.getFullPathName() << "\n";
    return true;
}

// This hub's own taps file and sink settings live in its environment (set at start); another stage reads its own.
static bool hubOwnEnv(const std::string& kv) {
    return kv.rfind("SB_TAPS_FILE=", 0) == 0 || kv.rfind("SB_AOO_", 0) == 0 || kv.rfind("SB_PLC_OFF=", 0) == 0;
}

bool startOtherStage(const juce::File& file, std::string& err) {
    err.clear();
    if (!canStartOtherStages()) { err = "only the main hub starts other stages"; return false; }
    if (!listed(file)) { err = "that stage is not in the list"; return false; }
    if (!file.existsAsFile()) { err = "settings file not found"; return false; }
    const juce::File dir = file.getParentDirectory();
    if (livePid(dir) > 0) { err = "that stage is running"; return false; }
    dir.getChildFile("QUIT").deleteFile();
    markStageOfThisHub(file);
    const std::string path = file.getFullPathName().toStdString();
#if defined(__APPLE__)
    // Through Launch Services, as a separate copy of this app (the same way a script would start it)
    juce::File bundle = juce::File::getSpecialLocation(juce::File::currentApplicationFile);
    std::vector<std::string> args = { "/usr/bin/open", "-n", "-g", "-a", bundle.getFullPathName().toStdString(), "--args", "--settings-file", path };
    std::vector<char*> cargv;
    for (auto& a : args) cargv.push_back(a.data());
    cargv.push_back(nullptr);
    posix_spawnattr_t attr;
    posix_spawn_file_actions_t fa;
    posix_spawnattr_init(&attr);
    posix_spawn_file_actions_init(&fa);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_CLOEXEC_DEFAULT);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    std::vector<std::string> envs;   // open hands its environment on to the app
    for (char** e = environ; e && *e; ++e) if (!hubOwnEnv(*e)) envs.emplace_back(*e);
    std::vector<char*> cenv;
    for (auto& e : envs) cenv.push_back(e.data());
    cenv.push_back(nullptr);
    pid_t pid = 0;
    const int rc = posix_spawn(&pid, "/usr/bin/open", &fa, &attr, cargv.data(), cenv.data());
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&attr);
    if (rc != 0) { err = std::string("could not start: ") + strerror(rc); return false; }
    int status = 0;
    ::waitpid(pid, &status, 0);   // open returns as soon as Launch Services has the request
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) { err = "Launch Services refused to start it"; return false; }
#elif defined(_WIN32)
    const juce::String exe = juce::File::getSpecialLocation(juce::File::currentExecutableFile).getFullPathName();
    juce::String cmd = "\"" + exe + "\" --settings-file \"" + file.getFullPathName() + "\"";
    std::wstring wcmd(cmd.toWideCharPointer());
    std::wstring env;   // without this hub's own settings (hubOwnEnv)
    if (wchar_t* block = GetEnvironmentStringsW()) {
        for (const wchar_t* e = block; *e; e += wcslen(e) + 1) {
            const std::wstring v(e);
            if (hubOwnEnv(juce::String(v.c_str()).toStdString())) continue;
            env += v; env.push_back(L'\0');
        }
        FreeEnvironmentStringsW(block);
    }
    env.push_back(L'\0');
    STARTUPINFOW si; memset(&si, 0, sizeof(si)); si.cb = sizeof(si);
    PROCESS_INFORMATION pi; memset(&pi, 0, sizeof(pi));
    if (!CreateProcessW(exe.toWideCharPointer(), &wcmd[0], nullptr, nullptr, FALSE, CREATE_NEW_PROCESS_GROUP | CREATE_UNICODE_ENVIRONMENT, &env[0], nullptr, &si, &pi)) {
        err = "could not start, error " + std::to_string((unsigned long) GetLastError());
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
#else
    // A child of this hub, so it lives in the same service: it stops with the service. Forked twice, so no
    // finished child waits for this hub to collect it.
    const std::string exe = juce::File::getSpecialLocation(juce::File::currentExecutableFile).getFullPathName().toStdString();
    std::vector<std::string> args = { exe, "--settings-file", path };
    std::vector<char*> cargv;
    for (auto& a : args) cargv.push_back(a.data());
    cargv.push_back(nullptr);
    std::vector<std::string> envs;
    for (char** e = environ; e && *e; ++e) if (!hubOwnEnv(*e)) envs.emplace_back(*e);
    std::vector<char*> cenv;
    for (auto& e : envs) cenv.push_back(e.data());
    cenv.push_back(nullptr);
    long maxfd = sysconf(_SC_OPEN_MAX);
    if (maxfd < 0 || maxfd > 65536) maxfd = 65536;
    const pid_t pid = fork();
    if (pid < 0) { err = std::string("could not start: ") + strerror(errno); return false; }
    if (pid == 0) {   // only async-signal-safe calls from here
        setsid();
        if (fork() != 0) _exit(0);
        for (int fd = 0; fd < (int) maxfd; ++fd) close(fd);
        open("/dev/null", O_RDONLY);
        open("/dev/null", O_WRONLY);
        open("/dev/null", O_WRONLY);
        execve(exe.c_str(), cargv.data(), cenv.data());
        _exit(127);
    }
    int status = 0;
    ::waitpid(pid, &status, 0);
#endif
    markStageStarted(file, true);
    std::cout << "[HUB-STAGES] start " << path << "\n";
    return true;
}

bool stopOtherStage(const juce::File& file, std::string& err) {
    err.clear();
    if (!canStartOtherStages()) { err = "only the main hub stops other stages"; return false; }
    if (!listed(file)) { err = "that stage is not in the list"; return false; }
    const juce::File dir = file.getParentDirectory();
    markStageStarted(file, false);
    if (livePid(dir) <= 0) { err = "that stage is not running"; return false; }
    if (!dir.getChildFile("QUIT").replaceWithText("quit\n", false, false, "\n")) { err = "could not write to " + dir.getFullPathName().toStdString(); return false; }
    std::cout << "[HUB-STAGES] stop " << file.getFullPathName().toStdString() << " (QUIT written)\n";
    return true;
}

bool showOtherStage(const juce::File& file, std::string& err, bool settings) {
    err.clear();
#if defined(__APPLE__)
    if (!canStartOtherStages()) { err = "only the main hub opens other stages"; return false; }
    if (!listed(file)) { err = "that stage is not in the list"; return false; }
    const juce::File dir = file.getParentDirectory();
    if (livePid(dir) <= 0) { err = "that stage is not running"; return false; }
    if (!dir.getChildFile("SHOW").replaceWithText(settings ? "settings\n" : "show\n", false, false, "\n")) { err = "could not write to " + dir.getFullPathName().toStdString(); return false; }
    std::cout << "[HUB-STAGES] open window " << file.getFullPathName().toStdString() << " (SHOW written)\n";
    return true;
#else
    juce::ignoreUnused(file, settings);
    err = "this system has no background stage windows to open";
    return false;
#endif
}

// ── New stage / Delete stage ──
// Folder name from the stage name: lower-case letters, digits and single dashes.
static juce::String stageSlug(const juce::String& name) {
    juce::String s;
    for (auto c : name.toLowerCase()) {
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) s << juce::String::charToString(c);
        else if (s.isNotEmpty() && !s.endsWithChar('-')) s << "-";
    }
    while (s.endsWithChar('-')) s = s.dropLastCharacters(1);
    s = s.substring(0, 40);
    while (s.endsWithChar('-')) s = s.dropLastCharacters(1);
    return s.isEmpty() ? juce::String("stage") : s;
}
// Free = nothing on this computer holds the port, on any address. The test binds with address re-use (a port
// that only waits out old connections is free) on every local address in turn, so a listener on one address counts.
static bool portFreeOn(int type, int port, uint32_t addrNet) {
#if defined(_WIN32)
    juce::ignoreUnused(addrNet);
    if (type == SOCK_STREAM) { juce::StreamingSocket s; return s.createListener(port); }
    juce::DatagramSocket s(false);
    return s.bindToPort(port);
#else
    const int fd = ::socket(AF_INET, type, 0);
    if (fd < 0) return false;
    if (type == SOCK_STREAM) { int one = 1; ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)); }
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = addrNet;
    a.sin_port = htons((uint16_t) port);
    const bool ok = ::bind(fd, (struct sockaddr*) &a, sizeof(a)) == 0;
    ::close(fd);
    return ok;
#endif
}
static bool portFree(int type, int port) {
    if (!portFreeOn(type, port, htonl(INADDR_ANY))) return false;
#if !defined(_WIN32)
    if (!portFreeOn(type, port, htonl(INADDR_LOOPBACK))) return false;
    for (const auto& ip : juce::IPAddress::getAllAddresses(false)) {
        if (ip.isNull() || ip == juce::IPAddress::local(false)) continue;
        struct in_addr in;
        if (::inet_pton(AF_INET, ip.toString().toRawUTF8(), &in) == 1 && !portFreeOn(type, port, in.s_addr)) return false;
    }
#endif
    return true;
}
static bool tcpPortFree(int port) { return portFree(SOCK_STREAM, port); }
static bool udpPortFree(int port) { return portFree(SOCK_DGRAM, port); }

// The stages' ports are one row: the main stage's pair, then the next pair up for each further stage. The first
// pair of the row that no listed stage has and nothing on this computer holds.
static int nextFreeStagePair(int mainServerPort, const std::set<int>& used) {
    for (int p = mainServerPort + 2; p + 1 <= 65535; p += 2)
        if (!used.count(p) && !used.count(p + 1) && tcpPortFree(p) && udpPortFree(p) && udpPortFree(p + 1)) return p;
    return 0;
}

// What the operator opens: the ports of the main stage and of every listed stage for players, as ranges, and the
// web page ports. Router text, or the firewall commands on Linux (shown, never run). Whole sentences.
static std::vector<std::pair<int, int>> portRanges(std::set<int> ports) {
    ports.erase(0);
    std::vector<std::pair<int, int>> ranges;
    for (const int p : ports) {
        if (!ranges.empty() && ranges.back().second + 1 == p) ranges.back().second = p;
        else ranges.push_back({ p, p });
    }
    return ranges;
}
std::string stagesForwardHelp() {
    HubLaunchArgs mine;
    std::string ignored;
    load(mainSettingsFile(), mine, ignored);
    std::set<int> ports = { mine.localServerPort, mine.udpPort }, web;
    if (!mine.webPassword.empty()) web.insert(mine.webPort);
    for (const auto& e : stageEntries()) {
        HubLaunchArgs o;
        if (!load(e.file, o, ignored)) continue;
        ports.insert({ o.localServerPort, o.udpPort });
        if (!o.webPassword.empty()) web.insert(o.webPort);
    }
    const auto ranges = portRanges(ports), webRanges = portRanges(web);
#if defined(__APPLE__) || defined(_WIN32)
    auto text = [](const std::vector<std::pair<int, int>>& rs) {
        std::string out;
        for (size_t i = 0; i < rs.size(); ++i) {
            if (i > 0) out += i + 1 == rs.size() ? " and " : ", ";
            out += std::to_string(rs[i].first);
            if (rs[i].second != rs[i].first) out += "\xe2\x80\x93" + std::to_string(rs[i].second);
        }
        return out;
    };
    std::string out = "Forward " + text(ranges) + " (TCP and UDP) to this computer for players.";
    if (!webRanges.empty()) out += " Web pages: " + text(webRanges) + " (TCP), only if you want them reachable from outside.";
    return out;
#else
    auto span = [](const std::pair<int, int>& r) { return std::to_string(r.first) + (r.second != r.first ? ":" + std::to_string(r.second) : std::string()); };
    std::string out;
    for (const auto& r : ranges) {
        if (!out.empty()) out += " && ";
        out += "sudo ufw allow " + span(r) + "/tcp && sudo ufw allow " + span(r) + "/udp";
    }
    out = "Open in this server's firewall (and in the provider's, if there is one), for players: " + out + ".";
    if (!webRanges.empty()) {
        std::string w;
        for (const auto& r : webRanges) { if (!w.empty()) w += " && "; w += "sudo ufw allow " + span(r) + "/tcp"; }
        out += " For the web pages: " + w + ".";
    }
    return out;
#endif
}

// A stage's page links back to its main hub's page when that hub has one (its settings are read at most every 10 s).
int mainHubWebPort() {
    if (!isStage()) return 0;
    static std::mutex m;
    static juce::int64 at = 0;
    static int port = 0;
    std::lock_guard<std::mutex> lk(m);
    const juce::int64 now = juce::Time::currentTimeMillis();
    if (at == 0 || now - at > 10000) {
        at = now;
        HubLaunchArgs a;
        std::string ignored;
        port = load(stageMainFolder().getChildFile("hub_settings.json"), a, ignored) && !a.webPassword.empty() ? a.webPort : 0;
    }
    return port;
}

int stageCores() {
    if (const char* e = std::getenv("SB_HUB_TEST_CORES"); e && std::atoi(e) > 0) return std::atoi(e);   // test aid
    const int n = juce::SystemStats::getNumPhysicalCpus();
    return n > 0 ? n : juce::SystemStats::getNumCpus();
}
int stagesRunning() {
    int n = 1;   // this hub
    for (const auto& e : stageEntries()) if (livePid(e.file.getParentDirectory()) > 0) ++n;
    return n;
}
std::string stageLoadWarning(int extra) {
    const int cores = stageCores(), n = stagesRunning() + extra;
    if (n <= std::max(1, cores - 1)) return {};
    return "This computer has " + std::to_string(cores) + " processor core" + (cores == 1 ? "" : "s") + " and " + (extra > 0 ? "would then run " : "runs ")
         + std::to_string(n) + " stages. For clean audio keep it to " + std::to_string(std::max(1, cores - 1)) + " or fewer, so one core stays free.";
}

bool createStage(const NewStage& in, OtherStage& made, std::string& err) {
    err.clear();
    if (!canManageOtherStages()) { err = "only the main hub makes new stages"; return false; }
    const juce::String name = juce::String::fromUTF8(in.name.c_str()).trim();
    if (name.isEmpty()) { err = "Enter the stage name"; return false; }
    std::string city = in.city, region = in.region;
    if (juce::String::fromUTF8(region.c_str()).trim().isEmpty()) splitLocation(in.city, city, region);   // "City, ST" in one text

    HubLaunchArgs a;   // the main stage's saved settings (built-in values when it has no file yet)
    std::string ignored;
    load(mainSettingsFile(), a, ignored);
    const auto entries = stageEntries();
    if (entries.size() >= 16) { err = "the list holds 16 stages"; return false; }
    std::set<int> used = { a.localServerPort, a.udpPort, a.publicAddressPort, a.webPort };
    const int mainServerPort = a.localServerPort;
    if (name.equalsIgnoreCase(juce::String::fromUTF8(a.groupName.c_str()))) { err = "that is the name of this hub's own stage"; return false; }
    for (const auto& e : entries) {
        HubLaunchArgs o;
        if (!load(e.file, o, ignored)) continue;
        if (name.equalsIgnoreCase(juce::String::fromUTF8(o.groupName.c_str()))) { err = "a stage with that name is in the list already"; return false; }
        used.insert({ o.localServerPort, o.udpPort, o.publicAddressPort, o.webPort });
    }
    // The next free pair of the row: 13001 / 13002 after the main stage's 12999 / 13000, then 13003 / 13004, ...
    const int serverPort = nextFreeStagePair(mainServerPort, used);
    if (serverPort == 0) { err = "no free pair of ports found"; return false; }
    const std::string webPassword = !in.webPassword.empty() ? in.webPassword : a.webPassword;
    int webPort = 0;   // no password = no web page
    if (!webPassword.empty()) {
        for (int p = 18080; p < 18180; ++p) if (!used.count(p) && tcpPortFree(p)) { webPort = p; break; }
        if (webPort == 0) { err = "no free web page port found"; return false; }
    }

    const juce::String slug = stageSlug(name);
    juce::File dir = stagesRoot().getChildFile(slug);
    for (int n = 2; dir.exists(); ++n) dir = stagesRoot().getChildFile(slug + "-" + juce::String(n));
    if (dir.createDirectory().failed()) { err = "could not make the folder " + dir.getFullPathName().toStdString(); return false; }

    // The main stage's settings, with this stage's own name, password, ports, log and listing. No directory
    // identity: the stage gets its own at its first start.
    a.groupName = name.toStdString();
    a.groupPassword = juce::String::fromUTF8(in.password.c_str()).trim().toStdString();
    a.localServerPort = serverPort;
    a.udpPort = a.publicAddressPort = serverPort + 1;
    a.webPort = webPort;
    a.webPassword = webPassword;
    a.logFile = dir.getChildFile("hub.log").getFullPathName().toStdString();
    a.listStage = in.listed;
    a.location = joinLocation(city, region);
    normaliseLocation(a);
    if (const std::string problem = locationProblem(a); !problem.empty()) { dir.deleteRecursively(); err = problem; return false; }
    a.stageGenre = HubLaunchArgs().stageGenre;
    a.stageDescription.clear();
    a.stageHubId.clear();
    a.stageHubKey.clear();
    if (const std::string problem = validate(a); !problem.empty()) { dir.deleteRecursively(); err = problem; return false; }
    const juce::File file = dir.getChildFile("hub_settings.json");
    if (!save(file, a, err)) { dir.deleteRecursively(); return false; }
#if !defined(_WIN32)
    ::chmod(file.getFullPathName().toRawUTF8(), 0600);   // it holds the passwords
#endif
    std::cout << "[HUB-STAGES] new stage '" << a.groupName << "' in " << dir.getFullPathName() << ": ports " << serverPort << " / " << serverPort + 1
              << ", web page " << (webPort > 0 ? std::to_string(webPort) : std::string("off")) << (in.listed ? ", listed once its ports are open" : ", not listed") << "\n";
    std::string startErr;
    if (!startOtherStage(file, startErr)) std::cout << "[HUB-STAGES] the new stage did not start: " << startErr << "\n";
    StageEntry e; e.file = file;
    made = describeStage(e);
    if (!startErr.empty()) made.problem = "made, but it did not start: " + startErr;
    return true;
}

// Moves a listed stage (made here or added by its path) to the lowest free pair of the row. Only the port values in
// its settings file change: its name, directory identity, players and history stay. A running stage restarts.
bool moveStagePorts(const juce::File& file, std::string& err) {
    err.clear();
    if (!canManageOtherStages()) { err = "only the main hub moves a stage's ports"; return false; }
    if (!listed(file)) { err = "that stage is not in the list"; return false; }
    HubLaunchArgs a, mine;
    std::string ignored;
    if (!load(file, a, err)) { if (err.empty()) err = "settings file cannot be read"; return false; }
    load(mainSettingsFile(), mine, ignored);
    std::set<int> used = { mine.localServerPort, mine.udpPort, mine.webPort };
    for (const auto& e : stageEntries()) {
        HubLaunchArgs o;
        if (samePlace(e.file, file) || !load(e.file, o, ignored)) continue;
        used.insert({ o.localServerPort, o.udpPort, o.webPort });
    }
    const int p = nextFreeStagePair(mine.localServerPort, used);
    if (p == 0 || p >= a.localServerPort) { err = "this stage has the lowest free ports already"; return false; }

    juce::var root = juce::JSON::parse(file.loadFileAsString());
    auto* obj = root.getDynamicObject();
    if (obj == nullptr) { err = "settings file cannot be read"; return false; }
    const int oldServer = a.localServerPort, oldUdp = a.udpPort;
    obj->setProperty("server_port", p);
    obj->setProperty("udp_port", p + 1);
    if (!obj->hasProperty("public_port") || a.publicAddressPort == oldUdp) obj->setProperty("public_port", p + 1);
    juce::TemporaryFile tmp(file);
    if (!tmp.getFile().replaceWithText(juce::JSON::toString(root, false) + "\n") || !tmp.overwriteTargetFileWithTemporary()) {
        err = "could not write " + file.getFullPathName().toStdString();
        return false;
    }
    std::cout << "[HUB-STAGES] ports of '" << a.groupName << "' (" << file.getFullPathName() << ") moved from " << oldServer << " / " << oldUdp
              << " to " << p << " / " << p + 1 << "\n";
    const juce::File dir = file.getParentDirectory();
    if (livePid(dir) <= 0) return true;
    if (!dir.getChildFile("QUIT").replaceWithText("quit\n", false, false, "\n")) { err = "the ports are changed, but the stage could not be restarted: stop and start it"; return false; }
    std::thread([file, dir]() {   // a clean stop, then a start on the new ports
        for (int i = 0; i < 300 && livePid(dir) > 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        std::string e;
        if (livePid(dir) > 0) { std::cout << "[HUB-STAGES] " << file.getFullPathName() << " did not stop in 30 s: start it again by hand for the new ports\n"; return; }
        if (startOtherStage(file, e)) std::cout << "[HUB-STAGES] restarted on its new ports: " << file.getFullPathName() << "\n";
        else std::cout << "[HUB-STAGES] could not start " << file.getFullPathName() << " on its new ports: " << e << "\n";
    }).detach();
    return true;
}

// The folder goes to stages-deleted/<folder>-<date>; moving it back to stages/ brings the stage back.
static bool moveStageAway(const juce::File& file, std::string& err) {
    const juce::File dir = file.getParentDirectory();
    const juce::String base = dir.getFileName() + "-" + juce::Time::getCurrentTime().formatted("%Y-%m-%d");
    juce::File dest = deletedStagesRoot().getChildFile(base);
    for (int n = 2; dest.exists(); ++n) dest = deletedStagesRoot().getChildFile(base + "-" + juce::String(n));
    deletedStagesRoot().createDirectory();
    dir.getChildFile("QUIT").deleteFile();
    dir.getChildFile("hub.pid").deleteFile();
    dir.getChildFile("hub_status.json").deleteFile();
    if (!dir.moveFileTo(dest)) { err = "could not move " + dir.getFullPathName().toStdString() + " to " + dest.getFullPathName().toStdString(); return false; }
    editStageList([&](std::vector<StageEntry>& list) {
        const auto n = list.size();
        list.erase(std::remove_if(list.begin(), list.end(), [&](const StageEntry& e) { return sameFile(e.file, file); }), list.end());
        return list.size() != n;
    }, err);
    std::cout << "[HUB-STAGES] deleted " << dir.getFullPathName() << ": moved to " << dest.getFullPathName() << "\n";
    return true;
}

bool deleteStage(const juce::File& file, std::string& err) {
    err.clear();
    if (!canManageOtherStages()) { err = "only the main hub deletes stages"; return false; }
    if (!listed(file)) { err = "that stage is not in the list"; return false; }
    if (!isManagedStage(file)) { err = "that stage was added by its path: it can be taken off the list, its folder is not this hub's to move"; return false; }
    const juce::File dir = file.getParentDirectory();
    if (livePid(dir) <= 0) return moveStageAway(file, err);
    markStageStarted(file, false);
    if (!dir.getChildFile("QUIT").replaceWithText("quit\n", false, false, "\n")) { err = "could not write to " + dir.getFullPathName().toStdString(); return false; }
    std::cout << "[HUB-STAGES] delete " << file.getFullPathName() << ": stopping it first (QUIT written)\n";
    std::thread([file, dir]() {   // its clean stop takes it out of the stage directory; then the folder moves
        for (int i = 0; i < 300 && livePid(dir) > 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        std::string e;
        if (livePid(dir) > 0) { std::cout << "[HUB-STAGES] delete " << file.getFullPathName() << ": it did not stop in 30 s, nothing moved\n"; return; }
        if (!moveStageAway(file, e)) std::cout << "[HUB-STAGES] delete: " << e << "\n";
    }).detach();
    return true;
}

// A stage puts its settings file on its main hub's list (other_stages.json in the main hub's data folder) when it
// starts, marked as started: the main hub starts it again at its own next start. A stage outside stages/ also
// writes the marker, so it stays this main hub's stage. Control path, at start.
void registerWithMainHub() {
    const juce::File mine = instanceFileRef();
    if (!isStage() || mine == juce::File() || !mine.existsAsFile()) return;
    bool added = false;
    std::string err;
    const bool ok = editStageList([&](std::vector<StageEntry>& list) {
        for (auto& e : list) if (samePlace(e.file, mine)) { if (e.run) return false; e.run = true; return true; }
        StageEntry e; e.file = mine; e.run = true;
        list.push_back(e);
        added = true;
        return true;
    }, err);
    markStageOfThisHub(mine);
    if (added && ok) std::cout << "[HUB-STAGES] added to the main hub's Other stages: " << mine.getFullPathName().toStdString() << "\n";
    else if (!ok) std::cout << "[HUB-STAGES] could not add this stage to the main hub's list: " << err << "\n";
}

// The operator quit this stage in its own window: the main hub does not start it again.
void stageQuitByOperator() {
    const juce::File mine = instanceFileRef();
    if (!isStage() || mine == juce::File()) return;
    markStageStarted(mine, false);
}

void writeInstancePid() {
    if (!hasOwnDataDir()) return;
#if defined(_WIN32)
    const long pid = (long) GetCurrentProcessId();
#else
    const long pid = (long) getpid();
#endif
    dataDir().getChildFile("hub.pid").replaceWithText(juce::String(pid) + "\n", false, false, "\n");
}

void writeStageStatus(int players, double inBytesPerSec, double outBytesPerSec, int portState, int portRuns, const std::string& portText) {
    if (!isStage()) return;
    const juce::String s = "{\"t\":" + juce::String(juce::Time::currentTimeMillis() / 1000) + ",\"players\":" + juce::String(players)
                         + ",\"in\":" + juce::String((juce::int64) inBytesPerSec) + ",\"out\":" + juce::String((juce::int64) outBytesPerSec)
                         + ",\"ports\":" + juce::String(portState) + ",\"portRuns\":" + juce::String(portRuns)
                         + ",\"portText\":" + juce::JSON::toString(juce::var(juce::String::fromUTF8(portText.c_str())), true) + "}\n";
    dataDir().getChildFile("hub_status.json").replaceWithText(s, false, false, "\n");
}

bool checkStagePorts(const juce::File& file, std::string& err) {
    err.clear();
    if (!canManageOtherStages()) { err = "only the main hub asks a stage to check its ports"; return false; }
    if (!listed(file)) { err = "that stage is not in the list"; return false; }
    const juce::File dir = file.getParentDirectory();
    if (livePid(dir) <= 0) { err = "that stage is not running"; return false; }
    if (!dir.getChildFile("CHECK").replaceWithText("check\n", false, false, "\n")) { err = "could not write to " + dir.getFullPathName().toStdString(); return false; }
    std::cout << "[HUB-STAGES] check ports " << file.getFullPathName().toStdString() << " (CHECK written)\n";
    return true;
}

// The main hub, at its start: every stage that was started and not stopped by the operator runs again (after a
// restart of the computer, an update, or a quit of everything).
void restartStartedStages() {
    if (!canManageOtherStages()) return;
    // An entry from an earlier build that lives outside stages/ and carries no marker: on a Mac or Windows computer
    // it is this hub's stage and gets the marker; on Linux it is a hub of its own (a service with its own settings
    // file) and leaves the list.
    {
        std::string err;
        editStageList([&](std::vector<StageEntry>& list) {
            bool changed = false;
            for (auto it = list.begin(); it != list.end();) {
                const juce::File dir = it->file.getParentDirectory();
                if (isManagedStage(it->file) || markedMainHub(dir) == mainDataDir() || !it->file.existsAsFile()) { ++it; continue; }
               #if defined(__APPLE__) || defined(_WIN32)
                markStageOfThisHub(it->file);
                std::cout << "[HUB-STAGES] " << it->file.getFullPathName() << " is marked as a stage of this hub\n";
                ++it;
               #else
                std::cout << "[HUB-STAGES] " << it->file.getFullPathName() << " is a hub of its own (no stage marker): taken off this list\n";
                it = list.erase(it);
                changed = true;
               #endif
            }
            return changed;
        }, err);
    }
    for (const auto& e : savedStageEntries()) {
        if (!e.run || !e.file.existsAsFile() || livePid(e.file.getParentDirectory()) > 0) continue;
        std::string err;
        if (startOtherStage(e.file, err)) std::cout << "[HUB-STAGES] started again: " << e.file.getFullPathName() << "\n";
        else std::cout << "[HUB-STAGES] could not start " << e.file.getFullPathName() << " again: " << err << "\n";
    }
}

// Stops every running stage and leaves it marked as started, so the next start of the main hub brings it back.
static int stopStagesKeepStarted(const char* why) {
    if (!canManageOtherStages()) return 0;
    std::vector<juce::File> dirs;
    for (const auto& e : stageEntries()) {
        const juce::File dir = e.file.getParentDirectory();
        if (livePid(dir) <= 0) continue;
        markStageStarted(e.file, true);
        dir.getChildFile("QUIT").replaceWithText("quit\n", false, false, "\n");
        dirs.push_back(dir);
    }
    if (dirs.empty()) return 0;
    std::cout << "[HUB-STAGES] stopping " << dirs.size() << " other stage(s) " << why << "\n";
    for (int i = 0; i < 250; ++i) {
        bool any = false;
        for (const auto& d : dirs) if (livePid(d) > 0) any = true;
        if (!any) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    for (const auto& d : dirs) if (livePid(d) > 0) std::cout << "[HUB-STAGES] still running after 25 s: " << d.getFullPathName() << "\n";
    return (int) dirs.size();
}

void stopChildStages() {
#if !defined(__APPLE__) && !defined(_WIN32)
    stopStagesKeepStarted("with this hub");
#endif
}

int stopStagesForUpdate() { return stopStagesKeepStarted("for the update: they start again with the new hub"); }

} // namespace hub_settings
} // namespace sb_hub_gui
