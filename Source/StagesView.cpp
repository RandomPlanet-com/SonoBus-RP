// SPDX-License-Identifier: GPLv3-or-later WITH Appstore-exception
// Copyright (C) 2026 Jason Buecker (Random Planet)
#include "StagesView.h"
#include "SonoLookAndFeel.h"
#include "SbLatencyColours.h"
#include "SbTextFit.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <iterator>

namespace {

const char* const kDirectoryBase = "https://stages.randomplanet.com/api/1/";

const juce::Colour kBg(0xff1a1a1a), kHeaderBg(0xff141414), kSel(0xff1f5c58), kSep(0xff262626), kDetailBg(0xff191d1d);
const juce::Colour kOpenRow(0xff1e2323), kOpenHere(0xff1d3231), kPanelLine(0xff2a3232), kPanelHead(0xff7f8a8a);
const juce::Colour kText(0xffe4e4e4), kDim(0xff8f8f8f), kSelDim(0xffd5e6e4), kAccent(0xff3fb8af);
const juce::Colour kGood(0xff6abe4f), kWarn(0xffd6b03e), kBad(0xffee6666);
const juce::Colour kHere(0xff5fd26b), kHereEdge(0xff2f6a36), kPrivateBar(0xff8a8a8a);

enum { colOpen = 1, colStage, colAbout, colCity, colCountry, colGenre, colPlayers, colPing, colVersion };

const int kIndent = 22;
const int kRowHeight = 28;
const int kPanelIndent = 30;   // where the opened part of a stage starts

// A list nobody touches slows down and stops pinging.
const int kRefreshMs = 30000, kRetryMs = 15000, kIdleRefreshMs = 120000, kIdleAfterMs = 120000;

float fontScale() { return SonoLookAndFeel::getFontScale(); }
juce::String dot() { return juce::String::fromUTF8(" \xc2\xb7 "); }

// The directory may move: it can name a new address, accepted only as https on randomplanet.com.
bool acceptableForward(const juce::String& u) {
    if (! u.startsWith("https://")) return false;
    const juce::String host = u.fromFirstOccurrenceOf("https://", false, false).upToFirstOccurrenceOf("/", false, false).toLowerCase();
    return host == "randomplanet.com" || host.endsWith(".randomplanet.com");
}

int compareVersions(const juce::String& a, const juce::String& b) {
    const auto pa = juce::StringArray::fromTokens(a, ".", ""), pb = juce::StringArray::fromTokens(b, ".", "");
    for (int i = 0; i < juce::jmax(pa.size(), pb.size()); ++i) {
        const int x = pa[i].getIntValue(), y = pb[i].getIntValue();
        if (x != y) return x < y ? -1 : 1;
    }
    return 0;
}

juce::String listedFor(juce::int64 since) {
    const juce::int64 s = juce::Time::currentTimeMillis() / 1000 - since;
    if (since <= 0 || s < 3600) return TRANS("under an hour");
    if (s < 7200) return "1 " + TRANS("hour");
    if (s < 48 * 3600) return juce::String(s / 3600) + " " + TRANS("hours");
    return juce::String(s / 86400) + " " + TRANS("days");
}

// "6 min", "1 h 12 min"; "-" when the stage did not say
juce::String onStageFor(int seconds) {
    if (seconds < 0) return "-";
    const int m = seconds / 60;
    if (m < 1) return TRANS("under 1 min");
    if (m < 60) return juce::String(m) + " min";
    return juce::String(m / 60) + " h " + juce::String(m % 60).paddedLeft('0', 2) + " min";
}

int tagWidth(const juce::String& t, float scale) { return juce::Font(11.0f * scale).getStringWidth(t) + 16; }

// A small outlined tag after a name ("you are here", "you").
void drawTag(juce::Graphics& g, juce::Rectangle<int> r, const juce::String& t, float scale) {
    g.setColour(kHereEdge);
    g.drawRoundedRectangle(r.toFloat().reduced(0.5f), (float) r.getHeight() * 0.5f, 1.0f);
    g.setColour(kHere);
    g.setFont(juce::Font(11.0f * scale));
    g.drawText(t, r, juce::Justification::centred, false);
}

bool isFull(const StagesView::Stage& s) { return s.maxPlayers > 0 && s.count >= s.maxPlayers; }

juce::Colour pingColour(float ms) {
    if (ms < 0.0f) return kDim;
    return ms <= sbplus::kStagePingGreenMs ? kGood : ms <= sbplus::kStagePingYellowMs ? kWarn : kBad;
}

// The open / closed arrow of a stage row, also drawn by the header's Expand all cell
void drawOpenArrow(juce::Graphics& g, const juce::Rectangle<float>& area, bool open, juce::Colour c) {
    const auto a = area.withSizeKeepingCentre(9.0f, 9.0f);
    juce::Path p;
    if (open) p.addTriangle(a.getX(), a.getY() + 1.5f, a.getRight(), a.getY() + 1.5f, a.getCentreX(), a.getBottom() - 0.5f);
    else      p.addTriangle(a.getX() + 1.5f, a.getY(), a.getRight() - 0.5f, a.getCentreY(), a.getX() + 1.5f, a.getBottom());
    g.setColour(c);
    g.fillPath(p);
}

void drawLock(juce::Graphics& g, juce::Rectangle<int> cell, juce::Colour c) {
    const auto r = cell.toFloat().withSizeKeepingCentre(10.0f, 12.0f);
    g.setColour(c);
    g.fillRoundedRectangle(r.withTrimmedTop(5.0f), 1.5f);
    juce::Path shackle;
    shackle.addCentredArc(r.getCentreX(), r.getY() + 5.0f, 3.0f, 3.5f, 0.0f, -juce::MathConstants<float>::halfPi, juce::MathConstants<float>::halfPi, true);
    g.strokePath(shackle, juce::PathStrokeType(1.6f));
}

#if JUCE_LINUX
juce::String runCurl(const juce::StringArray& cmd, int& exitCode) {
    juce::ChildProcess cp;
    if (! cp.start(cmd, juce::ChildProcess::wantStdOut)) { exitCode = -1; return {}; }
    const auto out = cp.readAllProcessOutput();
    cp.waitForProcessToFinish(8000);
    exitCode = (int) cp.getExitCode();
    return out;
}
#endif

class RootItem : public juce::TreeViewItem {
public:
    bool mightContainSubItems() override { return true; }
};

}   // namespace

//==============================================================================
// Fetches the list and pings the stages, away from the message thread.
class StagesView::Worker : public juce::Thread
{
public:
    explicit Worker(StagesView& v) : juce::Thread("stage list"), view(v), base(kDirectoryBase) {}
    ~Worker() override { shutdown(); }

    void shutdown() {
        signalThreadShouldExit();
        {
            const juce::ScopedLock l(streamLock);
            if (stream != nullptr) stream->cancel();
        }
        notify();
        stopThread(8000);
    }

    void run() override {
        juce::int64 lastAt = 0;
        bool lastOk = false;
        while (! threadShouldExit()) {
            if (active.load()) {
                const int every = idle.load() ? kIdleRefreshMs : lastOk ? kRefreshMs : kRetryMs;
                if (refreshNow.exchange(false) || juce::Time::currentTimeMillis() >= lastAt + every) {
                    lastOk = fetch();
                    ++attempts;
                    lastAt = juce::Time::currentTimeMillis();
                    view.triggerAsyncUpdate();
                    if (lastOk && ! idle.load() && ! threadShouldExit()) ping();
                }
            }
            wait(active.load() ? 500 : -1);   // asleep until the list is shown
        }
    }

    std::atomic<bool> active { false }, refreshNow { false }, idle { false }, wantTest { false };
    std::atomic<int> attempts { 0 }, pingsSent { 0 };

    void setMemory(const juce::File& file) {
        const juce::String saved = file.loadFileAsString().trim();
        const juce::ScopedLock l(lock);
        memory = file;
        if (acceptableForward(saved)) base = saved + (saved.endsWithChar('/') ? "" : "/");   // a move learned earlier wins
    }

