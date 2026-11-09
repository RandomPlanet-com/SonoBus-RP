// See SonobusHubGeo.h (moved out of SonobusHubWeb.cpp so the Mac GUI shares it)
#include "SonobusHubGeo.h"
#include "JuceHeader.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#ifndef SONOBUS_HUB_VERSION
#define SONOBUS_HUB_VERSION "dev"
#endif
#ifndef SONOBUS_HUB_BUILD
#define SONOBUS_HUB_BUILD ""
#endif
#ifndef SONOBUS_HUB_COMMIT
#define SONOBUS_HUB_COMMIT "dev"
#endif
#include "SonobusPlusVersion.h"

// The hub's DATE build id — Mac bundle: CFBundleVersion stamped by the release scripts;
// Linux: -DSONOBUS_HUB_BUILD from the package script; otherwise the source commit.
std::string hubBuildId() {
    const auto info = SonobusPlusVersion::get();
    if (info.version.isNotEmpty()) return info.version.toStdString();
    if (std::string(SONOBUS_HUB_BUILD).size() > 0) return std::string(SONOBUS_HUB_BUILD);
    return std::string(SONOBUS_HUB_COMMIT);
}

// The source commit — Mac bundle: SBPlusCommit stamped by the release scripts (the compile-time define can be
// stale in a long-lived build dir); Linux: -DSONOBUS_HUB_COMMIT from the build.
std::string hubCommitId() {
    const auto info = SonobusPlusVersion::get();
    if (info.commit.isNotEmpty()) return info.commit.toStdString();
    return std::string(SONOBUS_HUB_COMMIT);
}

static long long geoNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool HubGeoResolver::isPrivateIp(const std::string& ip) {
    unsigned a = 0, b = 0;
    if (sscanf(ip.c_str(), "%u.%u.", &a, &b) != 2) return ip.empty() || ip == "::1" || ip.rfind("fe80", 0) == 0 || ip.rfind("fd", 0) == 0;
    return a == 10 || a == 127 || (a == 172 && b >= 16 && b <= 31) || (a == 192 && b == 168) || (a == 169 && b == 254) || (a == 100 && b >= 64 && b <= 127) || a == 0;
}

void HubGeoResolver::start(std::function<void()> onUpdate) {
    std::lock_guard<std::mutex> g(mLock);
    if (mRun) return;
    mOnUpdate = std::move(onUpdate);
    mRun = true;
    mThread = std::thread([this] { threadMain(); });
}

void HubGeoResolver::stop() {
    { std::lock_guard<std::mutex> g(mLock); if (!mRun) return; mRun = false; }
    mCv.notify_all();
    if (mThread.joinable()) mThread.join();
}

std::string HubGeoResolver::locationFor(const std::string& ip, bool lan, bool enabled) {
    if (lan || isPrivateIp(ip)) return "LAN";
    if (!enabled) return "";
    std::lock_guard<std::mutex> g(mLock);
    auto it = mCache.find(ip);
    if (it != mCache.end()) return it->second == "-" ? "\xe2\x80\x93" : it->second;   // "-" = lookup failed → en dash
    if (!mRun) return "";
    mCache[ip] = "";   // pending
    mQueue.push_back(ip);
    mCv.notify_one();
    return "";
}

void HubGeoResolver::threadMain() {
    for (;;) {
        std::string ip;
        {
            std::unique_lock<std::mutex> l(mLock);
            mCv.wait_for(l, std::chrono::milliseconds(500), [this] { return !mRun || !mQueue.empty(); });
            if (!mRun) return;
            if (mQueue.empty()) continue;
            ip = mQueue.front(); mQueue.pop_front();
        }
        const std::string loc = geoLookup(ip);
        std::cout << "[HUB-GEO] location " << ip << " -> " << (loc.empty() ? "(lookup failed)" : loc) << "\n";
        std::function<void()> cb;
        { std::lock_guard<std::mutex> g(mLock); mCache[ip] = loc.empty() ? "-" : loc; cb = mOnUpdate; }
        if (cb) cb();
    }
}

