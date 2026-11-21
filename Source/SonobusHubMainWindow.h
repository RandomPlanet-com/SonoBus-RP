// SPDX-License-Identifier: GPLv3-or-later WITH Appstore-exception
// Copyright (C) 2026 Jason Buecker (Random Planet)
// SonoBus RP — Hub Server GUI (status window).
//
// Two tabs:
//   Status   — header (uptime, group, sr/bs/feed-default ch, codec, CPU) +
//              peer table (user, in ch, feeds, addr:port, blocks, RTT, JitBuf,
//              NetJit p99, Buf, joined-at)
//   Settings — every persisted hub parameter (see SonobusHubSettings.h) with
//              Save / Save & Restart hub, and a per-field live/restart/relaunch tag
//
// No std::cout capture: a mutex-guarded tee would block the AOO receive thread. The log is hub.log.
//
// Latency policy: every GUI read goes through SonobusHubServer::snapshotForUI()
// on a 1 Hz juce::Timer (message thread). The audio thread is never accessed.
// The Settings tab touches only the settings file, the process env (before a
// relaunch) and HubJuceApp's stored launch args — never the running server.

#pragma once

#include "JuceHeader.h"
#include "SonobusHubServer.h"
#include "SonobusHubDirectory.h"
#include "SonobusHubWeb.h"   // Optional web page (off by default on macOS)
#include "SonobusHubGeo.h"   // Player locations in the native table
#include "SonobusHubSettings.h"
#include "SonobusHubUpdater.h"

#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace sb_hub_gui {

// Cells whose text was drawn shortened, with the full text (hover tooltip)
using CutCells = std::vector<std::pair<juce::Rectangle<int>, juce::String>>;

// ─── Sub-components ───────────────────────────────────────────────────────────
class StatusHeader : public juce::Component, public juce::SettableTooltipClient {
public:
    StatusHeader();
    void setSnapshot(const SonobusHubServer::UiSnapshot& s);
    void setLocation(const juce::String& loc) { mLocation = loc; repaint(); }   // The `location` setting
    void setOutRate(float bytesPerSec) { mOutBytesPerSec = bytesPerSec; }   // hub upload now, for the limits line; painted with the next snapshot
    void paint(juce::Graphics& g) override;
    juce::String getTooltip() override;   // adds the full text of any line drawn shortened
private:
    juce::StringArray mCutLines;
    SonobusHubServer::UiSnapshot mSnap;
    juce::String mLocation, mPlatform;
    juce::Image mIcon;   // Hub icon top-left, like the web page
    float mOutBytesPerSec = -1.0f;   // -1 = not measured yet
};

// Full-width notice above the tabs while a newer hub is on the update server.
class UpdateBanner : public juce::Component, public juce::SettableTooltipClient {
public:
    static constexpr int kHeight = 74;
    UpdateBanner();
    // state = install progress or the last failure, shown instead of the warning line.
    void setInfo(const HubUpdateInfo& info, const juce::String& runningVersion, bool canInstall, const juce::String& state);
    bool isAvailable() const { return mInfo.available; }
    bool canInstall() const { return mCanInstall; }
    void paint(juce::Graphics& g) override;
    void resized() override;
    std::function<void()> onInstall;
private:
    void layoutLink();
    int mLine2W = 0;   // width the second line gets left of the link
    HubUpdateInfo mInfo;
    juce::String mRunning, mState;
    bool mCanInstall = false;
    juce::TextButton mButton;
    juce::HyperlinkButton mNotes;
};

