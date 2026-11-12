// SPDX-License-Identifier: GPLv3-or-later WITH Appstore-exception
// Copyright (C) 2026 Jason Buecker (Random Planet)
#include "StagesView.h"
#include "SonoLookAndFeel.h"
#include "SbLatencyColours.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <iterator>

namespace {

const char* const kDirectoryBase = "https://stages.randomplanet.com/api/1/";

const juce::Colour kBg(0xff1a1a1a), kHeaderBg(0xff141414), kSel(0xff17302f), kSep(0xff262626), kDetailBg(0xff132524);
const juce::Colour kText(0xffe4e4e4), kDim(0xff8f8f8f), kAccent(0xff3fb8af);
const juce::Colour kGood(0xff6abe4f), kWarn(0xffd6b03e), kBad(0xffee6666);

enum { colOpen = 1, colLock, colStage, colCity, colCountry, colGenre, colPlayers, colPing, colVersion, colAbout, colJoin };

const int kIndent = 22;
const int kRowHeight = 28;

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
    if (s < 48 * 3600) return juce::String(s / 3600) + " " + TRANS("hours");
    return juce::String(s / 86400) + " " + TRANS("days");
}

bool isFull(const StagesView::Stage& s) { return s.maxPlayers > 0 && s.count >= s.maxPlayers; }

juce::Colour pingColour(float ms) {
    if (ms < 0.0f) return kDim;
    return ms <= sbplus::kStagePingGreenMs ? kGood : ms <= sbplus::kStagePingYellowMs ? kWarn : kBad;
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
        juce::int64 nextAt = 0;
        while (! threadShouldExit()) {
            if (active.load()) {
                if (refreshNow.exchange(false) || juce::Time::currentTimeMillis() >= nextAt) {
                    const bool ok = fetch();
                    ++attempts;
                    nextAt = juce::Time::currentTimeMillis() + (ok ? 30000 : 15000);
                    view.triggerAsyncUpdate();
                    if (ok && pingAllowed.load() && ! threadShouldExit()) ping();
                }
            }
            wait(active.load() ? 500 : -1);   // asleep until the list is shown
        }
    }

    std::atomic<bool> active { false }, refreshNow { false }, pingAllowed { true }, wantTest { false };
    std::atomic<int> attempts { 0 };

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
        const juce::String body = httpGet(from + "stages" + (wantTest.load() ? "?test=1" : ""), status);
        const juce::var v = status == 200 ? juce::JSON::parse(body) : juce::var();
        const auto* arr = v["stages"].getArray();
        if (arr == nullptr) {
            const juce::ScopedLock l(lock);
            reached = false;
            return false;
        }
        std::vector<Stage> list;
        juce::String newest;
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
            if (threadShouldExit() || ! pingAllowed.load()) break;
        }
    }

    StagesView& view;
    juce::String base;      // guarded by lock
    juce::File memory;      // guarded by lock
    juce::CriticalSection streamLock;
    juce::WebInputStream* stream = nullptr;
};

//==============================================================================
// One stage = one row. Opening it shows who is playing, one player per row.
class StagesView::StageItem : public juce::TreeViewItem
{
public:
    StageItem(StagesView& o, const Stage& s) : owner(o), stage(s) {}

    bool mightContainSubItems() override { return true; }
    int getItemHeight() const override { return kRowHeight; }
    juce::String getUniqueName() const override { return stage.id; }
    void paintHorizontalConnectingLine(juce::Graphics&, const juce::Line<float>&) override {}
    void paintVerticalConnectingLine(juce::Graphics&, const juce::Line<float>&) override {}

    void paintOpenCloseButton(juce::Graphics& g, const juce::Rectangle<float>& area, juce::Colour, bool mouseOver) override {
        const auto a = area.withSizeKeepingCentre(9.0f, 9.0f);
        juce::Path p;
        if (isOpen()) p.addTriangle(a.getX(), a.getY() + 1.5f, a.getRight(), a.getY() + 1.5f, a.getCentreX(), a.getBottom() - 0.5f);
        else          p.addTriangle(a.getX() + 1.5f, a.getY(), a.getRight() - 0.5f, a.getCentreY(), a.getX() + 1.5f, a.getBottom());
        g.setColour(mouseOver ? kText : kDim);
        g.fillPath(p);
    }

