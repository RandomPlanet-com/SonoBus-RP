// SonobusHubWeb
// A tiny dependency-free HTTP/1.1 status server inside the hub: GET / = the status page, GET /api/status = JSON built
// from SonobusHubServer::snapshotForUI() (the same numbers the Mac hub GUI shows). Optional Basic auth (--web-password).
// One thread, blocking accept with a short poll timeout, requests are tiny; never touches the audio path.
#pragma once
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>

#include "SonobusHubGeo.h"        // Location lookup shared with the Mac GUI
#include "SonobusHubSettings.h"
#include "SonobusHubUpdater.h"   // Settings section shares the hub's form spec + bindings

#include "SonobusHubServer.h"

class SonobusHubWeb {
public:
    SonobusHubWeb(SonobusHubServer& hub, int port, std::string password);
    ~SonobusHubWeb();
    bool start();
    void stop();
    int port() const { return mPort; }
    std::string statusJson() const;   // public for tests / the GUI path
    static std::string joinLink(const SonobusHubServer::UiSnapshot& s);
    std::string removePlayerRequest(const std::string& body, bool& ok);   // POST /api/sessions/remove
    std::string removeSessionRequest(const std::string& body, bool& ok);   // POST /api/sessions/remove
    static std::string joinLinkFor(const std::string& host, int port, const std::string& group, const std::string& password);   // // shared with the Mac GUI
    void setUpdateCheck(sb_hub_gui::HubUpdateCheck* check) { mUpdateCheck = check; }   // call before start()
    void setGeoResolver(HubGeoResolver* shared) { mSharedGeo = shared; }   // Call before start
    void setLocation(std::string loc) { mLocation = std::move(loc); }   // Shown in the header
    // Enable the Settings section. `args` is the live launch-args model (owned by the
    // caller, outlives this object; only this thread touches it after start), `file` the settings file ("" = disabled),
    // `requestRestart` ends the process cleanly (systemd Restart=always brings it back with the saved file).
    void setSettings(sb_hub_gui::HubLaunchArgs* args, juce::File file, std::function<void()> requestRestart);
    std::string settingsJson() const;
    std::string logJson(const std::string& query) const;   // /api/log (connection ring | hub.log tail)
    std::string recentJson() const;   // /api/recent (current session's players, newest first)
    std::string sessionsJson() const;   // /api/sessions (recent group sessions, newest first; passwords masked)
private:
    std::string applySessionRequest(const std::string& body, bool& ok);   // POST /api/sessions/apply -> JSON reply
    void threadMain();
    std::string buildStatusJson() const;
    void handleClient(int fd);
    bool authorised(const std::string& headers) const;
    std::string applySettings(const std::string& body, bool& ok);   // returns the JSON reply
    // Per-player city/state via the shared HubGeoResolver (SonobusHubGeo.h).
    std::string locationFor(const std::string& ip, bool lan) const;
    bool geoEnabled() const;
    mutable HubGeoResolver mGeo;
    HubGeoResolver* mSharedGeo = nullptr;   // The Mac GUI's resolver when the GUI hosts the page (one cache, no blank locations)
    SonobusHubServer& mHub;
    int mPort;
    std::string mPassword;
    std::string mLocation;
    sb_hub_gui::HubLaunchArgs* mArgs = nullptr;
    juce::File mSettingsFile;
    std::function<void()> mRequestRestart;
    sb_hub_gui::HubUpdateCheck* mUpdateCheck = nullptr;
    std::thread mUpdateThread;
    std::atomic<bool> mUpdateRunning{ false };
    mutable std::mutex mUpdateLock;
    std::string mUpdateState;
    std::string chatRequest(const std::string& body, bool& ok);
    std::string licensesText() const;
    bool updateReady() const;
    std::string updateLine() const;
    std::string startUpdate(bool& ok);
    mutable std::mutex mSettingsLock;
    // One snapshot per second shared by every viewer (N browsers cost the same as one)
    mutable std::mutex mCacheLock;
    mutable std::string mCachedJson;
    mutable long long mCachedAtMs = 0;
    int mListenFd = -1;
    std::atomic<bool> mRun{false};
    std::thread mThread;
    const long long mStartedAtMs;
};