class PeerTable : public juce::Component, public juce::SettableTooltipClient {
public:
    static constexpr int kRowH = 28, kHeaderH = 30;
    int numRows() const { return (int) mPeers.size(); }   // StatusPage sizes the table to the players present
    void setSnapshot(std::vector<SonobusHubServer::UiPeerSnapshot> peers);
    void setHubFormat(int sampleRate, int blockSize) { mHubSampleRate = sampleRate; mHubBlockSize = blockSize; }   
    void paint(juce::Graphics& g) override;
    // Hover tooltips — User cell = address + audio device (+ feed warning),
    // Inputs cell = the player's channel names. The tooltip text follows the cell under the mouse.
    void mouseMove(const juce::MouseEvent& e) override;
    void mouseExit(const juce::MouseEvent&) override { mHoverRow = -1; mHoverCol = -1; }
    juce::String getTooltip() override;
    int requiredWidth() const { return mRequiredWidth; }   // Sum of the measured columns; the page scrolls sideways when narrower
    // Click a column header to sort (▲ ascending, again ▼ descending, again = hub order).
    // Applied inside setSnapshot so the choice survives the 1-s refresh. Kept in memory only — the hub has no
    // per-app properties file (hub_settings.json is the strict launch-args model).
    // Right-click on a player row: menu with "Remove from stage...", then a choice of how long, then onKick.
    void mouseDown(const juce::MouseEvent& e) override;
    std::function<void(const std::string& user, SonobusHubServer::RemovalKind keep)> onKick;
private:
    void applySort();
    void layoutColumns();   // column widths from the measured header and cell texts
    int mSortCol = -1;
    bool mSortDesc = false;
    std::vector<SonobusHubServer::UiPeerSnapshot> mPeers;
    int mHubSampleRate = 0, mHubBlockSize = 0;
    int mHoverRow = -1, mHoverCol = -1;
    std::vector<int> mColX, mColW;   // column left edges and widths (layoutColumns; hit testing)
    int mRequiredWidth = 0;
    CutCells mCut;
};

// "Recent players" — the distinct players seen in one session (the hub's own join/leave events),
// shown under the live table. Rows = the selected recent session's personnel (or the current session's when nothing
// is selected), from SonobusHubServer with locations filled by HubContent.
class RecentTable : public juce::Component, public juce::SettableTooltipClient, private juce::ScrollBar::Listener {
public:
    RecentTable();
    void setRows(std::vector<SonobusHubServer::RecentPlayer> rows);
    void paint(juce::Graphics& g) override;
    void resized() override { layoutScroll(); }
    // Real scroll bar + mouse wheel when the rows do not fit; click a column header to sort
    // (▲, again ▼, again = hub order) like the live table.
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override;
    juce::String getTooltip() override;
    std::function<void(const std::string& user)> onRemove;
    static constexpr int kRowH = 22, kHeaderH = 24, kScrollW = 12;
private:
    CutCells mCut;
    void scrollBarMoved(juce::ScrollBar*, double newRangeStart) override { mFirstRow = (int) newRangeStart; repaint(); }
    void layoutScroll();
    void applySort();
    std::vector<SonobusHubServer::RecentPlayer> mRows, mRowsHubOrder;
    juce::ScrollBar mScroll{true};
    int mFirstRow = 0, mSortCol = -1; bool mSortDesc = false;
    std::vector<int> mColX;   // column left edges from the last paint (header hit testing)
};

// "Previous players", the one list (SonobusHubServer::previousRows): removed players first, each with how long the
// removal lasts, the address and an "Allow back" button; then everyone who joined (the last 1000), newest first, each
// with a Remove button (the Kick choices) and an X that takes the name off the list. Shown in the strip's lower table
// while no recent stage is selected.
class PreviousTable : public juce::Component, public juce::SettableTooltipClient, private juce::ScrollBar::Listener {
public:
    PreviousTable();
    void setRows(std::vector<SonobusHubServer::PreviousRow> rows);
    int numRows() const { return (int) mRows.size(); }
    void paint(juce::Graphics& g) override;
    void resized() override { layoutScroll(); }
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override;
    juce::String getTooltip() override;
    std::function<void(const std::string& user)> onRemove, onForget;
    std::function<void(const std::string& user, const std::string& ip)> onAllowBack;   // the removal's name and address
    bool anyGone() const;   // a row that Clear list would take off
    static constexpr int kRowH = 22, kHeaderH = 24, kScrollW = 12, kButtonW = 92;
private:
    CutCells mCut;
    void scrollBarMoved(juce::ScrollBar*, double newRangeStart) override { mFirstRow = (int) newRangeStart; repaint(); }
    void layoutScroll();
    std::vector<SonobusHubServer::PreviousRow> mRows;
    juce::ScrollBar mScroll{true};
    int mFirstRow = 0, mButtonX = 1 << 30, mForgetX = 1 << 30;
};