    void paintItem(juce::Graphics& g, int width, int height) override {
        const int ox = getItemPosition(false).getX();
        if (isSelected()) { g.setColour(kSel); g.fillRect(0, 0, width, height); }
        g.setColour(kSep);
        g.fillRect(0, height - 1, width, 1);

        const bool full = isFull(stage);
        const float sc = fontScale();
        const juce::Font plain(14.5f * sc), bold(14.5f * sc, juce::Font::bold), small(11.0f * sc);
        auto cell = [&](int col) { return owner.columnRect(col).translated(-ox, 0).withY(0).withHeight(height).reduced(5, 0); };
        auto text = [&](int col, const juce::String& t, juce::Colour c) {
            if (cell(col).getWidth() < 8) return;   // column folded away
            g.setColour(full ? c.withMultipliedAlpha(0.55f) : c);
            g.setFont(plain);
            g.drawText(t, cell(col), juce::Justification::centredLeft, true);
        };

        if (stage.locked) drawLock(g, cell(colLock), full ? kDim.withMultipliedAlpha(0.6f) : kWarn.withMultipliedAlpha(0.85f));

        auto nameArea = cell(colStage);
        if (stage.test) {
            const auto tag = nameArea.removeFromRight(36).withSizeKeepingCentre(32, 15);
            g.setColour(kWarn.withAlpha(0.85f));
            g.drawRoundedRectangle(tag.toFloat(), 3.0f, 1.0f);
            g.setFont(juce::Font(10.0f * sc));
            g.drawText("TEST", tag, juce::Justification::centred, false);
        }
        g.setColour(full ? kText.withMultipliedAlpha(0.55f) : juce::Colours::white);
        g.setFont(bold);
        g.drawText(stage.name, nameArea, juce::Justification::centredLeft, true);

        text(colCity, stage.city, kText);
        text(colCountry, stage.country, kDim);
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
            if (r.getWidth() > 20) {
                g.setColour(kWarn.withMultipliedAlpha(full ? 0.55f : 1.0f));
                g.setFont(small);
                g.drawText(TRANS("older"), r, juce::Justification::centredLeft, true);
            }
        }
        text(colAbout, stage.about, kDim);

        const auto button = joinRect(ox, height).toFloat();
        const bool can = ! full;
        const juce::String label = full ? TRANS("Full") : stage.locked ? TRANS("Password") : TRANS("Join");
        if (can && ! stage.locked) { g.setColour(juce::Colour(0xff1d3b3a)); g.fillRoundedRectangle(button, 4.0f); }
        g.setColour(! can ? juce::Colour(0xff3a3a3a) : stage.locked ? juce::Colour(0xff6a6a6a) : kAccent);
        g.drawRoundedRectangle(button, 4.0f, 1.0f);
        g.setColour(! can ? juce::Colour(0xff6a6a6a) : kText);
        g.setFont(juce::Font(13.0f * sc));
        g.drawText(label, button.toNearestInt(), juce::Justification::centred, false);
    }

    void itemClicked(const juce::MouseEvent& e) override {
        if (joinRect(getItemPosition(false).getX(), kRowHeight).expanded(2).contains(e.getPosition())) join();
    }
    void itemDoubleClicked(const juce::MouseEvent& e) override {
        if (! joinRect(getItemPosition(false).getX(), kRowHeight).expanded(2).contains(e.getPosition())) join();
    }
    void itemOpennessChanged(bool open) override {
        if (owner.mRebuilding) return;
        if (open) owner.mOpen.insert(stage.id); else owner.mOpen.erase(stage.id);
    }
    void itemSelectionChanged(bool selected) override {
        if (selected && ! owner.mRebuilding) owner.mSelectedId = stage.id;
    }