    juce::CriticalSection lock;   // guards the results below
    std::vector<Stage> stages;
    juce::StringArray genres;
    std::map<juce::String, float> pings;
    juce::int64 fetchedAt = 0;
    bool reached = false, everReached = false, testData = false;

private:
    juce::String httpGet(const juce::String& url, int& status) {
        status = 0;
       #if JUCE_LINUX
        int rc = 0;
        const juce::String out = runCurl({ "curl", "-sS", "--connect-timeout", "4", "--max-time", "6", "-w", "\n%{http_code}", url }, rc);
        if (rc != 0) return {};
        status = out.fromLastOccurrenceOf("\n", false, false).trim().getIntValue();
        return out.upToLastOccurrenceOf("\n", false, false);
       #else
        juce::WebInputStream in(juce::URL(url), false);
        in.withConnectionTimeout(6000).withExtraHeaders("Cache-Control: no-cache");
        {
            const juce::ScopedLock l(streamLock);
            stream = &in;
        }
        juce::String body;
        if (! threadShouldExit() && in.connect(nullptr)) {
            status = in.getStatusCode();
            body = in.readEntireStreamAsString();
        }
        {
            const juce::ScopedLock l(streamLock);
            stream = nullptr;
        }
        return body;
       #endif
    }

    bool fetch() {
        int status = 0;
        juce::String from;
        {
            const juce::ScopedLock l(lock);
            from = base;
        }
        // Test aid: SB_STAGES_FILE=<file> stands in for the directory's answer.
        static const juce::String standIn = juce::SystemStats::getEnvironmentVariable("SB_STAGES_FILE", {});
        juce::String body;
        if (standIn.isNotEmpty()) { body = juce::File(standIn).loadFileAsString(); status = 200; }
        else body = httpGet(from + "stages" + (wantTest.load() ? "?test=1" : ""), status);
        const juce::var v = status == 200 ? juce::JSON::parse(body) : juce::var();
        const auto* arr = v["stages"].getArray();
        if (arr == nullptr) {
            const juce::ScopedLock l(lock);
            reached = false;
            return false;
        }
        std::vector<Stage> list;
        juce::String newest;
        const juce::int64 generated = (juce::int64) v["generated"];
        for (const auto& e : *arr) {
            if (list.size() >= 2000) break;
            Stage s;
            s.id = e["id"].toString().substring(0, 64);
            s.name = e["name"].toString().substring(0, 80);
            s.host = e["host"].toString().substring(0, 255);
            s.port = (int) e["port"];
            if (s.id.isEmpty() || s.name.isEmpty() || s.host.isEmpty() || s.port <= 0 || s.port > 65535) continue;
            s.city = e["location"].toString().substring(0, 60);
            s.country = e["country"].toString().substring(0, 8);
            s.genre = e["genre"].toString().substring(0, 40);
            s.about = e["description"].toString().substring(0, 200);
            s.version = e["version"].toString().substring(0, 20);
            s.os = e["os"].toString().substring(0, 20);
            s.count = juce::jmax(0, (int) e["count"]);
            s.maxPlayers = juce::jmax(0, (int) e["max_players"]);
            s.maxChannels = juce::jmax(0, (int) e["max_channels"]);
            s.since = (juce::int64) e["since"];
            s.locked = (bool) e["locked"];
            s.test = (bool) e["test"];
            if (const auto* pa = e["players"].getArray())
                for (const auto& p : *pa) {
                    if (s.players.size() >= 64) break;
                    s.players.add(p.toString().substring(0, 64));
                }
            if (const auto* ia = e["instruments"].getArray())
                for (const auto& p : *ia) {
                    if (s.instruments.size() >= s.players.size()) break;
                    s.instruments.add(p.toString().substring(0, 24));
                }
            // Per player, as the stage last reported it. Older stages and directories send none of these.
            auto numbers = [&](const char* key, juce::Array<int>& out) {
                if (const auto* a = e[juce::Identifier(key)].getArray())
                    for (const auto& x : *a) {
                        if (out.size() >= s.players.size()) break;
                        out.add((x.isInt() || x.isInt64() || x.isDouble()) ? (int) juce::jlimit(-1.0, 1.0e9, (double) x) : -1);
                    }
            };
            numbers("on_stage_s", s.playerSeconds);
            numbers("ping_ms", s.playerPing);
            numbers("channels", s.playerChannels);
            const juce::int64 reportAge = juce::jlimit((juce::int64) 0, (juce::int64) 600, generated - (juce::int64) e["updated"]);
            if (generated > 0 && (juce::int64) e["updated"] > 0)
                for (auto& sec : s.playerSeconds) if (sec >= 0) sec += (int) reportAge;
            if (s.test) {   // made-up entries carry a made-up ping
                const juce::var sp = e["sim_ping_ms"];
                s.pingMs = (sp.isInt() || sp.isInt64() || sp.isDouble()) ? (float) (double) sp : -1.0f;
            }
            if (compareVersions(s.version, newest) > 0) newest = s.version;
            list.push_back(std::move(s));
        }
        for (auto& s : list) s.older = compareVersions(s.version, newest) < 0;

        juce::StringArray g;
        if (const auto* ga = v["genres"].getArray())
            for (const auto& x : *ga) if (g.size() < 40) g.add(x.toString().substring(0, 40));

        const juce::String fwd = v["forward"].toString();
        juce::File remember;

        const juce::ScopedLock l(lock);
        if (fwd.isNotEmpty() && acceptableForward(fwd)) {
            const juce::String moved = fwd + (fwd.endsWithChar('/') ? "" : "/");
            if (moved != base) { base = moved; remember = memory; }
        }
        if (remember != juce::File()) remember.replaceWithText(fwd);
        for (auto it = pings.begin(); it != pings.end();) {
            const bool present = std::any_of(list.begin(), list.end(), [&](const Stage& s) { return s.id == it->first; });
            it = present ? std::next(it) : pings.erase(it);
        }
        stages = std::move(list);
        genres = g;
        testData = (bool) v["test_data"];
        fetchedAt = juce::Time::currentTimeMillis();
        reached = everReached = true;
        return true;
    }

    // "/sb/stage/ping" h:token -> "/sb/stage/pong" h:token i:players i:maxPlayers, on the stage's connection port.
    void ping() {
        struct Target { juce::String id, host; int port; };
        std::vector<Target> targets;
        {
            const juce::ScopedLock l(lock);
            for (const auto& s : stages) if (! s.test) targets.push_back({ s.id, s.host, s.port });
        }
        if (targets.empty()) return;
        juce::DatagramSocket sock(false);
        if (! sock.bindToPort(0)) return;
        juce::int64 token = juce::Time::currentTimeMillis() << 8;
        for (const auto& t : targets) {
            float best = -1.0f;
            int silent = 0;
            for (int n = 0; n < 3 && silent < 2 && ! threadShouldExit(); ++n) {
                char pkt[28] = { 0 };
                std::memcpy(pkt, "/sb/stage/ping", 14);
                pkt[16] = ','; pkt[17] = 'h';
                ++token;
                for (int b = 0; b < 8; ++b) pkt[20 + b] = (char) ((token >> (8 * (7 - b))) & 0xff);
                if (sock.write(t.host, t.port, pkt, (int) sizeof(pkt)) != (int) sizeof(pkt)) { silent = 2; break; }
                ++pingsSent;
                const double t0 = juce::Time::getMillisecondCounterHiRes();   // after the send, so a name lookup is not counted
                float rtt = -1.0f;
                while (! threadShouldExit()) {
                    const double left = 450.0 - (juce::Time::getMillisecondCounterHiRes() - t0);
                    if (left <= 0.0) break;
                    const int ready = sock.waitUntilReady(true, (int) juce::jmin(left, 100.0) + 1);
                    if (ready < 0) break;
                    if (ready == 0) continue;
                    char in[96];
                    juce::String fromHost; int fromPort = 0;
                    const int got = sock.read(in, (int) sizeof(in), false, fromHost, fromPort);
                    const double t1 = juce::Time::getMillisecondCounterHiRes();
                    if (got < 32 || std::memcmp(in, "/sb/stage/pong", 14) != 0) continue;
                    juce::int64 tk = 0;
                    for (int b = 0; b < 8; ++b) tk = (tk << 8) | (juce::int64) (juce::uint8) in[24 + b];
                    if (tk == token) { rtt = (float) (t1 - t0); break; }
                }
                if (rtt < 0.0f) { ++silent; continue; }
                best = best < 0.0f ? rtt : juce::jmin(best, rtt);
                wait(40);
            }
            {
                const juce::ScopedLock l(lock);
                pings[t.id] = best;
            }
            view.triggerAsyncUpdate();
            if (threadShouldExit() || idle.load() || ! active.load()) break;
        }
    }

    StagesView& view;
    juce::String base;      // guarded by lock
    juce::File memory;      // guarded by lock
    juce::CriticalSection streamLock;
    juce::WebInputStream* stream = nullptr;
};