// "Recent sessions" — one row per group this hub has served (SonobusHubServer::sessions,
// newest first): group, first/last used, player count, personnel. Click a row to select it (highlight); the owner
// shows that session's personnel in the RecentTable and enables "Apply & Restart". The selection is kept by GROUP
// NAME so the 2-s refresh never loses it. Mouse wheel scrolls when there are more rows than fit.
class SessionTable : public juce::Component, public juce::SettableTooltipClient, private juce::ScrollBar::Listener {
public:
    SessionTable();
    void setRows(std::vector<SonobusHubServer::SessionRecord> rows, const std::string& currentGroup);
    void paint(juce::Graphics& g) override;
    void resized() override { layoutScroll(); }
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override;
    juce::String getTooltip() override;
    const SonobusHubServer::SessionRecord* selected() const;   // nullptr = nothing selected (pointer valid until the next setRows)
    std::function<void()> onSelectionChanged;
    // "a Copy group link on the row instead of the personnel" — the main window supplies the link builder
    std::function<std::string(const SonobusHubServer::SessionRecord&)> linkFor;
    // The "X" at the end of a row removes that session (not the current one) — asks first
    std::function<void(const std::string& group)> onRemove;
    int numRows() const { return (int) mRows.size(); }
    static constexpr int kRowH = 22, kHeaderH = 24, kScrollW = 12;
private:
    // Scroll bar + header sort (▲ / ▼ / newest-first), like the live table
    void scrollBarMoved(juce::ScrollBar*, double newRangeStart) override { mFirstRow = (int) newRangeStart; repaint(); }
    void layoutScroll();
    void applySort();
    std::vector<SonobusHubServer::SessionRecord> mRows, mRowsHubOrder;
    std::string mSelectedGroup, mCurrentGroup, mCopiedGroup;
    int mLinkColX = 0, mRemoveColX = 1 << 30;   // column starts (set in paint, used by mouseDown)
    int mFirstRow = 0, mSortCol = -1; bool mSortDesc = false;
    juce::ScrollBar mScroll{true};
    std::vector<int> mColX;
    CutCells mCut;
};


// A line of text parts; a part can carry the padlock of a stage that has a password, drawn right after it.
class LockText : public juce::Component, public juce::SettableTooltipClient {
public:
    struct Part { juce::String text; juce::Colour colour; bool lock = false, bold = false; };
    void setParts(std::vector<Part> parts, float fontHeight = 15.0f);
    void paint(juce::Graphics& g) override;
    juce::String getTooltip() override;   // the whole line when it is drawn cut
private:
    std::vector<Part> mParts;
    float mFontH = 15.0f;
};

// "Players on other stages" (main hub): who is on the other stages, as each stage reports it.
class OtherPlayersTable : public juce::Component, public juce::SettableTooltipClient, private juce::ScrollBar::Listener {
public:
    struct Row { juce::String user, stage, instrument; bool locked = false, lan = false; juce::int64 joinedMs = 0; };
    OtherPlayersTable();
    void setRows(std::vector<Row> rows);
    int numRows() const { return (int) mRows.size(); }
    void paint(juce::Graphics& g) override;
    void resized() override { layoutScroll(); }
    void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override;
    juce::String getTooltip() override;
    static constexpr int kRowH = 22, kHeaderH = 24, kScrollW = 12;
private:
    void scrollBarMoved(juce::ScrollBar*, double newRangeStart) override { mFirstRow = (int) newRangeStart; repaint(); }
    void layoutScroll();
    std::vector<Row> mRows;
    juce::ScrollBar mScroll{true};
    int mFirstRow = 0;
    CutCells mCut;
};

// "<- Main hub" in a stage's own window: the arrow is drawn.
class BackButton : public juce::TextButton {
public:
    void paintButton(juce::Graphics& g, bool over, bool down) override;
    int widthFor(int height);
};

