// SPDX-License-Identifier: GPLv3-or-later WITH Appstore-exception
// Copyright (C) 2026 Jason Buecker (Random Planet)
// SonoBus RP — Hub Server implementation

#include "SonobusHubServer.h"
#include "SonobusHubMixControl.h"
#include "SonobusHubLayout.h"   
#include "SonobusHubSettings.h"
#include "aoo/aoo_pcm.h"
#include "aoo/aoo_opus.h"
#include "oscpack/osc/OscReceivedElements.h"
#include "oscpack/osc/OscOutboundPacketStream.h"

#include <algorithm>
#include <chrono>
#include <deque>   // Connection-log ring
#include <map>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>   // Local firewall inspection
#include <fcntl.h>    // Port check
#include <netdb.h>
#include <arpa/inet.h>
#include <poll.h>     // Recv thread poll set
#include <pthread.h>
#include <pwd.h>
#include <sys/resource.h>
#include <sys/socket.h>   // disconnectx
#include <cstdlib>
// One monotonic tick source for the whole hub.
//   macOS: mach_absolute_time ticks + mach timebase.
//   Linux: CLOCK_MONOTONIC nanoseconds, timebase 1/1, clock_nanosleep(TIMER_ABSTIME) for absolute waits.
#if defined(__APPLE__)
  #include <mach/mach_time.h>   // SCM_TIMESTAMP_MONOTONIC → ns
  struct hub_tb_t { uint32_t numer, denom; };
  static inline void hub_timebase(hub_tb_t* tb) { mach_timebase_info_data_t m; mach_timebase_info(&m); tb->numer = m.numer; tb->denom = m.denom; }
  static inline uint64_t hubNowTicks() { return mach_absolute_time(); }
  static inline void hubWaitUntilTicks(uint64_t t) { mach_wait_until(t); }
  static inline void hub_set_thread_name(const char* n) { pthread_setname_np(n); }
#elif defined(__linux__)
  #include <time.h>
  #include <sched.h>
  #include <pthread.h>
  struct hub_tb_t { uint32_t numer, denom; };
  static inline void hub_timebase(hub_tb_t* tb) { tb->numer = 1; tb->denom = 1; }
  static inline uint64_t hubNowTicks() { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (uint64_t) ts.tv_sec * 1000000000ULL + (uint64_t) ts.tv_nsec; }
  static inline void hubWaitUntilTicks(uint64_t t) { struct timespec ts; ts.tv_sec = (time_t)(t / 1000000000ULL); ts.tv_nsec = (long)(t % 1000000000ULL); while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr) == EINTR) {} }
  static inline void hub_set_thread_name(const char* n) { char b[16]; strncpy(b, n, 15); b[15] = 0; pthread_setname_np(pthread_self(), b); }
  // Linux stand-in for the Mach time-constraint policy: SCHED_FIFO (needs root or CAP_SYS_NICE; warns otherwise)
  static inline void hub_set_fifo(int prioFromMax, const char* who) {
      struct sched_param sp; sp.sched_priority = std::max(1, sched_get_priority_max(SCHED_FIFO) - prioFromMax);
      if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) != 0) std::cerr << "[HUB] WARNING: SCHED_FIFO failed for " << who << " (" << strerror(errno) << ") — run as root or grant CAP_SYS_NICE\n";
      else std::cout << "[HUB] " << who << " SCHED_FIFO prio " << sp.sched_priority << "\n";
  }
#endif
// Recvmsg with the kernel's monotonic receive timestamp (ns, 0 = none).
// Same semantics as recvfrom (flags 0, source address filled).
static ssize_t hubRecvKts(int fd, char* buf, size_t len, struct sockaddr* sa, socklen_t* salen, uint64_t* ktsNs) {
    struct iovec iov; iov.iov_base = buf; iov.iov_len = len;
    alignas(8) char cbuf[64];
    struct msghdr mh; memset(&mh, 0, sizeof(mh));
    mh.msg_name = sa; mh.msg_namelen = *salen; mh.msg_iov = &iov; mh.msg_iovlen = 1; mh.msg_control = cbuf; mh.msg_controllen = sizeof(cbuf);
    ssize_t n = ::recvmsg(fd, &mh, 0);
    *ktsNs = 0;
    if (n >= 0) {
        *salen = mh.msg_namelen;
        for (struct cmsghdr* c = CMSG_FIRSTHDR(&mh); c; c = CMSG_NXTHDR(&mh, c)) {
#if defined(__APPLE__)
            if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_TIMESTAMP_MONOTONIC && c->cmsg_len >= CMSG_LEN(sizeof(uint64_t))) {
                uint64_t t; memcpy(&t, CMSG_DATA(c), sizeof(t));
                static mach_timebase_info_data_t tb = {0, 0};
                if (tb.denom == 0) mach_timebase_info(&tb);
                *ktsNs = t * tb.numer / tb.denom;
            }
#elif defined(__linux__)
            // Linux: SCM_TIMESTAMPNS is CLOCK_REALTIME; shift it onto CLOCK_MONOTONIC with an offset sampled once
            // so the value compares with hubNowTicks() (the jitter stats only use deltas anyway).
            if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_TIMESTAMPNS && c->cmsg_len >= CMSG_LEN(sizeof(struct timespec))) {
                struct timespec rt; memcpy(&rt, CMSG_DATA(c), sizeof(rt));
                static const int64_t off = []{ struct timespec m, r; clock_gettime(CLOCK_MONOTONIC, &m); clock_gettime(CLOCK_REALTIME, &r);
                    return ((int64_t) m.tv_sec * 1000000000LL + m.tv_nsec) - ((int64_t) r.tv_sec * 1000000000LL + r.tv_nsec); }();
                *ktsNs = (uint64_t)(((int64_t) rt.tv_sec * 1000000000LL + rt.tv_nsec) + off);
            }
#endif
        }
    }
    return n;
}
// Dissolve a connected UDP socket. macOS: connect(AF_UNSPEC) returns EAFNOSUPPORT for UDP, so
// disconnectx(2); Linux/BSD: connect() with an AF_UNSPEC address is the documented way.
static int hubDissolveUdp(int fd) {
#if defined(__APPLE__)
    return ::disconnectx(fd, SAE_ASSOCID_ANY, SAE_CONNID_ANY);
#else
    struct sockaddr sa; memset(&sa, 0, sizeof(sa)); sa.sa_family = AF_UNSPEC;
    return ::connect(fd, &sa, sizeof(sa));
#endif
}
static bool hubIdleMaskEnabled() {   // SB_IDLE_MASK=0 disables (A/B measurements)
    static const bool on = [] { const char* e = std::getenv("SB_IDLE_MASK"); return !(e && e[0] == '0'); }();
    return on;
}
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#if defined(__APPLE__)
  #include <mach/mach.h>
  #include <mach/mach_time.h>
  #include <mach/thread_act.h>
  #include <mach/thread_policy.h>
#elif defined(__linux__)
  #include <time.h>
#endif

#include <opus/opus_defines.h>
#include <cstdio>    // Printf formatting in drainRtLog
#include <cstdlib>

// AOO_HUB_DEBUG=1 in env enables verbose per-packet/per-tick diagnostic logs. Off by default: only
// high-signal events are logged (peer joined/left/wired, audio flow stats every 2 s, server signaling).
static const bool kHubDebug = []() {
    const char* v = std::getenv("AOO_HUB_DEBUG");
    return v && *v && v[0] != '0';
}();

// Each peer gets a unique AOO id, peerId * 2, shared by its sink and its source.
// SonoBus clients create each RemotePeer's oursink and oursource at the same id (ourId), and the
// peer handshake assumes the other end does too (peer->remoteSourceId = peer->remoteSinkId in
// SonobusPluginProcessor.cpp). With different ids, the peer's oursink->invite_source() lands on a
// non-existent source on the hub and inbound audio wiring is half-broken. Sharing is safe because
// AOO_TYPE_SOURCE and AOO_TYPE_SINK dispatch by type + id, so both can coexist at one id.
// CPU pause hint for the audio thread's short bounded barrier spin. Pure instruction: no syscall,
// no scheduler interaction (std::this_thread::yield() on macOS is a swtch_pri syscall per call).
static inline void hub_cpu_pause() {
#if defined(__aarch64__) || defined(__arm64__)
    asm volatile("yield" ::: "memory");
#elif defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#else
    asm volatile("" ::: "memory");
#endif
}

static int32_t aooIdForPeer(int32_t peerId) { return peerId * 2; }
static int32_t sinkIdForPeer(int32_t peerId)   { return aooIdForPeer(peerId); }
static int32_t sourceIdForPeer(int32_t peerId) { return aooIdForPeer(peerId); }

// OSC-bundle packing on the emitter.
// tlsPacerBundle is non-null only on the emitting thread, and only inside one listener's drain:
// the hub-pacer thread inside a listener's slot (set/cleared in pacerThread), or with --cut-through
// that listener's own hub-listen thread right after its block barrier (set/cleared in
// listenerProcessThread). Every other caller of hub_endpoint_send_impl sees nullptr and takes the
// one-sendto-per-message path.
static thread_local HubBundleBuilder* tlsPacerBundle = nullptr;

// Per-thread scratch for SonobusHubServer::listenerPumpFeeds (silence, mute-zero and gain-scaled
// input blocks). With --inline-listeners the pumping thread is the audio thread, so
// listenerScratchWarm() sizes these once, to their maximum (blockSize × HUB_MAX_CHANNELS_PER_PEER),
// before the RT loop; the grow-checks in listenerPumpFeeds are then never true. Zero/mute buffers
// are only ever read.
static thread_local std::vector<float>  tlsListenerZeroBuf;
static thread_local std::vector<float*> tlsListenerZeroPtrs;
static thread_local std::vector<float>  tlsListenerMuteBuf;
static thread_local std::vector<float*> tlsListenerMutePtrs;
static thread_local std::vector<float>  tlsListenerGainBuf;
static thread_local std::vector<float*> tlsListenerGainPtrs;
static void listenerScratchWarm(int blockSize) {
    const size_t n = (size_t)std::max(1, blockSize) * HUB_MAX_CHANNELS_PER_PEER;
    if (tlsListenerZeroBuf.size()  < n) tlsListenerZeroBuf.assign(n, 0.0f);
    if (tlsListenerMuteBuf.size()  < n) tlsListenerMuteBuf.assign(n, 0.0f);
    if (tlsListenerGainBuf.size()  < n) tlsListenerGainBuf.resize(n);
    if (tlsListenerZeroPtrs.size() < HUB_MAX_CHANNELS_PER_PEER) tlsListenerZeroPtrs.resize(HUB_MAX_CHANNELS_PER_PEER);
    if (tlsListenerMutePtrs.size() < HUB_MAX_CHANNELS_PER_PEER) tlsListenerMutePtrs.resize(HUB_MAX_CHANNELS_PER_PEER);
    if (tlsListenerGainPtrs.size() < HUB_MAX_CHANNELS_PER_PEER) tlsListenerGainPtrs.resize(HUB_MAX_CHANNELS_PER_PEER);
}

// Hub-wide send-failure counter for the [HUB-PACE] window (send_err=). Error path only; printed and
// reset by statsWriterThread. File-static because the send impls are free functions.
static std::atomic<uint64_t> gHubSendErrors{0};

// The one place every datagram to a peer leaves the hub (bare sends, bundle flushes, AOO replies).
// If the endpoint owns a connect()ed socket, send() on it: no per-datagram route/address lookup in
// the kernel. Otherwise sendto on the shared socket. A failed send() is not retried via sendto
// (same peer, same outcome, and it would double the syscalls of a dead peer).
// RT: one acquire load on the success path (fd and sockaddr come from the same immutable HubDest,
// see HubEndpoint::dest); the error path adds two relaxed RMWs and a relaxed store, with no log,
// lock or allocation, so ECONNREFUSED storms from a vanished peer cost only the failed syscall
// until peer-timeout removes the peer.
static inline int32_t hub_send_bare(HubEndpoint* ep, const char* data, int32_t size) {
    const HubDest* d = ep->dest.load(std::memory_order_acquire);
    ssize_t result = d->connFd >= 0
        ? ::send(d->connFd, data, (size_t)size, 0)
        : ::sendto(ep->socketFd, data, (size_t)size, 0,
                   (const struct sockaddr*)&d->addr, d->addrlen);
    if (result > 0) {
        ep->sentBytes += result;
    } else if (result < 0) {
        ep->lastErrno.store(errno, std::memory_order_relaxed);
        ep->sendErrors.fetch_add(1, std::memory_order_relaxed);
        gHubSendErrors.fetch_add(1, std::memory_order_relaxed);
    }
    return (int32_t)result;
}

// Emit whatever the builder holds: 0 msgs -> nothing; 1 msg -> bare; ≥2 -> one "#bundle" datagram.
// Emitter thread only (pacer under --send-pace, the owning listener thread under --cut-through).
static void hub_bundle_flush(HubBundleBuilder* b) {
    if (b->count == 0) return;
    if (b->count == 1) {
        // single message: strip bundle header + its size prefix, send as-is
        const int off = HubBundleBuilder::kHeader + 4;
        hub_send_bare(b->ep, b->buf + off, b->used - off);
    } else {
        hub_send_bare(b->ep, b->buf, b->used);
        b->statBundles++;
        b->statMsgsInBundles += (uint64_t)b->count;
    }
    b->statDatagrams++;
    b->used = HubBundleBuilder::kHeader;
    b->count = 0;
}

// Stage one message. Never splits: if it cannot fit after a flush (larger
// than cap - header - 4, e.g. an oversized cap-vs-packetsize config), it
// goes out bare after flushing what was pending, so ordering is preserved.
static inline int32_t hub_bundle_append(HubBundleBuilder* b, const char* data, int32_t size) {
    b->statMsgs++;
    if (size + 4 > b->cap - HubBundleBuilder::kHeader) {
        hub_bundle_flush(b);
        b->statDatagrams++;
        return hub_send_bare(b->ep, data, size);
    }
    if (b->used + 4 + size > b->cap) {
        hub_bundle_flush(b);   // full: emit, then this message starts the next bundle
    }
    const uint32_t be = htonl((uint32_t)size);
    memcpy(b->buf + b->used, &be, 4);
    memcpy(b->buf + b->used + 4, data, (size_t)size);
    b->used += 4 + size;
    b->count++;
    return size;   // aoo::endpoint::send ignores the return; report "sent"
}

// Destination check for the packing fast path: the builder belongs to ONE
// listener and we pack only datagrams addressed to that listener. AOO hands
// us the sink's endpoint pointer (the listener's persistentEndpoint, whose
// sockaddr is the listener's address), so pointer identity is the fast
// answer; a different HubEndpoint object carrying the same sockaddr (the
// stack endpoints used on control paths) is compared byte-wise as a fallback.
static inline bool hub_bundle_same_dest(const HubBundleBuilder* b, const HubEndpoint* ep) {
    if (b->ep == ep) return true;
    const HubDest* bd = b->ep->dest.load(std::memory_order_acquire);
    const HubDest* ed = ep->dest.load(std::memory_order_acquire);
    return b->ep->socketFd == ep->socketFd
        && bd->addrlen == ed->addrlen
        && memcmp(&bd->addr, &ed->addr, (size_t)ed->addrlen) == 0;
}

static int32_t hub_endpoint_send_impl(void* user, const char* data, int32_t size) {
    auto* ep = static_cast<HubEndpoint*>(user);
    if (ep->socketFd < 0) return -1;
    // Inside a pacer slot, same destination: pack instead of sendto. Anything else (other destination,
    // other thread) is sent directly.
    if (HubBundleBuilder* b = tlsPacerBundle; b != nullptr && size > 0 && hub_bundle_same_dest(b, ep)) {
        return hub_bundle_append(b, data, size);
    }
    return hub_send_bare(ep, data, size);
}

static int32_t hub_client_send_impl(void* user, const char* data, int32_t size, void* raddr) {
    auto* ep = static_cast<HubEndpoint*>(user);
    if (ep->socketFd < 0) return -1;
    const struct sockaddr* addr = (const struct sockaddr*)raddr;
    socklen_t len = (addr->sa_family == AF_INET6) ? sizeof(struct sockaddr_in6) : sizeof(struct sockaddr_in);
    ssize_t result = ::sendto(ep->socketFd, data, (size_t)size, 0, addr, len);
    if (result > 0) ep->sentBytes += result;
    return (int32_t)result;
}

SonobusHubServer::SonobusHubServer() {
    // The pacer's raw handle starts on the
    // initial (empty, gen 0) snapshot owned by mPeerSnapshot.
    mPacerSnapPtr.store(mPeerSnapshot.get(), std::memory_order_release);
}

SonobusHubServer::~SonobusHubServer() {
    // A destructor is noexcept — anything escaping here is
    // std::terminate → SIGABRT. stop() is a complete, idempotent teardown
    // (never-started / partially-started included, see there); the catch is a
    // last-line guard so a failed start() can never turn into a crash report.
    try {
        stop();
    } catch (const std::exception& e) {
        std::cerr << "[HUB] teardown exception ignored: " << e.what() << "\n";
    } catch (...) {
        std::cerr << "[HUB] teardown exception ignored (non-std)\n";
    }
}

// Advertised-address resolution.
// A launch without a script points the hub's own AOO client at its built-in server over loopback
// (resolveServerEndpoint -> 127.0.0.1). The AOO library then derives both addresses it advertises
// in LOGIN from that loopback path: local = getsockname() of the TCP socket (client.cpp try_connect)
// = 127.0.0.1, public = the server's UDP reply of the observed source (server.cpp handle_udp_message)
// = 127.0.0.1:11000. The server's LAN classifier (server.cpp on_user_joined_group) then fails
// is_lan_address(local) and every player, LAN or WAN, is told to reach the hub at 127.0.0.1.
// So the hub derives both from what the machine knows: the LAN address from the interface inside
// the configured LAN subnet, the WAN address from one STUN binding request. Control path only
// (start(), before the signaling server exists); nothing on the audio thread.
#include <ifaddrs.h>
#include <net/if.h>    // IFF_UP / IFF_LOOPBACK
#include <random>

namespace {

bool hub_host_is_loopback(const std::string& host) {
    return host == "localhost" || host == "::1" || host.rfind("127.", 0) == 0;
}

// IPv4 of the interface to advertise on the LAN. Prefers an UP, non-loopback
// interface whose address falls inside `inLan` (the configured --lan-subnet);
// otherwise the interface the kernel would route a public address through
// (UDP connect() sends nothing). Empty when the machine has no usable IPv4.
template <typename InLan>
std::string hub_pick_lan_ipv4(InLan inLan, std::string& ifname, bool& matchedSubnet) {
    ifname.clear();
    matchedSubnet = false;
    struct ifaddrs* list = nullptr;
    if (getifaddrs(&list) == 0) {
        std::string pick;
        for (auto* ifa = list; ifa; ifa = ifa->ifa_next) {
            if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;
            if ((ifa->ifa_flags & IFF_UP) == 0 || (ifa->ifa_flags & IFF_LOOPBACK)) continue;
            struct sockaddr_storage ss {};
            memcpy(&ss, ifa->ifa_addr, sizeof(struct sockaddr_in));
            if (!inLan(ss)) continue;
            char buf[INET_ADDRSTRLEN] = {0};
            inet_ntop(AF_INET, &reinterpret_cast<const struct sockaddr_in*>(ifa->ifa_addr)->sin_addr, buf, sizeof(buf));
            pick = buf;
            ifname = ifa->ifa_name ? ifa->ifa_name : "";
            matchedSubnet = true;
            break;
        }
        freeifaddrs(list);
        if (!pick.empty()) return pick;
    }
    // No subnet match: the default-route interface.
    int s = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return {};
    struct sockaddr_in probe {};
    probe.sin_family = AF_INET;
    probe.sin_port = htons(53);
    inet_pton(AF_INET, "8.8.8.8", &probe.sin_addr);
    std::string out;
    if (::connect(s, (struct sockaddr*)&probe, sizeof(probe)) == 0) {
        struct sockaddr_in me {};
        socklen_t len = sizeof(me);
        if (::getsockname(s, (struct sockaddr*)&me, &len) == 0) {
            char buf[INET_ADDRSTRLEN] = {0};
            inet_ntop(AF_INET, &me.sin_addr, buf, sizeof(buf));
            out = buf;
            ifname = "default route";
        }
    }
    ::close(s);
    return out;
}

// Derive the LAN CIDR from the default-route interface (its address + netmask).
// Empty when nothing usable is found (everyone is then WAN).
std::string hub_default_route_cidr(std::string& ifname) {
    ifname.clear();
    int s = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return {};
    struct sockaddr_in probe {};
    probe.sin_family = AF_INET;
    probe.sin_port = htons(53);
    inet_pton(AF_INET, "8.8.8.8", &probe.sin_addr);
    struct sockaddr_in me {};
    socklen_t len = sizeof(me);
    bool ok = (::connect(s, (struct sockaddr*)&probe, sizeof(probe)) == 0)
           && (::getsockname(s, (struct sockaddr*)&me, &len) == 0);
    ::close(s);
    if (!ok) return {};
    std::string out;
    struct ifaddrs* list = nullptr;
    if (getifaddrs(&list) != 0) return {};
    for (auto* ifa = list; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || !ifa->ifa_netmask || ifa->ifa_addr->sa_family != AF_INET) continue;
        if (ifa->ifa_flags & IFF_LOOPBACK) continue;   // Never derive 127.0.0.0/8 from lo0 (a login launch before the network is up)
        const auto* a = reinterpret_cast<const struct sockaddr_in*>(ifa->ifa_addr);
        if (a->sin_addr.s_addr != me.sin_addr.s_addr) continue;
        const uint32_t mask = ntohl(reinterpret_cast<const struct sockaddr_in*>(ifa->ifa_netmask)->sin_addr.s_addr);
        int prefix = 0; for (uint32_t m = mask; m & 0x80000000u; m <<= 1) ++prefix;
        if (prefix <= 0 || prefix > 30) continue;   // /31, /32 or no mask: not a LAN
        struct in_addr net {}; net.s_addr = htonl(ntohl(a->sin_addr.s_addr) & mask);
        char buf[INET_ADDRSTRLEN] = {0};
        inet_ntop(AF_INET, &net, buf, sizeof(buf));
        out = std::string(buf) + "/" + std::to_string(prefix);
        ifname = ifa->ifa_name ? ifa->ifa_name : "";
        break;
    }
    freeifaddrs(list);
    return out;
}

// One RFC 5389 STUN Binding request from a throw-away UDP socket; returns the
// reflexive (public) IPv4 in `ip`. Bounded by timeoutMs per attempt.
bool hub_stun_public_ipv4(const char* host, int port, int timeoutMs, std::string& ip, std::string& err) {
    ip.clear(); err.clear();
    struct addrinfo hints {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    struct addrinfo* res = nullptr;
    const std::string portStr = std::to_string(port);
    if (int rc = getaddrinfo(host, portStr.c_str(), &hints, &res); rc != 0 || !res) {
        err = std::string("resolve: ") + (rc != 0 ? gai_strerror(rc) : "no address");
        return false;
    }
    int s = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) { freeaddrinfo(res); err = std::string("socket: ") + strerror(errno); return false; }

    uint8_t req[20] = {0};
    req[0] = 0x00; req[1] = 0x01;                 // Binding Request
    req[2] = 0x00; req[3] = 0x00;                 // length 0
    req[4] = 0x21; req[5] = 0x12; req[6] = 0xA4; req[7] = 0x42;   // magic cookie
    std::random_device rd;
    for (int i = 8; i < 20; ++i) req[i] = (uint8_t)rd();

    bool ok = false;
    if (::sendto(s, req, sizeof(req), 0, res->ai_addr, res->ai_addrlen) == (ssize_t)sizeof(req)) {
        struct pollfd pfd { s, POLLIN, 0 };
        if (::poll(&pfd, 1, timeoutMs) > 0 && (pfd.revents & POLLIN)) {
            uint8_t rsp[576];
            ssize_t n = ::recv(s, rsp, sizeof(rsp), 0);
            if (n >= 20 && rsp[0] == 0x01 && rsp[1] == 0x01 && memcmp(rsp + 4, req + 4, 16) == 0) {
                const size_t mlen = std::min<size_t>((size_t)n, 20 + ((size_t)rsp[2] << 8 | rsp[3]));
                std::string plain;
                for (size_t off = 20; off + 4 <= mlen;) {
                    const uint16_t type = (uint16_t)(rsp[off] << 8 | rsp[off + 1]);
                    const uint16_t alen = (uint16_t)(rsp[off + 2] << 8 | rsp[off + 3]);
                    const uint8_t* v = rsp + off + 4;
                    if (off + 4 + alen > mlen) break;
                    if ((type == 0x0020 || type == 0x0001) && alen >= 8 && v[1] == 0x01) {   // (XOR-)MAPPED-ADDRESS, IPv4
                        uint8_t a[4] = { v[4], v[5], v[6], v[7] };
                        if (type == 0x0020) for (int i = 0; i < 4; ++i) a[i] ^= req[4 + i];
                        char buf[INET_ADDRSTRLEN] = {0};
                        inet_ntop(AF_INET, a, buf, sizeof(buf));
                        if (type == 0x0020) { ip = buf; ok = true; break; }
                        plain = buf;
                    }
                    off += 4 + ((alen + 3u) & ~3u);
                }
                if (!ok && !plain.empty()) { ip = plain; ok = true; }
                if (!ok) err = "reply without a mapped address";
            } else {
                err = n < 0 ? std::string("recv: ") + strerror(errno) : "unexpected reply";
            }
        } else {
            err = "timeout";
        }
    } else {
        err = std::string("send: ") + strerror(errno);
    }
    ::close(s);
    freeaddrinfo(res);
    return ok;
}

} // namespace

// Fill mAdvertisedLanHost / mAdvertisedPublicHost(+Port) for this start().
// Only acts when the hub's own client reaches the signaling server over loopback; a script launch
// that dials the public IP (or an external server) keeps the library's NAT-observed values.
void SonobusHubServer::resolveAdvertisedAddresses(const std::string& aooServerHost) {
    mAdvertisedLanHost.clear();
    mAdvertisedPublicHost.clear();
    mAdvertisedPublicPort = 0;
    if (!hub_host_is_loopback(aooServerHost)) {
        std::cout << "[HUB] Advertised addresses: signaling via " << aooServerHost
                  << " — using the server's observed public address and this connection's local address (no override)\n";
        return;
    }
    std::string ifname;
    bool matched = false;
    mAdvertisedLanHost = hub_pick_lan_ipv4(
        [this](const struct sockaddr_storage& ss) { return addrMatchesLanSubnet(ss); },   // false when no --lan-subnet -> default-route interface
        ifname, matched);
    if (mAdvertisedLanHost.empty()) {
        std::cerr << "[HUB] WARNING: no IPv4 interface found for the LAN address — same-LAN players will be told 127.0.0.1\n";
    } else {
        std::cout << "[HUB] Advertised LAN address: " << mAdvertisedLanHost << ":" << mLocalPort
                  << " (" << ifname << (matched && !mLanSubnetCidr.empty() ? ", in lan subnet " + mLanSubnetCidr : "") << ")\n";
    }
    if (!mPublicAddressHost.empty()) {
        std::cout << "[HUB] Advertised public address: " << mPublicAddressHost << " (settings: public_address)\n";
        return;   // explicit setting wins; applied via set_self_public_address below
    }
    static const struct { const char* host; int port; } kStun[] = {
        { "stun.l.google.com", 19302 }, { "stun.cloudflare.com", 3478 },
    };
    for (auto& srv : kStun) {
        std::string ip, err;
        if (hub_stun_public_ipv4(srv.host, srv.port, 800, ip, err)) {
            mAdvertisedPublicHost = ip;
            mAdvertisedPublicPort = mLocalPort;   // the router's forward of the hub UDP port (same port as LAN)
            std::cout << "[HUB] Advertised public address: " << ip << ":" << mLocalPort
                      << " (STUN " << srv.host << ")\n";
            return;
        }
        std::cerr << "[HUB] STUN " << srv.host << ":" << srv.port << " failed: " << err << "\n";
    }
    std::cerr << "[HUB] WARNING: public address unknown — WAN players will be told 127.0.0.1. "
                 "Set 'Public address' in Settings (public_address) or check the internet connection.\n";
}

// Port check.
// Does the router forward this hub's ports? A hub cannot test that from inside its own network, so it asks
// a hub outside (the helper, default the original project hub) to reach back. The request goes out from a
// throw-away UDP socket, so no NAT mapping exists on the real ports; the helper then
//   - sends /sb/fwprobe/ping from fresh sockets to <our public IP>:udp_port and :server_port (UDP),
//   - tries a TCP connect to <our public IP>:server_port,
//   - answers /sb/fwprobe/result (TCP outcome) from its hub socket to the request's source.
// We count the pings that arrive on the hub socket (recv thread) and on the signaling server's UDP socket
// (aoo server hook). Control path only. Runs at start; re-runs every 5 minutes until every port passes.
extern "C" uint32_t aoo_sb_fwprobe_server_hits();
extern "C" void aoo_sb_set_group_policy(const char* name, const char* pwd);   

static bool hub_resolve_ipv4(const std::string& host, struct in_addr& out) {
    if (inet_pton(AF_INET, host.c_str(), &out) == 1) return true;
    struct addrinfo hints {}; hints.ai_family = AF_INET; hints.ai_socktype = SOCK_DGRAM;
    struct addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res) return false;
    out = ((struct sockaddr_in*)res->ai_addr)->sin_addr;
    freeaddrinfo(res);
    return true;
}

bool SonobusHubServer::portCheckViaHelper(std::string& text, int& state, bool& answered) {
    // 'answered' = the checker gave a verdict (pass/fail/self); false = unreachable / no reply,
    // so the caller can fall back to a connected WAN player instead of showing the no-answer text.
    answered = false;
    const auto colon = mPortCheckHelper.rfind(':');
    const std::string hHost = colon == std::string::npos ? mPortCheckHelper : mPortCheckHelper.substr(0, colon);
    const int hPort = colon == std::string::npos ? 11000 : std::atoi(mPortCheckHelper.c_str() + colon + 1);
    struct in_addr helperIp {};
    if (hHost.empty() || hPort <= 0 || !hub_resolve_ipv4(hHost, helperIp)) {
        text = "Port check: the port checker could not be resolved"; state = 2; return false;   // Never show the checker's address
    }
    // This hub IS the checker (same public IP): a self-probe would only test the router's hairpin.
    const std::string ourPublic = !mPublicAddressHost.empty() ? mPublicAddressHost : mAdvertisedPublicHost;
    struct in_addr ourIp {};
    if (!ourPublic.empty() && hub_resolve_ipv4(ourPublic, ourIp) && ourIp.s_addr == helperIp.s_addr) {
        text = "Port check: this hub is the port checker for others"; state = 3; answered = true; return true;   
    }
    std::random_device rd;
    const int64_t token = ((int64_t)rd() << 31) ^ (int64_t)rd() ^ 0x5b0b05;
    mPortCheckToken.store(token, std::memory_order_relaxed);
    const uint32_t a0 = mPortProbeAudioHits.load(std::memory_order_relaxed);
    const uint32_t s0 = aoo_sb_fwprobe_server_hits();

    int sock = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) { text = "Port check: no socket"; state = 2; return false; }
    struct timeval tv { 0, 250000 }; setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    struct sockaddr_in hAddr {}; hAddr.sin_family = AF_INET; hAddr.sin_port = htons((uint16_t)hPort); hAddr.sin_addr = helperIp;
    char obuf[256];
    osc::OutboundPacketStream req(obuf, sizeof(obuf));
    req << osc::BeginMessage("/sb/fwprobe/req") << (osc::int64)token << (int32_t)mLocalPort << (int32_t)mLocalServerPort << osc::EndMessage;
    int tcpOk = -1;
    for (int attempt = 0; attempt < 3 && tcpOk < 0 && mRunning.load(); ++attempt) {
        ::sendto(sock, req.Data(), req.Size(), 0, (struct sockaddr*)&hAddr, sizeof(hAddr));
        for (int w = 0; w < 16 && tcpOk < 0 && mRunning.load(); ++w) {   // up to 4 s per attempt (the helper's TCP try is <= 2 s)
            char rbuf[512];
            ssize_t rn = ::recv(sock, rbuf, sizeof(rbuf), 0);
            if (rn <= 0) continue;
            try {
                osc::ReceivedPacket pk(rbuf, (osc::osc_bundle_element_size_t)rn);
                if (!pk.IsMessage()) continue;
                osc::ReceivedMessage m(pk);
                if (std::strcmp(m.AddressPattern(), "/sb/fwprobe/result") != 0) continue;
                auto it = m.ArgumentsBegin();
                if (it->AsInt64() != token) continue;
                ++it; tcpOk = it->AsInt32();
            } catch (...) {}
        }
    }
    ::close(sock);
    if (mRunning.load()) std::this_thread::sleep_for(std::chrono::milliseconds(400));   // late pings
    const bool udpAudio = mPortProbeAudioHits.load(std::memory_order_relaxed) != a0;
    const bool udpServer = aoo_sb_fwprobe_server_hits() != s0;
    if (tcpOk < 0) {
        text = "Port check: no answer from the port checker - try again later";   // Never show the checker's address
        state = 2; return false;
    }
    answered = true;   // A verdict arrived (pass or fail)
    auto mark = [](bool ok) { return std::string(ok ? "OK" : "BLOCKED"); };
    text = "Port check: TCP " + std::to_string(mLocalServerPort) + " " + mark(tcpOk == 1)
         + "   UDP " + std::to_string(mLocalServerPort) + " " + mark(udpServer)
         + "   UDP " + std::to_string(mLocalPort) + " " + mark(udpAudio);
    // server_port (TCP+UDP) is required for outside players. udp_port is optional: players usually get in by
    // hole punching, but forwarding it helps players behind strict routers.
    const bool required = tcpOk == 1 && udpServer;
    const std::string lan = mAdvertisedLanHost.empty() ? std::string("this Mac") : mAdvertisedLanHost;
    if (!required) text += "   - open TCP+UDP " + std::to_string(mLocalServerPort) + " in your router, forwarded to " + lan + ", or in your cloud firewall";
    else if (!udpAudio) text += "   (UDP " + std::to_string(mLocalPort) + " optional: open it the same way for players behind strict routers)";
    state = required ? 1 : 2;
    return required;   // stop once the required ports pass (re-check every 5 min only while they fail)
}

// ─── port check without the single-hub dependency ──────────────────────────────────────────────
// LOCAL tier: are the sockets bound and does the HOST firewall let the ports in? (ufw / iptables on Linux, the
// application firewall on macOS). This cannot see the router, so it is only half the answer.
static std::string hub_run_cmd(const char* cmd) {
    std::string out;
    if (FILE* f = ::popen(cmd, "r")) { char b[512]; while (fgets(b, sizeof(b), f)) out += b; ::pclose(f); }
    return out;
}
std::string SonobusHubServer::localPortCheckText() const {
    const int sp = mLocalServerPort, up = mLocalPort;
    std::string t = "Local: listening on TCP+UDP " + std::to_string(sp) + " and UDP " + std::to_string(up) + "; firewall: ";
#if defined(__APPLE__)
    const std::string fw = hub_run_cmd("/usr/libexec/ApplicationFirewall/socketfilterfw --getglobalstate 2>/dev/null");
    if (fw.find("disabled") != std::string::npos) t += "macOS firewall off";
    else if (fw.find("enabled") != std::string::npos) t += "macOS firewall ON (allow the hub when macOS asks)";
    else t += "not inspected";
#else
    const std::string ufw = hub_run_cmd("ufw status 2>/dev/null");
    auto ufwAllows = [&](int port, const char* proto) {
        std::istringstream in(ufw); std::string line;
        const std::string p1 = std::to_string(port) + "/" + proto, p2 = std::to_string(port) + " ";
        while (std::getline(in, line))
            if (line.find("ALLOW") != std::string::npos && (line.rfind(p1, 0) == 0 || line.rfind(p2, 0) == 0)) return true;
        return false;
    };
    if (ufw.find("Status: active") != std::string::npos) {
        std::string blocked;
        if (!ufwAllows(sp, "tcp")) blocked += " " + std::to_string(sp) + "/tcp";
        if (!ufwAllows(sp, "udp")) blocked += " " + std::to_string(sp) + "/udp";
        if (!ufwAllows(up, "udp")) blocked += " " + std::to_string(up) + "/udp";
        t += blocked.empty() ? "ufw allows all three" : "ufw BLOCKS" + blocked + " (ufw allow <port>/<proto>)";
    } else if (ufw.find("Status: inactive") != std::string::npos) {
        t += "ufw inactive (open)";
    } else {
        const std::string ipt = hub_run_cmd("iptables -S INPUT 2>/dev/null");
        if (ipt.empty()) t += "none found (open)";
        else if (ipt.find("-P INPUT DROP") == std::string::npos && ipt.find("-j DROP") == std::string::npos && ipt.find("-j REJECT") == std::string::npos) t += "iptables accepts (open)";
        else {
            std::string blocked;
            auto allows = [&](int port, const char* proto) {
                return ipt.find(std::string("-p ") + proto) != std::string::npos && ipt.find("--dport " + std::to_string(port)) != std::string::npos && ipt.find("-j ACCEPT") != std::string::npos;
            };
            if (!allows(sp, "tcp")) blocked += " " + std::to_string(sp) + "/tcp";
            if (!allows(sp, "udp")) blocked += " " + std::to_string(sp) + "/udp";
            if (!allows(up, "udp")) blocked += " " + std::to_string(up) + "/udp";
            t += blocked.empty() ? "iptables allows all three" : "iptables may BLOCK" + blocked;
        }
    }
#endif
    return t;
}

// EXTERNAL tier, player-assisted: ask one connected WAN player to probe us from outside (fresh UDP sockets to our
// udp_port and server_port, a TCP connect to server_port) and answer /sb/fwprobe/result to the hub socket. The
// request goes out on the hub socket (the player's NAT only lets that endpoint back in). Clients that do not
// implement this never answer: after 5 s the caller tries the next player.
bool SonobusHubServer::portCheckViaPlayer(const std::string& user, const std::string& ip, int port, std::string& text, int& state, bool& answered) {
    answered = false;
    struct sockaddr_in to {}; to.sin_family = AF_INET; to.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, ip.c_str(), &to.sin_addr) != 1 || mSocket < 0) return false;
    std::random_device rd;
    const int64_t token = ((int64_t)rd() << 31) ^ (int64_t)rd() ^ 0x5b0b05;
    mPortProbeResultTcp.store(-1, std::memory_order_relaxed);
    mPortProbeResultToken.store(0, std::memory_order_relaxed);
    mPortCheckToken.store(token, std::memory_order_relaxed);
    const uint32_t a0 = mPortProbeAudioHits.load(std::memory_order_relaxed);
    const uint32_t s0 = aoo_sb_fwprobe_server_hits();
    char obuf[256];
    osc::OutboundPacketStream req(obuf, sizeof(obuf));
    req << osc::BeginMessage("/sb/fwprobe/req") << (osc::int64)token << (int32_t)mLocalPort << (int32_t)mLocalServerPort << osc::EndMessage;
    for (int attempt = 0; attempt < 2 && mRunning.load(); ++attempt) {
        ::sendto(mSocket, req.Data(), req.Size(), 0, (const struct sockaddr*)&to, sizeof(to));
        for (int w = 0; w < 25 && mRunning.load(); ++w) {   // 2.5 s per attempt (the player's TCP try is <= 2 s)
            if (mPortProbeResultToken.load(std::memory_order_acquire) == token) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (mPortProbeResultToken.load(std::memory_order_acquire) == token) break;
    }
    if (mPortProbeResultToken.load(std::memory_order_acquire) != token) return false;
    answered = true;
    if (mRunning.load()) std::this_thread::sleep_for(std::chrono::milliseconds(400));   // late pings
    const int tcpOk = mPortProbeResultTcp.load(std::memory_order_relaxed);
    const bool udpAudio = mPortProbeAudioHits.load(std::memory_order_relaxed) != a0;
    const bool udpServer = aoo_sb_fwprobe_server_hits() != s0;
    auto mark = [](bool ok) { return std::string(ok ? "OK" : "BLOCKED"); };
    // The time is published as epoch ms (portCheckVerifiedAtMs) and rendered in the VIEWER's local
    // time (browser / Mac GUI) — the cloud hub's clock is UTC.
    { std::lock_guard<std::mutex> lk(mPortCheckMutex);
      mPortCheckVerifiedAtMs = (int64_t) std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count(); }
    text = "Port check: TCP " + std::to_string(mLocalServerPort) + " " + mark(tcpOk == 1)
         + "   UDP " + std::to_string(mLocalServerPort) + " " + mark(udpServer)
         + "   UDP " + std::to_string(mLocalPort) + " " + mark(udpAudio)
         + "   (verified from " + user + ")";
    const bool required = tcpOk == 1 && udpServer;
    const std::string lan = mAdvertisedLanHost.empty() ? std::string("this machine") : mAdvertisedLanHost;
    if (!required) text += "   - open TCP+UDP " + std::to_string(mLocalServerPort) + " in your router, forwarded to " + lan + ", or in your cloud firewall";
    else if (!udpAudio) text += "   (UDP " + std::to_string(mLocalPort) + " optional: open it the same way for players behind strict routers)";
    state = required ? 1 : 2;
    return required;
}

bool SonobusHubServer::portCheckOnce(std::string& text, int& state) {
    { const std::string l = localPortCheckText(); std::lock_guard<std::mutex> lk(mPortCheckMutex); mPortLocalText = l; }
    // The configured outside checker is asked first (it is always there and answers in seconds); a connected WAN
    // player is the fallback when the checker cannot be reached. Blank helper = player-only path. Players that do
    // not answer fall back to the checker when one exists.
    const bool haveHelper = !mPortCheckHelper.empty();
    std::string helperText; int helperState = 0; bool helperAnswered = false; bool helperOk = false;
    if (haveHelper) {
        helperOk = portCheckViaHelper(helperText, helperState, helperAnswered);
        if (helperAnswered) { text = helperText; state = helperState; return helperOk; }
        if (!mRunning.load()) return false;
    }
    const UiSnapshot snap = snapshotForUI();
    int wanPeers = 0;
    for (const auto& p : snap.peers) {
        if (p.isLan || p.userName == mHubUserName || p.ipAddr.empty() || p.port <= 0) continue;
        ++wanPeers;
        bool answered = false;
        const bool ok = portCheckViaPlayer(p.userName, p.ipAddr, p.port, text, state, answered);
        if (answered) return ok;
        if (!mRunning.load()) return false;
    }
    if (haveHelper) { text = helperText; state = helperState; return helperOk; }   // checker unreachable AND no player verdict: show the checker's no-answer text
    text = wanPeers == 0 ? "Port check: waiting for a player outside this network to connect (it will verify the ports)"
                         : "Port check: the connected players did not answer the probe (older client?) - waiting";
    state = 0;
    return false;
}

void SonobusHubServer::portCheckThreadMain() {
    for (int i = 0; i < 30 && mRunning.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));   // let the sockets settle
    while (mRunning.load()) {
        { std::lock_guard<std::mutex> lk(mPortCheckMutex); mPortCheckText = "Port check: running..."; mPortCheckState = 0; mPortCheckVerifiedAtMs = 0; }
        std::string text; int state = 0;
        const bool done = portCheckOnce(text, state);
        { std::lock_guard<std::mutex> lk(mPortCheckMutex); mPortCheckText = text; mPortCheckState = state; }
        std::cout << "[HUB-PORTCHECK] " << text << "\n";
        // Stay alive — once the required ports pass we only re-run on request (web page button /
        // GUI); while they fail, every 5 min or on request.
        for (int i = 0; mRunning.load() && !mPortCheckRequested.exchange(false, std::memory_order_relaxed) && (done || i < 300); ++i)
            std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

// Helper side: another hub asks us to probe it back. One shared, rate-limited routine (one probe every 2 s,
// so we are no amplifier); no hub state is touched, so it is safe from the signaling server's thread too.
// replyFd is a dup() of the socket the request arrived on (the requester's NAT only lets that source back in).
static std::atomic<uint64_t> g_hubProbeLastServeMs{0};
static void hub_serve_port_probe(const struct sockaddr_in to, int64_t token, int udpPort, int serverPort, int replyFd) {
    if (udpPort <= 0 || udpPort > 65535 || serverPort <= 0 || serverPort > 65535) { ::close(replyFd); return; }
    const uint64_t now = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    uint64_t last = g_hubProbeLastServeMs.load(std::memory_order_relaxed);
    if (now - last < 2000 || !g_hubProbeLastServeMs.compare_exchange_strong(last, now)) { ::close(replyFd); return; }
    std::thread([to, token, udpPort, serverPort, replyFd]() {
        auto ping = [&](int port, const char* what) {
            int s = ::socket(AF_INET, SOCK_DGRAM, 0);
            if (s < 0) return;
            struct sockaddr_in d = to; d.sin_port = htons((uint16_t)port);
            char b[128]; osc::OutboundPacketStream m(b, sizeof(b));
            m << osc::BeginMessage("/sb/fwprobe/ping") << (osc::int64)token << what << osc::EndMessage;
            for (int i = 0; i < 3; ++i) { ::sendto(s, m.Data(), m.Size(), 0, (struct sockaddr*)&d, sizeof(d)); std::this_thread::sleep_for(std::chrono::milliseconds(20)); }
            ::close(s);
        };
        ping(udpPort, "udp-audio");
        ping(serverPort, "udp-server");
        int tcpOk = 0;
        int t = ::socket(AF_INET, SOCK_STREAM, 0);
        if (t >= 0) {
            fcntl(t, F_SETFL, fcntl(t, F_GETFL, 0) | O_NONBLOCK);
            struct sockaddr_in d = to; d.sin_port = htons((uint16_t)serverPort);
            int r = ::connect(t, (struct sockaddr*)&d, sizeof(d));
            if (r == 0) tcpOk = 1;
            else if (errno == EINPROGRESS) {
                struct pollfd pf { t, POLLOUT, 0 };
                if (::poll(&pf, 1, 2000) == 1) { int err = 0; socklen_t el = sizeof(err); getsockopt(t, SOL_SOCKET, SO_ERROR, &err, &el); tcpOk = err == 0 ? 1 : 0; }
            }
            ::close(t);
        }
        char b[128]; osc::OutboundPacketStream m(b, sizeof(b));
        m << osc::BeginMessage("/sb/fwprobe/result") << (osc::int64)token << (int32_t)tcpOk << osc::EndMessage;
        for (int i = 0; i < 2; ++i) ::sendto(replyFd, m.Data(), m.Size(), 0, (const struct sockaddr*)&to, sizeof(to));
        ::close(replyFd);
        char ip[INET_ADDRSTRLEN] = {}; inet_ntop(AF_INET, &to.sin_addr, ip, sizeof(ip));
        std::cout << "[HUB-PORTCHECK] checked " << ip << " for another hub (TCP " << serverPort << (tcpOk ? " OK" : " BLOCKED") << ")\n";
    }).detach();
}

// Entry 1: request on the hub's audio UDP socket (works on a LAN, or where udp_port is forwarded).
void SonobusHubServer::handlePortProbeRequest(const struct sockaddr_storage* src, int64_t token, int udpPort, int serverPort) {
    if (src->ss_family != AF_INET) return;
    const int replyFd = ::dup(mSocket);   // own fd: safe even if the hub stops meanwhile
    if (replyFd < 0) return;
    hub_serve_port_probe(*(const struct sockaddr_in*)src, token, udpPort, serverPort, replyFd);
}

// Entry 2: request on the signaling server's UDP port (server_port is forwarded on every working hub; some routers forward only the signaling port, the hub's audio port is reached by hole punching). Called on the server thread.
extern "C" void aoo_sb_set_fwprobe_req_handler(void (*fn)(const char*, int, const struct sockaddr*, int, int));
extern "C" void aoo_sb_set_join_gate(int (*fn)(const char*, const char*, int, char*, int));
extern "C" void aoo_sb_set_stage_max_players(int n);
extern "C" void aoo_sb_kick_user(const char* name);

// The built-in server asks the hub before admitting a new member (stage full, recently removed).
static std::atomic<SonobusHubServer*> g_hubForJoinGate{nullptr};
static int hub_join_gate(const char* ip, const char* user, int others, char* why, int whylen) {
    SonobusHubServer* hub = g_hubForJoinGate.load(std::memory_order_acquire);
    if (!hub) return 1;
    const std::string r = hub->stageJoinRefusal(ip ? ip : "", user ? user : "", others);
    if (r.empty()) return 1;
    std::snprintf(why, (size_t)whylen, "%s", r.c_str());
    return 0;
}
static std::string hub_plain_ip(std::string ip) {   // "::ffff:1.2.3.4" and "1.2.3.4" are the same player
    if (ip.rfind("::ffff:", 0) == 0) ip = ip.substr(7);
    return ip;
}
// Connection/security log (see SonobusHubServer::connLog)
extern "C" void aoo_sb_set_conn_event_handler(void (*fn)(const char*, const char*, const char*, const char*, const char*));
static void hub_conn_event_from_server(const char* kind, const char* addr, const char* user, const char* group, const char* detail) {
    SonobusHubServer::connLog(kind, addr ? addr : "", user ? user : "", group ? group : "", detail ? detail : "",
                              std::strcmp(kind, "bad-message") == 0 || std::strcmp(kind, "tcp-connect") == 0);
}
static std::string hubSockaddrInToString(const struct sockaddr_in* a) {   // "192.0.2.11:5678" for the connection log
    char ip[INET_ADDRSTRLEN] = {}; if (a) inet_ntop(AF_INET, &a->sin_addr, ip, sizeof(ip));
    return std::string(ip) + ":" + std::to_string(a ? ntohs(a->sin_port) : 0);
}
namespace {
std::mutex g_connLogMutex;
std::deque<std::string> g_connLogRing;
std::map<std::string, long long> g_connLogLast;   // (kind|addr) -> last emit ms, for rate limiting
}
void SonobusHubServer::connLog(const char* kind, const std::string& addr, const std::string& user, const std::string& group, const std::string& detail, bool rateLimited) {
    const auto now = std::chrono::system_clock::now();
    const long long nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    std::string line;
    {
        std::lock_guard<std::mutex> g(g_connLogMutex);
        if (rateLimited) {
            const std::string key = std::string(kind) + "|" + addr;
            auto it = g_connLogLast.find(key);
            if (it != g_connLogLast.end() && nowMs - it->second < 10000) return;
            g_connLogLast[key] = nowMs;
            if (g_connLogLast.size() > 4096) g_connLogLast.clear();
        }
        const std::time_t t = std::chrono::system_clock::to_time_t(now);
        struct tm lt; localtime_r(&t, &lt);
        char stamp[32]; strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &lt);
        line = std::string("[HUB-CONN] ") + stamp + " " + kind + " addr=" + addr;
        if (!user.empty()) line += " user='" + user + "'";
        if (!group.empty()) line += " group='" + group + "'";
        if (!detail.empty()) line += " " + detail;
        g_connLogRing.push_back(line);
        while (g_connLogRing.size() > 2000) g_connLogRing.pop_front();
    }
    std::cout << line << "\n";
}
std::vector<std::string> SonobusHubServer::connLogLines(int n) {
    std::lock_guard<std::mutex> g(g_connLogMutex);
    std::vector<std::string> out;
    const size_t take = n <= 0 ? g_connLogRing.size() : std::min<size_t>((size_t) n, g_connLogRing.size());
    out.assign(g_connLogRing.end() - (long) take, g_connLogRing.end());
    return out;
}

// ─── recent players + recent sessions ──────────────────────────────────────────────────
// One SessionRecord per group name the hub has served (≤ 50), each carrying the settings snapshot the hub ran with
// and the distinct players seen (by user name). Updated where starts, joins and removals are already logged and
// mirrored whole to hub_sessions.json (written on every change — rare events). The server has no JUCE, so the JSON is
// written and read with the small helpers below (a strict, nesting-aware scanner: a settings value may itself be
// called "group" or "first", so plain substring lookups are not safe here).
static std::string hub_json_esc(const std::string& in) {
    std::string o; o.reserve(in.size() + 8);
    for (unsigned char c : in) {
        switch (c) {
            case '"': o += "\\\""; break; case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break; case '\r': o += "\\r"; break; case '\t': o += "\\t"; break;
            default: if (c < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", c); o += b; } else o += (char) c;
        }
    }
    return o;
}
static size_t hub_json_ws(const std::string& s, size_t p) { while (p < s.size() && (s[p] == ' ' || s[p] == '\n' || s[p] == '\r' || s[p] == '\t')) ++p; return p; }
// Index just past the JSON value starting at s[p] (string / number / literal / object / array). npos on malformed text.
static size_t hub_json_skip_value(const std::string& s, size_t p) {
    p = hub_json_ws(s, p);
    if (p >= s.size()) return std::string::npos;
    if (s[p] == '"') {
        for (++p; p < s.size(); ++p) { if (s[p] == '\\') ++p; else if (s[p] == '"') return p + 1; }
        return std::string::npos;
    }
    if (s[p] == '{' || s[p] == '[') {
        int depth = 0; bool inStr = false;
        for (; p < s.size(); ++p) {
            const char c = s[p];
            if (inStr) { if (c == '\\') ++p; else if (c == '"') inStr = false; continue; }
            if (c == '"') inStr = true; else if (c == '{' || c == '[') ++depth; else if ((c == '}' || c == ']') && --depth == 0) return p + 1;
        }
        return std::string::npos;
    }
    while (p < s.size() && s[p] != ',' && s[p] != '}' && s[p] != ']' && s[p] != ' ' && s[p] != '\n' && s[p] != '\r' && s[p] != '\t') ++p;
    return p;
}
// "abc" (JSON string text, quotes included) -> abc with escapes decoded; anything else is returned as-is (numbers, bools).
static std::string hub_json_unquote(const std::string& raw) {
    if (raw.size() < 2 || raw[0] != '"') return raw;
    std::string out; out.reserve(raw.size());
    for (size_t p = 1; p + 1 < raw.size(); ++p) {
        if (raw[p] == '\\' && p + 2 < raw.size()) {
            const char c = raw[p + 1];
            if (c == 'u' && p + 5 < raw.size()) { const long v = strtol(raw.substr(p + 2, 4).c_str(), nullptr, 16); out += v < 0x80 ? (char) v : '?'; p += 5; }
            else { out += c == 'n' ? '\n' : c == 't' ? '\t' : c == 'r' ? '\r' : c; ++p; }
        } else out += raw[p];
    }
    return out;
}
// Top-level members of one JSON object text: key -> raw value text (strings keep their quotes, nested objects/arrays
// come back whole so the caller can recurse).
static std::vector<std::pair<std::string, std::string>> hub_json_members(const std::string& obj) {
    std::vector<std::pair<std::string, std::string>> out;
    size_t p = hub_json_ws(obj, 0);
    if (p >= obj.size() || obj[p] != '{') return out;
    p = hub_json_ws(obj, p + 1);
    while (p < obj.size() && obj[p] != '}') {
        const size_t k0 = p, k1 = hub_json_skip_value(obj, p);
        if (k1 == std::string::npos || obj[k0] != '"') break;
        p = hub_json_ws(obj, k1);
        if (p >= obj.size() || obj[p] != ':') break;
        const size_t v0 = hub_json_ws(obj, p + 1), v1 = hub_json_skip_value(obj, v0);
        if (v1 == std::string::npos) break;
        out.emplace_back(hub_json_unquote(obj.substr(k0, k1 - k0)), obj.substr(v0, v1 - v0));
        p = hub_json_ws(obj, v1);
        if (p < obj.size() && obj[p] == ',') p = hub_json_ws(obj, p + 1);
    }
    return out;
}
// Top-level items of one JSON array text (raw value texts).
static std::vector<std::string> hub_json_items(const std::string& arr) {
    std::vector<std::string> out;
    size_t p = hub_json_ws(arr, 0);
    if (p >= arr.size() || arr[p] != '[') return out;
    p = hub_json_ws(arr, p + 1);
    while (p < arr.size() && arr[p] != ']') {
        const size_t v1 = hub_json_skip_value(arr, p);
        if (v1 == std::string::npos) break;
        out.push_back(arr.substr(p, v1 - p));
        p = hub_json_ws(arr, v1);
        if (p < arr.size() && arr[p] == ',') p = hub_json_ws(arr, p + 1);
    }
    return out;
}
static std::string hub_json_member(const std::vector<std::pair<std::string, std::string>>& m, const char* key) {
    for (const auto& kv : m) if (kv.first == key) return hub_json_unquote(kv.second);
    return "";
}
static int64_t hub_now_ms() {
    return (int64_t) std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}
static void hub_player_to_json(std::ostream& o, const SonobusHubServer::RecentPlayer& r) {
    o << "{\"user\":\"" << hub_json_esc(r.user) << "\",\"ip\":\"" << hub_json_esc(r.ip) << "\",\"lan\":" << (r.lan ? "true" : "false")
      << ",\"first\":" << r.firstSeenMs << ",\"join\":" << r.lastJoinMs << ",\"left\":" << r.lastLeftMs << ",\"joins\":" << r.joins << "}";
}
static SonobusHubServer::RecentPlayer hub_player_from_json(const std::string& obj) {
    const auto m = hub_json_members(obj);
    SonobusHubServer::RecentPlayer r;
    r.user = hub_json_member(m, "user"); r.ip = hub_json_member(m, "ip"); r.lan = hub_json_member(m, "lan") == "true";
    r.firstSeenMs = atoll(hub_json_member(m, "first").c_str()); r.lastJoinMs = atoll(hub_json_member(m, "join").c_str());
    r.lastLeftMs = atoll(hub_json_member(m, "left").c_str()); r.joins = atoi(hub_json_member(m, "joins").c_str());
    return r;
}
static constexpr size_t kHubMaxSessions = 50;
static constexpr size_t kHubMaxSessionPlayers = 200;

// hub_sessions.json:
// {"_schema":1,"sessions":[{"group":"g","password":"p","first":ms,"last":ms,"settings":{"key":"text",...},
//                           "players":[{"user","ip","lan","first","join","left","joins"},...]}, ...]}
// Sessions oldest-used first (the current one last), players oldest first — the same order as in memory.
void SonobusHubServer::sessionsSaveLocked() const {
    if (mSessionsFile.empty()) return;
    std::ostringstream o; o << "{\"_schema\":1,\"sessions\":[\n";
    bool first = true;
    for (const auto& sdesc : mSessions) {
        o << (first ? "" : ",\n") << " {\"group\":\"" << hub_json_esc(sdesc.group) << "\",\"password\":\"" << hub_json_esc(sdesc.password) << "\""
          << ",\"first\":" << sdesc.firstUsedMs << ",\"last\":" << sdesc.lastUsedMs << ",\"settings\":{";
        bool f2 = true;
        for (const auto& kv : sdesc.settings) { o << (f2 ? "" : ",") << "\"" << hub_json_esc(kv.first) << "\":\"" << hub_json_esc(kv.second) << "\""; f2 = false; }
        o << "},\"players\":[";
        f2 = true;
        for (const auto& r : sdesc.players) { o << (f2 ? "" : ","); hub_player_to_json(o, r); f2 = false; }
        o << "]}";
        first = false;
    }
    o << "\n]}\n";
    const std::string tmp = mSessionsFile + ".tmp";
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f) return;   // directory not there yet (hub_settings.json never saved) — in memory only until then
    f << o.str(); f.close();
    if (!f) { ::remove(tmp.c_str()); return; }
    ::rename(tmp.c_str(), mSessionsFile.c_str());
}
void SonobusHubServer::setSessionsFile(const std::string& path) {
    std::lock_guard<std::mutex> g(mRecentMutex);
    mSessionsFile = path;
    mSessions.clear();
    mSessionGroup.clear();
    if (path.empty()) return;
    std::ifstream f(path, std::ios::binary);
    if (!f) return;
    std::stringstream ss; ss << f.rdbuf();
    const std::string text = ss.str();
    std::string arr;
    for (const auto& kv : hub_json_members(text)) if (kv.first == "sessions") arr = kv.second;
    for (const auto& item : hub_json_items(arr)) {
        const auto m = hub_json_members(item);
        SessionRecord sdesc;
        sdesc.group = hub_json_member(m, "group"); sdesc.password = hub_json_member(m, "password");
        sdesc.firstUsedMs = atoll(hub_json_member(m, "first").c_str()); sdesc.lastUsedMs = atoll(hub_json_member(m, "last").c_str());
        for (const auto& kv : m) {
            if (kv.first == "settings") { for (const auto& skv : hub_json_members(kv.second)) sdesc.settings.emplace_back(skv.first, hub_json_unquote(skv.second)); }
            else if (kv.first == "players") { for (const auto& pj : hub_json_items(kv.second)) { auto r = hub_player_from_json(pj); if (!r.user.empty()) sdesc.players.push_back(std::move(r)); } }
        }
        // a hub that was killed mid-session never logged the leave: anything still "connected" in the file left when we did
        for (auto& r : sdesc.players) if (r.lastLeftMs < r.lastJoinMs) r.lastLeftMs = r.lastJoinMs;
        while (sdesc.players.size() > kHubMaxSessionPlayers) sdesc.players.erase(sdesc.players.begin());
        if (!sdesc.group.empty()) mSessions.push_back(std::move(sdesc));
    }
    while (mSessions.size() > kHubMaxSessions) mSessions.pop_front();
    std::cout << "[HUB-SESSION] " << mSessions.size() << " recent session(s) loaded from " << path << "\n";
}
void SonobusHubServer::setSessionSettingsSnapshot(std::vector<std::pair<std::string, std::string>> kv) {
    std::lock_guard<std::mutex> g(mRecentMutex);
    mSessionSettings = std::move(kv);
}
SonobusHubServer::SessionRecord* SonobusHubServer::currentSessionLocked() {
    if (mSessionGroup.empty()) return nullptr;
    for (auto& sdesc : mSessions) if (sdesc.group == mSessionGroup) return &sdesc;
    return nullptr;
}
// start() -> the hub now serves `group`: open (or re-open) its record as the current session.
void SonobusHubServer::sessionBegin(const std::string& group, const std::string& password) {
    if (group.empty()) return;
    const int64_t now = hub_now_ms();
    std::lock_guard<std::mutex> g(mRecentMutex);
    mPlayersSinceStart.clear();
    SessionRecord rec;
    for (auto it = mSessions.begin(); it != mSessions.end(); ++it) if (it->group == group) { rec = std::move(*it); mSessions.erase(it); break; }
    if (rec.firstUsedMs == 0) rec.firstUsedMs = now;
    rec.group = group; rec.password = password; rec.lastUsedMs = now;
    if (!mSessionSettings.empty()) rec.settings = mSessionSettings;
    for (auto& r : rec.players) if (r.lastLeftMs < r.lastJoinMs) r.lastLeftMs = r.lastJoinMs;   // fresh start: nobody is connected yet
    mSessions.push_back(std::move(rec));
    while (mSessions.size() > kHubMaxSessions) mSessions.pop_front();
    mSessionGroup = group;
    std::cout << "[HUB-SESSION] session group='" << group << "' (" << (mSessions.back().firstUsedMs == now ? "new" : "seen before") << ", " << mSessions.back().players.size() << " players remembered, " << mSessions.size() << " sessions)\n";
    sessionsSaveLocked();
}
void SonobusHubServer::recentNoteJoin(const std::string& user, const std::string& ip, bool lan) {
    if (user.empty() || user == mHubUserName) return;
    const int64_t now = hub_now_ms();
    std::lock_guard<std::mutex> g(mRecentMutex);
    mPlayersSinceStart.insert(user);
    SessionRecord* cur = currentSessionLocked();
    if (!cur) return;
    RecentPlayer e;
    for (auto it = cur->players.begin(); it != cur->players.end(); ++it) if (it->user == user) { e = *it; cur->players.erase(it); break; }
    if (e.firstSeenMs == 0) e.firstSeenMs = now;
    e.user = user; e.ip = ip; e.lan = lan; e.lastJoinMs = now; ++e.joins;
    cur->players.push_back(std::move(e));
    while (cur->players.size() > kHubMaxSessionPlayers) cur->players.erase(cur->players.begin());
    sessionsSaveLocked();
}
void SonobusHubServer::recentNoteLeave(const std::string& user) {
    const int64_t now = hub_now_ms();
    std::lock_guard<std::mutex> g(mRecentMutex);
    SessionRecord* cur = currentSessionLocked();
    if (!cur) return;
    for (auto& r : cur->players) if (r.user == user) { r.lastLeftMs = now; sessionsSaveLocked(); return; }
}
std::vector<SonobusHubServer::RecentPlayer> SonobusHubServer::recentPlayers(int max) const {
    std::lock_guard<std::mutex> g(mRecentMutex);
    std::vector<RecentPlayer> out;
    for (const auto& sdesc : mSessions) {
        if (sdesc.group != mSessionGroup) continue;
        for (auto it = sdesc.players.rbegin(); it != sdesc.players.rend() && (max <= 0 || (int) out.size() < max); ++it) out.push_back(*it);
        break;
    }
    return out;
}
int SonobusHubServer::playersSinceStart() const {
    std::lock_guard<std::mutex> g(mRecentMutex);
    return (int) mPlayersSinceStart.size();
}
std::vector<SonobusHubServer::SessionRecord> SonobusHubServer::sessions() const {
    std::lock_guard<std::mutex> g(mRecentMutex);
    std::vector<SessionRecord> out(mSessions.rbegin(), mSessions.rend());   // newest (last used) first
    return out;
}
int SonobusHubServer::sendGroupMessage(const std::string& text) {
    if (text.empty() || text.size() > 1000) return 0;
    char buf[2048];
    osc::OutboundPacketStream msg(buf, sizeof(buf));
    const std::string group = snapshotForUI().groupName;
    try {
        msg << osc::BeginMessage(SB_HXMSG_DOMAIN SB_HXMSG_CHAT) << group.c_str() << "Hub" << "" << "" << text.c_str() << osc::EndMessage;
    } catch (const osc::Exception&) { return 0; }
    std::vector<std::shared_ptr<HubPeer>> snap;
    {
        std::shared_lock<std::shared_mutex> lock(mPeerMutex);
        for (auto& p : mPeers) if (p) snap.push_back(p);
    }
    int sent = 0;
    for (auto& p : snap) {
        if (!p->active || p->addrlen <= 0) continue;
        ::sendto(mSocket, msg.Data(), msg.Size(), 0, (const struct sockaddr*)&p->addr, p->addrlen);
        ++sent;
    }
    std::cout << "[HUB-CHAT] hub message to " << sent << " peer(s)\n";
    return sent;
}
bool SonobusHubServer::removeSessionPlayer(const std::string& group, const std::string& user, std::string& err) {
    std::lock_guard<std::mutex> g(mRecentMutex);
    for (auto& sdesc : mSessions) {
        if (sdesc.group != group) continue;
        for (auto it = sdesc.players.begin(); it != sdesc.players.end(); ++it) {
            if (it->user != user) continue;
            if (it->lastLeftMs < it->lastJoinMs) { err = "that player is connected now"; return false; }
            sdesc.players.erase(it);
            std::cout << "[HUB-SESSION] removed player '" << user << "' from group='" << group << "'\n";
            sessionsSaveLocked();
            return true;
        }
        err = "no such player"; return false;
    }
    err = "no such session"; return false;
}
bool SonobusHubServer::removeSession(const std::string& group, std::string& err) {
    std::lock_guard<std::mutex> g(mRecentMutex);
    if (group == mSessionGroup) { err = "that is the session the hub is serving now"; return false; }
    for (auto it = mSessions.begin(); it != mSessions.end(); ++it) {
        if (it->group != group) continue;
        mSessions.erase(it);
        std::cout << "[HUB-SESSION] removed session group='" << group << "' (" << mSessions.size() << " sessions left)\n";
        sessionsSaveLocked();
        return true;
    }
    err = "no such session"; return false;
}
bool SonobusHubServer::sessionFor(const std::string& group, SessionRecord& out) const {
    std::lock_guard<std::mutex> g(mRecentMutex);
    for (const auto& sdesc : mSessions) if (sdesc.group == group) { out = sdesc; return true; }
    return false;
}

static void hub_fwprobe_req_from_server(const char* buf, int len, const struct sockaddr* from, int /*fromlen*/, int replyfd) {
    if (from->sa_family != AF_INET) return;
    try {
        osc::ReceivedPacket pk(buf, (osc::osc_bundle_element_size_t)len);
        if (!pk.IsMessage()) return;
        osc::ReceivedMessage m(pk);
        if (std::strcmp(m.AddressPattern(), "/sb/fwprobe/req") != 0) return;
        auto it = m.ArgumentsBegin();
        const int64_t token = it->AsInt64(); ++it;
        const int32_t up = it->AsInt32(); ++it;
        const int32_t sp = it->AsInt32();
        const int fd = ::dup(replyfd);
        if (fd < 0) return;
        hub_serve_port_probe(*(const struct sockaddr_in*)from, token, up, sp, fd);
    } catch (...) {}
}

bool SonobusHubServer::start(const std::string& aooServerHost, int aooServerPort,
                             const std::string& username, const std::string& groupName,
                             const std::string& groupPassword,
                             int localUdpPort, int sampleRate, int blockSize, int channels) {
    if (mRunning.load()) {
        std::cerr << "[HUB] Already running\n";
        return false;
    }

    // Open on the control path.
    if (!mDumpPeerName.empty() && mDumpSecs > 0) {
        std::string path = sb_hub_gui::hub_settings::dataDir().getChildFile("dump_peer.f32").getFullPathName().toStdString();
        mDumpFile = fopen(path.c_str(), "wb");
        if (mDumpFile) {
            setvbuf(mDumpFile, nullptr, _IOFBF, 4 * 1024 * 1024);
            mDumpFramesLeft.store(mDumpSecs * sampleRate, std::memory_order_relaxed);
            std::cout << "[HUB-DUMP] dumping '" << mDumpPeerName << "' to " << path
                      << " (" << mDumpSecs << "s)\n";
        }
    }

    mSampleRate = sampleRate;
    mBlockSize = blockSize;
    mChannels = channels;
    mGroupName = groupName;
    mGroupPassword = groupPassword;   // For Copy Group Link
    mHubUserName = username;
    mHubStartedAtMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    mMixBuffer.resize(mBlockSize * mChannels, 0.0f);

    // The local AOO signaling
    // server is started AFTER the hub UDP socket is bound — see below.

    // Create UDP socket
    mSocket = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (mSocket < 0) {
        std::cerr << "[HUB] Failed to create UDP socket\n";
        return false;
    }

    // The per-listener sockets bind to this socket's port (clients must keep seeing the hub's known
    // port as the source). On BSD/macOS every socket sharing a port must carry SO_REUSEPORT (and
    // SO_REUSEADDR), including the first one, so set them here, before bind, only when the mode is on.
    // SO_REUSEPORT also lets a second hub process bind the port while a previous instance is shutting
    // down. That instance keeps its per-peer connect()ed sockets open, so (1) our connect() for the same
    // 4-tuple fails EADDRINUSE (XNU rejects a duplicate connected UDP 4-tuple; openConnectedSendSocket
    // falls back to the shared socket) and (2) the kernel keeps steering that peer's inbound datagrams
    // into the old process's socket, which nobody reads. So probe the port without SO_REUSEPORT (fails
    // EADDRINUSE while any socket of a previous instance holds it), wait up to 30 s for it to be
    // released, then fail start() loudly instead of co-binding.
    if (mConnectedSockets && localUdpPort != 0) {
        struct sockaddr_in probeAddr;
        memset(&probeAddr, 0, sizeof(probeAddr));
        probeAddr.sin_family = AF_INET;
        probeAddr.sin_addr.s_addr = INADDR_ANY;
        probeAddr.sin_port = htons(localUdpPort);
        bool held = false;
        for (int attempt = 0; attempt < 120; ++attempt) {   // 120 x 250 ms = 30 s
            const int probe = ::socket(AF_INET, SOCK_DGRAM, 0);
            if (probe < 0) break;   // cannot probe: fall through to the normal bind
            const int rc = ::bind(probe, (struct sockaddr*)&probeAddr, sizeof(probeAddr));
            const int err = errno;
            ::close(probe);
            if (rc == 0) { held = false; break; }
            if (err != EADDRINUSE) break;   // anything else: let the real bind below report it
            if (!held) {
                std::cerr << "[HUB-SOCK] UDP port " << localUdpPort
                          << " is still held by a previous hub instance (or its connected sockets)"
                          << " — waiting for it to be released before binding\n";
            }
            held = true;
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
        if (held) {
            std::cerr << "[HUB-SOCK] UDP port " << localUdpPort
                      << " still held after 30 s — refusing to start a second hub on it"
                      << " (quit the previous instance first)\n";
            ::close(mSocket);
            mSocket = -1;
            return false;
        }
    }

    if (mConnectedSockets) {
        int one = 1;
        if (::setsockopt(mSocket, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) != 0
            || ::setsockopt(mSocket, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one)) != 0) {
            std::cerr << "[HUB] warning: SO_REUSEADDR/SO_REUSEPORT failed (" << strerror(errno)
                      << ") — connected send sockets disabled, using the shared socket\n";
            mConnectedSockets = false;
        }
    }

    // DSCP EF + macOS VOICE service type
    // on every hub datagram (feeds to WAN listeners cross the hub site uplink).
    if (mDscpEnabled) {
        int tos_ef = 0xB8;
        if (::setsockopt(mSocket, IPPROTO_IP, IP_TOS, &tos_ef, sizeof(tos_ef)) != 0)
            std::cerr << "[HUB] warning: could not set DSCP EF on UDP socket\n";
#ifdef SO_NET_SERVICE_TYPE
        int st = NET_SERVICE_TYPE_VO;
        ::setsockopt(mSocket, SOL_SOCKET, SO_NET_SERVICE_TYPE, &st, sizeof(st));
#endif
    }

    // Raise the UDP recv buffer well past the macOS default (~786 KB) so transient recv-thread stalls
    // and scheduler hiccups under heavy peer load do not drop audio packets.
    {
        int kts = 1;   // Kernel receive timestamps for [HUB-KJIT]
#if defined(SO_TIMESTAMP_MONOTONIC)
        if (::setsockopt(mSocket, SOL_SOCKET, SO_TIMESTAMP_MONOTONIC, &kts, sizeof(kts)) < 0)
            std::cerr << "[HUB] Warning: SO_TIMESTAMP_MONOTONIC failed: " << strerror(errno) << "\n";
#elif defined(SO_TIMESTAMPNS)
        if (::setsockopt(mSocket, SOL_SOCKET, SO_TIMESTAMPNS, &kts, sizeof(kts)) < 0)
            std::cerr << "[HUB] Warning: SO_TIMESTAMPNS failed: " << strerror(errno) << "\n";
#endif
    }
    {
        int rcvbuf = 4 * 1024 * 1024;  // 4 MB
        if (::setsockopt(mSocket, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf)) < 0) {
            std::cerr << "[HUB] Warning: SO_RCVBUF setsockopt failed: " << strerror(errno) << "\n";
        } else {
            int actual = 0;
            socklen_t aLen = sizeof(actual);
            
#ifdef SO_NOSIGPIPE
    { int nsp = 1; ::setsockopt(mSocket, SOL_SOCKET, SO_NOSIGPIPE, &nsp, sizeof(nsp)); }
#endif
   // See openConnectedSendSocket
            if (::getsockopt(mSocket, SOL_SOCKET, SO_RCVBUF, &actual, &aLen) == 0) {
                std::cout << "[HUB] UDP recv buffer set to " << actual << " bytes\n";
            }
        }
    }

    struct sockaddr_in bindAddr;
    memset(&bindAddr, 0, sizeof(bindAddr));
    bindAddr.sin_family = AF_INET;
    bindAddr.sin_addr.s_addr = INADDR_ANY;
    bindAddr.sin_port = htons(localUdpPort);

    if (::bind(mSocket, (struct sockaddr*)&bindAddr, sizeof(bindAddr)) < 0) {
        std::cerr << "[HUB] Failed to bind UDP socket on port " << localUdpPort << "\n";
        ::close(mSocket);
        mSocket = -1;
        return false;
    }

    // Get assigned port
    struct sockaddr_in actualAddr;
    socklen_t addrLen = sizeof(actualAddr);
    ::getsockname(mSocket, (struct sockaddr*)&actualAddr, &addrLen);
    mLocalPort = ntohs(actualAddr.sin_port);
    std::cout << "[HUB] UDP socket bound on port " << mLocalPort << "\n";

    // Set socket non-blocking for recv thread
    int flags = fcntl(mSocket, F_GETFL, 0);
    fcntl(mSocket, F_SETFL, flags | O_NONBLOCK);

    // LAN subnet is derived from the default-route interface unless a --lan-subnet CLI override was given.
    if (mLanSubnetCidr.empty()) {
        // At login the hub can start before en0 has an address; the default-route probe then answers via
        // lo0 and every LAN player gets the WAN hold. Wait up to 30 s for a real (non-loopback)
        // default-route interface before giving up.
        std::string ifname;
        for (int tries = 0; tries < 30; ++tries) {
            mLanSubnetCidr = hub_default_route_cidr(ifname);
            if (!mLanSubnetCidr.empty()) break;
            if (tries == 0) std::cout << "[HUB] LAN subnet: no non-loopback default route yet — waiting for the network (up to 30 s)\n";
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        if (mLanSubnetCidr.empty())
            std::cerr << "[HUB] WARNING: could not derive the LAN subnet from the default-route interface — every player is treated as WAN\n";
        else
            std::cout << "[HUB] LAN subnet auto-detected: " << mLanSubnetCidr << " (" << ifname << ")\n";
    }
    // Decide what to advertise BEFORE the
    // signaling server exists (no client can reconnect during the STUN wait).
    resolveAdvertisedAddresses(aooServerHost);

    // Start the local AOO signaling server if requested.
    // This must follow the UDP bind. A server already accepting reconnects during the port probe above
    // (up to 30 s) would make each client fire its one-shot /sb/hxfeedreq at a hub port not bound yet,
    // and those listeners would stay deaf. Binding first means clients cannot reconnect until this
    // process owns the port: their reconnect timer retries 1 s later, and the first feedreq they send
    // lands on a socket we are reading.
    if (mRunLocalServer) {
        int32_t err = 0;
        // The built-in server serves only this hub's group, with this hub's password,
        // from before the first player can connect (see aoo server get_group).
        aoo_sb_set_group_policy(groupName.c_str(), groupPassword.c_str());
        std::cout << "[HUB] Group policy: only '" << groupName << "' on this server, "
                  << (groupPassword.empty() ? "no password" : "password required") << "\n";
        mAooServer.reset(aoo::net::iserver::create(mLocalServerPort, &err));
        if (!mAooServer || err != 0) {
            std::cerr << "[HUB] Failed to create local AOO server on port " << mLocalServerPort << " (err=" << err << ")\n";
            mAooServer.reset();
            ::close(mSocket);
            mSocket = -1;
            return false;
        }
        if (!mLanSubnetCidr.empty()) {
            if (mAooServer->set_lan_subnet(mLanSubnetCidr.c_str()) != 1) {
                std::cerr << "[HUB] WARNING: failed to apply LAN subnet " << mLanSubnetCidr << " — continuing without classification.\n";
            }
        }
        mServerThread = std::make_unique<std::thread>([this]() {
            std::cout << "[HUB] Local AOO signaling server running on port " << mLocalServerPort << "\n";
            mAooServer->run();
            std::cout << "[HUB] Local AOO signaling server stopped.\n";
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    // Create AOO client endpoint for the send callback
    auto* clientEndpoint = new HubEndpoint();
    clientEndpoint->socketFd = mSocket;
    mClientEndpoint = clientEndpoint;

    mClient.reset(aoo::net::iclient::create(
        (void*)clientEndpoint,
        [](void* user, const char* data, int32_t size, void* addr) -> int32_t {
            return hub_client_send_impl(user, data, size, addr);
        },
        mLocalPort
    ));

    if (!mClient) {
        std::cerr << "[HUB] Failed to create AOO client\n";
        ::close(mSocket);
        mSocket = -1;
        delete clientEndpoint;
        mClientEndpoint = nullptr;
        return false;
    }

    if (!mPublicAddressHost.empty()) {
        // public_address may be a domain (the group link shows it as typed), but what goes on the wire must
        // be numeric: players parse the advertised address with inet_addr(), so a name becomes 255.255.255.255
        // and no WAN player can reach the hub's audio port. Resolve here, once per hub start.
        std::string advHost;
        struct in_addr advIp {};
        if (hub_resolve_ipv4(mPublicAddressHost, advIp)) {
            char numeric[INET_ADDRSTRLEN] = {};
            if (inet_ntop(AF_INET, &advIp, numeric, sizeof(numeric))) advHost = numeric;
        }
        if (advHost.empty()) {   // DNS down at start: fall back to the STUN-derived address, else the server's NAT-observed source
            advHost = mAdvertisedPublicHost;
            std::cerr << "[HUB] public_address '" << mPublicAddressHost << "' did not resolve; "
                      << (advHost.empty() ? std::string("using the server's observed address") : "using " + advHost) << "\n";
        }
        if (!advHost.empty()) {
            mClient->set_self_public_address(advHost.c_str(), mPublicAddressPort);
            std::cout << "[HUB] Advertising public address override: "
                      << advHost << (advHost != mPublicAddressHost ? " (" + mPublicAddressHost + ")" : std::string())
                      << ":" << (mPublicAddressPort > 0 ? std::to_string(mPublicAddressPort) : std::string("(auto)"))
                      << " (override AOO server's NAT-observed source)\n";
        }
    } else if (!mAdvertisedPublicHost.empty()) {
        // STUN-derived WAN address (see resolveAdvertisedAddresses)
        mClient->set_self_public_address(mAdvertisedPublicHost.c_str(), mAdvertisedPublicPort);
    }
    if (!mAdvertisedLanHost.empty())
        mClient->set_self_local_address(mAdvertisedLanHost.c_str(), mLocalPort);   // Loopback signaling -> LAN address in LOGIN


    mRunning = true;

    // Start client run thread (blocking TCP signaling with AOO server)
    mClientRunThread = std::make_unique<std::thread>([this]() {
        std::cout << "[HUB] Client TCP thread started.\n";
        if (mClient) mClient->run();
        std::cout << "[HUB] Client TCP thread stopped.\n";
    });

    // Start network receive thread
    mRecvThread = std::make_unique<std::thread>([this]() { networkRecvThread(); });

    // Start send thread
    mSendThread = std::make_unique<std::thread>([this]() { sendThread(); });

    // Resolve the send mode once, here, before any emitter thread exists. Cut-through and pacing are
    // mutually exclusive owners of HubPeer::outBundle; cut-through wins and the pacer thread is not
    // started (it would only hold a 60%-of-period time-constraint claim for nothing).
    // Resolve --inline-listeners first: it can demote cut-through to paced. P = block period @48k; the
    // audio thread's computation claim is P/2; governor budget = 60% of P/2; per-listener work is
    // 10-50 µs, one bundled sendto about 25 µs. With 6 listeners:
    //   @128  budget 800 µs: inline work and cut-through both fit.
    //   @64   budget 400 µs: the typical case fits; the worst case is left to the runtime governor.
    //   @32   budget 200 µs: cut-through is dropped and send mode is forced to paced (the pacer thread
    //         carries the sendto's); inline work alone is arbitrated by the governor.
    // Static gate: typical inline+cut-through estimate 6×(30+25) µs vs budget.
    const bool inlineReq = mInlineListenersReq.load(std::memory_order_relaxed);
    if (inlineReq) {
        const uint64_t blockNsStart = (uint64_t)(1000.0 * mBlockSize / std::max(1, mSampleRate) * 1e6);
        const uint64_t budgetNs = (blockNsStart / 2) * 60ULL / 100ULL;
        const uint64_t estCutThroughNs = 6ULL * (30000ULL + 25000ULL);
        if (mCutThrough.load(std::memory_order_relaxed) && estCutThroughNs > budgetNs) {
            std::cout << "[HUB] --inline-listeners + --cut-through: estimated 6 x (30+25) us = "
                      << (estCutThroughNs / 1000) << " us exceeds 60% of P/2 = " << (budgetNs / 1000)
                      << " us at blocksize " << mBlockSize
                      << " -> cut-through dropped, send mode forced to paced.\n";
            mCutThrough.store(false, std::memory_order_relaxed);
            mSendPacing.store(true, std::memory_order_relaxed);
        }
        mInlineActive.store(true, std::memory_order_release);
    }
    const bool cutThrough = mCutThrough.load(std::memory_order_relaxed);
    if (cutThrough && mSendPacing.load(std::memory_order_relaxed)) {
        std::cout << "[HUB] --cut-through overrides --send-pace (pacer not started).\n";
        mSendPacing.store(false, std::memory_order_relaxed);
    }
    // Paced fan-out emission thread. Started unconditionally (the idle condvar wait costs nothing when
    // --send-pace is off); emission is gated per block by mSendPacing, except under --cut-through, where
    // the listener threads are the emitters.
    if (!cutThrough)
        mPacerThread = std::make_unique<std::thread>([this]() { pacerThread(); });
    // One-line banner so a log tail shows the
    // wire mode. Bundling happens on the emitter: pacer (--send-pace) or
    // listener thread (--cut-through).
    std::cout << "[HUB] DSCP marking: " << (mDscpEnabled ? "EF (voice)" : "off") << "\n";
    // Send-socket mode banner (A/B anchor for --connected-sockets).
    std::cout << "[HUB] Send sockets: " << (mConnectedSockets ? "connected per listener" : "shared") << "\n";
    std::cout << "[HUB] Redundancy: LAN " << mRedundancyLan << " / WAN " << mRedundancyWan << " copies per block\n";
    // Send-mode banner (paced / cut-through / legacy inline).
    if (cutThrough)
        std::cout << "[HUB] Send mode: cut-through\n";
    else if (mSendPacing.load(std::memory_order_relaxed))
        std::cout << "[HUB] Send mode: paced (dither "
                  << (mPaceDither.load(std::memory_order_relaxed) ? "on" : "off") << ")\n";
    else
        std::cout << "[HUB] Send mode: inline (unpaced, unbundled)\n";
    // Listener execution mode banner.
    if (inlineReq)
        std::cout << "[HUB] Listener mode: inline (audio thread pumps every listener; no listener RT threads;"
                  << " governor budget 60% of P/2, window " << kInlineWindowBlocks << " blocks)\n";
    else
        std::cout << "[HUB] Listener mode: threaded (one RT thread per listener, fork-join barrier)\n";
    const bool bundleActive = cutThrough || mSendPacing.load(std::memory_order_relaxed);
    std::cout << "[HUB] Bundle packing: " << (mBundleEnabled.load(std::memory_order_relaxed) ? "ON" : "OFF")
              << " (mtu cap " << mBundleMtu << " bytes"
              << (bundleActive ? "" : "; inactive without --send-pace / --cut-through")
              << ")\n";


    // Connect to AOO server
    std::cout << "[HUB] Connecting to AOO server " << aooServerHost << ":" << aooServerPort << " as '" << username << "'\n";
    mClient->connect(aooServerHost.c_str(), aooServerPort, username.c_str(), "");

    // Wait for connection
    for (int i = 0; i < 100 && !mConnected.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        if (mClient->events_available()) {
            mClient->handle_events(
                [](void* user, const aoo_event** events, int32_t n) -> int32_t {
                    auto* self = static_cast<SonobusHubServer*>(user);
                    for (int32_t i = 0; i < n; ++i) {
                        if (events[i]->type == AOONET_CLIENT_CONNECT_EVENT) {
                            auto* e = (aoonet_client_event*)events[i];
                            if (e->result > 0) {
                                self->mConnected = true;
                                std::cout << "[HUB] Connected to AOO server\n";
                            } else {
                                std::cerr << "[HUB] Connection failed: " << (e->errormsg ? e->errormsg : "unknown") << "\n";
                            }
                        }
                    }
                    return 1;
                }, this);
        }
    }

    if (!mConnected.load()) {
        std::cerr << "[HUB] Timed out connecting to AOO server\n";
        stop();
        return false;
    }

    // Join group
    std::cout << "[HUB] Joining group '" << groupName << "'\n";
    mClient->group_join(groupName.c_str(), groupPassword.c_str());

    // Wait for group join
    for (int i = 0; i < 50 && !mGroupJoined.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (mClient->events_available()) {
            mClient->handle_events(
                [](void* user, const aoo_event** events, int32_t n) -> int32_t {
                    auto* self = static_cast<SonobusHubServer*>(user);
                    for (int32_t i = 0; i < n; ++i) {
                        if (events[i]->type == AOONET_CLIENT_GROUP_JOIN_EVENT) {
                            auto* e = (aoonet_client_group_event*)events[i];
                            if (e->result > 0) {
                                self->mGroupJoined = true;
                                std::cout << "[HUB] Joined group '" << e->name << "'\n";
                            } else {
                                std::cerr << "[HUB] Failed to join group: " << (e->errormsg ? e->errormsg : "unknown") << "\n";
                            }
                        }
                    }
                    return 1;
                }, this);
        }
    }

    if (!mGroupJoined.load()) {
        std::cerr << "[HUB] Timed out joining group\n";
        stop();
        return false;
    }

    // Start client event thread (handles peer join/leave events)
    mClientEventThread = std::make_unique<std::thread>([this]() { clientRunThread(); });

    // Start audio processing thread
    mAudioThread = std::make_unique<std::thread>([this]() { audioProcessThread(); });

    // Stats writer. Off the audio thread, flushes the per-block timing histogram to stats.csv in the
    // hub data folder every 5 s. Reads the mStats atomics, with no synchronization with the audio thread.
    mStatsThread = std::make_unique<std::thread>([this]() { statsWriterThread(); });
    mPortCheckThread = std::make_unique<std::thread>([this]() { portCheckThreadMain(); });   
    if (mRunLocalServer) aoo_sb_set_fwprobe_req_handler(&hub_fwprobe_req_from_server);   // answer other hubs' port checks on server_port
    g_hubForJoinGate.store(this, std::memory_order_release);
    aoo_sb_set_join_gate(&hub_join_gate);
    aoo_sb_set_stage_max_players(stageSettings().maxPlayers);
    aoo_sb_set_conn_event_handler(&hub_conn_event_from_server);   // Connection log

    sessionBegin(mGroupName, mGroupPassword);   // Recent sessions — the hub now serves this group

    std::cout << "[HUB] Hub server running. Waiting for peers...\n";
    return true;
}

void SonobusHubServer::stop() {
    // Teardown must also work after a failed start(). start() creates mServerThread (the local AOO
    // signaling server, blocking in mAooServer->run()) before it binds the hub UDP socket and sets
    // mRunning much later, so a failed start() leaves that thread joinable with mRunning false, and
    // destroying a joinable std::thread calls std::terminate. The running branch is conditional and the
    // resource teardown below always runs; every step is guarded on the resource existing, so a second
    // call (explicit stop() followed by the destructor) is a clean no-op.
    const bool wasRunning = mRunning.exchange(false);

    if (wasRunning) {
    std::cout << "[HUB] Stopping hub server...\n";

    if (mClient) {
        if (mGroupJoined.load()) {
            mClient->group_leave(mGroupName.c_str());
        }
        mClient->disconnect();
        mClient->quit();
    }

    if (mAudioThread && mAudioThread->joinable()) mAudioThread->join();
    if (mStatsThread && mStatsThread->joinable()) mStatsThread->join();
    if (mPortCheckThread && mPortCheckThread->joinable()) mPortCheckThread->join();   
    aoo_sb_set_fwprobe_req_handler(nullptr);
    aoo_sb_set_conn_event_handler(nullptr);
    aoo_sb_set_join_gate(nullptr);
    { SonobusHubServer* self = this; g_hubForJoinGate.compare_exchange_strong(self, nullptr); }
    if (mSendThread && mSendThread->joinable()) mSendThread->join();
    // (mLevelSendThread removed)
    // Wake + join the pacer (its wait predicate checks mRunning).
    mPacerCv.notify_all();
    if (mPacerThread && mPacerThread->joinable()) mPacerThread->join();
    if (mRecvThread && mRecvThread->joinable()) mRecvThread->join();
    if (mClientEventThread && mClientEventThread->joinable()) mClientEventThread->join();
    if (mClientRunThread && mClientRunThread->joinable()) mClientRunThread->join();

    {
        // Swap the peers out under the lock, then
        // stop+join each listener thread OUTSIDE it before dropping the refs
        // (same ownership-cycle break as removePeer; joining under the
        // exclusive lock could deadlock with a freshly-started listener
        // thread's self-lookup, which takes mPeerMutex shared). The audio
        // thread is already joined above, so its snapshot no longer changes;
        // ~HubPeer runs when the last ref drops, with its join a no-op.
        std::vector<std::shared_ptr<HubPeer>> peers;
        {
            std::lock_guard<std::shared_mutex> lock(mPeerMutex);  // exclusive: mutates mPeers
            peers.swap(mPeers);
            rebuildPeerSnapshotLocked();  // Publish the empty list
        }
        for (auto& p : peers) {
            if (p) p->stopAndJoinListenerThread();
        }
    }
    // Every snapshot reader (audio, pacer,
    // recv) is joined above, so nothing can be using a retired snapshot.
    // Force-drain here, on this control thread, so the removed peers'
    // ~HubPeer run now rather than at hub destruction.
    drainRetiredSnapshots(true);
    }   // wasRunning

    // From here down runs for a never-started server too.
    // Nothing below the `mRunning = true` line of start() exists in that
    // case (no reader threads, no peers), so this is plain resource release.
    mClient.reset();   // no run thread ever started for it if !wasRunning

    if (mAooServer) mAooServer->quit();   // unblocks mAooServer->run() in mServerThread
    if (mServerThread) {
        // Never leave a joinable std::thread for ~unique_ptr (= std::terminate).
        // The thread exists only together with mAooServer, whose quit() above
        // makes run() return, so this join is bounded.
        if (mServerThread->joinable()) mServerThread->join();
        mServerThread.reset();
    }
    mAooServer.reset();

    if (mSocket >= 0) {
        ::close(mSocket);
        mSocket = -1;
    }

    if (mClientEndpoint) {
        delete static_cast<HubEndpoint*>(mClientEndpoint);
        mClientEndpoint = nullptr;
    }

    mConnected = false;
    mGroupJoined = false;
    if (wasRunning) std::cout << "[HUB] Hub server stopped.\n";
}

int SonobusHubServer::getNumConnectedPeers() const {
    // Not locking since this is informational only
    int count = 0;
    for (auto& p : mPeers) {
        if (p && p->active) ++count;
    }
    return count;
}

// GUI snapshot: copies peer state under mPeerMutex briefly. Runs on the JUCE message thread via
// SonobusHubMainWindow's 5Hz timer. Contended only against networkRecvThread and the add/remove
// paths, which hold this mutex briefly; the audio thread holds the lock only across its own
// snapshot/flatten block. Doing the work here keeps the audio thread untouched by UI polling.
static int hub_tap_int(const char* name, int def);   // defined below (hub_taps.conf reader)

SonobusHubServer::UiSnapshot SonobusHubServer::snapshotForUI() const {
    UiSnapshot s;
    s.sampleRate = mSampleRate;
    s.blockSize = mBlockSize;
    s.channels = mChannels;
    s.useOpus = mUseOpus;
    s.opusBitrate = mOpusBitrate;
    s.groupName = mGroupName;
    s.groupPassword = mGroupPassword;   
    s.publicAddressHost = mPublicAddressHost;
    s.publicAddressPort = mPublicAddressPort;
    s.advertisedLanHost = mAdvertisedLanHost;        
    s.advertisedPublicHost = mAdvertisedPublicHost;
    s.advertisedPublicPort = mAdvertisedPublicPort;
    s.localUdpPort = mLocalPort;
    s.aooServerPort = mLocalServerPort;
    s.hubStartedAtMs = mHubStartedAtMs;

    // Bandwidth-in is the cumulative counter
    // updated by networkRecvThread. Bytes-out is aggregated below from each
    // peer's HubEndpoint::sentBytes (the static send impl writes directly
    // there; doing the aggregate read here avoids any audio-path counter
    // touches).
    s.bytesInTotal = mBytesInTotal.load(std::memory_order_relaxed);
    s.holdMode = mHoldMode.load(std::memory_order_relaxed);   
    { std::lock_guard<std::mutex> lk(mPortCheckMutex); s.portCheckText = mPortCheckText; s.portCheckState = mPortCheckState; s.portLocalText = mPortLocalText; s.portCheckVerifiedAtMs = mPortCheckVerifiedAtMs; }

    // CPU sample: process user+sys time over at least 0.9 s, so callers on several threads (window, web page,
    // internals) all read the same steady figure instead of a burst over a few milliseconds.
    {
        std::lock_guard<std::mutex> cl(mCpuMutex);
        struct rusage ru{};
        if (getrusage(RUSAGE_SELF, &ru) == 0) {
            uint64_t userUs = (uint64_t)ru.ru_utime.tv_sec * 1000000ULL + ru.ru_utime.tv_usec;
            uint64_t sysUs  = (uint64_t)ru.ru_stime.tv_sec * 1000000ULL + ru.ru_stime.tv_usec;
            int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            if (mLastCpuSampleMs == 0 || nowMs - mLastCpuSampleMs >= 900) {   // too soon: keep the last figure
                if (mLastCpuSampleMs > 0) {
                    const int64_t dtMs = nowMs - mLastCpuSampleMs;
                    const uint64_t dCpuUs = (userUs + sysUs) - (mLastCpuUserUs + mLastCpuSysUs);
                    mLastCpuPct = (float)((double)dCpuUs / 1000.0 / (double)dtMs * 100.0);   // % of one core
                }
                mLastCpuSampleMs = nowMs;
                mLastCpuUserUs = userUs;
                mLastCpuSysUs = sysUs;
            }
        }
    }
    s.hubCpuPct = mLastCpuPct;
    {
        std::lock_guard<std::mutex> sl(mStageMutex);
        s.stageMaxPlayers = mStage.maxPlayers;
        s.stageMaxChannels = mStage.maxChannels;
        s.stageListingState = mStage.list ? mStageListingState : 0;
        s.stageListingText = mStage.list ? mStageListingText : std::string();
    }

    // Pull peer shared_ptrs out under the lock, then format/copy after release
    // to keep the critical section short. (mPeerMutex is const-accessible
    // via mutable not needed — the lock_guard mutates a member declared
    // non-mutable; we drop the const for the lock by const_cast since the
    // semantics are observer-only.)
    std::vector<std::shared_ptr<HubPeer>> peers;
    {
        std::shared_lock<std::shared_mutex> lock(mPeerMutex);
        peers.reserve(mPeers.size());
        for (auto& p : mPeers) if (p) peers.push_back(p);
    }

    // Build id → userName index for resolving feed sender names.
    std::unordered_map<int32_t, std::string> idToName;
    idToName.reserve(peers.size());
    for (auto& p : peers) idToName[p->id] = p->userName;

    s.peers.reserve(peers.size());
    for (auto& p : peers) {
        UiPeerSnapshot up;
        up.id = p->id;
        up.userName = p->userName;
        up.recvChannels = p->uiRecvChannels.load(std::memory_order_relaxed);   // Atomic mirror (was a racy plain-int read)

        char ipBuf[INET6_ADDRSTRLEN] = {};
        const auto* sa = (const struct sockaddr*)&p->addr;
        if (sa->sa_family == AF_INET) {
            auto* sin = (const struct sockaddr_in*)sa;
            inet_ntop(AF_INET, &sin->sin_addr, ipBuf, sizeof(ipBuf));
            up.port = ntohs(sin->sin_port);
        } else if (sa->sa_family == AF_INET6) {
            auto* sin6 = (const struct sockaddr_in6*)sa;
            inet_ntop(AF_INET6, &sin6->sin6_addr, ipBuf, sizeof(ipBuf));
            up.port = ntohs(sin6->sin6_port);
        }
        up.ipAddr = ipBuf;

        up.lastBlocks = p->lastBlocks.load(std::memory_order_relaxed);
        up.lastBlocksNonZero = p->lastBlocksNonZero.load(std::memory_order_relaxed);
        up.smoothedRttMs = p->smoothedRttMs.load(std::memory_order_relaxed);
        up.smoothedRttUpMs = p->smoothedRttUpMs.load(std::memory_order_relaxed);
        up.joinedAtMs = p->joinedAtMs;

        // Peak jitter buffer fill since the last GUI sample. sendThread (1 ms cadence) atomic-maxes the live
        // ratio into this field; we read and reset it here. An instantaneous reading would sit near 0%
        // because the audio thread drains the queue every block; the peak shows headroom over the window.
        float peak = p->jitterBufferPeakFillPct.exchange(0.0f, std::memory_order_relaxed);
        if (peak > 0.0f) up.jitterBufferFillPct = peak;

        // Per-peer network jitter quantiles (recv-thread
        // arrival stats). compute() copies the ring under the peer's own
        // stats lock — never contends with the audio thread.
        {
            // Caller-owned rate window, so this 1 Hz GUI sampler does not reset the stats thread's pps window.
            auto q = p->jstats.compute(p->jstats.uiWin);
            up.jitP50Ms = q.p50; up.jitP95Ms = q.p95; up.jitP99Ms = q.p99;
            up.jitMaxMs = q.maxMs; up.pktRate = q.pps; up.gapPct = q.gapPct;
            {   // Hold in force: the adaptive LAN sink's live depth, or the WAN hold
                const int32_t lb = p->lanDepthBlocks.load(std::memory_order_relaxed);
                up.sinkBufMs = (p->isLan && lb > 0) ? (float) (lb * 1000.0 * (double) mBlockSize / (double) std::max(1, mSampleRate)) : (float) p->abufCurrentMs;
            }
            up.holdAtCeiling = p->holdAtCeiling.load(std::memory_order_relaxed);   
            up.clientRecvBufMs = p->clientRecvBufMs.load(std::memory_order_relaxed);
            up.clientRxUnderruns10s = p->clientRxUnderruns10s.load(std::memory_order_relaxed);   
            up.isRecording = p->isRecording.load(std::memory_order_relaxed) != 0;                   
            up.isSendMuted = p->isSendMuted.load(std::memory_order_relaxed) != 0;                   
            { std::lock_guard<std::mutex> vl(p->verLock); up.clientVersion = p->clientVersion; up.clientOs = p->clientOs; up.clientInstrument = p->clientInstrument; }
            up.clientDevBuf = p->clientDevBuf.load(std::memory_order_relaxed);
            up.clientDevSr = p->clientDevSr.load(std::memory_order_relaxed);
            up.clientRxPops10s = p->clientRxPops10s.load(std::memory_order_relaxed);
            up.clientXruns10s = p->clientXruns10s.load(std::memory_order_relaxed);
            up.clientLatFeedAgeS = p->clientLatFeedAgeS.load(std::memory_order_relaxed);
            up.clientRecvBufMode = p->clientRecvBufMode.load(std::memory_order_relaxed);   
            { std::lock_guard<std::mutex> nl(p->clayNamesLock); up.channelNames = p->channelNames; }   
            up.clientRxUnderrunsTotal = p->clientRxUnderrunsTotal.load(std::memory_order_relaxed);   
            up.clientRxPopsTotal = p->clientRxPopsTotal.load(std::memory_order_relaxed);
            up.isLan = p->isLan;
        }

        // Bytes-out: per-peer HubEndpoint::sentBytes is updated by the static
        // send impl. Sum into hub-level aggregate.
        if (p->persistentEndpoint) {
            auto* ep = reinterpret_cast<HubEndpoint*>(p->persistentEndpoint);
            s.bytesOutTotal += (uint64_t)ep->sentBytes;
            up.bytesOut = (uint64_t)ep->sentBytes;   // Per-player Down
        }
        up.bytesIn = p->bytesIn.load(std::memory_order_relaxed);
        up.lostBlocks = p->blkLostTotal.load(std::memory_order_relaxed);   // Drops column

        // Feed allocations: hold outFeedsLock briefly per peer.
        {
            std::lock_guard<std::mutex> flock(p->outFeedsLock);
            up.outFeeds.reserve(p->outFeeds.size());
            for (auto& kv : p->outFeeds) {
                if (!kv.second) continue;
                UiFeedSnapshot uf;
                uf.senderId = kv.first;
                auto it = idToName.find(kv.first);
                uf.senderUserName = (it != idToName.end()) ? it->second : std::string("?");
                uf.channels = kv.second->channels;
                up.outFeeds.push_back(std::move(uf));
            }
        }

        s.peers.push_back(std::move(up));
    }

    // The per-player latency broadcast (/sb/ping + /sb/hxlat) lives in latencyBroadcastTick
    // (statsWriterThread): it must not depend on anyone polling this snapshot.
    return s;
}

// Called from statsWriterThread every 200 ms (10-s cadence inside), independent of any viewer: clients
// blank their latency display after 35 s without /sb/hxlat, so this must not depend on a snapshot poll.
void SonobusHubServer::latencyBroadcastTick() {
    std::vector<std::shared_ptr<HubPeer>> peers;
    {
        std::shared_lock<std::shared_mutex> lock(mPeerMutex);
        peers.reserve(mPeers.size());
        for (auto& p : mPeers) if (p) peers.push_back(p);
    }
    // Broadcast each player's to/from-hub latency to every other hosted client, at the SB_HUB_LAT_BCAST_S
    // interval, so relayed rows can show "->hub X | hub-> Y ms" instead of "via hub". Control path only
    // (small datagrams on the hub UDP socket, same as [HUB-HELLO]); nothing here touches the audio thread.
    //   toHub   = sink-ping RTT/2 (their uplink one-way) + our inbound sink buffer for them
    //   fromHub = source-ping RTT/2 (hub->them one-way)
    {
        const int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        // Interval is a hub tap: SB_HUB_LAT_BCAST_S, default 10 s, 0 = off. Display-only, control path.
        static const int latBcastS = hub_tap_int("SB_HUB_LAT_BCAST_S", 10);   // 2 s was too chatty; display-only
        if (latBcastS > 0 && nowMs - mLastLatBroadcastMs >= (int64_t) latBcastS * 1000 && mSocket >= 0) {
            mLastLatBroadcastMs = nowMs; ++mLatBroadcastCount;
            // Hub-originated stock /sb/ping to every player (answered with /sb/pngack, handled
            // in handleSonobusControlMessage) — the per-player RTT source that survives rejoins.
            for (auto& q : peers) {
                if (!q || q->addrlen == 0) continue;
                char pbuf[128];
                try {
                    osc::OutboundPacketStream pm(pbuf, sizeof(pbuf));
                    pm << osc::BeginMessage(SB_HXMSG_DOMAIN "/ping") << osc::TimeTag(aoo_osctime_get()) << osc::EndMessage;
                    ::sendto(mSocket, pm.Data(), pm.Size(), 0, (const struct sockaddr*)&q->addr, q->addrlen);
                } catch (const osc::Exception&) {}
            }
            for (auto& q : peers) {
                if (!q) continue;
                const float rttUp = q->smoothedRttUpMs.load(std::memory_order_relaxed);
                const float rttDn = q->smoothedRttMs.load(std::memory_order_relaxed);
                float toHub = 0.0f, fromHub = 0.0f;
                const int32_t lanBlocks = q->lanDepthBlocks.load(std::memory_order_relaxed);
                const double holdMs = (q->isLan && lanBlocks > 0) ? lanBlocks * 1000.0 * (double) mBlockSize / (double) std::max(1, mSampleRate) : q->abufCurrentMs;
                const bool have = computeHubLatency(rttUp, rttDn, holdMs, toHub, fromHub, q->clientRecvBufMs.load(std::memory_order_relaxed));
                const int32_t uplinkLost = (int32_t) q->blkLostTotal.load(std::memory_order_relaxed);
                // 30-s audit line so the per-player legs can be checked from hub.log
                if (mLatBroadcastCount % 15 == 0)   // every 15th broadcast
                    std::cout << "[HUB-LAT] peer='" << q->userName << "' rttUp=" << rttUp << " rttDown=" << rttDn
                              << " buf=" << holdMs << " -> toHub=" << toHub << " fromHub=" << fromHub << (have ? "" : " (no data)") << "\n";
                // The player's OWN receive health as it reported it (see /sb/hxbuf) — every 3rd pass.
                if (mLatBroadcastCount % 3 == 0 && q->clientRxUnderruns10s.load(std::memory_order_relaxed) >= 0)
                    std::cout << "[HUB-RX] peer='" << q->userName << "' underruns10s=" << q->clientRxUnderruns10s.load(std::memory_order_relaxed)
                              << " pops10s=" << q->clientRxPops10s.load(std::memory_order_relaxed)
                              << " xruns10s=" << q->clientXruns10s.load(std::memory_order_relaxed)
                              << " totals=" << q->clientRxUnderrunsTotal.load(std::memory_order_relaxed) << "/" << q->clientRxPopsTotal.load(std::memory_order_relaxed)
                              << " latfeed_age_s=" << q->clientLatFeedAgeS.load(std::memory_order_relaxed)   // -1 = old client / never
                              << " (player-side)\n";
                if (!have) continue;   // no ping data yet
                char lbuf[256];
                try {
                    osc::OutboundPacketStream omsg(lbuf, sizeof(lbuf));
                    const float rttForDisplay = rttDn > 0.0f ? rttDn : rttUp;
                    {   // This player's uplink rate (kbit/s, wire bytes into the hub) since the last broadcast
                        const uint32_t nowMsU = (uint32_t) std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
                        const uint64_t bi = q->bytesIn.load(std::memory_order_relaxed);
                        if (q->latSnapMs != 0 && nowMsU > q->latSnapMs) q->latUpKbps = (float)((double)(bi - q->latBytesInSnap) * 8.0 / (double)(nowMsU - q->latSnapMs));
                        q->latBytesInSnap = bi; q->latSnapMs = nowMsU;
                    }
                    omsg << osc::BeginMessage(SB_HXFULLMSG_LAT)
                         << q->userName.c_str() << toHub << fromHub << uplinkLost   // + blocks the hub lost on THEIR uplink (per player, same for every viewer)
                         << rttForDisplay                                             // Raw round trip, so a player's own header can show the SAME model as everyone's rows
                         << (int32_t) q->isRecording.load(std::memory_order_relaxed) // That player is recording (older clients ignore it)
                         << q->latUpKbps                                              // Their uplink rate, kbit/s (older clients ignore it)
                         << (int32_t) q->isSendMuted.load(std::memory_order_relaxed) // That player muted its send (older clients ignore it)
                         << osc::EndMessage;
                    for (auto& l : peers) {
                        if (!l || l->addrlen == 0) continue;   // A player gets its OWN entry too (header "you")
                        ::sendto(mSocket, omsg.Data(), omsg.Size(), 0,
                                 (const struct sockaddr*)&l->addr, l->addrlen);
                    }
                } catch (const osc::Exception& e) {
                    std::cerr << "[HUB-LAT] build failed: " << e.what() << "\n";
                }
            }
        }
    }
}

// IPv4 CIDR match for per-peer sink-buffer sizing.
// Handshake-time only (called from addPeer under mPeerMutex). IPv6 and parse
// failures classify as NOT-LAN — the safe direction (peer keeps the fat
// WAN-safe buffer).

// Adaptive WAN sinks on the hub: SB_HUB_ADAPT_WAN=1 (env, or a line in the hub's taps file
// SB_TAPS_FILE, hub_taps.conf) puts every WAN peer's inbound sink under the aoo adaptive-depth
// policy (sink.cpp adapt_*: measured arrival jitter + glitch-budget headroom, grow = PLC refill,
// shrink = one splice) instead of the fixed --sink-buffer (12 ms). Tuning via the SB_AOO_ADAPT_*
// taps in the same file. LAN sinks untouched. Read once per process; never on the audio thread.
// Generic hub tap reader: env first, then the hub's taps file (SB_TAPS_FILE).
// Read once per key by the callers (function statics); never on the audio thread's hot path.
static std::string hub_tap_str(const char* name, const char* def){
    if (const char* e = std::getenv(name); e && e[0]) return std::string(e);
    if (const char* tf = std::getenv("SB_TAPS_FILE"); tf && tf[0]){
        if (FILE* f = std::fopen(tf, "r")){
            char line[256]; const size_t n = std::strlen(name); std::string out(def);
            while (std::fgets(line, sizeof(line), f)){
                if (!std::strncmp(line, name, n) && line[n] == '='){ out = line + n + 1; while (!out.empty() && (out.back()=='\n' || out.back()=='\r')) out.pop_back(); break; }
            }
            std::fclose(f); return out;
        }
    }
    return std::string(def);
}
static int hub_tap_int(const char* name, int def){ const std::string v = hub_tap_str(name, ""); return v.empty() ? def : std::atoi(v.c_str()); }

// Tap SB_HUB_HOLD_FLOOR_SETTING: 1 = WAN players start at the setting and it is their floor.
static bool hub_hold_floor_setting(){
    static int cached = -1;
    if (cached < 0) cached = (hub_tap_int("SB_HUB_HOLD_FLOOR_SETTING", 0) == 1) ? 1 : 0;
    return cached == 1;
}
static int hub_adapt_wan_enabled(){
    static int cached = -1;
    if (cached < 0){
        cached = 0;
        if (const char* e = std::getenv("SB_HUB_ADAPT_WAN"); e && e[0]) cached = std::atoi(e) > 0 ? 1 : 0;
        else if (const char* tf = std::getenv("SB_TAPS_FILE"); tf && tf[0]){
            if (FILE* f = std::fopen(tf, "r")){
                char line[256];
                while (std::fgets(line, sizeof(line), f)){
                    if (!std::strncmp(line, "SB_HUB_ADAPT_WAN=", 17)){ cached = std::atoi(line + 17) > 0 ? 1 : 0; break; }
                }
                std::fclose(f);
            }
        }
        std::cout << "[HUB] adaptive WAN sinks: " << (cached ? "ON" : "off") << "\n";
    }
    return cached;
}

// Test switches for the WAN hold and the receive thread (env or hub taps file). Unset = the defaults.
static double hub_tap_double(const char* name, double def){ const std::string v = hub_tap_str(name, ""); return v.empty() ? def : std::atof(v.c_str()); }
// SB_HUB_HOLD_FLOOR_MS: lowest hold a WAN player can get (ms).
static double hub_hold_floor_ms(double def){
    static const double v = std::min(40.0, std::max(1.0, hub_tap_double("SB_HUB_HOLD_FLOOR_MS", def)));
    return v;
}
// SB_HUB_HOLD_MARGIN_MS: safety margin added to the measured arrival gap (ms, on top of one block).
static double hub_hold_margin_ms(){
    static const double v = std::min(10.0, std::max(0.0, hub_tap_double("SB_HUB_HOLD_MARGIN_MS", 1.0)));
    return v;
}
// The WAN hold is set in whole blocks, so it is rounded up once. SB_HUB_HOLD_BLOCKS=0 sets it in whole milliseconds.
static bool hub_hold_blocks(){
    static const bool v = (hub_tap_int("SB_HUB_HOLD_BLOCKS", 1) == 1) && !hub_adapt_wan_enabled();
    return v;
}
// SB_HUB_HOLD_FINE=1: the hold target reads the arrival gaps in 0.25-ms steps instead of whole milliseconds.
static bool hub_hold_fine(){
    static const bool v = (hub_tap_int("SB_HUB_HOLD_FINE", 0) == 1);
    return v;
}
// SB_HUB_HOLD_FOLLOW=1: the hold also grows when the measured target stays a block or more above it for 30 s.
static bool hub_hold_follow(){
    static const bool v = (hub_tap_int("SB_HUB_HOLD_FOLLOW", 0) == 1);
    return v;
}
// One-step return of the hold after a stall. SB_HUB_HOLD_FAST_DECAY=0 keeps only the slow steps.
static bool hub_hold_fast_decay(){
    static const bool v = (hub_tap_int("SB_HUB_HOLD_FAST_DECAY", 1) == 1);
    return v;
}
static int32_t hub_hold_to_blocks(double ms, double blockMs){ return std::max<int32_t>(2, (int32_t) std::ceil(ms / blockMs - 1e-6)); }
// SB_HUB_RX_RT=1: the receive thread asks for real-time scheduling (macOS).
static bool hub_rx_rt(){
    static const bool v = hub_tap_int("SB_HUB_RX_RT", 0) == 1;
    return v;
}

// LAN sinks under the adaptive LAN profile (zero tolerance). Default ON; SB_HUB_ADAPT_LAN=0 (env or hub taps file) = fixed hold.
static int hub_adapt_lan_enabled(){
    static int cached = -1;
    if (cached < 0){ cached = hub_tap_int("SB_HUB_ADAPT_LAN", 1) > 0 ? 1 : 0; std::cout << "[HUB] adaptive LAN sinks (zero tolerance): " << (cached ? "ON" : "off") << "\n"; }
    return cached;
}

static int hub_asrc_enabled(){
    // ASRC defaults off. The linear-interpolation dynamic resampler, applied to every stream against
    // the hub's software clock, is smooth on sines but adds continuous high-frequency grit on music.
    // Without it, clock drift costs a rare buffer correction instead. SB_HUB_ASRC_ON=1 enables it.
    static const int on = [](){ const char* e = getenv("SB_HUB_ASRC_ON");
                                return (e && e[0]=='1') ? 1 : 0; }();
    return on;
}

bool SonobusHubServer::addrMatchesLanSubnet(const struct sockaddr_storage& addr) const {
    if (mLanSubnetCidr.empty()) return false;
    if (addr.ss_family != AF_INET) return false;
    auto slash = mLanSubnetCidr.find('/');
    if (slash == std::string::npos) return false;
    struct in_addr net {};
    if (inet_pton(AF_INET, mLanSubnetCidr.substr(0, slash).c_str(), &net) != 1) return false;
    int prefix = std::atoi(mLanSubnetCidr.substr(slash + 1).c_str());
    if (prefix <= 0 || prefix > 32) return false;
    uint32_t mask = (prefix == 32) ? 0xFFFFFFFFu : ~((1u << (32 - prefix)) - 1u);
    const auto* sin = reinterpret_cast<const struct sockaddr_in*>(&addr);
    return (ntohl(sin->sin_addr.s_addr) & mask) == (ntohl(net.s_addr) & mask);
}

// Build one peer's connected send socket. Control path only (addPeer, exclusive mPeerMutex held):
// allocates a descriptor, logs one line. Mirrors the shared socket's options: same local address/port
// (SO_REUSEADDR + SO_REUSEPORT, bound via getsockname of mSocket so the client keeps seeing the hub's
// known port as the source), the 4 MB SO_RCVBUF (this socket receives everything that peer sends: the
// kernel steers all of X's datagrams to the socket connected to X), DSCP EF + NET_SERVICE_TYPE_VO when
// --dscp is on, O_NONBLOCK. connect() on UDP is local state only (no packet), so it is equally fine for
// NAT'd WAN peers. Any failure returns -1 and that peer keeps the shared sendto path.
// Also called from rebindPeerEndpointLocked (same lock, same thread class) with `prev` = the
// destination being replaced, for the log line only.
int SonobusHubServer::openConnectedSendSocket(const struct sockaddr_storage& peerAddr,
                                              socklen_t peerAddrLen,
                                              const std::string& userName,
                                              const HubDest* prev,
                                              int* errOut) {
    if (errOut) *errOut = 0;
    if (mSocket < 0) return -1;
    if (peerAddr.ss_family != AF_INET || peerAddrLen < (socklen_t)sizeof(struct sockaddr_in)) return -1;
    auto fail = [&](const char* what, int fd) {
        const int e = errno;   // before close() can clobber it
        if (errOut) *errOut = e;
        std::cerr << "[HUB-SOCK] '" << userName << "': " << what << " failed (errno " << e << " " << strerror(e)
                  << ") — this peer uses the shared socket\n";
        if (fd >= 0) ::close(fd);
        return -1;
    };
    const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return fail("socket", -1);
    int one = 1;
    if (::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) != 0
        || ::setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one)) != 0)
        return fail("SO_REUSEADDR/SO_REUSEPORT", fd);
    if (mDscpEnabled) {
        int tos_ef = 0xB8;
        if (::setsockopt(fd, IPPROTO_IP, IP_TOS, &tos_ef, sizeof(tos_ef)) != 0)
            std::cerr << "[HUB-SOCK] '" << userName << "': warning: could not set DSCP EF\n";
#ifdef SO_NET_SERVICE_TYPE
        int st = NET_SERVICE_TYPE_VO;
        ::setsockopt(fd, SOL_SOCKET, SO_NET_SERVICE_TYPE, &st, sizeof(st));
#endif
    }
    {
        int kts = 1;   // Kernel receive timestamps for [HUB-KJIT]
#if defined(SO_TIMESTAMP_MONOTONIC)
        ::setsockopt(fd, SOL_SOCKET, SO_TIMESTAMP_MONOTONIC, &kts, sizeof(kts));
#elif defined(SO_TIMESTAMPNS)
        ::setsockopt(fd, SOL_SOCKET, SO_TIMESTAMPNS, &kts, sizeof(kts));
#endif
    }
    {
        int rcvbuf = 4 * 1024 * 1024;  // same as mSocket: this socket carries the peer's inbound audio
        if (::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf)) < 0)
            std::cerr << "[HUB-SOCK] '" << userName << "': warning: SO_RCVBUF failed: " << strerror(errno) << "\n";
    }
    struct sockaddr_storage local;
    socklen_t localLen = sizeof(local);
    if (::getsockname(mSocket, (struct sockaddr*)&local, &localLen) != 0) return fail("getsockname", fd);
    if (::bind(fd, (struct sockaddr*)&local, localLen) != 0) return fail("bind", fd);
    // A send on a socket the kernel has killed under us (SOF_DEFUNCT / SS_CANTSENDMORE, not a vanished
    // peer, which is ECONNREFUSED/EHOSTUNREACH via ICMP) must return EPIPE, not raise SIGPIPE.
    // healEpipeSockets() replaces such a socket.
    
#ifdef SO_NOSIGPIPE
    { int nsp = 1; ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &nsp, sizeof(nsp)); }
#endif

    if (::connect(fd, (const struct sockaddr*)&peerAddr, sizeof(struct sockaddr_in)) != 0) return fail("connect", fd);
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) return fail("O_NONBLOCK", fd);

    char ipStr[INET_ADDRSTRLEN] = {};
    const auto* sin = reinterpret_cast<const struct sockaddr_in*>(&peerAddr);
    inet_ntop(AF_INET, &sin->sin_addr, ipStr, sizeof(ipStr));
    if (prev) {
        char wasStr[INET_ADDRSTRLEN] = {};
        int wasPort = 0;
        if (prev->addr.ss_family == AF_INET) {
            const auto* was = reinterpret_cast<const struct sockaddr_in*>(&prev->addr);
            inet_ntop(AF_INET, &was->sin_addr, wasStr, sizeof(wasStr));
            wasPort = ntohs(was->sin_port);
        }
        std::cout << "[HUB-SOCK] re-connected send socket fd=" << fd << " for '" << userName
                  << "' -> " << ipStr << ":" << ntohs(sin->sin_port)
                  << " (was " << wasStr << ":" << wasPort << ", old fd=" << prev->connFd << ")\n";
    } else {
        std::cout << "[HUB-SOCK] connected send socket fd=" << fd << " for '" << userName
                  << "' -> " << ipStr << ":" << ntohs(sin->sin_port)
                  << " (bound :" << mLocalPort << ")\n";
    }
    return fd;
}

// A peer's source address moved (NAT rebind on WAN, client re-connect from a new ephemeral port,
// Wi-Fi to Ethernet). The caller has already written peer->addr/addrlen; this makes the peer's
// persistentEndpoint, the target of every feed datagram and AOO reply, follow it.
// Discipline (see HubEndpoint lifetime rule + PeerSnapshot ownership rule):
//   1. build the new HubDest fully (sockaddr + socket()/connect() when the
//      old destination had a connected fd: a 1:1 replacement, so the
//      connected-socket count the recv thread's poll set is sized for never
//      grows; a failed connect() falls back to sendto on the new sockaddr);
//   2. release-store it into ep->dest, the one atomic swap readers see;
//   3. retire the old HubDest with the current snapshot gen k into
//      mRetiredDests; the caller's rebuildPeerSnapshotLocked() bumps to k+1
//      and drainRetiredSnapshots() closes + frees it once every reader has
//      acked past k (and the peer's outFeedsLock has been taken once).
// Never closes anything itself; never called with a send in flight on this
// thread. Control path only: exclusive mPeerMutex, allocates, logs.
// `mode` defaults to IfAddrChanged; ForceReopen / ForceShared take the same swap path for a socket
// the kernel killed under us (healEpipeSockets): same address, new HubDest, old one retired for the
// drain to close. The recv thread's poll set follows at the caller's rebuild, so the dead fd stops
// being polled too.
void SonobusHubServer::rebindPeerEndpointLocked(const std::shared_ptr<HubPeer>& peer, const char* why,
                                                RebindMode mode, int* errOut) {
    if (errOut) *errOut = 0;
    if (!peer || !peer->persistentEndpoint || peer->addrlen <= 0) return;
    auto* ep = reinterpret_cast<HubEndpoint*>(peer->persistentEndpoint);
    HubDest* old = ep->dest.load(std::memory_order_relaxed);   // we are the only writer (exclusive lock)
    if (mode == RebindMode::IfAddrChanged
        && old->addrlen == peer->addrlen
        && memcmp(&old->addr, &peer->addr, (size_t)peer->addrlen) == 0) {
        return;   // same destination: nothing to swap, no socket churn
    }

    auto* nd = new HubDest();
    memcpy(&nd->addr, &peer->addr, sizeof(peer->addr));
    nd->addrlen = peer->addrlen;
    nd->connFd = -1;
    // IfAddrChanged keeps what the peer had (connected stays connected, shared
    // stays shared); ForceReopen always tries a fresh connect()ed socket (the
    // peer is on the shared socket after an EPIPE fallback, old->connFd < 0).
    if (mConnectedSockets && (mode == RebindMode::ForceReopen
                              || (mode == RebindMode::IfAddrChanged && old->connFd >= 0))) {
        nd->connFd = openConnectedSendSocket(nd->addr, nd->addrlen, peer->userName, old, errOut);
    }
    if (mode == RebindMode::ForceReopen && nd->connFd < 0 && old->connFd < 0) {
        delete nd;   // shared -> shared: nothing to swap; the caller logs the errno
        return;
    }
    if (nd->connFd < 0 && mode == RebindMode::IfAddrChanged) {   // forced modes: the caller logs
        char newStr[INET6_ADDRSTRLEN] = {}, wasStr[INET6_ADDRSTRLEN] = {};
        int newPort = 0, wasPort = 0;
        if (nd->addr.ss_family == AF_INET) {
            const auto* sa = reinterpret_cast<const struct sockaddr_in*>(&nd->addr);
            inet_ntop(AF_INET, &sa->sin_addr, newStr, sizeof(newStr)); newPort = ntohs(sa->sin_port);
        }
        if (old->addr.ss_family == AF_INET) {
            const auto* sa = reinterpret_cast<const struct sockaddr_in*>(&old->addr);
            inet_ntop(AF_INET, &sa->sin_addr, wasStr, sizeof(wasStr)); wasPort = ntohs(sa->sin_port);
        }
        std::cout << "[HUB-SOCK] re-targeted send endpoint for '" << peer->userName
                  << "' -> " << newStr << ":" << newPort
                  << " (was " << wasStr << ":" << wasPort << ", shared socket) [" << why << "]\n";
    }

    // The swap. Release pairs with the acquire in hub_send_bare /
    // hub_bundle_same_dest / the recv thread's poll-set rebuild.
    ep->dest.store(nd, std::memory_order_release);

    // Retire the old destination under the gen the caller is about to bump.
    RetiredDest rd;
    rd.gen  = mPeerListGen.load(std::memory_order_relaxed);
    rd.dest = old;
    rd.heap = (old != &ep->dest0);
    rd.peer = peer;
    // The drain decrements after ::close.
    if (old->connFd >= 0) ep->connFdsPendingClose.fetch_add(1, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> rl(mRetiredSnapMutex);
        mRetiredDests.push_back(std::move(rd));
    }
}

// Self-heal for connected send sockets the kernel has killed under us. On Darwin a send() on a UDP
// socket returns EPIPE from exactly one place, sosendcheck(), for exactly two states: SOF_DEFUNCT
// (sodefunct(), e.g. an NECP verdict such as a Local Network privacy re-evaluation) or SS_CANTSENDMORE
// (shutdown()). Neither is transient and neither is about the peer: the socket is dead for the rest of
// its life while the same 4-tuple through a fresh socket (or sendto on the shared socket) works. That
// is the opposite of ECONNREFUSED / EHOSTUNREACH (ICMP echo of a vanished peer, cleared by the read,
// handled by peer-timeout), which this function ignores. A defunct socket is also sodisconnect()ed:
// the peer's inbound audio then falls through to the shared socket (still attributed by byAddr4), and
// the dead fd reads EOF forever; the recv thread flags that as EPIPE too.
//
// Two phases, because an immediate re-connect of the same 4-tuple fails: the dead fd is only retired
// at the swap and stays hashed until the drain's ack rule closes it, so connect() collides, and the
// peer must not wait for that.
//   1. EPIPE seen  -> ForceShared now (the peer keeps working on sendto),
//      reopenPending + a >= kConnReopenDelayMs holdoff.
//   2. later scans -> once every retired connected fd of the peer is closed
//      (connFdsPendingClose == 0) and the holdoff passed: ForceReopen (fresh
//      socket + connect()). Success: connected again. Failure: retry only on
//      collision-class errnos, spaced >= kConnReopenDelayMs, kConnReopenMax
//      attempts in total per peer; a policy errno (EPERM/EACCES/EHOSTUNREACH/
//      ENETUNREACH/EADDRNOTAVAIL) or an exhausted budget = shared for good.
//      A re-opened socket that dies again re-enters phase 1 (same budget).
// statsWriterThread only (~1 s): a shared-lock scan of the per-peer atomics (no allocation, no log
// when healthy); only when there is work, one exclusive section that swaps destinations via
// rebindPeerEndpointLocked and rebuilds the snapshot, the same path as an address change, so the same
// drain rule closes the dead fd after every reader has acked past the swap.
// RT threads keep their one acquire load per send; nothing here runs on them.
void SonobusHubServer::healEpipeSockets() {
    const int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    struct Work { std::shared_ptr<HubPeer> peer; HubDest* dest; bool reopen; };
    std::vector<Work> work;
    {
        std::shared_lock<std::shared_mutex> lock(mPeerMutex);
        for (auto& p : mPeers) {
            if (!p || !p->persistentEndpoint) continue;
            auto* ep = reinterpret_cast<HubEndpoint*>(p->persistentEndpoint);
            HubDest* d = ep->dest.load(std::memory_order_acquire);
            if (d->connFd >= 0) {
                if (ep->lastErrno.load(std::memory_order_relaxed) != EPIPE) continue;
                if (ep->connHealSettle) {
                    // First scan after a swap: an emitter (or the recv thread's
                    // EOF flag) that was still on the OLD fd at the swap may
                    // have stored its EPIPE after we cleared it. Clear once
                    // more and skip; a dead NEW socket fails every send, so it
                    // is caught on the next scan.
                    ep->connHealSettle = false;
                    ep->lastErrno.store(0, std::memory_order_relaxed);
                    continue;
                }
                work.push_back({p, d, false});
            } else if (ep->reopenPending
                       && nowMs >= ep->reopenNotBeforeMs
                       && ep->connFdsPendingClose.load(std::memory_order_acquire) == 0) {
                work.push_back({p, d, true});
            }
        }
    }
    if (work.empty()) return;

    std::lock_guard<std::shared_mutex> lock(mPeerMutex);   // exclusive: swap + rebuild (same as addPeer / removal)
    bool swapped = false;
    for (auto& w : work) {
        bool listed = false;
        for (auto& p : mPeers) { if (p == w.peer) { listed = true; break; } }
        if (!listed) continue;   // removed between the scan and now
        auto* ep = reinterpret_cast<HubEndpoint*>(w.peer->persistentEndpoint);
        if (!ep) continue;
        HubDest* cur = ep->dest.load(std::memory_order_relaxed);
        if (cur != w.dest) continue;   // re-pointed since the scan (addr change / dedupe): re-evaluate next scan
        const std::string& name = w.peer->userName;

        if (!w.reopen) {
            // Phase 1: off the dead socket immediately.
            if (cur->connFd < 0) continue;
            const int deadFd = cur->connFd;
            ep->lastErrno.store(0, std::memory_order_relaxed);
            ep->connHealSettle = true;
            rebindPeerEndpointLocked(w.peer, "EPIPE self-heal", RebindMode::ForceShared);
            ++mSockSharedFallbacks;
            ep->reopenPending = (ep->connReopens < kConnReopenMax);
            ep->reopenNotBeforeMs = nowMs + kConnReopenDelayMs;
            std::cout << "[HUB-SOCK] EPIPE on fd=" << deadFd << " for '" << name
                      << "' -> falling back to shared socket";
            if (ep->reopenPending)
                std::cout << "; re-open in >= " << (kConnReopenDelayMs / 1000) << " s once fd=" << deadFd
                          << " is closed (attempts used " << ep->connReopens << "/" << kConnReopenMax << ")";
            else
                std::cout << "; re-open budget exhausted (" << kConnReopenMax << "/" << kConnReopenMax
                          << "), shared for good";
            std::cout << " (hub re-opens=" << mSockEpipeReopens << " fallbacks=" << mSockSharedFallbacks << ")\n";
            swapped = true;
            continue;
        }

        // Phase 2: the dead fd is closed and the holdoff passed — fresh connect().
        if (cur->connFd >= 0) { ep->reopenPending = false; continue; }   // connected again by another path
        ++ep->connReopens;
        int err = 0;
        rebindPeerEndpointLocked(w.peer, "EPIPE self-heal re-open", RebindMode::ForceReopen, &err);
        const int newFd = ep->dest.load(std::memory_order_relaxed)->connFd;
        if (newFd >= 0) {
            ++mSockEpipeReopens;
            ep->reopenPending = false;
            ep->lastErrno.store(0, std::memory_order_relaxed);
            ep->connHealSettle = true;
            std::cout << "[HUB-SOCK] '" << name << "' -> re-opened connected socket fd=" << newFd
                      << " (attempt " << ep->connReopens << "/" << kConnReopenMax
                      << "; hub re-opens=" << mSockEpipeReopens << " fallbacks=" << mSockSharedFallbacks << ")\n";
            swapped = true;
            continue;
        }
        // No swap happened (shared -> shared is a no-op in rebindPeerEndpointLocked).
        const bool policyErr = (err == EPERM || err == EACCES || err == EHOSTUNREACH
                                || err == ENETUNREACH || err == EADDRNOTAVAIL);
        const bool retry = !policyErr && ep->connReopens < kConnReopenMax;
        std::cout << "[HUB-SOCK] '" << name << "' re-open attempt " << ep->connReopens << "/" << kConnReopenMax
                  << " failed (errno " << err << " " << strerror(err) << ")";
        if (retry) {
            ep->reopenNotBeforeMs = nowMs + kConnReopenDelayMs;
            std::cout << " -> retrying in >= " << (kConnReopenDelayMs / 1000) << " s\n";
        } else {
            ep->reopenPending = false;
            std::cout << " -> staying on the shared socket for good"
                      << (policyErr ? " (policy-class errno)" : " (budget exhausted)") << "\n";
        }
    }
    if (swapped) rebuildPeerSnapshotLocked();   // REQUIRED after a rebind: readers + the recv poll set move to the new dest
}

HubPeer* SonobusHubServer::addPeer(int32_t peerId, const std::string& userName, void* sockaddr) {
    std::unique_lock<std::shared_mutex> lock(mPeerMutex);  // exclusive: mutates mPeers + mPendingFeedReqs (dropped to SHARED after the gen bump, see below)

    // Dedupe by name. With the feedreq self-heal path,
    // an emergency addPeer can race with the normal AOONET_CLIENT_PEER_JOIN_EVENT
    // path; both arrive at addPeer with the same userName but different peerIds.
    // Without dedupe we end up with two HubPeer entries for the same client,
    // which corrupts feed routing. Prefer the existing entry; just update its
    // address from the new sockaddr (the most recent source is most likely
    // current).
    for (auto& existing : mPeers) {
        if (existing && existing->userName == userName) {
            if (sockaddr) {
                const struct sockaddr* sa = (const struct sockaddr*)sockaddr;
                if (sa->sa_family == AF_INET) {
                    memcpy(&existing->addr, sa, sizeof(struct sockaddr_in));
                    existing->addrlen = sizeof(struct sockaddr_in);
                } else if (sa->sa_family == AF_INET6) {
                    memcpy(&existing->addr, sa, sizeof(struct sockaddr_in6));
                    existing->addrlen = sizeof(struct sockaddr_in6);
                }
            }
            existing->active = true;
            rebindPeerEndpointLocked(existing, "addPeer dedupe");   // Endpoint (feeds + connected fd) follows
            rebuildPeerSnapshotLocked();  // Addr may have changed — REQUIRED right after the rebind
            std::cout << "[HUB] addPeer dedupe: '" << userName
                      << "' already present (id=" << existing->id
                      << "), refreshed addr\n";
            return existing.get();
        }
    }

    auto peer = std::make_shared<HubPeer>();
    peer->id = peerId;
    peer->userName = userName;
    mStageMembersGen.fetch_add(1, std::memory_order_relaxed);
    peer->recvChannels = mChannels;
    peer->uiRecvChannels.store(mChannels, std::memory_order_relaxed);
    peer->sendChannels = mChannels;
    peer->recvBuffer.resize(mBlockSize * mChannels, 0.0f);

    // Copy address
    char peerAddrStr[INET6_ADDRSTRLEN] = {};
    int peerPort = 0;
    if (sockaddr) {
        const struct sockaddr* sa = (const struct sockaddr*)sockaddr;
        if (sa->sa_family == AF_INET) {
            memcpy(&peer->addr, sa, sizeof(struct sockaddr_in));
            peer->addrlen = sizeof(struct sockaddr_in);
            auto* sin = (const struct sockaddr_in*)sa;
            inet_ntop(AF_INET, &sin->sin_addr, peerAddrStr, sizeof(peerAddrStr));
            peerPort = ntohs(sin->sin_port);
        } else if (sa->sa_family == AF_INET6) {
            memcpy(&peer->addr, sa, sizeof(struct sockaddr_in6));
            peer->addrlen = sizeof(struct sockaddr_in6);
            auto* sin6 = (const struct sockaddr_in6*)sa;
            inet_ntop(AF_INET6, &sin6->sin6_addr, peerAddrStr, sizeof(peerAddrStr));
            peerPort = ntohs(sin6->sin6_port);
        }
    }
    // Classify LAN vs WAN once, at handshake.
    peer->isLan = addrMatchesLanSubnet(peer->addr);
    connLog("peer-audio", std::string(peerAddrStr) + ":" + std::to_string(peerPort), userName, mGroupName, peer->isLan ? "LAN" : "WAN");   
    recentNoteJoin(userName, peerAddrStr, peer->isLan);   // Recent-players list
    std::cout << "[HUB] Peer '" << userName << "' address: " << peerAddrStr << ":" << peerPort
              << (peer->isLan ? " [LAN" : " [WAN")
              << ", sinkBuf " << ((peer->isLan || hub_hold_floor_setting()) ? sinkBufferMsFor(peer.get()) : hub_hold_floor_ms(kHoldFloorMs)) << "ms, evlog s=" << sinkIdForPeer(peerId) << "]\n";   // Maps hub_sinkev.log entries to this player

    int32_t sinkId = sinkIdForPeer(peerId);
    int32_t sourceId = sourceIdForPeer(peerId);

    // Create sink (receives audio FROM this peer).
    // Initial channel count is HUB_DEFAULT_CHANNELS; the real value comes from
    // AOO_SOURCE_FORMAT_EVENT once the sender announces its format. Sink will
    // be re-setup'd (under peer->sinkLock) when that event fires.
    peer->recvChannels = HUB_DEFAULT_CHANNELS;
    peer->uiRecvChannels.store(HUB_DEFAULT_CHANNELS, std::memory_order_relaxed);
    peer->sink.reset(aoo::isink::create(sinkId));
    peer->sink->setup(mSampleRate, mBlockSize, peer->recvChannels);
    {   // No resend requests from the hub's inbound sinks — at a 1.33/12 ms hold a
        // resent block can never arrive in time, and the requests pile on exactly during loss.
        int32_t noresend = 0;
        peer->sink->set_option(aoo_opt_resend_limit, &noresend, sizeof(int32_t));
    }
    // Per-peer LAN/WAN sizing, applied here before audio flows.
    {
        double initialMs = sinkBufferMsFor(peer.get());
        // WAN players START AT THE LOWEST hold and are scaled up only if measured necessary.
        if (!peer->isLan && !hub_hold_floor_setting()) initialMs = hub_hold_floor_ms(kHoldFloorMs);
        peer->abufCurrentMs = initialMs;
        // WAN players get their own hold committed by the stats thread after kHoldStartMeasureS of
        // measurement. LAN players: fixed.
        peer->abufCommitted = peer->isLan;
        peer->holdMeasureUntilMs = (int64_t) std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count() + (int64_t)(kHoldStartMeasureS * 1000.0);
        // WAN peers under the adaptive sink (the ms below becomes the
        // first/only rebuild; later ms sets are ignored by the sink once adaptive).
        if (!peer->isLan && hub_adapt_wan_enabled()){
            int32_t one = 1;
            peer->sink->set_option(aoo_opt_adapt, &one, sizeof(int32_t));
            std::cout << "[HUB-ADAPT] peer='" << userName << "' WAN sink adaptive (fixed " << initialMs << " ms bypassed)\n";
        }
        // LAN inbound sinks under the aoo LAN profile (2) = zero tolerance in sink.cpp (hold = worst gap +
        // 1 block, grow-only, no decay). The fixed --sink-buffer-lan (2 blocks) is a hold with no margin, so
        // each arrival stall at that depth becomes a silence hole forwarded to everyone.
        // SB_HUB_ADAPT_LAN=0 restores the fixed hold.
        if (peer->isLan && hub_adapt_lan_enabled()){
            int32_t two = 2;
            peer->sink->set_option(aoo_opt_adapt, &two, sizeof(int32_t));
            std::cout << "[HUB-ADAPT] peer='" << userName << "' LAN sink adaptive, zero tolerance (starts at " << initialMs << " ms)\n";
        }
        peer->sink->set_buffersize((int32_t)initialMs);
        if (!peer->isLan && hub_hold_blocks()){
            const double blockMs = 1000.0 * (double)mBlockSize / (double)std::max(1, mSampleRate);
            int32_t nb = hub_hold_to_blocks(initialMs, blockMs);
            peer->sink->set_option(aoo_opt_iso_blocks, &nb, sizeof(int32_t));
            peer->abufCurrentMs = nb * blockMs;
        }
    }
    // ASRC re-clocks the sender's audio onto the hub's clock at the sink. With every peer running on its
    // own crystal (or worse, the system clock when no hardware interface is attached), per-pair drift
    // accumulates over minutes and is heard as warble. Enabling at the sink localizes the drift
    // compensation to one place (hub) instead of N² listener-side relationships. Cost: ~21µs added
    // latency per sink + some CPU for the ASRC computation.
    peer->sink->set_dynamic_resampling(hub_asrc_enabled());

    int32_t flags = AOO_PROTOCOL_FLAG_COMPACT_DATA | (hubIdleMaskEnabled() ? AOO_PROTOCOL_FLAG_IDLE_MASK : 0);   // We decode idle-masked blocks
    peer->sink->set_option(aoo_opt_protocol_flags, &flags, sizeof(int32_t));

    // No per-peer outbound source is allocated here. Listener clients request feeds via /sb/hxfeedreq;
    // the hub allocates per-(listener, sender) sources on demand. See handleSonobusControlMessage and
    // allocateOrUpdateFeed.

    // Create a persistent endpoint owned by the peer. AOO identifies sinks/sources
    // by endpoint pointer, so every add_sink / set_sinkoption for this peer must
    // use this same pointer.
    auto* ep = new HubEndpoint();
    ep->socketFd = mSocket;
    memcpy(&ep->dest0.addr, &peer->addr, sizeof(peer->addr));   // Initial destination, inline
    ep->dest0.addrlen = peer->addrlen;
    peer->persistentEndpoint = ep;
    // The listener's bundle builder is bound
    // to its persistent endpoint here, once — the pacer never allocates.
    peer->outBundle.init(ep, mBundleMtu);
    // Give the peer its own
    // connect()ed send socket NOW — handshake path, under exclusive
    // mPeerMutex, before the peer is pushed to mPeers / published in a
    // snapshot, so connFd is final before any reader can see the peer (the
    // invite below is already the first send through it). Every outbound
    // datagram to this peer (feeds, invites, pings, format) benefits, not
    // only the listener feeds. IPv4 only; anything else keeps sendto.
    // Bounded to HUB_MAX_PEERS holders: the recv thread's preallocated poll
    // set must cover EVERY connected socket, because the kernel steers ALL
    // of peer X's inbound datagrams to the socket connected to X — a socket
    // we did not poll would silently swallow that peer's audio.
    if (mConnectedSockets && (int)mPeers.size() < HUB_MAX_PEERS) {
        // Plain write into dest0: the peer is not yet reachable by anyone,
        // and its publication (mPeers push + snapshot, under this lock)
        // orders it for every reader.
        ep->dest0.connFd = openConnectedSendSocket(peer->addr, peer->addrlen, userName);
    }

    // Invite the peer's source (upstream from peer to hub). Client convention
    // is that its outbound oursource is at the same id as the hub's outbound
    // sink for this peer — kSbClientOurId=1 (the SonoBus first-remote-peer
    // convention). We invite source #1 so the peer's audio starts flowing
    // toward our sink immediately on AOO handshake.
    {
        const int32_t kSbClientOurId = 1;
        peer->sink->invite_source(ep, kSbClientOurId,
            (aoo_replyfn)hub_endpoint_send_impl);
    }

    peer->active = true;
    peer->joinedAtMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    // Pre-allocate the per-peer decoded buffer +
    // pointer table so the audio thread never allocates during sink loop.
    peer->decodedBlock.assign((size_t)mBlockSize * HUB_MAX_CHANNELS_PER_PEER, 0.0f);
    peer->decodedPtrs.resize(HUB_MAX_CHANNELS_PER_PEER);
    for (int slot = 0; slot < 2; ++slot) {   // Torn-block fix
        peer->decodedBlockSlots[slot].assign((size_t)mBlockSize * HUB_MAX_CHANNELS_PER_PEER, 0.0f);
        peer->decodedPtrSlots[slot].resize(HUB_MAX_CHANNELS_PER_PEER);
        for (int ch = 0; ch < HUB_MAX_CHANNELS_PER_PEER; ++ch)
            peer->decodedPtrSlots[slot][ch] = peer->decodedBlockSlots[slot].data() + (size_t)ch * mBlockSize;
    }
    for (int ch = 0; ch < HUB_MAX_CHANNELS_PER_PEER; ++ch) {
        peer->decodedPtrs[ch] = peer->decodedBlock.data() + (size_t)ch * mBlockSize;
    }
    peer->decodedChannels = peer->recvChannels;

    // Launch this peer's per-listener feed-pump
    // thread. Joined by ~HubPeer when the shared_ptr drops. Must happen
    // BEFORE the peer is pushed to mPeers — the listener thread looks
    // itself up in mPeers on entry, but the audio thread also looks up
    // peers from mPeers for the fan-out signal. Order matters: do the
    // push_back below FIRST, then start the thread (so it can find itself).
    // (Pushed below; thread started after.)


    HubPeer* ret = peer.get();
    mPeers.push_back(peer);
    rebuildPeerSnapshotLocked();  // Publish new membership
    // The gen bump above makes the audio and recv threads take mPeerMutex SHARED on their next block.
    // Everything below (listener-thread spawn, HELLO sendto + cout, replaying stashed feed requests) must
    // not run under this EXCLUSIVE lock, or every listener's audio thread stalls at each join. Take this
    // sender's stashed requests out under the exclusive lock, then drop to SHARED.
    std::vector<PendingFeedReq> stashedForMe;
    for (auto it = mPendingFeedReqs.begin(); it != mPendingFeedReqs.end();) {
        if (it->senderName == userName) { stashedForMe.push_back(*it); it = mPendingFeedReqs.erase(it); }
        else ++it;
    }
    lock.unlock();
    std::shared_lock<std::shared_mutex> slock(mPeerMutex);

    // Start the per-listener thread now. The thread receives its own shared_ptr directly (no self-lookup
    // under mPeerMutex, see listenerProcessThread). listenerAlive is set before the thread exists and
    // before this peer can appear in an audio-thread snapshot (we hold mPeerMutex exclusive), so the audio
    // thread's alive-check under listenerMutex is well-defined from the first block.
    // Under --inline-listeners no thread is started: the audio thread pumps this listener itself.
    // listenerAlive stays false, so the threaded Step-2 path (used again only after
    // inlineListenersFallback(), which starts the missing threads under this same exclusive mPeerMutex
    // before clearing mInlineActive) never counts a thread that does not exist.
    if (!mInlineActive.load(std::memory_order_acquire)) {
        ret->listenerThreadShouldStop.store(false, std::memory_order_release);
        ret->listenerAlive.store(true, std::memory_order_release);
        ret->listenerThread = std::make_unique<std::thread>(
            [this, sp = std::move(peer)]() mutable { listenerProcessThread(std::move(sp)); });
    }

    // Tell this freshly-joined peer about us. If their AOO PEER_JOIN handler missed our peer entry, this
    // lets them force-add the hub peer into their remote list and start sending audio. Fires once per
    // peer-join. No steady-state cost: this addPeer codepath only runs at handshake.
    if (ret->addrlen > 0) {
        char hbuf[256];
        try {
            osc::OutboundPacketStream omsg(hbuf, sizeof(hbuf));
            // Args 3+4 = the address players should share, so a group link does not go via go.sonobus.net with
            // the copier's LAN path: the public host (settings override, else STUN) + the AOO server port.
            // Older clients read the first two args only.
            const std::string linkHost = !mPublicAddressHost.empty() ? mPublicAddressHost : mAdvertisedPublicHost;
            omsg << osc::BeginMessage(SB_HXFULLMSG_HUBHELLO)
                 << mHubUserName.c_str()
                 << (int32_t)mLocalPort
                 << linkHost.c_str()
                 << (int32_t)(mRunLocalServer ? mLocalServerPort : 0)
                 // Args 5+6 = the hub block size + sample rate,
                 // so a player can grey out device buffer sizes below the hub block ("hub minimum").
                 << (int32_t)mBlockSize
                 << (int32_t)mSampleRate
                 << osc::EndMessage;
            ::sendto(mSocket, omsg.Data(), omsg.Size(), 0,
                     (const struct sockaddr*)&ret->addr, ret->addrlen);
            std::cout << "[HUB-HELLO] sent to '" << userName
                      << "' (audioPort=" << mLocalPort
                      << ", hubUser=" << mHubUserName << ")\n";
        } catch (const osc::Exception& e) {
            std::cerr << "[HUB-HELLO] build failed: " << e.what() << "\n";
        }
    }

    // Drain pending feedreqs whose sender == this peer.
    // Pre-conditions: lock held (we're inside addPeer's mPeerMutex scope).
    // Replays remove from mPendingFeedReqs in-place.
    for (auto& req : stashedForMe) {   // Under the SHARED lock (allocateOrUpdateFeed needs peers stable, not exclusive)
        HubPeer* listener = nullptr;
        for (auto& p : mPeers) {
            if (p && p->id == req.listenerPeerId) { listener = p.get(); break; }
        }
        if (listener) {
            std::cout << "[HUB-FEED] replaying stashed feedreq listener='"
                      << listener->userName << "' sender='" << userName
                      << "' ourId=" << req.clientOurId << "\n";
            allocateOrUpdateFeed(listener, ret, req.clientOurId);
        }
    }
    return ret;
}

void SonobusHubServer::removePeer(int32_t peerId) {
    // Retain the removed peers past the erase so we
    // can stop+join their listener threads AFTER releasing mPeerMutex (the
    // listener thread takes mPeerMutex shared at startup to find itself —
    // joining while holding it exclusively could deadlock on a remove-
    // right-after-add race). See HubPeer::stopAndJoinListenerThread.
    std::vector<std::shared_ptr<HubPeer>> removed;
    std::vector<std::unique_ptr<HubOutFeed>> graveyard;   // (stale feed slots)
    {
        std::lock_guard<std::shared_mutex> lock(mPeerMutex);  // exclusive: mutates mPeers

        // Collect the departing peer(s) before remove_if: it move-assigns the survivors down over the matched
        // slot, leaving moved-from (null) shared_ptrs in [it, end), so a later walk of that range finds nothing.
        for (auto& p : mPeers) if (p && p->id == peerId) removed.push_back(p);
        auto it = std::remove_if(mPeers.begin(), mPeers.end(),
                                 [peerId](const std::shared_ptr<HubPeer>& p) {
                                     return p && p->id == peerId;
                                 });

        if (it != mPeers.end()) {
            for (auto i = removed.begin(); i != removed.end(); ++i) {
                if (!*i) continue;
                std::cout << "[HUB] Removed peer '" << (*i)->userName << "' (id=" << (*i)->id << ")\n";
                mStageMembersGen.fetch_add(1, std::memory_order_relaxed);
                connLog("peer-removed", "", (*i)->userName, mGroupName, "");   
                recentNoteLeave((*i)->userName);   // Recent-players list

                // Purge any buffered invites that match this leaving peer's
                // address. Otherwise stale invites accumulate across reconnect
                // cycles (clients get fresh ephemeral UDP ports each time, so
                // old buffer entries can never match a future peer).
                std::lock_guard<std::mutex> il(mPendingInvitesMutex);
                const auto* pa = (const struct sockaddr_in*)&(*i)->addr;
                auto pi_it = mPendingInvites.begin();
                while (pi_it != mPendingInvites.end()) {
                    const auto* ea = (const struct sockaddr_in*)&pi_it->addr;
                    if (pa->sin_family == AF_INET && ea->sin_family == AF_INET
                        && pa->sin_addr.s_addr == ea->sin_addr.s_addr) {
                        pi_it = mPendingInvites.erase(pi_it);
                    } else {
                        ++pi_it;
                    }
                }
            }
            // AOO teardown (uninvite_all / source->stop / delete persistentEndpoint) runs in ~HubPeer when the
            // last shared_ptr drops; the audio thread's snapshot may briefly hold the last reference, and teardown
            // defers to its next snapshot rebuild.
            //
            // The connect()ed send socket cannot wait for that. A client keeps its UDP socket across
            // disconnect/connect, so it can rejoin from the same port, and XNU would steer its handshake pings
            // into the dead fd (SO_REUSEPORT: a connected socket wins the 4-tuple over the shared listener).
            // Whoever still holds the HubPeer, dissolve the association now (connect(AF_UNSPEC)): the fd stays
            // open for any in-flight send() (which fails harmlessly with EDESTADDRREQ/ENOTCONN for a peer that is
            // gone) but no longer captures inbound datagrams, so the rejoin handshake reaches the shared socket.
            for (auto& rp : removed) {
                if (!rp || !rp->persistentEndpoint) continue;
                auto* ep = reinterpret_cast<HubEndpoint*>(rp->persistentEndpoint);
                HubDest* d = ep->dest.load(std::memory_order_acquire);
                if (d && d->connFd >= 0) {
                    // macOS: connect(AF_UNSPEC) returns EAFNOSUPPORT for UDP; disconnectx(2) is the XNU way.
                    const int rc = hubDissolveUdp(d->connFd);   // Portable (disconnectx on macOS, connect(AF_UNSPEC) on Linux)
                    std::cout << "[HUB-SOCK] dissolved send socket fd=" << d->connFd << " for departed '"
                              << rp->userName << "' (rc=" << rc << (rc != 0 ? std::string(" ") + strerror(errno) : std::string(""))
                              << ") so a rejoin from the same port is not steered into it\n";
                }
            }
            // ... and the peer's RETIRED (rebound-away) destinations, which are still connect()ed to
            // earlier ports of the same client until the drain closes them.
            {
                std::lock_guard<std::mutex> rl(mRetiredSnapMutex);
                for (auto& rd : mRetiredDests) {
                    bool mine = false;
                    for (auto& rp : removed) if (rd.peer && rp && rd.peer.get() == rp.get()) mine = true;
                    if (mine && rd.dest && rd.dest->connFd >= 0) {
                        const int rc = hubDissolveUdp(rd.dest->connFd);
                        std::cout << "[HUB-SOCK] dissolved retired send socket fd=" << rd.dest->connFd
                                  << " for departed '" << rd.peer->userName << "' (rc=" << rc << ")\n";
                    }
                }
            }
            mPeers.erase(it, mPeers.end());
            rebuildPeerSnapshotLocked();  // (F10)
            // Unlink every remaining listener's outFeeds[peerId] now, while we still hold the exclusive lock.
            // A ghost slot keyed by a dead sender id would let a rejoin (new id, same names, same deterministic
            // hubSourceId) give the listener two sources aimed at one sink id. Destruction is deferred to
            // `graveyard` below, outside every lock.
            graveyard = unlinkFeedsForDepartedPeerLocked(peerId);
        }
    }
    // Outside the lock: end the ownership cycle. The listener thread holds a strong self ref, and only
    // this explicit stop lets the refcount reach zero; otherwise each removed peer leaks one RT thread.
    for (auto& p : removed) {
        p->stopAndJoinListenerThread();
    }
    // AOO teardown of the unlinked feeds
    // here — no lock held, control thread. Nothing can reach them any more
    // (every reader walks outFeeds under outFeedsLock, and they left the map
    // under it), so stop()/remove_all()/free never contend with the pacer,
    // listener or audio threads.
    retireFeeds(graveyard, peerId);
    // Control-path GC pass, outside every
    // lock. Frees whatever retired snapshots all readers have already moved
    // past; anything still in use waits for the stats thread's next 200 ms
    // pass. `removed` drops right after — if that is the last ref, ~HubPeer
    // runs here on this control thread, never on the pacer.
    drainRetiredSnapshots(false);
}

// Unlink outFeeds[departedId] from every
// peer still in mPeers and drop pending feedreqs that named the departed peer
// as listener. Caller holds mPeerMutex EXCLUSIVE; this takes each remaining
// peer's outFeedsLock in turn (mPeerMutex -> outFeedsLock, the established
// order — never the reverse). The lock hold per listener is a map find +
// node move-out: no AOO call, no free. Returned feeds are unreachable
// (every reader walks outFeeds under outFeedsLock) and are destroyed by
// retireFeeds() after the caller has released every lock.
std::vector<std::unique_ptr<HubOutFeed>>
SonobusHubServer::unlinkFeedsForDepartedPeerLocked(int32_t departedId) {
    std::vector<std::unique_ptr<HubOutFeed>> unlinked;
    for (auto& other : mPeers) {
        if (!other) continue;
        std::lock_guard<std::mutex> ofl(other->outFeedsLock);
        auto it = other->outFeeds.find(departedId);
        if (it == other->outFeeds.end()) continue;
        if (it->second) unlinked.push_back(std::move(it->second));
        other->outFeeds.erase(it);
    }
    auto pit = mPendingFeedReqs.begin();
    while (pit != mPendingFeedReqs.end()) {
        if (pit->listenerPeerId == departedId) pit = mPendingFeedReqs.erase(pit);
        else ++pit;
    }
    return unlinked;
}

// AOO teardown + free of feeds already
// unlinked from their maps. Control threads only, NO lock held by the caller.
void SonobusHubServer::retireFeeds(std::vector<std::unique_ptr<HubOutFeed>>& feeds,
                                   int32_t departedId) {
    if (feeds.empty()) return;
    for (auto& f : feeds) {
        if (f && f->source) {
            f->source->stop();
            f->source->remove_all();
        }
    }
    std::cout << "[HUB] stale-feed GC: removed " << feeds.size()
              << " outFeed entries pointing at departed sender id=" << departedId << "\n";
    feeds.clear();   // ~HubOutFeed → ~aoo::source, here, outside every lock
}

// Rebuild + publish the immutable peer snapshot.
// Caller MUST hold mPeerMutex exclusively (every mPeers / HubPeer::addr
// writer already does). Allocates — that is fine: every caller is a control
// path (handshake, leave, self-heal, shutdown), never an RT thread. The gen
// bump is a release store so a reader that observes the new gen (acquire)
// and then takes mPeerMutex shared sees this snapshot pointer.
void SonobusHubServer::rebuildPeerSnapshotLocked() {
    auto snap = std::make_shared<PeerSnapshot>();
    snap->gen = mPeerListGen.load(std::memory_order_relaxed) + 1;   // the gen published below
    snap->peers.reserve(mPeers.size());
    snap->pacerPeers.reserve(mPeers.size());
    for (auto& p : mPeers) {
        if (!p) continue;
        const int idx = (int)snap->peers.size();
        snap->peers.push_back(p);
        const auto* pa = (const struct sockaddr_in*)&p->addr;
        if (pa->sin_family == AF_INET) {
            snap->byAddr4.emplace(addrKey4(pa), idx);   // emplace: first match wins, as the old scan did
        }
        // Pacer view = active peers, mPeers order
        if (p->active) snap->pacerPeers.push_back(p.get());
    }
    // Publish the pacer pointer BEFORE the gen
    // bump (pacer does gen load → ptr load, so a new gen always finds the
    // new pointer); retire the previous snapshot AFTER the bump, under
    // mRetiredSnapMutex — that mutex then orders the bump before any
    // drain's ack read, which is the Dekker pairing the pacer's claim /
    // re-sync relies on (see pacerThread). The retired snapshot keeps
    // owning its peers until drainRetiredSnapshots() frees it off-RT.
    std::shared_ptr<const PeerSnapshot> retired = std::move(mPeerSnapshot);
    mPeerSnapshot = std::move(snap);
    mPacerSnapPtr.store(mPeerSnapshot.get(), std::memory_order_release);
    mPeerListGen.fetch_add(1, std::memory_order_seq_cst);
    if (retired) {
        std::lock_guard<std::mutex> rl(mRetiredSnapMutex);
        mRetiredSnaps.push_back(std::move(retired));
    }
}

// Free retired snapshots whose gen EVERY
// reader has moved past (see the PeerSnapshot ownership rule in the header).
// force=true (stop(): all reader threads already joined) frees everything.
// The destructors run AFTER mRetiredSnapMutex is released, on the CALLING
// thread — statsWriterThread, a peer-removal control path, or stop().
// Never call from an RT thread; never call while holding mPeerMutex.
// The same pass frees retired
// peer DESTINATIONS (mRetiredDests) under the same ack rule, then — outside
// mRetiredSnapMutex — takes and releases the owning peer's outFeedsLock once
// (the cut-through listener thread emits under that lock without acking a
// gen; a control path taking outFeedsLock briefly is the established
// pattern, see unlinkFeedsForDepartedPeerLocked) before close()ing the old
// connected fd and deleting the HubDest. Lock order here: mRetiredSnapMutex
// alone, then outFeedsLock alone — never nested, never under mPeerMutex.
void SonobusHubServer::drainRetiredSnapshots(bool force) {
    std::vector<std::shared_ptr<const PeerSnapshot>> freeable;
    std::vector<RetiredDest> freeableDests;
    {
        std::lock_guard<std::mutex> rl(mRetiredSnapMutex);
        if (mRetiredSnaps.empty() && mRetiredDests.empty()) return;
        // seq_cst: pairs with the readers' seq_cst claim stores (pacer), so
        // a reader busy with gen k is either seen here as busy, or has
        // itself seen the newer mPeerListGen and re-synced away from k.
        std::atomic_thread_fence(std::memory_order_seq_cst);
        const uint64_t pacerAck = mPacerSnapAck.load(std::memory_order_seq_cst);
        const uint64_t audioAck = mAudioSnapAck.load(std::memory_order_seq_cst);
        const uint64_t recvAck  = mRecvSnapAck.load(std::memory_order_seq_cst);
        for (auto it = mRetiredSnaps.begin(); it != mRetiredSnaps.end();) {
            const uint64_t k = (*it)->gen;
            // kSnapAckNone (= max) means "holds nothing" → always past k.
            if (force || (pacerAck > k && audioAck > k && recvAck > k)) {
                freeable.push_back(std::move(*it));
                it = mRetiredSnaps.erase(it);
            } else {
                ++it;
            }
        }
        for (auto it = mRetiredDests.begin(); it != mRetiredDests.end();) {
            const uint64_t k = it->gen;
            if (force || (pacerAck > k && audioAck > k && recvAck > k)) {
                freeableDests.push_back(std::move(*it));
                it = mRetiredDests.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (auto& rd : freeableDests) {
        if (rd.peer) {
            // Every emitter of this peer's feeds holds peer->outFeedsLock
            // across its send (listener thread / inline audio / pacer slot);
            // once we have held it, no emitter can still be inside a send
            // that loaded the old pointer, and its next acquire of the
            // mutex sees the swap. Held for zero work.
            std::lock_guard<std::mutex> ofl(rd.peer->outFeedsLock);
        }
        if (rd.dest) {
            if (rd.dest->connFd >= 0) {
                std::cout << "[HUB-SOCK] closed retired send socket fd=" << rd.dest->connFd
                          << " for '" << (rd.peer ? rd.peer->userName : std::string("?")) << "'\n";
                ::close(rd.dest->connFd);
                rd.dest->connFd = -1;
                // The 4-tuple is free
                // again (pcb detached) — healEpipeSockets may re-connect now.
                // rd.peer pins the HubPeer, which owns the endpoint.
                if (rd.peer && rd.peer->persistentEndpoint)
                    reinterpret_cast<HubEndpoint*>(rd.peer->persistentEndpoint)
                        ->connFdsPendingClose.fetch_sub(1, std::memory_order_release);
            }
            if (rd.heap) delete rd.dest;   // dest0 stays inside its HubEndpoint
        }
    }
    // `freeable` / `freeableDests` destruct here: ~PeerSnapshot → drops its
    // peer refs → possibly ~HubPeer — on this non-RT thread, outside every lock.
}

void SonobusHubServer::handleSonobusControlMessage(const char* msg, int32_t n,
                                                   const struct sockaddr_storage* srcAddr,
                                                   socklen_t /*srcAddrLen*/) {
    // OSC pattern check: must start with "/sb/" and then a known hx* tail.
    if (n < SB_HXMSG_DOMAIN_LEN + SB_HXMSG_MUTE_LEN
        || std::memcmp(msg, SB_HXMSG_DOMAIN, SB_HXMSG_DOMAIN_LEN) != 0) {
        return;
    }
    const char* tail = msg + SB_HXMSG_DOMAIN_LEN;
    // Port-forward self-test messages (see PORT CHECK).
    if (n >= SB_HXMSG_DOMAIN_LEN + 9 && std::memcmp(tail, "/fwprobe/", 9) == 0) {
        try {
            osc::ReceivedPacket pk(msg, (osc::osc_bundle_element_size_t)n);
            if (!pk.IsMessage()) return;
            osc::ReceivedMessage m(pk);
            auto it = m.ArgumentsBegin();
            if (std::strcmp(m.AddressPattern(), "/sb/fwprobe/req") == 0) {
                const int64_t token = it->AsInt64(); ++it;
                const int32_t up = it->AsInt32(); ++it;
                const int32_t sp = it->AsInt32();
                handlePortProbeRequest(srcAddr, token, up, sp);
            } else if (std::strcmp(m.AddressPattern(), "/sb/fwprobe/ping") == 0) {
                if (it->AsInt64() == mPortCheckToken.load(std::memory_order_relaxed))
                    mPortProbeAudioHits.fetch_add(1, std::memory_order_relaxed);
            } else if (std::strcmp(m.AddressPattern(), "/sb/fwprobe/result") == 0) {
                // A connected player answered our probe request (reply comes to the hub socket)
                const int64_t token = it->AsInt64(); ++it;
                if (token == mPortCheckToken.load(std::memory_order_relaxed)) {
                    mPortProbeResultTcp.store(it->AsInt32(), std::memory_order_relaxed);
                    mPortProbeResultToken.store(token, std::memory_order_release);
                }
            }
        } catch (...) {}
        return;
    }
    bool isMute    = std::memcmp(tail, SB_HXMSG_MUTE, SB_HXMSG_MUTE_LEN) == 0;
    bool isGain    = !isMute && std::memcmp(tail, SB_HXMSG_GAIN, SB_HXMSG_GAIN_LEN) == 0;
    bool isSolo    = !isMute && !isGain
        && (n >= SB_HXMSG_DOMAIN_LEN + SB_HXMSG_SOLO_LEN)
        && std::memcmp(tail, SB_HXMSG_SOLO, SB_HXMSG_SOLO_LEN) == 0;
    bool isFeedreq = !isMute && !isGain && !isSolo
        && (n >= SB_HXMSG_DOMAIN_LEN + SB_HXMSG_FEEDREQ_LEN)
        && std::memcmp(tail, SB_HXMSG_FEEDREQ, SB_HXMSG_FEEDREQ_LEN) == 0;
    bool isClayinfo = !isMute && !isGain && !isSolo && !isFeedreq
        && (n >= SB_HXMSG_DOMAIN_LEN + SB_HXMSG_CLAYINFO_LEN)
        && std::memcmp(tail, SB_HXMSG_CLAYINFO, SB_HXMSG_CLAYINFO_LEN) == 0;
    // Recognize /sb/chat.
    bool isChat = !isMute && !isGain && !isSolo && !isFeedreq && !isClayinfo
        && (n >= SB_HXMSG_DOMAIN_LEN + SB_HXMSG_CHAT_LEN)
        && std::memcmp(tail, SB_HXMSG_CHAT, SB_HXMSG_CHAT_LEN) == 0;
    // /sb/pngack = reply to the hub's own /sb/ping (see latencyBroadcastTick). AOO source pings can stop
    // attaching after a listener rejoins, so the hub measures each player's RTT itself with the stock
    // SonoBus ping, which every client already answers. Args: t:origtime t:theirtime. Stored in
    // smoothedRttMs (used for both legs when the sink-side ping is absent). Control path only.
    bool isPngack = !isMute && !isGain && !isSolo && !isFeedreq && !isClayinfo && !isChat
        && (n >= SB_HXMSG_DOMAIN_LEN + 7) && std::memcmp(tail, "/pngack", 7) == 0;
    // /sb/hxbuf f:ms — a player reporting its own receive buffer so the hub can add it to
    // that player's "from hub" leg for everyone else's display. Control path only.
    const bool isHxbuf = !isMute && !isGain && !isSolo && !isFeedreq && !isClayinfo && !isChat && !isPngack
        && (n >= SB_HXMSG_DOMAIN_LEN + 6) && std::memcmp(tail, "/hxbuf", 6) == 0;
    // Recording indicator. A player reports start/stop (/sb/hxrec i:rec; /sb/hxbuf re-asserts it every
    // 10 s); everyone learns it from the periodic /sb/hxlat line (+ i:rec). Control path only.
    auto sbApplyRec = [this](const std::shared_ptr<HubPeer>& who, int32_t rec) {
        const int now = rec != 0 ? 1 : 0;
        if (who->isRecording.exchange(now, std::memory_order_relaxed) == now) return;
        std::cout << "[HUB-REC] '" << who->userName << "' " << (now ? "started" : "stopped") << " recording\n";
        // Tell every player right away (the 10-s /sb/hxlat line would take up to 10 s).
        char rbuf[256];
        try {
            osc::OutboundPacketStream m(rbuf, sizeof(rbuf));
            m << osc::BeginMessage(SB_HXMSG_DOMAIN "/hxrecstate") << who->userName.c_str() << (int32_t) now << osc::EndMessage;
            std::vector<std::shared_ptr<HubPeer>> snap;
            { std::shared_lock<std::shared_mutex> lock(mPeerMutex); for (auto& p : mPeers) if (p && p->active && p->addrlen > 0) snap.push_back(p); }
            for (int k = 0; k < 2; ++k)   // twice: a lone lost datagram must not hide a recording
                for (auto& l : snap) ::sendto(mSocket, m.Data(), m.Size(), 0, (const struct sockaddr*)&l->addr, l->addrlen);
        } catch (const osc::Exception&) {}
    };
    if (n >= SB_HXMSG_DOMAIN_LEN + 6 && std::memcmp(tail, "/hxrec", 6) == 0) {
        if (srcAddr->ss_family != AF_INET) return;
        const auto* sa_in = (const struct sockaddr_in*)srcAddr;
        std::shared_ptr<HubPeer> who;
        {
            std::shared_lock<std::shared_mutex> lock(mPeerMutex);
            for (auto& p : mPeers) {
                if (!p || !p->active) continue;
                const auto* pa = (const struct sockaddr_in*)&p->addr;
                if (pa->sin_family == AF_INET && pa->sin_addr.s_addr == sa_in->sin_addr.s_addr && pa->sin_port == sa_in->sin_port) { who = p; break; }
            }
        }
        if (!who) return;
        try {
            osc::ReceivedPacket pkt(msg, n);
            osc::ReceivedMessage rm(pkt);
            sbApplyRec(who, rm.ArgumentsBegin()->AsInt32());
        } catch (const osc::Exception&) {}
        return;
    }
    // Send-mute indicator across the hub. A player reports its own send mute (/sb/hxsendmute i:muted;
    // /sb/hxbuf arg 10 re-asserts it every 10 s); everyone learns it at once (/sb/hxsendmutestate) and from the /sb/hxlat line.
    // NOT /sb/hxmute — that name is the hub-mix mute control (SB_HXMSG_MUTE). Control path only.
    auto sbApplySendMute = [this](const std::shared_ptr<HubPeer>& who, int32_t muted) {
        const int now = muted != 0 ? 1 : 0;
        if (who->isSendMuted.exchange(now, std::memory_order_relaxed) == now) return;
        std::cout << "[HUB-MUTE] '" << who->userName << "' " << (now ? "muted" : "unmuted") << " its send\n";
        char mbuf[256];
        try {
            osc::OutboundPacketStream m(mbuf, sizeof(mbuf));
            m << osc::BeginMessage(SB_HXMSG_DOMAIN "/hxsendmutestate") << who->userName.c_str() << (int32_t) now << osc::EndMessage;
            std::vector<std::shared_ptr<HubPeer>> snap;
            { std::shared_lock<std::shared_mutex> lock(mPeerMutex); for (auto& p : mPeers) if (p && p->active && p->addrlen > 0) snap.push_back(p); }
            for (int k = 0; k < 2; ++k)
                for (auto& l : snap) ::sendto(mSocket, m.Data(), m.Size(), 0, (const struct sockaddr*)&l->addr, l->addrlen);
        } catch (const osc::Exception&) {}
    };
    if (n >= SB_HXMSG_DOMAIN_LEN + 11 && std::memcmp(tail, "/hxsendmute", 11) == 0 && !(n >= SB_HXMSG_DOMAIN_LEN + 16 && std::memcmp(tail, "/hxsendmutestate", 16) == 0)) {
        if (srcAddr->ss_family != AF_INET) return;
        const auto* sa_in = (const struct sockaddr_in*)srcAddr;
        std::shared_ptr<HubPeer> who;
        {
            std::shared_lock<std::shared_mutex> lock(mPeerMutex);
            for (auto& p : mPeers) {
                if (!p || !p->active) continue;
                const auto* pa = (const struct sockaddr_in*)&p->addr;
                if (pa->sin_family == AF_INET && pa->sin_addr.s_addr == sa_in->sin_addr.s_addr && pa->sin_port == sa_in->sin_port) { who = p; break; }
            }
        }
        if (!who) return;
        try {
            osc::ReceivedPacket pkt(msg, n);
            osc::ReceivedMessage rm(pkt);
            sbApplySendMute(who, rm.ArgumentsBegin()->AsInt32());
        } catch (const osc::Exception&) {}
        return;
    }
    // A player reports its version, system and instrument (/sb/hxver s:version s:os [s:instrument], re-sent with its
    // 10-s report). Control path only.
    if (n >= SB_HXMSG_DOMAIN_LEN + 6 && std::memcmp(tail, "/hxver", 6) == 0) {
        if (srcAddr->ss_family != AF_INET) return;
        const auto* sa_in = (const struct sockaddr_in*)srcAddr;
        std::shared_ptr<HubPeer> who;
        {
            std::shared_lock<std::shared_mutex> lock(mPeerMutex);
            for (auto& p : mPeers) {
                if (!p || !p->active) continue;
                const auto* pa = (const struct sockaddr_in*)&p->addr;
                if (pa->sin_family == AF_INET && pa->sin_addr.s_addr == sa_in->sin_addr.s_addr && pa->sin_port == sa_in->sin_port) { who = p; break; }
            }
        }
        if (!who) return;
        try {
            osc::ReceivedPacket pkt(msg, n);
            osc::ReceivedMessage rm(pkt);
            auto it = rm.ArgumentsBegin();
            std::string ver = (it != rm.ArgumentsEnd()) ? std::string((it++)->AsString()) : std::string();
            std::string os  = (it != rm.ArgumentsEnd()) ? std::string((it++)->AsString()) : std::string();
            std::string inst = (it != rm.ArgumentsEnd() && it->IsString()) ? std::string(it->AsString()) : std::string();
            if (ver.size() > 60) ver.resize(60);
            if (os.size() > 16) os.resize(16);
            if (inst.size() > 24) inst.resize(24);
            auto clean = [](std::string& t){ for (auto& c : t) if ((unsigned char) c < 32 || c == '"' || c == '\\' || c == '<' || c == '>' || c == '&') c = ' '; };
            clean(ver); clean(os); clean(inst);
            bool changed = false, instChanged = false;
            { std::lock_guard<std::mutex> vl(who->verLock); changed = (who->clientVersion != ver || who->clientOs != os); who->clientVersion = ver; who->clientOs = os;
              instChanged = who->clientInstrument != inst; who->clientInstrument = inst; }
            if (instChanged) mStageGen.fetch_add(1, std::memory_order_relaxed);   // the listing shows it: check in again
            if (changed) std::cout << "[HUB-VER] '" << who->userName << "' runs " << ver << " on " << os << "\n";
        } catch (const osc::Exception&) {}
        return;
    }
    if (isHxbuf) {
        if (srcAddr->ss_family != AF_INET) return;
        const auto* sa_in = (const struct sockaddr_in*)srcAddr;
        std::shared_ptr<HubPeer> who;
        {
            std::shared_lock<std::shared_mutex> lock(mPeerMutex);
            for (auto& p : mPeers) {
                if (!p || !p->active) continue;
                const auto* pa = (const struct sockaddr_in*)&p->addr;
                if (pa->sin_family == AF_INET && pa->sin_addr.s_addr == sa_in->sin_addr.s_addr && pa->sin_port == sa_in->sin_port) { who = p; break; }
            }
        }
        if (!who) return;
        try {
            osc::ReceivedPacket pkt(msg, n);
            osc::ReceivedMessage rm(pkt);
            auto it = rm.ArgumentsBegin();
            const float ms = (it++)->AsFloat();
            if (ms >= 0.0f && ms < 1000.0f) who->clientRecvBufMs.store(ms, std::memory_order_relaxed);
            if (rm.ArgumentCount() >= 4) {   // Receive health (underruns, pops, xruns in the last 10 s)
                const int32_t u = (it++)->AsInt32(), pp = (it++)->AsInt32(), x = (it++)->AsInt32();
                if (u >= 0 && pp >= 0 && x >= 0 && u < 100000 && pp < 100000 && x < 100000) {
                    who->clientRxUnderruns10s.store(u, std::memory_order_relaxed);
                    who->clientRxPops10s.store(pp, std::memory_order_relaxed);
                    who->clientXruns10s.store(x, std::memory_order_relaxed);
                    who->clientRxUnderrunsTotal.fetch_add((uint32_t) u, std::memory_order_relaxed);
                    who->clientRxPopsTotal.fetch_add((uint32_t) pp, std::memory_order_relaxed);
                }
            }
            if (rm.ArgumentCount() >= 5) sbApplyRec(who, (it++)->AsInt32());   // Recording, re-asserted every 10 s
            if (rm.ArgumentCount() >= 7) {   // Audio buffer + sample rate in force on the player
                const int32_t b = (it++)->AsInt32(), r = (it++)->AsInt32();
                if (b > 0 && b <= 8192) who->clientDevBuf.store(b, std::memory_order_relaxed);
                if (r > 0 && r <= 768000) who->clientDevSr.store(r, std::memory_order_relaxed);
            }
            if (rm.ArgumentCount() >= 8) {   // Seconds since the player last applied our /sb/hxlat (-1 never)
                const int32_t a = (it++)->AsInt32();
                if (a >= -1 && a < 100000000) who->clientLatFeedAgeS.store(a, std::memory_order_relaxed);
            }
            if (rm.ArgumentCount() >= 9) {   // The player's receive-buffer mode (0 Manual, 1 Auto)
                const int32_t m = (it++)->AsInt32();
                if (m >= 0 && m <= 1) who->clientRecvBufMode.store(m, std::memory_order_relaxed);
            }
            if (rm.ArgumentCount() >= 10) {  // Send mute re-asserted every 10 s (late joiners / lost datagrams converge)
                const int32_t sm = (it++)->AsInt32();
                if (sm >= 0 && sm <= 1) sbApplySendMute(who, sm);
            }
            if (rm.ArgumentCount() >= 11) {  // Capability bits from the player (bit0 = decodes idle-masked blocks). The stream-flag
                // path below only fires on the player's OWN uplink format, which a send-muted player never sends, so that
                // player would get every sender's channels unmasked. Same enable as that path, same per-peer lock only.
                const int32_t caps = (it++)->AsInt32();
                if ((caps & 1) && hubIdleMaskEnabled() && !who->capIdleMask.exchange(true)) {
                    std::cout << "[HUB-CAP] peer='" << who->userName << "' decodes idle-masked blocks (from health report): enabling on its feeds\n";
                    std::lock_guard<std::mutex> fl(who->outFeedsLock);
                    int32_t wf = AOO_PROTOCOL_FLAG_COMPACT_DATA | AOO_PROTOCOL_FLAG_IDLE_MASK;
                    for (auto& kv : who->outFeeds)
                        if (kv.second && kv.second->source)
                            kv.second->source->set_sinkoption(who->persistentEndpoint, kv.second->clientOurId, aoo_opt_protocol_flags, &wf, sizeof(wf));
                }
            }
        } catch (const osc::Exception&) {}
        return;
    }
    if (isPngack) {
        if (srcAddr->ss_family != AF_INET) return;
        const auto* sa_in = (const struct sockaddr_in*)srcAddr;
        std::shared_ptr<HubPeer> who;
        {
            std::shared_lock<std::shared_mutex> lock(mPeerMutex);
            for (auto& p : mPeers) {
                if (!p || !p->active) continue;
                const auto* pa = (const struct sockaddr_in*)&p->addr;
                if (pa->sin_family == AF_INET && pa->sin_addr.s_addr == sa_in->sin_addr.s_addr && pa->sin_port == sa_in->sin_port) { who = p; break; }
            }
        }
        if (!who) return;
        try {
            osc::ReceivedPacket pkt(msg, n);
            osc::ReceivedMessage rm(pkt);
            auto it = rm.ArgumentsBegin();
            const uint64_t tt1 = (it++)->AsTimeTag();
            (void)(it++)->AsTimeTag();
            const uint64_t tt3 = aoo_osctime_get();
            const double rttMs = aoo_osctime_duration(tt1, tt3) * 1000.0;
            if (rttMs >= 0.0 && rttMs < 5000.0) {
                // Median of the last 5 replies (outlier-proof); see HubPeer::rttRing.
                who->smoothedRttMs.store(HubPeer::medianPush(who->rttAckRing, who->rttAckRingIdx, (float) rttMs), std::memory_order_relaxed);   // recv-thread ring
            }
        } catch (const osc::Exception&) {}
        return;
    }
    if (!isMute && !isGain && !isSolo && !isFeedreq && !isClayinfo && !isChat) {
        return;
    }

    // /sb/clayinfo passthrough: stock SB sender broadcasts per-channel labels
    // via this OSC message to each direct P2P peer. In hub-relay we receive
    // it from the sender and forward a rewritten copy to every listener of
    // that sender, with sourceid replaced by the listener's local relayed-
    // peer ourId. Stock SB's existing handler (SonobusPluginProcessor.cpp:
    // 9704 SONOBUS_MSGTYPE_LAYOUTINFO) then finds the peer via
    // findRemotePeer(hubEndpoint, sourceid) and applies the layout.
    if (isClayinfo) {
        if (srcAddr->ss_family != AF_INET) return;
        const auto* sa_in = (const struct sockaddr_in*)srcAddr;

        std::shared_ptr<HubPeer> sender;
        std::vector<std::shared_ptr<HubPeer>> snap;
        {
            std::shared_lock<std::shared_mutex> lock(mPeerMutex);
            for (auto& p : mPeers) if (p) snap.push_back(p);
            for (auto& p : snap) {
                if (!p || !p->active) continue;
                const auto* pa = (const struct sockaddr_in*)&p->addr;
                if (pa->sin_family == AF_INET
                    && pa->sin_addr.s_addr == sa_in->sin_addr.s_addr
                    && pa->sin_port == sa_in->sin_port) { sender = p; break; }
            }
        }
        if (!sender) {
            std::cerr << "[HUB-CLAY] /sb/clayinfo from unknown sender (addr not matched)\n";
            // (not on the connection log: a joining player's first /sb/clayinfo normally lands a moment before its audio
            //  handshake registers the address — a benign join-order race, not an odd sender)
            return;
        }

        const void* blob = nullptr;
        osc::osc_bundle_element_size_t blobSize = 0;
        try {
            osc::ReceivedPacket pkt(msg, n);
            osc::ReceivedMessage rm(pkt);
            auto it = rm.ArgumentsBegin();
            (it++)->AsInt32();  // discard original sourceid (sender's view)
            (it++)->AsBlob(blob, blobSize);
        } catch (const osc::Exception& e) {
            std::cerr << "[HUB-CLAY] parse failed: " << e.what() << "\n";
            return;
        }
        if (!blob || blobSize <= 0) return;

        // Cache so newly-joining listeners get the layout immediately when
        // their feed is allocated (otherwise they'd miss layouts broadcast
        // before they connected).
        sender->clayinfoBlob.assign((const char*)blob, (const char*)blob + (size_t)blobSize);
        {   // Per-channel names for the hub page / GUI hover list
            auto names = hubParseChannelNames((const char*)blob, (size_t)blobSize);
            std::lock_guard<std::mutex> nl(sender->clayNamesLock);
            sender->channelNames = std::move(names);
        }

        int forwarded = 0;
        for (auto& listener : snap) {
            if (!listener || listener->id == sender->id || !listener->active) continue;
            int32_t listenerOurIdForSender = -1;
            {
                std::lock_guard<std::mutex> flock(listener->outFeedsLock);
                auto it = listener->outFeeds.find(sender->id);
                if (it == listener->outFeeds.end() || !it->second) continue;
                listenerOurIdForSender = it->second->clientOurId;
            }
            // A 28-input layout is ~2.9 KB, so size the forward buffer to the client's receive limit
            // (AOO_MAXPACKETSIZE 4096); a smaller one throws "out of buffer memory" and listeners show blank labels.
            char outbuf[AOO_MAXPACKETSIZE];
            try {
                osc::OutboundPacketStream outmsg(outbuf, sizeof(outbuf));
                outmsg << osc::BeginMessage(SB_HXFULLMSG_CLAYINFO)
                       << listenerOurIdForSender
                       << osc::Blob(blob, (int)blobSize)
                       << osc::EndMessage;
                if (listener->addrlen > 0) {
                    ::sendto(mSocket, outmsg.Data(), outmsg.Size(), 0,
                             (const struct sockaddr*)&listener->addr,
                             listener->addrlen);
                    ++forwarded;
                }
            } catch (const osc::Exception& e) {
                std::cerr << "[HUB-CLAY] forward build failed: " << e.what() << "\n";
            }
        }
        std::cout << "[HUB-CLAY] forwarded sender='" << sender->userName
                  << "' layout to " << forwarded << " listener(s) blobSize="
                  << (int)blobSize << "\n";
        return;
    }

    // /sb/chat relay. Hosted clients only have the hub endpoint, so every chat message lands here.
    // Wire format (stock SB, SonobusPluginProcessor sendChatEvent):
    //   /sb/chat s:group s:from s:targets s:tags s:message
    // The payload is self-describing (receivers attribute by the 'from' field, never by UDP source
    // address), so we relay the original bytes verbatim to every other active peer, honoring the
    // "|"-separated 'targets' user-name list (empty = everyone). Runs on the network recv thread: same
    // snapshot-under-shared-lock discipline as the clayinfo relay above, no new locks inside the per-peer
    // loop, no heap churn beyond the peer snapshot.
    if (isChat) {
        if (srcAddr->ss_family != AF_INET) return;
        // Chat is user-typed and rare — no throttling needed, but bound the
        // relayed size (client packets are <= AOO_MAXPACKETSIZE anyway).
        if (n > 4096) {
            std::cerr << "[HUB-CHAT] dropping oversized /sb/chat ("
                      << n << " bytes)\n";
            return;
        }
        const auto* sa_in = (const struct sockaddr_in*)srcAddr;

        std::shared_ptr<HubPeer> sender;
        std::vector<std::shared_ptr<HubPeer>> snap;
        {
            std::shared_lock<std::shared_mutex> lock(mPeerMutex);
            for (auto& p : mPeers) if (p) snap.push_back(p);
            for (auto& p : snap) {
                if (!p || !p->active) continue;
                const auto* pa = (const struct sockaddr_in*)&p->addr;
                if (pa->sin_family == AF_INET
                    && pa->sin_addr.s_addr == sa_in->sin_addr.s_addr
                    && pa->sin_port == sa_in->sin_port) { sender = p; break; }
            }
        }
        if (!sender) {
            std::cerr << "[HUB-CHAT] /sb/chat from unknown sender (addr not matched)\n";
            connLog("udp-unknown", hubSockaddrInToString(sa_in), "", "", "/sb/chat from an address that is not a peer", true);   
            return;
        }

        // Parse only what routing needs (from = display name for the log,
        // targets = recipient filter); the relayed bytes stay untouched.
        std::string from, targets;
        try {
            osc::ReceivedPacket pkt(msg, n);
            osc::ReceivedMessage rm(pkt);
            auto it = rm.ArgumentsBegin();
            (it++)->AsString();          // group
            from = (it++)->AsString();
            targets = (it++)->AsString();
        } catch (const osc::Exception& e) {
            std::cerr << "[HUB-CHAT] parse failed: " << e.what() << "\n";
            return;
        }

        int forwarded = 0;
        for (auto& listener : snap) {
            if (!listener || listener->id == sender->id || !listener->active) continue;
            if (listener->addrlen <= 0) continue;
            if (!targets.empty()) {
                // "|"-separated exact user names (client uses
                // StringArray::fromTokens(targets, "|", "")).
                bool match = false;
                size_t pos = 0;
                for (;;) {
                    size_t bar = targets.find('|', pos);
                    size_t end = (bar == std::string::npos) ? targets.size() : bar;
                    if (targets.compare(pos, end - pos, listener->userName) == 0) {
                        match = true; break;
                    }
                    if (bar == std::string::npos) break;
                    pos = bar + 1;
                }
                if (!match) continue;
            }
            ::sendto(mSocket, msg, n, 0,
                     (const struct sockaddr*)&listener->addr,
                     listener->addrlen);
            ++forwarded;
        }
        std::cout << "[HUB-CHAT] relayed chat from '" << from << "' (peer '"
                  << sender->userName << "') to " << forwarded
                  << " peer(s), " << n << " bytes\n";
        return;
    }

    // Parse the OSC message body.
    std::string senderName;
    std::string listenerNameFromMsg;  // Optional 3rd feedreq arg
    bool muted = false;
    float gain = 1.0f;
    bool soloed = false;
    int32_t clientOurId = 0;  // for isFeedreq
    try {
        osc::ReceivedPacket pkt(msg, n);
        osc::ReceivedMessage rm(pkt);
        auto it = rm.ArgumentsBegin();
        senderName = (it++)->AsString();
        if (isMute) {
            muted = (it++)->AsInt32() != 0;
        } else if (isGain) {
            gain = (it++)->AsFloat();
        } else if (isSolo) {
            soloed = (it++)->AsInt32() != 0;
        } else { // isFeedreq
            clientOurId = (it++)->AsInt32();
            // Optional listener name for self-heal. Older
            // clients omit this; treat absence as empty string and skip heal.
            if (it != rm.ArgumentsEnd()) {
                try { listenerNameFromMsg = (it++)->AsString(); } catch (...) {}
            }
        }
    } catch (const osc::Exception& e) {
        std::cerr << "[HUB-CTRL] failed to parse "
                  << (isMute ? SB_HXFULLMSG_MUTE
                       : isGain ? SB_HXFULLMSG_GAIN
                       : isSolo ? SB_HXFULLMSG_SOLO
                       : SB_HXFULLMSG_FEEDREQ)
                  << ": " << e.what() << "\n";
        return;
    }

    // Resolve listener (sender of THIS UDP packet) by source address.
    if (srcAddr->ss_family != AF_INET) return;
    const auto* sa = (const struct sockaddr_in*)srcAddr;

    std::unique_lock<std::shared_mutex> lock(mPeerMutex);

    HubPeer* listener = nullptr;
    for (auto& p : mPeers) {
        if (!p || !p->active) continue;
        const auto* pa = (const struct sockaddr_in*)&p->addr;
        if (pa->sin_family == AF_INET
            && pa->sin_addr.s_addr == sa->sin_addr.s_addr
            && pa->sin_port == sa->sin_port) {
            listener = p.get();
            break;
        }
    }

    // Self-heal pass: if address-based lookup missed but the OSC payload carries the listener's userName
    // (feedreq 3rd arg), recover via name. Two heal paths:
    //   (a) name in mPeers but addr drifted: just update p->addr
    //   (b) name not in mPeers at all: emergency addPeer (covers AOO PEER_JOIN events that get lost
    //       across rapid hub bounces)
    if (!listener && isFeedreq && !listenerNameFromMsg.empty()) {
        for (auto& p : mPeers) {
            if (p && p->userName == listenerNameFromMsg) {
                char srcIp[INET_ADDRSTRLEN] = {};
                inet_ntop(AF_INET, &sa->sin_addr, srcIp, sizeof(srcIp));
                std::cerr << "[HUB-CTRL] self-heal: addr drift for peer '"
                          << listenerNameFromMsg << "' → " << srcIp
                          << ":" << ntohs(sa->sin_port) << "\n";
                memcpy(&p->addr, srcAddr, sizeof(struct sockaddr_in));
                p->addrlen = sizeof(struct sockaddr_in);
                p->active = true;
                rebindPeerEndpointLocked(p, "feedreq self-heal");   // Endpoint (feeds + connected fd) follows
                rebuildPeerSnapshotLocked();  // Addr changed → re-key byAddr4 — REQUIRED right after the rebind
                listener = p.get();
                break;
            }
        }
    }
    if (!listener && isFeedreq && !listenerNameFromMsg.empty()) {
        // Path (b): name not present at all. Emergency addPeer. addPeer takes
        // mPeerMutex itself so we must release ours first.
        std::string healName = std::move(listenerNameFromMsg);
        std::string healSenderName = senderName;
        int32_t healOurId = clientOurId;
        int32_t newId = mNextPeerId++;
        lock.unlock();
        char srcIp[INET_ADDRSTRLEN] = {};
        inet_ntop(AF_INET, &sa->sin_addr, srcIp, sizeof(srcIp));
        std::cerr << "[HUB-CTRL] emergency addPeer for unknown listener '"
                  << healName << "' (id=" << newId << ") from " << srcIp
                  << ":" << ntohs(sa->sin_port) << "\n";
        addPeer(newId, healName, const_cast<struct sockaddr_storage*>(srcAddr));
        // Re-acquire lock + look up the newly-added peer + sender, then run
        // the same feed-alloc path the normal flow would have taken.
        lock.lock();
        HubPeer* healedListener = nullptr;
        HubPeer* healedSender = nullptr;
        for (auto& p : mPeers) {
            if (!p) continue;
            if (p->id == newId) healedListener = p.get();
            if (p->userName == healSenderName) healedSender = p.get();
        }
        if (healedListener) {
            if (healedSender) {
                allocateOrUpdateFeed(healedListener, healedSender, healOurId);
            } else {
                // Stash for replay when sender eventually
                // joins (via PEER_JOIN or another self-heal).
                std::cout << "[HUB-FEED] stashing pending feedreq listener='"
                          << healedListener->userName << "' sender='" << healSenderName
                          << "' ourId=" << healOurId << "\n";
                mPendingFeedReqs.push_back({healedListener->id, healSenderName, healOurId});
            }
        }
        return;
    }
    if (!listener) {
        // Unrecoverable: non-feedreq op, or a feedreq without a listener name. Log and drop with enough
        // context for diagnosis.
        char srcIp[INET_ADDRSTRLEN] = {};
        inet_ntop(AF_INET, &sa->sin_addr, srcIp, sizeof(srcIp));
        std::cerr << "[HUB-CTRL] "
                  << (isMute ? "mute" : isGain ? "gain" : isSolo ? "solo" : "feedreq")
                  << " from unknown peer (src=" << srcIp << ":" << ntohs(sa->sin_port)
                  << ", listenerNameFromMsg='" << listenerNameFromMsg << "')\n";
        connLog("udp-unknown", std::string(srcIp) + ":" + std::to_string(ntohs(sa->sin_port)), listenerNameFromMsg, "", std::string(isMute ? "mute" : isGain ? "gain" : isSolo ? "solo" : "feedreq") + " control message from an address that is not a peer", true);   
        return;
    }

    // Resolve sender by userName (the peer being controlled / requested).
    HubPeer* sender = nullptr;
    for (auto& p : mPeers) {
        if (p && p->active && p->userName == senderName) { sender = p.get(); break; }
    }
    if (!sender) {
        if (isFeedreq) {
            // Stash for replay when the sender eventually joins (via normal AOO PEER_JOIN or another peer's
            // emergency self-heal addPeer). Clients have limited retry budgets (5 tiers over 4 s), so a missed
            // retry would leave the listener permanently muted for that sender. Stashing makes recovery
            // deterministic regardless of arrival order.
            std::cout << "[HUB-FEED] stashing pending feedreq listener='"
                      << listener->userName << "' sender='" << senderName
                      << "' ourId=" << clientOurId << "\n";
            mPendingFeedReqs.push_back({listener->id, senderName, clientOurId});
            return;
        }
        // Mute/gain/solo: allow the override anyway — sender might join later.
        std::cerr << "[HUB-CTRL] override for not-yet-present sender '"
                  << senderName << "' from listener '" << listener->userName << "'\n";
    }

    if (isFeedreq) {
        // Feed requests arrive within ms of a join, right after addPeer bumped the peer-list gen, when the
        // audio thread needs mPeerMutex SHARED, so this handler must not hold it EXCLUSIVE across the aoo
        // source build. Peers cannot be removed under a shared lock, so the two raw pointers stay valid;
        // drop to shared for the allocation.
        lock.unlock();
        std::shared_lock<std::shared_mutex> slock(mPeerMutex);
        allocateOrUpdateFeed(listener, sender, clientOurId);
        return;
    }

    auto& ovr = listener->mixOverrides[senderName];
    if (isMute) {
        ovr.muted = muted;
        std::cout << "[HUB-CTRL] '" << listener->userName << "' set mute("
                  << senderName << ") = " << (muted ? "ON" : "off") << "\n";
    } else if (isGain) {
        ovr.gain = gain;
        std::cout << "[HUB-CTRL] '" << listener->userName << "' set gain("
                  << senderName << ") = " << gain << "\n";
    } else { // isSolo
        ovr.soloed = soloed;
        std::cout << "[HUB-CTRL] '" << listener->userName << "' set solo("
                  << senderName << ") = " << (soloed ? "ON" : "off") << "\n";
    }
    // This is the only writer of any mixOverrides map. Bump the mix generation while still holding
    // mPeerMutex exclusive so the audio thread, which re-reads the gen under its shared lock before
    // flattening, can never observe the new gen with a half-applied map.
    mMixGen.fetch_add(1, std::memory_order_release);
}

// Configure an AOO source's wire format. Used for per-(listener × sender) outbound feeds: the channel
// count comes from the sender's negotiated recvChannels, not from the hub-global mChannels.
void SonobusHubServer::setupSourceFormatFor(aoo::isource* source, int channels) {
    aoo_format_storage f;

    if (mUseOpus) {
        aoo_format_opus* fmt = (aoo_format_opus*)&f;
        fmt->header.codec = AOO_CODEC_OPUS;
        fmt->header.blocksize = mBlockSize;
        fmt->header.samplerate = mSampleRate;
        fmt->header.nchannels = channels;
        fmt->bitrate = mOpusBitrate * channels;
        fmt->complexity = mOpusComplexity;
        fmt->signal_type = OPUS_SIGNAL_MUSIC;
        fmt->application_type = OPUS_APPLICATION_RESTRICTED_LOWDELAY;
    } else {
        aoo_format_pcm* fmt = (aoo_format_pcm*)&f;
        fmt->header.codec = AOO_CODEC_PCM;
        fmt->header.blocksize = mBlockSize;
        fmt->header.samplerate = mSampleRate;
        fmt->header.nchannels = channels;
        fmt->bitdepth = AOO_PCM_INT16;
    }

    source->set_format(f.header);
}

// Allocate (or reconfigure) listener->outFeeds[sender->id] for a /sb/hxfeedreq request. Caller holds
// mPeerMutex. Returns the feed pointer (owned by listener->outFeeds; not transferring ownership).
HubOutFeed* SonobusHubServer::allocateOrUpdateFeed(HubPeer* listener, HubPeer* sender,
                                                   int32_t clientOurId) {
    if (!listener || !sender || !listener->persistentEndpoint) return nullptr;

    int32_t channels = std::max(1, std::min((int)sender->recvChannels,
                                            HUB_MAX_CHANNELS_PER_PEER));

    // Belt-and-braces against a ghost
    // slot. hubSourceId is deterministic per (listener, sender) NAME pair
    // while the map is keyed by sender ID. If some OTHER key already carries
    // this source id, it is a leftover from a previous incarnation of this
    // sender that no removal path unlinked — leaving it would put TWO hub
    // sources on the listener's one sink id, and a lost/late /sb/hxfeedreq
    // could bind the dead one. Unlink under the lock (find + node move-out);
    // `evicted` is declared BEFORE mlock so its AOO stop/remove_all/free run
    // AFTER outFeedsLock is released, at function exit.
    struct EvictedFeeds {
        std::vector<std::unique_ptr<HubOutFeed>> feeds;
        ~EvictedFeeds() {
            for (auto& f : feeds) {
                if (f && f->source) { f->source->stop(); f->source->remove_all(); }
            }
        }
    } evicted;

    // Build the candidate feed before taking outFeedsLock. The audio thread pumps this listener under
    // that lock every block, and aoo source create/setup/start allocates and sets up sinks, so holding
    // the lock across it would stall every existing listener at each join. The lock covers only the map
    // scan and a pointer move; the cout/clayinfo I/O below runs after it is released. If the slot turns
    // out to exist already, the prebuilt feed is torn down after the lock (UnusedFeed dtor, declared
    // before it).
    struct UnusedFeed {
        std::unique_ptr<HubOutFeed> f;
        ~UnusedFeed() { if (f && f->source) { f->source->stop(); f->source->remove_all(); } }
    } unused;
    std::unique_ptr<HubOutFeed> prebuilt = std::make_unique<HubOutFeed>();
    {
        auto& feed = prebuilt;
        // Deterministic source id derived from usernames so the listener can
        // independently compute the same id and pre-invite us. The invite
        // forces AOO source to send format immediately; without it, audio
        // playback at the listener can stall up to ~10s waiting for the
        // first format announce to arrive without acknowledgement.
        feed->hubSourceId = feedSourceIdFor(listener->userName, sender->userName);
        feed->clientOurId = clientOurId;
        feed->channels    = channels;
        feed->source.reset(aoo::isource::create(feed->hubSourceId));
        {   // Mask idle channels toward listeners that can decode them
            int32_t im = hubIdleMaskEnabled() ? 1 : 0;
            feed->source->set_option(aoo_opt_idle_mask, &im, sizeof(int32_t));
        }
        feed->source->setup(mSampleRate, mBlockSize, channels);
        // Mark this as a synthetic-clock source. Feed sources are process()ed with the hub's synthetic
        // monotonic block timestamps (base + n*period, see audioProcessThread), so a timer error on them
        // means the pump slipped, not lost audio. With the flag, aoo::source resyncs its DLL instead of
        // scheduling dropped_ empties that would trash real queued blocks. Hub feed sources only; clients
        // never set this (default 0). The flag is a member atomic, so it survives later re-setup() calls
        // (channel-change paths) without re-assertion.
        {
            int32_t syntheticClock = 1;
            feed->source->set_option(aoo_opt_synthetic_clock,
                                     &syntheticClock, sizeof(int32_t));
        }
        feed->source->set_buffersize((int32_t)HUB_SOURCE_BUFFERTIME_MS);
        // PCM INT16 at blocksize 128 / 2ch is 512 data bytes (+8B rate = 520B in the compact form); with the
        // AOO_DATA_HEADERSIZE=80 header budget, a 600B packetsize sits right at the 512/520B fragmentation
        // cliff, where one extra option byte tips a single-frame block into two frames (two datagrams,
        // reassembly, added latency). 700 clears the cliff with margin and stays far under
        // AOO_MAXPACKETSIZE (4096) and path MTU.
        feed->source->set_packetsize(700);
        // Per-listener redundancy. On wired LAN, loss is near zero and AOO resend (limit 5 / 10 ms) recovers
        // in-buffer at 1 ms RTT, so LAN=1 halves hub sendto and client pps. WAN keeps 2.
        feed->source->set_redundancy(listener->isLan ? mRedundancyLan : mRedundancyWan);
        // Enable ASRC on
        // the OUTBOUND feed source too, so audio leaves the hub re-clocked
        // to the listener client's sampling cadence. Pairs with sink-side
        // ASRC (addPeer) to make the entire sender→hub→listener path drift-
        // tolerant. ~21µs latency cost per source.
        feed->source->set_dynamic_resampling(hub_asrc_enabled());
        setupSourceFormatFor(feed->source.get(), channels);

        // Userformat passthrough: if the sender has a cached userformat blob (per-channel labels + layout),
        // set it on this feed source so listeners get the same labels stock SB applies to direct P2P peers
        // via applyLayoutFormatToPeer on AOO_SOURCE_FORMAT_EVENT.
        if (!sender->userformatBlob.empty()) {
            feed->source->set_option(aoo_opt_userformat,
                                     sender->userformatBlob.data(),
                                     (int32_t)sender->userformatBlob.size());
        }

        int32_t wireFlags = AOO_PROTOCOL_FLAG_COMPACT_DATA | ((hubIdleMaskEnabled() && listener->capIdleMask.load()) ? AOO_PROTOCOL_FLAG_IDLE_MASK : 0);   
        feed->source->add_sink(listener->persistentEndpoint, clientOurId,
                               (aoo_replyfn)hub_endpoint_send_impl);
        feed->source->set_sinkoption(listener->persistentEndpoint, clientOurId,
                                     aoo_opt_protocol_flags, &wireFlags, sizeof(int32_t));
        feed->source->start();

    }

    std::unique_lock<std::mutex> mlock(listener->outFeedsLock);

    {
        const int32_t wantId = feedSourceIdFor(listener->userName, sender->userName);
        for (auto it = listener->outFeeds.begin(); it != listener->outFeeds.end();) {
            if (it->first != sender->id && it->second && it->second->hubSourceId == wantId) {
                std::cout << "[HUB-FEED] evicted stale feed listener='" << listener->userName
                          << "' sender='" << sender->userName
                          << "' sourceId=" << wantId
                          << " staleSenderId=" << it->first
                          << " liveSenderId=" << sender->id << "\n";
                evicted.feeds.push_back(std::move(it->second));
                it = listener->outFeeds.erase(it);
            } else {
                ++it;
            }
        }
    }

    auto& slot = listener->outFeeds[sender->id];

    if (!slot) {
        // First-time allocation for this (listener, sender) pair (built above, off the lock).
        slot = std::move(prebuilt);
        HubOutFeed* result = slot.get();
        const int32_t newSourceId = result->hubSourceId;
        mlock.unlock();   // The I/O below runs off the audio thread's lock

        std::cout << "[HUB-FEED] allocated feed for listener='" << listener->userName
                  << "' sender='" << sender->userName
                  << "' sourceId=" << newSourceId
                  << " clientOurId=" << clientOurId
                  << " ch=" << channels
                  << " (listener evlog s=" << clientOurId << ":" << newSourceId << ")\n";   // Names the stream in the player's own log

        // /sb/clayinfo replay: if sender has a cached layout blob, push it to
        // this brand-new listener right now so they get channel labels even
        // if the sender's last layout broadcast happened before they joined.
        if (!sender->clayinfoBlob.empty() && listener->addrlen > 0) {
            char outbuf[AOO_MAXPACKETSIZE];   // Same 1500-byte failure as the forward path (28-input layout = 2.9 KB)
            try {
                osc::OutboundPacketStream outmsg(outbuf, sizeof(outbuf));
                outmsg << osc::BeginMessage(SB_HXFULLMSG_CLAYINFO)
                       << clientOurId
                       << osc::Blob(sender->clayinfoBlob.data(),
                                    (int)sender->clayinfoBlob.size())
                       << osc::EndMessage;
                ::sendto(mSocket, outmsg.Data(), outmsg.Size(), 0,
                         (const struct sockaddr*)&listener->addr,
                         listener->addrlen);
                std::cout << "[HUB-CLAY] replayed cached layout to new listener='"
                          << listener->userName << "' sender='" << sender->userName
                          << "' blobSize=" << sender->clayinfoBlob.size() << "\n";
            } catch (const osc::Exception& e) {
                std::cerr << "[HUB-CLAY] replay build failed: " << e.what() << "\n";
            }
        }
        return result;
    }
    unused.f = std::move(prebuilt);   // Slot existed; tear the candidate down after the lock

    // Existing feed — either same ourId (re-request, no-op) or different
    // (re-bind to new ourId, e.g., client renumbered).
    if (slot->clientOurId == clientOurId && slot->channels == channels) {
        return slot.get();  // nothing to do
    }

    std::lock_guard<std::mutex> flock(slot->lock);

    if (slot->clientOurId != clientOurId && slot->source) {
        // Drop old sink, add new sink at the new ourId.
        slot->source->remove_sink(listener->persistentEndpoint, slot->clientOurId);
        int32_t wireFlags = AOO_PROTOCOL_FLAG_COMPACT_DATA | ((hubIdleMaskEnabled() && listener->capIdleMask.load()) ? AOO_PROTOCOL_FLAG_IDLE_MASK : 0);   
        slot->source->add_sink(listener->persistentEndpoint, clientOurId,
                               (aoo_replyfn)hub_endpoint_send_impl);
        slot->source->set_sinkoption(listener->persistentEndpoint, clientOurId,
                                     aoo_opt_protocol_flags, &wireFlags, sizeof(int32_t));
        std::cout << "[HUB-FEED] re-bound feed listener='" << listener->userName
                  << "' sender='" << sender->userName << "' "
                  << slot->clientOurId << " -> " << clientOurId
                  << " (listener evlog s=" << clientOurId << ":" << slot->hubSourceId << ")\n";
        slot->clientOurId = clientOurId;
    }

    if (slot->channels != channels && slot->source) {
        // Sender's channel count changed; reconfigure the source.
        slot->source->setup(mSampleRate, mBlockSize, channels);
        slot->source->set_buffersize((int32_t)HUB_SOURCE_BUFFERTIME_MS);
        slot->source->set_dynamic_resampling(hub_asrc_enabled());  // re-assert post-setup, see addPeer note
        setupSourceFormatFor(slot->source.get(), channels);
        std::cout << "[HUB-FEED] reconfigured feed listener='" << listener->userName
                  << "' sender='" << sender->userName << "' "
                  << slot->channels << "ch -> " << channels << "ch\n";
        slot->channels = channels;
    }

    return slot.get();
}

// Delegates to the shared inline in SonobusHubMixControl.h so both hub and
// listener client compute the same id from the same (listener, sender) pair.
int32_t SonobusHubServer::feedSourceIdFor(const std::string& listenerName,
                                          const std::string& senderName) {
    return sb_feedSourceIdFor(listenerName, senderName);
}

// Static C trampoline so AOO can call back into a member function with a peer id.
int32_t SonobusHubServer::hub_sink_event_handler(void* user, const aoo_event** events, int32_t n) {
    auto* ctx = static_cast<PeerSinkEventCtx*>(user);
    if (!ctx || !ctx->hub) return 0;

    for (int32_t i = 0; i < n; ++i) {
        if (!events[i]) continue;

        // Capture inbound (peer->hub) RTT from sink-side pings. Pairs with smoothedRttMs (outbound
        // source-side ping) for to/from latency display in the GUI peer table. Off the audio thread
        // (sendThread polls events). Relaxed atomic store.
        if (events[i]->type == AOO_PING_EVENT) {
            auto* e = (const aoo_ping_event*)events[i];
            double rttMs = aoo_osctime_duration(e->tt1, e->tt3) * 1000.0;
            if (!(rttMs >= 0.0 && rttMs < 5000.0)) continue;
            HubPeer* peerForRtt = ctx->peer;   // Deadlock fix: no locks in handlers
            if (!peerForRtt) continue;
            // Median of the last 5 samples instead of an EMA (one late reply no longer inflates the display).
            peerForRtt->smoothedRttUpMs.store(HubPeer::medianPush(peerForRtt->rttUpRing, peerForRtt->rttUpRingIdx, (float) rttMs), std::memory_order_relaxed);
            continue;
        }

        // Count block events per peer.
        if (events[i]->type == AOO_BLOCK_LOST_EVENT
            || events[i]->type == AOO_BLOCK_REORDERED_EVENT
            || events[i]->type == AOO_BLOCK_RESENT_EVENT
            || events[i]->type == AOO_BLOCK_GAP_EVENT) {
            auto* be = (const aoo_block_lost_event*)events[i];
            HubPeer* lp = ctx->peer;   // Deadlock fix: no locks in handlers
            if (lp) {
                uint32_t cnt = (uint32_t)std::max(1, be->count);
                switch (events[i]->type) {
                case AOO_BLOCK_LOST_EVENT:      lp->blkLost.fetch_add(cnt, std::memory_order_relaxed); lp->blkLostTotal.fetch_add(cnt, std::memory_order_relaxed); break;
                case AOO_BLOCK_REORDERED_EVENT: lp->blkReordered.fetch_add(cnt, std::memory_order_relaxed); break;
                case AOO_BLOCK_RESENT_EVENT:    lp->blkResent.fetch_add(cnt, std::memory_order_relaxed); break;
                case AOO_BLOCK_GAP_EVENT:       lp->blkGap.fetch_add(cnt, std::memory_order_relaxed); break;
                }
            }
            continue;
        }

        if (events[i]->type != AOO_SOURCE_FORMAT_EVENT) continue;
        auto* e = (const aoo_source_event*)events[i];

        // Peer passed directly: the caller's snapshot shared_ptr keeps it alive across handle_events, even
        // against removePeer.
        HubPeer* peerPtr = ctx->peer;
        if (!peerPtr || !peerPtr->sink) continue;

        // Pull the new format from the sink. Returns >0 on success.
        aoo_format_storage f{};
        if (peerPtr->sink->get_source_format(e->endpoint, e->id, f) <= 0) {
            std::cerr << "[HUB-FMT] peer='" << peerPtr->userName
                      << "' AOO_SOURCE_FORMAT_EVENT but get_source_format failed\n";
            continue;
        }

        {   // Capability detection from the player's own stream — a build that masks
            // its sends can decode masked feeds. Hub-driven, so it does not depend on invite/format-request ordering.
            int32_t pf = 0;
            if (hubIdleMaskEnabled() && peerPtr->sink->get_sourceoption(e->endpoint, e->id, aoo_opt_protocol_flags, &pf, sizeof(pf)) > 0
                && (pf & AOO_PROTOCOL_FLAG_IDLE_MASK) && !peerPtr->capIdleMask.exchange(true)) {
                std::cout << "[HUB-CAP] peer='" << peerPtr->userName << "' decodes idle-masked blocks (stream flags 0x" << std::hex << pf << std::dec << "): enabling on its feeds\n";
                std::lock_guard<std::mutex> fl(peerPtr->outFeedsLock);   // per-peer lock only (never mPeerMutex here)
                int32_t wf = AOO_PROTOCOL_FLAG_COMPACT_DATA | AOO_PROTOCOL_FLAG_IDLE_MASK;
                for (auto& kv : peerPtr->outFeeds)
                    if (kv.second && kv.second->source)
                        kv.second->source->set_sinkoption(peerPtr->persistentEndpoint, kv.second->clientOurId, aoo_opt_protocol_flags, &wf, sizeof(wf));
            }
        }
        int32_t newCh = f.header.nchannels;
        if (newCh < 1) newCh = 1;
        if (newCh > HUB_MAX_CHANNELS_PER_PEER) {
            std::cerr << "[HUB-FMT] peer='" << peerPtr->userName << "' source format declares "
                      << newCh << " channels, capping at " << HUB_MAX_CHANNELS_PER_PEER << "\n";
            newCh = HUB_MAX_CHANNELS_PER_PEER;
        }
        {   // The stage's channel limit: only the first channels are received and passed on
            const int cap = ctx->hub->mStageMaxChannels.load(std::memory_order_relaxed);
            if (cap > 0 && newCh > cap) {
                std::cout << "[HUB-FMT] peer='" << peerPtr->userName << "' sends " << newCh
                          << " channels, stage limit " << cap << ": passing on the first " << cap << "\n";
                newCh = cap;
            }
        }

        if (newCh == peerPtr->recvChannels) {
            // Sender's channel count unchanged on the SINK side, but we still
            // need to check every listener's feed for this sender — a listener
            // may have allocated a feed at a stale channel count (e.g., during
            // a race where /sb/hxfeedreq arrived before sender's format event
            // updated recvChannels). Walk and rebuild any wrong-ch feeds.
            std::cout << "[HUB-FMT] peer='" << peerPtr->userName << "' format event nch="
                      << newCh << " sr=" << f.header.samplerate
                      << " bs=" << f.header.blocksize
                      << " codec='" << f.header.codec << "'"
                      << " (unchanged channel count, sweeping listener feeds)\n";
            ctx->hub->rebuildListenerFeedsForSender(peerPtr, newCh, "sweep-rebuilt");
            continue;
        }

        int32_t oldCh = peerPtr->recvChannels;
        // Excludes the audio thread's sink->process for the brief moment we
        // re-setup. Audio thread takes peer->sinkLock around sink->process,
        // so the two operations serialize cleanly.
        {
            std::lock_guard<std::mutex> wlock(peerPtr->sinkLock);
            peerPtr->recvChannels = newCh;
            peerPtr->uiRecvChannels.store(newCh, std::memory_order_relaxed);   // UI mirror
            peerPtr->sink->setup(ctx->hub->mSampleRate, ctx->hub->mBlockSize, newCh);
            // Preserve the peer's current (possibly adaptive)
            // buffer across format-driven sink re-setup.
            peerPtr->sink->set_buffersize((int32_t)(peerPtr->abufCurrentMs > 0
                ? peerPtr->abufCurrentMs : ctx->hub->sinkBufferMsFor(peerPtr)));
            peerPtr->sink->set_dynamic_resampling(hub_asrc_enabled());  // re-assert post-setup, see addPeer note
        }
        std::cout << "[HUB-FMT] peer='" << peerPtr->userName
                  << "' sink reconfigured " << oldCh << "ch -> " << newCh
                  << "ch (sr=" << f.header.samplerate << " bs=" << f.header.blocksize
                  << " codec='" << f.header.codec << "')\n";

        // A changed count must rebuild the per-(listener × sender) feed sources as well as the hub's sink, or
        // every listener keeps receiving the count the feed was allocated at until the sender rejoins. Rebuild
        // feeds before the userformat push below so the final /format the listener sees carries new nch and
        // new labels together.
        ctx->hub->rebuildListenerFeedsForSender(peerPtr, newCh, "rebuilt");

        // Userformat passthrough: cache the sender's userformat blob so any
        // listener feed for this sender can carry it through to the listener
        // client (channel labels, layout). Best-effort.
        char ubuf[1024];
        int32_t ulen = peerPtr->sink->get_sourceoption(e->endpoint, e->id,
                                                       aoo_opt_userformat,
                                                       ubuf, sizeof(ubuf));
        if (ulen > 0 && ulen <= (int32_t)sizeof(ubuf)) {
            // Unchanged blob -> nothing to push (each push re-sends /format to every listener)
            if (peerPtr->userformatBlob.size() == (size_t) ulen
                && std::equal(peerPtr->userformatBlob.begin(), peerPtr->userformatBlob.end(), ubuf)) {
                continue;
            }
            peerPtr->userformatBlob.assign(ubuf, ubuf + ulen);
            std::cout << "[HUB-FMT] peer='" << peerPtr->userName
                      << "' cached userformat blob size=" << ulen << "\n";
            // Push the new userformat into every listener's feed source for
            // this sender. Walk all other peers, look up their outFeeds[me].
            std::vector<std::shared_ptr<HubPeer>> snap;
            {
                std::shared_lock<std::shared_mutex> plock(ctx->hub->mPeerMutex);
                for (auto& p : ctx->hub->mPeers) if (p) snap.push_back(p);
            }
            for (auto& listener : snap) {
                if (!listener || listener->id == peerPtr->id) continue;
                std::lock_guard<std::mutex> flock(listener->outFeedsLock);
                auto it = listener->outFeeds.find(peerPtr->id);
                if (it == listener->outFeeds.end() || !it->second || !it->second->source) continue;
                std::lock_guard<std::mutex> flock2(it->second->lock);
                it->second->source->set_option(aoo_opt_userformat,
                                               peerPtr->userformatBlob.data(),
                                               (int32_t)peerPtr->userformatBlob.size());
            }
        }
    }
    return 1;
}

// Re-setup every listener's per-(L,S) feed source for `sender` at `newCh` channels. Called only from
// the sink format-event handler (sendThread, 1 ms cadence), never from the audio, listener or pacer
// threads. Lock order: mPeerMutex(shared) for the peer snapshot, released, then listener->outFeedsLock
// (map lookup), then feed->lock (serializes with that listener thread's feed->source->process for the
// single block it collides with). No lock or allocation lands on any RT path; the RT side reads
// feed->channels under feed->lock. Never nests outFeedsLock -> mPeerMutex (deadlock).
//
// AOO mechanics this relies on (deps/aoo/lib/src/source.cpp): source::setup() and set_format() keep
// the sink list, re-salt and mark format_changed, so the listener's sink gets a fresh /format with the
// new nchannels, its handle_format() reconfigures the decoder and fires AOO_SOURCE_FORMAT_EVENT, and
// the client (SonobusPluginProcessor.cpp AOO_SOURCE_FORMAT_EVENT case) re-setups its oursink and
// channel groups, same as stock P2P.
void SonobusHubServer::rebuildListenerFeedsForSender(HubPeer* sender, int32_t newCh,
                                                     const char* tag) {
    if (!sender) return;
    std::vector<std::shared_ptr<HubPeer>> snap;
    snap.reserve(HUB_MAX_PEERS);
    {
        std::shared_lock<std::shared_mutex> plock(mPeerMutex);
        for (auto& p : mPeers) if (p) snap.push_back(p);
    }
    int rebuilt = 0;
    for (auto& listener : snap) {
        if (!listener || listener->id == sender->id) continue;
        std::lock_guard<std::mutex> flock(listener->outFeedsLock);
        auto it = listener->outFeeds.find(sender->id);
        if (it == listener->outFeeds.end() || !it->second || !it->second->source) continue;
        HubOutFeed* feed = it->second.get();
        std::lock_guard<std::mutex> flock2(feed->lock);
        if (feed->channels == newCh) continue;
        int32_t prevCh = feed->channels;
        feed->source->setup(mSampleRate, mBlockSize, newCh);
        feed->source->set_buffersize((int32_t)HUB_SOURCE_BUFFERTIME_MS);
        feed->source->set_dynamic_resampling(hub_asrc_enabled());  // re-assert post-setup, see addPeer note
        setupSourceFormatFor(feed->source.get(), newCh);
        feed->channels = newCh;
        ++rebuilt;
        std::cout << "[HUB-FEED] " << tag << " feed listener='" << listener->userName
                  << "' sender='" << sender->userName << "' "
                  << prevCh << "ch -> " << newCh << "ch\n";
    }
    if (rebuilt > 0) {
        std::cout << "[HUB-FMT] peer='" << sender->userName << "' " << rebuilt
                  << " listener feed(s) now at " << newCh << "ch\n";
    }
}

// Polls every peer's sink for AOO events. Called from sendThread on its
// existing 1ms cadence. Each handle_events() call dispatches synchronously
// into hub_sink_event_handler, so by the time this returns all pending
// format-change events have been applied.
void SonobusHubServer::pollPeerSinkEvents() {
    // Snapshot peers under mPeerMutex briefly; handle_events runs without
    // holding mPeerMutex so format reconfiguration can take the per-peer
    // sinkLock without nested-lock surprises.
    std::vector<std::shared_ptr<HubPeer>> snap;
    snap.reserve(HUB_MAX_PEERS);
    {
        std::shared_lock<std::shared_mutex> lock(mPeerMutex);
        for (auto& p : mPeers) {
            if (p && p->sink) snap.push_back(p);
        }
    }
    for (auto& peer : snap) {
        if (!peer->sink) continue;
        if (peer->sink->events_available() > 0) {
            PeerSinkEventCtx ctx{this, peer.get()};
            peer->sink->handle_events(hub_sink_event_handler, &ctx);
        }
        // Sample jitter buffer fill at 1ms cadence
        // and store peak-since-last-GUI-read in an atomic per peer. The GUI
        // reads + resets this on each snapshot. Catches transient peaks the
        // 5Hz GUI sampling would otherwise miss.
        if (peer->persistentEndpoint) {
            constexpr int32_t kSbClientOurId = 1;
            float ratio = 0.0f;
            int32_t sz = (int32_t)sizeof(ratio);
            if (peer->sink->get_sourceoption(peer->persistentEndpoint, kSbClientOurId,
                                             aoo_opt_buffer_fill_ratio, &ratio, sz) > 0) {
                float pct = ratio * 100.0f;
                float cur = peer->jitterBufferPeakFillPct.load(std::memory_order_relaxed);
                while (pct > cur && !peer->jitterBufferPeakFillPct.compare_exchange_weak(
                                            cur, pct, std::memory_order_relaxed)) {}
            }
            if (peer->isLan) {   // The LAN sink sets its own depth: keep the live figure for the latency display
                int32_t db = 0; int32_t dsz = (int32_t) sizeof(db);
                if (peer->sink->get_sourceoption(peer->persistentEndpoint, kSbClientOurId, aoo_opt_depth_blocks, &db, dsz) > 0 && db > 0)
                    peer->lanDepthBlocks.store(db, std::memory_order_relaxed);
            }
            // Peak resampler backlog (ms), the last unmeasured queue in the sink pipeline.
            float rsamp = 0.0f; int32_t rsz = (int32_t)sizeof(rsamp);
            if (peer->sink->get_sourceoption(peer->persistentEndpoint, kSbClientOurId,
                                             aoo_opt_resampler_backlog, &rsamp, rsz) > 0) {
                float rms2 = rsamp * 1000.0f / (float)std::max(1, mSampleRate);
                float cur2 = peer->resamplerPeakMs.load(std::memory_order_relaxed);
                while (rms2 > cur2 && !peer->resamplerPeakMs.compare_exchange_weak(
                                            cur2, rms2, std::memory_order_relaxed)) {}
            }
        }
    }
}

// Per-feed source events. Each outbound feed is an aoo::isource the hub uses
// to push one sender's audio to one listener. The listener's local sink pings
// our source on every cycle; AOO surfaces that round-trip as AOO_PING_EVENT
// on the source (with tt3 populated — see deps/aoo/lib/aoo/aoo.h:236-242).
// We use it for the listener's smoothedRttMs displayed in the GUI.
//
// Latency note: off the audio thread entirely. Called from sendThread (1ms
// cadence). Writes to peer->smoothedRttMs (relaxed atomic, read only by JUCE
// timer on the message thread). No new lock on audio path.
int32_t SonobusHubServer::hub_source_event_handler(void* user, const aoo_event** events, int32_t n) {
    auto* ctx = static_cast<FeedSourceEventCtx*>(user);
    if (!ctx || !ctx->hub) return 0;

    for (int32_t i = 0; i < n; ++i) {
        if (!events[i]) continue;
        if (events[i]->type != AOO_PING_EVENT) continue;
        auto* e = (const aoo_ping_event*)events[i];

        double rttMs = aoo_osctime_duration(e->tt1, e->tt3) * 1000.0;
        if (!(rttMs >= 0.0 && rttMs < 5000.0)) continue;  // sanity-clamp out NaN/garbage

        // Deadlock fix: listener passed directly; NO locks here.
        HubPeer* listener = ctx->listener;
        if (!listener) continue;

        // Median of the last 5 samples, not an EMA: this path is the main writer of smoothedRttMs, and one
        // reply delayed by a player stall must not inflate the displayed RTT for several seconds.
        listener->smoothedRttMs.store(HubPeer::medianPush(listener->rttRing, listener->rttRingIdx, (float) rttMs), std::memory_order_relaxed);
    }
    return 1;
}

void SonobusHubServer::pollPeerSourceEvents() {
    // Snapshot listeners under mPeerMutex; release lock before
    // feed->source->handle_events (which dispatches synchronously into
    // hub_source_event_handler — that re-acquires mPeerMutex only briefly
    // to look up the listener peer for the RTT EMA store).
    std::vector<std::shared_ptr<HubPeer>> snap;
    snap.reserve(HUB_MAX_PEERS);
    {
        std::shared_lock<std::shared_mutex> lock(mPeerMutex);
        for (auto& p : mPeers) {
            if (p) snap.push_back(p);
        }
    }
    for (auto& listener : snap) {
        if (!listener) continue;
        std::lock_guard<std::mutex> mlock(listener->outFeedsLock);
        for (auto& kv : listener->outFeeds) {
            auto& feed = kv.second;
            if (!feed || !feed->source) continue;
            if (feed->source->events_available() <= 0) continue;
            FeedSourceEventCtx ctx{this, listener.get()};
            feed->source->handle_events(hub_source_event_handler, &ctx);
        }
    }
}

// Audio processing: runs at a fixed interval matching the block size.
// For each block:
//   1) Process all sinks (receive audio from each peer)
//   2) For each peer, compute mix-minus-self (sum of all OTHER peers' audio)
//   3) Feed each peer's source with their personalized mix
void SonobusHubServer::audioProcessThread() {
    const double blockDurationMs = 1000.0 * mBlockSize / mSampleRate;
    const uint64_t blockNs = (uint64_t)(blockDurationMs * 1e6);

    hub_tb_t tb;
    hub_timebase(&tb);
    auto nsToMachTicks = [&tb](uint64_t ns) -> uint32_t {
        return (uint32_t)((ns * (uint64_t)tb.denom) / (uint64_t)tb.numer);
    };
#if defined(__APPLE__)
    // Real-time scheduling via Mach THREAD_TIME_CONSTRAINT_POLICY.
    // Same primitive CoreAudio uses for its render thread; far stronger guarantee
    // than QOS_CLASS_USER_INTERACTIVE, which is just a scheduler hint.
    {
        thread_time_constraint_policy_data_t policy;
        policy.period      = nsToMachTicks(blockNs);
        policy.computation = nsToMachTicks(blockNs / 2);   // expected work per block
        policy.constraint  = nsToMachTicks(blockNs);       // hard deadline = full period
        policy.preemptible = 0;
        kern_return_t kr = thread_policy_set(
            pthread_mach_thread_np(pthread_self()),
            THREAD_TIME_CONSTRAINT_POLICY,
            (thread_policy_t)&policy,
            THREAD_TIME_CONSTRAINT_POLICY_COUNT);
        if (kr != KERN_SUCCESS) {
            std::cerr << "[HUB] WARNING: thread_policy_set TIME_CONSTRAINT failed (kr="
                      << kr << "), falling back to QOS_USER_INTERACTIVE\n";
            pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
        } else {
            std::cout << "[HUB] audio thread set to TIME_CONSTRAINT realtime (period="
                      << blockNs << "ns)\n";
        }
    }
#elif defined(__linux__)
    hub_set_fifo(1, "audio thread");
#endif
    // Hub clock lock (SB_HUB_CLOCK_LOCK, SB_HUB_CLOCK_REF=<sender name>).
    // The mix loop runs on mach time while every client consumes on its audio crystal, so a rate offset
    // accumulates in client buffers and the depth governor splices it out as audible blips. Discipline the
    // block period to the reference sender's device clock: every 2-s stats tick, compare the sender's data
    // sequence number (aoo_opt_newest_seq, a lock-free atomic read through the hub's own sink) with blocks
    // produced over the baseline (SB_HUB_CLOCK_BASE_S), integrate the ppm error (gain per step
    // SB_HUB_CLOCK_GAIN_PCT), clamp +/-SB_HUB_CLOCK_MAX_PPM. Inbound packet counts are no use as the
    // reference: they track the hub's own production.
    // The deadline is kept in fractional ticks (double accumulator, rounded only at the wait), since
    // truncating the block period to whole mach ticks is itself a 62.5 ppm error (32/48000 s at 24 MHz is
    // 16000 ticks, 15999 after truncating blockNs), and so the clock lock can trim the period continuously.
    const double blockTicksF0 = ((double)mBlockSize / (double)std::max(1, mSampleRate)) * 1e9
                                * (double)tb.denom / (double)tb.numer;
    double blockTicksF = blockTicksF0;
    double deadlineF = 0.0;
    const uint64_t blockTicksNominal = (uint64_t) llround(blockTicksF0);
    uint64_t blockTicks = blockTicksNominal;
    std::cout << "[HUB-CLOCK] block period " << blockTicksF0 << " mach ticks (nominal int " << blockTicksNominal
              << ", old truncated " << nsToMachTicks(blockNs) << ")\n";
    // The clock lock (fast arrival regression + 180-s count trim) is on by code default, so it does not
    // depend on a per-host tap.
    static const int clockLockOn   = hub_tap_int("SB_HUB_CLOCK_LOCK", 1);
    static const int clockMaxPpm   = hub_tap_int("SB_HUB_CLOCK_MAX_PPM", 300);
    static const int clockGainPct  = hub_tap_int("SB_HUB_CLOCK_GAIN_PCT", 60);
    static const int clockBaseSteps = std::max(5, hub_tap_int("SB_HUB_CLOCK_BASE_S", 180) / 2);   // stats ticks (2 s) per baseline
    static const std::string clockRef = hub_tap_str("SB_HUB_CLOCK_REF", "");
    double   clockAdjPpm = 0.0;          // integrated correction applied to the block period (+ = slower)
    uint64_t clockBaseRx = 0, clockBaseProduced = 0; int clockBaseTicks = 0; uint64_t clockProduced = 0;
    // Fast lock: SB_HUB_CLOCK_FAST=1 (default) applies the recv thread's arrival-regression period
    // (20-s windows) — first window 100 %, then SB_HUB_CLOCK_FAST_GAIN_PCT (60). The 180-s count lock stays as a
    // slow trim (its err reads ~0 once the fast lock has landed).
    static const int clockFastOn      = hub_tap_int("SB_HUB_CLOCK_FAST", 1);
    static const int clockFastGainPct = hub_tap_int("SB_HUB_CLOCK_FAST_GAIN_PCT", 60);
    uint32_t clockFastGenSeen = 0; int clockFastApplied = 0; double clockFastPrevPer = 0.0;
    std::chrono::steady_clock::time_point clockBaseWall{};
    std::cout << "[HUB-CLOCK] lock " << (clockLockOn ? "ON" : "off") << " ref='" << clockRef << "' max_ppm=" << clockMaxPpm << " gain=" << clockGainPct << "%\n";
    // Catch-up resync tolerance as wall time, not blocks, so it means the same at every blocksize:
    // ordinary scheduler hiccups must not trigger the full deadline resync + synthetic-clock re-anchor
    // (a production gap). Converted to mach ticks once here, outside the RT loop.
    const uint64_t catchupSlipTicks =
        12000000ULL * (uint64_t)tb.denom / (uint64_t)tb.numer;

    // Per-peer decoded audio buffers live on each HubPeer (peer->decodedBlock), not on this thread's stack,
    // so per-listener threads (HubPeer::listenerThread) can read other peers' decoded blocks during
    // fan-out. peer->decodedBlock is allocated in addPeer().

    std::cout << "[HUB] Audio thread started (sr=" << mSampleRate
              << " bs=" << mBlockSize << " ch=" << mChannels
              << " period=" << blockDurationMs << "ms)\n";

    // Per-peer audio flow counters, logged every 2 seconds. Indexed by the
    // audio thread's current snapshot slot (mCurrentSnap[i]).
    std::vector<uint64_t> peerRxBlocks(HUB_MAX_PEERS, 0);
    // Chirp-transit detector. Timestamps the mach time when each peer's decoded audio goes loud after
    // >=700 ms of quiet (suits a 0.5 s chirp / 2 s period probe). Lets offline analysis split a measured
    // RTT into per-leg transit times through the hub. Audio-thread cost: reuses the existing per-block
    // peak scan + one compare + rare relaxed store.
    std::vector<uint64_t> peerLastLoudTicks(HUB_MAX_PEERS, 0);
    std::vector<uint64_t> peerChirpPrinted(HUB_MAX_PEERS, 0);
    std::vector<uint64_t> peerRxNonZeroBlocks(HUB_MAX_PEERS, 0);
    auto lastStatsTime = std::chrono::steady_clock::now();

    // Audio-thread-owned snapshot buffers, sized once. Stored as members (mCurrentSnap / mCurrentCoefs /
    // mCurrentListenerSolo) so per-listener threads can read them while we wait at the fork-join barrier.
    // Safe because we only mutate these between blocks (writes during Phase A, reads during Step 2
    // fan-out, never both simultaneously per peer).
    mCurrentSnap.assign(HUB_MAX_PEERS, nullptr);
    mCurrentCoefs.assign(HUB_MAX_PEERS * HUB_MAX_PEERS, HubMixOverride{});
    mCurrentListenerSolo.assign(HUB_MAX_PEERS, 0);
    mCurrentNumPeers = 0;
    for (int i = 0; i < HUB_MAX_PEERS; ++i) mCurrentIds[i] = kNoPeerId;
    // Last-seen generations. ~0 forces the first
    // block to build the snapshot + coefficient matrix.
    uint64_t audioPeerGen = ~0ULL;
    uint64_t audioMixGen  = ~0ULL;
    // Barrier pause-spin bound (mach ticks). 20µs
    // covers the fastest fan-outs without ever polling the scheduler; past
    // it we park on mBarrierCv and the last listener wakes us.
    const uint64_t barrierSpinTicks = 20000ULL * (uint64_t)tb.denom / (uint64_t)tb.numer;
    mBarrierPending.store(0, std::memory_order_relaxed);
    // --inline-listeners governor state. Budget
    // = 60% of this thread's P/2 computation claim (blockNs/2). Window stats
    // are plain locals (audio thread only); published to atomics once per
    // window for the stats thread's [HUB-INLINE] line. Scratch is pre-sized
    // here, outside the loop, so inline pumping never allocates.
    const uint64_t inlineBudgetNs = (blockNs / 2) * 60ULL / 100ULL;
    uint64_t inlineWinMaxTicks = 0;
    int      inlineWinPeersMax = 0;
    uint32_t inlineWinBlocks   = 0;
    int      inlineOverWindows = 0;
    mInlineMeasBudgetNs.store(inlineBudgetNs, std::memory_order_relaxed);
    if (mInlineActive.load(std::memory_order_relaxed)) listenerScratchWarm(mBlockSize);

    // Pace using kernel-level absolute-deadline waits.
    // macOS: mach_wait_until — same primitive CoreAudio uses; sub-100µs jitter.
    // Linux: clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME) — same approach.
    uint64_t nextDeadlineMach = hubNowTicks() + blockTicks;   // Same code on macOS and Linux (hub ticks)
    deadlineF = (double) nextDeadlineMach;

    // Synthetic monotonic feed clock. The outbound feeds are driven by our fixed-cadence pump, so their
    // timeline is one block period per block: t = base + blockIndex * blockPeriod. Raw aoo_osctime_get()
    // here would feed every wall-clock or scheduler hiccup into all feed sources' DLL timers, and a
    // hiccup over ~43 ms would make them emit empty "dropped" blocks that disrupt every listener's sink.
    //   - aoo osctime is NTP 32.32 fixed-point packed in a uint64 (see
    //     deps/aoo/lib/src/time.hpp): integer seconds in the high 32 bits,
    //     1/2^32-sec fraction in the low 32. Adding two such uint64s IS
    //     fixed-point addition (fraction carry propagates into seconds), so
    //     base + n*period needs no helper from the aoo lib.
    //   - blockPeriod in 2^-32 s units = (blockSize << 32) / sampleRate.
    //     Rounding error ≤1 unit/block ≈ 6e-5 ppm.
    //   - Base is wall time captured once here at audio-pump start, so the
    //     synthetic timeline stays anchored near real NTP time; it is
    //     re-anchored on the catch-up resync below (the hub's only
    //     "audio restart" analog: it has no CoreAudio device, the pump IS
    //     the audio callback, and this function restarts with the thread).
    // Real aoo_osctime_get() stays in place for sink->process() (inbound timing genuinely reflects
    // arrival wall time) and all protocol paths.
    uint64_t feedBaseOscTime = aoo_osctime_get();
    uint64_t feedBlockIndex = 0;
    const uint64_t feedBlockPeriodOsc =
        ((uint64_t)mBlockSize << 32) / (uint64_t)std::max(1, mSampleRate);

    // Per-block timing via mach_absolute_time.
    // CAS-loop max updaters keep multiple atomic stores per block lock-free.
    auto updateMax = [](std::atomic<uint64_t>& m, uint64_t v) {
        uint64_t cur = m.load(std::memory_order_relaxed);
        while (v > cur && !m.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {}
    };
    auto ticksToNs = [&tb](uint64_t ticks) -> uint64_t {
        return ticks * (uint64_t)tb.numer / (uint64_t)tb.denom;
    };

    uint64_t prevBlockStartTicks = 0;   // Block-timing instrument
    while (mRunning.load()) {
        uint64_t t = aoo_osctime_get();
        uint64_t blockStartTicks = hubNowTicks();
        if (prevBlockStartTicks) {   // Worst gap + late starts, relaxed atomics only
            const uint64_t gapNs = ticksToNs(blockStartTicks - prevBlockStartTicks);
            updateMax(mAudioGapNsMax, gapNs);
            if (gapNs > blockNs + blockNs / 2) mAudioLateBlocks.fetch_add(1, std::memory_order_relaxed);
        }
        prevBlockStartTicks = blockStartTicks;

        // ----- Phase A: snapshot peers + per-listener mix coefficients.
        // Hold mPeerMutex JUST long enough to copy shared_ptrs and flatten the
        // listener×sender override map. Per-listener threads run lock-free
        // against these snapshots; recv thread can dispatch incoming UDP in
        // parallel. mCurrent* members are read by per-listener threads during
        // fan-out — safe because audio thread blocks at the fork-join barrier
        // before overwriting them on the next block.
        uint64_t phaseATicks = hubNowTicks();
        // The snapshot copy and the N² flatten are expensive per block (N shared_ptr copies, N² string-keyed
        // hash lookups, rwlock contention with the recv thread's per-packet shared_lock), so they are gated
        // on two generation counters: mPeerListGen (membership / addr, via rebuildPeerSnapshotLocked) and
        // mMixGen (any mixOverrides write). Steady state = two acquire loads + two compares; the lock and the
        // rebuild only run when something actually changed.
        //
        // Equivalence with a per-block rebuild: the only inputs are (a) mPeers membership/order, published via
        // rebuildPeerSnapshotLocked -> mPeerListGen, and (b) each listener's mixOverrides, whose sole writer
        // bumps mMixGen under the same exclusive lock. Both gens are re-read under the shared lock, so a
        // writer cannot slip a change in between our gen read and our copy. Safe to rewrite here: we are past
        // the previous block's barrier, so no listener thread is reading.
        {
            const uint64_t pg = mPeerListGen.load(std::memory_order_acquire);
            const uint64_t mg = mMixGen.load(std::memory_order_acquire);
            if (pg != audioPeerGen || mg != audioMixGen) {
                std::shared_lock<std::shared_mutex> lock(mPeerMutex);
                audioPeerGen = mPeerListGen.load(std::memory_order_relaxed);  // under lock: exact
                audioMixGen  = mMixGen.load(std::memory_order_relaxed);
                const auto& snapPeers = mPeerSnapshot->peers;
                mCurrentNumPeers = std::min((int)snapPeers.size(), HUB_MAX_PEERS);
                for (int i = 0; i < mCurrentNumPeers; ++i) {
                    mCurrentSnap[i] = snapPeers[i];  // shared_ptr copy keeps peer alive past removePeer
                    mCurrentIds[i] = mCurrentSnap[i] ? mCurrentSnap[i]->id : kNoPeerId;
                }
                // Reset any trailing slots from a prior larger block so stale
                // shared_ptrs don't linger and pin destroyed peers in memory.
                for (int i = mCurrentNumPeers; i < HUB_MAX_PEERS; ++i) {
                    mCurrentSnap[i].reset();
                    mCurrentIds[i] = kNoPeerId;
                }
                // Flatten mixOverrides → mCurrentCoefs. Defaults (1.0 gain, no
                // mute/solo) preserve mix-minus-self when there's no override.
                for (int i = 0; i < mCurrentNumPeers; ++i) {
                    bool anySolo = false;
                    const auto& listener = mCurrentSnap[i];
                    for (int j = 0; j < mCurrentNumPeers; ++j) {
                        HubMixOverride& slot = mCurrentCoefs[i * HUB_MAX_PEERS + j];
                        slot = HubMixOverride{};
                        if (j == i || !listener || !mCurrentSnap[j]) continue;
                        auto it = listener->mixOverrides.find(mCurrentSnap[j]->userName);
                        if (it != listener->mixOverrides.end()) {
                            slot = it->second;
                            if (slot.soloed) anySolo = true;
                        }
                    }
                    mCurrentListenerSolo[i] = anySolo ? 1u : 0u;
                }
                // Acknowledge the gen now in
                // use. Ordered AFTER the mCurrentSnap rewrite above, so the
                // refs just dropped were never the last ones (the retired
                // snapshot still pins them until this ack is past its gen).
                // One release store on this rare (membership/mix change) path.
                mAudioSnapAck.store(audioPeerGen, std::memory_order_release);
            }
        }
        // ----- mPeerMutex not held. Below is lock-free against the snapshot.
        // Publish the SYNTHETIC feed timestamp for
        // this block (see comment at feedBaseOscTime above). Written before
        // the listener-thread fan-out is triggered — same publication rule as
        // the other mCurrent* members. Cost: one mul + add + store.
        mFeedOscTime = feedBaseOscTime + feedBlockIndex * feedBlockPeriodOsc;
        ++feedBlockIndex;
        const int numPeers = mCurrentNumPeers;
        uint64_t phaseAEndTicks = hubNowTicks();

        // Step 1: Receive audio from all peers. Takes a shared lock on each
        // peer's sinkLock so a concurrent sink->setup() (from the format-event
        // handler in sendThread) is serialized after this block's process.
        // Writes decoded samples to peer->decodedBlock so the per-listener
        // threads can read them during Step 2 fan-out.
        uint64_t sinkLoopStartTicks = hubNowTicks();
        uint64_t perPeerSinkMaxTicks = 0;
        for (int i = 0; i < numPeers; ++i) {
            auto& peer = mCurrentSnap[i];
            if (!peer || !peer->active || !peer->sink) continue;

            // Never block here: the stats thread holds sinkLock across set_buffersize, and waiting would cost
            // every listener a drain on each hold resize. If this peer's sink is busy, skip its block this tick
            // (listeners re-read the last block of this peer only) and count it; everyone else's block goes out
            // on time.
            std::unique_lock<std::mutex> rlock(peer->sinkLock, std::try_to_lock);
            if (!rlock.owns_lock()) { mAudioSinkSkips.fetch_add(1, std::memory_order_relaxed); continue; }
            const int peerCh = std::max(1,
                                        std::min((int)peer->recvChannels,
                                                 HUB_MAX_CHANNELS_PER_PEER));

            // Defensive: ensure decodedBlock + decodedPtrs are sized. addPeer
            // pre-allocates these; we only re-allocate if HUB_MAX_CHANNELS_PER_PEER
            // changes (won't happen at runtime). No allocation in steady state.
            if ((int)peer->decodedBlock.size() < mBlockSize * HUB_MAX_CHANNELS_PER_PEER) {
                peer->decodedBlock.assign(mBlockSize * HUB_MAX_CHANNELS_PER_PEER, 0.0f);
            }
            if ((int)peer->decodedPtrs.size() < HUB_MAX_CHANNELS_PER_PEER) {
                peer->decodedPtrs.resize(HUB_MAX_CHANNELS_PER_PEER);
                for (int ch = 0; ch < HUB_MAX_CHANNELS_PER_PEER; ++ch) {
                    peer->decodedPtrs[ch] = peer->decodedBlock.data() + ch * mBlockSize;
                }
            }

            // Decode into the NON-published
            // slot, publish after the block is complete. Readers (per-listener
            // feed threads) only ever see finished blocks.
            uint32_t seqNow = peer->decodedSeq.load(std::memory_order_relaxed);
            int wslot = (int)((seqNow + 1) & 1);
            auto& wblock = peer->decodedBlockSlots[wslot];
            auto& wptrs  = peer->decodedPtrSlots[wslot];
            if ((int)wblock.size() < mBlockSize * HUB_MAX_CHANNELS_PER_PEER) {
                wblock.assign((size_t)mBlockSize * HUB_MAX_CHANNELS_PER_PEER, 0.0f);
                wptrs.resize(HUB_MAX_CHANNELS_PER_PEER);
                for (int ch = 0; ch < HUB_MAX_CHANNELS_PER_PEER; ++ch)
                    wptrs[ch] = wblock.data() + (size_t)ch * mBlockSize;
            }
            std::fill(wblock.begin(),
                      wblock.begin() + (size_t)mBlockSize * (size_t)peerCh,
                      0.0f);

            uint64_t sinkCallStart = hubNowTicks();
            peer->sink->process(wptrs.data(), mBlockSize, t);
            uint64_t sinkCallDur = hubNowTicks() - sinkCallStart;
            if (sinkCallDur > perPeerSinkMaxTicks) perPeerSinkMaxTicks = sinkCallDur;
            // Publish channel count for this block — per-listener readers use
            // this to know how many channels of valid audio are in decodedBlock.
            peer->decodedChannels = peerCh;
            peer->decodedSeq.store(seqNow + 1, std::memory_order_release);  // publish completed block

            // Capture this peer's decoded
            // audio. Active only during an explicit --dump-peer window.
            if (mDumpFile && mDumpFramesLeft.load(std::memory_order_relaxed) > 0
                && peer->userName == mDumpPeerName) {
                float tmp[2 * 1024];
                int n = std::min(mBlockSize, 1024);
                for (int f2 = 0; f2 < n; ++f2) {
                    tmp[2*f2]   = wptrs[0][f2];
                    tmp[2*f2+1] = (peerCh > 1 ? wptrs[1][f2] : wptrs[0][f2]);
                }
                fwrite(tmp, sizeof(float), (size_t)n * 2, mDumpFile);
                mDumpFramesLeft.fetch_sub(n, std::memory_order_relaxed);
            }

            // Track audio flow: count blocks and check for non-silent audio
            // across ALL the peer's actual channels (drummer's 8 mics, etc.).
            peerRxBlocks[i]++;
            float peak = 0.0f;
            for (int ch = 0; ch < peerCh; ++ch) {
                for (int s = 0; s < mBlockSize; ++s) {
                    float v = std::fabs(wptrs[ch][s]);
                    if (v > peak) peak = v;
                }
            }
            if (peak > 0.0001f) {
                peerRxNonZeroBlocks[i]++;
            }
            // Chirp onset (see decl above). 700ms quiet gap.
            if (peak > 0.05f) {
                const uint64_t quietGapTicks =
                    700000000ULL * (uint64_t)tb.denom / (uint64_t)tb.numer;
                if (peerLastLoudTicks[i] == 0
                    || blockStartTicks - peerLastLoudTicks[i] > quietGapTicks) {
                    peer->chirpOnsetTicks.store(blockStartTicks, std::memory_order_relaxed);
                }
                peerLastLoudTicks[i] = blockStartTicks;
            }
        }

        uint64_t sinkLoopEndTicks = hubNowTicks();

        // Step 2 (per-listener fan-out): trigger each active listener's dedicated thread to pump its outFeeds
        // in parallel, then wait at the fork-join barrier until all have signaled done. Wall-clock cost of
        // this step is O(max-per-listener), not O(sum-of-all-listeners), which breaks the N²-feed
        // serialization.
        uint64_t feedPumpStartTicks = hubNowTicks();

        uint64_t perFeedSourceMaxTicks = 0;
        // --inline-listeners: do every listener's Step-2 work right here, sequentially, instead of signalling
        // N listener RT threads and waiting at the barrier. Lock discipline is identical to
        // listenerProcessThread: outFeedsLock held across pump (+ cut-through emit); feed->lock per feed
        // inside. Saved per block: N cv notifies, N RT wakes, the barrier spin/park, N fetch_subs.
        // RT: no allocation (scratch pre-sized by listenerScratchWarm), no I/O except the sendto's cut-through
        // would otherwise issue from a listener thread, 2 mach_absolute_time reads + 1 relaxed store per
        // listener, a handful of relaxed atomics per window.
        if (mInlineActive.load(std::memory_order_acquire)) {
            const bool cutThroughNow = mCutThrough.load(std::memory_order_relaxed);
            const bool inlineSendNow = !cutThroughNow && !mSendPacing.load(std::memory_order_relaxed);
            uint64_t worstListenerTicks = 0;
            int pumped = 0;
            for (int i = 0; i < numPeers; ++i) {
                auto& listener = mCurrentSnap[i];
                if (!listener || !listener->active) continue;
                const uint64_t l0 = hubNowTicks();
                uint64_t feedMax = 0;
                {
                    std::lock_guard<std::mutex> mlock(listener->outFeedsLock);
                    feedMax = listenerPumpFeeds(listener.get(), i, inlineSendNow);
                    if (cutThroughNow) listenerCutThroughEmit(listener.get());
                }
                const uint64_t ld = hubNowTicks() - l0;
                if (ld > worstListenerTicks) worstListenerTicks = ld;
                if (feedMax > perFeedSourceMaxTicks) perFeedSourceMaxTicks = feedMax;
                listener->listenerSourceMaxTicks.store(feedMax, std::memory_order_relaxed);
                ++pumped;
            }
            // Governor: report-only measurement; the DECISION (start threads,
            // flip the mode) is taken off-RT by the stats thread.
            if (pumped > 0) {
                if (worstListenerTicks > inlineWinMaxTicks) inlineWinMaxTicks = worstListenerTicks;
                if (pumped > inlineWinPeersMax) inlineWinPeersMax = pumped;
                if (++inlineWinBlocks >= kInlineWindowBlocks) {
                    const uint64_t perListenerNs = ticksToNs(inlineWinMaxTicks);
                    const uint64_t projectedNs   = perListenerNs * (uint64_t)inlineWinPeersMax;
                    mInlineMeasPerListenerMaxNs.store(perListenerNs, std::memory_order_relaxed);
                    mInlineMeasProjectedNs.store(projectedNs, std::memory_order_relaxed);
                    mInlineMeasPeersMax.store(inlineWinPeersMax, std::memory_order_relaxed);
                    mInlineMeasWindows.fetch_add(1, std::memory_order_relaxed);
                    if (projectedNs > inlineBudgetNs) {
                        // two consecutive over-budget windows → refuse the mode
                        if (++inlineOverWindows >= 2)
                            mInlineFallbackReq.store(true, std::memory_order_release);
                    } else {
                        inlineOverWindows = 0;
                    }
                    inlineWinMaxTicks = 0; inlineWinPeersMax = 0; inlineWinBlocks = 0;
                }
            }
        } else {
            // Snapshot which listener threads we're signaling (so we don't try
            // to wait on a listener whose thread was torn down between signal
            // and barrier).
            int triggered[HUB_MAX_PEERS];
            int triggeredCount = 0;
            for (int i = 0; i < numPeers; ++i) {
                auto& listener = mCurrentSnap[i];
                // listenerAlive, not the listenerThread unique_ptr, whose reset() from the removal path would race
                // this read. Cheap relaxed pre-filter here; the authoritative check is under listenerMutex below.
                if (!listener || !listener->active
                    || !listener->listenerAlive.load(std::memory_order_relaxed)) continue;
                // Reset per-block stat counter on the listener side.
                listener->listenerSourceMaxTicks.store(0, std::memory_order_relaxed);
                bool signaled = false;
                {
                    std::lock_guard<std::mutex> lk(listener->listenerMutex);
                    // The listener clears listenerAlive under THIS mutex on its
                    // way out and acknowledges any go it consumed, so: either we
                    // see alive==true and count it (it WILL ack — its exit path
                    // checks goGen != lastSeen under the mutex), or we see false
                    // and never count it. No third state → the barrier count is
                    // exact and cannot hang.
                    if (listener->listenerAlive.load(std::memory_order_relaxed)) {
                        // Count BEFORE the go. Ordered w.r.t. the listener's
                        // fetch_sub by this mutex's unlock→lock handoff, so
                        // relaxed is sufficient here.
                        mBarrierPending.fetch_add(1, std::memory_order_relaxed);
                        ++listener->listenerGoGen;
                        signaled = true;
                    }
                }
                if (signaled) {
                    listener->listenerCv.notify_one();
                    triggered[triggeredCount++] = i;
                }
            }

            // Barrier: one shared countdown (mBarrierPending). Bounded pause-spin (pure `yield` instruction, no
            // syscall) for ≤ barrierSpinTicks (~20µs), which catches the fastest fan-outs, then park on mBarrierCv;
            // the listener whose fetch_sub hits zero notifies. Cost per block: at most one cv wait + one wake. No
            // std::this_thread::yield() polling: on this preemptible=0 time-constraint thread each call is a
            // swtch_pri syscall that burns the pump's own RT budget.
            // Memory ordering: each listener's fetch_sub(acq_rel) is sequenced after all its fan-out work
            // (feed->source->process/send, listenerSourceMaxTicks store); our acquire load that observes 0 is the
            // tail of that release sequence (RMWs chain it across listeners), so everything every listener did
            // happens-before our next-block writes to mCurrent* and our listenerSourceMaxTicks reads below. The
            // cv path adds a mutex handoff on top: strictly stronger, never weaker.
            if (triggeredCount > 0) {
                const uint64_t spinUntil = hubNowTicks() + barrierSpinTicks;
                while (mBarrierPending.load(std::memory_order_acquire) != 0) {
                    if (hubNowTicks() >= spinUntil) {
                        std::unique_lock<std::mutex> lk(mBarrierMutex);
                        mBarrierCv.wait(lk, [this]() {
                            return mBarrierPending.load(std::memory_order_acquire) == 0;
                        });
                        break;
                    }
                    hub_cpu_pause();
                }
            }
            for (int idx = 0; idx < triggeredCount; ++idx) {
                auto& listener = mCurrentSnap[triggered[idx]];
                if (!listener) continue;
                uint64_t lmax = listener->listenerSourceMaxTicks.load(std::memory_order_relaxed);
                if (lmax > perFeedSourceMaxTicks) perFeedSourceMaxTicks = lmax;
            }

        }
        uint64_t feedPumpEndTicks = hubNowTicks();

        // Every feed has queued this block's
        // packets (barrier passed) — hand emission to the pacer. One mutex'd
        // increment + notify per block; the wait/stagger happens entirely on
        // the pacer thread, never here.
        if (mSendPacing.load(std::memory_order_relaxed)) {
            { std::lock_guard<std::mutex> lk(mPacerMutex); ++mPacerGen; }
            mPacerCv.notify_one();
        }

        // Per-block stats update. All atomics, no mutex, no I/O; the stats writer thread reads and flushes on
        // its own cadence. Keep this section deliberately cheap.
        {
            uint64_t blockTimeNs    = ticksToNs(feedPumpEndTicks - blockStartTicks);
            uint64_t snapNs         = ticksToNs(phaseAEndTicks   - phaseATicks);
            uint64_t sinkLoopNs     = ticksToNs(sinkLoopEndTicks - sinkLoopStartTicks);
            uint64_t feedPumpNs     = ticksToNs(feedPumpEndTicks - feedPumpStartTicks);
            uint64_t perPeerSinkNs  = ticksToNs(perPeerSinkMaxTicks);
            uint64_t perFeedSrcNs   = ticksToNs(perFeedSourceMaxTicks);

            mStats.totalBlocks.fetch_add(1, std::memory_order_relaxed);
            if (blockTimeNs > blockNs) {  // blockNs = deadline budget from top of function
                mStats.deadlineMisses.fetch_add(1, std::memory_order_relaxed);
            }
            updateMax(mStats.blockNsMax,         blockTimeNs);
            updateMax(mAudioWorkNsMax,           blockTimeNs);   // Windowed copy for [HUB-TIMING]
            updateMax(mStats.snapNsMax,          snapNs);
            updateMax(mStats.sinkLoopNsMax,      sinkLoopNs);
            updateMax(mStats.feedPumpNsMax,      feedPumpNs);
            updateMax(mStats.perPeerSinkNsMax,   perPeerSinkNs);
            updateMax(mStats.perFeedSourceNsMax, perFeedSrcNs);

            // Bucket width derived from the block period: period/256 (min 4µs) keeps the 256 buckets spanning
            // about one block period at any blocksize; cap stays 255. Two integer divides on an already
            // instrumented stats line, no RT impact. The percentile reconstruction in statsWriterThread uses the
            // same formula (kept in lockstep).
            const uint64_t bucketUs = std::max<uint64_t>(4ULL, blockNs / 1000ULL / 256ULL);
            uint64_t blockUs = blockTimeNs / 1000ULL;
            uint32_t bucket  = (uint32_t)std::min<uint64_t>(blockUs / bucketUs, 255ULL);
            mStats.hist[bucket].fetch_add(1, std::memory_order_relaxed);
        }

        // Periodically log per-peer audio flow stats. Outside any lock.
        auto nowStats = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(nowStats - lastStatsTime).count() >= 2) {
            // Fast lock: mark the reference peer for the recv thread and apply any newly published window
            if (clockLockOn && clockFastOn) {
                for (int i = 0; i < numPeers; ++i) {
                    auto& pr = mCurrentSnap[i];
                    if (!pr) continue;
                    const bool isRef = pr->active && pr->userName == clockRef;
                    if (pr->isClockRef.load(std::memory_order_relaxed) != isRef) pr->isClockRef.store(isRef, std::memory_order_relaxed);
                    if (!isRef) continue;
                    const uint32_t gen = pr->arrGen.load(std::memory_order_acquire);
                    if (gen != clockFastGenSeen){
                        clockFastGenSeen = gen;
                        const double per = pr->arrPeriodTicks.load(std::memory_order_relaxed);
                        if (per > 0.0 && std::fabs(per / blockTicksF0 - 1.0) < 500e-6){   // sane: within ±500 ppm of nominal
                            // Robustness: (1) the first application waits for two consecutive windows within 15 ppm of each
                            // other, to skip the join transient; (2) once locked (>=3 steps) a single window may move the period
                            // by at most ±10 ppm, so one clump-skewed window cannot jump it.
                            const double errPpmFast = (per / blockTicksF - 1.0) * 1e6;   // + = sender's period longer than ours → lengthen ours
                            bool apply = true; double gain = (clockFastApplied >= 5 ? clockFastGainPct / 200.0 : clockFastGainPct / 100.0);   // Half gain once locked, to limit window-to-window wobble
                            if (clockFastApplied == 0){
                                if (clockFastPrevPer > 0.0 && std::fabs(per / clockFastPrevPer - 1.0) < 15e-6) gain = 1.0; else apply = false;
                                clockFastPrevPer = per;
                            }
                            double step = apply ? (per - blockTicksF) * gain : 0.0;
                            if (clockFastApplied >= 3){ const double lim = blockTicksF0 * 10e-6; step = std::max(-lim, std::min(lim, step)); }
                            if (!apply) continue;
                            blockTicksF += step;
                            blockTicks = (uint64_t) llround(blockTicksF);
                            clockAdjPpm = (1.0 - blockTicksF / blockTicksF0) * 1e6;      // legacy accounting: blockTicksF = F0 * (1 - adj*1e-6)
                            clockFastApplied++;
                            rtLogPush(RtLogTag::ClockFast, clockFastApplied, clockRef.c_str(), 0, errPpmFast, (blockTicksF / blockTicksF0 - 1.0) * 1e6, per, 0.0);
                        }
                    }
                }
            }
            // Clock-lock step (see blockTicks above). 10 stats ticks = 20-s baseline.
            if (clockLockOn) {
                uint64_t rx = 0; bool found = false;
                for (int i = 0; i < numPeers; ++i) {
                    auto& pr = mCurrentSnap[i];
                    if (pr && pr->active && pr->userName == clockRef && pr->persistentEndpoint && pr->sink) {
                        int32_t seq = 0; int32_t sz = (int32_t)sizeof(seq);
                        if (pr->sink->get_sourceoption(pr->persistentEndpoint, 1 /* kSbClientOurId */, aoo_opt_newest_seq, &seq, sz) > 0 && seq > 0) {
                            rx = (uint64_t)seq; found = true;
                        }
                        break;
                    }
                }
                if (!found || rx == 0 || rx < clockBaseRx) { clockBaseTicks = 0; }   // absent, or sequence reset (sender restart)
                else if (clockBaseTicks == 0) { clockBaseRx = rx; clockBaseProduced = clockProduced; clockBaseTicks = 1; clockBaseWall = nowStats; }
                else if (++clockBaseTicks >= clockBaseSteps) {
                    const double drx = (double)(rx - clockBaseRx), dpr = (double)(clockProduced - clockBaseProduced);
                    const auto wallNow = nowStats;
                    if (dpr > 1000 && drx > 0.98 * dpr && drx < 1.02 * dpr) {   // sender present, no gap/dup storm
                        const double errPpm = (drx / dpr - 1.0) * 1e6;          // + = sender faster than our clock -> shorten the period
                        clockAdjPpm += errPpm * (clockGainPct / 100.0);
                        clockAdjPpm = std::max(-(double)clockMaxPpm, std::min((double)clockMaxPpm, clockAdjPpm));
                        blockTicksF = blockTicksF0 * (1.0 - clockAdjPpm * 1e-6);   // Continuous, not whole ticks
                        blockTicks = (uint64_t) llround(blockTicksF);
                        const double wallS = std::chrono::duration<double>(nowStats - clockBaseWall).count();
                        rtLogPush(RtLogTag::Clock, 0, clockRef.c_str(), 0, errPpm, clockAdjPpm, drx, dpr * 1e6 + wallS * 1e3);   // f3 = dpr*1e6 + wall_ms
                    }
                    clockBaseRx = rx; clockBaseProduced = clockProduced; clockBaseTicks = 1; clockBaseWall = wallNow;
                }
            }
            for (int i = 0; i < numPeers; ++i) {
                auto& peer = mCurrentSnap[i];
                if (!peer || !peer->active) continue;
                // Publish chirp onsets (absolute mach ms; offline analysis diffs peers' onsets, so only deltas matter).
                // No stdout on the audio thread: a blocking write() on the RT deadline path can stall a block. Push
                // POD records into the lock-free SPSC ring; statsWriterThread drains and printf-formats them with the
                // same line formats. Push cost: bounded name copy + a few stores.
                {
                    uint64_t ct = peer->chirpOnsetTicks.load(std::memory_order_relaxed);
                    if (ct != 0 && ct != peerChirpPrinted[i]) {
                        peerChirpPrinted[i] = ct;
                        double tms = (double)(ct * (uint64_t)tb.numer / (uint64_t)tb.denom) / 1e6;
                        rtLogPush(RtLogTag::Chirp, i, peer->userName.c_str(), ct, tms);
                    }
                }
                {
                    float rp = peer->resamplerPeakMs.exchange(0.0f, std::memory_order_relaxed);
                    if (rp > 0.0f) {
                        rtLogPush(RtLogTag::Resamp, i, peer->userName.c_str(),
                                  blockStartTicks, (double)rp);
                    }
                }
                rtLogPush(RtLogTag::Audio, i, peer->userName.c_str(), blockStartTicks,
                          (double)peerRxBlocks[i], (double)peerRxNonZeroBlocks[i]);
                // Publish the 2-second window to the GUI snapshot atomics.
                // Latency note: one relaxed store per peer per 2 seconds —
                // essentially free, and only fires inside the already-existing
                // logging branch (not the per-block hot path).
                peer->lastBlocks.store(peerRxBlocks[i], std::memory_order_relaxed);
                peer->lastBlocksNonZero.store(peerRxNonZeroBlocks[i], std::memory_order_relaxed);
                peerRxBlocks[i] = 0;
                peerRxNonZeroBlocks[i] = 0;
            }
            lastStatsTime = nowStats;
        }

        // Pace against absolute deadline using kernel primitives.
        hubWaitUntilTicks(nextDeadlineMach);
        deadlineF += blockTicksF;                       // Fractional period
        nextDeadlineMach = (uint64_t) llround(deadlineF);
        ++clockProduced;
        // If we're badly behind (scheduler slipped), don't try to "catch up" by
        // running back-to-back — re-sync to now to avoid burning CPU in a spin.
        uint64_t nowMach = hubNowTicks();
        // Time-based (12 ms) so it holds at every blocksize; see catchupSlipTicks above.
        if (nowMach > nextDeadlineMach + catchupSlipTicks) {
            nextDeadlineMach = nowMach + blockTicks;
            deadlineF = (double) nextDeadlineMach;      // Re-anchor the fractional deadline too
            // The pump's only restart-like event —
            // we abandoned >4 blocks and re-synced the deadline, i.e. a real
            // production gap, not a small hiccup (small hiccups run catch-up
            // blocks back-to-back and the synthetic clock stays exactly
            // right). Re-anchor base/index here so the synthetic timeline
            // doesn't (a) pretend continuity across a genuine gap or
            // (b) drift unboundedly behind wall time across many stalls.
            // aoo_osctime_get() here is off the per-block hot path (this
            // branch fires only after a >4-block scheduler slip).
            feedBaseOscTime = aoo_osctime_get();
            feedBlockIndex = 0;
        }
    }

    std::cout << "[HUB] Audio thread stopped.\n";
}

// Per-peer outbound feed-pump thread. One instance per HubPeer (started in addPeer, joined in
// ~HubPeer). N parallel per-listener threads, which the central audio thread fork-joins on each block,
// keep the N²-feed wall-clock out of the per-block budget.
//
// Per-block protocol:
//   audio thread: mBarrierPending += 1, listenerGoGen++ (under listenerMutex), notify
//   this thread:  wait for new goGen, do work, listenerDoneGen = goGen,
//                 listenerBarrierDone()  (mBarrierPending -= 1; last one notifies)
//   audio thread: pause-spin then cv-wait until mBarrierPending == 0 (barrier)
// Exit protocol: clear listenerAlive under listenerMutex, and if a go was consumed but not yet
// acknowledged, acknowledge it on the way out. The audio thread may only count a listener it saw
// alive under that mutex, so the barrier count always drains.
//
// Reads from server members mCurrent* (set by audio thread before notify; safe because audio thread
// blocks at barrier before next iteration writes). Reads other peers' decodedBlock, also safe because
// the audio thread's Step 1 finished before fan-out, so no concurrent writer.
void SonobusHubServer::listenerProcessThread(std::shared_ptr<HubPeer> selfPeer) {
    if (!selfPeer) return;
    const int32_t peerId = selfPeer->id;
    // The RT audio thread waits on these listener threads every block, so their scheduling is the audio
    // deadline: at default QoS, any preemption of a listener thread stalls the audio thread past its
    // deadline, and the glitched block is an audible pop on all feeds. Mach TIME_CONSTRAINT is the proper
    // primitive, scoped to exactly the threads inside the audio deadline path. A broad QoS boost is not:
    // many threads at USER_INTERACTIVE starve a co-resident client's audio callbacks.

    hub_set_thread_name(("hub-listen-" + std::to_string(peerId)).c_str());

#if defined(__APPLE__)
    // THREAD_TIME_CONSTRAINT like the central audio thread: these are real-time audio-path threads.
    // Period matches one block; constraint stays a full period (hard deadline).
    // Computation claim: max(400µs, blockNs/4), clamped ≤ period. Actual work per listener per block is
    // 10-50µs, but per-wake costs (context switch, cv wake, cache refill) are absolute, so a purely
    // proportional claim shrinks at small blocks and the kernel demotes these threads at load peaks.
    // preemptible=1 because a non-preemptible 50% claim per listener runs into the RT admission ceiling
    // with several peers, while a preemptible claim is reclaimed when unused.
    // (The central audio thread's own policy at the top of audioProcessThread is deliberately different.)
    hub_tb_t tb;
    hub_timebase(&tb);
    const uint64_t blockNs = (uint64_t)(1000.0 * mBlockSize / mSampleRate * 1e6);
    auto nsToMachTicks = [&tb](uint64_t ns) -> uint32_t {
        return (uint32_t)((ns * (uint64_t)tb.denom) / (uint64_t)tb.numer);
    };
    {
        thread_time_constraint_policy_data_t policy;
        policy.period      = nsToMachTicks(blockNs);
        policy.computation = nsToMachTicks(blockNs < 1000000ULL
            ? blockNs * 35ULL / 100ULL                                 // Sub-ms blocks (32) claim 35% (≈233µs @667µs; real work 10-50µs)
            : std::min<uint64_t>(std::max<uint64_t>(400000ULL, blockNs / 4), blockNs));  // Floor 400µs; clamped ≤ period for tiny blocks
        policy.constraint  = nsToMachTicks(blockNs);
        policy.preemptible = 1;                           // Was 0
        thread_policy_set(pthread_mach_thread_np(pthread_self()),
                          THREAD_TIME_CONSTRAINT_POLICY,
                          (thread_policy_t)&policy,
                          THREAD_TIME_CONSTRAINT_POLICY_COUNT);
    }
#elif defined(__linux__)
    hub_set_fifo(3, "listener thread");
#endif

    // selfPeer arrives by value from addPeer, so there is no mPeerMutex self-lookup. The strong ref is
    // dropped when this function returns; see stopAndJoinListenerThread for the ownership-cycle rationale.
    HubPeer* self = selfPeer.get();

    uint64_t lastSeenGoGen = 0;
    bool unackedGo = false;
    for (;;) {
        // Wait for the audio thread to bump goGen for this block.
        {
            std::unique_lock<std::mutex> lk(self->listenerMutex);
            self->listenerCv.wait(lk, [&]() {
                return self->listenerGoGen != lastSeenGoGen
                    || self->listenerThreadShouldStop.load(std::memory_order_acquire);
            });
            if (self->listenerThreadShouldStop.load(std::memory_order_acquire)) {
                // Exit protocol: under the same mutex the audio thread uses to count us, declare ourselves gone and
                // note whether a counted go is still outstanding (at most one: the audio thread never issues a
                // second before the barrier).
                self->listenerAlive.store(false, std::memory_order_relaxed);
                unackedGo = (self->listenerGoGen != lastSeenGoGen);
                lastSeenGoGen = self->listenerGoGen;   // for the diagnostic doneGen store below
                break;
            }
            lastSeenGoGen = self->listenerGoGen;
        }

        // Find self's index in mCurrentSnap to pick our coefficient row +
        // solo flag. Audio thread guarantees mCurrentSnap is stable while
        // we're between go and done.
        int selfIdx = -1;
        for (int i = 0; i < mCurrentNumPeers; ++i) {
            if (mCurrentSnap[i].get() == self) { selfIdx = i; break; }
        }
        if (selfIdx < 0) {
            // We weren't included in this block's snapshot (e.g., just
            // added). Skip — still signal done so audio thread doesn't
            // wait on us indefinitely.
            self->listenerDoneGen.store(lastSeenGoGen, std::memory_order_release);
            listenerBarrierDone();
            continue;
        }

        // The per-listener work lives in listenerPumpFeeds() / listenerCutThroughEmit() so the audio thread
        // can run it inline (--inline-listeners). Read the send mode once per block; hold outFeedsLock across
        // pump -> barrier-done -> (cut-through) emit; feed->lock per feed inside. listenerBarrierDone() sits
        // between pump and emit so the audio thread's barrier wait never includes our sendto.
        //   paced       -> queue only; the pacer drains in this listener's slot
        //   cut-through -> queue, then drain-all + bundle-flush ourselves
        //   inline      -> legacy: one send() per feed right after process()
        const bool cutThrough = mCutThrough.load(std::memory_order_relaxed);
        const bool inlineSend = !cutThrough && !mSendPacing.load(std::memory_order_relaxed);
        std::lock_guard<std::mutex> mlock(self->outFeedsLock);
        const uint64_t feedMax = listenerPumpFeeds(self, selfIdx, inlineSend);

        // Publish per-block max for audio-thread stats aggregation, then signal done. The acq_rel fetch_sub
        // inside listenerBarrierDone() pairs with the audio thread's acquire read of mBarrierPending==0
        // (listenerDoneGen is a release-published diagnostic only).
        self->listenerSourceMaxTicks.store(feedMax, std::memory_order_relaxed);
        self->listenerDoneGen.store(lastSeenGoGen, std::memory_order_release);
        listenerBarrierDone();

        if (cutThrough) listenerCutThroughEmit(self);
    }
    // Exit protocol: drain a go that was counted but never processed.
    if (unackedGo) {
        self->listenerDoneGen.store(lastSeenGoGen, std::memory_order_release);
        listenerBarrierDone();
    }
}

// Per-listener Step-2 work, shared.
// Called either from a listener RT thread (threaded mode) or from the audio thread
// (--inline-listeners). Caller holds self->outFeedsLock. Reads mCurrent* (stable: the audio thread
// does not rewrite them until every listener of this block is done, trivially so when the audio
// thread itself is the caller) and other peers' published decoded slots (Step 1 finished before
// Step 2). Returns the max ticks spent in one feed's process(+send).
uint64_t SonobusHubServer::listenerPumpFeeds(HubPeer* self, int selfIdx, bool inlineSend) {
    const bool listenerHasSolo = (mCurrentListenerSolo[selfIdx] != 0);
    const HubMixOverride* row = &mCurrentCoefs[selfIdx * HUB_MAX_PEERS];
    // Feed sources get the synthetic monotonic block timestamp (base + n*period), not wall clock, so
    // wall-clock hiccups do not jitter the feed DLLs or trigger empty "dropped" blocks. See
    // feedBaseOscTime in audioProcessThread.
    const uint64_t t = mFeedOscTime;
    const int blockSize = mBlockSize;
    uint64_t feedMax = 0;

    for (auto& kv : self->outFeeds) {
        int32_t senderId = kv.first;
        auto& feed = kv.second;
        if (!feed || !feed->source) continue;

        // Find sender's index in the snapshot: flat int array filled at snapshot rebuild, no shared_ptr deref
        // per slot. Empty slots are kNoPeerId, which no real peer id equals.
        int j = -1;
        for (int k = 0; k < mCurrentNumPeers; ++k) {
            if (mCurrentIds[k] == senderId) { j = k; break; }
        }

        if (j < 0 || !mCurrentSnap[j] || !mCurrentSnap[j]->active) {
            // Sender absent — feed silence to keep the stream alive.
            std::lock_guard<std::mutex> flock(feed->lock);
            int fch = feed->channels;
            if ((int)tlsListenerZeroBuf.size() < blockSize * fch) tlsListenerZeroBuf.assign(blockSize * fch, 0.0f);
            if ((int)tlsListenerZeroPtrs.size() < fch) tlsListenerZeroPtrs.resize(fch);
            for (int ch = 0; ch < fch; ++ch) tlsListenerZeroPtrs[ch] = tlsListenerZeroBuf.data() + ch * blockSize;
            uint64_t fpStart = hubNowTicks();
            feed->source->process((const aoo_sample**)tlsListenerZeroPtrs.data(), blockSize, t);
            if (inlineSend)
                feed->source->send();  // legacy inline send: minimize wire-side inter-packet jitter; under --send-pace the pacer drains post-barrier, under --cut-through the caller drains after the barrier
            uint64_t fpDur = hubNowTicks() - fpStart;
            if (fpDur > feedMax) feedMax = fpDur;
            continue;
        }

        const HubMixOverride& ov = row[j];
        const bool soloGated = (listenerHasSolo && !ov.soloed);
        const float coef = ov.gain;

        if (ov.muted || soloGated || coef == 0.0f) {
            // Muted / solo-gated / zero-gain: pump zeros.
            std::lock_guard<std::mutex> flock(feed->lock);
            int fch = feed->channels;
            if ((int)tlsListenerMuteBuf.size() < blockSize * fch) tlsListenerMuteBuf.assign(blockSize * fch, 0.0f);
            if ((int)tlsListenerMutePtrs.size() < fch) tlsListenerMutePtrs.resize(fch);
            for (int ch = 0; ch < fch; ++ch) tlsListenerMutePtrs[ch] = tlsListenerMuteBuf.data() + ch * blockSize;
            uint64_t fpStart = hubNowTicks();
            feed->source->process((const aoo_sample**)tlsListenerMutePtrs.data(), blockSize, t);
            if (inlineSend)
                feed->source->send();
            uint64_t fpDur = hubNowTicks() - fpStart;
            if (fpDur > feedMax) feedMax = fpDur;
            continue;
        }

        std::lock_guard<std::mutex> flock(feed->lock);
        HubPeer* sender = mCurrentSnap[j].get();
        int fch = std::min(feed->channels,
                           std::min(sender->decodedChannels, HUB_MAX_CHANNELS_PER_PEER));
        if (fch <= 0) continue;

        // Read the PUBLISHED slot only.
        uint32_t rseq = sender->decodedSeq.load(std::memory_order_acquire);
        // Dup/skip detection.
        if (feed->lastSeqRead != 0) {
            if (rseq == feed->lastSeqRead)
                sender->feedDupReads.fetch_add(1, std::memory_order_relaxed);
            else if (rseq > feed->lastSeqRead + 1)
                sender->feedSkipReads.fetch_add(1, std::memory_order_relaxed);
        }
        feed->lastSeqRead = rseq;
        int rslot = (int)(rseq & 1);
        auto& rptrs = sender->decodedPtrSlots[rslot];
        if ((int)rptrs.size() < fch) continue;   // not yet initialized

        if (coef == 1.0f) {
            uint64_t fpStart = hubNowTicks();
            feed->source->process((const aoo_sample**)rptrs.data(),
                                  blockSize, t);
            if (inlineSend)
                feed->source->send();
            uint64_t fpDur = hubNowTicks() - fpStart;
            if (fpDur > feedMax) feedMax = fpDur;
        } else {
            if ((int)tlsListenerGainBuf.size() < blockSize * HUB_MAX_CHANNELS_PER_PEER)
                tlsListenerGainBuf.resize(blockSize * HUB_MAX_CHANNELS_PER_PEER);
            if ((int)tlsListenerGainPtrs.size() < HUB_MAX_CHANNELS_PER_PEER)
                tlsListenerGainPtrs.resize(HUB_MAX_CHANNELS_PER_PEER);
            for (int ch = 0; ch < fch; ++ch) {
                tlsListenerGainPtrs[ch] = tlsListenerGainBuf.data() + ch * blockSize;
                const float* src = rptrs[ch];   // Published slot (torn-block fix)
                float* dst = tlsListenerGainPtrs[ch];
                for (int s = 0; s < blockSize; ++s) dst[s] = src[s] * coef;
            }
            uint64_t fpStart = hubNowTicks();
            feed->source->process((const aoo_sample**)tlsListenerGainPtrs.data(), blockSize, t);
            if (inlineSend)
                feed->source->send();
            uint64_t fpDur = hubNowTicks() - fpStart;
            if (fpDur > feedMax) feedMax = fpDur;
        }
    }
    return feedMax;
}

// Emit
// this listener's block NOW on the calling thread instead of waiting for a
// pacer slot. In threaded mode the caller invokes this AFTER
// listenerBarrierDone() so the audio thread's barrier wait does not include
// the sendto; in --inline-listeners mode the caller IS the audio thread and
// the sendto cost is inside its measured per-listener time (governor).
// CALLER HOLDS self->outFeedsLock (every outFeeds insert/erase site takes
// that lock, so feed lifetime is pinned for the drain); feed->lock per feed
// is the same pair the pacer takes. The builder is ours alone: the pacer
// thread does not exist in cut-through mode (start()).
// Cost model (6 peers → 5 feeds, blocksize 32, bundle ON): 5×send() packs
// ~200-byte messages by memcpy (≈1-3 µs each) + ONE sendto for the ~1 KB
// bundle (≈20-30 µs) → ≈35-45 µs added to a 10-50 µs block. Without
// bundling it is 5 (LAN) / 10 (WAN, redundancy 2) sendto ≈ 100-200 µs.
// RT audit: no allocation, no new lock objects, no I/O beyond the sendto
// that pacing would have issued from another thread.
void SonobusHubServer::listenerCutThroughEmit(HubPeer* self) {
    HubBundleBuilder* bb = mBundleEnabled.load(std::memory_order_relaxed) ? &self->outBundle : nullptr;
    tlsPacerBundle = bb;
    for (auto& kv : self->outFeeds) {
        auto& feed = kv.second;
        if (!feed || !feed->source) continue;
        std::lock_guard<std::mutex> fl(feed->lock);
        int drainGuard = 0;
        while (feed->source->send() > 0 && ++drainGuard < 256) {}
    }
    tlsPacerBundle = nullptr;
    if (bb) {
        hub_bundle_flush(bb);   // at most one sendto for what is pending
        mPaceStatDatagrams.fetch_add(bb->statDatagrams, std::memory_order_relaxed);
        mPaceStatMsgs.fetch_add(bb->statMsgs, std::memory_order_relaxed);
        mPaceStatBundles.fetch_add(bb->statBundles, std::memory_order_relaxed);
        mPaceStatMsgsInBundles.fetch_add(bb->statMsgsInBundles, std::memory_order_relaxed);
        bb->statDatagrams = bb->statMsgs = bb->statBundles = bb->statMsgsInBundles = 0;
    }
    // one increment per LISTENER-block (N per audio block) — printed
    // as listener_blocks= in the cut-through [HUB-PACE] line
    mPaceStatBlocks.fetch_add(1, std::memory_order_relaxed);
}

// The inline governor refused the mode. Runs
// on the stats thread (never RT): under EXCLUSIVE mPeerMutex start a
// listener thread for every current peer that has none (same recipe as
// addPeer: stop=false, alive=true, thread holds its own strong ref), THEN
// clear mInlineActive (release). The audio thread's acquire load sees the
// flag flip only after every thread it will count exists; addPeer decides
// under the same exclusive lock, so no peer can be missed either side of
// the flip. Send mode is left as configured (paced / cut-through /
// legacy) — the threaded path supports all three.
void SonobusHubServer::inlineListenersFallback() {
    int started = 0;
    {
        std::unique_lock<std::shared_mutex> lock(mPeerMutex);
        for (auto& p : mPeers) {
            if (!p || p->listenerThread) continue;
            p->listenerThreadShouldStop.store(false, std::memory_order_release);
            p->listenerAlive.store(true, std::memory_order_release);
            std::shared_ptr<HubPeer> sp = p;
            p->listenerThread = std::make_unique<std::thread>(
                [this, sp = std::move(sp)]() mutable { listenerProcessThread(std::move(sp)); });
            ++started;
        }
        mInlineActive.store(false, std::memory_order_release);
    }
    std::cout << "[HUB] --inline-listeners REFUSED: projected " << mInlineMeasPeersMax.load(std::memory_order_relaxed)
              << " listeners x " << (mInlineMeasPerListenerMaxNs.load(std::memory_order_relaxed) / 1000)
              << " us = " << (mInlineMeasProjectedNs.load(std::memory_order_relaxed) / 1000)
              << " us > budget " << (mInlineMeasBudgetNs.load(std::memory_order_relaxed) / 1000)
              << " us (60% of P/2) in 2 consecutive windows -> falling back to per-listener threads ("
              << started << " started; send mode unchanged).\n";
}

// RT log ring consumer.
// Runs on statsWriterThread (~every 200 ms, the tick loop's sleep step). Pops records the audio thread
// pushed and does the printf formatting here, off the RT path. Log analyzers match these tags with
// tolerant [\d.]+ numeric patterns, so keep the line formats stable:
//   [HUB-CHIRP]  t_ms             -> %.2f
//   [HUB-RESAMP] peak_ms          -> %g
//   [HUB-AUDIO]  blocks/non_zero  -> %llu
// printf is immune to std::cout's sticky fixed/precision flags. stdout is line-buffered (HubMain
// setvbuf) and cout is stdio-synced, so ordering vs other log lines is preserved.
void SonobusHubServer::drainRtLog() {
    uint32_t tail = mRtLogTail.load(std::memory_order_relaxed);
    const uint32_t head = mRtLogHead.load(std::memory_order_acquire);
    while (tail != head) {
        const RtLogRec& r = mRtLogRing[tail % kRtLogRingSize];
        switch ((RtLogTag)r.tag) {
        case RtLogTag::Chirp:
            std::printf("[HUB-CHIRP] peer='%s' t_ms=%.2f\n", r.peerName, r.f0);
            break;
        case RtLogTag::Resamp:
            std::printf("[HUB-RESAMP] peer='%s' peak_ms=%g\n", r.peerName, r.f0);
            break;
        case RtLogTag::Clock:   // Hub clock-lock step
            std::printf("[HUB-CLOCK] ref='%s' err_ppm=%.1f adj_ppm=%.1f rx_blocks=%.0f produced=%.0f wall_s=%.3f\n", r.peerName, r.f0, r.f1, r.f2, std::floor(r.f3 / 1e6), std::fmod(r.f3, 1e6) / 1e3);
            break;
        case RtLogTag::ClockFast:   // Fast arrival-regression lock step
            std::printf("[HUB-CLOCK-FAST] ref='%s' step=%d err_ppm=%.1f period_ppm=%.1f measured_ticks=%.3f\n", r.peerName, r.peerIndex, r.f0, r.f1, r.f2);
            break;
        case RtLogTag::Audio:
            std::printf("[HUB-AUDIO] peer='%s' blocks=%llu non_zero=%llu\n",
                        r.peerName,
                        (unsigned long long)r.f0, (unsigned long long)r.f1);
            break;
        default:
            break;   // unknown tag: skip (forward-compat)
        }
        ++tail;
        // Publish per-record so the producer sees freed slots ASAP even if
        // formatting a long batch.
        mRtLogTail.store(tail, std::memory_order_release);
    }
    // Overflow visibility: if the producer dropped records since the last drain, say so.
    uint64_t drops = mRtLogDrops.exchange(0, std::memory_order_relaxed);
    if (drops > 0) {
        std::printf("[HUB-RTLOG] dropped=%llu (ring full)\n",
                    (unsigned long long)drops);
    }
}

// Off-audio-thread stats flusher. Reads the mStats atomics every 5 s, computes percentiles from the
// histogram, snapshots peer/feed counts, appends a CSV row. The CSV can be tailed during live sessions
// to correlate timing drift with audible quality.
void SonobusHubServer::statsWriterThread() {
    std::cout << "[HUB-STATS] writer thread started.\n";

    // Resolve the stats.csv path in the hub data folder.
    std::string outPath;
    {
        const auto dir = sb_hub_gui::hub_settings::dataDir();
        dir.createDirectory();
        outPath = dir.getChildFile("stats.csv").getFullPathName().toStdString();
    }

    // Write header if file is empty/new.
    bool needHeader = true;
    {
        struct stat st;
        if (::stat(outPath.c_str(), &st) == 0 && st.st_size > 0) needHeader = false;
    }
    {
        std::ofstream f(outPath, std::ios::app);
        if (!f.is_open()) {
            std::cerr << "[HUB-STATS] failed to open " << outPath << " — writer exiting.\n";
            return;
        }
        if (needHeader) {
            f << "ts,peers,feeds,blocks,misses,miss_pct,"
                 "p50_us,p95_us,p99_us,max_us,"
                 "snap_max_us,sinkloop_max_us,feedpump_max_us,"
                 "worstpeer_sink_us,worstfeed_src_us\n";
        }
    }
    std::cout << "[HUB-STATS] writing to " << outPath << "\n";

    using clock = std::chrono::steady_clock;
    auto nextTick = clock::now() + std::chrono::seconds(5);
    // [HUB-PACE] 10-s window, printed here off the RT pacer thread. Only meaningful when pacing is on (the
    // pacer never wakes otherwise) or when --cut-through is on (listener-thread emitters feed the same
    // atomics; printed as a mode=cut-through line).
    auto nextPaceLog = clock::now() + std::chrono::seconds(10);
    // Connected-socket EPIPE self-heal
    // scan, once a second (a dead socket = a deaf listener until healed).
    auto nextHealScan = clock::now() + std::chrono::seconds(1);

    while (mRunning.load()) {
        // Sleep in 200ms chunks so shutdown isn't blocked by a full 5s sleep.
        auto now = clock::now();
        while (mRunning.load() && now < nextTick) {
            auto remain = nextTick - now;
            auto step = std::min<std::chrono::nanoseconds>(remain, std::chrono::milliseconds(200));
            std::this_thread::sleep_for(step);
            // Drain the audio thread's log ring on
            // every 200ms step, not just the 5s tick — keeps [HUB-CHIRP]/
            // [HUB-RESAMP]/[HUB-AUDIO] near-real-time for live tailing and
            // keeps the 256-slot ring far from full (worst case ~N peers × 3
            // records per 2s window).
            drainRtLog();
            latencyBroadcastTick();   // 10-s /sb/ping + /sb/hxlat cadence, viewer-independent
            // Free retired peer snapshots
            // (and so removed peers) that every RT reader has moved past —
            // THIS thread runs ~PeerSnapshot / ~HubPeer, never the pacer.
            drainRetiredSnapshots(false);
            // Act on the inline governor's
            // refusal here, off the RT path (thread creation + logging).
            if (mInlineFallbackReq.exchange(false, std::memory_order_acq_rel)
                && mInlineActive.load(std::memory_order_acquire))
                inlineListenersFallback();
            now = clock::now();
            if (mConnectedSockets && now >= nextHealScan) {
                nextHealScan = now + std::chrono::seconds(1);
                healEpipeSockets();
            }
            if (now >= nextPaceLog) {
                nextPaceLog = now + std::chrono::seconds(10);
                // Inline-listener governor report
                // (values published once per kInlineWindowBlocks by the audio
                // thread). Printed whenever the mode was requested, so a
                // fallback shows as active=0 with the last measured window.
                if (mInlineListenersReq.load(std::memory_order_relaxed)) {
                    std::cout << "[HUB-INLINE] active=" << (mInlineActive.load(std::memory_order_relaxed) ? 1 : 0)
                              << " windows=" << mInlineMeasWindows.load(std::memory_order_relaxed)
                              << " peers_max=" << mInlineMeasPeersMax.load(std::memory_order_relaxed)
                              << " per_listener_max_us=" << (mInlineMeasPerListenerMaxNs.load(std::memory_order_relaxed) / 1000)
                              << " projected_us=" << (mInlineMeasProjectedNs.load(std::memory_order_relaxed) / 1000)
                              << " budget_us=" << (mInlineMeasBudgetNs.load(std::memory_order_relaxed) / 1000)
                              << "\n";
                }
                {   // Audio-thread block timing, per 10-s window
                    const uint64_t gapNs  = mAudioGapNsMax.exchange(0, std::memory_order_relaxed);
                    const uint64_t late   = mAudioLateBlocks.exchange(0, std::memory_order_relaxed);
                    const uint64_t workNs = mAudioWorkNsMax.exchange(0, std::memory_order_relaxed);
                    const uint64_t skips  = mAudioSinkSkips.exchange(0, std::memory_order_relaxed);
                    std::cout << "[HUB-TIMING] gap_max_us=" << (gapNs / 1000) << " late_blocks=" << late
                              << " work_max_us=" << (workNs / 1000) << " sink_skips=" << skips << " (period_us=" << (1000000.0 * mBlockSize / std::max(1, mSampleRate)) << ")\n";
                }
                if (mSendPacing.load(std::memory_order_relaxed)) {
                    const uint64_t blocks    = mPaceStatBlocks.exchange(0, std::memory_order_relaxed);
                    const uint64_t overNs    = mPaceStatOvershootNsMax.exchange(0, std::memory_order_relaxed);
                    const uint64_t collapsed = mPaceStatSlotsCollapsed.exchange(0, std::memory_order_relaxed);
                    // Bundle packing window counters
                    const uint64_t dgrams    = mPaceStatDatagrams.exchange(0, std::memory_order_relaxed);
                    const uint64_t msgs      = mPaceStatMsgs.exchange(0, std::memory_order_relaxed);
                    const uint64_t bundles   = mPaceStatBundles.exchange(0, std::memory_order_relaxed);
                    const uint64_t msgsInB   = mPaceStatMsgsInBundles.exchange(0, std::memory_order_relaxed);
                    char avgBuf[32];
                    snprintf(avgBuf, sizeof(avgBuf), "%.2f", bundles > 0 ? (double)msgsInB / (double)bundles : 0.0);
                    std::cout << "[HUB-PACE] blocks=" << blocks
                              << " dither=" << (mPaceDither.load(std::memory_order_relaxed) ? 1 : 0)
                              << " peers=" << mPaceLastPeers.load(std::memory_order_relaxed)
                              << " slot_us=" << (mPaceLastSlotNs.load(std::memory_order_relaxed) / 1000)
                              << " worst_wake_overshoot_us=" << (overNs / 1000)
                              << " slots_collapsed=" << collapsed  // (F2a)
                              << " bundle=" << (mBundleEnabled.load(std::memory_order_relaxed) ? 1 : 0)
                              << " datagrams=" << dgrams
                              << " msgs_packed=" << msgs
                              << " bundles=" << bundles
                              << " avg_msgs_per_bundle=" << avgBuf
                              << " sock=" << (mConnectedSockets ? "connected" : "shared")   
                              << " send_err=" << gHubSendErrors.exchange(0, std::memory_order_relaxed)
                              << "\n";
                } else if (mCutThrough.load(std::memory_order_relaxed)) {
                    // Cut-through window — same atomics, fed
                    // by the listener threads (no slots, so no slot_us /
                    // overshoot / collapsed fields; blocks counts per
                    // listener-block, i.e. peers × audio blocks).
                    const uint64_t lblocks   = mPaceStatBlocks.exchange(0, std::memory_order_relaxed);
                    const uint64_t dgrams    = mPaceStatDatagrams.exchange(0, std::memory_order_relaxed);
                    const uint64_t msgs      = mPaceStatMsgs.exchange(0, std::memory_order_relaxed);
                    const uint64_t bundles   = mPaceStatBundles.exchange(0, std::memory_order_relaxed);
                    const uint64_t msgsInB   = mPaceStatMsgsInBundles.exchange(0, std::memory_order_relaxed);
                    char avgBuf[32];
                    snprintf(avgBuf, sizeof(avgBuf), "%.2f", bundles > 0 ? (double)msgsInB / (double)bundles : 0.0);
                    std::cout << "[HUB-PACE] mode=cut-through"
                              << " listener_blocks=" << lblocks
                              << " bundle=" << (mBundleEnabled.load(std::memory_order_relaxed) ? 1 : 0)
                              << " datagrams=" << dgrams
                              << " msgs_packed=" << msgs
                              << " bundles=" << bundles
                              << " avg_msgs_per_bundle=" << avgBuf
                              << " sock=" << (mConnectedSockets ? "connected" : "shared")   
                              << " send_err=" << gHubSendErrors.exchange(0, std::memory_order_relaxed)
                              << "\n";
                }
                // Per-peer socket error
                // report, rate-limited by construction to ONE line per peer per
                // 10 s window (deltas against the *Printed marks, which only this
                // thread touches). The emitters never log; a vanished peer's
                // ICMP storm therefore shows up here, once, until peer-timeout
                // removes the peer. Brief shared mPeerMutex, same as the
                // [HUB-JIT] pass below.
                if (mConnectedSockets) {
                    std::shared_lock<std::shared_mutex> lock(mPeerMutex);
                    for (auto& p : mPeers) {
                        if (!p || !p->persistentEndpoint) continue;
                        auto* ep = reinterpret_cast<HubEndpoint*>(p->persistentEndpoint);
                        const uint32_t se = ep->sendErrors.load(std::memory_order_relaxed);
                        const uint32_t re = ep->recvErrors.load(std::memory_order_relaxed);
                        if (se == ep->sendErrorsPrinted && re == ep->recvErrorsPrinted) continue;
                        const int err = ep->lastErrno.load(std::memory_order_relaxed);
                        std::cout << "[HUB-SOCK] peer '" << p->userName
                                  << "' fd=" << ep->dest.load(std::memory_order_acquire)->connFd
                                  << " send_err+=" << (se - ep->sendErrorsPrinted)
                                  << " recv_err+=" << (re - ep->recvErrorsPrinted)
                                  << " last_errno=" << err << " (" << strerror(err) << ")\n";
                        ep->sendErrorsPrinted = se;
                        ep->recvErrorsPrinted = re;
                    }
                }
            }
        }
        if (!mRunning.load()) break;
        nextTick += std::chrono::seconds(5);

        // Drain the window: exchange-to-zero on counters + maxes, copy+clear
        // the histogram bucket-by-bucket. Audio thread keeps writing into the
        // *next* window in parallel — that's fine, we just lose strict
        // alignment which doesn't matter at this granularity.
        uint64_t blocks  = mStats.totalBlocks.exchange(0, std::memory_order_relaxed);
        uint64_t misses  = mStats.deadlineMisses.exchange(0, std::memory_order_relaxed);
        uint64_t blkMax  = mStats.blockNsMax.exchange(0, std::memory_order_relaxed);
        uint64_t snpMax  = mStats.snapNsMax.exchange(0, std::memory_order_relaxed);
        uint64_t skMax   = mStats.sinkLoopNsMax.exchange(0, std::memory_order_relaxed);
        uint64_t fpMax   = mStats.feedPumpNsMax.exchange(0, std::memory_order_relaxed);
        uint64_t ppMax   = mStats.perPeerSinkNsMax.exchange(0, std::memory_order_relaxed);
        uint64_t pfMax   = mStats.perFeedSourceNsMax.exchange(0, std::memory_order_relaxed);

        uint32_t hist[256];
        uint64_t histTotal = 0;
        for (int i = 0; i < 256; ++i) {
            hist[i] = mStats.hist[i].exchange(0, std::memory_order_relaxed);
            histTotal += hist[i];
        }

        // Percentiles from the time-bucketed histogram; we report bucket-edge µs. Bucket width is
        // period-derived, same formula as the audio thread's fill site (max(4, blockNs/1000/256)), kept in
        // lockstep so reported percentiles stay in real µs.
        const uint64_t statBlockNs =
            (uint64_t)(1000.0 * mBlockSize / std::max(1, mSampleRate) * 1e6);
        const uint32_t statBucketUs =
            (uint32_t)std::max<uint64_t>(4ULL, statBlockNs / 1000ULL / 256ULL);
        auto pctile = [&](double p) -> uint32_t {
            if (histTotal == 0) return 0;
            uint64_t target = (uint64_t)((double)histTotal * p);
            uint64_t cum = 0;
            for (int i = 0; i < 256; ++i) {
                cum += hist[i];
                if (cum >= target) return (uint32_t)(i * statBucketUs);
            }
            return 255 * statBucketUs;
        };
        uint32_t p50 = pctile(0.50);
        uint32_t p95 = pctile(0.95);
        uint32_t p99 = pctile(0.99);

        // Peer / feed snapshot. Brief lock — contends only with addPeer /
        // removePeer / clientRunThread. Off the audio thread entirely.
        int peerCount = 0;
        int feedCount = 0;
        // Close a finished diagnostic dump (off audio thread).
        if (mDumpFile && mDumpFramesLeft.load(std::memory_order_relaxed) <= 0) {
            fclose(mDumpFile); mDumpFile = nullptr;
            std::cout << "[HUB-DUMP] dump complete.\n";
        }

        // Resize candidates gathered
        // under the shared lock, applied after release.
        std::vector<std::tuple<std::shared_ptr<HubPeer>, double, int>> abufApply;   // Kind 0 commit, 1 grow, 2 mode
        // Per-peer network-jitter log every 2nd tick (10s).
        // This is THE number adaptive sink-buffer sizing will key off:
        // buffer ≈ (p99 − p50) + margin. p50 ≈ packet cadence (block period).
        static int jitTick = 0;
        bool logJitter = ((jitTick++ % 2) == 1);
        {
            std::shared_lock<std::shared_mutex> lock(mPeerMutex);
            for (auto& p : mPeers) {
                if (!p || !p->active) continue;
                ++peerCount;
                {
                    std::lock_guard<std::mutex> fl(p->outFeedsLock);
                    feedCount += (int)p->outFeeds.size();
                }
                if (logJitter) {
                    // Own rate window, so the 1 Hz GUI snapshot cannot clobber the shared pps snapshot this
                    // [HUB-JIT] line reads.
                    auto q = p->jstats.compute(p->jstats.statsWin);
                    if (q.samples >= 32) {
                        uint32_t lost = p->blkLost.exchange(0, std::memory_order_relaxed);
                        uint32_t reord = p->blkReordered.exchange(0, std::memory_order_relaxed);
                        uint32_t resent = p->blkResent.exchange(0, std::memory_order_relaxed);
                        uint32_t bgap = p->blkGap.exchange(0, std::memory_order_relaxed);
                        std::cout << "[HUB-LOSS] peer='" << p->userName
                                  << "' lost=" << lost << " reordered=" << reord
                                  << " resent=" << resent << " gap=" << bgap
                                  << " (per 10s)\n";
                        uint32_t dups = p->feedDupReads.exchange(0, std::memory_order_relaxed);
                        uint32_t skips = p->feedSkipReads.exchange(0, std::memory_order_relaxed);
                        std::cout << "[HUB-CADENCE] sender='" << p->userName
                                  << "' dupReads=" << dups << " skipReads=" << skips
                                  << " (per 10s window)\n";
                        std::cout << "[HUB-JIT] peer='" << p->userName << "'"
                                  << (p->isLan ? " LAN" : " WAN")
                                  << std::fixed << std::setprecision(2)
                                  << " p50=" << q.p50 << "ms p95=" << q.p95
                                  << "ms p99=" << q.p99 << "ms max=" << q.maxMs
                                  << "ms rate=" << std::setprecision(0) << q.pps << "pps"
                                  << std::setprecision(2) << " gap=" << q.gapPct << "%"
                                  << " jit(p99-p50)=" << (q.p99 - q.p50) << "ms"
                                  << " unmatched=" << p->fallbackDispatch.exchange(0, std::memory_order_relaxed)   // Fallback-attributed datagrams per 10 s
                                  << " kbps=" << std::setprecision(0) << ((double)(p->bytesIn.load(std::memory_order_relaxed) - p->jitBytesSnap) * 8.0 / 10.0 / 1000.0)   // Wire rate this 10-s window
                                  << " down_kbps=" << ((double)((uint64_t) std::max<int64_t>(0, p->persistentEndpoint ? reinterpret_cast<HubEndpoint*>(p->persistentEndpoint)->sentBytes : 0) - p->jitBytesOutSnap) * 8.0 / 10.0 / 1000.0)
                                  << "\n";
                        p->jitBytesSnap = p->bytesIn.load(std::memory_order_relaxed);
                        p->jitBytesOutSnap = (uint64_t) std::max<int64_t>(0, p->persistentEndpoint ? reinterpret_cast<HubEndpoint*>(p->persistentEndpoint)->sentBytes : 0);
                        {   // The same window from the kernel's receive timestamps
                            auto k = p->kstats.compute(p->kstats.statsWin);
                            if (k.pps > 0)
                                std::cout << "[HUB-KJIT] peer='" << p->userName << "'" << std::fixed << std::setprecision(2)
                                          << " p50=" << k.p50 << "ms p95=" << k.p95 << "ms p99=" << k.p99 << "ms max=" << k.maxMs
                                          << "ms gap=" << k.gapPct << "% (kernel ts)\n";
                        }
                    }
                }
                // Per-player WAN hold: runs every 10 s (the logJitter tick), WAN players only. Candidates are
                // collected here under the shared lock and APPLIED after release (set_buffersize under the peer's
                // own sinkLock).
                if (logJitter && !p->isLan) {
                    const int64_t nowMs = (int64_t) std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count();
                    const double blockMs = 1000.0 * (double)mBlockSize / (double)std::max(1, mSampleRate);
                    const double marginMs = hub_hold_margin_ms() + blockMs;
                    const double floorMs = hub_hold_floor_ms(kHoldFloorMs);
                    const bool holdBlocks = hub_hold_blocks();
                    auto holdQuant = [&](double ms) { return holdBlocks ? hub_hold_to_blocks(ms, blockMs) * blockMs : ms; };
                    const int mode = mHoldMode.load(std::memory_order_relaxed);   // 0 Jam, 1 Print
                    if (!p->holdGapsCleared && nowMs - p->joinedAtMs >= 5000) {   // Drop the join transient
                        p->jstats.clearGaps(); p->holdGapsCleared = true; p->holdClearedAtMs = nowMs;
                    }
                    auto gw = p->jstats.gapWindow();
                    p->jstats.rotateGapSlice();
                    // Target hold for THIS link from the last 60 s: smallest integer H (ms) such that the
                    // number of gaps longer than H stays within the mode's allowance, scaled to the window.
                    double target = -1.0;
                    double needMs = -1.0;   // gap the target was sized for (log)
                    // The window length is time, not sample count: record skips gaps < kCoalesceMs, so on a cable link
                    // the histogram counts bursts, not packets, and a count-derived window would round the 2-gaps/min
                    // allowance to zero. 6 x 10-s slices, full once the player has been in 60 s.
                    const double windowSec = p->holdGapsCleared ? std::min(60.0, (double)(nowMs - p->holdClearedAtMs) / 1000.0) : 0.0;
                    if (gw.total >= 1000 && windowSec >= 55.0) {
                        const double windowMin = windowSec / 60.0;
                        const uint32_t allowed = (mode == 1) ? 0u : (uint32_t) std::floor(kHoldJamPerMin * windowMin);
                        const uint32_t uncoverable = gw.above[std::min(HubPeer::JitterStats::kGapBuckets - 1, (int) kHoldCeilMs)];   // Stalls > ceiling never set the target
                        int H = 1;
                        for (H = 1; H < HubPeer::JitterStats::kGapBuckets - 1; ++H) if (gw.above[H] - std::min(gw.above[H], uncoverable) <= allowed) break;
                        double Hms = (double) H;
                        if (hub_hold_fine()) {
                            for (int f = 1; f < HubPeer::JitterStats::kFineBuckets - 1; ++f)
                                if (gw.aboveFine[f] - std::min(gw.aboveFine[f], uncoverable) <= allowed) { Hms = std::min(Hms, f * HubPeer::JitterStats::kFineStepMs); break; }
                        }
                        needMs = Hms;
                        // The operator's "Sink buffer WAN (ms)" is the start value, not the floor, so links with a ~1 ms
                        // tail do not pay for it. SB_HUB_HOLD_FLOOR_SETTING=1 makes it the floor of every WAN player's hold.
                        const bool floorIsSetting = hub_hold_floor_setting();
                        target = holdQuant(floorIsSetting
                            ? std::min(kHoldCeilMs, std::max({floorMs, mSinkBufferMs, Hms + marginMs}))
                            : std::min(kHoldCeilMs, std::max(floorMs, Hms + marginMs)));
                    }
                    // Evidence counter for a target under the setting (12 consecutive samples = 2 min)
                    const uint32_t lostNow = p->blkLostTotal.load(std::memory_order_relaxed);
                    static thread_local std::unordered_map<int32_t, uint32_t> lastLost;   // per peer id, stats thread only
                    const uint32_t lost = lostNow - lastLost[p->id]; lastLost[p->id] = lostNow;
                    const double limitPerMin = (mode == 1) ? 0.0 : kHoldJamPerMin;
                    if (!p->abufCommitted) {
                        // QUICK — after ~10 s of clean measurement jump straight to the hold that covers
                        // every gap seen so far (no allowance yet); a clean link stays at the floor, a rough one jumps once.
                        if (!p->holdQuickDone && !hub_hold_floor_setting() && p->holdGapsCleared && windowSec >= 9.0 && gw.total >= 100) {
                            // Gaps no hold can cover (> the ceiling) are stalls, not lateness; one long freeze must not pin a
                            // player at a high hold for the whole session.
                            const uint32_t uncoverableQ = gw.above[std::min(HubPeer::JitterStats::kGapBuckets - 1, (int) kHoldCeilMs)];
                            int Hq = 1;
                            for (Hq = 1; Hq < HubPeer::JitterStats::kGapBuckets - 1; ++Hq) if (gw.above[Hq] - std::min(gw.above[Hq], uncoverableQ) == 0) break;
                            double HqMs = (double) Hq;
                            if (hub_hold_fine()) {
                                for (int f = 1; f < HubPeer::JitterStats::kFineBuckets - 1; ++f)
                                    if (gw.aboveFine[f] - std::min(gw.aboveFine[f], uncoverableQ) == 0) { HqMs = std::min(HqMs, f * HubPeer::JitterStats::kFineStepMs); break; }
                            }
                            const double quick = holdQuant(std::min(kHoldCeilMs, std::max(floorMs, HqMs + marginMs)));
                            if (std::fabs(quick - p->abufCurrentMs) >= 0.5) abufApply.push_back({p, quick, 6});   // QUICK (not a commit)
                            p->holdQuickDone = true;
                        }
                        // Commit at 75 s from a full measured minute, up or down.
                        if (nowMs >= p->holdMeasureUntilMs && target > 0.0)
                            abufApply.push_back({p, target, 0});
                    } else {
                        if (mHoldResizeAll.load(std::memory_order_relaxed) && target > 0.0 && std::fabs(target - p->abufCurrentMs) >= 1.0)
                            abufApply.push_back({p, target, 2});                        // MODE switch: re-size both ways
                        // Grow for cause: LATE blocks (gaps longer than the hold, from the histogram — not lost
                        // packets, which no hold can fix) above the limit for 12 consecutive 10-s windows (2 min)
                        const double latePerMin = (gw.total > 0) ? (double) gw.above[std::min(HubPeer::JitterStats::kGapBuckets - 1, std::max(0, (int) std::floor(p->abufCurrentMs - marginMs)))] / std::max(0.01, (double) gw.total / 1500.0 / 60.0) : 0.0;
                        // Rolling 2-min total (12 windows): bursty links never give 12 bad windows in a row.
                        const double windowMinNow = std::max(0.01, (double) gw.total / 1500.0 / 60.0);
                        // Split late gaps into moderate (hold < gap <= stall threshold) and stalls. A gap is late only when
                        // it is longer than the hold itself; counting gaps the hold absorbed would put commit and grow on the
                        // same edge, and normal variation would grow every link.
                        auto bucketOf = [&](double ms) { return std::min(HubPeer::JitterStats::kGapBuckets - 1, std::max(0, (int) std::floor(ms))); };
                        const double stallMs = std::max(2.0 * p->abufCurrentMs, p->abufCurrentMs + kHoldStallExtraMs);
                        const uint32_t allLate  = (gw.total > 0) ? gw.above[bucketOf(p->abufCurrentMs)] : 0u;
                        const uint32_t uncoverableG = (gw.total > 0) ? gw.above[bucketOf(kHoldCeilMs)] : 0u;   // > ceiling = not growable
                        const uint32_t stallsNowAll = (gw.total > 0) ? gw.above[bucketOf(stallMs)] : 0u;
                        const uint32_t stallsNow = stallsNowAll - std::min(stallsNowAll, uncoverableG);
                        const uint32_t lateNow = (allLate > stallsNow) ? (allLate - stallsNow) : 0u;   // moderate lateness only
                        (void) windowMinNow;
                        p->holdStallRing[p->holdStallIdx] = stallsNow; p->holdStallIdx = (p->holdStallIdx + 1) % 12;
                        uint64_t stallSum = 0; for (int k = 0; k < 12; ++k) stallSum += p->holdStallRing[k];
                        const double stall2minPerMin = (double) stallSum / 6.0 / 2.0;
                        // EMERGENCY grow — new late blocks in THIS 10-s sample (60-s count delta, clamped),
                        // so a stall grows the hold within 30 s instead of being concealed for up to 2 min.
                        const uint32_t newLate = (lateNow > p->holdLatePrev) ? (lateNow - p->holdLatePrev) : 0u;
                        p->holdLatePrev = lateNow;
                        bool decided = false;   // ONE hold decision per peer per sample — emergency > grow > decay
                        if (newLate >= kHoldEmergencyLate && p->abufCurrentMs < kHoldCeilMs - 0.5
                            && nowMs - p->holdLastChangeMs >= kHoldEmergencyGapMs) {
                            abufApply.push_back({p, std::min(kHoldCeilMs, p->abufCurrentMs * 1.33), 4});   // GROW now
                            for (int k = 0; k < 12; ++k) p->holdLateRing[k] = 0;
                            decided = true;
                        }
                        // Grow-stall: a new stall this sample (a burst the hold could not cover, costing late blocks) grows
                        // the hold at once toward the measured gap, capped at kHoldStallStepMs per sample. Recurring stalls
                        // still use the 2-min rule below; decay is untouched.
                        const uint32_t newStalls = (stallsNow > p->holdStallPrev) ? (stallsNow - p->holdStallPrev) : 0u;
                        p->holdStallPrev = stallsNow;
                        if (!decided && newStalls > 0 && p->abufCurrentMs < kHoldCeilMs - 0.5
                            && nowMs - p->holdLastChangeMs >= kHoldStallStepGapMs) {
                            const double measuredGap = std::min(gw.maxMs, kHoldCeilMs);                     // the stall we just saw (60-s window max)
                            const double need = std::min(kHoldCeilMs, measuredGap + marginMs);
                            const double step = std::min(need, p->abufCurrentMs + kHoldStallStepMs);
                            if (step > p->abufCurrentMs + 0.5) {
                                std::cout << "[HUB-HOLD] peer='" << p->userName << "' GROW-STALL gap=" << std::fixed << std::setprecision(1) << measuredGap
                                          << " ms new_stalls=" << newStalls << " need=" << need << " ms\n";
                                abufApply.push_back({p, step, 7});
                                decided = true;
                            }
                        }
                        p->holdLateRing[p->holdLateIdx] = lateNow; p->holdLateIdx = (p->holdLateIdx + 1) % 12;
                        uint64_t lateSum = 0; for (int k = 0; k < 12; ++k) lateSum += p->holdLateRing[k];
                        // gw.above is a 60-s count sampled every 10 s → the 12-entry ring sums ~6 overlapping minutes; divide back to per-minute
                        const double late2minPerMin = (double) lateSum / 6.0 / 2.0;
                        if (!decided && (late2minPerMin > limitPerMin || stall2minPerMin > kHoldStallGrowPerMin) && p->abufCurrentMs < kHoldCeilMs - 0.5
                            && nowMs - p->holdLastChangeMs >= 120000) {
                            abufApply.push_back({p, std::min(kHoldCeilMs, p->abufCurrentMs * 1.33), 1});   // GROW one step
                            for (int k = 0; k < 12; ++k) { p->holdLateRing[k] = 0; p->holdStallRing[k] = 0; }
                            decided = true;
                        }
                        // Follow a rising target. The rules above grow only when gaps are longer than the hold itself; a hold
                        // that sits on its target has no slack for that, so three samples in a row (30 s) with the target a
                        // block or more above the hold raise it to the target. Only while the hold still covers the gaps:
                        // gaps longer than the hold are late blocks or a burst, and the grow rules above own those.
                        if (hub_hold_follow()) {
                            const bool covered = needMs > 0.0 && needMs <= p->abufCurrentMs + 0.01;
                            if (covered && target > p->abufCurrentMs + blockMs - 0.01) p->holdFollowWindows++; else p->holdFollowWindows = 0;
                            if (!decided && p->holdFollowWindows >= 3 && p->abufCurrentMs < kHoldCeilMs - 0.5) {
                                abufApply.push_back({p, target, 8});
                                p->holdFollowWindows = 0;
                                p->holdNoDecayUntilMs = nowMs + 600000;   // 10 min: every change resets this player's queue
                                for (int k = 0; k < 12; ++k) p->holdLateRing[k] = 0;
                                decided = true;
                            }
                        }
                        // Live decay: 5 min with no moderate lateness and a measured target below the hold -> step down one
                        // notch (never below the target), at most every 5 min. Stalls do not block it.
                        // "clean" = no new stall this sample; moderate lateness only moves the target.
                        // With the fine grid a hold steps down only when one more grid step of need would still fit under it;
                        // otherwise a need that sits on a grid edge moves the hold up and down by a block all session.
                        const double decayTarget = (hub_hold_fine() && needMs > 0.0 && target > 0.0)
                            ? std::max(target, holdQuant(std::min(kHoldCeilMs, std::max(floorMs, needMs + HubPeer::JitterStats::kFineStepMs + marginMs)))) : target;
                        if (decayTarget > 0.0 && decayTarget < p->abufCurrentMs - 0.5 && newStalls == 0) p->holdDecayWindows++; else p->holdDecayWindows = 0;
                        // Fast way back after a stall: a hold 2 ms or more above its target for holdFastWindows samples in a row
                        // (30 s) with no new stall goes to the target in ONE step, one queue reset instead of one every two
                        // minutes. A hold that has to grow again within two minutes of that doubles the wait (up to 4 min);
                        // ten quiet minutes reset it.
                        if (hub_hold_fast_decay()) {
                            if (p->holdFastAtMs > 0 && nowMs - p->holdFastAtMs <= 120000 && p->abufCurrentMs > p->holdFastToMs + 0.5) {
                                p->holdFastWindows = std::min(24, p->holdFastWindows * 2); p->holdFastAtMs = 0;
                            } else if (p->holdFastAtMs > 0 && nowMs - p->holdFastAtMs > 600000) {
                                p->holdFastWindows = 3; p->holdFastAtMs = 0;
                            }
                            if (!decided && decayTarget > 0.0 && p->abufCurrentMs - decayTarget >= 2.0 && p->holdDecayWindows >= p->holdFastWindows
                                && nowMs - p->holdLastChangeMs >= 30000) {   // the stay after a follow holds off the slow decay only
                                abufApply.push_back({p, decayTarget, 9});
                                p->holdDecayWindows = 0; p->holdFastAtMs = nowMs; p->holdFastToMs = decayTarget;
                                decided = true;
                            }
                        }
                        if (!decided && p->holdDecayWindows >= kHoldDecayWindows && nowMs - p->holdLastChangeMs >= kHoldDecayGapMs && nowMs >= p->holdNoDecayUntilMs) {
                            std::cout << "[HUB-HOLD] peer='" << p->userName << "' DECAY toward need=" << std::fixed << std::setprecision(1) << decayTarget << " ms\n";
                            double decayTo = std::max(decayTarget, p->abufCurrentMs / 1.33);             // DECAY one step, never below the measured target
                            if (holdBlocks) decayTo = std::max(decayTarget, std::min(holdQuant(decayTo), p->abufCurrentMs - blockMs));   // at least one block down
                            abufApply.push_back({p, decayTo, 5});
                            p->holdDecayWindows = 0;
                        }
                    }
                    p->holdAtCeiling.store(p->abufCommitted && p->abufCurrentMs >= kHoldCeilMs - 0.5, std::memory_order_relaxed);
                    if (target > 0.0 && (mLatBroadcastCount % 3 == 0))
                        std::cout << "[HUB-HOLD] peer='" << p->userName << "' hold=" << std::fixed << std::setprecision(1) << p->abufCurrentMs
                                  << " ms target=" << target << " ms (stalls>ceil/60s=" << gw.above[std::min(HubPeer::JitterStats::kGapBuckets - 1, (int) kHoldCeilMs)] << " samples=" << gw.total << " gaps>target/min=" << (double)gw.above[std::min(HubPeer::JitterStats::kGapBuckets - 1, (int)std::ceil(target - marginMs))] / std::max(0.01, windowSec / 60.0)
                                  << " maxgap=" << gw.maxMs << " ms, lost10s=" << lost << ", need=" << std::setprecision(2) << needMs << std::setprecision(1) << ")\n";
                }
            }
        }

        // Apply adaptive resizes outside mPeerMutex. Each set_buffersize is an AOO queue reset (one brief
        // blip on that peer's inbound feed), so resizes must stay rare.
        for (auto& a : abufApply) {
            auto& pp = std::get<0>(a);
            double ms = std::get<1>(a);
            const int kind = std::get<2>(a);
            if (hub_hold_blocks()) {
                const double blockMs = 1000.0 * (double)mBlockSize / (double)std::max(1, mSampleRate);
                int32_t nb = hub_hold_to_blocks(ms, blockMs);
                ms = nb * blockMs;
                std::lock_guard<std::mutex> sl(pp->sinkLock);
                if (pp->sink) pp->sink->set_option(aoo_opt_iso_blocks, &nb, sizeof(int32_t));
            } else {
                std::lock_guard<std::mutex> sl(pp->sinkLock);
                if (pp->sink) pp->sink->set_buffersize((int32_t) std::lround(ms));
            }
            double prev = pp->abufCurrentMs;
            pp->abufCurrentMs = ms;
            if (kind != 6) pp->abufCommitted = true;   // QUICK keeps measuring until the 75-s COMMIT
            pp->holdLastChangeMs = (int64_t) std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            std::cout << "[HUB-HOLD] peer='" << pp->userName << "' "
                      << (kind == 0 ? "COMMIT" : kind == 1 ? "GROW" : kind == 2 ? "MODE" :  kind == 4 ? "GROW-EMERGENCY" : kind == 5 ? "DECAY" : kind == 7 ? "GROW-STALL" : kind == 8 ? "FOLLOW" : kind == 9 ? "DECAY-FAST" : "QUICK") << " hold "
                      << std::fixed << std::setprecision(1) << prev << " -> " << ms << " ms\n";
        }
        if (!abufApply.empty() || mHoldResizeAll.load(std::memory_order_relaxed)) mHoldResizeAll.store(false, std::memory_order_relaxed);
        abufApply.clear();

        // Timestamp (epoch ms, easy to post-process and correlate).
        int64_t epochMs = (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::system_clock::now().time_since_epoch()).count();

        double missPct = (blocks > 0) ? (100.0 * (double)misses / (double)blocks) : 0.0;

        std::ofstream f(outPath, std::ios::app);
        if (f.is_open()) {
            f << epochMs << ','
              << peerCount << ','
              << feedCount << ','
              << blocks << ','
              << misses << ','
              << std::fixed << std::setprecision(3) << missPct << ','
              << p50 << ','
              << p95 << ','
              << p99 << ','
              << (blkMax / 1000ULL) << ','
              << (snpMax / 1000ULL) << ','
              << (skMax  / 1000ULL) << ','
              << (fpMax  / 1000ULL) << ','
              << (ppMax  / 1000ULL) << ','
              << (pfMax  / 1000ULL) << '\n';
        }
    }

    drainRtLog();   // Final flush at shutdown
    std::cout << "[HUB-STATS] writer thread stopped.\n";
}

// Receives UDP packets and dispatches them to the appropriate sink or client
void SonobusHubServer::networkRecvThread() {
#if defined(__APPLE__)
    // Elevate above default QoS. These threads feed and drain the RT audio path, and default QoS lets
    // background-class work delay them, which shows up as correlated inter-arrival spikes across all
    // peers. USER_INTERACTIVE is the right tier (the audio thread keeps its stronger Mach time-constraint
    // policy).
    bool rxRt = false;
    if (hub_rx_rt()) {
        // Same claim as the listener threads: one block period, preemptible.
        hub_tb_t tb;
        hub_timebase(&tb);
        const uint64_t blockNs = (uint64_t)(1000.0 * mBlockSize / mSampleRate * 1e6);
        auto nsToMachTicks = [&tb](uint64_t ns) -> uint32_t {
            return (uint32_t)((ns * (uint64_t)tb.denom) / (uint64_t)tb.numer);
        };
        thread_time_constraint_policy_data_t policy;
        policy.period      = nsToMachTicks(blockNs);
        policy.computation = nsToMachTicks(blockNs < 1000000ULL
            ? blockNs * 35ULL / 100ULL
            : std::min<uint64_t>(std::max<uint64_t>(400000ULL, blockNs / 4), blockNs));
        policy.constraint  = nsToMachTicks(blockNs);
        policy.preemptible = 1;
        rxRt = thread_policy_set(pthread_mach_thread_np(pthread_self()),
                                 THREAD_TIME_CONSTRAINT_POLICY,
                                 (thread_policy_t)&policy,
                                 THREAD_TIME_CONSTRAINT_POLICY_COUNT) == KERN_SUCCESS;
    }
    if (!rxRt) pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    std::cout << "[HUB] receive thread scheduling: " << (rxRt ? "real-time" : "user-interactive") << "\n";
#elif defined(__linux__)
    hub_set_fifo(10, "recv thread");
#endif
    std::cout << "[HUB] WAN hold switches: floor " << hub_hold_floor_ms(kHoldFloorMs) << " ms, margin " << hub_hold_margin_ms()
              << " ms, whole blocks " << (hub_hold_blocks() ? 1 : 0) << ", fine grid " << (hub_hold_fine() ? 1 : 0) << ", follow " << (hub_hold_follow() ? 1 : 0) << "\n";

    char buf[AOO_MAXPACKETSIZE];
    struct sockaddr_storage srcAddr;
    socklen_t srcAddrLen;
    uint64_t ktsNs = 0;   // Kernel receive timestamp of the current datagram (0 = none)

    std::cout << "[HUB] Network receive thread started.\n";

    // Thread-local cached peer snapshot, refreshed
    // only when mPeerListGen moves (peer add/remove/addr change). One
    // acquire load per loop iteration in steady state. Refreshing on every
    // iteration (not just on packets) bounds how long a removed peer stays
    // pinned by this thread's strong refs to ≤ one iteration (500µs idle
    // sleep or one packet's processing).
    std::shared_ptr<const PeerSnapshot> rsnap;
    uint64_t rsnapGen = ~0ULL;

    // The poll set = the shared socket + every current peer's connected socket. A connected socket
    // receives all of that peer's inbound datagrams (kernel demux; the wildcard socket gets none of them),
    // so a datagram read from pfds[i] with pfdPeer[i] >= 0 is already attributed to rsnap->peers[pfdPeer[i]]
    // with no byAddr4 lookup. Preallocated on the stack (addPeer bounds connected-socket holders to
    // HUB_MAX_PEERS) and rebuilt only when the snapshot gen moves, from the snapshot this thread holds by
    // shared_ptr: every fd in the set belongs to a peer rsnap pins, so it stays open for exactly as long
    // as it can be in the set (see HubEndpoint::connFd). Shape of the loop in this mode: poll(<= 1 ms) ->
    // walk the ready set, draining each ready fd until EAGAIN (or kRecvBurstMax datagrams, for fairness)
    // -> one datagram per outer iteration into the dispatch below -> re-poll when the ready set is
    // exhausted. The 1 ms timeout only bounds how long a snapshot change or shutdown waits when the wire
    // is silent. Shared mode (default) uses recvfrom + a 500 us idle sleep.
    const bool connMode = mConnectedSockets;
    constexpr int kRecvBurstMax = 64;
    struct pollfd pfds[HUB_MAX_PEERS + 1];
    int pfdPeer[HUB_MAX_PEERS + 1];
    int npfds = 0;
    int rdCursor = 0;   // next pfds index to examine in the current ready set
    int rdCount = 0;    // pfds entries covered by the last poll (0 = must poll)
    int rdBurst = 0;    // datagrams taken from pfds[rdCursor] in a row
    if (connMode) std::cout << "[HUB] Network receive thread: polling the shared socket + connected sockets.\n";

    while (mRunning.load()) {
        if (mPeerListGen.load(std::memory_order_acquire) != rsnapGen) {
            {
                std::shared_lock<std::shared_mutex> lock(mPeerMutex);
                rsnapGen = mPeerListGen.load(std::memory_order_relaxed);  // under lock: exact
                rsnap = mPeerSnapshot;
                // Ack AFTER the old rsnap was
                // released above — its drop is never the last (see ownership rule).
                mRecvSnapAck.store(rsnapGen, std::memory_order_release);
            }
            if (connMode) {
                // Rebuild the poll set from the NEW snapshot.
                // No allocation; indices refer to rsnap->peers (this gen).
                npfds = 0;
                pfds[npfds].fd = mSocket; pfds[npfds].events = POLLIN; pfds[npfds].revents = 0;
                pfdPeer[npfds] = -1; ++npfds;
                const auto& ps = rsnap->peers;
                for (int i = 0; i < (int)ps.size() && npfds < HUB_MAX_PEERS + 1; ++i) {
                    auto* ep = reinterpret_cast<HubEndpoint*>(ps[(size_t)i]->persistentEndpoint);
                    // The CURRENT
                    // destination's fd. A rebind is always followed by a gen
                    // bump, so this rebuild is what moves the poll set to the
                    // new connected socket; the old one stays open (retired,
                    // not yet drained) until our ack passes the swap gen.
                    const int cfd = ep ? ep->dest.load(std::memory_order_acquire)->connFd : -1;
                    if (cfd < 0) continue;
                    pfds[npfds].fd = cfd; pfds[npfds].events = POLLIN; pfds[npfds].revents = 0;
                    pfdPeer[npfds] = i; ++npfds;
                }
                rdCursor = rdCount = rdBurst = 0;   // stale revents / indices: force a fresh poll
            }
        }

        ssize_t n = -1;
        HubPeer* preMatched = nullptr;   // Kernel-attributed sender (connected socket)
        if (connMode) {
            bool idle = false;
            for (;;) {
                if (rdCursor >= rdCount) {
                    const int pr = ::poll(pfds, (nfds_t)npfds, 1);   // <= 1 ms; wakes on the first datagram on ANY socket
                    rdCursor = 0; rdBurst = 0;
                    if (pr <= 0) {
                        rdCount = 0;
                        if (pr < 0 && errno != EINTR && errno != EAGAIN) {
                            std::cerr << "[HUB] poll error: " << strerror(errno) << "\n";
                            std::this_thread::sleep_for(std::chrono::milliseconds(10));   // never spin on a hard error
                        }
                        idle = true;
                        break;
                    }
                    rdCount = npfds;
                }
                struct pollfd& pf = pfds[rdCursor];
                if (pf.revents == 0) { ++rdCursor; rdBurst = 0; continue; }
                if (pf.revents & POLLNVAL) {
                    // Cannot happen under the connFd lifetime rule (every fd in
                    // the set is pinned by rsnap); if it ever does, do not spin
                    // at 100% re-polling a dead descriptor — log once per
                    // occurrence, back off, and let the next snapshot rebuild
                    // drop it.
                    std::cerr << "[HUB] poll: POLLNVAL on fd " << pf.fd << " (pfd " << rdCursor << ")\n";
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    ++rdCursor; rdBurst = 0; continue;
                }
                srcAddrLen = sizeof(srcAddr);
                // recvfrom (not recv) on purpose: the control paths below
                // (/sb/* handlers, mClient->handle_message) consume srcAddr.
                n = hubRecvKts(pf.fd, buf, sizeof(buf), (struct sockaddr*)&srcAddr, &srcAddrLen, &ktsNs);   // + kernel ts
                if (n > 0) {
                    if (pfdPeer[rdCursor] >= 0) preMatched = rsnap->peers[(size_t)pfdPeer[rdCursor]].get();
                    if (++rdBurst >= kRecvBurstMax) { ++rdCursor; rdBurst = 0; }   // fairness: hand the walk to the next ready fd
                    break;
                }
                // n <= 0: EAGAIN / EWOULDBLOCK / EINTR (or an empty datagram) =
                // this socket is drained for now. Anything else on a CONNECTED
                // socket is the ICMP echo of a vanished peer (ECONNREFUSED /
                // EHOSTUNREACH): count it — the existing peer-timeout logic
                // removes the peer, and the kernel clears the pending error on
                // this read, so advancing the cursor cannot spin. Logging is
                // the stats thread's job ([HUB-SOCK], once per peer per 10 s).
                if (n == 0 && pfdPeer[rdCursor] >= 0) {
                    // EOF on a connected
                    // UDP socket = SS_CANTRCVMORE — the kernel defunct'ed / shut
                    // it down under us (the recv-side twin of EPIPE on send;
                    // AOO/OSC never emits a 0-byte datagram). poll() keeps it
                    // readable, so this walk re-arms immediately until
                    // healEpipeSockets (<= 1 s) swaps the fd out at its
                    // snapshot rebuild. Count only — same two relaxed atomics
                    // as the error path below, no log, no sleep (the other
                    // fds in the ready set are still served every pass).
                    auto* ep = reinterpret_cast<HubEndpoint*>(rsnap->peers[(size_t)pfdPeer[rdCursor]]->persistentEndpoint);
                    if (ep) {
                        ep->lastErrno.store(EPIPE, std::memory_order_relaxed);
                        ep->recvErrors.fetch_add(1, std::memory_order_relaxed);
                    }
                }
                if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                    if (pfdPeer[rdCursor] >= 0) {
                        auto* ep = reinterpret_cast<HubEndpoint*>(rsnap->peers[(size_t)pfdPeer[rdCursor]]->persistentEndpoint);
                        if (ep) {
                            ep->lastErrno.store(errno, std::memory_order_relaxed);
                            ep->recvErrors.fetch_add(1, std::memory_order_relaxed);
                        }
                    } else {
                        std::cerr << "[HUB] recvfrom error: " << strerror(errno) << "\n";
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));   // as in shared mode: never spin
                    }
                }
                ++rdCursor; rdBurst = 0;
            }
            if (idle) continue;   // poll already waited; re-check mRunning + snapshot gen
        } else {
            srcAddrLen = sizeof(srcAddr);
            n = hubRecvKts(mSocket, buf, sizeof(buf), (struct sockaddr*)&srcAddr, &srcAddrLen, &ktsNs);   // + kernel ts
        }

        if (n > 0) {
            // Cumulative bytes-in counter for the GUI's
            // bandwidth display. Relaxed atomic add — no synchronization
            // attached, no audio-thread effect.
            mBytesInTotal.fetch_add((uint64_t)n, std::memory_order_relaxed);
            int32_t type = 0;
            int32_t id = 0;

            bool isAudio = (aoo_parse_pattern(buf, (int32_t)n, &type, &id) > 0);
            bool isNet = !isAudio && (aoonet_parse_pattern(buf, (int32_t)n, &type) > 0);

            if (kHubDebug) {
                static std::atomic<int> netrecv_n{0};
                static std::atomic<int> audio_recv_n{0};
                int nn = netrecv_n.fetch_add(1);
                bool is_sink_audio = (isAudio && type == AOO_TYPE_SINK);
                bool is_source_msg = (isAudio && type == AOO_TYPE_SOURCE);
                bool log_it = false;
                if (nn < 30) log_it = true;
                if (is_sink_audio) {
                    int an = audio_recv_n.fetch_add(1);
                    if (an < 20 || (an % 500) == 0) log_it = true;
                }
                if (is_source_msg) log_it = true;
                if (log_it) {
                    char srcStr[INET6_ADDRSTRLEN] = {};
                    int srcPortLog = 0;
                    if (((struct sockaddr*)&srcAddr)->sa_family == AF_INET) {
                        auto* sin = (struct sockaddr_in*)&srcAddr;
                        inet_ntop(AF_INET, &sin->sin_addr, srcStr, sizeof(srcStr));
                        srcPortLog = ntohs(sin->sin_port);
                    }
                    std::cerr << "[NETRECV #" << nn << "] from " << srcStr << ":" << srcPortLog
                              << " size=" << n
                              << " isAudio=" << isAudio << " isNet=" << isNet
                              << " type=" << type << " id=" << id << "\n";
                }
            }

            if (!isAudio && !isNet) {
                // Not an AOO/AOONET packet: try SonoBus /sb/... peer control messages (currently /sb/hxmute and
                // /sb/hxgain).
                handleSonobusControlMessage(buf, (int32_t)n, &srcAddr, srcAddrLen);
                continue;
            }

            if (isNet) {
                if (type == AOO_TYPE_CLIENT || type == AOO_TYPE_PEER) {
                    // HUB-UDP-PEER debug log silenced for production.
                    // Was firing per inbound UDP packet from a peer — debug-only.
                    // Re-enable by uncommenting if needed for UDP path debugging.
                    // if (type == AOO_TYPE_PEER) {
                    //     char srcStr[INET6_ADDRSTRLEN] = {};
                    //     int srcPortLog = 0;
                    //     if (((struct sockaddr*)&srcAddr)->sa_family == AF_INET) {
                    //         auto* sin = (struct sockaddr_in*)&srcAddr;
                    //         inet_ntop(AF_INET, &sin->sin_addr, srcStr, sizeof(srcStr));
                    //         srcPortLog = ntohs(sin->sin_port);
                    //     }
                    //     std::cout << "[HUB-UDP-PEER] from " << srcStr << ":" << srcPortLog << " size=" << n << "\n";
                    // }
                    mClient->handle_message(buf, (int32_t)n, &srcAddr);
                }
                continue;
            }

            HubEndpoint epStack;
            epStack.socketFd = mSocket;
            memcpy(&epStack.dest0.addr, &srcAddr, srcAddrLen);   // inline dest0: no allocation per datagram
            epStack.dest0.addrlen = srcAddrLen;

            // Run AOO handle_message outside mPeerMutex against the cached immutable rsnap (refreshed above only
            // on gen change), which keeps each peer alive past removePeer mid-loop. `snap` is a const ref into
            // rsnap, which this iteration holds for its whole duration.
            const std::vector<std::shared_ptr<HubPeer>>& snap = rsnap->peers;

            // Find the peer matching the packet's source address and substitute its persistentEndpoint pointer
            // when handing to AOO: AOO::sink::find_source compares endpoints by pointer identity (sink.cpp:573).
            // A stable pointer for every packet from that peer keeps audio and the get_sourceoption() calls that
            // fill RTT and JitBuf in the GUI consistent.
            void* epForAoo = &epStack;
            // O(1) hash lookup keyed on (sin_addr, sin_port), built at snapshot-rebuild time. Raw pointer is
            // safe: rsnap (held for this whole iteration) owns a strong ref to the peer.
            // A datagram read from a peer's connected socket is attributed by the kernel; it joins the byAddr4-hit
            // path here with the lookup skipped. Shared-socket datagrams (joins, unknown peers, signaling) keep
            // the lookup.
            HubPeer* matchedPeer = preMatched;
            if (!matchedPeer && srcAddr.ss_family == AF_INET) {
                const auto* sa = (const struct sockaddr_in*)&srcAddr;
                auto mit = rsnap->byAddr4.find(addrKey4(sa));
                if (mit != rsnap->byAddr4.end()) {
                    matchedPeer = snap[(size_t)mit->second].get();
                }
            }
            if (matchedPeer && matchedPeer->persistentEndpoint) epForAoo = matchedPeer->persistentEndpoint;
            if (matchedPeer) matchedPeer->bytesIn.fetch_add((uint64_t)n, std::memory_order_relaxed);   // Per-player Up

            // Timestamp inbound
            // sink-bound traffic from the matched peer. Recv-thread only,
            // one ring write under an uncontended per-peer mutex — the audio
            // thread never sees any of this. Data packets dominate (≈375/s)
            // so the occasional ping/format message in the mix is noise.
            if (matchedPeer && type == AOO_TYPE_SINK) {
                matchedPeer->jstats.record(std::chrono::steady_clock::now());
                if (ktsNs) matchedPeer->kstats.record(std::chrono::steady_clock::time_point(std::chrono::nanoseconds(ktsNs)));   
            }

            if (type == AOO_TYPE_SINK) {
                // Address-verified sink isolation: a packet from peer X's address may only enter peer X's sink, or a
                // stale or duplicate stream can land in the wrong peer's sink (one player's audio audible inside
                // another's feed). Fall back to the try-every-sink loop only when the sender address matches no peer
                // (e.g. first packets racing addPeer, or a NAT rebind changing the source port mid-session), logged at
                // low rate so silent misrouting stays visible.
                if (matchedPeer) {
                    if (matchedPeer->active && matchedPeer->sink) {
                        const int32_t handled = matchedPeer->sink->handle_message(buf, (int32_t)n, epForAoo,
                                                                                  (aoo_replyfn)hub_endpoint_send_impl);
                        // Only audio the sink accepted counts. Pings and audio from a stream it does not know must leave the
                        // re-invite armed, or a player whose invite was lost sends into nothing for as long as it runs.
                        const bool isAudio = id == AOO_ID_NONE || std::strstr(buf, "/data") != nullptr;
                        if (isAudio && handled != 0)
                            matchedPeer->lastDataTicks.store(hubNowTicks(), std::memory_order_relaxed);
                        // Fast clock lock: (arrival mach time, newest seq) regression for the reference sender.
                        // Recv thread only (single writer); publishes one measured block period per 20-s window.
                        if (matchedPeer->isClockRef.load(std::memory_order_relaxed) && matchedPeer->persistentEndpoint) {
                            int32_t seq = 0; int32_t sz = (int32_t)sizeof(seq);
                            if (matchedPeer->sink->get_sourceoption(matchedPeer->persistentEndpoint, 1 /* kSbClientOurId */, aoo_opt_newest_seq, &seq, sz) > 0
                                && seq > 0 && seq != matchedPeer->arrLastSeq) {
                                const uint64_t tnow = hubNowTicks();
                                HubPeer& rp = *matchedPeer;
                                static const double tbScale = [](){ hub_tb_t tb; hub_timebase(&tb); return (double) tb.denom / (double) tb.numer; }();
                                const double nomTicks = 1e9 * ((double) mBlockSize / (double) std::max(1, mSampleRate)) * tbScale;   // Was hard-coded 32/48000
                                const double dtPrev = rp.arrLastT ? (double)(tnow - rp.arrLastT) : nomTicks;
                                rp.arrLastT = tnow;
                                if (rp.arrSeq0 < 0 || seq < rp.arrSeq0){ rp.arrSeq0 = seq; rp.arrT0 = (double) tnow; rp.arrSx = rp.arrSy = rp.arrSxx = rp.arrSxy = 0; rp.arrN = 0; rp.arrWinStart = tnow; }
                                // Clump filter: only packets that arrived on cadence (0.2..2 blocks after the previous one) enter
                                // the regression, so a late clump cannot skew a window; the window clock still runs.
                                if (dtPrev >= 0.2 * nomTicks && dtPrev <= 2.0 * nomTicks){
                                    const double x = (double)(seq - rp.arrSeq0), y = (double) tnow - rp.arrT0;
                                    rp.arrSx += x; rp.arrSy += y; rp.arrSxx += x * x; rp.arrSxy += x * y; rp.arrN++;
                                }
                                rp.arrLastSeq = seq;
                                static const double winTicks = [](){ hub_tb_t tb; hub_timebase(&tb); return 20e9 * (double) tb.denom / (double) tb.numer; }();   // 20-s windows
                                if ((double)(tnow - rp.arrWinStart) >= winTicks && rp.arrN >= 5000){
                                    const double den = (double) rp.arrN * rp.arrSxx - rp.arrSx * rp.arrSx;
                                    if (den > 0){
                                        const double slope = ((double) rp.arrN * rp.arrSxy - rp.arrSx * rp.arrSy) / den;   // mach ticks per source block
                                        rp.arrPeriodTicks.store(slope, std::memory_order_relaxed);
                                        rp.arrGen.fetch_add(1, std::memory_order_release);
                                    }
                                    rp.arrSeq0 = -1;   // next sample re-anchors a fresh window
                                }
                            }
                        }
                    }
                } else {
                    static std::atomic<int> unmatched_n{0};
                    int un = unmatched_n.fetch_add(1);
                    if (un < 10 || (un % 1000) == 0) {
                        char srcStr[INET6_ADDRSTRLEN] = {};
                        if (((struct sockaddr*)&srcAddr)->sa_family == AF_INET) {
                            auto* sin = (struct sockaddr_in*)&srcAddr;
                            inet_ntop(AF_INET, &sin->sin_addr, srcStr, sizeof(srcStr));
                        }
                        std::cout << "[HUB-ISOLATE] sink msg #" << un
                                  << " from UNMATCHED addr " << srcStr
                                  << " — legacy dispatch fallback\n";
                        connLog("udp-unknown", srcStr, "", "", "audio/sink datagram from an address that is not a peer", true);   
                    }
                    for (auto& peer : snap) {
                        if (!peer->active || !peer->sink) continue;

                        if (id == AOO_ID_NONE) {
                            if (peer->sink->handle_message(buf, (int32_t)n, epForAoo,
                                                          (aoo_replyfn)hub_endpoint_send_impl)) {
                                // The sink that
                                // ACCEPTED the datagram is the sender — feed its
                                // telemetry and mark it as fallback-attributed,
                                // so [HUB-JIT] can never go silent for a peer
                                // whose audio is demonstrably arriving.
                                peer->jstats.record(std::chrono::steady_clock::now());
                                peer->fallbackDispatch.fetch_add(1, std::memory_order_relaxed);
                                break;
                            }
                            continue;
                        }

                        int32_t sinkId = sinkIdForPeer(peer->id);
                        if (id == AOO_ID_WILDCARD || id == sinkId) {
                            peer->sink->handle_message(buf, (int32_t)n, epForAoo,
                                                       (aoo_replyfn)hub_endpoint_send_impl);
                            if (id != AOO_ID_WILDCARD) {
                                peer->jstats.record(std::chrono::steady_clock::now());   // (hub-jit-lag)
                                peer->fallbackDispatch.fetch_add(1, std::memory_order_relaxed);
                                break;
                            }
                        }
                    }
                }
            } else if (type == AOO_TYPE_SOURCE) {
                // Dispatch AOO_TYPE_SOURCE messages (e.g., client oursink inviting our per-feed source, or sending
                // /ping back to us) into the matching feed source. Iterate all peers' outFeeds and forward by source
                // id. Per-peer outFeedsLock still acquired here (cheap, contention only against allocateOrUpdateFeed).
                for (auto& peer : snap) {
                    if (!peer->active) continue;
                    std::lock_guard<std::mutex> flock(peer->outFeedsLock);
                    for (auto& kv : peer->outFeeds) {
                        auto& feed = kv.second;
                        if (!feed || !feed->source) continue;
                        if (id == AOO_ID_WILDCARD || id == feed->hubSourceId) {
                            feed->source->handle_message(buf, (int32_t)n, epForAoo,
                                                         (aoo_replyfn)hub_endpoint_send_impl);
                            if (id != AOO_ID_WILDCARD) break;
                        }
                    }
                }

            }
        } else if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                std::this_thread::sleep_for(std::chrono::microseconds(500));
            } else {
                std::cerr << "[HUB] recvfrom error: " << strerror(errno) << "\n";
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
    }

    std::cout << "[HUB] Network receive thread stopped.\n";
}

// Runs the AOO client (TCP signaling with the AOO server)
void SonobusHubServer::clientRunThread() {
    std::cout << "[HUB] Client run thread started.\n";

    while (mRunning.load()) {
        if (mClient) {
            // Handle client events (peer join/leave, errors, etc.)
            if (mClient->events_available()) {
                mClient->handle_events(
                    [](void* user, const aoo_event** events, int32_t n) -> int32_t {
                        auto* self = static_cast<SonobusHubServer*>(user);

                        for (int32_t i = 0; i < n; ++i) {
                            switch (events[i]->type) {
                            case AOONET_CLIENT_PEER_JOIN_EVENT: {
                                auto* e = (aoonet_client_peer_event*)events[i];
                                if (e->result > 0) {
                                    std::cout << "[HUB] Peer joined: '" << e->user << "' in group '" << e->group << "'\n";
                                    int32_t newId = self->mNextPeerId++;
                                    self->addPeer(newId, e->user, e->address);
                                }
                                break;
                            }
                            case AOONET_CLIENT_PEER_LEAVE_EVENT: {
                                auto* e = (aoonet_client_peer_event*)events[i];
                                if (e->result > 0) {
                                    std::cout << "[HUB] Peer left: '" << e->user << "'\n";
                                    // One removal path: resolve the id by name under a brief shared lock, then removePeer does erase,
                                    // snapshot rebuild, invite purge, feed unlink (under the exclusive lock), listener join + feed
                                    // teardown (outside it) and the retired-snapshot GC pass.
                                    int32_t leavingId = -1;
                                    {
                                        std::shared_lock<std::shared_mutex> lock(self->mPeerMutex);
                                        for (auto& p : self->mPeers) {
                                            if (p && p->userName == e->user) { leavingId = p->id; break; }
                                        }
                                    }
                                    if (leavingId >= 0) {
                                        self->removePeer(leavingId);
                                    } else {
                                        std::cout << "[HUB] Peer left: '" << e->user
                                                  << "' was not in the peer table (nothing to remove)\n";
                                    }
                                }
                                break;
                            }
                            case AOONET_CLIENT_DISCONNECT_EVENT:
                                std::cout << "[HUB] Disconnected from server\n";
                                self->mConnected = false;
                                break;
                            case AOONET_CLIENT_ERROR_EVENT: {
                                auto* e = (aoonet_client_event*)events[i];
                                std::cerr << "[HUB] Client error: " << (e->errormsg ? e->errormsg : "unknown") << "\n";
                                break;
                            }
                            default:
                                break;
                            }
                        }
                        return 1;
                    }, this);
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    std::cout << "[HUB] Client run thread stopped.\n";
}

// Periodically sends outgoing data (AOO source packets and client messages)
void SonobusHubServer::sendThread() {
    // Default QoS on purpose: a USER_INTERACTIVE boost here is a priority flood (many threads, ~36k
    // sends/s) that starves a co-resident client's audio callbacks (CoreAudio ClientTimeout overloads,
    // audible noise in that client's sent feed). Only networkRecvThread is boosted.

    std::cout << "[HUB] Send thread started.\n";

    while (mRunning.load()) {
        // Send AOO client messages (TCP signaling keepalive, etc.)
        if (mClient) {
            mClient->send();
        }


        // Outbound feed source->send() happens in each per-listener thread, right after that thread's
        // source->process(). Iterating N×(N-1) feeds back-to-back here would make the wire spacing between
        // consecutive packets of any one source vary with N, which looks like clock jitter to receivers'
        // jitter buffers. The hub remains responsible for sink->send() (peer-side ACKs/pings), which is cheap.
        {
            std::shared_lock<std::shared_mutex> lock(mPeerMutex);
            for (auto& peer : mPeers) {
                if (!peer || !peer->active) continue;
                if (peer->sink) peer->sink->send();
            }
        }

        // Periodic sink re-invite. AOO sink's
        // invite is one-shot — `invitation_state.exchange(NONE)` resets on the
        // very next send. If the peer's AOO source wasn't ready when the
        // single invite landed (PEER_JOIN race, fresh-launch ordering, NAT
        // open-up timing), the source never registers our sink → silent
        // upstream forever, no recovery without manual quit+restart.
        //
        // Re-invite every 5 seconds. invite_source is idempotent — if the
        // source already has us as a sink, the duplicate /invite is harmless
        // (AOO source matches by sink-id and ignores). Cost: ~one 40-byte
        // invite packet per peer per 5s = ~8 B/s/peer wire overhead.
        // Audio-thread safe: this happens on sendThread (1ms cadence) under
        // shared_lock(mPeerMutex), same context as peer->sink->send() above.
        {
            using clk = std::chrono::steady_clock;
            static auto lastReinvite = clk::now();
            auto now = clk::now();
            if (std::chrono::duration_cast<std::chrono::seconds>(
                    now - lastReinvite).count() >= 5) {
                lastReinvite = now;
                int reinvited = 0;
                std::shared_lock<std::shared_mutex> lock(mPeerMutex);
                constexpr int32_t kSbClientOurId = 1;
                // Only re-invite a peer whose audio has stopped arriving (no inbound packet for 3 s), the case this
                // loop exists for. If a client's source has no entry for the hub, the invite lands as a brand-new
                // connection: source start, userformat re-send, layout + peer-info re-broadcast.
                static const uint64_t staleTicks = [](){ hub_tb_t tb; hub_timebase(&tb); return (uint64_t)(3e9 * (double) tb.denom / (double) tb.numer); }();
                const uint64_t nowTicks = hubNowTicks();
                for (auto& peer : mPeers) {
                    if (!peer || !peer->active || !peer->sink
                        || !peer->persistentEndpoint) continue;
                    const uint64_t last = peer->lastDataTicks.load(std::memory_order_relaxed);
                    if (last != 0 && nowTicks - last < staleTicks) continue;   // streaming fine — leave it alone
                    // A player with its send muted is not stalled, whether it streamed before or joined muted.
                    // If its invite was lost, it is re-invited as soon as it un-mutes.
                    if (peer->isSendMuted.load(std::memory_order_relaxed) != 0) continue;
                    peer->sink->invite_source(peer->persistentEndpoint,
                                              kSbClientOurId,
                                              (aoo_replyfn)hub_endpoint_send_impl);
                    ++reinvited;
                }
                if (reinvited > 0) std::cerr << "[HUB-REINVITE] fired for " << reinvited << " stalled peer(s)\n";
            }
        }

        // Dispatch per-peer sink events on this same 1 ms cadence. AOO_SOURCE_FORMAT_EVENT detection happens
        // here; any sink rebuild takes peer->sinkLock exclusively, serializing with the audio thread.
        pollPeerSinkEvents();

        // Pump per-feed source events for RTT. Off the audio thread; updates relaxed atomics read by the
        // JUCE message thread.
        pollPeerSourceEvents();

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    std::cout << "[HUB] Send thread stopped.\n";
}

// Paced fan-out emission (--send-pace).
// See header comment on mSendPacing for the why. Protocol per block:
//   audio thread (post-barrier): ++mPacerGen, notify; all feeds have this
//     block's packets queued in their AOO sources (lock-free audioqueues).
//   here: snapshot active peers (fixed mPeers order), drain listener i's
//     feeds at absolute deadline base + i*(0.8*blockNs/N) via mach_wait_until
//     (same primitive as the audio pump; the 20% tail is headroom so the last
//     listener's drain never collides with the next block's hand-off).
// Deadlines already passed are skipped (drain immediately): the pacer can
// lag but never accumulates. AOO source queues absorb, next round drains.
// Locking: takes the same peer->outFeedsLock + feed->lock the listener
// threads take, but only during the drain itself (µs). Slot waits happen
// outside all locks, so worst-case audio-thread contention is one drain.
// [HUB-PACE] logs show wake overshoot.
void SonobusHubServer::pacerThread() {
    hub_set_thread_name("hub-pacer");
    hub_tb_t tb;
    hub_timebase(&tb);
#if defined(__APPLE__)
    // Promote to a time-constraint thread with a small preemptible reservation: this is a deadline thread
    // doing many timed sends per block, and at USER_INITIATED it loses the CPU exactly at load peaks and
    // misses slot deadlines. One thread with a modest claim is not a priority flood.
    {
        const uint64_t blockNsRt = (uint64_t)((double)mBlockSize / (double)mSampleRate * 1.0e9);
        // Blocksize-aware computation budget. The pacer's real per-block work (N timed sends + wakes) does not
        // shrink with the period, and if the declared budget under-represents it the kernel RT-throttles the
        // thread, leaving produced blocks unemitted. Declare max(700µs, 70% of period), clamped to the period
        // so computation never exceeds constraint (thread_policy_set rejects that). preemptible stays 1.
        // For sub-millisecond blocks the 700µs floor would declare 100% of the period, so claim 60% of the
        // period instead (real per-block work at 32 ≈ 6 sendto ≈ 150-250µs).
        const uint64_t compNsRt = blockNsRt < 1000000ULL
            ? blockNsRt * 6ULL / 10ULL
            : std::min<uint64_t>(std::max<uint64_t>(700000ULL, blockNsRt * 7ULL / 10ULL), blockNsRt);
        thread_time_constraint_policy_data_t pol;
        pol.period      = (uint32_t)(blockNsRt * (uint64_t)tb.denom / (uint64_t)tb.numer);
        pol.computation = (uint32_t)(compNsRt * (uint64_t)tb.denom / (uint64_t)tb.numer);  // Was absolute 200000ns
        pol.constraint  = pol.period;
        pol.preemptible = 1;
        thread_policy_set(pthread_mach_thread_np(pthread_self()),
                          THREAD_TIME_CONSTRAINT_POLICY,
                          (thread_policy_t)&pol,
                          THREAD_TIME_CONSTRAINT_POLICY_COUNT);
    }
#elif defined(__linux__)
    hub_set_fifo(2, "pacer thread");
#endif
    auto nsToTicks = [&tb](uint64_t ns) -> uint64_t {
        return ns * (uint64_t)tb.denom / (uint64_t)tb.numer;
    };
    auto ticksToNs = [&tb](uint64_t ticks) -> uint64_t {
        return ticks * (uint64_t)tb.numer / (uint64_t)tb.denom;
    };
    std::cout << "[HUB] Pacer thread started (paced fan-out "
              << (mSendPacing.load(std::memory_order_relaxed) ? "ENABLED" : "disabled")
              << ").\n";

    const uint64_t blockNs =
        (uint64_t)(1000.0 * mBlockSize / std::max(1, mSampleRate) * 1e6);

    uint64_t lastGen = 0;
    // The pacer's peer view is the published immutable PeerSnapshot (ownership rule at PeerSnapshot in the
    // header). Held as a raw pointer across blocks; re-acquired with one atomic load only when
    // mPeerListGen moved. No mutex, no vector build, no shared_ptr traffic on this time-constraint thread,
    // and this thread never owns a reference, so ~HubPeer / ~PeerSnapshot can never run here.
    const PeerSnapshot* snap = nullptr;
    uint64_t snapGen = kSnapAckNone;   // gen of `snap`; kSnapAckNone = holding nothing

    // 10-s stat window: blocks paced, worst slot-wake overshoot, collapsed slots. A slot whose deadline is
    // already past fires immediately; counting it keeps a pacer that has degenerated into bursty emission
    // visible in [HUB-PACE]. Counters are mPaceStat* atomics (printed by statsWriterThread) so this RT
    // thread does no console I/O.

    while (mRunning.load()) {
        // PARK — we touch no snapshot while
        // waiting (and we wait forever when pacing is off), so tell the GC
        // it may free anything. Re-claimed + re-synced below before any use.
        mPacerSnapAck.store(kSnapAckNone, std::memory_order_seq_cst);
        {
            std::unique_lock<std::mutex> lk(mPacerMutex);
            mPacerCv.wait(lk, [&]() {
                return mPacerGen != lastGen || !mRunning.load();
            });
            if (!mRunning.load()) break;
            lastGen = mPacerGen;
        }

        const uint64_t base = hubNowTicks();
        // CLAIM, then check the list gen.
        // Our claim (seq_cst store) precedes our gen load (seq_cst); the
        // publisher's gen bump (seq_cst) precedes the GC's ack load — so
        // either the GC sees us busy with snapGen and keeps that snapshot,
        // or we see the new gen and switch to the new snapshot. Steady
        // state: 2 uncontended atomic stores + 1 load. Nothing here can
        // block, allocate, or run a destructor.
        mPacerSnapAck.store(snapGen, std::memory_order_seq_cst);
        if (mPeerListGen.load(std::memory_order_seq_cst) != snapGen) {
            snap = mPacerSnapPtr.load(std::memory_order_acquire);
            snapGen = snap ? snap->gen : kSnapAckNone;
            mPacerSnapAck.store(snapGen, std::memory_order_release);
        }
        const int n = snap ? (int)snap->pacerPeers.size() : 0;
        if (n == 0) continue;

        // A pure percentage spread narrows the slot with every extra listener until the pacer collapses slots
        // and spins. Floor the slot at mPaceMinSlotUs, but never let the whole fan-out exceed 95% of the block
        // period (then it is simply a burst, which is what plain fan-out does).
        uint64_t slotNs = (blockNs * (uint64_t)mPaceSpreadPct.load(std::memory_order_relaxed) / 100) / (uint64_t)n;
        {
            const uint64_t minSlot = (uint64_t)mPaceMinSlotUs.load(std::memory_order_relaxed) * 1000ULL;
            if (slotNs < minSlot) slotNs = minSlot;
            const uint64_t maxSlot = (blockNs * 95 / 100) / (uint64_t)n;
            if (slotNs > maxSlot) slotNs = maxSlot;
        }

        // --pace-dither, default on: perfectly regular slots sit at a fixed phase vs each client's block
        // clock; slow clock drift walks that phase into a consistently-just-late regime for minutes at a time,
        // and the sustained late-arrival signal makes clients' auto netbuf creep up. Adding uniform random
        // [0, slotNs) to the whole schedule each block decorrelates the phase without bursts (bounded to one
        // slot). Cheap LCG: no libc rand contention.
        uint64_t ditherNs = 0;
        if (mPaceDither.load(std::memory_order_relaxed) && slotNs > 0) {
            mPacerLcg = mPacerLcg * 6364136223846793005ULL + 1442695040888963407ULL;
            ditherNs = (mPacerLcg >> 33) % slotNs;
        }
        // Read the bundle switch once per block.
        const bool bundling = mBundleEnabled.load(std::memory_order_relaxed);

        for (int i = 0; i < n; ++i) {
            if (i > 0 || ditherNs > 0) {
                const uint64_t deadline = base + nsToTicks(slotNs * (uint64_t)i + ditherNs);
                const uint64_t now = hubNowTicks();
                if (now < deadline) {
                    hubWaitUntilTicks(deadline);
                    const uint64_t after = hubNowTicks();
                    if (after > deadline) {
                        const uint64_t over = ticksToNs(after - deadline);
                        // CAS-max into the shared stat (rare path).
                        uint64_t cur = mPaceStatOvershootNsMax.load(std::memory_order_relaxed);
                        while (over > cur && !mPaceStatOvershootNsMax.compare_exchange_weak(
                                   cur, over, std::memory_order_relaxed)) {}
                    }
                } else {
                    // Deadline already passed — the
                    // slot collapses into the previous one (immediate fire,
                    // no stagger). Record it so [HUB-PACE] shows when paced
                    // emission has degenerated into a burst.
                    mPaceStatSlotsCollapsed.fetch_add(1, std::memory_order_relaxed);
                }
            }
            // Raw pointer, pinned by the
            // snapshot claimed above (see PeerSnapshot ownership rule).
            HubPeer* const peer = snap->pacerPeers[i];
            std::lock_guard<std::mutex> ml(peer->outFeedsLock);
            // Arm this listener's bundle
            // builder for the duration of the slot. Every feed send below
            // that targets this listener is packed by hub_endpoint_send_impl;
            // flush once at slot end. Pointer set/clear only — no lock, no
            // allocation, no timing change to the slot schedule above.
            HubBundleBuilder* bb = bundling ? &peer->outBundle : nullptr;
            tlsPacerBundle = bb;
            for (auto& kv : peer->outFeeds) {
                auto& feed = kv.second;
                if (!feed || !feed->source) continue;
                std::lock_guard<std::mutex> fl(feed->lock);
                // Drain-all safety net. A single send() emits at most one audio block (source::send_data), so a missed
                // pacer pass would lose that slot's emission budget for good and clients would starve. send() returns
                // 1 while it did any work (format/data/resend/ping, see deps/aoo/lib/src/source.cpp source::send) and
                // 0 when idle, so looping while it reports work drains the whole audioqueue backlog in one pass: a
                // rate deficit becomes bounded burstiness. Bounds: the audioqueue holds ~10 ms
                // (HUB_SOURCE_BUFFERTIME_MS), dropped_ is capped to that too, and ping/format fire at most once per
                // interval. The guard is a pure safety valve against a send() that misreports work done.
                int drainGuard = 0;
                while (feed->source->send() > 0 && ++drainGuard < 256) {}
            }
            tlsPacerBundle = nullptr;
            if (bb) {
                hub_bundle_flush(bb);   // end of slot: at most one sendto for what is pending
                // fold the slot's counters into the 10s window (4 relaxed adds/slot)
                mPaceStatDatagrams.fetch_add(bb->statDatagrams, std::memory_order_relaxed);
                mPaceStatMsgs.fetch_add(bb->statMsgs, std::memory_order_relaxed);
                mPaceStatBundles.fetch_add(bb->statBundles, std::memory_order_relaxed);
                mPaceStatMsgsInBundles.fetch_add(bb->statMsgsInBundles, std::memory_order_relaxed);
                bb->statDatagrams = bb->statMsgs = bb->statBundles = bb->statMsgsInBundles = 0;
            }
        }

        // The 10-s [HUB-PACE] line is printed by statsWriterThread from these relaxed atomics, so the pacer
        // does no I/O on this time-constraint thread.
        mPaceStatBlocks.fetch_add(1, std::memory_order_relaxed);
        mPaceLastPeers.store(n, std::memory_order_relaxed);
        mPaceLastSlotNs.store(slotNs, std::memory_order_relaxed);
    }

    std::cout << "[HUB] Pacer thread stopped.\n";
}

// ─── Stage limits, kick and the listing status (control path only) ───────────────────────
void SonobusHubServer::applyStageSettings(const StageSettings& st) {
    {
        std::lock_guard<std::mutex> l(mStageMutex);
        const bool wasListed = mStage.list;
        mStage = st;
        if (mStage.list && !wasListed) { mStageListingState = 4; mStageListingText = "Checking with the stage directory..."; }
    }
    mStageMaxChannels.store(st.maxChannels, std::memory_order_relaxed);
    aoo_sb_set_stage_max_players(st.maxPlayers);
    mStageGen.fetch_add(1, std::memory_order_relaxed);
}

SonobusHubServer::StageSettings SonobusHubServer::stageSettings() const {
    std::lock_guard<std::mutex> l(mStageMutex);
    return mStage;
}

void SonobusHubServer::setStageListingStatus(int state, const std::string& text) {
    std::lock_guard<std::mutex> l(mStageMutex);
    mStageListingState = state;
    mStageListingText = text;
}

bool SonobusHubServer::kickPlayer(const std::string& userName, int blockMinutes) {
    if (userName.empty()) return false;
    std::string ip;
    bool found = false;
    {
        std::shared_lock<std::shared_mutex> lk(mPeerMutex);
        for (auto& p : mPeers) {
            if (!p || p->userName != userName) continue;
            found = true;
            char ipBuf[INET6_ADDRSTRLEN] = {0};
            if (p->addr.ss_family == AF_INET) inet_ntop(AF_INET, &((const struct sockaddr_in*)&p->addr)->sin_addr, ipBuf, sizeof(ipBuf));
            else if (p->addr.ss_family == AF_INET6) inet_ntop(AF_INET6, &((const struct sockaddr_in6*)&p->addr)->sin6_addr, ipBuf, sizeof(ipBuf));
            ip = hub_plain_ip(ipBuf);
            break;
        }
    }
    const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    {
        std::lock_guard<std::mutex> l(mStageMutex);
        mStageBlocks.erase(std::remove_if(mStageBlocks.begin(), mStageBlocks.end(), [now](const StageBlock& b) { return b.untilMs <= now; }), mStageBlocks.end());
        if (blockMinutes > 0) mStageBlocks.push_back({ ip, userName, now + (int64_t) blockMinutes * 60000 });
    }
    connLog("kick", ip, userName, mGroupName, "removed by the host, kept out " + std::to_string(blockMinutes) + " min");
    aoo_sb_kick_user(userName.c_str());
    return found;
}

std::string SonobusHubServer::stageJoinRefusal(const std::string& ipRaw, const std::string& user, int othersInStage) {
    const std::string ip = hub_plain_ip(ipRaw);
    const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    std::lock_guard<std::mutex> l(mStageMutex);
    mStageBlocks.erase(std::remove_if(mStageBlocks.begin(), mStageBlocks.end(), [now](const StageBlock& b) { return b.untilMs <= now; }), mStageBlocks.end());
    for (const auto& b : mStageBlocks) {
        if (b.user == user || (!b.ip.empty() && b.ip == ip)) {
            const int64_t mins = std::max<int64_t>(1, (b.untilMs - now + 59999) / 60000);
            return "The host removed you from this stage. You can rejoin in " + std::to_string(mins) + (mins == 1 ? " minute." : " minutes.");
        }
    }
    if (mStage.maxPlayers > 0 && othersInStage >= mStage.maxPlayers)
        return "This stage is full (" + std::to_string(othersInStage) + " of " + std::to_string(mStage.maxPlayers) + " players).";
    return {};
}