//==============================================================================
// One stage = one row. A click selects it, a double-click connects. Opening it shows who is playing.
class StagesView::StageItem : public juce::TreeViewItem
{
public:
    StageItem(StagesView& o, const Stage& s) : owner(o), stage(s) { setDrawsInLeftMargin(true); }

    bool mightContainSubItems() override { return true; }
    int getItemHeight() const override { return kRowHeight; }
    juce::String getUniqueName() const override { return stage.id; }
    void paintHorizontalConnectingLine(juce::Graphics&, const juce::Line<float>&) override {}
    void paintVerticalConnectingLine(juce::Graphics&, const juce::Line<float>&) override {}

    void paintOpenCloseButton(juce::Graphics& g, const juce::Rectangle<float>& area, juce::Colour, bool mouseOver) override {
        drawOpenArrow(g, area, isOpen(), mouseOver ? kText : kDim);
    }

    void paintItem(juce::Graphics& g, int width, int height) override {
        const int ox = getItemPosition(false).getX();
        const bool here = owner.isHere(stage), sel = isSelected(), open = isOpen();
        if (sel) {
            g.setColour(kSel);
            g.fillRect(-ox, 0, width + ox, height);
            g.setColour(kAccent);
            g.fillRect(-ox, 0, 3, height);
        } else if (open) {
            g.setColour(here ? kOpenHere : kOpenRow);
            g.fillRect(-ox, 0, width + ox, height);
        }
        if (! open) {
            g.setColour(kSep);
            g.fillRect(-ox, height - 1, width + ox, 1);
        }

        const bool full = isFull(stage);
        const float sc = fontScale();
        const juce::Font plain(14.5f * sc), bold(14.5f * sc, juce::Font::bold), small(11.0f * sc);
        auto cell = [&](int col) { return owner.columnRect(col).translated(-ox, 0).withY(0).withHeight(height).reduced(5, 0); };
        auto text = [&](int col, const juce::String& t, juce::Colour c) {
            if (cell(col).getWidth() < 8) return;   // column folded away
            if (sel && c == kDim) c = kSelDim;
            g.setColour(full ? c.withMultipliedAlpha(0.55f) : c);
            g.setFont(plain);
            g.drawText(t, cell(col), juce::Justification::centredLeft, true);
        };

        auto nameArea = cell(colStage);
        if (stage.locked) {
            drawLock(g, nameArea.removeFromLeft(14), full ? kDim.withMultipliedAlpha(0.6f) : kWarn.withMultipliedAlpha(0.85f));
            nameArea.removeFromLeft(5);
        }
        if (stage.test) {
            const auto tag = nameArea.removeFromRight(36).withSizeKeepingCentre(32, 15);
            g.setColour(kWarn.withAlpha(0.85f));
            g.drawRoundedRectangle(tag.toFloat(), 3.0f, 1.0f);
            g.setFont(juce::Font(10.0f * sc));
            g.drawText("TEST", tag, juce::Justification::centred, false);
        }
        if (here) {
            const juce::String t = TRANS("you are here");
            const int tw = tagWidth(t, sc);
            if (nameArea.getWidth() >= tw + 60) {
                const int nameW = juce::jmin(bold.getStringWidth(stage.name) + 2, nameArea.getWidth() - tw - 8);
                drawTag(g, juce::Rectangle<int>(nameArea.getX() + nameW + 8, 0, tw, height).withSizeKeepingCentre(tw, 17), t, sc);
                nameArea.setWidth(nameW);
            }
        }
        g.setColour(full ? kText.withMultipliedAlpha(0.55f) : juce::Colours::white);
        g.setFont(bold);
        g.drawText(stage.name, nameArea, juce::Justification::centredLeft, true);

        text(colAbout, stage.about, kDim);
        text(colCity, stage.city, kText);
        text(colCountry, stage.country, kText);
        text(colGenre, stage.genre, kText);
        text(colPlayers, stage.maxPlayers > 0 ? juce::String(stage.count) + " / " + juce::String(stage.maxPlayers) : juce::String(stage.count), kText);

        juce::String ping = stage.pingMs >= 0.0f ? juce::String(juce::roundToInt(stage.pingMs)) + " ms"
                          : stage.pingMs > -1.5f ? TRANS("no answer") : juce::String("...");
        text(colPing, ping, pingColour(stage.pingMs));

        const juce::String ver = (stage.version + " " + stage.os).trim();
        text(colVersion, ver, kDim);
        if (stage.older && cell(colVersion).getWidth() >= 8) {
            auto r = cell(colVersion);
            r.removeFromLeft(plain.getStringWidth(ver) + 6);
            if (r.getWidth() >= small.getStringWidth(TRANS("older")) + 2) {
                g.setColour(kWarn.withMultipliedAlpha(full ? 0.55f : 1.0f));
                g.setFont(small);
                g.drawText(TRANS("older"), r, juce::Justification::centredLeft, true);
            }
        }
    }

    // The whole entry in the hover when a column is folded away or too narrow for its text
    juce::String getTooltip() override {
        const float sc = fontScale();
        const juce::Font plain(14.5f * sc), bold(14.5f * sc, juce::Font::bold);
        auto cut = [&](int col, const juce::String& t, int room) {
            return t.isNotEmpty() && (float) (room < 0 ? owner.columnRect(col).getWidth() - 10 : room) < plain.getStringWidthFloat(t);
        };
        int nameRoom = owner.columnRect(colStage).getWidth() - 10 - (stage.locked ? 19 : 0) - (stage.test ? 36 : 0);
        const int tw = tagWidth(TRANS("you are here"), sc);
        if (owner.isHere(stage) && nameRoom >= tw + 60) nameRoom -= tw + 8;
        const juce::String ver = (stage.version + " " + stage.os).trim();
        const bool any = bold.getStringWidthFloat(stage.name) > (float) nameRoom
                      || cut(colAbout, stage.about, -1) || cut(colCity, stage.city, -1) || cut(colCountry, stage.country, -1)
                      || cut(colGenre, stage.genre, -1) || cut(colVersion, ver, -1);
        if (! any) return {};
        juce::StringArray lines;
        lines.add(stage.name);
        if (stage.about.isNotEmpty()) lines.add(stage.about);
        const juce::String place = stage.city + (stage.city.isNotEmpty() && stage.country.isNotEmpty() ? ", " : "") + stage.country;
        if (place.isNotEmpty()) lines.add(place);
        if (stage.genre.isNotEmpty()) lines.add(stage.genre);
        if (ver.isNotEmpty()) lines.add(ver);
        return lines.joinIntoString("\n");
    }

    void itemDoubleClicked(const juce::MouseEvent&) override {
        StagesView& o = owner;
        const Stage s = stage;   // the row may be rebuilt while a dialog is open
        o.requestJoin(s);
    }
    void itemOpennessChanged(bool open) override {
        if (owner.mRebuilding) return;
        owner.mOpenChoice[stage.id] = open;
        owner.updateExpandAll();
    }
    void itemSelectionChanged(bool selected) override {
        if (owner.mRebuilding) return;
        if (selected) owner.mSelectedId = stage.id;
        else if (owner.mSelectedId == stage.id) owner.mSelectedId.clear();
        owner.updateConnect();
    }

    const Stage& getStage() const { return stage; }

private:
    StagesView& owner;
    Stage stage;
};

// The header cell above the open arrows: one click opens or closes every stage.
class StagesView::ExpandAllButton : public juce::Button
{
public:
    ExpandAllButton() : juce::Button("ExpandAll") { setWantsKeyboardFocus(false); setShowsOpen(false); }

    bool showsOpen() const { return open; }
    void setShowsOpen(bool o) {
        open = o;
        setTooltip(o ? TRANS("Collapse all") : TRANS("Expand all"));
        repaint();
    }

    void paintButton(juce::Graphics& g, bool over, bool down) override {
        auto r = getLocalBounds();
        g.setColour(kHeaderBg);
        g.fillRect(r);
        g.setColour(kSep);   // the header's own lines
        g.fillRect(r.removeFromBottom(1));
        g.fillRect(r.removeFromRight(1));
        drawOpenArrow(g, r.toFloat(), open, over || down ? kText : kDim);
    }

private:
    bool open = false;
};

// The opened part of a stage: a small table of who is playing, then how long it has been listed and the invite link.
class StagesView::SubItem : public juce::TreeViewItem
{
public:
    enum Kind { header, player, note, info };