// The Status content as one tab page.
class StatusPage : public juce::Component {
public:
    StatusPage();
    void resized() override;
    // Main hub: the other stages as last read, and this stage's player count, for the server line and the table
    void setOtherStages(const std::vector<hub_settings::OtherStage>& stages);
    void setPlayersHere(int n);
    void setStageLocked(bool locked);   // padlock after the Stage field
    StatusHeader header;
    // The web page's header tools in the native GUI.
    juce::TextButton copyLinkButton{"Copy Stage Link"};
    juce::Label linkLabel;
    juce::TextButton portCheckButton{"Re-run port check"};
    std::function<void()> onCopyLink, onRerunPortCheck;
    // Group name on the front page. Apply = persist +
    // in-process hub restart (players rejoin the new group).
    juce::Label groupLabel;
    juce::TextEditor groupEditor;
    LockText groupLock;
    juce::TextButton groupApply{"Apply (restarts hub)"};
    juce::TextEditor messageEditor;
    juce::TextButton messageSend{"Send"};
    std::function<void(const juce::String&)> onSendMessage;
    std::function<void(const juce::String&)> onApplyGroup;
    // Hold mode switch (retired: never shown).
    juce::TextButton holdModeButton{"Hold: Jam"};
    std::function<void(bool)> onToggleHold;   // true = print
    juce::Label peerLabel;
    PeerTable peerTable;
    juce::Viewport peerView;   // sideways scroll when the window is narrower than the table
    LockText serverTotal;      // "On this server: N players ..." under the Players heading
    juce::Label otherLabel;
    OtherPlayersTable otherTable;
    // Bottom strip — recent sessions (left) | the selected session's personnel + password
    // override + "Apply & Restart" (right). onApplySession(group, passwordOverride) — empty override = keep the
    // password that session had.
    juce::Label sessionsLabel;
    SessionTable sessionTable;
    juce::Label recentLabel;
    RecentTable recentTable;
    PreviousTable previousTable;   // in the recent table's place while no stage is selected
    juce::TextButton clearPrevious{"Clear list"};
    juce::Label sessionPwLabel;
    juce::TextEditor sessionPwEditor;
    juce::TextButton sessionApply{"Apply & Restart"};
    std::function<void(const std::string& group, const std::string& passwordOverride)> onApplySession;
private:
    void rebuildOther();
    std::vector<hub_settings::OtherStage> mStages;
    int mPlayersHere = 0;
};

// ─── Other stages tab (the main hub only) ─────────────
// The extra stages this computer hosts, each its own copy of this app with its own settings folder
// (hub_settings::otherStages): name, settings file, ports, running or stopped, Start / Stop / Forget, and
// "Add existing stage..." (a hub_settings.json). Reads small files every 2 s while shown; never the server.
class OtherStagesPage : public juce::Component, private juce::Timer {
public:
    OtherStagesPage();
    ~OtherStagesPage() override;
    void resized() override;
    void paint(juce::Graphics& g) override;
    void visibilityChanged() override { if (isVisible()) refresh(); }
    void refresh();
    void askNewStage();
    juce::Label& heading() { return mTitle; }
    std::function<void(const juce::String&)> onMessage;   // test aid: every change of the status text
    void testAct(const juce::String& action);              // test aid: that action on the first stage it fits
private:
    struct Row;
    void timerCallback() override;
    void act(juce::File file, juce::String action);
    // The status text beside New stage...: "Starting X..." while an action runs, then its result for 5 s; a
    // failure stays until the next action; no result after 15 s = "No answer from X".
    void say(const juce::String& text, int kind);   // 0 working, 1 result (clears after 5 s), 2 failure (stays)
    void checkPending(const std::vector<hub_settings::OtherStage>& list);
    struct Pending { juce::File file; juce::String action, name; juce::int64 at = 0; int target = 0; } mPending;
    juce::int64 mMsgClearAt = 0;
    int mTicks = 0;
    void layoutRows();
    juce::Label mTitle, mHint, mMsg, mEmpty, mWarn, mForward;
    juce::TextButton mNew{"New stage..."};
    std::vector<std::unique_ptr<Row>> mRows;
    juce::String mLast;   // what the rows show (rebuilt only when it changes)
};

