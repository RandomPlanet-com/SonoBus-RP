// SPDX-License-Identifier: GPLv3-or-later WITH Appstore-exception
// Copyright (C) 2026 Jason Buecker (Random Planet)
// SonoBus RP — Hub Server GUI implementation.

#include "SbLatencyColours.h"
#include "SbRateFormat.h"
#include "SonobusHubMainWindow.h"
#include "SonobusPlusVersion.h"   // About window (version + commit from Info.plist)
#include "SonobusHubGeo.h"        // HubBuildId
#include "HubBinaryData.h"        // hub icon for the About window (juce_add_binary_data SonoBusHubData)

#include <algorithm>
#include <chrono>
#include <cmath>   // Std::lround in hubRateText
#include <climits>
#include <csignal>
#include <iostream>

#ifdef _WIN32
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif

namespace sb_hub_gui {

// ─── Colour palette (dark, spiritually consistent with SonoBus client) ────────
namespace pal {
    static const juce::Colour bg              = juce::Colour::fromRGB(0x1c, 0x1d, 0x21);
    static const juce::Colour panel           = juce::Colour::fromRGB(0x25, 0x27, 0x2c);
    static const juce::Colour panelLight      = juce::Colour::fromRGB(0x32, 0x35, 0x3a);
    static const juce::Colour text            = juce::Colour::fromRGB(0xe0, 0xe2, 0xe6);
    static const juce::Colour textDim         = juce::Colour::fromRGB(0x9b, 0x9e, 0xa5);
    static const juce::Colour accent          = juce::Colour::fromRGB(0x3e, 0xb5, 0xa9);  // teal
    static const juce::Colour accentSoft      = juce::Colour::fromRGB(0x2d, 0x6f, 0x69);
    static const juce::Colour green           = juce::Colour::fromRGB(0x6a, 0xbe, 0x4f);
    static const juce::Colour yellow          = juce::Colour::fromRGB(0xd6, 0xb0, 0x3e);
    static const juce::Colour orange          = juce::Colour::fromRGB(0xe0, 0x8a, 0x3c);
    static const juce::Colour red             = juce::Colour::fromRGB(0xc7, 0x5a, 0x5a);
}

// Logging stays plain stdout/stderr (tail -f hub.log or Console.app). A mutex-guarded tee would make the AOO
// receive thread contend with the GUI reader on every LOG_WARNING and drop /sb/hxfeedreq dispatch.

// ─── StatusHeader ─────────────────────────────────────────────────────────────
// 12-hour clock text, e.g. "1:43:56 pm".
static juce::String hubClock12h(const juce::Time& t) {
    int h = t.getHours();
    const bool pm = h >= 12;
    h %= 12; if (h == 0) h = 12;
    return juce::String(h) + ":" + juce::String(t.getMinutes()).paddedLeft('0', 2)
         + ":" + juce::String(t.getSeconds()).paddedLeft('0', 2) + (pm ? " pm" : " am");
}
// Bytes/s as "228 KB/s" / "3.10 MB/s"; "-" when unknown.
static juce::String hubRateText(float bytesPerSec) {
    // BITS, one rule for hub + client (SbRateFormat.h): "328 kb/s", "2.6 Mb/s" — never "41.2138 K…"
    return sbplus::rateTextBytes(bytesPerSec);
}

// "9m 11s" / "1h 02m" for "Connected for" and uptime.
static juce::String hubDurText(int64_t secs) {
    if (secs < 0) secs = 0;
    const int64_t h = secs / 3600, m = (secs / 60) % 60, sec = secs % 60;
    if (h > 0) return juce::String(h) + "h " + juce::String(m).paddedLeft('0', 2) + "m";
    return juce::String(m) + "m " + juce::String(sec).paddedLeft('0', 2) + "s";
}
// "1:43 pm" local time from epoch ms.
static juce::String hubClockShort(int64_t epochMs) {
    juce::Time t(epochMs);
    int h = t.getHours(); const bool pm = h >= 12; h %= 12; if (h == 0) h = 12;
    return juce::String(h) + ":" + juce::String(t.getMinutes()).paddedLeft('0', 2) + (pm ? " pm" : " am");
}

StatusHeader::StatusHeader() {
    mIcon = juce::ImageCache::getFromMemory(HubBinaryData::sonobus_hub_icon_mac_256_png, HubBinaryData::sonobus_hub_icon_mac_256_pngSize);   // 
    mPlatform = juce::String::fromUTF8(hubPlatformString().c_str());
}

void StatusHeader::setSnapshot(const SonobusHubServer::UiSnapshot& s) {
    mSnap = s;
    repaint();
}

// The same header the Linux web page shows — icon, "SonoBus RP Hub — group",
// platform · location, the key/value line (uptime, players, audio, codec, CPU, server, UDP, version), then the
// two-tier port check (local + external, verified time in LOCAL time).
void StatusHeader::paint(juce::Graphics& g) {
    g.fillAll(pal::panel);
    int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    int64_t uptimeS = (mSnap.hubStartedAtMs > 0) ? (nowMs - mSnap.hubStartedAtMs) / 1000 : 0;

    auto bounds = getLocalBounds().reduced(14, 10);
    // icon top-left, title + platform line beside it
    const int iconSz = 56;
    if (mIcon.isValid()) g.drawImage(mIcon, juce::Rectangle<float>((float) bounds.getX(), (float) bounds.getY(), (float) iconSz, (float) iconSz), juce::RectanglePlacement::centred);
    auto titleArea = bounds.removeFromTop(iconSz).withTrimmedLeft(iconSz + 12);
    g.setFont(juce::Font(24.0f, juce::Font::bold));
    g.setColour(pal::accent);
    g.drawText(juce::String::fromUTF8("SonoBus RP Hub \xe2\x80\x94 ") + juce::String::fromUTF8(mSnap.groupName.c_str()), titleArea.removeFromTop(30), juce::Justification::centredLeft);
    g.setFont(juce::Font(16.0f));
    g.setColour(pal::textDim);
    g.drawText(mPlatform + (mLocation.isNotEmpty() ? juce::String::fromUTF8("  \xc2\xb7  ") + mLocation : juce::String()), titleArea.removeFromTop(22), juce::Justification::centredLeft);

    bounds.removeFromTop(6);
    // key/value line
    juce::String host = !mSnap.publicAddressHost.empty() ? juce::String(mSnap.publicAddressHost)
                      : !mSnap.advertisedPublicHost.empty() ? juce::String(mSnap.advertisedPublicHost) : juce::String("(auto)");
    struct KV { juce::String k, v; };
    const KV kvs[] = {
        { "uptime", hubDurText(uptimeS) }, { "players", juce::String((int) mSnap.peers.size()) },
        { "audio", juce::String(mSnap.sampleRate) + " Hz / " + juce::String(mSnap.blockSize) + " samples" },
        { "codec", "PCM 16-bit" }, { "hub CPU", juce::String(mSnap.hubCpuPct, 1) + "%" },
        { "server", host + (mSnap.aooServerPort > 0 ? ":" + juce::String(mSnap.aooServerPort) : juce::String()) },
        { "audio UDP", juce::String(mSnap.localUdpPort) },
        { "version", juce::String("Hub ") + SONOBUS_HUB_VERSION + "  (build " + juce::String(hubBuildId()) + ", " + juce::String(hubCommitId()) + ")" } };   // Date build id
    auto kvRow = bounds.removeFromTop(24);
    int x = kvRow.getX();
    const juce::Font fk(15.0f), fv(15.0f, juce::Font::bold);
    for (auto& kv : kvs) {
        g.setFont(fk); g.setColour(pal::textDim);
        int w = (int) fk.getStringWidthFloat(kv.k) + 6;
        g.drawText(kv.k, x, kvRow.getY(), w, kvRow.getHeight(), juce::Justification::centredLeft); x += w;
        g.setFont(fv); g.setColour(pal::text);
        w = (int) fv.getStringWidthFloat(kv.v) + 22;
        g.drawText(kv.v, x, kvRow.getY(), w, kvRow.getHeight(), juce::Justification::centredLeft); x += w;
    }
    // Port check: local tier (dim) + external tier (coloured)
    g.setFont(juce::Font(15.0f));
    if (!mSnap.portLocalText.empty()) {
        g.setColour(pal::textDim);
        g.drawText(juce::String(mSnap.portLocalText), bounds.removeFromTop(20), juce::Justification::centredLeft);
    }
    if (!mSnap.portCheckText.empty()) {
        juce::String t(mSnap.portCheckText);
        if (mSnap.portCheckVerifiedAtMs > 0) t = t.replaceFirstOccurrenceOf(")", " at " + hubClockShort(mSnap.portCheckVerifiedAtMs) + ")");
        g.setColour(mSnap.portCheckState == 1 ? pal::green : mSnap.portCheckState == 2 ? pal::red : pal::textDim);
        g.drawText(t, bounds.removeFromTop(20), juce::Justification::centredLeft);
    }
    if (mUpdateLine.isNotEmpty()) {   // Update check against randomplanet.com
        g.setColour(mUpdateUrl.isNotEmpty() ? juce::Colours::orange : pal::textDim);
        g.drawText(mUpdateLine, bounds.removeFromTop(20), juce::Justification::centredLeft);
    }
}

// ─── PeerTable ────────────────────────────────────────────────────────────────
void PeerTable::setSnapshot(std::vector<SonobusHubServer::UiPeerSnapshot> peers) {
    mPeers = std::move(peers);
    applySort();   // The chosen column order survives the 1-s refresh
    repaint();
}

// Sort keys per column (indexes follow colName[] in paint). Numeric columns sort by the value behind the cell
// (a pair sorts by its first value); unknown / "-" = -1 = lowest. Text columns sort case-insensitively.
static bool peerSortIsText(int col) { return col == 0 || col == 1 || col == 3 || col == 14; }
static double peerSortNumber(const SonobusHubServer::UiPeerSnapshot& p, int col, int64_t nowMs) {
    switch (col) {
        case 2: return p.recvChannels;
        case 4: return p.inBytesPerSec;
        case 5: return (double) p.lostBlocks;
        case 6: {
            float toHub = 0.0f, fromHub = 0.0f;
            const bool have = SonobusHubServer::computeHubLatency(p.smoothedRttUpMs, p.smoothedRttMs, p.sinkBufMs, toHub, fromHub, p.clientRecvBufMs);
            return have ? toHub : -1.0;
        }
        case 7: return p.jitterBufferFillPct;
        case 8: return p.jitP99Ms;
        case 9: return p.sinkBufMs;
        case 10: return p.clientRxUnderruns10s >= 0 ? (double) (p.clientRxUnderrunsTotal + p.clientRxPopsTotal) : -1.0;
        case 11: return p.clientLatFeedAgeS;
        case 12: return p.clientDevBuf;
        case 13: return p.joinedAtMs > 0 ? (double) (nowMs - p.joinedAtMs) : -1.0;
        default: return 0.0;
    }
}
static juce::String peerSortText(const SonobusHubServer::UiPeerSnapshot& p, int col) {
    switch (col) {
        case 0: return juce::String::fromUTF8(p.userName.c_str());
        case 1: return juce::String::fromUTF8(p.location.c_str());
        case 3: return p.isSendMuted ? "muted" : p.lastBlocksNonZero > 0 ? "sending" : "silent";
        case 14: return juce::String::fromUTF8(p.clientVersion.c_str());
        default: return {};
    }
}
void PeerTable::applySort() {
    if (mSortCol < 0) return;
    const int col = mSortCol; const bool desc = mSortDesc;
    const int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    std::stable_sort(mPeers.begin(), mPeers.end(), [col, desc, nowMs](const SonobusHubServer::UiPeerSnapshot& a, const SonobusHubServer::UiPeerSnapshot& b) {
        int r;
        if (peerSortIsText(col)) r = peerSortText(a, col).compareNatural(peerSortText(b, col), false);
        else { const double x = peerSortNumber(a, col, nowMs), y = peerSortNumber(b, col, nowMs); r = x < y ? -1 : x > y ? 1 : 0; }
        return desc ? r > 0 : r < 0;
    });
}
void PeerTable::mouseDown(const juce::MouseEvent& e) {
    if (e.y >= kHeaderH || e.mods.isPopupMenu()) return;   // header row only
    int col = -1;
    for (size_t i = 0; i + 1 < mColX.size(); ++i) if (e.x >= mColX[i] && e.x < mColX[i + 1]) { col = (int) i; break; }
    if (col < 0) return;
    if (col != mSortCol) { mSortCol = col; mSortDesc = false; }
    else if (!mSortDesc) mSortDesc = true;
    else mSortCol = -1;   // third click: back to the hub's order
    applySort();
    repaint();
}

// The sort arrow is DRAWN
static void hubDrawSortArrow(juce::Graphics& g, float x, float cy, bool desc) {
    const float w = 4.0f, h = 3.5f, cx = x + w + 1.0f;
    juce::Path p;
    if (desc) p.addTriangle(cx - w, cy - h, cx + w, cy - h, cx, cy + h);
    else      p.addTriangle(cx - w, cy + h, cx + w, cy + h, cx, cy - h);
    g.fillPath(p);
}
void PeerTable::paint(juce::Graphics& g) {
    // Same columns as the web page, all left-aligned: Player, Location, Inputs, Signal, Up / Down, Lost (hub),
    // To / From hub, JitBuf, Net jitter, Hub / Player buf (A/M), Dropouts (xN = overloads), Lat feed, Device,
    // Connected, Version. The address is in the name's tooltip. The Player and Location columns grow to their
    // longest text, so nothing truncates.
    constexpr int kNumCols = 15;
    const int paddingX = 10;
    auto rowFont = juce::Font(16.0f);
    auto unitFont = juce::Font(12.0f);
    auto headFont = juce::Font(14.0f, juce::Font::bold);
    static const int colWFixed[kNumCols] = { 150, 110, 64, 86, 126, 94, 152, 70, 112, 166, 112, 78, 86, 106, 134 };
    static const char* colName[kNumCols] = { "Player", "Location", "Inputs", "Signal", "Up / Down Mb/s", "Lost (hub)", "To / From hub",
                                             "JitBuf", "Net jitter", "Hub / Player buf", "Dropouts", "Lat feed", "Device", "Connected", "Version" };
    int colW[kNumCols]; for (int i = 0; i < kNumCols; ++i) colW[i] = colWFixed[i];
    {
        int w = colWFixed[0], lw = colWFixed[1];
        for (auto& p : mPeers) {
            w = std::max(w, rowFont.getStringWidth(juce::String::fromUTF8(p.userName.c_str())) + 40);   // + record dot
            lw = std::max(lw, rowFont.getStringWidth(juce::String::fromUTF8(p.location.c_str())) + 18);
        }
        mUserColW = w; colW[0] = w; colW[1] = lw;
    }
    mColX.assign(kNumCols + 1, 0);
    { int x = paddingX; for (int i = 0; i < kNumCols; ++i) { mColX[i] = x; x += colW[i]; } mColX[kNumCols] = x; mRequiredWidth = x + paddingX; }
    const int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();

    auto area = getLocalBounds();
    auto header = area.removeFromTop(kHeaderH);
    g.setColour(pal::panelLight);
    g.fillRect(header);
    g.setFont(headFont);
    for (int i = 0; i < kNumCols; ++i) {
        const juce::String label(colName[i]);
        g.setColour(pal::textDim);
        g.drawText(label, mColX[i], header.getY(), colW[i], header.getHeight(), juce::Justification::centredLeft);
        if (i == mSortCol) {   // The arrow sits right of the heading, in white
            g.setColour(juce::Colours::white);
            hubDrawSortArrow(g, (float) mColX[i] + headFont.getStringWidthFloat(label) + 4.0f, header.getCentreY(), mSortDesc);
        }
    }

    // A cell made of several pieces, each with its own colour; "unit" pieces use the small font.
    struct Part { juce::String text; juce::Colour colour; bool unit = false; };
    const juce::String dash("-");
    const Part slash { " / ", pal::textDim };
    auto msParts = [&](std::vector<Part>& out, bool have, float ms, juce::Colour col) {
        if (! have) { out.push_back({ dash, pal::textDim }); return; }
        out.push_back({ juce::String(ms, 1), col });
        out.push_back({ " ms", pal::textDim, true });
    };

    g.setFont(rowFont);
    int y = area.getY();
    for (size_t r = 0; r < mPeers.size(); ++r) {
        auto& p = mPeers[r];
        g.setColour((r % 2) == 0 ? pal::panel : pal::panel.brighter(0.06f));
        g.fillRect(area.getX(), y, area.getWidth(), kRowH);
        int c = 0;
        auto cell = [&](const juce::String& text, juce::Colour col) { g.setColour(col); g.drawText(text, mColX[c], y, colW[c] - 6, kRowH, juce::Justification::centredLeft); ++c; };
        auto cellParts = [&](const std::vector<Part>& parts) {
            int x = mColX[c]; const int right = mColX[c] + colW[c] - 6;
            for (auto& pt : parts) {
                const juce::Font& f = pt.unit ? unitFont : rowFont;
                const juce::String t = pt.text.trimStart();   // spaces become plain gaps
                const int w = f.getStringWidth(pt.text), lead = w - f.getStringWidth(t);
                if (x + w > right) break;
                g.setFont(f); g.setColour(pt.colour);
                g.drawText(t.trimEnd(), x + lead, y + (pt.unit ? 2 : 0), w - lead + 2, kRowH, juce::Justification::centredLeft);
                x += w;
            }
            g.setFont(rowFont); ++c;
        };

        {
            juce::String name = (p.isRecording ? juce::String::fromUTF8("\xe2\x97\x8f ") : juce::String()) + juce::String::fromUTF8(p.userName.c_str());
            cell(name, p.isRecording ? juce::Colours::red : pal::text);
        }
        cell(p.location.empty() ? juce::String::fromUTF8("\xe2\x80\xa6") : juce::String::fromUTF8(p.location.c_str()), p.location.empty() ? pal::textDim : pal::text);
        cell(juce::String(p.recvChannels), pal::text);
        const bool sending = p.lastBlocksNonZero > 0;
        if (p.isSendMuted) cell(juce::String::fromUTF8("\xe2\x97\x8f ") + "muted", pal::textDim);   // Player muted its send
        else cell(juce::String::fromUTF8("\xe2\x97\x8f ") + (sending ? "sending" : "silent"), sending ? pal::green : pal::textDim);
        {   // Up / Down in Mb/s (bits)
            auto mb = [&](float bytesPerSec) { return bytesPerSec >= 0.0f ? Part { juce::String(bytesPerSec * 8.0f / 1.0e6f, 1), pal::text } : Part { dash, pal::textDim }; };
            cellParts({ mb(p.inBytesPerSec), slash, mb(p.outBytesPerSec) });
        }
        cell(juce::String((juce::int64) p.lostBlocks), p.lostBlocks == 0 ? pal::textDim : p.lostBlocks < 100 ? pal::yellow : pal::red);
        {   // To / From hub
            float toHub = 0.0f, fromHub = 0.0f;
            const bool have = SonobusHubServer::computeHubLatency(p.smoothedRttUpMs, p.smoothedRttMs, p.sinkBufMs, toHub, fromHub, p.clientRecvBufMs);
            auto latColour = [](float ms){ return ms < sbplus::kLatOneWayGreenMs ? pal::green : ms < sbplus::kLatOneWayYellowMs ? pal::yellow : pal::red; };   // Shared thresholds (SbLatencyColours.h)
            std::vector<Part> parts;
            msParts(parts, have, toHub, latColour(toHub));
            parts.push_back(slash);
            msParts(parts, have, fromHub, latColour(fromHub));
            cellParts(parts);
        }
        if (p.jitterBufferFillPct >= 0.0f)   // JitBuf
            cell(juce::String((int) p.jitterBufferFillPct) + "%", p.jitterBufferFillPct < 15.0f ? pal::red : p.jitterBufferFillPct < 95.0f ? pal::yellow : pal::green);
        else cell(dash, pal::textDim);
        {   // Net jitter: the same thresholds as the web page (up to 1.5 green, up to 3 yellow, else red)
            std::vector<Part> parts;
            msParts(parts, p.jitP99Ms >= 0.0f, p.jitP99Ms, p.jitP99Ms <= 1.5f ? pal::green : p.jitP99Ms <= 3.0f ? pal::yellow : pal::red);
            if (p.jitP99Ms >= 0.0f) parts.push_back({ p.isLan ? " LAN" : " WAN", pal::textDim });
            cellParts(parts);
        }
        {   // Hub buf (the hold in force, "!" at its ceiling) / Player buf (as the player reports it, A = Auto, M = Manual)
            std::vector<Part> parts;
            msParts(parts, p.sinkBufMs > 0.0f, p.sinkBufMs, p.holdAtCeiling ? pal::red : p.sinkBufMs <= 12.0f ? pal::green : pal::text);
            if (p.sinkBufMs > 0.0f && p.holdAtCeiling) parts.push_back({ " !", pal::red });
            parts.push_back(slash);
            msParts(parts, p.clientRecvBufMs >= 0.0f, p.clientRecvBufMs, pal::text);
            if (p.clientRecvBufMs >= 0.0f && (p.clientRecvBufMode == 1 || p.clientRecvBufMode == 0)) parts.push_back({ p.clientRecvBufMode == 1 ? " A" : " M", pal::textDim });
            cellParts(parts);
        }
        // Dropouts: underruns + pops since joined; "+n" = the last 10 s; xN = CoreAudio overloads in the last 10 s
        if (p.clientRxUnderruns10s >= 0) {
            const int total = (int) (p.clientRxUnderrunsTotal + p.clientRxPopsTotal);
            const int recent = p.clientRxUnderruns10s + std::max(0, p.clientRxPops10s);
            std::vector<Part> parts { { juce::String(total), total == 0 ? pal::textDim : pal::text } };
            if (recent > 0) parts.push_back({ " (+" + juce::String(recent) + ")", pal::yellow });
            if (p.clientXruns10s > 0) parts.push_back({ " x" + juce::String(p.clientXruns10s), pal::red });
            cellParts(parts);
        } else cell(dash, pal::textDim);
        // Lat feed: age of the last hub latency report the player received
        if (p.clientLatFeedAgeS >= 0) cell(juce::String(p.clientLatFeedAgeS) + "s", p.clientLatFeedAgeS <= 35 ? pal::green : pal::red);
        else cell(dash, pal::textDim);
        // Device: the player's buffer @ sample rate
        if (p.clientDevBuf > 0 && p.clientDevSr > 0) {
            const juce::String sr = (p.clientDevSr % 1000 == 0) ? juce::String(p.clientDevSr / 1000) + "k" : juce::String(p.clientDevSr / 1000.0, 1) + "k";
            const bool off = p.clientDevBuf != 32 || p.clientDevSr != 48000;
            cell(juce::String(p.clientDevBuf) + " @ " + sr, off ? juce::Colours::orange : pal::text);
        } else cell(dash, pal::textDim);
        cell(p.joinedAtMs > 0 ? hubDurText((nowMs - p.joinedAtMs) / 1000) : dash, pal::textDim);
        {   // The player's version and system; older players do not report it
            const juce::String full = juce::String::fromUTF8(p.clientVersion.c_str());
            juce::String v = full.upToFirstOccurrenceOf(" (", false, false);   // marketing version; the build id stays in the log
            if (full.contains("-dev")) v << " dev";
            if (v.isNotEmpty() && ! p.clientOs.empty()) v << juce::String::fromUTF8(" \xc2\xb7 ") << juce::String::fromUTF8(p.clientOs.c_str());
            cell(v.isNotEmpty() ? v : dash, pal::textDim);
        }

        y += kRowH;
        if (y > area.getBottom()) break;
    }

    if (mPeers.empty()) {
        g.setColour(pal::textDim);
        g.setFont(juce::Font(16.0f, juce::Font::italic));
        g.drawText("(no players connected)", area, juce::Justification::centred);
    }
}

// Tooltips follow the cell under the mouse
void PeerTable::mouseMove(const juce::MouseEvent& e) {
    const int row = (e.y - kHeaderH) >= 0 ? (e.y - kHeaderH) / kRowH : -1;
    int col = -1;
    for (size_t i = 0; i + 1 < mColX.size(); ++i) if (e.x >= mColX[i] && e.x < mColX[i + 1]) { col = (int) i; break; }
    mHoverRow = (row >= 0 && row < (int) mPeers.size()) ? row : -1;
    mHoverCol = col;
}

juce::String PeerTable::getTooltip() {
    if (mHoverRow < 0 || mHoverRow >= (int) mPeers.size()) return {};
    const auto& p = mPeers[(size_t) mHoverRow];
    if (mHoverCol == 0) {   // User: address + audio device (+ feed warning)
        juce::String t = juce::String(p.ipAddr) + ":" + juce::String(p.port);
        if (p.clientDevBuf > 0 && p.clientDevSr > 0) {
            const juce::String sr = (p.clientDevSr % 1000 == 0) ? juce::String(p.clientDevSr / 1000) + "k" : juce::String(p.clientDevSr / 1000.0, 1) + "k";
            t << "   audio " << p.clientDevBuf << " @ " << sr;
        }
        if (p.clientLatFeedAgeS > 35) t << "\n" << juce::String::fromUTF8("\xe2\x9a\xa0 the hub's latency report has not reached this player for ") << p.clientLatFeedAgeS << " s";
        return t;
    }
    return {};
}

// ─── RecentTable ─────────────────────────────────────────
// A real juce::ScrollBar (shown only when the rows do not fit) + wheel, header click sorts (▲, ▼, hub order).
static int hubCiCompare(const std::string& a, const std::string& b) { return juce::String::fromUTF8(a.c_str()).compareIgnoreCase(juce::String::fromUTF8(b.c_str())); }

static constexpr int kRecentRemoveCol = 6;
RecentTable::RecentTable() {
    addChildComponent(mScroll);
    mScroll.setAutoHide(false);
    mScroll.setSingleStepSize(1.0);
    mScroll.setColour(juce::ScrollBar::thumbColourId, pal::accent);   
    mScroll.setColour(juce::ScrollBar::trackColourId, pal::panelLight.brighter(0.25f));
    mScroll.addListener(this);
}
void RecentTable::setRows(std::vector<SonobusHubServer::RecentPlayer> rows) {
    mRowsHubOrder = std::move(rows);
    applySort();
    layoutScroll();
    repaint();
}
void RecentTable::applySort() {
    mRows = mRowsHubOrder;
    if (mSortCol < 0) return;
    const int col = mSortCol; const bool desc = mSortDesc;
    std::stable_sort(mRows.begin(), mRows.end(), [col, desc](const SonobusHubServer::RecentPlayer& a, const SonobusHubServer::RecentPlayer& b) {
        int c = 0;
        switch (col) {
            case 0: c = hubCiCompare(a.user, b.user); break;
            case 1: c = hubCiCompare(a.location, b.location); break;
            case 2: c = (a.lastJoinMs < b.lastJoinMs) ? -1 : (a.lastJoinMs > b.lastJoinMs) ? 1 : 0; break;
            case 3: { const int64_t la = a.lastLeftMs < a.lastJoinMs ? INT64_MAX : a.lastLeftMs, lb = b.lastLeftMs < b.lastJoinMs ? INT64_MAX : b.lastLeftMs;   // connected sorts as "latest"
                      c = (la < lb) ? -1 : (la > lb) ? 1 : 0; break; }
            case 4: c = (a.joins < b.joins) ? -1 : (a.joins > b.joins) ? 1 : 0; break;
            case 5: c = (int) a.lan - (int) b.lan; break;
            default: break;
        }
        return desc ? c > 0 : c < 0;
    });
}
void RecentTable::layoutScroll() {
    const int rows = (int) mRows.size();
    const int visible = std::max(1, (getHeight() - kHeaderH) / kRowH);
    mFirstRow = std::max(0, std::min(mFirstRow, rows - visible));
    const bool need = rows > visible;
    mScroll.setBounds(getWidth() - kScrollW, kHeaderH, kScrollW, std::max(0, getHeight() - kHeaderH));
    mScroll.setRangeLimits(0.0, (double) rows, juce::dontSendNotification);
    mScroll.setCurrentRange((double) mFirstRow, (double) visible, juce::dontSendNotification);
    mScroll.setVisible(need);
}
void RecentTable::mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails& wheel) {
    const int visible = std::max(1, (getHeight() - kHeaderH) / kRowH);
    const int maxFirst = std::max(0, (int) mRows.size() - visible);
    const int step = wheel.deltaY > 0 ? -1 : wheel.deltaY < 0 ? 1 : 0;
    const int nf = std::max(0, std::min(maxFirst, mFirstRow + step));
    if (nf != mFirstRow) { mFirstRow = nf; mScroll.setCurrentRangeStart((double) nf, juce::dontSendNotification); repaint(); }
}
void RecentTable::mouseDown(const juce::MouseEvent& e) {
    if (e.mods.isPopupMenu()) return;
    int col = -1;
    for (size_t i = 0; i + 1 < mColX.size(); ++i) if (e.x >= mColX[i] && e.x < mColX[i + 1]) { col = (int) i; break; }
    if (col < 0) return;
    if (e.y >= kHeaderH) {   // the X column removes a player who is not connected
        const int row = mFirstRow + (e.y - kHeaderH) / kRowH;
        if (col != kRecentRemoveCol || row < 0 || row >= (int) mRows.size() || !onRemove) return;
        const auto& p = mRows[(size_t) row];
        if (p.lastLeftMs < p.lastJoinMs) return;
        const std::string user = p.user;
        juce::AlertWindow::showOkCancelBox(juce::MessageBoxIconType::QuestionIcon, "Remove player",
            "Remove \"" + juce::String::fromUTF8(user.c_str()) + "\" from this session's players?",
            "Remove", "Cancel", this,
            juce::ModalCallbackFunction::create([sp = juce::Component::SafePointer<RecentTable>(this), user](int result) {
                if (result == 1 && sp && sp->onRemove) sp->onRemove(user);
            }));
        return;
    }
    if (col == kRecentRemoveCol) return;
    if (col != mSortCol) { mSortCol = col; mSortDesc = false; }
    else if (!mSortDesc) mSortDesc = true;
    else mSortCol = -1;
    applySort(); repaint();
}
void RecentTable::paint(juce::Graphics& g) {
    constexpr int kNumCols = 7;
    int colW[kNumCols] = { 220, 150, 150, 150, 54, 46, 28 };   // The User column takes what is left (the table shares the strip with the sessions list)
    static const char* colName[kNumCols] = { "Player", "Location", "Last joined", "Last left", "Joins", "Net", "" };
    const int paddingX = 10;
    auto area = getLocalBounds();
    if (mScroll.isVisible()) { auto track = area.removeFromRight(kScrollW).withTrimmedTop(kHeaderH); g.setColour(pal::panelLight.brighter(0.25f)); g.fillRect(track); }
    { int fixed = 0; for (int i = 1; i < kNumCols; ++i) fixed += colW[i]; colW[0] = std::max(110, area.getWidth() - 2 * paddingX - fixed); }
    auto header = area.removeFromTop(kHeaderH);
    g.setColour(pal::panelLight); g.fillRect(header);
    g.setColour(pal::textDim); g.setFont(juce::Font(13.0f, juce::Font::bold));
    mColX.clear();
    { int x = paddingX; const juce::Font hf(13.0f, juce::Font::bold); for (int i = 0; i < kNumCols; ++i) { mColX.push_back(x); const juce::String label(colName[i]);
          g.drawText(label, x, header.getY(), colW[i], header.getHeight(), juce::Justification::centredLeft);
          if (i == mSortCol) hubDrawSortArrow(g, (float) x + hf.getStringWidthFloat(label) + 4.0f, (float) header.getCentreY(), mSortDesc);   // Drawn arrow
          x += colW[i]; } mColX.push_back(x); }
    g.setFont(juce::Font(14.0f));
    auto when = [](int64_t ms) { return ms > 0 ? juce::Time(ms).toString(true, true, false, false) : juce::String("-"); };
    int y = area.getY();
    for (size_t r = (size_t) std::max(0, mFirstRow); r < mRows.size(); ++r) {
        const auto& p = mRows[r];
        g.setColour((r % 2) == 0 ? pal::panel : pal::panel.brighter(0.06f));
        g.fillRect(area.getX(), y, area.getWidth(), kRowH);
        int x = paddingX, c = 0;
        auto cell = [&](const juce::String& text, juce::Colour col) { g.setColour(col); g.drawText(text, x, y, colW[c] - 6, kRowH, juce::Justification::centredLeft); x += colW[c]; ++c; };
        cell(juce::String::fromUTF8(p.user.c_str()), pal::text);
        cell(p.location.empty() ? juce::String("-") : juce::String::fromUTF8(p.location.c_str()), p.location.empty() ? pal::textDim : pal::text);
        cell(when(p.lastJoinMs), pal::text);
        const bool connected = p.lastLeftMs < p.lastJoinMs;
        cell(connected ? juce::String("connected") : when(p.lastLeftMs), connected ? pal::green : pal::text);
        cell(juce::String(p.joins), pal::text);
        cell(p.lan ? "LAN" : "WAN", pal::textDim);
        cell(connected ? juce::String() : juce::String(juce::CharPointer_UTF8("\xc3\x97")), pal::red);
        y += kRowH;
        if (y > area.getBottom()) break;
    }
    if (mRows.empty()) {
        g.setColour(pal::textDim); g.setFont(juce::Font(14.0f, juce::Font::italic));
        g.drawText("(no players yet)", area, juce::Justification::centred);
    }
}