    SubItem(StagesView& o, const Stage& s, Kind k, int playerIndex = -1, const juce::String& t = {})
        : owner(o), stage(s), kind(k), index(playerIndex), text(t) { setDrawsInLeftMargin(true); }

    bool mightContainSubItems() override { return false; }
    bool canBeSelected() const override { return false; }
    juce::String getUniqueName() const override { return stage.id + "/" + juce::String((int) kind) + "/" + juce::String(index); }
    void paintHorizontalConnectingLine(juce::Graphics&, const juce::Line<float>&) override {}
    void paintVerticalConnectingLine(juce::Graphics&, const juce::Line<float>&) override {}
    int getItemHeight() const override {
        if (kind == note) {   // a note too long for the row wraps onto a second line
            const int full = owner.mTree.getWidth() - owner.mTree.getViewport()->getScrollBarThickness();
            const PlayerColumns c = owner.playerColumns(full);
            return juce::Font(13.5f * fontScale()).getStringWidthFloat(text) > (float) juce::jmax(60, full - c.name - 12) ? 42 : 25;
        }
        return kind == info ? 32 : kind == header ? 24 : 25;
    }

    // A player's full name and instrument in the hover when their columns are too narrow
    juce::String getTooltip() override {
        if (kind != player || lastFull <= 0 || index < 0 || index >= stage.players.size()) return {};
        const PlayerColumns c = owner.playerColumns(lastFull);
        const juce::Font f(13.5f * fontScale());
        const juce::String name = stage.players[index];
        const juce::String inst = index < stage.instruments.size() ? stage.instruments[index] : juce::String();
        const bool cut = f.getStringWidthFloat(name) > (float) c.nameW
                      || (inst.isNotEmpty() && (c.instW <= 0 || f.getStringWidthFloat(inst) > (float) c.instW));
        return cut ? name + (inst.isNotEmpty() ? " (" + inst + ")" : juce::String()) : juce::String();
    }

    void paintItem(juce::Graphics& g, int width, int height) override {
        const int ox = getItemPosition(false).getX();
        const int full = width + ox;   // the whole row, from the left edge of the list
        lastFull = full;
        g.setColour(kDetailBg);
        g.fillRect(-ox, 0, full, height);

        const bool here = owner.isHere(stage);
        const bool hidden = stage.players.isEmpty() && stage.count > 0;
        g.setColour(here ? kHere : hidden ? kPrivateBar : kAccent);
        g.fillRect(kPanelIndent - ox, 0, 3, kind == info ? height - 8 : height);

        const float sc = fontScale();
        const juce::Font f(13.5f * sc);
        const PlayerColumns c = owner.playerColumns(full);
        auto at = [&](int x, int w) { return juce::Rectangle<int>(x - ox, 0, juce::jmax(0, w), height); };
        const auto left = juce::Justification::centredLeft, right = juce::Justification::centredRight;

        if (kind == header) {
            g.setColour(kPanelHead);
            g.setFont(juce::Font(10.5f * sc));
            // Captions wrap onto two lines when a narrow window squeezes their columns
            auto caption = [&](const juce::String& t, juce::Rectangle<int> r, juce::Justification j) {
                SonoLookAndFeel::drawTextNoSqueeze(g, t.toUpperCase(), r, j, 2);
            };
            caption(TRANS("Player"), at(c.name, c.nameW), left);
            if (c.instW > 0) caption(TRANS("Instrument"), at(c.inst, c.instW), left);
            caption(TRANS("On stage"), at(c.since, c.numW), right);
            caption(TRANS("Their ping"), at(c.ping, c.numW), right);
            caption(TRANS("Sending"), at(c.send, c.numW), right);
            g.setColour(kPanelLine);
            g.fillRect(c.name - ox, height - 1, c.right - c.name, 1);
            return;
        }
        if (kind == player) {
            const juce::String name = stage.players[index];
            auto nameArea = at(c.name, c.nameW);
            const juce::String me = owner.stageUserName ? owner.stageUserName() : juce::String();
            if (here && me.isNotEmpty() && name == me.substring(0, 30)) {   // the stage lists the first 30 characters
                const juce::String t = TRANS("you");
                const int tw = tagWidth(t, sc);
                if (nameArea.getWidth() >= tw + 50) {
                    const int nameW = juce::jmin(f.getStringWidth(name) + 2, nameArea.getWidth() - tw - 8);
                    drawTag(g, juce::Rectangle<int>(nameArea.getX() + nameW + 8, 0, tw, height).withSizeKeepingCentre(tw, 16), t, sc);
                    nameArea.setWidth(nameW);
                }
            }
            g.setFont(f);
            g.setColour(kText);
            g.drawText(name, nameArea, left, true);
            if (c.instW > 0) {
                const bool given = index < stage.instruments.size();
                const juce::String inst = given ? stage.instruments[index] : juce::String("-");
                g.setColour(inst.isEmpty() || ! given ? kDim : kText.withMultipliedAlpha(0.85f));
                g.drawText(inst.isEmpty() ? TRANS("not set") : inst, at(c.inst, c.instW), left, true);
            }
            const int secs = index < stage.playerSeconds.size() ? stage.playerSeconds[index] : -1;
            g.setColour(secs >= 0 ? kText : kDim);
            g.drawText(onStageFor(secs), at(c.since, c.numW), right, true);
            const int ping = index < stage.playerPing.size() ? stage.playerPing[index] : -1;
            g.setColour(pingColour(ping >= 0 ? (float) ping : -1.0f));
            g.drawText(ping >= 0 ? juce::String(ping) + " ms" : juce::String("-"), at(c.ping, c.numW), right, true);
            const int ch = index < stage.playerChannels.size() ? stage.playerChannels[index] : 0;
            g.setColour(kDim);
            g.drawText(ch > 0 ? juce::String(ch) + " ch" : juce::String("-"), at(c.send, c.numW), right, true);
            if (index + 1 < stage.players.size()) {
                g.setColour(kPanelLine.withMultipliedAlpha(0.7f));
                g.fillRect(c.name - ox, height - 1, c.right - c.name, 1);
            }
            return;
        }
        g.setFont(f);
        if (kind == note) {
            g.setColour(kDim);
            SonoLookAndFeel::drawTextNoSqueeze(g, text, { c.name - ox, 0, juce::jmax(60, full - c.name - 12), height }, left, 2);
            return;
        }
        g.setColour(kSep);
        g.fillRect(-ox, height - 1, full, 1);
        const juce::Font mf(12.5f * sc);
        g.setFont(mf);
        const juce::String listed = TRANS("Listed for") + " " + listedFor(stage.since);
        g.setColour(kDim);
        g.drawText(listed, c.name - ox, 0, juce::jmax(60, full - c.name - 12), height - 8, left, true);
        const juce::String linkText = stage.test ? TRANS("Made-up test stage: it cannot be joined.") : TRANS("Copy invite link");
        link = juce::Rectangle<int>(c.name - ox + mf.getStringWidth(listed) + 16, 0, mf.getStringWidth(linkText) + 6, height - 8);
        g.setColour(stage.test ? kWarn.withMultipliedAlpha(0.8f) : kAccent);
        g.drawText(linkText, link, left, true);
    }

    void itemClicked(const juce::MouseEvent& e) override {
        if (kind != info || stage.test || ! link.contains(e.getPosition())) return;
        const juce::URL url = juce::URL("sonobusrp://" + stage.host + ":" + juce::String(stage.port) + "/").withParameter("g", stage.name);
        juce::SystemClipboard::copyTextToClipboard(url.toString(true));
        owner.flash(TRANS("Invite link copied."));
    }

private:
    StagesView& owner;
    Stage stage;
    Kind kind;
    int index;
    juce::String text;
    juce::Rectangle<int> link;
    int lastFull = 0;   // row width at the last paint
};