// ─── Log tab ─────────────────────────────────────────
// Same surface as the web page's Log tab: the hub's timestamped connection/rejection ring ([HUB-CONN]) or the tail of
// hub.log, refreshed every 2 s; "View Log" opens the full log in its own resizable window. Reads files / a static ring
// only, never the server.
class LogView : public juce::Component, private juce::Timer {
public:
    LogView(juce::File logFile, bool connectionsOnly, bool showControls);
    ~LogView() override;
    void resized() override;
    void paint(juce::Graphics& g) override;
    std::function<void()> onViewLog;
private:
    void timerCallback() override;
    void refresh();
    juce::File mLogFile;
    bool mShowControls;
    juce::ToggleButton mConnOnly{"connections only"};
    juce::TextButton mViewLog{"View Log"};
    juce::TextEditor mText;
    juce::String mLast;
};

class LogWindow : public juce::DocumentWindow {
public:
    LogWindow(juce::File logFile);
    void closeButtonPressed() override { setVisible(false); }
};

// ─── Settings tab ───────────────────────────────────────────
// Edits a working copy of HubLaunchArgs. Save → persists JSON (+ live-applies
// the Live-tier fields via the owner); Save & Restart hub → persists, then the
// owner relaunches the process. Message thread only; no server access.
class SettingsPanel : public juce::Component {
public:
    struct Callbacks {
        // Persist `args` and live-apply the Live-tier fields. false + err on failure.
        std::function<bool(const HubLaunchArgs& args, std::string& err)> save;
        // Spawn the replacement process (the owner quits this one on success).
        std::function<bool(std::string& err)> relaunch;
        // Persist only the stage password (empty = none); it applies after the hub restarts.
        std::function<bool(const std::string& password, std::string& err)> setStagePassword;
    };
    SettingsPanel(const HubLaunchArgs& running, const HubSettingsMeta& meta, Callbacks cb);
    ~SettingsPanel() override;

    void resized() override;
    void paint(juce::Graphics& g) override;
    // The address found at startup, shown in the empty Public address field. Never saved.
    void setDetectedAddress(const juce::String& address);

private:
    struct Row;
    void resetToDefaults();
    void buildRows();
    void layoutForm();
    void controlsFromModel(const HubLaunchArgs& a);
    void onRowEdited(Row& r);
    void applyStagePassword(const juce::String& password);
    void refreshStatus(const juce::String& transient = {});
    void updateCutTips();
    bool doSave();

    HubLaunchArgs mRunning;   // effective values of the live hub (Live-tier fields track saves)
    HubLaunchArgs mSaved;     // what the file holds (== running until first Save)
    HubLaunchArgs mEdit;      // what the controls hold
    HubSettingsMeta mMeta;
    Callbacks mCb;
    bool mHaveFile = false;   // a settings file exists (loaded at launch or written by Save)

    juce::Viewport mViewport;
    juce::Component mForm;    // viewed component: section labels + rows
    std::vector<std::unique_ptr<juce::Label>> mSectionLabels;
    std::vector<std::unique_ptr<juce::Label>> mSectionHints;   // one per section label; null = no hint line
    std::vector<std::unique_ptr<Row>> mRows;
    std::set<std::string> mInvalid;   // keys whose text failed to parse

    juce::Label mPathLabel;
    juce::Label mStatus;
    juce::TextButton mResetButton{"Reset to Defaults"};
    juce::TextButton mRevertButton{"Revert"};
    juce::String mDetectedAddress;
    bool mDetectedShown = false;
    juce::TextButton mSaveButton{"Save"};
    juce::TextButton mSaveRestartButton{"Save & Restart hub"};
};

// ─── Window content (root component) ──────────────────────────────────────────
class HubContent : public juce::Component, public juce::SettableTooltipClient, private juce::Timer {
public:
    HubUpdateCheck* updateCheck() { return mUpdateCheck.get(); }
    void showSettingsTab() { showTab("Settings"); }
    void showTab(const juce::String& name) { for (int t = 0; t < mTabs.getNumTabs(); ++t) if (mTabs.getTabNames()[t] == name) mTabs.setCurrentTabIndex(t); }
    HubGeoResolver& geoResolver() { return mGeo; }   // Shared with the web page so locations never differ
    // restartFn: called when the operator clicks the Restart button. The
    // owning JuceApp wires this to a stop()/start() cycle of the SonobusHub.
    // quitFn: called when Quit is clicked. Typically systemRequestedQuit().
    HubContent(SonobusHubServer* hub,
               const HubLaunchArgs& running,
               const HubSettingsMeta& meta,
               SettingsPanel::Callbacks settingsCb,
               std::function<void()> restartFn,
               std::function<void()> quitFn,
               std::function<void(const std::string&)> applyGroupFn,    // Status-page group field
               std::function<void(bool)> applyHoldFn,                    // unused (retired hold mode)
               std::function<void(const std::string&, const std::string&)> applySessionFn);   // Recent session -> Apply & Restart
    ~HubContent() override;