// ─── SessionTable ───────────────────────────────────────
SessionTable::SessionTable() {
    addChildComponent(mScroll);
    mScroll.setAutoHide(false);
    mScroll.setSingleStepSize(1.0);
    mScroll.setColour(juce::ScrollBar::thumbColourId, pal::accent);   
    mScroll.setColour(juce::ScrollBar::trackColourId, pal::panelLight.brighter(0.25f));
    mScroll.addListener(this);
}
void SessionTable::setRows(std::vector<SonobusHubServer::SessionRecord> rows, const std::string& currentGroup) {
    mRowsHubOrder = std::move(rows); mCurrentGroup = currentGroup;
    applySort();
    if (!mSelectedGroup.empty()) {   // the selected group may have aged out of the list
        bool still = false; for (const auto& r : mRows) if (r.group == mSelectedGroup) { still = true; break; }
        if (!still) { mSelectedGroup.clear(); if (onSelectionChanged) onSelectionChanged(); }
    }
    layoutScroll();
    repaint();
}
void SessionTable::applySort() {
    mRows = mRowsHubOrder;   // newest (last used) first
    if (mSortCol < 0) return;
    const int col = mSortCol; const bool desc = mSortDesc;
    std::stable_sort(mRows.begin(), mRows.end(), [col, desc](const SonobusHubServer::SessionRecord& a, const SonobusHubServer::SessionRecord& b) {
        int c = 0;
        switch (col) {
            case 0: c = hubCiCompare(a.group, b.group); break;
            case 1: c = (a.firstUsedMs < b.firstUsedMs) ? -1 : (a.firstUsedMs > b.firstUsedMs) ? 1 : 0; break;
            case 2: c = (a.lastUsedMs < b.lastUsedMs) ? -1 : (a.lastUsedMs > b.lastUsedMs) ? 1 : 0; break;
            case 3: c = (a.players.size() < b.players.size()) ? -1 : (a.players.size() > b.players.size()) ? 1 : 0; break;
            default: break;   // link / X columns: no sort
        }
        return desc ? c > 0 : c < 0;
    });
}
void SessionTable::layoutScroll() {
    const int rows = (int) mRows.size();
    const int visible = std::max(1, (getHeight() - kHeaderH) / kRowH);
    mFirstRow = std::max(0, std::min(mFirstRow, rows - visible));
    mScroll.setBounds(getWidth() - kScrollW, kHeaderH, kScrollW, std::max(0, getHeight() - kHeaderH));
    mScroll.setRangeLimits(0.0, (double) rows, juce::dontSendNotification);
    mScroll.setCurrentRange((double) mFirstRow, (double) visible, juce::dontSendNotification);
    mScroll.setVisible(rows > visible);
}
const SonobusHubServer::SessionRecord* SessionTable::selected() const {
    if (mSelectedGroup.empty()) return nullptr;
    for (const auto& r : mRows) if (r.group == mSelectedGroup) return &r;
    return nullptr;
}
void SessionTable::mouseDown(const juce::MouseEvent& e) {
    if (e.mods.isPopupMenu()) return;
    if (e.y < kHeaderH) {   // Header click = sort (▲, ▼, newest-first)
        int col = -1;
        for (size_t i = 0; i + 1 < mColX.size(); ++i) if (e.x >= mColX[i] && e.x < mColX[i + 1]) { col = (int) i; break; }
        if (col < 0 || col > 3) return;
        if (col != mSortCol) { mSortCol = col; mSortDesc = false; }
        else if (!mSortDesc) mSortDesc = true;
        else mSortCol = -1;
        applySort(); repaint();
        return;
    }
    if (mScroll.isVisible() && e.x >= getWidth() - kScrollW) return;
    const int row = mFirstRow + (e.y - kHeaderH) / kRowH;
    // Click on the X = remove that session (asks first; never the current one)
    if (row >= 0 && row < (int) mRows.size() && e.x >= mRemoveColX) {
        const auto& sd = mRows[(size_t) row];
        if (sd.group == mCurrentGroup || !onRemove) return;
        const std::string group = sd.group;
        juce::AlertWindow::showOkCancelBox(juce::MessageBoxIconType::QuestionIcon, "Remove session",
            "Remove \"" + juce::String::fromUTF8(group.c_str()) + "\" from Recent sessions?\n\nIts remembered players and settings go with it.",
            "Remove", "Cancel", this,
            juce::ModalCallbackFunction::create([sp = juce::Component::SafePointer<SessionTable>(this), group](int result) { if (result == 1 && sp && sp->onRemove) sp->onRemove(group); }));
        return;
    }
    // Click in the link column = copy that session's group link (does not change the selection)
    if (row >= 0 && row < (int) mRows.size() && e.x >= mLinkColX && e.x < mRemoveColX && linkFor) {
        const auto& sd = mRows[(size_t) row];
        juce::SystemClipboard::copyTextToClipboard(juce::String::fromUTF8(linkFor(sd).c_str()));
        mCopiedGroup = sd.group; repaint();
        juce::Timer::callAfterDelay(1500, [sp = juce::Component::SafePointer<SessionTable>(this)]() { if (sp) { sp->mCopiedGroup.clear(); sp->repaint(); } });
        return;
    }
    std::string pick = (row >= 0 && row < (int) mRows.size()) ? mRows[(size_t) row].group : std::string();
    if (pick == mSelectedGroup) pick.clear();   // click the selected row again = deselect
    if (pick == mSelectedGroup) return;
    mSelectedGroup = pick;
    repaint();
    if (onSelectionChanged) onSelectionChanged();
}
void SessionTable::mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails& wheel) {
    const int visible = std::max(1, (getHeight() - kHeaderH) / kRowH);
    const int maxFirst = std::max(0, (int) mRows.size() - visible);
    const int step = wheel.deltaY > 0 ? -1 : wheel.deltaY < 0 ? 1 : 0;
    const int nf = std::max(0, std::min(maxFirst, mFirstRow + step));
    if (nf != mFirstRow) { mFirstRow = nf; mScroll.setCurrentRangeStart((double) nf, juce::dontSendNotification); repaint(); }
}
void SessionTable::paint(juce::Graphics& g) {
    constexpr int kNumCols = 6;
    int colW[kNumCols] = { 262, 132, 132, 60, 200, 30 };   // + "X" column (remove)
    static const char* colName[kNumCols] = { "Group", "First used", "Last used", "Players", "Group link", "" };
    const int paddingX = 10;
    auto area = getLocalBounds();
    if (mScroll.isVisible()) { auto track = area.removeFromRight(kScrollW).withTrimmedTop(kHeaderH); g.setColour(pal::panelLight.brighter(0.25f)); g.fillRect(track); }
    {   // the link column takes what the fixed columns (incl. the X column at the far right) leave
        int fixed = 0; for (int i = 0; i < kNumCols; ++i) if (i != kNumCols - 2) fixed += colW[i];
        colW[kNumCols - 2] = std::max(80, area.getWidth() - 2 * paddingX - fixed);
        mLinkColX = paddingX + colW[0] + colW[1] + colW[2] + colW[3];
        mRemoveColX = mLinkColX + colW[kNumCols - 2];
    }
    auto header = area.removeFromTop(kHeaderH);
    g.setColour(pal::panelLight); g.fillRect(header);
    g.setColour(pal::textDim); g.setFont(juce::Font(13.0f, juce::Font::bold));
    mColX.clear();
    { int x = paddingX; const juce::Font hf(13.0f, juce::Font::bold); for (int i = 0; i < kNumCols; ++i) { mColX.push_back(x); const juce::String label(colName[i]);
          g.drawText(label, x, header.getY(), colW[i], header.getHeight(), juce::Justification::centredLeft);
          if (i == mSortCol) hubDrawSortArrow(g, (float) x + hf.getStringWidthFloat(label) + 4.0f, (float) header.getCentreY(), mSortDesc);   // Drawn arrow
          x += colW[i]; } mColX.push_back(x); }
    g.setFont(juce::Font(14.0f));
    auto when = [](int64_t ms) { return ms > 0 ? juce::Time(ms).toString(true, true, false, false) : juce::String("-"); };
    int y = area.getY();
    for (size_t r = (size_t) std::max(0, mFirstRow); r < mRows.size(); ++r) {
        const auto& sd = mRows[r];
        const bool sel = sd.group == mSelectedGroup;
        g.setColour(sel ? pal::accentSoft : (r % 2) == 0 ? pal::panel : pal::panel.brighter(0.06f));
        g.fillRect(area.getX(), y, area.getWidth(), kRowH);
        int x = paddingX, c = 0;
        auto cell = [&](const juce::String& text, juce::Colour col) { g.setColour(col); g.drawText(text, x, y, colW[c] - 6, kRowH, juce::Justification::centredLeft); x += colW[c]; ++c; };
        juce::String name = juce::String::fromUTF8(sd.group.c_str());
        if (!sd.password.empty()) name += "  pw: " + juce::String::fromUTF8(sd.password.c_str());   // Password in clear, plain text
        if (sd.group == mCurrentGroup) name += "  (current)";
        cell(name, sd.group == mCurrentGroup ? pal::green : pal::text);
        cell(when(sd.firstUsedMs), pal::text);
        cell(when(sd.lastUsedMs), pal::text);
        cell(juce::String((int) sd.players.size()), pal::text);
        {   // "Copy link" button-ish cell + the link itself (dim); click copies it
            const bool copied = sd.group == mCopiedGroup;
            juce::Rectangle<int> btn(x, y + 2, 84, kRowH - 4);
            g.setColour(copied ? pal::green : pal::accentSoft.brighter(0.2f)); g.fillRoundedRectangle(btn.toFloat(), 4.0f);
            g.setColour(pal::text); g.setFont(juce::Font(12.0f, juce::Font::bold));
            g.drawText(copied ? "Copied" : "Copy link", btn, juce::Justification::centred);
            g.setFont(juce::Font(14.0f));
            const juce::String link = linkFor ? juce::String::fromUTF8(linkFor(sd).c_str()) : juce::String();
            g.setColour(pal::textDim); g.drawText(link, x + 92, y, colW[c] - 98, kRowH, juce::Justification::centredLeft);
            x += colW[c]; ++c;
        }
        if (sd.group != mCurrentGroup) {   // "X" removes the session (drawn, not a glyph)
            const float cx = (float) x + 12.0f, cy = (float) y + kRowH * 0.5f, rr = 4.5f;
            g.setColour(pal::textDim);
            g.drawLine(cx - rr, cy - rr, cx + rr, cy + rr, 1.6f);
            g.drawLine(cx - rr, cy + rr, cx + rr, cy - rr, 1.6f);
        }
        y += kRowH;
        if (y > area.getBottom()) break;
    }
    if (mRows.empty()) {
        g.setColour(pal::textDim); g.setFont(juce::Font(14.0f, juce::Font::italic));
        g.drawText("(no sessions yet)", area, juce::Justification::centred);
    }
}