//==============================================================================
StagesView::StagesView()
{
    setOpaque(true);

    mSearch.setTextToShowWhenEmpty(TRANS("Search name, city, player"), juce::Colour(0x66ffffff));
    mSearch.setFont(juce::Font(14.0f * fontScale()));
    mSearch.onTextChange = [this] { touched(); rebuild(); };
    addAndMakeVisible(mSearch);

    mGenre.onChange = [this] { updateChoiceTips(); rebuild(); };
    mCountry.onChange = [this] { updateChoiceTips(); rebuild(); };
    fillChoice(mGenre, TRANS("All genres"), {});
    fillChoice(mCountry, TRANS("All countries"), {});
    addAndMakeVisible(mGenre);
    addAndMakeVisible(mCountry);
    mKind.addItem(TRANS("Real and test"), 1);
    mKind.addItem(TRANS("Real only"), 2);
    mKind.addItem(TRANS("Test only"), 3);
    mKind.setSelectedItemIndex(0, juce::dontSendNotification);
    mKind.onChange = [this] { updateChoiceTips(); rebuild(); };
    addChildComponent(mKind);   // shown only while the directory sends test stages

    mOpenOnly.setButtonText(TRANS("Open stages only"));
    mOpenOnly.onClick = [this] { rebuild(); };
    mHideFull.setButtonText(TRANS("Hide full"));
    mHideFull.onClick = [this] { rebuild(); };
    addAndMakeVisible(mOpenOnly);
    addAndMakeVisible(mHideFull);

    mRefresh.setButtonText(TRANS("Refresh"));
    mRefresh.onClick = [this] {
        touched();
        mWorker->refreshNow = true;
        mWorker->notify();
    };
    addAndMakeVisible(mRefresh);

    mStatus.setJustificationType(juce::Justification::centredRight);
    mStatus.setFont(juce::Font(13.0f * fontScale()));
    mStatus.setColour(juce::Label::textColourId, kDim);
    addAndMakeVisible(mStatus);

    // One Connect button for the selected row, top right.
    mConnect.setButtonText(TRANS("Connect"));
    mConnect.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff1f8f86));
    mConnect.setColour(juce::TextButton::textColourOffId, juce::Colours::white);
    mConnect.onClick = [this] { connectSelected(); };
    mConnect.setEnabled(false);
    addAndMakeVisible(mConnect);
    mSelected.setJustificationType(juce::Justification::centredRight);
    mSelected.setFont(juce::Font(14.0f * fontScale()));
    mSelected.setColour(juce::Label::textColourId, juce::Colour(0xffcfcfcf));
    mSelected.setMinimumHorizontalScale(1.0f);
    addAndMakeVisible(mSelected);
    mHint.setJustificationType(juce::Justification::centredRight);
    mHint.setFont(juce::Font(12.5f * fontScale()));
    mHint.setColour(juce::Label::textColourId, kDim);
    mHint.setText(TRANS("Double-click a stage to connect"), juce::dontSendNotification);
    addAndMakeVisible(mHint);
    mNotice.setJustificationType(juce::Justification::centredLeft);
    mNotice.setFont(juce::Font(14.0f * fontScale()));
    mNotice.setColour(juce::Label::textColourId, kWarn);
    mNotice.setMinimumHorizontalScale(1.0f);
    addAndMakeVisible(mNotice);

    const int fixed = juce::TableHeaderComponent::visible;
    const int normal = juce::TableHeaderComponent::visible | juce::TableHeaderComponent::sortable | juce::TableHeaderComponent::resizable;
    // A column never gets narrower than its caption with the sort arrow (header 28 high: bold 14, arrow 14, sides 4 + 4),
    // nor than the fixed texts it shows
    const juce::Font headFont(14.0f, juce::Font::bold);
    auto minFor = [&headFont](const juce::String& title, int base) { return juce::jmax(base, SbTextFit::textWidth(headFont, title) + 8 + 14 + 2); };
    const int pingMin = juce::jmax(minFor(TRANS("Ping"), 66), SbTextFit::textWidth(juce::Font(14.5f * fontScale()), TRANS("no answer")) + 10 + 2);
    mHeader.addColumn({}, colOpen, kIndent, kIndent, kIndent, fixed);
    mHeader.addColumn(TRANS("Stage"), colStage, 280, minFor(TRANS("Stage"), 130), -1, normal);
    mHeader.addColumn(TRANS("About"), colAbout, 300, minFor(TRANS("About"), 80), -1, normal);
    mHeader.addColumn(TRANS("City"), colCity, 150, minFor(TRANS("City"), 70), -1, normal);
    mHeader.addColumn(TRANS("Country"), colCountry, 80, minFor(TRANS("Country"), 60), -1, normal);
    mHeader.addColumn(TRANS("Genre"), colGenre, 130, minFor(TRANS("Genre"), 70), -1, normal);
    mHeader.addColumn(TRANS("Players"), colPlayers, 84, minFor(TRANS("Players"), 66), -1, normal);
    mHeader.addColumn(TRANS("Ping"), colPing, 90, pingMin, -1, normal);
    mHeader.addColumn(TRANS("Version"), colVersion, 140, minFor(TRANS("Version"), 80), -1, normal);

    // Player table: number columns as wide as their widest caption or value, the instrument column at least its caption
    {
        const juce::Font cap(10.5f * fontScale()), val(13.5f * fontScale());
        mNumNeed = juce::jmax(SbTextFit::widestText(cap, { TRANS("On stage").toUpperCase(), TRANS("Their ping").toUpperCase(), TRANS("Sending").toUpperCase() }),
                              SbTextFit::widestText(val, { TRANS("under 1 min"), "99 h 59 min", "999 ms", "64 ch" })) + 4;
        mInstMin = juce::jmax(60, SbTextFit::textWidth(cap, TRANS("Instrument").toUpperCase()) + 2);
        mInstNeed = juce::jmax(mInstMin, SbTextFit::textWidth(val, TRANS("not set")) + 2);
    }
    mHeader.setStretchToFitActive(true);
    mHeader.setPopupMenuActive(false);
    mHeader.setColour(juce::TableHeaderComponent::backgroundColourId, kHeaderBg);
    mHeader.setColour(juce::TableHeaderComponent::textColourId, kDim);
    mHeader.setColour(juce::TableHeaderComponent::outlineColourId, kSep);
    mHeader.setColour(juce::TableHeaderComponent::highlightColourId, kAccent.withAlpha(0.25f));
    mHeader.setSortColumnId(colPing, true);
    mHeader.addListener(this);
    addAndMakeVisible(mHeader);
    mExpandAll = std::make_unique<ExpandAllButton>();
    mExpandAll->onClick = [this] { setAllOpen(! mExpandAll->showsOpen()); };
    addAndMakeVisible(*mExpandAll);

    mTree.setColour(juce::TreeView::backgroundColourId, kBg);
    mTree.setIndentSize(kIndent);
    mTree.setDefaultOpenness(false);
    mTree.setMultiSelectEnabled(false);
    mTree.setOpenCloseButtonsVisible(true);
    mRoot = std::make_unique<RootItem>();
    mTree.setRootItem(mRoot.get());
    mTree.setRootItemVisible(false);
    addAndMakeVisible(mTree);

    addMouseListener(this, true);   // any use of the list counts as a touch

    mWorker = std::make_unique<Worker>(*this);
    mWorker->startThread();

    setSize(1320, 812);
    updateStatus();
}

StagesView::~StagesView()
{
    stopTimer();
    mWorker->shutdown();
    cancelPendingUpdate();
    mHeader.removeListener(this);
    mTree.setRootItem(nullptr);
}

void StagesView::setDirectoryMemory(const juce::File& file) { mWorker->setMemory(file); }

void StagesView::setWantTestStages(bool want)
{
    if (mWorker->wantTest.exchange(want) == want) return;
    mWorker->refreshNow = true;
    mWorker->notify();
}

void StagesView::setActive(bool active)
{
    if (mWorker->active.exchange(active) == active) return;
    if (active) {
        mAllOpen = true;   // each viewing starts with the stages open
        mOpenChoice.clear();
        rebuild();
        mTouchedAt = juce::Time::getMillisecondCounter();
        mIdle = false;
        mWorker->idle = false;
        mWorker->refreshNow = true;
        mWorker->notify();
        startTimer(1000);
    } else {
        stopTimer();
    }
}

void StagesView::touched()
{
    mTouchedAt = juce::Time::getMillisecondCounter();
    if (! mIdle) return;
    mIdle = false;
    mWorker->idle = false;
    mWorker->refreshNow = true;
    mWorker->notify();
    updateStatus();
}

bool StagesView::isHere(const Stage& s) const
{
    const juce::String here = currentStage ? currentStage() : juce::String();
    if (here.isEmpty() || s.name != here) return false;
    const int port = currentPort ? currentPort() : 0;
    return port <= 0 || port == s.port;
}

bool StagesView::joinByName(const juce::String& name)
{
    for (const auto& s : mStages)
        if (s.name == name) { const Stage copy = s; touched(); requestJoin(copy); return true; }
    return false;
}

juce::String StagesView::counters() const
{
    return "list requests " + juce::String(mWorker->attempts.load()) + ", pings sent " + juce::String(mWorker->pingsSent.load())
         + (mWorker->active.load() ? (mIdle ? ", idle" : ", in use") : ", asleep");
}