    void resized() override;
    void paint(juce::Graphics& g) override;
private:
    std::unique_ptr<HubUpdateCheck> mUpdateCheck;   
    void timerCallback() override;
    void refreshSessionDetail();
    void startInstallUpdate();
    std::string mPwPrefillGroup;   // The session whose password was last prefilled, so refreshes do not overwrite typing; refreshSessionDetail: personnel table + Apply button follow the selected session

    SonobusHubServer* mHub;
    std::function<bool(std::string& err)> mRelaunchFn;
    std::unique_ptr<juce::ThreadWithProgressWindow> mUpdateRun;
    UpdateBanner mUpdateBanner;
    juce::String mUpdateFailure;   // shown in the banner after a failed install
    juce::Label mPortAlert;        // red bar while a required port is closed from outside
    std::function<void()> mRestartFn;
    std::function<void()> mQuitFn;

    // For bandwidth rate derivation (bytes/s) — we diff successive snapshots.
    uint64_t mLastBytesIn = 0;
    uint64_t mLastBytesOut = 0;
    int64_t  mLastSampleMs = 0;
    float    mInBytesPerSec  = 0.0f;
    float    mOutBytesPerSec = 0.0f;
    // Per-player Up/Down rates from successive per-peer counters.
    struct PeerBytes { uint64_t in = 0, out = 0; int64_t sampleMs = 0; int64_t joinedAtMs = 0; };
    std::map<int32_t, PeerBytes> mPeerBytes;

    // Locations + log for the native pages.
    HubGeoResolver mGeo;
    bool mGeoEnabled = true;
    bool mSnapHeadingsLogged = false;   // test aid
    juce::TooltipWindow mTooltips { nullptr, 500 };   // Hover tooltips on the players table
    juce::File mLogFile;
    std::unique_ptr<LogWindow> mLogWindow;
    // Pages are declared BEFORE the TabbedComponent so the tab bar (which
    // holds non-owning references to them) is destroyed first.
    StatusPage mStatusPage;
    SettingsPanel mSettings;
    std::unique_ptr<LogView> mLogPage;
    std::unique_ptr<OtherStagesPage> mStagesPage;   // main hub only
    // Main hub: the other stages' players for the Status tab, read on a worker (small files), shown on the message thread
    std::unique_ptr<juce::ThreadPool> mStageRead;
    bool mStageReadBusy = false;
    BackButton mMainHubButton;   // a stage's window: brings the main hub's window forward
    juce::TabbedComponent mTabs{juce::TabbedButtonBar::TabsAtTop};
    juce::TextButton mRestartButton{"Restart Hub"};
    juce::TextButton mSupportButton{"Save Logs for Support"};
    std::unique_ptr<juce::Thread> mSupportRun;   // builds the support zip off the message thread
    int mWebPort = 0;
    void saveLogsForSupport();
    int mLogCheckTicks = 0;
    // Test aid, off unless SB_HUB_SNAPSHOT=<folder> (environment or the hub taps file): every 10 s the window draws
    // itself into PNG files there, the Status and Other stages tabs at each width in SB_HUB_SNAPSHOT_WIDTHS (default:
    // the current width).
    juce::File mSnapDir;
    std::vector<int> mSnapWidths;
    int mSnapTicks = 0;
    void saveSnapshots(const juce::String& tag = {});
    juce::TextButton mQuitButton{"Quit"};
};

// ─── DocumentWindow ───────────────────────────────────────────────────────────
class HubMainWindow : public juce::DocumentWindow {
public:
    HubMainWindow(const juce::String& title,
                  SonobusHubServer* hub,
                  const HubLaunchArgs& running,
                  const HubSettingsMeta& meta,
                  SettingsPanel::Callbacks settingsCb,
                  std::function<void()> restartFn,
                  std::function<void()> quitFn,
                  std::function<void(const std::string&)> applyGroupFn,
                  std::function<void(bool)> applyHoldFn,
                  std::function<void(const std::string&, const std::string&)> applySessionFn);   
    void closeButtonPressed() override;
};