// ─── StatusPage ───────────────────────────────────────────────────────────────
StatusPage::StatusPage() {
    addAndMakeVisible(header);
    addAndMakeVisible(peerLabel);
    addAndMakeVisible(peerTable);
    peerLabel.setText("  Players", juce::dontSendNotification);
    peerLabel.setFont(juce::Font(16.0f, juce::Font::bold));
    peerLabel.setColour(juce::Label::textColourId, pal::accent);
    peerLabel.setColour(juce::Label::backgroundColourId, pal::bg);
    // Bottom strip — recent sessions (left) | selected session's personnel + password + Apply (right)
    addAndMakeVisible(sessionsLabel);
    addAndMakeVisible(sessionTable);
    sessionsLabel.setText("  Recent sessions", juce::dontSendNotification);
    sessionsLabel.setFont(juce::Font(16.0f, juce::Font::bold));
    sessionsLabel.setColour(juce::Label::textColourId, pal::accent);
    sessionsLabel.setColour(juce::Label::backgroundColourId, pal::bg);
    sessionsLabel.setTooltip("Groups this hub has served, newest first. Click one to see who was in it; Apply & Restart switches the hub back to that group with the settings it ran with.");
    addAndMakeVisible(recentLabel);
    addAndMakeVisible(recentTable);
    recentLabel.setText("  Players seen in this session", juce::dontSendNotification);
    recentLabel.setFont(juce::Font(16.0f, juce::Font::bold));
    recentLabel.setColour(juce::Label::textColourId, pal::accent);
    recentLabel.setColour(juce::Label::backgroundColourId, pal::bg);
    recentLabel.setTooltip("The distinct players seen in the selected session (from the hub's own join/leave events), newest first");
    addAndMakeVisible(sessionPwLabel);
    addAndMakeVisible(sessionPwEditor);
    addAndMakeVisible(sessionApply);
    sessionPwLabel.setText("Password", juce::dontSendNotification);
    sessionPwLabel.setFont(juce::Font(15.0f));
    sessionPwLabel.setColour(juce::Label::textColourId, pal::text);
    sessionPwEditor.setFont(juce::Font(15.0f));
    // The session password is shown in clear (no password character)
    sessionPwEditor.setColour(juce::TextEditor::backgroundColourId, pal::panelLight);
    sessionPwEditor.setColour(juce::TextEditor::textColourId, pal::text);
    sessionPwEditor.setColour(juce::TextEditor::outlineColourId, pal::panelLight.brighter(0.2f));
    sessionPwEditor.setColour(juce::TextEditor::focusedOutlineColourId, pal::accent);
    sessionPwEditor.setSelectAllWhenFocused(false);
    sessionPwEditor.setTextToShowWhenEmpty("select a session", pal::textDim);
    sessionPwEditor.setTooltip("The password the hub will use for this group - edit it, or clear it for no password");
    sessionPwEditor.setEnabled(false);
    sessionApply.setTooltip("Write this session's group, password and hub settings to hub_settings.json and restart the hub (players must rejoin)");
    sessionApply.setEnabled(false);
    sessionApply.onClick = [this]() { if (const auto* sd = sessionTable.selected(); sd && onApplySession) onApplySession(sd->group, sessionPwEditor.getText().toStdString()); };

    // Group name on the front page.
    addAndMakeVisible(groupLabel);
    addAndMakeVisible(groupEditor);
    addAndMakeVisible(groupApply);
    addAndMakeVisible(messageEditor);
    addAndMakeVisible(messageSend);
    messageEditor.setFont(juce::Font(15.0f));
    messageEditor.setColour(juce::TextEditor::backgroundColourId, pal::panelLight);
    messageEditor.setColour(juce::TextEditor::textColourId, pal::text);
    messageEditor.setColour(juce::TextEditor::outlineColourId, pal::panelLight.brighter(0.2f));
    messageEditor.setColour(juce::TextEditor::focusedOutlineColourId, pal::accent);
    messageEditor.setTextToShowWhenEmpty("Message to the group", pal::textDim);
    messageEditor.setTooltip("Sends a chat message to every player, from Hub.");
    messageSend.setTooltip("Send this message to every player.");
    auto send = [this]() {
        const auto text = messageEditor.getText().trim();
        if (text.isEmpty()) return;
        if (onSendMessage) onSendMessage(text);
        messageEditor.clear();
    };
    messageEditor.onReturnKey = send;
    messageSend.onClick = send;
    groupLabel.setText("Group", juce::dontSendNotification);
    groupLabel.setFont(juce::Font(16.0f, juce::Font::bold));
    groupLabel.setColour(juce::Label::textColourId, pal::accent);
    groupEditor.setFont(juce::Font(16.0f));
    groupEditor.setColour(juce::TextEditor::backgroundColourId, pal::panelLight);
    groupEditor.setColour(juce::TextEditor::textColourId, pal::text);
    groupEditor.setColour(juce::TextEditor::outlineColourId, pal::panelLight.brighter(0.2f));
    groupEditor.setColour(juce::TextEditor::focusedOutlineColourId, pal::accent);
    groupEditor.setSelectAllWhenFocused(false);
    groupEditor.onReturnKey = [this]() { if (onApplyGroup) onApplyGroup(groupEditor.getText().trim()); };
    groupApply.onClick = [this]() { if (onApplyGroup) onApplyGroup(groupEditor.getText().trim()); };
    groupLabel.setTooltip("The group this hub is hosting. Players join by this name.");
    groupEditor.setTooltip("Type a new group name, then click Apply.");
    groupApply.setTooltip("Switch the hub to this group. The hub restarts and players must rejoin.");
    header.setTooltip("This hub's version, addresses, ports and current load.");
    peerLabel.setTooltip("Everyone connected to this hub right now. The heading counts the players connected now and the different players who have joined since the hub started. Hover a column for details.");
    sessionPwLabel.setTooltip("The password for the selected session.");
    sessionTable.setTooltip("Groups this hub has hosted. Click a row to select it, or click a column heading to sort.");
    recentTable.setTooltip("Players seen in the selected session. Click a column heading to sort.");
    // Copy Group Link + link text + Re-run port check, like the web page header.
    addAndMakeVisible(copyLinkButton);
    addAndMakeVisible(linkLabel);
    addAndMakeVisible(portCheckButton);
    addChildComponent(installUpdateButton);
    linkLabel.setFont(juce::Font(juce::Font::getDefaultMonospacedFontName(), 14.0f, juce::Font::plain));
    linkLabel.setColour(juce::Label::textColourId, pal::textDim);
    linkLabel.setMinimumHorizontalScale(1.0f);
    copyLinkButton.onClick = [this]() {
        if (onCopyLink) onCopyLink();
        copyLinkButton.setButtonText("Copied");
        juce::Timer::callAfterDelay(1500, [sp = juce::Component::SafePointer<StatusPage>(this)]() { if (sp) sp->copyLinkButton.setButtonText("Copy Group Link"); });
    };
    portCheckButton.onClick = [this]() { if (onRerunPortCheck) onRerunPortCheck(); };
}