juce::Rectangle<int> StagesView::columnRect(int columnId) const
{
    if (! mHeader.isColumnVisible(columnId)) return {};   // folded away
    return mHeader.getColumnPosition(mHeader.getIndexOfColumnId(columnId, true));
}

void StagesView::paint(juce::Graphics& g)
{
    g.fillAll(kBg);

    auto f = mFooter;
    g.setColour(kHeaderBg);
    g.fillRect(f);
    g.setColour(kSep);
    g.fillRect(f.removeFromTop(1));
    f = f.reduced(12, 0);
    const juce::Font font(12.5f * fontScale());
    g.setFont(font);

    juce::String me = userName ? userName() : juce::String();
    const juce::String inst = userInstrument ? userInstrument() : juce::String();
    if (me.isNotEmpty() && f.getWidth() > 700) {
        const juce::String t = TRANS("You join as") + " " + me + (inst.isNotEmpty() ? " (" + inst + ")" : juce::String());
        g.setColour(kDim);
        g.drawText(t, f.removeFromRight(font.getStringWidth(t) + 10), juce::Justification::centredRight, true);
    }

    auto put = [&](const juce::String& t, juce::Colour c, bool withDot) {
        const int w = font.getStringWidth(t) + 14 + (withDot ? 14 : 0);
        if (f.getWidth() < w) { f.setWidth(0); return; }
        if (withDot) {
            auto d = f.removeFromLeft(14);
            g.setColour(c);
            g.fillEllipse(d.toFloat().withSizeKeepingCentre(7.0f, 7.0f));
        }
        g.setColour(c);
        g.drawText(t, f.removeFromLeft(font.getStringWidth(t) + 14), juce::Justification::centredLeft, true);
    };
    if (f.getWidth() > 900) put(TRANS("Ping = round trip from this computer to the stage."), kDim, false);
    put(juce::String((int) sbplus::kStagePingGreenMs) + " ms " + TRANS("or less"), kGood, true);
    put(TRANS("up to") + " " + juce::String((int) sbplus::kStagePingYellowMs), kWarn, true);
    put(TRANS("over") + " " + juce::String((int) sbplus::kStagePingYellowMs), kBad, true);
    if (mTestData) put(TRANS("Test data is on: stages marked TEST are made up."), kWarn, false);
}

void StagesView::resized()
{
    auto r = getLocalBounds();
    const int w = r.getWidth();

    // A narrow window drops the least needed columns first.
    mHeader.setColumnVisible(colAbout, w >= 1080);
    mHeader.setColumnVisible(colVersion, w >= 900);
    mHeader.setColumnVisible(colCountry, w >= 780);
    mHeader.setColumnVisible(colGenre, w >= 680);
    mHeader.setColumnVisible(colCity, w >= 560);

    mConnectRow = r.removeFromTop(46);
    layoutConnectRow();

    const bool oneRow = w >= (mKind.isVisible() ? 1400 : 1180);
    auto top = r.removeFromTop(oneRow ? 44 : 80).reduced(10, 7);
    mCountry.setVisible(w >= 620);
    // Captions get their measured width; a choice box never gets narrower than its "All ..." entry
    const int rowH = 30;
    const int openW = SbTextFit::toggleWidth(mOpenOnly, rowH) + 8, hideW = SbTextFit::toggleWidth(mHideFull, rowH) + 8;
    const int refreshW = juce::jmax(90, SbTextFit::buttonWidth(mRefresh, rowH));
    const int genreMin = SbTextFit::comboWidth(mGenre, rowH, TRANS("All genres"));
    const int countryMin = SbTextFit::comboWidth(mCountry, rowH, TRANS("All countries"));
    int kindMin = 0;
    for (int i = 0; i < mKind.getNumItems(); ++i) kindMin = juce::jmax(kindMin, SbTextFit::comboWidth(mKind, rowH, mKind.getItemText(i)));
    if (oneRow) {
        mSearch.setBounds(top.removeFromLeft(230));  top.removeFromLeft(8);
        mGenre.setBounds(top.removeFromLeft(juce::jmax(170, genreMin)));   top.removeFromLeft(8);
        mCountry.setBounds(top.removeFromLeft(juce::jmax(150, countryMin))); top.removeFromLeft(8);
        if (mKind.isVisible()) { mKind.setBounds(top.removeFromLeft(juce::jmax(140, kindMin))); top.removeFromLeft(8); }
        top.removeFromLeft(4);
        mOpenOnly.setBounds(top.removeFromLeft(openW));
        mHideFull.setBounds(top.removeFromLeft(hideW));
        mRefresh.setBounds(top.removeFromLeft(refreshW));  top.removeFromLeft(8);
        mStatus.setBounds(top);
    } else {
        auto row1 = top.removeFromTop(rowH);
        top.removeFromTop(6);
        mRefresh.setBounds(row1.removeFromRight(refreshW)); row1.removeFromRight(8);
        if (mCountry.isVisible()) { mCountry.setBounds(row1.removeFromRight(juce::jmax(countryMin, juce::jmin(150, w / 5)))); row1.removeFromRight(8); }
        mGenre.setBounds(row1.removeFromRight(juce::jmax(genreMin, juce::jmin(170, w / 4)))); row1.removeFromRight(8);
        mSearch.setBounds(row1);
        mOpenOnly.setBounds(top.removeFromLeft(openW));
        mHideFull.setBounds(top.removeFromLeft(hideW));
        if (mKind.isVisible()) { mKind.setBounds(top.removeFromLeft(juce::jmax(kindMin, juce::jmin(140, w / 4)))); top.removeFromLeft(8); }
        mStatus.setBounds(top);
    }
    updateChoiceTips();

    mFooter = r.removeFromBottom(30);
    auto header = r.removeFromTop(28);
    header.removeFromRight(mTree.getViewport()->getScrollBarThickness());
    mHeader.setBounds(header);
    mHeader.resizeAllColumnsToFit(header.getWidth());
    mExpandAll->setBounds(columnRect(colOpen).translated(header.getX(), header.getY()).withHeight(header.getHeight()));
    mTree.setBounds(r);
    if (mRoot != nullptr) mRoot->treeHasChanged();
}

void StagesView::tableColumnsChanged(juce::TableHeaderComponent*) { mTree.repaint(); }
void StagesView::tableColumnsResized(juce::TableHeaderComponent*) { if (mRoot != nullptr) mRoot->treeHasChanged(); mTree.repaint(); }
void StagesView::tableSortOrderChanged(juce::TableHeaderComponent*) { rebuild(); }

void StagesView::timerCallback()
{
    if (! mIdle && juce::Time::getMillisecondCounter() - mTouchedAt > (juce::uint32) kIdleAfterMs) {
        mIdle = true;
        mWorker->idle = true;
    }
    updateStatus();
    if (mNotice.getText().isNotEmpty() && juce::Time::getMillisecondCounter() - mNoticeAt > 60000u) showNotice({});
    const juce::String here = currentStage ? currentStage() : juce::String();
    if (here != mHereShown) {   // joined or left: the tags and the Connect button follow
        mHereShown = here;
        mTree.repaint();
    }
    updateConnect();
    repaint(mFooter);
}

// The columns of the small player table, as x positions from the left edge of the list.
StagesView::PlayerColumns StagesView::playerColumns(int fullWidth) const
{
    const float sc = fontScale();
    const int gap = 20;
    PlayerColumns c;
    c.name = kPanelIndent + 3 + 14;
    const int avail = juce::jmax(160, fullWidth - c.name - 14);
    c.numW = mNumNeed;
    int flex = avail - 3 * (c.numW + gap);
    if (flex < 110) {   // a narrow window squeezes the number columns first (their captions wrap)
        c.numW = juce::jmax(44, (avail - 110) / 3 - gap);
        flex = avail - 3 * (c.numW + gap);
    }
    // The instrument column takes what its longest entry needs, the name column the rest up to its cap.
    // Names come first: when they do not fit, the instrument column gives way down to its caption, then folds.
    const int nameMax = juce::jmax(90, juce::roundToInt(250.0f * sc)), instMax = juce::roundToInt(200.0f * sc);
    c.instW = juce::jmin(instMax, mInstNeed);
    c.nameW = juce::jlimit(90, nameMax, flex - c.instW - gap);
    if (c.nameW < juce::jmin(nameMax, mNameNeed)) {
        c.instW = juce::jlimit(mInstMin, juce::jmax(mInstMin, c.instW), flex - juce::jmin(nameMax, mNameNeed) - gap);
        c.nameW = juce::jlimit(90, nameMax, flex - c.instW - gap);
    }
    if (flex - c.nameW - gap < mInstMin) {   // no room for the instrument
        c.instW = 0;
        c.nameW = juce::jlimit(90, nameMax, flex);
    }
    c.inst = c.name + c.nameW + gap;
    c.since = c.instW > 0 ? c.inst + c.instW + gap : c.inst;
    c.ping = c.since + c.numW + gap;
    c.send = c.ping + c.numW + gap;
    c.right = c.send + c.numW;
    return c;
}