// ─── JUCEApplication entry point ──────────────────────────────────────────────
class HubJuceApp : public juce::JUCEApplication {
public:
    HubJuceApp(const HubLaunchArgs& args, const HubSettingsMeta& meta, std::vector<std::string> argv);

    const juce::String getApplicationName() override       { return "SonoBus RP Hub"; }   // (AOO user name stays "SonoBus-Hub": the client keys on it)
    const juce::String getApplicationVersion() override    { return "1.0.0"; }
    bool moreThanOneInstanceAllowed() override             { return hub_settings::hasOwnDataDir(); }   // own folder: guarded by its own lock

    void initialise(const juce::String& commandLine) override;
    void shutdown() override;
    void systemRequestedQuit() override;
    void anotherInstanceStarted(const juce::String& commandLine) override;   // Windows: brings the window back
    // Close button / Quit: on Windows asks first when players are on the stage, then stops the hub.
    void userRequestedStop();

private:
    // Tear down + bring back the SonobusHubServer with the stored launch
    // args. Used by the GUI's "Restart Hub" button (and, after a Save, applies
    // every Restart-tier setting without leaving the process).
    void doRestart();
    // Construct (or re-construct) the main window pointed at the current
    // mHub instance and wired to doRestart() + systemRequestedQuit().
    void makeWindow();
    // Settings tab → persist + live-apply (log_file) + adopt into mArgs.
    bool applySavedSettings(const HubLaunchArgs& a, std::string& err);
    // Settings tab → spawn replacement process; on success quit this one.
    bool doRelaunch(std::string& err);
    // Status-page group field → persist + doRestart.
    void applyGroupAndRestart(const std::string& group);
    // Jam/Print → persist + live-apply (no restart).
    void applyHoldMode(bool print);
    // Status-page "Recent sessions" → overlay that session's settings + group + password
    // (override when non-empty) onto mArgs via hub_settings::applySession, persist through applySavedSettings (the
    // Settings tab's Save path), then restart: in-process doRestart(), or doRelaunch() when a Relaunch-tier (SB_AOO_*)
    // value differs — the same two mechanisms the Settings tab uses.
    void applySessionAndRestart(const std::string& group, const std::string& passwordOverride);
    // Application menu with "About SonoBus RP Hub".
    void installAppMenu();
    void showAbout();
    void askFirstLaunch();
    void startHubAndWindow();
    // (re)start the web status page against the CURRENT mHub (web_port 0 = off).
    void startWeb();
    std::unique_ptr<juce::MenuBarModel> mMenuModel;
    void attachWindowMenu();                  // Windows: the menu sits in the window (no app menu bar)
    std::unique_ptr<juce::Timer> mQuitWatch;  // a QUIT file in the data folder stops the hub (Windows Stop file; Stop of another stage)
    bool mStopAsked = false;

    HubLaunchArgs mArgs;
    HubSettingsMeta mMeta;
    std::vector<std::string> mArgv;   // original argv (for relaunch)
    std::unique_ptr<SonobusHubServer> mHub;
    std::unique_ptr<SonobusHubWeb> mWeb;   // Holds a reference to *mHub → reset BEFORE mHub
    std::unique_ptr<HubStageDirectory> mStageDirectory;   // Holds a reference to *mHub → reset BEFORE mHub
    void startStageDirectory();
    bool mStartFailed = false;   // Surfaced as an alert after makeWindow
    bool mRestartQueued = false;      // message thread: a restart is waiting or running
    juce::int64 mRestartDoneAtMs = 0; // when the last one finished: a click queued during it is dropped
    bool mRelaunching = false;        // the replacement process is already started
    std::unique_ptr<HubMainWindow> mWindow;
};

// Entry point invoked from SonobusHubMain.cpp::main when --nogui is NOT set.
int runGui(const HubLaunchArgs& args, const HubSettingsMeta& meta, int argc, char* argv[]);

} // namespace sb_hub_gui