void StatusPage::resized() {
    auto bounds = getLocalBounds();
    // Header bumped 76 → 100 for larger title + two info lines.
    header.setBounds(bounds.removeFromTop(176));   // Icon + title + platform + KV + local/external port check (+ update line)
    // Tool row — Copy Group Link, the link, Re-run port check (right).
    auto trow = bounds.removeFromTop(40).reduced(14, 4);
    portCheckButton.setBounds(trow.removeFromRight(170));
    if (installUpdateButton.isVisible()) { trow.removeFromRight(8); installUpdateButton.setBounds(trow.removeFromRight(130)); }
    copyLinkButton.setBounds(trow.removeFromLeft(160));
    trow.removeFromLeft(10);
    linkLabel.setBounds(trow);
    // Group row under the header.
    auto grow = bounds.removeFromTop(44).reduced(14, 6);
    groupLabel.setBounds(grow.removeFromLeft(64));
    groupEditor.setBounds(grow.removeFromLeft(260));
    grow.removeFromLeft(10);
    groupApply.setBounds(grow.removeFromLeft(200));
    grow.removeFromLeft(24);
    messageSend.setBounds(grow.removeFromRight(80));
    grow.removeFromRight(8);
    messageEditor.setBounds(grow);
    // A fixed strip at the bottom: recent sessions (left 55 %) | the selected session's players with the password
    // field + Apply & Restart under them (right). The live table takes only the rows it has (min 3), the sessions
    // strip gets everything else (min 214).
    const int tableH = 28 + PeerTable::kHeaderH + std::max(3, peerTable.numRows()) * PeerTable::kRowH + 16;
    // Recent sessions on top (sized to its rows, 3..6 visible, more scroll),
    // the selected session's players UNDER it, the password + Apply & Restart row at the very bottom.
    const int sessionsH = 28 + SessionTable::kHeaderH + std::max(3, std::min(6, sessionTable.numRows())) * SessionTable::kRowH + 6;
    const int playersMinH = 28 + 24 + 3 * 22 + 6;
    const int minStrip = sessionsH + playersMinH + 38;
    auto strip = bounds.removeFromBottom(std::max(minStrip, bounds.getHeight() - tableH));
    auto srow = strip.removeFromTop(sessionsH);
    sessionsLabel.setBounds(srow.removeFromTop(28));
    sessionTable.setBounds(srow.withTrimmedBottom(6));
    auto arow = strip.removeFromBottom(38).reduced(0, 4);
    sessionApply.setBounds(arow.removeFromRight(170));
    arow.removeFromRight(10);
    sessionPwLabel.setBounds(arow.removeFromLeft(78));
    sessionPwEditor.setBounds(arow.removeFromLeft(std::min(260, arow.getWidth())));
    recentLabel.setBounds(strip.removeFromTop(28));
    recentTable.setBounds(strip);
    peerLabel.setBounds(bounds.removeFromTop(28));
    peerTable.setBounds(bounds);
}

// ─── LogView / LogWindow ──────────────────────────────
LogView::LogView(juce::File logFile, bool connectionsOnly, bool showControls)
    : mLogFile(std::move(logFile)), mShowControls(showControls) {
    mConnOnly.setToggleState(connectionsOnly, juce::dontSendNotification);
    mConnOnly.setColour(juce::ToggleButton::textColourId, pal::text);
    mConnOnly.setColour(juce::ToggleButton::tickColourId, pal::accent);
    mConnOnly.onClick = [this]() { mLast.clear(); refresh(); };
    mViewLog.setColour(juce::TextButton::buttonColourId, pal::accentSoft);
    mViewLog.setColour(juce::TextButton::textColourOffId, pal::text);
    mViewLog.onClick = [this]() { if (onViewLog) onViewLog(); };
    mConnOnly.setTooltip("Show only players connecting, leaving and being refused.");
    mViewLog.setTooltip("Open the full log in its own window.");
    if (mShowControls) { addAndMakeVisible(mConnOnly); addAndMakeVisible(mViewLog); }
    mText.setMultiLine(true, false);
    mText.setReadOnly(true);
    mText.setCaretVisible(false);
    mText.setScrollbarsShown(true);
    mText.setFont(juce::Font(juce::Font::getDefaultMonospacedFontName(), 13.0f, juce::Font::plain));
    mText.setColour(juce::TextEditor::backgroundColourId, pal::bg);
    mText.setColour(juce::TextEditor::textColourId, pal::text);
    mText.setColour(juce::TextEditor::outlineColourId, pal::panelLight);
    addAndMakeVisible(mText);
    refresh();
    startTimer(2000);
}
LogView::~LogView() { stopTimer(); }
void LogView::paint(juce::Graphics& g) { g.fillAll(pal::panel); }
void LogView::resized() {
    auto b = getLocalBounds().reduced(14, 10);
    if (mShowControls) {
        auto row = b.removeFromTop(32);
        mConnOnly.setBounds(row.removeFromLeft(190));
        mViewLog.setBounds(row.removeFromRight(120));
        b.removeFromTop(6);
    }
    mText.setBounds(b);
}
void LogView::timerCallback() { refresh(); }
void LogView::refresh() {
    juce::String text;
    if (mShowControls && mConnOnly.getToggleState()) {
        for (const auto& l : SonobusHubServer::connLogLines(500)) text << juce::String::fromUTF8(l.c_str()) << "\n";
        if (text.isEmpty()) text = "(no connection events yet)\n";
    } else if (mLogFile == juce::File() || !mLogFile.existsAsFile()) {
        text = "(no log file - set Log file path in Settings)\n";
    } else {
        // last 64 KB of hub.log, from its first full line
        juce::FileInputStream in(mLogFile);
        if (in.openedOk()) {
            const juce::int64 len = in.getTotalLength(), want = 64 * 1024;
            if (len > want) in.setPosition(len - want);
            juce::MemoryBlock mb; in.readIntoMemoryBlock(mb, (ssize_t) juce::jmin<juce::int64>(len, want));
            text = juce::String::fromUTF8((const char*) mb.getData(), (int) mb.getSize());
            if (len > want) text = text.fromFirstOccurrenceOf("\n", false, false);
        }
    }
    if (text == mLast) return;
    mLast = text;
    mText.setText(text, false);
    mText.moveCaretToEnd();
}