private:
    juce::Rectangle<int> joinRect(int ox, int height) const {
        return owner.columnRect(colJoin).translated(-ox, 0).withY(0).withHeight(height).reduced(8, 4);
    }
    void join() {
        StagesView& o = owner;
        const Stage s = stage;   // the row may be rebuilt while a dialog is open
        o.requestJoin(s);
    }

    StagesView& owner;
    Stage stage;
};

// The opened part of a row: one row per player, then the limits and the invite link.
class StagesView::SubItem : public juce::TreeViewItem
{
public:
    enum Kind { player, note, info };

    SubItem(StagesView& o, const Stage& s, Kind k, const juce::String& t = {}, const juce::String& t2 = {})
        : owner(o), stage(s), kind(k), text(t), text2(t2) {}

    bool mightContainSubItems() override { return false; }
    bool canBeSelected() const override { return false; }
    juce::String getUniqueName() const override { return stage.id + "/" + juce::String((int) kind) + "/" + text; }
    void paintHorizontalConnectingLine(juce::Graphics&, const juce::Line<float>&) override {}
    void paintVerticalConnectingLine(juce::Graphics&, const juce::Line<float>&) override {}
    int getItemHeight() const override { return kind == info ? 28 : 24; }

    void paintItem(juce::Graphics& g, int width, int height) override {
        const int ox = getItemPosition(false).getX();
        g.setColour(kDetailBg);
        g.fillRect(0, 0, width, height);
        const float sc = fontScale();
        const juce::Font f(13.5f * sc);
        g.setFont(f);
        const int x = owner.columnRect(colStage).getX() - ox + 5;
        if (kind == player) {
            const auto city = owner.columnRect(colCity);
            const int nameW = city.isEmpty() ? width - x - 12 : city.getX() - ox - x - 6;
            g.setColour(kText.withMultipliedAlpha(0.92f));
            juce::String shown = text;
            if (city.isEmpty() && text2.isNotEmpty()) shown << dot() << text2;
            g.drawText(shown, x, 0, juce::jmax(40, nameW), height, juce::Justification::centredLeft, true);
            if (! city.isEmpty() && text2.isNotEmpty()) {
                g.setColour(kDim);
                g.drawText(text2, city.getX() - ox + 5, 0, juce::jmax(40, width - (city.getX() - ox) - 16), height, juce::Justification::centredLeft, true);
            }
            return;
        }
        if (kind == note) {
            g.setColour(kDim);
            g.drawText(text, x, 0, juce::jmax(60, width - x - 12), height, juce::Justification::centredLeft, true);
            return;
        }
        g.setColour(kSep);
        g.fillRect(0, height - 1, width, 1);
        const juce::String limits = (stage.maxChannels > 0 ? TRANS("Up to") + " " + juce::String(stage.maxChannels) + " " + TRANS("channels per player") + dot() : juce::String())
                                  + TRANS("listed for") + " " + listedFor(stage.since);
        g.setColour(kDim);
        g.drawText(limits, x, 0, juce::jmax(60, width - x - 12), height - 2, juce::Justification::centredLeft, true);
        const juce::String linkText = stage.test ? TRANS("Made-up test stage: it cannot be joined.") : TRANS("Copy invite link");
        link = juce::Rectangle<int>(x + f.getStringWidth(limits) + 18, 0, f.getStringWidth(linkText) + 6, height - 2);
        g.setColour(stage.test ? kWarn.withMultipliedAlpha(0.8f) : kAccent);
        g.drawText(linkText, link, juce::Justification::centredLeft, true);
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
    juce::String text, text2;
    juce::Rectangle<int> link;
};

//==============================================================================
StagesView::StagesView()
{
    setOpaque(true);

    mSearch.setTextToShowWhenEmpty(TRANS("Search name, city, player"), juce::Colour(0x66ffffff));
    mSearch.setFont(juce::Font(14.0f * fontScale()));
    mSearch.onTextChange = [this] { rebuild(); };
    addAndMakeVisible(mSearch);

    mGenre.onChange = [this] { rebuild(); };
    mCountry.onChange = [this] { rebuild(); };
    fillChoice(mGenre, TRANS("All genres"), {});
    fillChoice(mCountry, TRANS("All countries"), {});
    addAndMakeVisible(mGenre);
    addAndMakeVisible(mCountry);
    mKind.addItem(TRANS("Real and test"), 1);
    mKind.addItem(TRANS("Real only"), 2);
    mKind.addItem(TRANS("Test only"), 3);
    mKind.setSelectedItemIndex(0, juce::dontSendNotification);
    mKind.onChange = [this] { rebuild(); };
    addChildComponent(mKind);   // shown only while the directory sends test stages

    mOpenOnly.setButtonText(TRANS("Open stages only"));
    mOpenOnly.onClick = [this] { rebuild(); };
    mHideFull.setButtonText(TRANS("Hide full"));
    mHideFull.onClick = [this] { rebuild(); };
    addAndMakeVisible(mOpenOnly);
    addAndMakeVisible(mHideFull);

    mRefresh.setButtonText(TRANS("Refresh"));
    mRefresh.onClick = [this] {
        mWorker->refreshNow = true;
        mWorker->notify();
    };
    addAndMakeVisible(mRefresh);

    mStatus.setJustificationType(juce::Justification::centredRight);
    mStatus.setFont(juce::Font(13.0f * fontScale()));
    mStatus.setColour(juce::Label::textColourId, kDim);
    addAndMakeVisible(mStatus);

    const int fixed = juce::TableHeaderComponent::visible;
    const int sortOnly = juce::TableHeaderComponent::visible | juce::TableHeaderComponent::sortable;
    const int normal = sortOnly | juce::TableHeaderComponent::resizable;
    mHeader.addColumn({}, colOpen, kIndent, kIndent, kIndent, fixed);
    mHeader.addColumn({}, colLock, 28, 28, 28, sortOnly);
    mHeader.addColumn(TRANS("Stage"), colStage, 240, 120, -1, normal);
    mHeader.addColumn(TRANS("City"), colCity, 140, 70, -1, normal);
    mHeader.addColumn(TRANS("Country"), colCountry, 80, 60, -1, normal);
    mHeader.addColumn(TRANS("Genre"), colGenre, 130, 70, -1, normal);
    mHeader.addColumn(TRANS("Players"), colPlayers, 84, 66, -1, normal);
    mHeader.addColumn(TRANS("Ping"), colPing, 90, 66, -1, normal);
    mHeader.addColumn(TRANS("Version"), colVersion, 140, 80, -1, normal);
    mHeader.addColumn(TRANS("About"), colAbout, 300, 80, -1, normal);
    mHeader.addColumn({}, colJoin, 104, 104, 104, fixed);
    mHeader.setStretchToFitActive(true);
    mHeader.setPopupMenuActive(false);
    mHeader.setColour(juce::TableHeaderComponent::backgroundColourId, kHeaderBg);
    mHeader.setColour(juce::TableHeaderComponent::textColourId, kDim);
    mHeader.setColour(juce::TableHeaderComponent::outlineColourId, kSep);
    mHeader.setColour(juce::TableHeaderComponent::highlightColourId, kAccent.withAlpha(0.25f));
    mHeader.setSortColumnId(colPing, true);
    mHeader.addListener(this);
    addAndMakeVisible(mHeader);

    mTree.setColour(juce::TreeView::backgroundColourId, kBg);
    mTree.setIndentSize(kIndent);
    mTree.setDefaultOpenness(false);
    mTree.setMultiSelectEnabled(false);
    mTree.setOpenCloseButtonsVisible(true);
    mRoot = std::make_unique<RootItem>();
    mTree.setRootItem(mRoot.get());
    mTree.setRootItemVisible(false);
    addAndMakeVisible(mTree);

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
        mWorker->refreshNow = true;
        mWorker->notify();
        startTimer(1000);
    } else {
        stopTimer();
    }
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