void StagesView::layoutConnectRow()
{
    auto bar = mConnectRow.reduced(10, 7);
    if (bar.isEmpty()) return;
    mConnect.setBounds(bar.removeFromRight(juce::roundToInt(130.0f * fontScale())));
    bar.removeFromRight(12);
    const int selW = mSelected.getText().isEmpty() ? 0
                   : juce::jmin(juce::jmax(0, bar.getWidth() - 10), mSelected.getFont().getStringWidth(mSelected.getText()) + 16);
    mSelected.setBounds(bar.removeFromRight(selW));
    const int hintW = mHint.getFont().getStringWidth(mHint.getText()) + 16;
    const bool showHint = bar.getWidth() >= hintW + (mNotice.getText().isNotEmpty() ? 420 : 8);   // the notice comes first
    mHint.setVisible(showHint);
    if (showHint) { bar.removeFromRight(6); mHint.setBounds(bar.removeFromRight(hintW)); }
    mNotice.setBounds(bar);

    // The full text in the hover when a label has to wrap past its lines or end in "..."
    auto tipIfCut = [](juce::Label& l) {
        const auto font = l.getFont();
        const int lines = juce::jmax(1, (int) ((float) (l.getHeight() - 2) / font.getHeight()));
        const int room = lines * (l.getWidth() - 10) - (lines > 1 ? juce::roundToInt(font.getHeight() * 3.0f) : 0);   // wrapping loses some at word ends
        l.setTooltip(SbTextFit::textWidth(font, l.getText()) > room ? l.getText() : juce::String());
    };
    tipIfCut(mSelected);
    tipIfCut(mNotice);
}

// Connect follows the selected row: off with nothing selected and on the stage this player is on.
void StagesView::updateConnect()
{
    const Stage* sel = nullptr;
    for (const auto& s : mRows) if (s.id == mSelectedId) { sel = &s; break; }
    const juce::String t = sel != nullptr ? TRANS("Selected:") + " " + sel->name : juce::String();
    if (mSelected.getText() != t) {
        mSelected.setText(t, juce::dontSendNotification);
        layoutConnectRow();
    }
    mConnect.setEnabled(sel != nullptr && ! isHere(*sel));
}

void StagesView::connectSelected()
{
    for (const auto& s : mRows)
        if (s.id == mSelectedId) { const Stage copy = s; touched(); requestJoin(copy); return; }
}

void StagesView::handleAsyncUpdate()
{
    std::map<juce::String, float> pings;
    juce::StringArray genres;
    {
        const juce::ScopedLock l(mWorker->lock);
        mStages = mWorker->stages;
        genres = mWorker->genres;
        pings = mWorker->pings;
        mReached = mWorker->reached;
        mEverReached = mWorker->everReached;
        mTestData = mWorker->testData;
        mFetchedAt = mWorker->fetchedAt;
    }
    mAttempts = mWorker->attempts.load();
    juce::StringArray countries;
    for (auto& s : mStages) {
        if (! s.test) {
            const auto it = pings.find(s.id);
            s.pingMs = it != pings.end() ? it->second : -2.0f;
        }
        if (s.country.isNotEmpty()) countries.addIfNotAlreadyThere(s.country);
        if (s.genre.isNotEmpty()) genres.addIfNotAlreadyThere(s.genre);
    }
    countries.sort(true);
    fillChoice(mGenre, TRANS("All genres"), genres);
    fillChoice(mCountry, TRANS("All countries"), countries);
    if (mKind.isVisible() != mTestData) { mKind.setVisible(mTestData); resized(); }
    rebuild();
    repaint(mFooter);
}

void StagesView::fillChoice(juce::ComboBox& box, const juce::String& allText, const juce::StringArray& items)
{
    juce::StringArray want;
    want.add(allText);
    want.addArray(items);
    juce::StringArray have;
    for (int i = 0; i < box.getNumItems(); ++i) have.add(box.getItemText(i));
    if (want == have) return;
    const juce::String chosen = box.getSelectedItemIndex() > 0 ? box.getText() : juce::String();
    box.clear(juce::dontSendNotification);
    box.addItemList(want, 1);
    box.setSelectedItemIndex(chosen.isEmpty() ? 0 : juce::jmax(0, want.indexOf(chosen)), juce::dontSendNotification);
    updateChoiceTips();
}

void StagesView::updateChoiceTips()
{
    for (auto* box : { &mGenre, &mCountry, &mKind })
        SbTextFit::tooltipIfCut(*box, box->getText(), SbTextFit::comboWidth(*box, box->getHeight(), box->getText()));
}

void StagesView::rebuild()
{
    const juce::String q = mSearch.getText().trim().toLowerCase();
    const juce::String genre = mGenre.getSelectedItemIndex() > 0 ? mGenre.getText() : juce::String();
    const juce::String country = mCountry.getSelectedItemIndex() > 0 ? mCountry.getText() : juce::String();
    const bool openOnly = mOpenOnly.getToggleState(), hideFull = mHideFull.getToggleState();
    const int kind = mTestData ? mKind.getSelectedItemIndex() : 0;   // 0 all, 1 real only, 2 test only

    std::vector<Stage> rows;
    for (const auto& s : mStages) {
        if ((kind == 1 && s.test) || (kind == 2 && ! s.test)) continue;
        if (genre.isNotEmpty() && s.genre != genre) continue;
        if (country.isNotEmpty() && s.country != country) continue;
        if (openOnly && s.locked) continue;
        if (hideFull && isFull(s)) continue;
        if (q.isNotEmpty()) {
            const juce::String hay = (s.name + " " + s.city + " " + s.country + " " + s.genre + " " + s.about + " " + s.players.joinIntoString(" ") + " " + s.instruments.joinIntoString(" ")).toLowerCase();
            if (! hay.contains(q)) continue;
        }
        rows.push_back(s);
    }

    const int col = mHeader.getSortColumnId();
    const bool forwards = mHeader.isSortedForwards();
    std::stable_sort(rows.begin(), rows.end(), [col, forwards](const Stage& a, const Stage& b) {
        auto num = [](double x, double y) { return x < y ? -1 : x > y ? 1 : 0; };
        auto pingKey = [](const Stage& s) { return s.pingMs < 0.0f ? 1.0e9 : (double) s.pingMs; };
        int c = 0;
        switch (col) {
            case colStage:   c = a.name.compareNatural(b.name); break;
            case colCity:    c = a.city.compareNatural(b.city); break;
            case colCountry: c = a.country.compareNatural(b.country); break;
            case colGenre:   c = a.genre.compareNatural(b.genre); break;
            case colPlayers: c = num(a.count, b.count); if (c == 0) c = num(a.maxPlayers, b.maxPlayers); break;
            case colPing:    c = num(pingKey(a), pingKey(b)); break;
            case colVersion: c = compareVersions(a.version, b.version); if (c == 0) c = a.os.compareNatural(b.os); break;
            case colAbout:   c = a.about.compareNatural(b.about); break;
            default: break;
        }
        if (! forwards) c = -c;
        if (c == 0) c = a.name.compareNatural(b.name);
        return c < 0;
    });

    mShown = (int) rows.size();
    mShownPlayers = 0;
    for (const auto& s : rows) mShownPlayers += s.count;

    // Player table columns follow the widest name and instrument shown, the same for every stage so they line up
    {
        const juce::Font val(13.5f * fontScale());
        mNameNeed = 0;
        mInstNeed = juce::jmax(mInstMin, SbTextFit::textWidth(val, TRANS("not set")) + 2);
        for (const auto& s : rows) {
            for (const auto& p : s.players) mNameNeed = juce::jmax(mNameNeed, SbTextFit::textWidth(val, p) + 2);
            for (const auto& i : s.instruments) mInstNeed = juce::jmax(mInstNeed, SbTextFit::textWidth(val, i) + 2);
        }
    }

    const int scrollY = mTree.getViewport()->getViewPositionY();
    mRebuilding = true;
    mRoot->clearSubItems();
    for (const auto& s : rows) {
        auto* item = new StageItem(*this, s);
        if (s.players.isEmpty()) {
            const juce::String hiddenNote = juce::String(s.count) + " " + (s.count == 1 ? TRANS("player on stage.") : TRANS("players on stage."))
                                          + " " + TRANS("This stage keeps its player names private.");
            item->addSubItem(new SubItem(*this, s, SubItem::note, -1, s.count > 0 ? hiddenNote : TRANS("Nobody is here yet.")));
        } else {
            item->addSubItem(new SubItem(*this, s, SubItem::header));
            // Longest on stage first, when the stage says how long.
            std::vector<int> order;
            for (int i = 0; i < s.players.size(); ++i) order.push_back(i);
            if (s.playerSeconds.size() == s.players.size())
                std::stable_sort(order.begin(), order.end(), [&s](int a, int b) { return s.playerSeconds[a] > s.playerSeconds[b]; });
            for (int i : order) item->addSubItem(new SubItem(*this, s, SubItem::player, i));
        }
        item->addSubItem(new SubItem(*this, s, SubItem::info));
        mRoot->addSubItem(item);
        if (wantOpen(s)) item->setOpen(true);
        if (s.id == mSelectedId) item->setSelected(true, false, juce::dontSendNotification);
    }
    mRebuilding = false;
    mRows = std::move(rows);
    mTree.getViewport()->setViewPosition(0, scrollY);
    updateStatus();
    updateConnect();
    updateExpandAll();
}