LogWindow::LogWindow(juce::File logFile)
    : juce::DocumentWindow(juce::String::fromUTF8("SonoBus RP Hub \xe2\x80\x94 hub.log"), pal::bg, juce::DocumentWindow::allButtons) {
    setUsingNativeTitleBar(true);
    setResizable(true, true);
    setContentOwned(new LogView(std::move(logFile), false, false), false);
    centreWithSize(1100, 700);
    setVisible(true);
}

// ─── SettingsPanel ────────────────────────────────────────────────────────────
namespace {
constexpr int kRowH = 28;
constexpr int kSectionH = 26;
constexpr int kNameW = 215;
constexpr int kTagW = 150;

// Form layout spec lives in hub_settings::formSpecs() (shared with the web page's
// Settings section). kind 3 (password) is shown as a plain text row here.
const auto& kSpecs = hub_settings::formSpecs();

void styleButton(juce::TextButton& b, juce::Colour bg, juce::Colour fg) {
    b.setColour(juce::TextButton::buttonColourId, bg);
    b.setColour(juce::TextButton::buttonOnColourId, bg.brighter(0.15f));
    b.setColour(juce::TextButton::textColourOffId, fg);
    b.setColour(juce::TextButton::textColourOnId, fg);
}
} // namespace

struct SettingsPanel::Row : public juce::Component {
    enum Kind { Text = 0, Bool = 1, Choice = 2 };

    Row(const char* k, Kind kd, const juce::String& label, const char* choiceList)
        : key(k), kind(kd) {
        name.setText(label, juce::dontSendNotification);
        name.setFont(juce::Font(15.0f));
        name.setColour(juce::Label::textColourId, pal::text);
        name.setMinimumHorizontalScale(1.0f);
        addAndMakeVisible(name);

        tag.setFont(juce::Font(13.0f));
        tag.setColour(juce::Label::textColourId, pal::textDim);
        tag.setJustificationType(juce::Justification::centredLeft);
        tag.setMinimumHorizontalScale(1.0f);
        addAndMakeVisible(tag);

        switch (kind) {
        case Text:
            editor = std::make_unique<juce::TextEditor>();
            editor->setFont(juce::Font(15.0f));
            editor->setColour(juce::TextEditor::backgroundColourId, pal::panelLight);
            editor->setColour(juce::TextEditor::textColourId, pal::text);
            editor->setColour(juce::TextEditor::outlineColourId, pal::panelLight.brighter(0.2f));
            editor->setColour(juce::TextEditor::focusedOutlineColourId, pal::accent);
            editor->setSelectAllWhenFocused(false);
            editor->onTextChange = [this]() { if (onEdited) onEdited(*this); };
            addAndMakeVisible(*editor);
            break;
        case Bool:
            toggle = std::make_unique<juce::ToggleButton>();
            toggle->setColour(juce::ToggleButton::textColourId, pal::text);
            toggle->setColour(juce::ToggleButton::tickColourId, pal::accent);
            toggle->setColour(juce::ToggleButton::tickDisabledColourId, pal::textDim);
            toggle->onClick = [this]() { if (onEdited) onEdited(*this); };
            addAndMakeVisible(*toggle);
            break;
        case Choice:
            combo = std::make_unique<juce::ComboBox>();
            combo->setColour(juce::ComboBox::backgroundColourId, pal::panelLight);
            combo->setColour(juce::ComboBox::textColourId, pal::text);
            combo->setColour(juce::ComboBox::outlineColourId, pal::panelLight.brighter(0.2f));
            combo->setColour(juce::ComboBox::arrowColourId, pal::accent);
            if (choiceList) {
                choices.addTokens(juce::String(choiceList), ",", "");
                for (int i = 0; i < choices.size(); ++i) combo->addItem(choices[i], i + 1);
            }
            combo->onChange = [this]() { if (onEdited) onEdited(*this); };
            addAndMakeVisible(*combo);
            break;
        }
        const juce::String tip(juce::CharPointer_UTF8(hub_settings::tipFor(key)));
        name.setTooltip(tip);
        if (editor) editor->setTooltip(tip);
        if (toggle) toggle->setTooltip(tip);
        if (combo)  combo->setTooltip(tip);
    }

    // Canonical text of the control's current value (matches getValueString()).
    juce::String value() const {
        switch (kind) {
        case Text:   return editor->getText();
        case Bool:   return toggle->getToggleState() ? "1" : "0";
        case Choice: {
            const int idx = combo->getSelectedId() - 1;
            return (idx >= 0 && idx < choices.size()) ? choices[idx] : juce::String();
        }
        }
        return {};
    }

    // Canonical text → control, without notifications.
    void setValue(const juce::String& v) {
        switch (kind) {
        case Text:
            editor->setText(v, false);
            break;
        case Bool: {
            bool on = false;
            auto t = v.trim().toLowerCase();
            on = (t == "true" || t == "yes" || t == "on" || t.getIntValue() != 0);
            toggle->setToggleState(on, juce::dontSendNotification);
            break;
        }
        case Choice: {
            int idx = choices.indexOf(v.trim());
            if (idx < 0) {   // value outside the preset list (e.g. blocksize 48): keep it selectable
                choices.add(v.trim());
                idx = choices.size() - 1;
                combo->addItem(choices[idx], idx + 1);
            }
            combo->setSelectedId(idx + 1, juce::dontSendNotification);
            break;
        }
        }
    }

    void setInvalid(bool bad) {
        invalid = bad;
        if (editor) editor->applyColourToAllText(bad ? pal::red : pal::text, true);
    }

    void resized() override {
        auto b = getLocalBounds();
        name.setBounds(b.removeFromLeft(nameW));
        tag.setBounds(b.removeFromRight(kTagW));
        auto c = b.reduced(4, 3);
        if (editor) editor->setBounds(c);
        if (toggle) toggle->setBounds(c);
        if (combo)  combo->setBounds(c);
    }

    std::string key;
    Kind kind;
    juce::Label name;
    juce::Label tag;
    std::unique_ptr<juce::TextEditor> editor;
    std::unique_ptr<juce::ToggleButton> toggle;
    std::unique_ptr<juce::ComboBox> combo;
    juce::StringArray choices;
    bool invalid = false;
    int nameW = kNameW;
    std::function<void(Row&)> onEdited;
};

SettingsPanel::SettingsPanel(const HubLaunchArgs& running, const HubSettingsMeta& meta, Callbacks cb)
    : mRunning(running), mSaved(running), mEdit(running), mMeta(meta), mCb(std::move(cb)) {
    // What the FILE holds may differ from what we're running (CLI/env pins),
    // so re-read it for an honest "unsaved changes" baseline.
    mHaveFile = mMeta.loadedFromFile;
    if (mHaveFile) {
        HubLaunchArgs fileModel;   // defaults + file = exactly what a bare launch would use
        std::string e;
        if (hub_settings::load(mMeta.file, fileModel, e)) { hub_settings::applyDefaults(fileModel); mSaved = fileModel; }
    }

    mPathLabel.setFont(juce::Font(14.0f));
    mPathLabel.setColour(juce::Label::textColourId, pal::textDim);
    juce::String pathText;
    if (mMeta.file == juce::File()) {
        pathText = "Settings file is turned off, so settings cannot be saved.";
    } else {
        pathText << "Settings file: " << mMeta.file.getFullPathName();
        if (!mMeta.loadedFromFile && !mMeta.loadError.empty())
            pathText << "  (could not be read, using defaults: " << juce::String::fromUTF8(mMeta.loadError.c_str()) << ")";
    }
    mPathLabel.setText(pathText, juce::dontSendNotification);
    addAndMakeVisible(mPathLabel);

    mStatus.setFont(juce::Font(14.0f));
    mStatus.setColour(juce::Label::textColourId, pal::text);
    mStatus.setJustificationType(juce::Justification::centredLeft);
    mStatus.setMinimumHorizontalScale(1.0f);
    addAndMakeVisible(mStatus);

    styleButton(mResetButton,       pal::panelLight, pal::textDim);
    styleButton(mRevertButton,      pal::panelLight, pal::textDim);
    styleButton(mSaveButton,        pal::accentSoft, pal::text);
    styleButton(mSaveRestartButton, pal::accent,     pal::bg);
    mRevertButton.onClick = [this]() { controlsFromModel(mHaveFile ? mSaved : mRunning); refreshStatus("Reverted"); };
    mResetButton.onClick = [this]() {
        juce::AlertWindow::showOkCancelBox(juce::MessageBoxIconType::QuestionIcon, "Reset to Defaults",
            "Put every setting back to its default? Your group name and password stay. Nothing changes until you click Save.",
            "Reset", "Cancel", this, juce::ModalCallbackFunction::create([this](int ok) {
                if (ok) resetToDefaults();
            }));
    };
    mResetButton.setTooltip("Put every setting back to its default, keeping your group name and password.");
    mRevertButton.setTooltip("Undo your edits and go back to the last saved settings.");
    mSaveButton.setTooltip("Save these settings. Some take effect after Restart Hub.");
    mSaveRestartButton.setTooltip("Save these settings and restart the hub. Players reconnect automatically.");
    mSaveButton.onClick = [this]() { doSave(); };
    mSaveRestartButton.onClick = [this]() {
        if (!doSave()) return;
        std::string err;
        if (!mCb.relaunch || !mCb.relaunch(err)) refreshStatus("Relaunch FAILED: " + juce::String(err));
        else refreshStatus("Relaunching hub process...");
    };
    addAndMakeVisible(mResetButton);
    addAndMakeVisible(mRevertButton);
    addAndMakeVisible(mSaveButton);
    addAndMakeVisible(mSaveRestartButton);

    mViewport.setViewedComponent(&mForm, false);
    mViewport.setScrollBarsShown(true, false);
    addAndMakeVisible(mViewport);

    buildRows();
    controlsFromModel(mEdit);
    refreshStatus();
}

SettingsPanel::~SettingsPanel() = default;

void SettingsPanel::buildRows() {
    for (auto& sp : kSpecs) {
        if (!sp.key) {
            auto l = std::make_unique<juce::Label>();
            l->setText(sp.label, juce::dontSendNotification);
            l->setFont(juce::Font(15.0f, juce::Font::bold));
            l->setColour(juce::Label::textColourId, pal::accent);
            mForm.addAndMakeVisible(*l);
            mSectionLabels.push_back(std::move(l));
            continue;
        }
        auto r = std::make_unique<Row>(sp.key, (Row::Kind)(sp.kind == 3 ? 0 : sp.kind), sp.label, sp.choices);
        r->onEdited = [this](Row& row) { onRowEdited(row); };
        mForm.addAndMakeVisible(*r);
        mRows.push_back(std::move(r));
    }
    // The name column fits the longest label, so no label is ever shortened.
    int nameW = kNameW;
    for (auto& r : mRows) nameW = std::max(nameW, r->name.getFont().getStringWidth(r->name.getText()) + 14);
    for (auto& r : mRows) r->nameW = nameW;
}

void SettingsPanel::setDetectedAddress(const juce::String& address) {
    if (address == mDetectedAddress && mDetectedShown) return;
    mDetectedAddress = address;
    mDetectedShown = true;
    for (auto& r : mRows) {
        if (r->key != "public_address" || !r->editor) continue;
        r->editor->setTextToShowWhenEmpty(address.isNotEmpty() ? address + " (auto-detected)"
                                                               : juce::String("Not detected, enter your address"),
                                          pal::text.withAlpha(0.65f));
        r->editor->repaint();
    }
}

void SettingsPanel::resetToDefaults() {
    HubLaunchArgs d;
    d.groupName = mEdit.groupName;
    d.groupPassword = mEdit.groupPassword;
    hub_settings::applyDefaults(d);
    controlsFromModel(d);
    refreshStatus("Defaults restored. Click Save to keep them.");
}

void SettingsPanel::layoutForm() {
    const int w = std::max(200, mViewport.getWidth() - mViewport.getScrollBarThickness());
    const int colGap = 24;
    const int colW = (w - colGap) / 2;
    int y[2] = { 6, 6 };
    size_t si = 0, ri = 0;
    for (auto& sp : kSpecs) {
        const int x = (sp.col == 0) ? 0 : colW + colGap;
        if (!sp.key) {
            if (si < mSectionLabels.size()) {
                if (y[sp.col] > 6) y[sp.col] += 10;
                mSectionLabels[si]->setBounds(x, y[sp.col], colW, kSectionH);
                y[sp.col] += kSectionH + 2;
            }
            ++si;
        } else {
            if (ri < mRows.size()) {
                mRows[ri]->setBounds(x, y[sp.col], colW, kRowH);
                y[sp.col] += kRowH;
            }
            ++ri;
        }
    }
    mForm.setSize(w, std::max(y[0], y[1]) + 8);
}

void SettingsPanel::resized() {
    auto b = getLocalBounds().reduced(14, 10);
    auto bottom = b.removeFromBottom(40);
    mSaveRestartButton.setBounds(bottom.removeFromRight(200));
    bottom.removeFromRight(10);
    mSaveButton.setBounds(bottom.removeFromRight(110));
    bottom.removeFromRight(10);
    mRevertButton.setBounds(bottom.removeFromRight(110));
    bottom.removeFromRight(10);
    mResetButton.setBounds(bottom.removeFromRight(150));
    bottom.removeFromRight(14);
    mStatus.setBounds(bottom);
    b.removeFromBottom(8);
    mPathLabel.setBounds(b.removeFromTop(24));
    b.removeFromTop(4);
    mViewport.setBounds(b);
    layoutForm();
}

void SettingsPanel::paint(juce::Graphics& g) {
    g.fillAll(pal::bg);
}

void SettingsPanel::controlsFromModel(const HubLaunchArgs& a) {
    mEdit = a;
    mInvalid.clear();
    for (auto& r : mRows) {
        r->setValue(hub_settings::getValueString(a, r->key));
        r->setInvalid(false);
    }
}

void SettingsPanel::onRowEdited(Row& r) {
    if (hub_settings::setValueFromString(mEdit, r.key, r.value())) {
        mInvalid.erase(r.key);
        r.setInvalid(false);
    } else {
        mInvalid.insert(r.key);
        r.setInvalid(true);
    }
    refreshStatus();
}