    const bool oneRow = w >= (mKind.isVisible() ? 1400 : 1180);
    auto top = r.removeFromTop(oneRow ? 48 : 84).reduced(10, 9);
    mCountry.setVisible(w >= 620);
    if (oneRow) {
        mSearch.setBounds(top.removeFromLeft(230));  top.removeFromLeft(8);
        mGenre.setBounds(top.removeFromLeft(170));   top.removeFromLeft(8);
        mCountry.setBounds(top.removeFromLeft(150)); top.removeFromLeft(8);
        if (mKind.isVisible()) { mKind.setBounds(top.removeFromLeft(140)); top.removeFromLeft(8); }
        top.removeFromLeft(4);
        mOpenOnly.setBounds(top.removeFromLeft(160));
        mHideFull.setBounds(top.removeFromLeft(110));
        mRefresh.setBounds(top.removeFromLeft(90));  top.removeFromLeft(8);
        mStatus.setBounds(top);
    } else {
        auto row1 = top.removeFromTop(30);
        top.removeFromTop(6);
        mRefresh.setBounds(row1.removeFromRight(90)); row1.removeFromRight(8);
        if (mCountry.isVisible()) { mCountry.setBounds(row1.removeFromRight(juce::jmin(150, w / 5))); row1.removeFromRight(8); }
        mGenre.setBounds(row1.removeFromRight(juce::jmin(170, w / 4))); row1.removeFromRight(8);
        mSearch.setBounds(row1);
        mOpenOnly.setBounds(top.removeFromLeft(160));
        mHideFull.setBounds(top.removeFromLeft(110));
        if (mKind.isVisible()) { mKind.setBounds(top.removeFromLeft(juce::jmin(140, w / 4))); top.removeFromLeft(8); }
        mStatus.setBounds(top);
    }

