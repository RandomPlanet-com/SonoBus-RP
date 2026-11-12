// SPDX-License-Identifier: GPLv3-or-later WITH Appstore-exception
// Copyright (C) 2026 Jason Buecker (Random Planet)
// SonoBus RP — Hub Server settings persistence (see SonobusHubSettings.h).

#include "SonobusHubSettings.h"
#include <cctype>
#include "SonobusHubServer.h"

#include <cerrno>
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
#include <spawn.h>
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
        { 0, nullptr,              "Stage directory",                   0, nullptr },
        { 0, "list_stage",         "List this stage",                   1, nullptr },
        { 0, "location",           "City",                              0, nullptr },
        { 0, "stage_country",      "Country (two letters)",             0, nullptr },
        { 0, "stage_genre",        "Genre",                             2, "Any,Rock/Pop,Jazz/Blues,Funk/Soul/R&B,Country/Folk,Classical/Choral,Electronic,Worship" },
        { 0, "stage_description",  "Description",                       0, nullptr },
        { 0, "stage_names",        "Player names in the listing",       2, "Automatic,Show,Hide" },
        { 0, nullptr,              "Audio",                             0, nullptr },
        { 0, "samplerate",         "Sample rate (Hz)",                  2, "44100,48000,88200,96000" },
        { 0, "blocksize",          "Block size (samples)",              2, "16,32,64,128,256" },   // 16 added (experiment)
        { 0, nullptr,              "Logging",                           0, nullptr },
        { 0, "log_file",           "Log file",                     0, nullptr },
        { 0, nullptr,              "Web page",                          0, nullptr },   
        { 0, "web_port",           "Web page port",    0, nullptr },
        { 0, "web_password",       "Web password", 3, nullptr },
        { 0, "geoip",              "Player locations", 1, nullptr },   

        { 1, nullptr,              "Fan-out & sink buffers",            0, nullptr },
        // No WAN sink buffer row: WAN players start at the lowest hold and are measured.
        { 1, "sink_buffer_lan_ms", "LAN sink buffer (ms)", 0, nullptr },   // 
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

Tier tierForKey(const std::string& key) {
    if (auto* b = findBinding(key)) return b->info.tier;
    return Tier::Restart;   // unknown → conservative
}

static juce::File homeDir() { return juce::File::getSpecialLocation(juce::File::userHomeDirectory); }

juce::File dataDir() {
#if JUCE_MAC
    return homeDir().getChildFile("Library/Application Support/SonoBus RP Hub");
#else
    return homeDir().getChildFile(".config/SonoBus RP Hub");
#endif
}

juce::File defaultFile() { return dataDir().getChildFile("hub_settings.json"); }

juce::File defaultLogFile() {
#if JUCE_MAC
    return homeDir().getChildFile("Library/Logs/SonoBus RP Hub/hub.log");
#else
    return dataDir().getChildFile("hub.log");
#endif
}

void applyDefaults(HubLaunchArgs& a) {
    if (a.logFile.find("/SonoBus-Plus/hub/") != std::string::npos) a.logFile.clear();   // that folder has moved
    if (a.logFile.empty()) a.logFile = defaultLogFile().getFullPathName().toStdString();
}

// One-time move from the folder older hubs used.
void migrateOldDataDir() {
    const juce::File oldDir = homeDir().getChildFile("SonoBus-Plus").getChildFile("hub");
    const juce::File newDir = dataDir();
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
        { "max_channels",       "The most channels one player may send. A stereo pair counts as 2. Extra channels are not passed on. 0 = no limit." },
        { "list_stage",         "Shows this stage at stages.randomplanet.com so players can find it. Its name, genre, description, city and this hub's public address are shown. Player names are sent to the directory; on a locked stage only the directory's operator sees them. Needs the city, and the ports forwarded." },
        { "stage_genre",        "The genre shown in the stage directory." },
        { "stage_description",  "One line shown in the stage directory, up to 160 characters." },
        { "stage_country",      "The two-letter country code shown in the stage directory, for example US, CA, GB or DE. Players can sort and filter the stage list by it." },
        { "stage_names",        "Who sees player names in the stage directory. Automatic shows them when the stage has no password and hides them from the public when it has one. The directory's operator always sees them." },
        { "samplerate",         "The sample rate of the session. Every player follows the hub." },
        { "blocksize",          "Audio block size. Smaller blocks mean lower latency and a heavier load on every machine." },
        { "log_file",           "Where the hub writes its log. It starts a new file when this one reaches 50 MB." },
        { "web_port",           "Port for the hub's web status page. 0 turns the page off." },
        { "web_password",       "Password for changing settings from the web page." },
        { "location",           "The stage's city, shown on the web page and in the stage directory. Required to list the stage." },
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

// ─── string accessors ─────────────────────────────────────────────────────────
juce::String getValueString(const HubLaunchArgs& a, const std::string& key) {
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
            fprintf(stderr, "[HUB-SETTINGS] %s=%s (launch env)\n", ek.env, cur);
            continue;
        }
        const std::string v = std::to_string(a.*(ek.member));
        setenv(ek.env, v.c_str(), 1);
        fprintf(stderr, "[HUB-SETTINGS] %s=%s (%s)\n", ek.env, v.c_str(), cliPinned ? "cli" : "settings");
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
    hub.setPortCheckHelper(a.portCheckHelper);   
    if (!a.lanSubnetCidr.empty()) hub.setLanSubnet(a.lanSubnetCidr);
    hub.setSinkBufferMs(a.sinkBufferMs);          // Latency tuning
    hub.setSinkBufferLanMs(a.sinkBufferLanMs);
    hub.setSendPacing(a.sendPace);                // Paced fan-out
    hub.setPaceDither(a.paceDither);
    hub.setPaceSpreadPct(a.paceSpread);
    hub.setPaceMinSlotUs(a.paceMinSlotUs);
    hub.setCutThrough(a.cutThrough);              // Cut-through emission
    hub.setInlineListeners(a.inlineListeners);    // (rt-split A)
    hub.setBundling(a.bundle);                    // Hub diet v2
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

void redirectLogFile(const std::string& path) {
    if (path.empty()) return;
    juce::File(juce::String::fromUTF8(path.c_str())).getParentDirectory().createDirectory();
    rotateLogIfLarge(path);
    // `open`-launched apps have no tty, so [HUB-JIT]/[HUB-ABUF]
    // telemetry would vanish without this. Line-buffered so tail -f works.
    if (freopen(path.c_str(), "a", stdout)) setvbuf(stdout, NULL, _IOLBF, 0);
    if (freopen(path.c_str(), "a", stderr)) setvbuf(stderr, NULL, _IOLBF, 0);
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
    (void)argvIn;
    err = "relaunch not implemented on Windows";
    return false;
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

} // namespace hub_settings
} // namespace sb_hub_gui