void SettingsPanel::refreshStatus(const juce::String& transient) {
    using hub_settings::Tier;
    const auto unsaved = hub_settings::diffKeys(mEdit, mSaved);
    const auto pending = hub_settings::diffKeys(mSaved, mRunning);   // saved but not yet effective
    int needRestart = 0, needRelaunch = 0;
    for (auto& k : pending) {
        switch (hub_settings::tierForKey(k)) {
        case Tier::Relaunch: ++needRelaunch; break;
        case Tier::Restart:  ++needRestart;  break;
        case Tier::Live:     break;
        }
    }

    // Per-row tag: tier + pin + "changed vs running" marker.
    for (auto& r : mRows) {
        juce::String t;
        juce::Colour c = pal::textDim;
        switch (hub_settings::tierForKey(r->key)) {
        case Tier::Live:     t = "live";     c = pal::green;  r->tag.setTooltip("Takes effect as soon as you save.");            break;
        case Tier::Restart:  t = "restart";  c = pal::yellow; r->tag.setTooltip("Takes effect after Restart Hub.");                 break;
        case Tier::Relaunch: t = "relaunch"; c = pal::orange; r->tag.setTooltip("Takes effect after Save & Restart hub.");          break;
        }
        const bool differs = hub_settings::getValueString(mEdit, r->key)
                          != hub_settings::getValueString(mRunning, r->key);
        if (differs) t = juce::String(juce::CharPointer_UTF8("\xe2\x97\x8f ")) + t;   // ● = differs from the running hub
        if (mMeta.cliKeys.count(r->key)) t += juce::String(juce::CharPointer_UTF8(" \xc2\xb7 CLI"));
        if (mMeta.envKeys.count(r->key)) t += juce::String(juce::CharPointer_UTF8(" \xc2\xb7 env"));
        if (r->invalid) { t = "invalid"; c = pal::red; }
        r->tag.setText(t, juce::dontSendNotification);
        r->tag.setColour(juce::Label::textColourId, (differs || r->invalid) ? c : pal::textDim);
    }

    juce::String s;
    if (transient.isNotEmpty()) s << transient << "   ";
    if (!mInvalid.empty()) s << (int)mInvalid.size() << " invalid field(s) — fix before saving.   ";
    if (mMeta.file == juce::File()) {
        s << "Settings cannot be saved in this mode.   ";
    } else if (!mHaveFile) {
        s << "Not saved yet. Click Save to keep these settings.   ";
    }
    if (!unsaved.empty()) s << (int)unsaved.size() << " unsaved change(s).   ";
    if (needRestart || needRelaunch) {
        s << "Saved but not live: ";
        if (needRestart)  s << needRestart << " need Restart Hub";
        if (needRestart && needRelaunch) s << ", ";
        if (needRelaunch) s << needRelaunch << " need Save & Restart (process relaunch)";
        s << ".   ";
    }
    if (mHaveFile && unsaved.empty() && !needRestart && !needRelaunch && mInvalid.empty())
        s << "Settings match the running hub.   ";
    if (!mMeta.cliKeys.empty()) {
        s << "Pinned by launch args this run: ";
        bool first = true;
        for (auto& k : mMeta.cliKeys) { if (!first) s << ", "; s << k; first = false; }
        s << " (Save & Restart drops them so the file applies).";
    }
    mStatus.setText(s, juce::dontSendNotification);

    const bool canSave = mInvalid.empty() && mMeta.file != juce::File();
    mSaveButton.setEnabled(canSave);
    mSaveRestartButton.setEnabled(canSave);
}

bool SettingsPanel::doSave() {
    if (!mInvalid.empty()) { refreshStatus("Fix the invalid field(s) first"); return false; }
    if (auto problem = hub_settings::validate(mEdit); !problem.empty()) {
        refreshStatus("Invalid: " + juce::String(problem));
        return false;
    }
    std::string err;
    if (!mCb.save || !mCb.save(mEdit, err)) {
        refreshStatus("Save FAILED: " + juce::String(err.empty() ? "no save handler" : err));
        return false;
    }
    mSaved = mEdit;
    mHaveFile = true;
    // Live-tier fields are effective as of this save.
    for (auto& f : hub_settings::fields())
        if (f.tier == hub_settings::Tier::Live)
            hub_settings::setValueFromString(mRunning, f.key, hub_settings::getValueString(mEdit, f.key));
    refreshStatus("Saved " + hubClock12h(juce::Time::getCurrentTime()));
    return true;
}

// ─── HubContent ───────────────────────────────────────────────────────────────
HubContent::HubContent(SonobusHubServer* hub,
                       const HubLaunchArgs& running,
                       const HubSettingsMeta& meta,
                       SettingsPanel::Callbacks settingsCb,
                       std::function<void()> restartFn,
                       std::function<void()> quitFn,
                       std::function<void(const std::string&)> applyGroupFn,
                       std::function<void(bool)> applyHoldFn,
                       std::function<void(const std::string&, const std::string&)> applySessionFn)
    : mHub(hub), mRelaunchFn(settingsCb.relaunch), mRestartFn(std::move(restartFn)), mQuitFn(std::move(quitFn)),
      mSettings(running, meta, std::move(settingsCb)) {
    (void) applyHoldFn;   // Jam/Print switch removed
    // Status-page group field.
    mStatusPage.groupEditor.setText(juce::String(running.groupName), juce::dontSendNotification);
    styleButton(mStatusPage.groupApply, pal::accentSoft, pal::text);
    styleButton(mStatusPage.messageSend, pal::accentSoft, pal::text);
    mStatusPage.onSendMessage = [this](const juce::String& text) { if (mHub) mHub->sendGroupMessage(text.toStdString()); };
    mStatusPage.onApplyGroup = [fn = std::move(applyGroupFn)](const juce::String& g) { if (fn) fn(g.toStdString()); };
    // Recent sessions — selection drives the personnel table; Apply & Restart goes to the app.
    styleButton(mStatusPage.sessionApply, pal::accentSoft, pal::text);
    mStatusPage.sessionTable.onSelectionChanged = [this]() { mStatusPage.sessionPwEditor.clear(); refreshSessionDetail(); };
    // The X on a row — remove that session from the hub's list (hub_sessions.json) and redraw at once
    mStatusPage.sessionTable.onRemove = [this](const std::string& group) {
        if (!mHub) return;
        std::string err;
        if (!mHub->removeSession(group, err)) { std::cout << "[HUB-SESSION] remove '" << group << "' refused: " << err << "\n"; return; }
        mStatusPage.sessionTable.setRows(mHub->sessions(), mHub->snapshotForUI().groupName);
        mStatusPage.resized();
        refreshSessionDetail();
    };
    mStatusPage.recentTable.onRemove = [this](const std::string& user) {
        if (!mHub) return;
        const auto* sel = mStatusPage.sessionTable.selected();
        const std::string group = sel ? sel->group : mHub->snapshotForUI().groupName;
        std::string err;
        if (!mHub->removeSessionPlayer(group, user, err)) { std::cout << "[HUB-SESSION] remove player '" << user << "' refused: " << err << "\n"; return; }
        mStatusPage.sessionTable.setRows(mHub->sessions(), mHub->snapshotForUI().groupName);
        refreshSessionDetail();
    };
    mStatusPage.onApplySession = std::move(applySessionFn);
    // Tab pages are members; the TabbedComponent does NOT own them (deleteComponentWhenNotNeeded=false).
    mTabs.setTabBarDepth(32);
    mTabs.setOutline(0);
    mTabs.setColour(juce::TabbedComponent::backgroundColourId, pal::bg);
    mTabs.setColour(juce::TabbedButtonBar::tabTextColourId, pal::textDim);
    mTabs.setColour(juce::TabbedButtonBar::frontTextColourId, pal::text);
    // Log tab + header tools + player locations (the Linux web page's surface, natively).
    mGeoEnabled = running.geoip;
    mLogFile = running.logFile.empty() ? juce::File() : juce::File(juce::String::fromUTF8(running.logFile.c_str()));
    mGeo.start();
    mLogPage = std::make_unique<LogView>(mLogFile, true, true);
    mLogPage->onViewLog = [this]() {
        if (!mLogWindow) mLogWindow = std::make_unique<LogWindow>(mLogFile);
        mLogWindow->setVisible(true); mLogWindow->toFront(true);
    };
    mStatusPage.header.setLocation(juce::String::fromUTF8(running.location.c_str()));
    styleButton(mStatusPage.copyLinkButton, pal::accentSoft, pal::text);
    styleButton(mStatusPage.portCheckButton, pal::panelLight, pal::text);
    mStatusPage.onCopyLink = [this]() { if (mHub) juce::SystemClipboard::copyTextToClipboard(juce::String::fromUTF8(SonobusHubWeb::joinLink(mHub->snapshotForUI()).c_str())); };
    // Per-session group link in the Recent sessions table (this hub's address/port + that session's group/password)
    mStatusPage.sessionTable.linkFor = [this](const SonobusHubServer::SessionRecord& sd) {
        if (!mHub) return std::string();
        const auto snap = mHub->snapshotForUI();
        return SonobusHubWeb::joinLinkFor(snap.publicAddressHost.empty() ? snap.advertisedPublicHost : snap.publicAddressHost, snap.aooServerPort, sd.group, sd.password);
    };
    mStatusPage.onRerunPortCheck = [this]() { if (mHub) mHub->requestPortCheck(); };
    mTabs.addTab("Status",   pal::panel, &mStatusPage, false);
    mTabs.addTab("Settings", pal::panel, &mSettings,   false);
    mTabs.addTab("Log",      pal::panel, mLogPage.get(), false);
    mTabs.setCurrentTabIndex(0, false);
    addAndMakeVisible(mTabs);

    // Control buttons — SonoBus-aesthetic dark with teal accent on action.
    styleButton(mRestartButton, pal::accentSoft, pal::text);
    styleButton(mQuitButton,    pal::panelLight, pal::textDim);
    mRestartButton.onClick = [this]() {
        if (mRestartFn) mRestartFn();
    };
    mQuitButton.onClick = [this]() {
        if (mQuitFn) mQuitFn();
    };
    addAndMakeVisible(mRestartButton);
    addAndMakeVisible(mQuitButton);
    mRestartButton.setTooltip("Restart the hub. Players reconnect automatically.");
    mQuitButton.setTooltip("Stop hosting and quit the hub.");
    mStatusPage.copyLinkButton.setTooltip("Copy a link that opens this group in SonoBus RP.");
    mStatusPage.linkLabel.setTooltip("The link players use to join this group.");
    mStatusPage.portCheckButton.setTooltip("Test whether players outside your network can reach this hub.");
    styleButton(mStatusPage.installUpdateButton, pal::accent, pal::bg);
    mStatusPage.installUpdateButton.setTooltip("Download and install the new hub. The hub restarts, so every player drops and rejoins.");
    mStatusPage.installUpdateButton.onClick = [this]() { startInstallUpdate(); };
    setTooltip("Total audio traffic through the hub right now.");
    auto& tabBar = mTabs.getTabbedButtonBar();
    tabBar.getTabButton(0)->setTooltip("Players, group link and recent sessions.");
    tabBar.getTabButton(1)->setTooltip("Hub settings.");
    tabBar.getTabButton(2)->setTooltip("Connection history and the hub log.");

    setSize(1500, 800);  // 16 columns like the web page
    // A slow refresh is enough for human-readable stats: each one costs per-peer quantile computes + a full repaint
    // on the same machine whose scheduler headroom the hub audio path depends on. Rates are dt-based, so still exact.
    startTimer(2000);   // 2 s, like the web page's feel without its 1 Hz cost
    mUpdateCheck = std::make_unique<HubUpdateCheck>();   // One check at start
    mUpdateCheck->startThread();
}

HubContent::~HubContent() {
    stopTimer();
    mUpdateCheck.reset();
    mLogWindow.reset();
    mTabs.clearTabs();   // the Log page is owned by mLogPage, destroyed next
    mLogPage.reset();
    mGeo.stop();
}

namespace {
class HubUpdateRun : public juce::ThreadWithProgressWindow {
public:
    HubUpdateRun(HubUpdateZip z, std::function<void(juce::Result)> done)
        : juce::ThreadWithProgressWindow("Installing SonoBus RP Hub update", true, true), mZip(std::move(z)), mDone(std::move(done)) {
        launchThread(juce::Thread::Priority::low);
    }
    void run() override {
        setProgress(-1.0);
        auto r = hubInstallUpdate(mZip, [this](const juce::String& s) { setStatusMessage(s); return !threadShouldExit(); });
        juce::MessageManager::callAsync([this, r]() { mDone(r); });
    }
private:
    HubUpdateZip mZip;
    std::function<void(juce::Result)> mDone;
};
} // namespace

void HubContent::startInstallUpdate() {
    if (!mUpdateCheck || mUpdateRun) return;
    const auto zip = mUpdateCheck->zip();
    if (zip.url.isEmpty()) return;
    juce::AlertWindow::showOkCancelBox(juce::MessageBoxIconType::QuestionIcon, "Install Update",
        "Install SonoBus RP Hub " + mUpdateCheck->latest() + "?\n\nThe hub restarts, so every player drops and rejoins.",
        "Install", "Cancel", this,
        juce::ModalCallbackFunction::create([this, zip](int result) {
            if (result != 1) return;
            mUpdateRun = std::make_unique<HubUpdateRun>(zip, [this](juce::Result r) {
                mUpdateRun.reset();
                std::string err;
                if (r.wasOk() && mRelaunchFn && mRelaunchFn(err)) return;
                juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Update not installed",
                    r.failed() ? r.getErrorMessage() + "\n\nThis hub keeps running."
                               : juce::String("The update is installed, but the hub could not restart: ") + juce::String(err) + "\n\nQuit and open the hub again.");
            });
        }));
}

void HubContent::paint(juce::Graphics& g) {
    g.fillAll(pal::bg);

    // Bandwidth strip, drawn in the control bar, left of the buttons.
    // (Bounds set in resized(). Painted here for live update without flicker.)
    auto controlBar = getLocalBounds().removeFromBottom(58);
    g.setColour(pal::panel);
    g.fillRect(controlBar);

    auto bwArea = controlBar.reduced(14, 10).withTrimmedRight(320);
    g.setColour(pal::accent);
    g.setFont(juce::Font(17.0f, juce::Font::bold));
    auto kbps = [](float bps) { return hubRateText(bps); };   // Bits, shared rule
    juce::String bwText;
    bwText << "IN  " << kbps(mInBytesPerSec)
           << "    OUT " << kbps(mOutBytesPerSec);
    g.drawText(bwText, bwArea, juce::Justification::centredLeft);
}

