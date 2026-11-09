// SonobusHubGeo — player city/state lookup shared by the Linux web page and the Mac hub GUI.
// One worker thread, one plain HTTP GET to ip-api.com per public IP for the life of the process (3 s timeout), results
// cached; private/LAN addresses never leave the machine. Also the platform string used in both headers.
#pragma once
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>

class HubGeoResolver {
public:
    HubGeoResolver() = default;
    ~HubGeoResolver() { stop(); }
    void start(std::function<void()> onUpdate = {});   // onUpdate: called on the worker thread after a lookup lands
    void stop();
    // "LAN" for private addresses, "" while a lookup is pending (or disabled), "–" when the lookup failed, else "City, ST".
    std::string locationFor(const std::string& ip, bool lan, bool enabled = true);
    static bool isPrivateIp(const std::string& ip);
    static std::string geoLookup(const std::string& ip);   // blocking; "" on failure
private:
    void threadMain();
    std::mutex mLock;
    std::map<std::string, std::string> mCache;   // ip -> "City, ST" | "" (pending) | "-" (failed)
    std::deque<std::string> mQueue;
    std::condition_variable mCv;
    std::thread mThread;
    bool mRun = false;
    std::function<void()> mOnUpdate;
};

// "Linux, Ubuntu 22.04.5 LTS" / "macOS 15.7 (Sequoia)" for the status headers.
std::string hubPlatformString();
std::string hubBuildId();
std::string hubCommitId();   // SBPlusCommit from the bundle, else the compile-time commit; hubBuildId: date build id (CFBundleVersion / SONOBUS_HUB_BUILD / commit)