    mFooter = r.removeFromBottom(30);
    auto header = r.removeFromTop(28);
    header.removeFromRight(mTree.getViewport()->getScrollBarThickness());
    mHeader.setBounds(header);
    mHeader.resizeAllColumnsToFit(header.getWidth());
    mTree.setBounds(r);
    if (mRoot != nullptr) mRoot->treeHasChanged();
}

void StagesView::tableColumnsChanged(juce::TableHeaderComponent*) { mTree.repaint(); }
void StagesView::tableColumnsResized(juce::TableHeaderComponent*) { if (mRoot != nullptr) mRoot->treeHasChanged(); mTree.repaint(); }
void StagesView::tableSortOrderChanged(juce::TableHeaderComponent*) { rebuild(); }

void StagesView::timerCallback()
{
    mWorker->pingAllowed = currentStage ? currentStage().isEmpty() : true;   // no pings while playing
    updateStatus();
    repaint(mFooter);
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
            case colLock:    c = num(a.locked ? 0 : 1, b.locked ? 0 : 1); break;
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

    const int scrollY = mTree.getViewport()->getViewPositionY();
    mRebuilding = true;
    mRoot->clearSubItems();
    for (const auto& s : rows) {
        auto* item = new StageItem(*this, s);
        for (int i = 0; i < s.players.size(); ++i)
            item->addSubItem(new SubItem(*this, s, SubItem::player, s.players[i], s.instruments[i]));
        if (s.players.isEmpty())
            item->addSubItem(new SubItem(*this, s, SubItem::note, s.count > 0 ? TRANS("Names are hidden by the host.") : TRANS("Nobody is here yet.")));
        item->addSubItem(new SubItem(*this, s, SubItem::info));
        mRoot->addSubItem(item);
        if (mOpen.count(s.id) > 0) item->setOpen(true);
        if (s.id == mSelectedId) item->setSelected(true, false, juce::dontSendNotification);
    }
    mRebuilding = false;
    mTree.getViewport()->setViewPosition(0, scrollY);
    updateStatus();
}

void StagesView::flash(const juce::String& note)
{
    mNote = note;
    mNoteUntil = juce::Time::getMillisecondCounter() + 4000;
    updateStatus();
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
        if (locked) safe->askPassword(req);
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

void StagesView::askPassword(const JoinRequest& req)
{
    auto* w = new juce::AlertWindow(TRANS("Stage password"), req.name, juce::MessageBoxIconType::NoIcon, this);
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