void HubContent::resized() {
    auto bounds = getLocalBounds();

    // Bottom control bar (58 px): bandwidth on the left, Restart + Quit on right.
    auto controlBar = bounds.removeFromBottom(58).reduced(14, 10);
    mQuitButton.setBounds(controlBar.removeFromRight(130));
    controlBar.removeFromRight(10);
    mRestartButton.setBounds(controlBar.removeFromRight(160));

    // Tabs take everything above the control bar.
    mTabs.setBounds(bounds);
}

void HubContent::timerCallback() {
    if (!mHub) return;
    auto snap = mHub->snapshotForUI();

    // Derive per-second bandwidth from successive cumulative counters.
    int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    if (mLastSampleMs > 0) {
        int64_t dt = nowMs - mLastSampleMs;
        if (dt > 0) {
            mInBytesPerSec  = (float)((snap.bytesInTotal  - mLastBytesIn)  * 1000.0 / dt);
            mOutBytesPerSec = (float)((snap.bytesOutTotal - mLastBytesOut) * 1000.0 / dt);
        }
    }
    mLastBytesIn = snap.bytesInTotal;
    mLastBytesOut = snap.bytesOutTotal;
    mLastSampleMs = nowMs;

    // Per-player Up/Down from successive per-peer counters.
    {
        std::set<int32_t> seen;
        for (auto& p : snap.peers) {
            seen.insert(p.id);
            auto& prev = mPeerBytes[p.id];
            if (prev.sampleMs > 0 && prev.joinedAtMs == p.joinedAtMs && nowMs > prev.sampleMs
                && p.bytesIn >= prev.in && p.bytesOut >= prev.out) {
                const double dt = (double)(nowMs - prev.sampleMs);
                p.inBytesPerSec  = (float)((double)(p.bytesIn  - prev.in)  * 1000.0 / dt);
                p.outBytesPerSec = (float)((double)(p.bytesOut - prev.out) * 1000.0 / dt);
            }
            prev = { p.bytesIn, p.bytesOut, nowMs, p.joinedAtMs };
        }
        for (auto it = mPeerBytes.begin(); it != mPeerBytes.end();)
            it = seen.count(it->first) ? std::next(it) : mPeerBytes.erase(it);
    }

    for (auto& p : snap.peers) p.location = mGeo.locationFor(p.ipAddr, p.isLan, mGeoEnabled);   
    mStatusPage.linkLabel.setText(juce::String::fromUTF8(SonobusHubWeb::joinLink(snap).c_str()), juce::dontSendNotification);
    mStatusPage.header.setSnapshot(snap);
    mSettings.setDetectedAddress(juce::String::fromUTF8(snap.advertisedPublicHost.c_str()));
    if (mLogFile != juce::File() && ++mLogCheckTicks >= 600) {
        mLogCheckTicks = 0;
        if (mLogFile.getSize() >= 50 * 1024 * 1024) hub_settings::redirectLogFile(mLogFile.getFullPathName().toStdString());
    }
    if (mUpdateCheck) mStatusPage.header.setUpdate(mUpdateCheck->line(), mUpdateCheck->url());   
    if (mUpdateCheck && !mStatusPage.installUpdateButton.isVisible() && mUpdateCheck->zip().url.isNotEmpty() && hubCanUpdateInPlace()) {
        mStatusPage.installUpdateButton.setVisible(true);
        mStatusPage.resized();
    }
    mStatusPage.peerTable.setHubFormat(snap.sampleRate, snap.blockSize);   
    {   // Re-layout when the player count changes (table height follows it)
        const int before = mStatusPage.peerTable.numRows();
        mStatusPage.peerTable.setSnapshot(snap.peers);
        if (mStatusPage.peerTable.numRows() != before) mStatusPage.resized();
    }
    // Heading: connected now, and distinct players since the hub started
    mStatusPage.peerLabel.setText(juce::String("  Players     ") + juce::String((int) snap.peers.size()) + " now" + juce::String::fromUTF8("  \xc2\xb7  ")
                                  + juce::String(mHub->playersSinceStart()) + " since hub start", juce::dontSendNotification);
    // Recent sessions (newest first) + the selected/current session's personnel
    {   // The sessions table is sized to its rows — re-layout when the count changes
        const int before = mStatusPage.sessionTable.numRows();
        mStatusPage.sessionTable.setRows(mHub->sessions(), snap.groupName);
        if (mStatusPage.sessionTable.numRows() != before) mStatusPage.resized();
    }
    refreshSessionDetail();
    repaint();  // for bandwidth strip
}

// The personnel table shows the SELECTED session's players (newest first) — or, with nothing
// selected, the current session's — with locations from the same resolver as the live table; the password field and
// Apply & Restart are enabled only with a selection.
void HubContent::refreshSessionDetail() {
    if (!mHub) return;
    const auto* sel = mStatusPage.sessionTable.selected();
    std::vector<SonobusHubServer::RecentPlayer> rows;
    if (sel) rows.assign(sel->players.rbegin(), sel->players.rend());
    else rows = mHub->recentPlayers(30);
    for (auto& r : rows) r.location = mGeo.locationFor(r.ip, r.lan, mGeoEnabled);
    mStatusPage.recentTable.setRows(std::move(rows));
    mStatusPage.recentLabel.setText(sel ? "  Players seen in \"" + juce::String::fromUTF8(sel->group.c_str()) + "\""
                                        : juce::String("  Players seen in this session"), juce::dontSendNotification);
    mStatusPage.sessionApply.setEnabled(sel != nullptr);
    mStatusPage.sessionPwEditor.setEnabled(sel != nullptr);
    {   // Prefill only when the SELECTION changes
        const std::string g = sel ? sel->group : std::string();
        if (g != mPwPrefillGroup) {
            mStatusPage.sessionPwEditor.setText(sel ? juce::String::fromUTF8(sel->password.c_str()) : juce::String(), juce::dontSendNotification);
            mPwPrefillGroup = g;
        }
    }
    mStatusPage.sessionPwEditor.setTextToShowWhenEmpty(!sel ? "select a session" : "(no password)", pal::textDim);
}

// ─── HubMainWindow ────────────────────────────────────────────────────────────
HubMainWindow::HubMainWindow(const juce::String& title,
                             SonobusHubServer* hub,
                             const HubLaunchArgs& running,
                             const HubSettingsMeta& meta,
                             SettingsPanel::Callbacks settingsCb,
                             std::function<void()> restartFn,
                             std::function<void()> quitFn,
                             std::function<void(const std::string&)> applyGroupFn,
                             std::function<void(bool)> applyHoldFn,
                             std::function<void(const std::string&, const std::string&)> applySessionFn)   
    : juce::DocumentWindow(title,
                           pal::bg,
                           juce::DocumentWindow::minimiseButton | juce::DocumentWindow::closeButton)
{
    setUsingNativeTitleBar(true);
    setResizable(true, true);
    setResizeLimits(1180, 760, 8000, 8000);   // Below ~760 pt the sessions strip overlapped the password row
    setContentOwned(new HubContent(hub, running, meta, std::move(settingsCb),
                                   std::move(restartFn), std::move(quitFn), std::move(applyGroupFn), std::move(applyHoldFn), std::move(applySessionFn)), true);
    centreWithSize(1500, 800);
    setVisible(true);
    toFront(true);
}

void HubMainWindow::closeButtonPressed() {
    juce::JUCEApplication::getInstance()->systemRequestedQuit();
}

// ─── HubJuceApp ───────────────────────────────────────────────────────────────
HubJuceApp::HubJuceApp(const HubLaunchArgs& args, const HubSettingsMeta& meta, std::vector<std::string> argv)
    : mArgs(args), mMeta(meta), mArgv(std::move(argv)) {}

#if JUCE_MAC
extern "C" void* objc_getClass(const char*);
extern "C" void* sel_registerName(const char*);
extern "C" void* objc_msgSend(void*, void*, ...);
static void forceRegularActivationPolicy() {
    // When launched outside Finder (CLI, nohup, etc.), AppKit may keep
    // the process as NSApplicationActivationPolicyProhibited so the
    // window never appears in the Dock / AX tree even though JUCE thinks
    // it's visible. Force the regular foreground policy explicitly.
    void* NSApp_cls = objc_getClass("NSApplication");
    void* sharedSel = sel_registerName("sharedApplication");
    void* app = objc_msgSend(NSApp_cls, sharedSel);
    void* setPolicySel = sel_registerName("setActivationPolicy:");
    // NSApplicationActivationPolicyRegular = 0
    typedef void (*SetPolicyFn)(void*, void*, long);
    ((SetPolicyFn)objc_msgSend)(app, setPolicySel, 0);
    void* activateSel = sel_registerName("activateIgnoringOtherApps:");
    typedef void (*ActivateFn)(void*, void*, char);
    ((ActivateFn)objc_msgSend)(app, activateSel, 1);
}
#endif

void HubJuceApp::initialise(const juce::String&) {
#if JUCE_MAC
    forceRegularActivationPolicy();
#endif
    const bool firstLaunch = !mMeta.loadedFromFile && mMeta.loadError.empty()
                             && mMeta.file != juce::File() && mMeta.cliKeys.count("group") == 0;
    if (firstLaunch) askFirstLaunch();
    else startHubAndWindow();
}

// Nothing is hosted until the operator has named the group.
void HubJuceApp::askFirstLaunch() {
    auto* w = new juce::AlertWindow("Welcome to SonoBus RP Hub",
                                    "Choose a group name. Players use it to join this hub.",
                                    juce::MessageBoxIconType::NoIcon);
    w->addTextEditor("group", "", "Group name");
    w->addTextEditor("password", "", "Password (optional)");
    w->addButton("Start Hub", 1, juce::KeyPress(juce::KeyPress::returnKey));
    w->addButton("Quit", 0);
    juce::Process::makeForegroundProcess();
    w->enterModalState(true, juce::ModalCallbackFunction::create([this, w](int result) {
        if (result == 0) { quit(); return; }
        const auto group = w->getTextEditorContents("group").trim();
        if (group.isEmpty()) { askFirstLaunch(); return; }
        mArgs.groupName = group.toStdString();
        mArgs.groupPassword = w->getTextEditorContents("password").toStdString();
        std::string err;
        if (hub_settings::save(mMeta.file, mArgs, err)) mMeta.loadedFromFile = true;
        else std::cerr << "[HUB] first-launch settings save FAILED: " << err << "\n";
        startHubAndWindow();
    }), true);
}

void HubJuceApp::startHubAndWindow() {
    std::string serverHost;
    int serverPort = 0;
    hub_settings::resolveServerEndpoint(mArgs, serverHost, serverPort);

    std::cout << "=== SonoBus RP Hub Server (GUI) ===\n"
              << "Server: " << serverHost << ":" << serverPort << "\n"
              << "User:   " << mArgs.userName << "\n"
              << "Group:  " << mArgs.groupName << "\n"
              << "Audio:  " << mArgs.sampleRate << " Hz, " << mArgs.blockSize << " samples, "
              << mArgs.channels << " ch\n"
              << "Codec:  " << (mArgs.useOpus
                                  ? std::string("Opus ") + std::to_string(mArgs.opusBitrate / 1000)
                                        + " kbps/ch"
                                  : std::string("PCM 16-bit"))
              << "\n"
              << "Local:  " << (mArgs.localServer
                                  ? std::string("Built-in AOO server on port ")
                                        + std::to_string(mArgs.localServerPort)
                                  : std::string("External server"))
              << "\n"
              << "=====================================\n";

    mHub = std::make_unique<SonobusHubServer>();
    hub_settings::applyToHub(*mHub, mArgs);   // One shared setter path (was three copies)
    if (mMeta.file != juce::File()) mHub->setSessionsFile(mMeta.file.getSiblingFile("hub_sessions.json").getFullPathName().toStdString());   // Recent sessions (+ their players) persist next to hub_settings.json

    if (!mHub->start(serverHost, serverPort, mArgs.userName, mArgs.groupName, mArgs.groupPassword,
                     mArgs.udpPort, mArgs.sampleRate, mArgs.blockSize, mArgs.channels)) {
        std::cerr << "[HUB] Failed to start hub server.\n";
        // Still show the window so the operator sees the log error.
        mStartFailed = true;
    }
    makeWindow();
    installAppMenu();   // About SonoBus RP Hub
    startWeb();   // After makeWindow so the page shares the window's geo resolver

    if (mStartFailed) {
        // Most likely a player launched before the hub on the same Mac (err 48).
        // Say it on screen, with the most likely cause, not only in the log.
        juce::AlertWindow::showMessageBoxAsync(
            juce::MessageBoxIconType::WarningIcon, "Hub failed to start",
            juce::String("Could not open UDP port ") + juce::String(mArgs.localServerPort)
                + " (or the server could not be reached).\n\nMost likely another SonoBus (or an older hub) is "
                  "already running on this Mac. Quit it, then press Restart.");
    }
}

void HubJuceApp::doRestart() {
    // Tear down + bring back the SonobusHubServer with the stored launch
    // args. Posted to the message thread so we don't tear the hub down
    // inside the button-click stack frame.
    juce::MessageManager::callAsync([this]() {
        std::cout << "[HUB] GUI: Restart requested.\n";
        mWeb.reset();   // References the hub we are about to replace
        if (mHub) mHub->stop();
        // Recreate from scratch so atomic counters / state reset cleanly.
        // mArgs may have been updated by a Settings-tab Save, so this also
        // applies every Restart-tier setting (not the SB_AOO_* env ones —
        // those are process statics in sink.cpp; they need Save & Restart).
        mHub = std::make_unique<SonobusHubServer>();
        hub_settings::applyToHub(*mHub, mArgs);
        if (mMeta.file != juce::File()) mHub->setSessionsFile(mMeta.file.getSiblingFile("hub_sessions.json").getFullPathName().toStdString());   // Recent sessions
        std::string serverHost;
        int serverPort = 0;
        hub_settings::resolveServerEndpoint(mArgs, serverHost, serverPort);
        if (!mHub->start(serverHost, serverPort, mArgs.userName, mArgs.groupName,
                         mArgs.groupPassword, mArgs.udpPort, mArgs.sampleRate,
                         mArgs.blockSize, mArgs.channels)) {
            std::cerr << "[HUB] GUI: Restart failed to bring server back up.\n";
        } else {
            std::cout << "[HUB] GUI: Restart complete.\n";
        }
        // Recreate the window pointed at the new hub instance.
        // Window FIRST, then the web server (see below)
        mWindow.reset();
        makeWindow();
        startWeb();   // After the new window: the page binds to that window's geo resolver
    });
}