static void geoSendAll(int fd, const std::string& s) {
    size_t off = 0;
    while (off < s.size()) {
        const ssize_t n = ::send(fd, s.data() + off, s.size() - off,
#ifdef MSG_NOSIGNAL
                                 MSG_NOSIGNAL
#else
                                 0
#endif
                                 );
        if (n <= 0) return; off += (size_t) n;
    }
}

// One plain HTTP/1.0 GET (no curl, no JUCE URL — JUCE_USE_CURL=0 on both hubs). ip-api.com: free for non-commercial
// use, 45 requests/min, HTTP only; we ask once per IP for the life of the hub process.
std::string HubGeoResolver::geoLookup(const std::string& ip) {
    struct addrinfo hints; memset(&hints, 0, sizeof(hints)); hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* res = nullptr;
    if (getaddrinfo("ip-api.com", "80", &hints, &res) != 0 || !res) return "";
    const int fd = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) { freeaddrinfo(res); return ""; }
    struct timeval tv; tv.tv_sec = 3; tv.tv_usec = 0;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)); ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#ifdef SO_NOSIGPIPE
    { int one = 1; ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one)); }
#endif
    const int fl = fcntl(fd, F_GETFL, 0); fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    int rc = ::connect(fd, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    if (rc < 0 && errno != EINPROGRESS) { ::close(fd); return ""; }
    if (rc < 0) {
        struct pollfd pf; pf.fd = fd; pf.events = POLLOUT; pf.revents = 0;
        if (::poll(&pf, 1, 3000) <= 0) { ::close(fd); return ""; }
        int err = 0; socklen_t el = sizeof(err);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el) < 0 || err != 0) { ::close(fd); return ""; }
    }
    fcntl(fd, F_SETFL, fl);
    const std::string req = "GET /json/" + ip + "?fields=status,city,region HTTP/1.0\r\nHost: ip-api.com\r\nUser-Agent: SonoBusRPHub/" SONOBUS_HUB_VERSION "\r\nConnection: close\r\n\r\n";
    geoSendAll(fd, req);
    std::string resp; char buf[1024];
    const long long t0 = geoNowMs();
    while (resp.size() < 8192 && geoNowMs() - t0 < 3500) {
        const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        resp.append(buf, (size_t) n);
    }
    ::close(fd);
    const size_t hdr = resp.find("\r\n\r\n");
    if (hdr == std::string::npos || resp.rfind("HTTP/1.", 0) != 0 || resp.find(" 200 ") == std::string::npos) return "";
    const juce::var root = juce::JSON::parse(juce::String::fromUTF8(resp.c_str() + hdr + 4, (int)(resp.size() - hdr - 4)));
    auto* obj = root.getDynamicObject();
    if (!obj || obj->getProperty("status").toString() != "success") return "";
    const std::string city = obj->getProperty("city").toString().toStdString();
    const std::string region = obj->getProperty("region").toString().toStdString();
    if (city.empty() && region.empty()) return "";
    return city.empty() ? region : region.empty() ? city : city + ", " + region;
}

std::string hubPlatformString() {
#if defined(__linux__)
    std::string os = "Linux";
    if (FILE* f = fopen("/etc/os-release", "r")) { char line[256]; while (fgets(line, sizeof(line), f)) { if (!strncmp(line, "PRETTY_NAME=", 12)) { std::string v(line + 12); while (!v.empty() && (v.back() == '\n' || v.back() == '"')) v.pop_back(); if (!v.empty() && v[0] == '"') v.erase(0, 1); os = "Linux, " + v; break; } } fclose(f); }
    return os;
#elif defined(__APPLE__)
    { auto n = juce::SystemStats::getOperatingSystemName(); if (n.startsWith("Mac OSX")) n = "macOS" + n.substring(7); return n.toStdString(); }   // JUCE says "Mac OSX 15.7.7"; show "macOS 15.7.7"
#else
    return "unknown OS";
#endif
}