bool StagesView::wantOpen(const Stage& s) const
{
    const auto it = mOpenChoice.find(s.id);
    if (it != mOpenChoice.end()) return it->second;
    return mAllOpen && s.count > 0;   // a stage with nobody on it shows just its row
}

bool StagesView::mostlyOpen() const
{
    int withPlayers = 0, openWith = 0, all = 0, openAll = 0;
    for (int i = 0; i < mRoot->getNumSubItems(); ++i) {
        const auto* item = dynamic_cast<const StageItem*>(mRoot->getSubItem(i));
        if (item == nullptr) continue;
        const bool o = item->isOpen();
        ++all; openAll += o ? 1 : 0;
        if (item->getStage().count > 0) { ++withPlayers; openWith += o ? 1 : 0; }
    }
    return withPlayers > 0 ? openWith * 2 >= withPlayers : all > 0 && openAll * 2 >= all;
}

void StagesView::setAllOpen(bool open)
{
    touched();
    mAllOpen = open;
    if (open) {
        for (auto it = mOpenChoice.begin(); it != mOpenChoice.end();)   // keeps empty stages the user opened
            it = it->second ? std::next(it) : mOpenChoice.erase(it);
        bool anyPlayers = false;
        for (const auto& s : mRows) anyPlayers = anyPlayers || s.count > 0;
        if (! anyPlayers)   // nobody anywhere: open every stage
            for (const auto& s : mRows) mOpenChoice[s.id] = true;
    } else {
        mOpenChoice.clear();
    }
    mRebuilding = true;
    for (int i = 0; i < mRoot->getNumSubItems(); ++i)
        if (auto* item = dynamic_cast<StageItem*>(mRoot->getSubItem(i)))
            item->setOpen(wantOpen(item->getStage()));
    mRebuilding = false;
    updateExpandAll();
}

void StagesView::updateExpandAll()
{
    if (mExpandAll != nullptr && mRoot != nullptr) {
        const bool o = mostlyOpen();
        if (o != mExpandAll->showsOpen()) mExpandAll->setShowsOpen(o);
    }
}

void StagesView::flash(const juce::String& note)
{
    mNote = note;
    mNoteUntil = juce::Time::getMillisecondCounter() + 4000;
    updateStatus();
}

void StagesView::showNotice(const juce::String& text)
{
    mNotice.setText(text, juce::dontSendNotification);
    mNoticeAt = juce::Time::getMillisecondCounter();
    layoutConnectRow();
}

void StagesView::updateStatus()
{
    juce::String t;
    juce::Colour c = kDim;
    if (mNote.isNotEmpty() && juce::Time::getMillisecondCounter() < mNoteUntil) {
        t = mNote;
        c = kWarn;
    } else if (! mEverReached) {
        t = mAttempts > 0 ? TRANS("The stage directory cannot be reached.") : TRANS("Loading the stage list");
        if (mAttempts > 0) c = kWarn;
    } else {
        const int total = (int) mStages.size();
        t = (mShown < total ? juce::String(mShown) + " " + TRANS("of") + " " + juce::String(total) : juce::String(total)) + " " + TRANS("stages")
          + dot() + juce::String(mShownPlayers) + " " + TRANS("players");
        if (mReached) {
            const juce::int64 age = juce::jmax((juce::int64) 0, (juce::Time::currentTimeMillis() - mFetchedAt) / 1000);
            t += dot() + TRANS("updated") + " " + juce::String(age) + " s " + TRANS("ago");
            if (mIdle) t += dot() + TRANS("resting");
        } else {
            t += dot() + TRANS("the directory cannot be reached");
            c = kWarn;
        }
    }
    mStatus.setColour(juce::Label::textColourId, c);
    mStatus.setText(t, juce::dontSendNotification);
}

void StagesView::requestJoin(const Stage& s)
{
    if (isHere(s)) { flash(TRANS("You are on this stage.")); return; }
    if (s.test)    { flash(TRANS("That is a made-up test stage. It cannot be joined.")); return; }
    if (isFull(s)) { flash(TRANS("That stage is full.")); return; }
    if (userName && userName().isEmpty()) { flash(TRANS("Enter your name in the Connect window first.")); return; }

    JoinRequest req;
    req.host = s.host;
    req.port = s.port;
    req.name = s.name;
    const bool locked = s.locked;
    juce::Component::SafePointer<StagesView> safe(this);
    auto go = [safe, req, locked] {
        if (safe == nullptr) return;
        safe->showNotice({});   // a new try: the last refusal is no longer the news
        safe->mSavedTryActive = false;
        const juce::String saved = locked && safe->savedPassword ? safe->savedPassword(req) : juce::String();
        if (saved.isNotEmpty()) {   // accepted here before: no question
            JoinRequest j = req;
            j.password = saved;
            safe->mSavedTry = j;
            safe->mSavedTryActive = true;
            if (safe->onJoin) safe->onJoin(j);
        }
        else if (locked) safe->askPassword(req);
        else if (safe->onJoin) safe->onJoin(req);
    };

    const juce::String here = currentStage ? currentStage() : juce::String();
    if (here.isNotEmpty()) {
        juce::AlertWindow::showOkCancelBox(juce::MessageBoxIconType::QuestionIcon, TRANS("Join stage"),
                                           TRANS("Leave") + " \"" + here + "\" " + TRANS("and join") + " \"" + s.name + "\"?",
                                           TRANS("Join"), TRANS("Cancel"), this,
                                           juce::ModalCallbackFunction::create([go](int result) { if (result == 1) go(); }));
        return;
    }
    go();
}

void StagesView::joinRefused(const juce::String& stage, const juce::String& why)
{
    const bool savedOne = mSavedTryActive && stage == mSavedTry.name && why.containsIgnoreCase("password");
    mSavedTryActive = false;
    if (! savedOne) return;
    if (forgetPassword) forgetPassword(mSavedTry);
    if (showMe) showMe();
    JoinRequest r = mSavedTry;
    r.password = {};
    askPassword(r, TRANS("The saved password was rejected. Enter the current password."));
}

void StagesView::askPassword(const JoinRequest& req, const juce::String& note)
{
    auto* w = new juce::AlertWindow(TRANS("Stage password"), note.isEmpty() ? req.name : req.name + "\n\n" + note,
                                    juce::MessageBoxIconType::NoIcon, this);
    w->addTextEditor("pw", {}, TRANS("Password"), true);
    w->addButton(TRANS("Join"), 1, juce::KeyPress(juce::KeyPress::returnKey));
    w->addButton(TRANS("Cancel"), 0, juce::KeyPress(juce::KeyPress::escapeKey));
    juce::Component::SafePointer<StagesView> safe(this);
    w->enterModalState(true, juce::ModalCallbackFunction::create([safe, w, req](int result) {
        if (result != 1 || safe == nullptr) return;
        JoinRequest j = req;
        j.password = w->getTextEditorContents("pw");
        if (safe->onJoin) safe->onJoin(j);
    }), true);
}