void HubJuceApp::makeWindow() {
    auto restartFn = [this]() { doRestart(); };
    auto quitFn = []() {
        juce::JUCEApplication::getInstance()->systemRequestedQuit();
    };
    SettingsPanel::Callbacks cb;
    cb.save = [this](const HubLaunchArgs& a, std::string& err) { return applySavedSettings(a, err); };
    cb.relaunch = [this](std::string& err) { return doRelaunch(err); };
    auto applyGroupFn = [this](const std::string& g) { applyGroupAndRestart(g); };
    auto applyHoldFn = [this](bool print) { applyHoldMode(print); };
    auto applySessionFn = [this](const std::string& g, const std::string& pw) { applySessionAndRestart(g, pw); };   
    mWindow = std::make_unique<HubMainWindow>(
        "SonoBus RP Hub", mHub.get(), mArgs, mMeta, std::move(cb), restartFn, quitFn, applyGroupFn, applyHoldFn, applySessionFn);
}

// ─── "About SonoBus RP Hub" in the application menu ──────────────────
namespace {
class HubAboutComponent : public juce::Component {
public:
    HubAboutComponent() {
        info = SonobusPlusVersion::get();
        logo = juce::ImageCache::getFromMemory(HubBinaryData::sonobus_hub_icon_mac_256_png,
                                               HubBinaryData::sonobus_hub_icon_mac_256_pngSize);
        // Third-party notices ship in the bundle (LICENSES-HUB.txt, copied by CMake POST_BUILD)
        addAndMakeVisible(noticesButton);
        noticesButton.setTooltip("Reveal Contents/Resources/LICENSES-HUB.txt (AOO, oscpack, Opus, MD5, JUCE notices)");
        noticesButton.onClick = [] {
            const juce::File f = juce::File::getSpecialLocation(juce::File::currentApplicationFile).getChildFile("Contents/Resources/LICENSES-HUB.txt");
            if (f.existsAsFile()) f.revealToUser();
        };
        setSize(600, 360);
    }
    void resized() override { noticesButton.setBounds(getLocalBounds().reduced(28).removeFromBottom(26).removeFromRight(190)); }   
    void paint(juce::Graphics& g) override {
        g.fillAll(juce::Colour(0xff262626));
        auto area = getLocalBounds().reduced(28);
        auto left = area.removeFromLeft(170);
        if (logo.isValid())
            g.drawImageWithin(logo, left.getX(), left.getY() + 20, 140, 140, juce::RectanglePlacement::centred);
        auto r = area.withTrimmedLeft(10);
        g.setColour(juce::Colours::white);
        g.setFont(juce::Font(34.0f, juce::Font::bold));
        g.drawText("SonoBus RP Hub", r.removeFromTop(48), juce::Justification::centredLeft);
        g.setColour(juce::Colour(0xffb0b0b0));
        g.setFont(juce::Font(17.0f));
        // The hub's OWN version first; the dated release id stays as the build identity
        g.drawText(juce::String("Hub ") + SONOBUS_HUB_VERSION + juce::String::fromUTF8("   \xc2\xb7   ") + (info.version.isNotEmpty() ? ("release " + info.version) : juce::String("development build")),
                   r.removeFromTop(26), juce::Justification::centredLeft);
        g.setFont(juce::Font(13.0f));
        r.removeFromTop(10);
        if (info.commit.isNotEmpty())
            g.drawText("Build " + info.commit + juce::String::fromUTF8("  ·  compiled ") + info.buildDate,
                       r.removeFromTop(20), juce::Justification::centredLeft);
        g.drawFittedText("The relay hub for SonoBus RP players. Each player sends one stream to the hub and "
                         "receives everyone else's, each feed held and paced by the hub. Low-latency PCM 16-bit; "
                         "nothing is mixed.", r.removeFromTop(54), juce::Justification::topLeft, 3);
        r.removeFromTop(6);
        g.drawText("Developed by Jason Buecker (Random Planet), 2026.", r.removeFromTop(20), juce::Justification::centredLeft);
        r.removeFromTop(8);
        g.setColour(juce::Colour(0xff8a8a8a));
        g.setFont(juce::Font(12.0f));
        g.drawFittedText("Built on the AOO library by Winfried Ritsch, Christof Ressi and others (BSD), oscpack (MIT), "
                         "Opus (BSD) and JUCE. Companion to SonoBus RP, a modified SonoBus 1.7.2 by Jesse Chappell "
                         "(Sonosaurus). Free software under the GNU General Public License v3.",
                         r.removeFromTop(64), juce::Justification::topLeft, 4);
        r.removeFromTop(4);
        g.drawText("Third-party notices: Contents/Resources/LICENSES-HUB.txt", r.removeFromTop(18), juce::Justification::centredLeft);   
    }
private:
    SonobusPlusVersion::Info info;
    juce::Image logo;
    juce::TextButton noticesButton { "Third-party notices" };   
};

// A menu bar with no menus of its own: it exists so the Apple menu can carry our About item.
class HubMenuModel : public juce::MenuBarModel {
public:
    juce::StringArray getMenuBarNames() override { return {}; }
    juce::PopupMenu getMenuForIndex(int, const juce::String&) override { return {}; }
    void menuItemSelected(int, int) override {}
};
}

void HubJuceApp::showAbout() {
    juce::DialogWindow::LaunchOptions o;
    o.content.setOwned(new HubAboutComponent());
    o.dialogTitle = "About SonoBus RP Hub";
    o.dialogBackgroundColour = juce::Colour(0xff262626);
    o.escapeKeyTriggersCloseButton = true;
    o.useNativeTitleBar = true;
    o.resizable = false;
    o.launchAsync();
}

void HubJuceApp::installAppMenu() {
#if JUCE_MAC
    mMenuModel = std::make_unique<HubMenuModel>();
    juce::PopupMenu extras;
    juce::PopupMenu::Item about("About SonoBus RP Hub");
    about.setAction([this]() { showAbout(); });
    extras.addItem(about);
    juce::MenuBarModel::setMacMainMenu(mMenuModel.get(), &extras);
#endif
}

// Jam/Print. Persist so an icon launch comes up the same way; apply live —
// the stats thread re-sizes every WAN player's hold within 10 s, both directions.
void HubJuceApp::applyHoldMode(bool print) {
    mArgs.holdMode = print ? "print" : "jam";
    if (mHub) mHub->setHoldMode(print ? 1 : 0);
    if (mMeta.file != juce::File()) {
        std::string err;
        if (!hub_settings::save(mMeta.file, mArgs, err)) std::cerr << "[HUB] GUI: hold mode save FAILED: " << err << "\n";
        else mMeta.loadedFromFile = true;
    }
    std::cout << "[HUB] GUI: hold mode -> " << mArgs.holdMode << " (live; WAN holds re-size on the next 10-s tick)\n";
}

// Status-page group field. Persist (so an icon launch
// comes up in the new group), then the same in-process restart as the button.
void HubJuceApp::applyGroupAndRestart(const std::string& group) {
    if (group.empty()) {
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Group",
                                               "Group name must not be empty.");
        return;
    }
    if (group == mArgs.groupName) {
        std::cout << "[HUB] GUI: group unchanged ('" << group << "') — nothing to do.\n";
        return;
    }
    mArgs.groupName = group;
    if (mMeta.file != juce::File()) {
        std::string err;
        if (!hub_settings::save(mMeta.file, mArgs, err))
            std::cerr << "[HUB] GUI: group save FAILED: " << err << " (restarting with the new group anyway)\n";
        else
            mMeta.loadedFromFile = true;
    }
    std::cout << "[HUB] GUI: group -> '" << group << "' — restarting hub; players must rejoin the new group.\n";
    doRestart();
}

// Status-page "Recent sessions" → Apply & Restart. The stored session's settings + group +
// password (override when typed) are overlaid onto a copy of the live model (hub_settings::applySession: formSpecs
// keys only, web page stays reachable, validate()), persisted through applySavedSettings (the Settings tab's Save
// path; adopts into mArgs), then the hub restarts — in-process like the Group field's Apply, or a full relaunch when
// a Relaunch-tier (SB_AOO_*) value differs, like the Settings tab's "Save & Restart hub".
void HubJuceApp::applySessionAndRestart(const std::string& group, const std::string& passwordOverride) {
    SonobusHubServer::SessionRecord rec;
    if (group.empty() || !mHub || !mHub->sessionFor(group, rec)) {
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Recent sessions",
                                               "No remembered session for group '" + juce::String::fromUTF8(group.c_str()) + "'.");
        return;
    }
    HubLaunchArgs n = mArgs;
    int applied = 0; std::string err;
    if (!hub_settings::applySession(n, rec.group, passwordOverride, rec.settings, applied, err)) {   // The field is authoritative — empty = NO password
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Recent sessions",
                                               "Cannot apply session '" + juce::String::fromUTF8(rec.group.c_str()) + "': " + juce::String(err));
        return;
    }
    bool needRelaunch = false;
    for (const auto& k : hub_settings::diffKeys(mArgs, n)) if (hub_settings::tierForKey(k) == hub_settings::Tier::Relaunch) needRelaunch = true;
    if (mMeta.file != juce::File()) {
        if (!applySavedSettings(n, err)) {
            juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Recent sessions", "Save FAILED: " + juce::String(err));
            return;
        }
        mMeta.loadedFromFile = true;
    } else {
        n.dumpPeer = mArgs.dumpPeer; n.dumpSecs = mArgs.dumpSecs;
        mArgs = n;   // --no-settings: nothing to persist, restart with the session's values anyway
    }
    std::cout << "[HUB-SESSION] apply group='" << rec.group << "' (" << applied << " settings) -> restart" << (needRelaunch ? " (relaunch: SB_AOO_* changed)" : "") << "\n";
    if (needRelaunch) {
        if (!doRelaunch(err)) { std::cerr << "[HUB-SESSION] relaunch failed (" << err << ") — in-process restart instead\n"; doRestart(); }
        return;
    }
    doRestart();
}

bool HubJuceApp::applySavedSettings(const HubLaunchArgs& a, std::string& err) {
    if (!hub_settings::save(mMeta.file, a, err)) {
        std::cerr << "[HUB] GUI: settings save FAILED: " << err << "\n";
        return false;
    }
    // Live tier: log-file path. freopen on the message thread; the RT threads
    // never write stdout (they go through the SPSC log ring → statsWriter).
    if (!a.logFile.empty() && a.logFile != mArgs.logFile) {
        std::cout << "[HUB] GUI: log file -> " << a.logFile << " (live)\n";
        hub_settings::redirectLogFile(a.logFile);
    }
    // Adopt into the stored launch args so "Restart Hub" (in-process) applies
    // every Restart-tier value. Diagnostic CLI-only fields are kept as launched.
    HubLaunchArgs n = a;
    n.dumpPeer = mArgs.dumpPeer;
    n.dumpSecs = mArgs.dumpSecs;
    if (n.holdMode != mArgs.holdMode && mHub) mHub->setHoldMode(n.holdMode == "print" ? 1 : 0);   // Live
    mArgs = n;
    std::cout << "[HUB] GUI: settings saved to " << mMeta.file.getFullPathName() << "\n";
    return true;
}

bool HubJuceApp::doRelaunch(std::string& err) {
    // The child must re-derive SB_AOO_* from the settings file, not inherit
    // whatever this process exported at its own startup.
    hub_settings::clearSinkEnv();
    auto argv = hub_settings::relaunchArgv(mArgv, (long)getpid());
    if (!hub_settings::relaunchProcess(argv, err)) {
        std::cerr << "[HUB] GUI: relaunch FAILED: " << err << "\n";
        return false;
    }
    // The child waits (--wait-for-pid) for us to be gone before it binds
    // ports or takes JUCE's single-instance lock — so quit cleanly now.
    std::cout << "[HUB] GUI: Save & Restart — replacement spawned; quitting this instance.\n";
    juce::MessageManager::callAsync([this]() { systemRequestedQuit(); });
    return true;
}

void HubJuceApp::shutdown() {
#if JUCE_MAC
    juce::MenuBarModel::setMacMainMenu(nullptr);
#endif
    mMenuModel.reset();
    mWeb.reset();   // FIRST — the web thread uses the window's geo resolver (HubContent::mGeo) and the hub;
                    // Destroying the window first would pull the resolver out from under sessionsJson.
                    // stop() joins the thread, so nothing is in flight after this.
    mWindow.reset();
    if (mHub) mHub->stop();
    mHub.reset();
}

// The Mac hub serves the same status/settings page as the Linux hub
// (http://<hub>:<web_port>/). Settings edits from the page land in mArgs (same object the Settings tab edits);
// "Save & Restart" on the page = the Settings tab's Save & Restart (relaunch), hopped onto the message thread.
void HubJuceApp::startWeb() {
    mWeb.reset();
    if (!mHub || mArgs.webPort <= 0) return;
    mWeb = std::make_unique<SonobusHubWeb>(*mHub, mArgs.webPort, mArgs.webPassword);
    if (mWindow) if (auto* content = dynamic_cast<HubContent*>(mWindow->getContentComponent())) { mWeb->setGeoResolver(&content->geoResolver()); mWeb->setUpdateCheck(content->updateCheck()); }   // One location cache
    mWeb->setLocation(mArgs.location);
    mWeb->setSettings(&mArgs, mMeta.file, [this]() {
        juce::MessageManager::callAsync([this]() {
            std::string err;
            if (!doRelaunch(err)) std::cerr << "[HUB-WEB] Save & Restart: relaunch failed: " << err << "\n";
        });
    });
    if (!mWeb->start()) mWeb.reset();
}

void HubJuceApp::systemRequestedQuit() {
    quit();
}

// Plumbing for our custom main(): JUCE provides JUCEApplicationBase::main() that
// runs the event loop, but it expects a createInstance global. We set that with
// a closure-captured args bundle.
static HubLaunchArgs gPendingArgs;
static HubSettingsMeta gPendingMeta;
static std::vector<std::string> gPendingArgv;
static juce::JUCEApplicationBase* createJuceAppInstance() {
    return new HubJuceApp(gPendingArgs, gPendingMeta, gPendingArgv);
}

int runGui(const HubLaunchArgs& args, const HubSettingsMeta& meta, int argc, char* argv[]) {
    gPendingArgs = args;
    gPendingMeta = meta;
    gPendingArgv.assign(argv, argv + argc);
    juce::JUCEApplicationBase::createInstance = createJuceAppInstance;
    return juce::JUCEApplicationBase::main(argc, (const char**)argv);
}

} // namespace sb_hub_gui
