// SPDX-License-Identifier: GPLv3-or-later WITH Appstore-exception
// Copyright (C) 2026 Jason Buecker (Random Planet)
// SonoBus RP — Hub Server (Jamulus-style centralized audio relay)

#pragma once

#include "aoo/aoo.hpp"
#include "aoo/aoo_net.hpp"
#include "aoo/aoo_pcm.h"
#include "aoo/aoo_opus.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>     // Std::ceil
#include <condition_variable>
#include <cstdint>
#include <deque>   // Recent-sessions ring
#include <utility>   // Std::pair (session settings snapshot)
#include <cstring>
#include <iostream>
#include <exception>
#include <memory>
#include <mutex>
#include <set>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <opus/opus.h>

#define HUB_MAX_PEERS 32
#define HUB_DEFAULT_CHANNELS 2
// Upper bound on per-peer channel count negotiated via AOO_SOURCE_FORMAT_EVENT.
// Matches stock SonoBus's MAX_PANNERS = 64 — supports a fully multi-mic'd drum
// kit (8-16 ch), a multi-channel keyboard, etc. Used to pre-size per-peer audio
// buffers so dynamic channel changes don't allocate on the audio thread.
#define HUB_MAX_CHANNELS_PER_PEER 64
#define HUB_DEFAULT_SAMPLERATE 48000
#define HUB_DEFAULT_BLOCKSIZE 128
#define HUB_SINK_BUFFERTIME_MS 30.0
#define HUB_SOURCE_BUFFERTIME_MS 10.0

struct HubEndpointHandle;

// AOO identifies sinks/sources by an opaque endpoint*. We use one heap-allocated
// HubEndpoint per peer (owned by HubPeer::persistentEndpoint) so the pointer is
// stable across the peer's lifetime. Lives in the header so ~HubPeer can delete it.
// ONE peer destination = its
// sockaddr + (--connected-sockets) the UDP socket connect()ed to exactly
// that sockaddr. Immutable once published through HubEndpoint::dest; an
// address change publishes a NEW HubDest and retires the old one (see
// SonobusHubServer::rebindPeerEndpointLocked / mRetiredDests). Kept as one
// object so the fd and the address can never disagree under a reader.
struct HubDest {
    struct sockaddr_storage addr;
    socklen_t addrlen = 0;
    // This destination's own connect()ed UDP socket, or -1 = send with sendto() on the shared hub socket.
    // send() on a connected UDP socket costs about a third of an unconnected sendto(), the hub's largest on-CPU cost.
    int connFd = -1;
};

struct HubEndpoint {
    int socketFd = -1;
    // The destination lives behind
    // ONE pointer. `dest0` is the initial destination, inline, so the stack
    // endpoints built per datagram in the recv loop and addPeer allocate
    // nothing extra; `dest` is what every sender reads.
    // Lifetime rule:
    //   * dest0 is filled in addPeer, under exclusive mPeerMutex and BEFORE
    //     the owning HubPeer is published into mPeers / any PeerSnapshot.
    //   * `dest` is re-pointed ONLY by rebindPeerEndpointLocked (control
    //     path: exclusive mPeerMutex, followed by a snapshot rebuild), at a
    //     heap HubDest built and connect()ed BEFORE the release store. The
    //     previous HubDest is NOT freed there: it goes to mRetiredDests
    //     tagged with the snapshot gen current at the swap, and is closed +
    //     freed by drainRetiredSnapshots() only after EVERY snapshot reader
    //     (pacer / audio / recv) has acked past that gen AND the owning
    //     peer's outFeedsLock has been taken once (cut-through listener
    //     threads emit under that lock without acking any gen). See the
    //     PeerSnapshot ownership rule for why that makes an in-flight send
    //     on the old fd impossible.
    //   * The current HubDest dies in ~HubEndpoint, which runs from
    //     ~HubPeer, which runs only after every snapshot reader has acked
    //     past every snapshot listing the peer. Every send reaches this
    //     endpoint through a pinned HubPeer, so the fd is open for the whole
    //     send.
    //   * Readers do ONE acquire load of `dest` per send (one ldar on arm64;
    //     the acquire pairs with the release store in the swap so the
    //     pointee is complete).
    HubDest dest0;
    std::atomic<HubDest*> dest{&dest0};
    int64_t sentBytes = 0;
    // Send / recv failures on this endpoint (ECONNREFUSED / EHOSTUNREACH is
    // the ICMP echo of a vanished peer on a connected socket; EAGAIN = full
    // sndbuf; EPIPE = the SOCKET itself is dead — see healEpipeSockets).
    // Emitter threads only count (one relaxed RMW on the ERROR
    // path, never a log); statsWriterThread prints at most one [HUB-SOCK]
    // line per peer per 10 s window from the *Printed deltas.
    std::atomic<uint32_t> sendErrors{0};
    std::atomic<uint32_t> recvErrors{0};
    std::atomic<int>      lastErrno{0};
    uint32_t sendErrorsPrinted = 0;   // statsWriterThread only
    uint32_t recvErrorsPrinted = 0;   // statsWriterThread only
    // Connected-socket self-heal state,
    // statsWriterThread only (SonobusHubServer::healEpipeSockets).
    uint32_t connReopens = 0;         // re-open ATTEMPTS after EPIPE, this peer (cap kConnReopenMax)
    bool     connHealSettle = false;  // one scan after a heal: clear a stale lastErrno instead of acting on it
    bool     reopenPending = false;   // on the shared socket after EPIPE; a fresh connect() is owed
    int64_t  reopenNotBeforeMs = 0;   // steady_clock ms: earliest re-open attempt (>= kConnReopenDelayMs after the last step)
    // Connected fds of this endpoint that are retired but NOT yet closed by
    // drainRetiredSnapshots (+1 at retire under exclusive mPeerMutex, -1 by
    // the drain right after ::close). A re-open must wait for 0: the fresh
    // socket connect()s the identical 4-tuple, which XNU rejects with
    // EADDRINUSE while the dead pcb is still hashed.
    std::atomic<uint32_t> connFdsPendingClose{0};

    HubEndpoint() = default;
    HubEndpoint(const HubEndpoint&) = delete;             // owns a socket + self-pointer; never copied (only ever used by pointer)
    HubEndpoint& operator=(const HubEndpoint&) = delete;
    ~HubEndpoint() {
        HubDest* d = dest.load(std::memory_order_relaxed);
        if (d->connFd >= 0) ::close(d->connFd);
        if (d != &dest0) delete d;   // a rebound destination; retired ones are freed by the drain
    }
};

// Per-listener OSC-bundle packer. Sends are the pacer's largest cost, so under --send-pace, where every listener's
// feeds are drained back-to-back inside that listener's slot, the datagrams of one slot ride in ONE UDP datagram
// as a standard OSC bundle ("#bundle\0" + 8-byte timetag + repeated [int32 BE size][message]). Added latency is
// zero: the wait is the microseconds between two consecutive feed sends in the same slot.
//   - one builder per listener, allocated ONCE inside HubPeer (addPeer);
//     nothing here allocates or locks. The pacer thread is the only writer
//     (via the thread-local pointer in SonobusHubServer.cpp).
//   - cap = UDP payload cap (default HUB_BUNDLE_MTU_DEFAULT = 1400 → 1428
//     bytes on the wire, under a 1500 MTU). A message that does not fit
//     flushes what is pending first; a message is NEVER split.
//   - a flush holding exactly ONE message sends it bare, without the bundle wrapper.
//   - COMPAT: a client must parse "#bundle" datagrams from a bundling hub (aoo_parse_pattern
//     otherwise rejects them as "not a valid AOO message", silence). --no-bundle sends every message bare.
#define HUB_BUNDLE_MTU_DEFAULT 1400
struct HubBundleBuilder {
    static constexpr int kBufMax = 4096;   // hard buffer (== AOO_MAXPACKETSIZE); runtime cap ≤ this
    static constexpr int kHeader = 16;     // "#bundle\0" + timetag(1 = immediate)
    static constexpr int kMinCap = kHeader + 4 + 64;
    HubEndpoint* ep = nullptr;             // destination identity: the listener's persistentEndpoint
    int cap   = HUB_BUNDLE_MTU_DEFAULT;    // max datagram payload (bytes), set at addPeer
    int used  = kHeader;                   // bytes staged (header always reserved)
    int count = 0;                         // messages staged
    // emitter-thread-only window counters (pacer slot, or the owning listener
    // thread under --cut-through); folded into the hub's [HUB-PACE] atomics
    // once per slot/block (one relaxed fetch_add each — no per-message atomics)
    uint64_t statDatagrams = 0;            // sendto() calls issued through the builder (bare + bundle)
    uint64_t statMsgs      = 0;            // messages that entered the builder
    uint64_t statBundles   = 0;            // datagrams that were real bundles (≥2 messages)
    uint64_t statMsgsInBundles = 0;        // messages carried inside those bundles
    char buf[kBufMax];

    void init(HubEndpoint* e, int mtuCap) {
        ep = e;
        cap = mtuCap < kMinCap ? kMinCap : (mtuCap > kBufMax ? kBufMax : mtuCap);
        memcpy(buf, "#bundle\0", 8);
        // OSC timetag "immediately" = 0x0000000000000001 (big-endian)
        memset(buf + 8, 0, 8);
        buf[15] = 1;
        used = kHeader;
        count = 0;
    }
};

// Per-listener override for one sender's contribution to the hub mix. Defaults (muted=false, gain=1.0) leave
// the sender unchanged; clients send /sb/hxmute and /sb/hxgain (see SonobusHubMixControl) to override.
struct HubMixOverride {
    bool muted = false;
    float gain = 1.0f;
    bool soloed = false;  // Phase 5: per-listener solo via /sb/hxsolo
};

// One outbound feed relays ONE sender's audio to ONE listener. The hub allocates an `aoo::isource` per (listener,
// sender) pair on receipt of /sb/hxfeedreq; the source carries sender->recvChannels of audio and is wired to push
// to the listener's oursink at the ourId the listener specified. The hub pumps these every audio block.
struct HubOutFeed {
    aoo::isource::pointer source;
    int32_t hubSourceId = -1;   // unique within the hub's source-id space
    int32_t clientOurId = -1;   // listener's local oursink id for this sender
    int32_t channels    = 0;    // sender's recvChannels at the time of allocation
    uint32_t lastSeqRead = 0;   // Dup/skip cadence detector
    // Serializes audio-thread feed.source->process against any reconfiguration
    // (channel-count change driven by sender's AOO_SOURCE_FORMAT_EVENT).
    std::mutex lock;
};

struct HubPeer {
    int32_t id = -1;
    std::string userName;

    // Inbound: receives audio FROM this peer. One sink per peer.
    aoo::isink::pointer sink;
    // Outbound: one feed per other peer in the group whose audio this listener has requested via /sb/hxfeedreq.
    // Keyed by sender's peer id, lazy-allocated. Each feed carries one sender's audio verbatim; the listener-side client mixes locally.
    std::unordered_map<int32_t, std::unique_ptr<HubOutFeed>> outFeeds;
    // Serializes outFeeds map mutation (insert / erase). Per-feed audio access
    // uses the feed's own lock (HubOutFeed::lock).
    mutable std::mutex outFeedsLock;

    struct sockaddr_storage addr;
    socklen_t addrlen = 0;

    // Set once in addPeer by matching addr against
    // the --lan-subnet CIDR. LAN peers may get a tighter sink jitter buffer
    // (--sink-buffer-lan); WAN peers always keep the base (safe) value.
    bool isLan = false;

    // Per-peer jitter telemetry, the input to adaptive sink buffers. Written ONLY by networkRecvThread (which already
    // matches sender addr → peer); read at low rate by GUI and stats under the internal lock. The audio thread NEVER touches this.
    struct JitterStats {
        // 16384 deltas = ~11 s at 1500 pps, the whole 10-s [HUB-JIT] window; a shorter ring would sample only part of
        // each window and miss one-second sender stalls in the max/p99.
        static constexpr int kRing = 16384;
        // Clients send 32-sample blocks with
        // redundancy=2 → every block arrives as a back-to-back twin (~3000pps,
        // half the deltas ≈0.02ms). Coalesce sub-0.15ms arrivals into one
        // event so p50 reflects true block cadence and gap% means real gaps —
        // otherwise the auto-sizer reads ~48% "gaps" and refuses to shrink.
        static constexpr float kCoalesceMs = 0.15f;
        std::array<float, kRing> deltasMs {};   // inter-arrival deltas (ms)
        int ringPos = 0;
        int ringCount = 0;
        std::chrono::steady_clock::time_point lastArrival {};
        std::chrono::steady_clock::time_point windowStart {};
        uint64_t packets = 0;
        std::atomic<uint64_t> packetsAtomic{0};   // Lock-free mirror of `packets` for the RT clock-lock
        // Rolling-rate snapshot state. Rolling because a lifetime average masks a live double stream once a peer record
        // survives a rejoin (dedupe path). compute() has several callers at different periods (snapshotForUI, statsWriter
        // [HUB-JIT] 10 s, statsWriter adaptive-buffer 5 s), so the window is caller-OWNED: each caller diffs against its own
        // previous call and none can clobber another's. Quantiles/gap%/max come from the shared ring.
        struct RateWindow {
            uint64_t snapPackets = 0;
            std::chrono::steady_clock::time_point snapTime {};
        };
        // Per-caller windows (all guarded by `lock` inside compute()):
        RateWindow uiWin;     // snapshotForUI (1Hz GUI)
        RateWindow statsWin;  // statsWriterThread [HUB-JIT] (10s)
        RateWindow abufWin;   // statsWriterThread adaptive sink buffer (5s)
        mutable std::mutex lock;

        // 60-s arrival-gap histogram,
        // 6 slices of 10 s, 1-ms buckets 0..64 + overflow (65). Recorded here (recv thread, under
        // the same lock), read + rotated by the stats thread every 10 s. Never the audio thread.
        static constexpr int kGapSlices = 6, kGapBuckets = 66;
        uint32_t gapHist[kGapSlices][kGapBuckets] = {};
        // The same 60 s in 0.25-ms steps for gaps up to 8 ms (last index = longer). The hold target reads it when
        // SB_HUB_HOLD_FINE=1, so a link whose gaps sit near a whole millisecond is not rounded up by a full one.
        static constexpr int kFineBuckets = 34;
        static constexpr double kFineStepMs = 0.25;
        uint32_t gapFine[kGapSlices][kFineBuckets] = {};
        int gapSlice = 0;
        void clearGaps() {                 // Drop the join transient before the quick check
            std::lock_guard<std::mutex> g(lock);
            for (int sl = 0; sl < kGapSlices; ++sl) for (int i = 0; i < kGapBuckets; ++i) gapHist[sl][i] = 0;
            for (int sl = 0; sl < kGapSlices; ++sl) for (int i = 0; i < kFineBuckets; ++i) gapFine[sl][i] = 0;
        }
        void rotateGapSlice() {            // stats thread, every 10 s: advance + clear the new slice
            std::lock_guard<std::mutex> g(lock);
            gapSlice = (gapSlice + 1) % kGapSlices;
            for (int i = 0; i < kGapBuckets; ++i) gapHist[gapSlice][i] = 0;
            for (int i = 0; i < kFineBuckets; ++i) gapFine[gapSlice][i] = 0;
        }
        struct GapWindow { uint32_t total = 0; uint32_t above[kGapBuckets] = {}; uint32_t aboveFine[kFineBuckets] = {}; double maxMs = 0; };
        GapWindow gapWindow() const {      // counts of gaps > H ms for every integer H, over the whole 60 s
            GapWindow w;
            std::lock_guard<std::mutex> g(lock);
            uint32_t cum = 0;
            for (int b = kGapBuckets - 1; b >= 0; --b) {
                uint32_t c = 0; for (int sl = 0; sl < kGapSlices; ++sl) c += gapHist[sl][b];
                if (c && w.maxMs == 0) w.maxMs = (b == kGapBuckets - 1) ? 65.0 : (double)b;
                w.above[b] = cum;          // gaps strictly above bucket b (bucket b holds gaps in (b-1, b])
                cum += c;
            }
            w.total = cum;
            uint32_t cumF = 0;
            for (int b = kFineBuckets - 1; b >= 0; --b) {
                uint32_t c = 0; for (int sl = 0; sl < kGapSlices; ++sl) c += gapFine[sl][b];
                w.aboveFine[b] = cumF;     // gaps strictly above b x 0.25 ms
                cumF += c;
            }
            return w;
        }
        void record(std::chrono::steady_clock::time_point now) {
            std::lock_guard<std::mutex> g(lock);
            if (packets == 0) windowStart = now;
            if (lastArrival.time_since_epoch().count() != 0) {
                float d = std::chrono::duration<float, std::milli>(now - lastArrival).count();
                if (d >= kCoalesceMs && d < 1000.0f) {  // skip redundant twins + >1s gaps
                    deltasMs[ringPos] = d;
                    ringPos = (ringPos + 1) % kRing;
                    if (ringCount < kRing) ringCount++;
                    int b = (int)std::ceil(d); if (b < 0) b = 0; if (b > kGapBuckets - 1) b = kGapBuckets - 1;
                    gapHist[gapSlice][b]++;
                    int f = (int)std::ceil(d * 4.0f); if (f < 0) f = 0; if (f > kFineBuckets - 1) f = kFineBuckets - 1;
                    gapFine[gapSlice][f]++;
                }
            }
            lastArrival = now;
            packets++;
            packetsAtomic.store(packets, std::memory_order_relaxed);
        }

        struct Quantiles {
            float p50 = -1, p95 = -1, p99 = -1, maxMs = -1;
            float pps = 0;          // observed packet rate
            float gapPct = -1;      // % of deltas > 1.5x median (suspected loss/burst)
            int samples = 0;
        };
        // `win` must be one of the per-caller windows on this JitterStats (uiWin/statsWin/abufWin) so it shares
        // the same lifetime and `lock` protection as the ring. See RateWindow above.
        Quantiles compute(RateWindow& win) const {
            Quantiles q;
            std::array<float, kRing> copy;
            uint64_t pk;
            double ppsInterval = 0.0; uint64_t ppsPkts = 0;
            {
                std::lock_guard<std::mutex> g(lock);
                auto now = std::chrono::steady_clock::now();
                // Rolling rate: packets since THIS caller's previous compute().
                if (win.snapTime.time_since_epoch().count() != 0) {
                    ppsInterval = std::chrono::duration<double>(now - win.snapTime).count();
                    ppsPkts = packets - win.snapPackets;
                }
                win.snapTime = now; win.snapPackets = packets;
                if (ringCount < 32) { q.samples = ringCount; return q; }
                copy = deltasMs; q.samples = ringCount;
                pk = packets;
            }
            (void)pk;
            if (ppsInterval > 0.15) q.pps = (float)(ppsPkts / ppsInterval);
            auto begin = copy.begin(), end = copy.begin() + q.samples;
            auto nth = [&](double p) {
                auto it = begin + (size_t)((double)(q.samples - 1) * p);
                std::nth_element(begin, it, end);
                return *it;
            };
            q.p50 = nth(0.50); q.p95 = nth(0.95); q.p99 = nth(0.99);
            q.maxMs = *std::max_element(begin, end);
            if (q.p50 > 0.0001f) {
                int gaps = 0;
                for (auto it = begin; it != end; ++it) if (*it > 1.5f * q.p50) gaps++;
                q.gapPct = 100.0f * gaps / q.samples;
            }
            return q;
        }
    };
    JitterStats jstats;
    // The same statistics keyed on the KERNEL receive timestamp
    // (SO_TIMESTAMP_MONOTONIC) instead of our user-space read time. Equal → the pairing is on the wire/NIC;
    // kernel even + user-space paired → it is this thread's wake-up pattern. Printed as [HUB-KJIT].
    JitterStats kstats;
    uint64_t jitBytesSnap = 0;   // Stats thread only — bytesIn at the last [HUB-JIT] print (→ kbps)
    uint64_t jitBytesOutSnap = 0;   // Endpoint sentBytes at the last [HUB-JIT] print (→ down_kbps)
    uint64_t latBytesInSnap = 0; uint32_t latSnapMs = 0; float latUpKbps = -1.0f;   // Per-player uplink rate for the 1-Hz /sb/hxlat line
    std::atomic<bool> capIdleMask{false};   // This player's stream announced AOO_PROTOCOL_FLAG_IDLE_MASK → it can decode masked feeds
    // Datagrams that reached this peer's sink
    // through the UNMATCHED legacy fallback (sender address matched no peer,
    // so neither the connected socket nor byAddr4 attributed it). Recv
    // thread increments (relaxed); statsWriterThread prints + resets it in
    // [HUB-JIT] as unmatched=N. Non-zero means the peer's telemetry AND
    // isolation are running on the fallback — the address the hub holds
    // for the peer is not the address it sends from.
    std::atomic<uint32_t> fallbackDispatch{0};

    // (adaptive sink buffer state). Written by addPeer +
    // statsWriterThread only; never the audio thread.
    double abufCurrentMs = -1.0;   // what the sink is actually set to (the hold in force)
    bool   abufCommitted = false;  // Per-player hold committed once after the join measurement
    bool   abufRegrown  = false;   // Unused; kept for layout stability
    // 
    int64_t holdMeasureUntilMs = 0;   // commit no earlier than this (join + 15 s)
    int64_t holdLastChangeMs = 0;
    int     holdLateWindows = 0;      // (unused since the rolling ring below)
    uint32_t holdLateRing[12] = {};   // PM: late blocks per 10-s window, last 2 min (bursty links never give 12 consecutive bad windows)
    int      holdLateIdx = 0;
    int     holdFollowWindows = 0;    // consecutive samples with the measured target a block or more above the hold
    int64_t holdNoDecayUntilMs = 0;   // after a follow the slow decay waits, so the hold cannot flap; the one-step return does not wait
    int     holdFastWindows = 3;      // calm samples required before a one-step return to the target (doubles on a relapse)
    int64_t holdFastAtMs = 0;         // time of the last one-step return
    double  holdFastToMs = 0;         // hold that step chose
    int     holdDecayWindows = 0;     // consecutive windows that would have earned a shrink (SHADOW only)
    uint32_t holdLatePrev = 0;
    uint32_t holdStallPrev = 0;       // Previous sample's 60-s STALL count (GROW-STALL sees NEW stalls)
    bool     holdGapsCleared = false;  // Join transient dropped
    int64_t  holdClearedAtMs = 0;      //   at ~5 s; the measurement window counts from here
    bool     holdQuickDone = false;    //   one QUICK jump to the 10-s measured need
    uint32_t holdStallRing[12] = {};  // STALL gaps (far longer than the hold) per 10-s sample, last 2 min
    int      holdStallIdx = 0;        // Next slot in holdStallRing; holdLatePrev: previous sample's 60-s late count, to see how many NEW late blocks arrived in the last 10 s (emergency grow)
    std::atomic<bool> holdAtCeiling { false };

    // Persistent endpoint pointer — AOO identifies sinks/sources by endpoint*,
    // so all add_sink/set_sinkoption for this peer must use the same pointer.
    void* persistentEndpoint = nullptr;
    // This listener's OSC-bundle packer.
    // Initialized in addPeer (right after persistentEndpoint). Single-owner
    // by construction: under --send-pace it is touched ONLY by the pacer
    // thread inside this listener's slot; under --cut-through ONLY by this listener's own hub-listen thread right after
    // its block barrier. The two modes are mutually exclusive and fixed at
    // start() (cut-through forces pacing off and never starts the pacer), so
    // no two threads can ever hold the builder at once.
    HubBundleBuilder outBundle;

    std::vector<float> recvBuffer;
    bool active = false;
    int recvChannels = HUB_DEFAULT_CHANNELS;
    int sendChannels = HUB_DEFAULT_CHANNELS;
    // Lock-free mirror of
    // recvChannels for the UI snapshot. Written ONLY where recvChannels is
    // written (addPeer + the sink FORMAT event handler — never the audio
    // thread); read by snapshotForUI on the message thread (relaxed). The
    // audio thread keeps reading the plain int under its existing sinkLock
    // discipline — this adds nothing to its path.
    std::atomic<int> uiRecvChannels{HUB_DEFAULT_CHANNELS};

    // Decoded audio is written by the audio thread and read by per-listener feed threads with NO sync, so a plain
    // buffer lets readers see half-old/half-new blocks (dense micro-clicks, worst on a loopback peer whose phase offset
    // is ~0). Lock-free double buffer: the audio thread writes the slot (seq+1)&1, then publishes seq+1 with release
    // semantics; readers use slot seq&1, always a COMPLETED block. A slot is only rewritten two block-periods after
    // publication, far beyond any reader's window. Zero locks, zero added latency.
    std::vector<float> decodedBlockSlots[2];
    std::vector<float*> decodedPtrSlots[2];
    std::atomic<uint32_t> decodedSeq { 0 };
    // How often feed pumps re-read the
    // same published block (dup) or jumped 2+ (skip) for THIS sender.
    std::atomic<uint32_t> feedDupReads { 0 };
    std::atomic<uint32_t> feedSkipReads { 0 };
    // AOO sink block events for THIS peer's
    // inbound stream. lost = never arrived, reordered = out of order,
    // resent = recovered via resend, gap = large sequence gap.
    std::atomic<uint32_t> blkLost { 0 };
    std::atomic<uint32_t> blkLostTotal { 0 };     // Cumulative (blkLost is a 10-s window, zeroed by the stats writer) -> /sb/hxlat
    std::atomic<float> clientRecvBufMs { -1.0f };  // The player's own receive buffer (ms), reported via /sb/hxbuf
    // The player's own receive health, last 10 s, from the same /sb/hxbuf report (-1 = never reported)
    std::atomic<int32_t> clientRxUnderruns10s { -1 }, clientRxPops10s { -1 }, clientXruns10s { -1 };
    std::atomic<int32_t> clientLatFeedAgeS { -1 };
    std::atomic<int32_t> clientRecvBufMode { -1 };   // 9th /sb/hxbuf arg: 0 = Manual, 1 = Auto, -1 = not reported; clientLatFeedAgeS: seconds since the player last APPLIED our /sb/hxlat (-1 = unknown)
    std::atomic<int> isRecording { 0 };   // Player reports it is recording (/sb/hxrec, /sb/hxbuf)
    std::atomic<int> isSendMuted { 0 };   // Player muted its own send (/sb/hxsendmute; /sb/hxbuf arg 10 re-asserts)
    std::atomic<int32_t> lanDepthBlocks { 0 };   // Live depth of this player's adaptive LAN sink in blocks (0 = not known)
    std::mutex verLock;                   // guards the two strings below (receive thread writes, UI snapshot reads)
    std::string clientVersion, clientOs;  // Player's version and system (/sb/hxver); empty for players that do not report it
    std::string clientInstrument;         // Player's instrument (third string of /sb/hxver); empty when not given
    std::atomic<int32_t> clientDevBuf { -1 }, clientDevSr { -1 };   // Player's audio buffer (samples) + rate in force
    std::atomic<uint32_t> clientRxUnderrunsTotal { 0 }, clientRxPopsTotal { 0 };
    // Last chirp-onset mach ticks seen in this peer's decoded
    // audio at the hub (equilibrium hunt; see audio thread detector).
    std::atomic<uint64_t> chirpOnsetTicks { 0 };
    std::atomic<float> resamplerPeakMs { 0.0f };  // Sink resampler backlog peak
    std::atomic<uint32_t> blkReordered { 0 };
    std::atomic<uint32_t> blkResent { 0 };
    std::atomic<uint32_t> blkGap { 0 };

    // Per-listener mix overrides keyed by sender userName. When this peer is the *listener*,
    // mPeers[i]->mixOverrides["Player A"] controls how that sender's audio appears in this peer's mix-minus-self output.
    // Written under mPeerMutex; the audio thread snapshots into a thread-owned coefficient table under that same
    // lock and reads its snapshot lock-free.
    std::unordered_map<std::string, HubMixOverride> mixOverrides;


    // GUI/observability fields.
    // joinedAtMs is set off-audio-thread when the peer is added.
    // lastBlocks / lastBlocksNonZero are written ONLY from the audio thread's
    // existing 2s stats branch (NOT per-block), so they add no hot-path cost.
    // smoothedRttMs is written ONLY from sendThread via hub_source_event_handler
    // on AOO_PING_EVENT. Reads happen on the JUCE message thread via
    // SonobusHubServer::snapshotForUI() — all under std::memory_order_relaxed
    // (display data, no synchronization invariant attached).
    int64_t joinedAtMs = 0;
    std::atomic<uint64_t> lastBlocks{0};
    std::atomic<uint64_t> lastBlocksNonZero{0};
    std::atomic<float> smoothedRttMs{-1.0f};
    // The /sb/pngack RTT is the MEDIAN of the last 5 replies, not an EMA: one late reply (a scheduling stall on the
    // player, a lost/late datagram) must not distort the displayed RTT for several seconds. Receive-thread only.
    float rttRing[5] = { -1.0f, -1.0f, -1.0f, -1.0f, -1.0f };     // smoothedRttMs samples (source-side AOO ping + /sb/pngack)
    int   rttRingIdx = 0;
    float rttUpRing[5] = { -1.0f, -1.0f, -1.0f, -1.0f, -1.0f };   // smoothedRttUpMs samples (sink-side AOO ping)
    int   rttUpRingIdx = 0;
    // The /sb/pngack path runs on the network RECV thread while rttRing is fed by the stage thread
    // (hub_source_event_handler). Each writer owns its own ring so neither races the other.
    float rttAckRing[5] = { -1.0f, -1.0f, -1.0f, -1.0f, -1.0f };  // smoothedRttMs samples from /sb/pngack (recv thread only)
    int   rttAckRingIdx = 0;
    // Push a sample into a 5-slot ring and return the median of what is filled (outlier-proof RTT display).
    static float medianPush(float* ring, int& idx, float sample) {
        ring[idx] = sample; idx = (idx + 1) % 5;
        float tmp[5]; int cnt = 0;
        for (int k = 0; k < 5; ++k) if (ring[k] >= 0.0f) tmp[cnt++] = ring[k];
        for (int a = 1; a < cnt; ++a) { float v = tmp[a]; int b = a - 1; while (b >= 0 && tmp[b] > v) { tmp[b + 1] = tmp[b]; --b; } tmp[b + 1] = v; }
        return cnt > 0 ? tmp[cnt / 2] : sample;
    }
    // Inbound RTT (sink-side ping from this peer's source → our sink → echo). Captured in hub_sink_event_handler on
    // AOO_PING_EVENT. Pair with smoothedRttMs (down-direction, source-side ping) to display to/from latency in the GUI peer table.
    std::atomic<float> smoothedRttUpMs{-1.0f};

    // FAST CLOCK LOCK: for the reference sender only, the
    // recv thread regresses packet ARRIVAL mach-time against the sender's newest sequence (≈1500 samples/s;
    // ±0.5 ms jitter → ~0.5 ppm slope error over a 20-s window) and publishes the measured source block period
    // in mach ticks. The audio thread sets its block period from it (100 % on the first window, then gain).
    // Single writer (recv thread); readers take the published snapshot only.
    std::atomic<bool>   isClockRef{false};
    double  arrT0 = 0, arrSx = 0, arrSy = 0, arrSxx = 0, arrSxy = 0; int64_t arrN = 0; int32_t arrSeq0 = -1, arrLastSeq = -1; uint64_t arrWinStart = 0;
    uint64_t arrLastT = 0;                     // Previous sample's arrival (clump filter)
    std::atomic<uint64_t> lastDataTicks { 0 };  // Mach time of the last inbound packet from this peer (recv thread, relaxed)
    std::atomic<uint64_t> bytesIn { 0 };        // Cumulative UDP bytes received from this peer (recv thread, relaxed)
    std::atomic<double> arrPeriodTicks{0.0};   // published: measured mach ticks per source block (0 = none yet)
    std::atomic<uint32_t> arrGen{0};           // bumps on every published window

    // Peak jitter buffer fill ratio (0..100%) since the last GUI snapshot read. sendThread (1ms cadence) does
    // sample-and-atomic-max into this; snapshotForUI exchange-to-zero reads it. Catching peaks at 1ms is much more
    // informative than a GUI-rate sample, which usually catches the queue right after the audio thread drained it.
    std::atomic<float> jitterBufferPeakFillPct{0.0f};

    // Cached sender AOO userformat blob (ValueTree of per-channel labels + layout). Read from the sender's sink via
    // aoo_opt_userformat when its format event fires; applied to each listener's per-(L,S) feed source so listeners
    // see the same labels stock SonoBus's applyLayoutFormatToPeer renders for direct P2P peers.
    std::vector<char> userformatBlob;

    // Sender's most recent channel-layout blob from a /sb/clayinfo OSC message. Replayed to a new listener immediately
    // when their feed is allocated, so they get labels even if the layout was broadcast before they joined.
    std::vector<char> clayinfoBlob;
    std::mutex clayNamesLock;                 // Channel names parsed from clayinfoBlob (recv thread writes, snapshot reads)
    std::vector<std::string> channelNames;

    // Serializes the audio thread's sink->process against the format-event
    // handler's sink->setup re-configuration on AOO_SOURCE_FORMAT_EVENT.
    // Plain std::mutex (not shared_mutex) because the audio thread already
    // processes peers sequentially — no two threads ever sink->process the
    // same peer concurrently; the only contention is audio-vs-sendThread,
    // which is brief and rare (only on actual channel-count changes).
    // (shared_mutex would need macOS 10.12+; project deployment target is 10.10.)
    mutable std::mutex sinkLock;

    // Per-peer decoded
    // block, populated by the central audio thread's sink loop (Step 1) and
    // read by OTHER peers' per-listener threads (Step 2). No mutex needed
    // because the audio thread blocks on a fork-join barrier waiting for all
    // listener threads to signal done, so no concurrent reader+writer can
    // touch the same peer's decodedBlock. decodedChannels carries the
    // channel-count snapshot from the writing block so readers know how much
    // valid audio is in the buffer (format changes are rare and serialized
    // via sinkLock; they take effect on the next block).
    std::vector<float> decodedBlock;
    std::vector<float*> decodedPtrs;
    int decodedChannels = 0;

    // Per-listener outbound feed-pump thread.
    // One thread per peer (created in addPeer, joined in ~HubPeer). The
    // central audio thread (acting as scheduler) signals every active
    // listener via listenerCv on each block; the listener thread runs Step 2
    // (per-(listener × sender) feed source->process) in parallel with all
    // other listeners, on a separate CPU core. This eliminates the
    // O(N²) feed pump from the central audio thread's per-block budget.
    std::unique_ptr<std::thread> listenerThread;
    std::atomic<bool> listenerThreadShouldStop{false};
    std::mutex listenerMutex;
    std::condition_variable listenerCv;
    uint64_t listenerGoGen = 0;                       // Guarded by listenerMutex; bumped ONLY by the audio thread (F9)
    std::atomic<uint64_t> listenerDoneGen{0};          // listener thread → audio thread, diagnostic only; the barrier itself is SonobusHubServer::mBarrierPending
    std::atomic<uint64_t> listenerSourceMaxTicks{0};   // per-block max, drained by audio thread
    // True from addPeer (before the thread is started) until the listener thread clears it UNDER listenerMutex on
    // its way out. The audio thread re-checks this under the same mutex before counting the listener into the per-block
    // barrier, so an exiting listener can never be counted without acknowledging, even if a stop lands between the
    // audio thread's trigger and the listener's wake.
    std::atomic<bool> listenerAlive{false};

    // Explicit listener-thread shutdown, callable from the peer-removal paths BEFORE the shared_ptr is dropped from mPeers.
    //
    // The listener thread must hold a strong ref for its whole lifetime because it blocks in listenerCv.wait() and the
    // cv/mutex/stop-flag all live inside HubPeer, so ~HubPeer alone would be an ownership cycle that leaks one RT thread
    // per removed peer. A per-iteration weak_ptr::lock() does not break the cycle and adds a worse hazard: if the lock()
    // temporary became the LAST ref, ~HubPeer would join its own thread (UB).
    //
    // So every removal path (removePeer, PEER_LEAVE handler, stop) retains the shared_ptr, erases it from mPeers,
    // RELEASES mPeerMutex, then calls this. The mutex comes first because a freshly-started thread takes mPeerMutex
    // (shared) to find itself, and joining under the exclusive lock could deadlock on a join-right-after-add race.
    // Once joined, the thread's own strong ref is released and the peer destructs normally when the last snapshot ref
    // drops. ~HubPeer calls this as a no-op backstop, so a last ref dropped on the AUDIO thread never joins anything there.
    // NEVER call from the listener thread itself.
    void stopAndJoinListenerThread() {
        if (!listenerThread) return;
        {
            // Set the stop flag UNDER listenerMutex (the wait predicate tests it under the same mutex, so no lost wakeup)
            // and do NOT bump listenerGoGen: every goGen advance means exactly one audio-thread barrier count that the
            // listener must acknowledge, and a stop-side bump would corrupt that count.
            std::lock_guard<std::mutex> lock(listenerMutex);
            listenerThreadShouldStop.store(true, std::memory_order_release);
        }
        listenerCv.notify_all();
        if (listenerThread->joinable()) listenerThread->join();
        listenerThread.reset();
    }

    // Tear down AOO objects + heap endpoint when the last shared_ptr drops.
    // Callers (removePeer, PEER_LEAVE handler, stop) stop+join the listener
    // Thread explicitly BEFORE dropping their mPeers ref;
    // the audio-thread snapshot keeps the peer alive until its next snapshot
    // rebuild, so mid-block destruction can't race with sink->process or
    // outFeeds[*]->source->process. outFeeds destructs via unique_ptr cascade.
    ~HubPeer() {
        // Backstop only — removal paths have already
        // joined the thread, making this a cheap early return.
        stopAndJoinListenerThread();
        if (sink) sink->uninvite_all();
        for (auto& kv : outFeeds) {
            if (kv.second && kv.second->source) {
                kv.second->source->stop();
                kv.second->source->remove_all();
            }
        }
        if (persistentEndpoint) {
            delete reinterpret_cast<HubEndpoint*>(persistentEndpoint);
            persistentEndpoint = nullptr;
        }
    }
};

class SonobusHubServer {
public:
    SonobusHubServer();
    ~SonobusHubServer();

    bool start(const std::string& aooServerHost, int aooServerPort,
               const std::string& username, const std::string& groupName,
               const std::string& groupPassword = "",
               int localUdpPort = 0,
               int sampleRate = HUB_DEFAULT_SAMPLERATE,
               int blockSize = HUB_DEFAULT_BLOCKSIZE,
               int channels = HUB_DEFAULT_CHANNELS);

    void stop();
    bool isRunning() const { return mRunning.load(); }

    void setOpusEnabled(bool enabled) { mUseOpus = enabled; }
    void setOpusBitrate(int bitrate) { mOpusBitrate = bitrate; }
    void setOpusComplexity(int complexity) { mOpusComplexity = complexity; }
    void setRunLocalServer(bool run, int port = 10999) { mRunLocalServer = run; mLocalServerPort = port; }

    // Override the public IP advertised to other peers, useful when the AOO
    // server (typically embedded in this same hub) sees our connection coming
    // from the LAN gateway address rather than the WAN IP because the router
    // does asymmetric hairpin NAT. Port == 0 keeps the auto-detected port.
    void setPublicAddressOverride(const std::string& host, int port = 0) { mPublicAddressHost = host; mPublicAddressPort = port; }
    // Port-forward self-test. host:port (its server_port) of a hub that probes us back from outside.
    void setPortCheckHelper(const std::string& hostPort) { mPortCheckHelper = hostPort; }
    void requestPortCheck() { mPortCheckRequested.store(true, std::memory_order_relaxed); }   // Web page button re-runs the self-test
    // Connection/security log — every join attempt and every rejection, with a wall-clock stamp:
    // "[HUB-CONN] 05 join-refused addr=192.0.2.11:5 user='x' group='g' wrong password" to stdout
    // (hub.log) AND an in-memory ring of the last 2000 lines for the web page's Log tab. rateLimited = at most one
    // line per (kind, addr) per 10 s (odd UDP traffic can arrive at packet rate).
    static void connLog(const char* kind, const std::string& addr, const std::string& user, const std::string& group, const std::string& detail, bool rateLimited = false);
    static std::vector<std::string> connLogLines(int n);
    // "Recent players": the distinct players (by user name) the hub saw in a session, from its own join/leave events:
    // when they last joined/left, how many joins, LAN/WAN, last address. Location is resolved by the viewer (web page /
    // Mac GUI) from `ip`, like the live table. Players live inside a SessionRecord (one per group name, see below);
    // recentPlayers() returns the CURRENT session's players.
    struct RecentPlayer {
        std::string user, ip;
        std::string location;                 // filled by the viewer, never stored
        bool lan = false;
        int64_t firstSeenMs = 0, lastJoinMs = 0, lastLeftMs = 0;   // epoch ms; lastLeftMs < lastJoinMs = still connected
        int joins = 0;
    };
    std::vector<RecentPlayer> recentPlayers(int max = 30) const;   // current session's players, newest first
    int playersSinceStart() const;   // distinct player names that joined since the hub started serving
    // "Recent sessions" — one record per GROUP name the hub has served: when it was first/last
    // used, the group password, a snapshot of EVERY hub setting in force (key -> text, as hub_settings.json stores
    // them; given by the settings layer via setSessionSettingsSnapshot() before start()) and the personnel seen in that
    // group (distinct by user name). Up to 50 sessions, keyed by group name — serving the same group again updates
    // its record (lastUsed, password, settings, players merged). Persisted whole to hub_sessions.json next to
    // hub_settings.json (setSessionsFile(); atomic rename) on every change; joins/leaves/starts are rare events.
    struct SessionRecord {
        std::string group, password;
        int64_t firstUsedMs = 0, lastUsedMs = 0;                              // epoch ms
        std::vector<std::pair<std::string, std::string>> settings;            // key -> value text, settings-file order
        std::vector<RecentPlayer> players;                                    // oldest first, newest last
    };
    void setSessionsFile(const std::string& path);                                                   // before start(); loads the file if present
    void setSessionSettingsSnapshot(std::vector<std::pair<std::string, std::string>> kv);             // before start(); recorded into the session start() opens
    std::vector<SessionRecord> sessions() const;                                                      // newest (last used) first
    bool sessionFor(const std::string& group, SessionRecord& out) const;                              // false = no such group
    int sendGroupMessage(const std::string& text);   // chat to every player, from "Hub"; returns how many got it
    bool removeSessionPlayer(const std::string& group, const std::string& user, std::string& err);   // never a connected player
    bool removeSession(const std::string& group, std::string& err);                                  // The "X" on a Recent sessions row; the current session cannot be removed

    // Tell the embedded AOO server which IPv4 subnet is LAN. When set, the
    // server classifies recipients of PEER_JOIN broadcasts by their TCP
    // source IP and advertises LAN-only addresses to LAN recipients. Lets
    // the hub be launched with --public-address <WAN-IP> without confusing
    // same-LAN peers (including this machine's own SonoBus client).
    void setLanSubnet(const std::string& cidr) { mLanSubnetCidr = cidr; }

    // Per-peer sink jitter-buffer tuning. Base value
    // (default 30 ms) applies to all peers; LAN value, when >0 and a
    // --lan-subnet is configured, applies only to CIDR-matched peers. Both are
    // read at sink setup/re-setup time only — never on the audio thread.
    void setSinkBufferMs(double ms)    { if (ms > 0) mSinkBufferMs = ms; }
    void setSinkBufferLanMs(double ms) { mSinkBufferLanMs = ms; }
    // --sink-buffer-auto.
    // Peers join at the starter size (base / LAN value), the stats thread
    // watches their measured jitter for ~5s, then commits a right-sized
    // buffer ONCE (AOO set_buffersize = queue reset = one brief blip, so we
    // never chase continuously). Known players skip even that: their last
    // committed size is cached by userName and applied at addPeer, before
    // audio flows. If a committed peer's gap% spikes, we regrow once to the
    // base size and hold for the session.
    // Hold mode — 0 = Jam (≤2 late blocks/min per player), 1 = Print (0/min). Live.
    void setHoldMode(int m)            { mHoldMode.store(m ? 1 : 0, std::memory_order_relaxed); mHoldResizeAll.store(true, std::memory_order_relaxed); }
    int  getHoldMode() const           { return mHoldMode.load(std::memory_order_relaxed); }
    // Enable paced fan-out emission (see mSendPacing).
    void setSendPacing(bool on)        { mSendPacing.store(on, std::memory_order_relaxed); }
    void setPaceDither(bool on)        { mPaceDither.store(on, std::memory_order_relaxed); }
    void setPaceSpreadPct(int pct)     { mPaceSpreadPct.store(pct < 5 ? 5 : (pct > 95 ? 95 : pct), std::memory_order_relaxed); }
    void setPaceMinSlotUs(int us)      { mPaceMinSlotUs.store(us < 0 ? 0 : (us > 500 ? 500 : us), std::memory_order_relaxed); }
    // Cut-through emission (see mCutThrough). Set before
    // start(); overrides --send-pace (resolved + logged in start()).
    void setCutThrough(bool on)        { mCutThrough.store(on, std::memory_order_relaxed); }
    // --inline-listeners — run the per-listener
    // fan-out on the audio thread itself instead of on N listener RT threads
    // (see mInlineActive). Set before start(); resolved + logged there.
    void setInlineListeners(bool on)   { mInlineListenersReq.store(on, std::memory_order_relaxed); }
    // OSC-bundle packing of a listener slot's
    // feed datagrams (see HubBundleBuilder). Effective under --send-pace (the
    // pacer is the slot-serialized emitter) or --cut-through (the listener's
    // own thread is the emitter). Read at addPeer (cap) and once per emitted
    // block (enable) — one relaxed load, never per feed.
    void setBundling(bool on)          { mBundleEnabled.store(on, std::memory_order_relaxed); }
    void setBundleMtu(int bytes)       { if (bytes > 0) mBundleMtu = bytes; }
    // Per-peer redundancy (set before start; read at feed creation)
    void setDscp(bool on) { mDscpEnabled = on; }   // Set before start
    // One connected send socket per listener (see
    // HubDest::connFd). Set before start(); read at start() (SO_REUSEPORT
    // on the shared socket, banner), addPeer (socket creation), the recv
    // thread's poll set and the [HUB-PACE] line.
    void setConnectedSockets(bool on) { mConnectedSockets = on; }
    void setRedundancy(int lan, int wan) { mRedundancyLan = lan < 1 ? 1 : (lan > 4 ? 4 : lan);
                                           mRedundancyWan = wan < 1 ? 1 : (wan > 4 ? 4 : wan); }
    // Dump one peer's decoded audio (ch0/ch1
    // interleaved float32 @ hub rate) to a file for offline noise analysis.
    // Short-lived debug instrument; fwrite to a 4MB-buffered FILE.
    void setDumpPeer(const std::string& name, int secs) { mDumpPeerName = name; mDumpSecs = secs; }

    int getNumConnectedPeers() const;

    // ─── GUI snapshot API ───
    // POD copies populated under a brief mPeerMutex acquire from the JUCE
    // message thread (via SonobusHubMainWindow's timer). The audio thread
    // never touches these structures. snapshotForUI() contends ONLY against
    // networkRecvThread / addPeer / removePeer paths that already hold
    // mPeerMutex — NOT against the audio thread's own snapshot block (which
    // is wait-free apart from its own lock-and-copy). This is a non-audio-thread
    // observer.
    struct UiFeedSnapshot {
        int32_t senderId = -1;
        std::string senderUserName;  // resolved if sender still present
        int channels = 0;
    };
    struct UiPeerSnapshot {
        int32_t id = -1;
        std::string userName;
        std::string location;   // Filled by the GUI from HubGeoResolver ("LAN", "City, ST", "" pending)
        int recvChannels = 0;
        std::string ipAddr;
        int port = 0;
        uint64_t lastBlocks = 0;
        uint64_t lastBlocksNonZero = 0;
        float smoothedRttMs = -1.0f;     // hub→peer round-trip (source ping)
        float smoothedRttUpMs = -1.0f;   // peer→hub round-trip (sink ping)
        int64_t joinedAtMs = 0;
        float jitterBufferFillPct = -1.0f;   // Hub sink jitter buf fill (0..100)
        // Per-peer network jitter telemetry (recv-thread
        // arrival stats). p50 ≈ nominal packet cadence; spread above it is
        // the jitter that sink buffers must absorb. -1 = not enough samples.
        float jitP50Ms = -1.0f;
        float jitP95Ms = -1.0f;
        float jitP99Ms = -1.0f;
        float jitMaxMs = -1.0f;
        float pktRate = 0.0f;                // observed packets/sec
        float gapPct = -1.0f;                // % deltas >1.5x median (loss/burst proxy)
        float sinkBufMs = -1.0f;             // committed hub sink buffer for this peer (the hold in force)
        bool  holdAtCeiling = false;         // This player's link needs more than the ceiling allows
        float clientRecvBufMs = -1.0f;       // The player's own receive buffer (reported)
        int clientRxUnderruns10s = -1, clientRxPops10s = -1, clientXruns10s = -1;   // Player-side, last 10 s
        int clientLatFeedAgeS = -1;   // Player-reported age of our latency feed, s (-1 unknown)
        int clientRecvBufMode = -1;   // 0 Manual / 1 Auto / -1 unknown
        std::vector<std::string> channelNames;   // Per-channel labels from the player's layout
        uint32_t clientRxUnderrunsTotal = 0, clientRxPopsTotal = 0;   // Since the player joined
        bool isRecording = false;   
        bool isSendMuted = false;   // Send muted (relayed to everyone)
        std::string clientVersion, clientOs;   // Player's version and system; empty when the player does not report it
        std::string clientInstrument;          // Player's instrument; empty when not given
        int clientDevBuf = -1, clientDevSr = -1;   // Player's audio buffer + sample rate
        bool  isLan = false;
        uint64_t lostBlocks = 0;             // Total blocks of this player's stream the hub lost (never arrived, not resent) — the 'Drops' column
        uint64_t bytesIn = 0;                // Cumulative bytes this peer sent us (Up)
        uint64_t bytesOut = 0;               // cumulative bytes we sent this peer (Down)
        float inBytesPerSec = -1.0f;         // filled by the GUI from successive snapshots (-1 = not yet)
        float outBytesPerSec = -1.0f;
        std::vector<UiFeedSnapshot> outFeeds;
    };
    struct UiSnapshot {
        std::vector<UiPeerSnapshot> peers;
        int sampleRate = 0;
        int blockSize = 0;
        int channels = 0;
        bool useOpus = true;
        int opusBitrate = 0;
        std::string groupName;
        std::string groupPassword;           // Copy Group Link carries it like the client's
        std::string publicAddressHost;
        int publicAddressPort = 0;
        std::string advertisedLanHost;      // What LOGIN advertises when signaling is via loopback
        std::string advertisedPublicHost;
        int advertisedPublicPort = 0;
        int localUdpPort = 0;       // hub audio UDP port (default 11000)
        int aooServerPort = 0;      // AOO server signaling TCP+UDP port (default 10999)
        int64_t hubStartedAtMs = 0;
        // Aggregate bandwidth counters (cumulative since hub
        // start). GUI computes per-second rates by diffing successive snapshots.
        uint64_t bytesInTotal = 0;
        uint64_t bytesOutTotal = 0;
        // CPU usage of the hub process as a percentage of one core. Sampled
        // off audio thread via getrusage() on each snapshotForUI call.
        float hubCpuPct = 0.0f;
        int holdMode = 0;                    // 0 Jam, 1 Print
        std::string portCheckText;           // Port-forward self-test result line
        std::string portLocalText;           // Local tier — sockets bound + host firewall inspection
        int64_t portCheckVerifiedAtMs = 0;   // Epoch ms of the last player-verified check (viewer renders local time)
        int portCheckState = 0;              // 0 running/not run, 1 all forwarded, 2 a port failed, 3 skipped
        // Stage limits and the stage directory listing
        int stageMaxPlayers = 0;             // 0 = no limit
        int stageMaxChannels = 0;            // per player; 0 = no limit
        int stageListingState = 0;           // 0 listing off, 1 listed, 2 not listed (reason in text), 3 directory not reachable, 4 checking
        std::string stageListingText;        // one line for the status area, e.g. "Listed at stages.randomplanet.com"
    };
    UiSnapshot snapshotForUI() const;

    // ─── Stage limits, kick and the stage directory (control path only, never the audio threads) ───
    struct StageSettings {
        int maxPlayers = 20;                 // 0 = no limit
        int maxChannels = 12;                // per player; 0 = no limit (HUB_MAX_CHANNELS_PER_PEER still applies)
        bool list = false;                   // listed at the stage directory (the operator opts in)
        std::string genre = "Any";
        std::string description;
        std::string city;
        std::string country;                 // two-letter code, empty = not given
        int names = 0;                       // player names in the listing: 0 automatic (shown on an open stage, hidden on a locked one), 1 show, 2 hide
    };
    void applyStageSettings(const StageSettings& s);   // any thread; limits apply to the next join / format change
    StageSettings stageSettings() const;
    uint64_t stageSettingsGeneration() const { return mStageGen.load(std::memory_order_relaxed); }
    uint64_t stageMembersGeneration() const { return mStageMembersGen.load(std::memory_order_relaxed); }   // changes on every join and leave
    // Removes a player now and keeps that name and address out for blockMinutes. False = no such player.
    bool kickPlayer(const std::string& userName, int blockMinutes = 60);
    void setStageListingStatus(int state, const std::string& text);   // written by the directory client
    // Join check used by the built-in connection server: empty = allow, else the reason shown to the player.
    std::string stageJoinRefusal(const std::string& ip, const std::string& user, int othersInStage);
    int getSampleRate() const { return mSampleRate; }
    int getBlockSize() const { return mBlockSize; }
    int getMixerChannels() const { return mChannels; }
    bool getOpusEnabled() const { return mUseOpus; }
    int getOpusBitrate() const { return mOpusBitrate; }
    const std::string& getGroupName() const { return mGroupName; }
    const std::string& getPublicAddressHost() const { return mPublicAddressHost; }
    int getPublicAddressPort() const { return mPublicAddressPort; }
    const std::string& getAdvertisedLanHost() const { return mAdvertisedLanHost; }         
    const std::string& getAdvertisedPublicHost() const { return mAdvertisedPublicHost; }

private:
    void audioProcessThread();
    // Port-forward self-test (see SonobusHubServer.cpp "PORT CHECK").
    void portCheckThreadMain();
    bool portCheckOnce(std::string& text, int& state);
    void handlePortProbeRequest(const struct sockaddr_storage* src, int64_t token, int udpPort, int serverPort);
    std::string mPortCheckHelper = "sonobus-rp-hub-check.randomplanet.com:10999";   // Hostname, never the raw host IP (it would show in other people's hub Settings)
    mutable std::mutex mPortCheckMutex;
    std::string mPortCheckText;
    int mPortCheckState = 0;
    std::atomic<int64_t> mPortCheckToken{0};
    std::atomic<bool> mPortCheckRequested{false};
    std::atomic<uint32_t> mPortProbeAudioHits{0};
    // Player-assisted external check: a connected WAN player probes us back (same /sb/fwprobe protocol the helper hub
    // speaks). The configured checker is asked FIRST; a connected WAN player is the fallback when it cannot be reached.
    std::string mPortLocalText;                       // under mPortCheckMutex
    int64_t mPortCheckVerifiedAtMs = 0;               // under mPortCheckMutex
    std::atomic<int64_t> mPortProbeResultToken{0};    // token of the last /sb/fwprobe/result that reached the hub socket
    std::atomic<int> mPortProbeResultTcp{-1};
    std::string localPortCheckText() const;
    bool portCheckViaPlayer(const std::string& user, const std::string& ip, int port, std::string& text, int& state, bool& answered);
    bool portCheckViaHelper(std::string& text, int& state, bool& answered);   // Answered = checker reached and gave a verdict
    // Recent players / recent sessions (see RecentPlayer + SessionRecord above). All under
    // mRecentMutex; the current session is the record whose group == mSessionGroup (opened by start()).
    void recentNoteJoin(const std::string& user, const std::string& ip, bool lan);
    void recentNoteLeave(const std::string& user);
    void sessionBegin(const std::string& group, const std::string& password);   // called by start(): open/update the record for `group`
    SessionRecord* currentSessionLocked();        // under mRecentMutex; nullptr before start()
    void sessionsSaveLocked() const;              // under mRecentMutex
    mutable std::mutex mRecentMutex;
    std::deque<SessionRecord> mSessions;          // least recently used first, current/newest last
    std::string mSessionsFile;                    // empty = in memory only
    std::string mSessionGroup;                    // group of the current session ("" before start())
    std::set<std::string> mPlayersSinceStart;     // under mRecentMutex; names that joined since start()
    std::vector<std::pair<std::string, std::string>> mSessionSettings;   // snapshot handed in before start()
    std::unique_ptr<std::thread> mPortCheckThread;
    void networkRecvThread();
    void clientRunThread();
    void sendThread();

    // Per-block audio-thread timing collected via atomics on the audio thread (no mutex, no I/O), then flushed every 5s
    // to stats.csv in the hub data dir by a separate writer thread, to correlate per-block budget compression with audible degradation.
    void statsWriterThread();
    void latencyBroadcastTick();   // /sb/ping + /sb/hxlat every 10 s from statsWriterThread
    std::unique_ptr<std::thread> mStatsThread;

    // ─── RT-safe telemetry log ring ────────────────
    // The audio thread may not gain syscalls/locks/allocs/IO, and stdout is line-buffered (under --log, freopen'd), so
    // a std::cout write there would be a blocking write() on the RT deadline path. Instead the audio thread pushes
    // fixed-size POD records into this preallocated lock-free SPSC ring (single producer = audio thread, single consumer
    // = statsWriterThread, which drains every ~200ms and does ALL the printf formatting). Tools grep these log tags, so
    // the line formats must stay stable.
    // Overflow policy: if the ring is full the producer DROPS the record and bumps mRtLogDrops (relaxed fetch_add); it
    // never blocks or waits. The consumer reports drops via a [HUB-RTLOG] line when nonzero.
    enum class RtLogTag : uint32_t { Chirp = 1, Resamp = 2, Audio = 3, Clock = 4, ClockFast = 5 };   // Clock = clock-lock step; ClockFast = arrival-regression step
    struct RtLogRec {                 // POD, fixed size — no heap, no dtors
        uint32_t tag = 0;             // RtLogTag
        int32_t  peerIndex = -1;      // audio-thread snapshot slot (diagnostic)
        char     peerName[48] = {};   // bounded copy; truncation is acceptable
        double   f0 = 0, f1 = 0, f2 = 0, f3 = 0;  // per-tag numeric payload
        uint64_t tick = 0;            // mach_absolute_time at push
    };
    static constexpr uint32_t kRtLogRingSize = 256;   // power of two
    RtLogRec mRtLogRing[kRtLogRingSize];
    std::atomic<uint32_t> mRtLogHead{0};   // written by producer only
    std::atomic<uint32_t> mRtLogTail{0};   // written by consumer only
    std::atomic<uint64_t> mRtLogDrops{0};  // producer-side overflow count
    // Producer (audio thread). Wait-free: two atomic loads, POD stores, one
    // release store. No locks, no syscalls, no allocation, no formatting.
    inline void rtLogPush(RtLogTag tag, int32_t peerIndex, const char* name,
                          uint64_t tick,
                          double f0 = 0, double f1 = 0,
                          double f2 = 0, double f3 = 0) {
        const uint32_t head = mRtLogHead.load(std::memory_order_relaxed);
        const uint32_t tail = mRtLogTail.load(std::memory_order_acquire);
        if (head - tail >= kRtLogRingSize) {          // full → DROP, never block
            mRtLogDrops.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        RtLogRec& r = mRtLogRing[head % kRtLogRingSize];
        r.tag = (uint32_t)tag;
        r.peerIndex = peerIndex;
        uint32_t i = 0;
        if (name) {
            for (; i < sizeof(r.peerName) - 1 && name[i] != '\0'; ++i)
                r.peerName[i] = name[i];
        }
        r.peerName[i] = '\0';
        r.f0 = f0; r.f1 = f1; r.f2 = f2; r.f3 = f3;
        r.tick = tick;
        mRtLogHead.store(head + 1, std::memory_order_release);  // publish
    }
    void drainRtLog();   // consumer (stats thread): format + printf

    // The central audio
    // thread is a scheduler. It runs Phase A (peer snapshot + mix-coef
    // flatten) and Step 1 (per-peer sink decode), then fans Step 2 (per-
    // (listener × sender) feed pump) out to N parallel per-peer threads
    // (HubPeer::listenerThread). These members hold the per-block context
    // the listener threads consume, populated by the audio thread BEFORE
    // it raises listenerCv on each peer. Safe to read concurrently from
    // listener threads because the audio thread blocks on the fork-join
    // barrier (mBarrierPending) before
    // overwriting any of these.
    std::vector<std::shared_ptr<HubPeer>> mCurrentSnap;
    std::vector<HubMixOverride> mCurrentCoefs;
    std::vector<uint8_t> mCurrentListenerSolo;
    int mCurrentNumPeers = 0;
    // MCurrentSnap[i]->id flattened into a plain
    // array at snapshot-rebuild time so the listener threads' per-feed
    // sender lookup compares ints in one cache line instead of chasing a
    // shared_ptr → HubPeer per slot. Empty slots hold kNoPeerId.
    static constexpr int32_t kNoPeerId = INT32_MIN;
    int32_t mCurrentIds[HUB_MAX_PEERS] = {};
    // Generation counters that gate the Phase A
    // rebuild. mPeerListGen: bumped by rebuildPeerSnapshotLocked() (peer
    // add/remove/addr change). mMixGen: bumped (under exclusive mPeerMutex)
    // wherever a HubPeer::mixOverrides entry is written — the ONLY writer is
    // handleSonobusControlMessage (/sb/hxmute, /sb/hxgain, /sb/hxsolo). The
    // audio thread rebuilds mCurrentSnap / mCurrentCoefs / mCurrentIds only
    // when one of them changed; in steady state Phase A is two acquire loads.
    std::atomic<uint64_t> mMixGen{0};

    // Fork-join barrier. Audio thread adds one per
    // listener it signals (under that listener's listenerMutex, BEFORE
    // ++listenerGoGen); each signaled listener subtracts exactly one when its
    // fan-out is done (or, if it is exiting with an unacknowledged go, on its
    // way out). Audio thread: short bounded pause-spin on the counter, then
    // blocks on mBarrierCv; the listener whose decrement reaches zero takes
    // mBarrierMutex (briefly) and notifies. Ordering: every listener's
    // fetch_sub is acq_rel and the audio thread's reads are acquire, so all
    // fan-out side effects (feed->source->process/send, listenerSourceMaxTicks)
    // happen-before the audio thread's next-block writes to mCurrent*.
    std::atomic<int32_t> mBarrierPending{0};
    std::mutex mBarrierMutex;
    std::condition_variable mBarrierCv;
    inline void listenerBarrierDone() {
        if (mBarrierPending.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            // Take+drop the mutex so a waiter between its predicate check
            // and cv.wait() cannot miss this notify (classic lost-wakeup
            // guard). Uncontended in practice: the audio thread is either
            // still pause-spinning (never touched the mutex) or already
            // parked in wait() (mutex released).
            { std::lock_guard<std::mutex> lk(mBarrierMutex); }
            mBarrierCv.notify_one();
        }
    }
    // SYNTHETIC monotonic osctime for the outbound feed sources. Wall-clock jitter would feed every source's DLL timer:
    // a scheduling hiccup above ~43 ms makes sources emit empty "dropped" blocks that disrupt every listener sink.
    // Value is
    //   streamBaseOscTime + blockIndex * blockPeriod   (NTP 32.32 fixed-point; plain uint64 addition is correct, carry
    //   propagates low→high).
    // Owned by the audio thread (written pre-fan-out each block, same publication rule as the other mCurrent* members);
    // listener threads read it between go/done generations. Real aoo_osctime_get() wall time is still used for
    // sink->process() and everything protocol-facing.
    uint64_t mFeedOscTime = 0;
    // Per-listener entry point (created in addPeer, joined in ~HubPeer). Takes the peer's shared_ptr directly (strong-ref
    // lifetime: see stopAndJoinListenerThread), so the thread never contends the peer mutex and cannot miss itself after
    // an add→remove race and exit silently, which would leave the audio thread on a barrier count nobody can acknowledge.
    void listenerProcessThread(std::shared_ptr<HubPeer> selfPeer);
    struct HubStats {
        std::atomic<uint64_t> totalBlocks{0};
        std::atomic<uint64_t> deadlineMisses{0};    // blocks taking > blockNs
        std::atomic<uint64_t> blockNsMax{0};
        std::atomic<uint64_t> snapNsMax{0};
        std::atomic<uint64_t> sinkLoopNsMax{0};
        std::atomic<uint64_t> feedPumpNsMax{0};
        std::atomic<uint64_t> perPeerSinkNsMax{0};
        std::atomic<uint64_t> perFeedSourceNsMax{0};
        // 256-bucket histogram, 16µs per bucket → 0..4.1ms range.
        // Anything >4.08ms gets bucket 255 (already 1.5× block budget).
        std::atomic<uint32_t> hist[256]{};
    };
    HubStats mStats;

    HubPeer* addPeer(int32_t peerId, const std::string& userName, void* sockaddr);
    void removePeer(int32_t peerId);
    // RemovePeer helpers. The first runs
    // under EXCLUSIVE mPeerMutex (mPeerMutex -> outFeedsLock, never nested the
    // other way) and only unlinks; the second destroys with NO lock held.
    std::vector<std::unique_ptr<HubOutFeed>> unlinkFeedsForDepartedPeerLocked(int32_t departedId);
    static void retireFeeds(std::vector<std::unique_ptr<HubOutFeed>>& feeds, int32_t departedId);
    HubPeer* findPeerByEndpoint(const struct sockaddr* addr);

    void setupSinkFormat(HubPeer* peer);
    // Configure an AOO source's wire format. Used for per-(listener × sender)
    // outbound feeds — channel count is the sender's negotiated recvChannels.
    void setupSourceFormatFor(aoo::isource* source, int channels);
    // Allocate (or reconfigure) listener->outFeeds[sender->id] for a
    // /sb/hxfeedreq request. Holds listener->outFeedsLock for map mutation +
    // OutFeed::lock per-feed for source reconfiguration. Returns the feed.
    HubOutFeed* allocateOrUpdateFeed(HubPeer* listener, HubPeer* sender,
                                     int32_t clientOurId);
    // Re-setup every listener's
    // outFeeds[sender->id] source at newCh channels after the sender's
    // AOO_SOURCE_FORMAT_EVENT. sendThread only (called from the sink event
    // handler); takes mPeerMutex(shared, released before) -> outFeedsLock ->
    // feed->lock. `tag` is the [HUB-FEED] log verb ("rebuilt" / "sweep-rebuilt").
    void rebuildListenerFeedsForSender(HubPeer* sender, int32_t newCh, const char* tag);

    // Dispatch a non-AOO UDP packet as a SonoBus /sb/... control message
    // from a peer (the listener) telling us how to mix one specific other peer
    // (the sender, named in the payload) in their personal mix-minus-self output.
    void handleSonobusControlMessage(const char* msg, int32_t n,
                                     const struct sockaddr_storage* srcAddr,
                                     socklen_t srcAddrLen);

    static int32_t hub_endpoint_send(void* user, const char* data, int32_t size);
    static int32_t hub_client_send(void* user, const char* data, int32_t size, void* addr);
    static int32_t hub_client_event_handler(void* user, const aoo_event** events, int32_t n);
    static int32_t hub_source_event_handler(void* user, const aoo_event** events, int32_t n);
    static int32_t hub_sink_event_handler(void* user, const aoo_event** events, int32_t n);

    // Per-peer sink event dispatch (called from sendThread). Polls each peer's
    // sink for AOO_SOURCE_FORMAT_EVENT and rebuilds the sink at the sender's
    // declared channel count. Holds mPeerMutex briefly during dispatch and
    // peer->sinkLock exclusively during any re-setup.
    void pollPeerSinkEvents();
    // User-data struct carried through hub_sink_event_handler so the callback can find its way back to (this hub, that
    // peer) without ambiguity. Carries the HubPeer* directly (kept alive by the caller's snapshot shared_ptr held across
    // handle_events), never an id resolved under mPeerMutex: a shared relock in the handler deadlocks in a triangle.
    // sendThread holds a listener's outFeedsLock while its handler queues behind an exclusive mPeerMutex waiter (peer add),
    // while networkRecvThread holds mPeerMutex-shared and wants that same outFeedsLock (allocateOrUpdateFeed).
    // Handlers must take NO locks.
    struct PeerSinkEventCtx { SonobusHubServer* hub; HubPeer* peer; };

    // Per-feed source event dispatch (called from sendThread). Polls each
    // outbound feed source for AOO_PING_EVENT and updates the listener's
    // smoothedRttMs via an EMA. Off the audio thread; takes mPeerMutex
    // briefly only to snapshot peers + look up listener by id.
    void pollPeerSourceEvents();
    struct FeedSourceEventCtx { SonobusHubServer* hub; HubPeer* listener; };  // see deadlock note above

    int mSocket = -1;
    int mLocalPort = 0;
    int mSampleRate = HUB_DEFAULT_SAMPLERATE;
    int mBlockSize = HUB_DEFAULT_BLOCKSIZE;
    int mChannels = HUB_DEFAULT_CHANNELS;

    bool mUseOpus = true;
    int mOpusBitrate = 128000;
    int mOpusComplexity = 5;

    std::string mGroupName;
    std::string mGroupPassword;   
    std::string mHubUserName;  // Hub's own user name (e.g. "SonoBus-Hub"), sent to clients in /sb/hxhubhello at peer-join

    aoo::net::iserver::pointer mAooServer;
    aoo::net::iclient::pointer mClient;

    bool mRunLocalServer = false;
    int mLocalServerPort = 10999;
    std::unique_ptr<std::thread> mServerThread;

    std::string mPublicAddressHost;
    int mPublicAddressPort = 0;

    std::string mLanSubnetCidr;
    // Addresses advertised in LOGIN when
    // the hub reaches its built-in server over loopback (see resolveAdvertisedAddresses).
    std::string mAdvertisedLanHost;
    std::string mAdvertisedPublicHost;
    int mAdvertisedPublicPort = 0;
    void resolveAdvertisedAddresses(const std::string& aooServerHost);   // start() control path only

    // Per-peer sink buffer config + helpers.
    // sinkBufferMsFor() / addrMatchesLanSubnet() run only at handshake and
    // sink re-setup — zero audio-thread involvement.
    double mSinkBufferMs = HUB_SINK_BUFFERTIME_MS;
    double mSinkBufferLanMs = -1.0;
    // Per-player WAN hold constants + state (profile cache REMOVED).
    static constexpr double kHoldFloorMs = 4.0, kHoldCeilMs = 40.0, kHoldStartMeasureS = 75.0, kHoldJamPerMin = 2.0;   // 15 s of join transient skipped + one FULL 60-s window (a shorter one rounds the allowance to 0 gaps, so one outlier sets the hold)
    // The WAN setting is the START value, not the floor. A measured target below it commits from a full 60-s window;
    // stalls grow the hold after kHoldEmergencyLate new late blocks in one 10-s sample, at most every kHoldEmergencyGapMs.
    // SB_HUB_HOLD_FLOOR_SETTING=1 in the hub's environment uses the setting as a floor instead (A/B only, run-scoped).
    static constexpr uint32_t kHoldEmergencyLate = 5;
    static constexpr int64_t  kHoldEmergencyGapMs = 30000;
    // A gap longer than max(2 x hold, hold + kHoldStallExtraMs) is a STALL: concealed, never a reason to grow,
    // unless stalls themselves recur (> kHoldStallGrowPerMin over 2 min). After kHoldDecayWindows consecutive 10-s
    // samples with no moderate lateness, DECAY steps down (/1.33, not below the measured target).
    static constexpr double  kHoldStallExtraMs = 10.0;
    // GROW-STALL: a NEW stall in this 10-s sample grows the hold at once toward the measured gap, capped at
    // kHoldStallStepMs (4 blocks @ 32/48k = 2.67 ms) per sample; recurring stalls still use the 2-min rule. Mirrors the
    // client's measured-step emergency grow, so one long stall costs at most one step, never its full length.
    static constexpr double  kHoldStallStepMs = 2.67;
    static constexpr int64_t kHoldStallStepGapMs = 10000;
    static constexpr double  kHoldStallGrowPerMin = 2.0;
    // DECAY counts STALL-free 10-s samples (moderate lateness only feeds the target), 12 samples = 2 min,
    // one step per 2 min, never below the measured target.
    static constexpr int     kHoldDecayWindows = 12;
    static constexpr int64_t kHoldDecayGapMs = 120000;
    std::atomic<int>  mHoldMode { 0 };
    std::atomic<bool> mHoldResizeAll { false };
    std::string mDumpPeerName; int mDumpSecs = 0;
    FILE* mDumpFile = nullptr;
    std::atomic<int> mDumpFramesLeft{0};
    double sinkBufferMsFor(const HubPeer* p) const {
        return (p && p->isLan && mSinkBufferLanMs > 0.0) ? mSinkBufferLanMs : mSinkBufferMs;
    }
    bool addrMatchesLanSubnet(const struct sockaddr_storage& addr) const;


    // Shared_mutex so readers (audio
    // thread Phase A, networkRecvThread snap, sendThread sink/source polls,
    // stats writer, snapshotForUI) don't serialize against each other. Only
    // writers (addPeer / removePeer / handleSonobusControlMessage / sink
    // event format-change handler) need exclusive access.
    mutable std::shared_mutex mPeerMutex;
    // shared_ptr so the audio thread can snapshot a stable view of the peer
    // list under a brief lock and process AOO sinks/sources without holding
    // the mutex across the block. If a peer is removed mid-block, the audio
    // thread's snapshot keeps it alive until the next block.
    std::vector<std::shared_ptr<HubPeer>> mPeers;
    int32_t mNextPeerId = 1;

    // RCU-style immutable peer snapshot. Rebuilt by
    // rebuildPeerSnapshotLocked() — caller holds mPeerMutex EXCLUSIVE — at
    // every point mPeers membership or a peer's addr changes (addPeer both
    // paths, removePeer, PEER_LEAVE, self-heal addr drift, stop). Readers
    // (networkRecvThread per packet, audio thread Phase A) keep their own
    // std::shared_ptr<const PeerSnapshot> and refresh it only when
    // mPeerListGen moved: steady-state cost is ONE acquire load, zero
    // mallocs, zero refcount traffic. Lifetime: a reader's snapshot pins every
    // peer in it (strong refs) until the reader refreshes — the recv thread
    // re-checks the gen on every loop iteration (≤ one 500µs idle sleep or
    // one packet), the audio thread on every block — so a removed peer
    // outlives its removal by at most that long. No use-after-free is possible: every
    // HubPeer* handed out below is backed by a shared_ptr in the snapshot
    // the caller still holds.
    //
    // OWNERSHIP RULE for
    // PeerSnapshot objects and the HubPeers they pin:
    //   * A snapshot is IMMUTABLE once rebuildPeerSnapshotLocked() publishes
    //     it. It pins every peer it lists (strong refs in `peers`); the raw
    //     HubPeer* in `pacerPeers` are backed by those refs and are valid
    //     for exactly as long as the snapshot object is alive.
    //   * Non-RT readers (networkRecvThread) may hold a snapshot by
    //     shared_ptr. RT readers hold it by RAW pointer (pacer:
    //     mPacerSnapPtr) or copy peer refs out of it (audio thread:
    //     mCurrentSnap) — but an RT thread NEVER owns the last reference to
    //     a snapshot or to a peer, so ~PeerSnapshot / ~HubPeer (AOO
    //     uninvite/stop, endpoint delete, listener-join backstop, vector
    //     frees) can never run on a deadline thread.
    //   * Enforced by deferred destruction: when a new snapshot is
    //     published the previous one is moved to mRetiredSnaps (still owning
    //     its peers). Each reader publishes the gen of the snapshot it is
    //     using (mPacerSnapAck / mAudioSnapAck / mRecvSnapAck; kSnapAckNone
    //     = "holding nothing"). drainRetiredSnapshots() — statsWriterThread
    //     every 200 ms, the peer-removal control paths after their
    //     listener-thread join, and forced from stop() once every reader
    //     thread is joined — frees a retired snapshot only when EVERY
    //     reader's ack is past its gen. Every reader drops/replaces its old
    //     refs BEFORE acking the new gen, so a reader's drop is never the
    //     last one. The freeing thread is therefore always the stats thread
    //     or a control thread — never pacer / audio / recv.
    //   * Peer removal vs. the pacer: removePeer / PEER_LEAVE erase the
    //     peer, publish a snapshot without it, release mPeerMutex, join its
    //     listener thread, drop their own ref. The peer's endpoint
    //     (persistentEndpoint — the sendto target of every feed and of
    //     outBundle) dies in ~HubPeer, which cannot run until the retired
    //     snapshot that still lists the peer is drained, which needs
    //     mPacerSnapAck > that snapshot's gen — and the pacer only advances
    //     its ack at the START of a block, after every send of the previous
    //     block is done and it has switched to the newer snapshot. So the
    //     pacer can never send to a destroyed endpoint, and nothing ever
    //     blocks the pacer to guarantee it.
    //   * the SAME chain covers the
    //     per-listener connected fd (HubDest::connFd behind
    //     HubEndpoint::dest). The initial destination is set before the peer
    //     is published and the CURRENT one is closed only in ~HubEndpoint
    //     (from ~HubPeer), so any thread that can reach the endpoint — feeds
    //     via a pinned snapshot / own shared_ptr, the recv thread via rsnap,
    //     the recv thread's poll set (rebuilt from rsnap on every gen change
    //     BEFORE the previous rsnap could be freed) — holds a pin that keeps
    //     the fd open. No fd number can be recycled under an in-flight send.
    //   * a peer whose address
    //     moves (NAT rebind, new ephemeral port, Wi-Fi→Ethernet) gets a NEW
    //     HubDest swapped into HubEndpoint::dest under exclusive mPeerMutex,
    //     immediately followed by a snapshot rebuild (gen k → k+1). The old
    //     HubDest is retired with gen k (mRetiredDests). Any reader that has
    //     acked > k observed gen k+1 (seq_cst bump) — which is sequenced
    //     after the release store of the new pointer — so its every later
    //     acquire load of `dest` yields the new one; and every ack is taken
    //     at a point with no send in flight (pacer: start of block; audio:
    //     after the mCurrentSnap rewrite; recv: at the top of an iteration).
    //     A reader still on ≤ k may be mid-send on the old fd → the drain
    //     keeps it. Senders that hold mPeerMutex shared instead of acking
    //     (clientRunThread sink->send, re-invite, stats) are serialized
    //     against the exclusive-lock swap itself. The one sender doing
    //     neither — a cut-through listener thread emitting after its
    //     barrier — holds its own outFeedsLock for the whole pump→emit, so
    //     the drain takes+releases that lock once before closing (the
    //     listener's next acquire of the mutex then sees the swap). The
    //     rebuild after the swap is also what makes the recv thread re-poll
    //     the NEW connected fd — a swap without a rebuild would leave that
    //     peer's inbound audio unread.
    struct PeerSnapshot {
        uint64_t gen = 0;                               // == mPeerListGen value at publish
        std::vector<std::shared_ptr<HubPeer>> peers;    // non-null, mPeers order
        // IPv4 (sin_addr, sin_port) → index into `peers`; first match wins. Non-IPv4 peers are not indexed.
        std::unordered_map<uint64_t, int> byAddr4;
        // Pacer view: every ACTIVE peer, in mPeers order. HubPeer::active is only ever written (to true) under exclusive
        // mPeerMutex and every such write is followed by a rebuild, so filtering at publish time == filtering per block.
        // Raw pointers, backed by `peers` above (see ownership rule).
        std::vector<HubPeer*> pacerPeers;
    };
    static inline uint64_t addrKey4(const struct sockaddr_in* sa) {
        return ((uint64_t)sa->sin_addr.s_addr << 16) | (uint64_t)sa->sin_port;
    }
    std::shared_ptr<const PeerSnapshot> mPeerSnapshot = std::make_shared<const PeerSnapshot>();  // guarded by mPeerMutex
    std::atomic<uint64_t> mPeerListGen{0};
    void rebuildPeerSnapshotLocked();
    // The pacer's lock-free handle to the
    // current snapshot (points at the object owned by mPeerSnapshot; set in
    // the ctor and in rebuildPeerSnapshotLocked BEFORE the gen bump).
    std::atomic<const PeerSnapshot*> mPacerSnapPtr{nullptr};
    // Reader acknowledgements (see PeerSnapshot ownership rule). Each holds
    // the gen of the snapshot that reader currently uses, or kSnapAckNone.
    static constexpr uint64_t kSnapAckNone = ~0ULL;
    std::atomic<uint64_t> mPacerSnapAck{kSnapAckNone};
    std::atomic<uint64_t> mAudioSnapAck{kSnapAckNone};
    std::atomic<uint64_t> mRecvSnapAck{kSnapAckNone};
    // Retired snapshots awaiting all-readers-past-gen. Touched by control
    // paths + the stats thread only, never by an RT thread. Lock order:
    // mPeerMutex (exclusive, in rebuild) → mRetiredSnapMutex; the drain
    // never holds mPeerMutex and runs the destructors after unlocking.
    std::mutex mRetiredSnapMutex;
    std::vector<std::shared_ptr<const PeerSnapshot>> mRetiredSnaps;
    // Previous destinations of
    // re-addressed peers, awaiting the same all-readers-past-gen rule (plus
    // one outFeedsLock touch, see HubEndpoint). Same mutex, same drain.
    struct RetiredDest {
        uint64_t gen = 0;                 // snapshot gen current at the swap
        HubDest* dest = nullptr;          // the old destination (owns dest->connFd)
        bool heap = false;                // false = it is the endpoint's inline dest0: close only, never delete
        std::shared_ptr<HubPeer> peer;    // pins the endpoint + lets the drain take outFeedsLock
    };
    std::vector<RetiredDest> mRetiredDests;
    void drainRetiredSnapshots(bool force);
    // Re-point peer->persistentEndpoint at peer->addr (already updated by the
    // caller). Caller holds mPeerMutex EXCLUSIVE and MUST call
    // rebuildPeerSnapshotLocked() right after. No-op when unchanged.
    // How the endpoint is re-pointed.
    //   IfAddrChanged — the caller updated peer->addr; no-op when unchanged
    //                   (addPeer dedupe, NAT rebind, control-path refresh).
    //   ForceReopen   — same address, but the connected socket is dead
    //                   (EPIPE): NEW HubDest with a fresh connect()ed socket
    //                   (or sendto on the shared socket if connect() fails).
    //   ForceShared   — same address, connected sockets given up for this
    //                   peer: NEW HubDest with connFd = -1.
    enum class RebindMode { IfAddrChanged, ForceReopen, ForceShared };
    // `errOut` (optional) receives the errno of a failed socket()/connect()
    // when a connected socket was attempted (0 otherwise).
    void rebindPeerEndpointLocked(const std::shared_ptr<HubPeer>& peer, const char* why,
                                  RebindMode mode = RebindMode::IfAddrChanged, int* errOut = nullptr);
    // StatsWriterThread only, ~1 s
    // cadence under --connected-sockets. Two-phase: a peer whose connected
    // socket reports EPIPE goes to the shared socket AT ONCE (keeps working);
    // once the drain has closed the dead fd (connFdsPendingClose == 0) and
    // >= kConnReopenDelayMs passed, a fresh connect()ed socket is attempted,
    // up to kConnReopenMax attempts spaced >= kConnReopenDelayMs, retrying
    // only on collision-class errnos (EADDRINUSE...) — a policy errno or an
    // exhausted budget leaves the peer on the shared socket for good.
    void healEpipeSockets();
    static constexpr uint32_t kConnReopenMax = 3;
    static constexpr int64_t  kConnReopenDelayMs = 2000;
    uint32_t mSockEpipeReopens = 0;     // hub totals, statsWriterThread only
    uint32_t mSockSharedFallbacks = 0;

    // Stash for /sb/hxfeedreq messages whose `sender` was
    // not yet present in mPeers when received. addPeer() drains matching
    // entries when the sender finally arrives. Protects against rapid-bounce
    // ordering races where listener A's feedreq for sender B arrives before
    // hub has learned about B via AOO PEER_JOIN (which itself can be lost
    // after multiple bounces).
    struct PendingFeedReq {
        int32_t listenerPeerId = -1;
        std::string senderName;
        int32_t clientOurId = 0;
    };
    std::vector<PendingFeedReq> mPendingFeedReqs;
    // Deterministic feed source-id: derived from (listener, sender) usernames via stable hash, so the listener can
    // independently compute the same ID and oursink->invite_source it, avoiding a ~10 s startup delay waiting for AOO's
    // first format announcement (the source only sends format on add_sink or on explicit request, and an invite is
    // the explicit request). See feedSourceIdFor().
    static int32_t feedSourceIdFor(const std::string& listenerName,
                                   const std::string& senderName);

    // Buffer for invites that arrived BEFORE the peer-add event for that peer was processed. Race: a client sends an
    // INVITE over UDP very quickly after registering with the AOO server; the server's PEER_JOIN notification reaches
    // us via TCP and is processed in clientRunThread, which lags slightly. Unmatched invites queue here and replay when
    // each peer is added.
    struct PendingInvite {
        struct sockaddr_storage addr;
        socklen_t addrlen;
        int32_t id;       // sink_id from invite event
        int32_t flags;    // protocol flags from invite event
    };
    std::mutex mPendingInvitesMutex;
    std::vector<PendingInvite> mPendingInvites;

    std::atomic<bool> mRunning{false};
    std::atomic<bool> mConnected{false};
    std::atomic<bool> mGroupJoined{false};

    std::unique_ptr<std::thread> mAudioThread;
    std::unique_ptr<std::thread> mRecvThread;
    std::unique_ptr<std::thread> mClientRunThread;
    std::unique_ptr<std::thread> mClientEventThread;
    std::unique_ptr<std::thread> mSendThread;

    // ─── paced fan-out emission (--send-pace) ───────────
    // With inline sends, all listener threads emit at the fork-join barrier, so N×(N-1) feeds' packets burst onto the
    // wire every block boundary; hub send gap% climbs with each peer and receivers' auto jitter buffers latch high.
    // The pacer thread drains listener i's feeds at a FIXED sub-block offset i*(0.8*block/N): constant per-listener skew
    // (≤ one block, absorbed once as path delay), zero added variance. The audio thread only bumps a generation
    // post-barrier, so pacing is entirely off the audio deadline path.
    std::atomic<bool> mSendPacing{false};
    // ─── cut-through emission (--cut-through) ───────────
    // Latency-critical LAN alternative to pacing: each listener thread emits
    // its own feeds' datagrams (drain-all, packed into ONE bundle per block
    // via its HubPeer::outBundle) immediately after signalling its block
    // barrier — no pacer slot wait (paced mode adds 0..~0.8 period, mean
    // ~0.4 period ≈ 0.27 ms @32). Per-listener bundling is kept, so wire
    // shape per client is unchanged (1 datagram/block); across clients the
    // N datagrams cluster at the block boundary instead of spreading.
    // Exclusive with --send-pace: start() forces mSendPacing=false and does
    // NOT start the pacer thread (no idle RT claim, no ack in the snapshot
    // GC — mPacerSnapAck stays kSnapAckNone = "holds nothing").
    std::atomic<bool> mCutThrough{false};
    // ─── inline listeners (--inline-listeners) ──
    // Default OFF. When ON, the audio thread performs each listener's Step-2
    // work (outFeeds → source->process, and under --cut-through the bundle
    // emit too) sequentially, inline, instead of fork-joining N per-listener
    // Mach time-constraint threads. Motivation (hub + client on one Mac): every RT thread declares constraint = period, so XNU's EDF
    // degenerates to FIFO-by-wake and the co-resident client's recv thread —
    // causally LAST each block — is the one that slips. Inline mode removes N
    // RT threads (and N cv wakes + the barrier) from every block edge.
    //   mInlineListenersReq  requested by CLI; read once in start()
    //   mInlineActive        resolved mode. Audio thread reads it per block;
    //                        addPeer reads it under mPeerMutex (exclusive) to
    //                        decide whether to start a listener thread. Cleared
    //                        ONLY by inlineListenersFallback() (stats thread,
    //                        also under exclusive mPeerMutex) after it has
    //                        started listener threads for every current peer.
    //   mInlineFallbackReq   audio thread → stats thread: the measured budget
    //                        governor tripped (see audioProcessThread).
    // Governor (audio thread, report-only, no allocation): per block it
    // measures the worst per-listener inline time with mach_absolute_time;
    // every kInlineWindowBlocks it projects peers_max × per_listener_max and
    // compares with 60% of the audio thread's P/2 computation claim. Two
    // consecutive windows over budget → fallback request. The window values
    // are published in the atomics below and printed as [HUB-INLINE] by the
    // stats thread.
    std::atomic<bool> mInlineListenersReq{false};
    std::atomic<bool> mInlineActive{false};
    std::atomic<bool> mInlineFallbackReq{false};
    static constexpr uint32_t kInlineWindowBlocks = 512;
    std::atomic<uint64_t> mInlineMeasPerListenerMaxNs{0};
    std::atomic<uint64_t> mInlineMeasProjectedNs{0};
    std::atomic<uint64_t> mInlineMeasBudgetNs{0};
    std::atomic<int>      mInlineMeasPeersMax{0};
    std::atomic<uint64_t> mInlineMeasWindows{0};
    void inlineListenersFallback();   // stats thread only
    // Per-listener Step-2 work, shared by listenerProcessThread (threaded
    // mode) and the audio thread (inline mode). CALLER HOLDS self->outFeedsLock
    // for both; feed->lock is taken per feed inside. Returns the max
    // source->process(+send) ticks for the listener's feeds.
    uint64_t listenerPumpFeeds(HubPeer* self, int selfIdx, bool inlineSend);
    void listenerCutThroughEmit(HubPeer* self);
    // Evening: dither pacer slot phase (see pacerThread).
    std::atomic<bool> mPaceDither{true};
    // Fraction of the block period the pacer spreads listener sends over
    // (percent). With connected sockets a send costs ~7 µs, so a
    // tighter spread (e.g. 25) cuts the last listener's wait from ~0.45 ms to ~0.14 ms.
    std::atomic<int> mPaceSpreadPct{50};
    std::atomic<int> mPaceMinSlotUs{50};   // Absolute floor for the pacer slot (µs); see pacer loop
    uint64_t mPacerLcg = 0x9E3779B97F4A7C15ULL;   // pacer thread only
    std::unique_ptr<std::thread> mPacerThread;
    std::mutex mPacerMutex;
    std::condition_variable mPacerCv;
    uint64_t mPacerGen = 0;               // guarded by mPacerMutex
    void pacerThread();
    // The pacer is a Mach time-constraint thread, so a std::cout there would be a blocking write() on its deadline path.
    // It only bumps these relaxed atomics; statsWriterThread prints the [HUB-PACE] line every 10s and resets them.
    std::atomic<uint64_t> mPaceStatBlocks{0};
    // Audio-thread block timing
    std::atomic<uint64_t> mAudioGapNsMax{0};      // worst block-start to block-start gap in the window
    std::atomic<uint64_t> mAudioLateBlocks{0};    // blocks that started > 1.5 periods after the previous one
    std::atomic<uint64_t> mAudioWorkNsMax{0};     // worst audio-thread work time per block in the window
    std::atomic<uint64_t> mAudioSinkSkips{0};     // Blocks where a peer's sink was busy (resize) and was skipped instead of waited for
    std::atomic<uint64_t> mPaceStatOvershootNsMax{0};
    std::atomic<uint64_t> mPaceStatSlotsCollapsed{0};
    std::atomic<int>      mPaceLastPeers{0};
    std::atomic<uint64_t> mPaceLastSlotNs{0};
    // Bundle packing state + [HUB-PACE] window
    // counters (pacer folds each slot's builder counters in with one relaxed
    // fetch_add per counter; statsWriterThread prints + resets every 10 s).
    std::atomic<bool> mBundleEnabled{true};
    int mBundleMtu = HUB_BUNDLE_MTU_DEFAULT;   // set before start(); read at addPeer
    bool mDscpEnabled = false;                 // DSCP EF on the UDP socket — DEFAULT OFF (A/B: EF hurt the WAN tail); --dscp enables
    // Per-listener connected send
    // sockets. Plain bool: written before start(),
    // read-only afterwards. Creation lives in openConnectedSendSocket()
    // (control path only — addPeer and rebindPeerEndpointLocked, both under
    // exclusive mPeerMutex); teardown is ~HubEndpoint via the deferred-
    // destruction chain (~HubPeer), or drainRetiredSnapshots for a
    // destination replaced by an address change.
    bool mConnectedSockets = true;
    int openConnectedSendSocket(const struct sockaddr_storage& peerAddr, socklen_t peerAddrLen,
                                const std::string& userName, const HubDest* prev = nullptr,
                                int* errOut = nullptr);
    int mRedundancyLan = 1;                    // Copies per block to LAN listeners
    int mRedundancyWan = 2;                    //   and to WAN listeners (unproven; measure)
    std::atomic<uint64_t> mPaceStatDatagrams{0};
    std::atomic<uint64_t> mPaceStatMsgs{0};
    std::atomic<uint64_t> mPaceStatBundles{0};
    std::atomic<uint64_t> mPaceStatMsgsInBundles{0};

    std::vector<float> mMixBuffer;
    void* mClientEndpoint = nullptr;

    int64_t mHubStartedAtMs = 0;

    // Aggregate bandwidth counters. Updated
    // from networkRecvThread (in) and hub_endpoint_send_impl (out) via
    // atomic fetch_add. Read on the message thread inside snapshotForUI.
    // Cumulative since hub start; GUI diffs successive
    // snapshots to derive per-second rate.
    std::atomic<uint64_t> mBytesInTotal{0};
    std::atomic<uint64_t> mBytesOutTotal{0};

    // CPU sampling state. snapshotForUI() reads getrusage() and computes
    // % of one core using the delta from the previous snapshot. State is
    // mutable because snapshotForUI is const. Only the message thread
    // (JUCE timer) calls snapshotForUI so no locking is needed.
    mutable int64_t mLastCpuSampleMs = 0;
    mutable uint64_t mLastCpuUserUs = 0;
    mutable uint64_t mLastCpuSysUs = 0;
    mutable float mLastCpuPct = 0.0f;
    mutable std::mutex mCpuMutex;              // snapshotForUI runs on several threads (window, web page, internals)
    // Stage limits, kick blocks and the listing status line
    mutable std::mutex mStageMutex;
    StageSettings mStage;
    struct StageBlock { std::string ip, user; int64_t untilMs = 0; };
    std::vector<StageBlock> mStageBlocks;
    int mStageListingState = 0;
    std::string mStageListingText;
    std::atomic<uint64_t> mStageGen{0};
    std::atomic<uint64_t> mStageMembersGen{0};
    std::atomic<int> mStageMaxChannels{12};   // read by the format handler without the lock
    mutable int64_t mLastLatBroadcastMs = 0;
    mutable uint32_t mLatBroadcastCount = 0;   // /sb/hxlat 1 Hz throttle (snapshotForUI runs at 5 Hz)
public:
    // ONE definition of a player's to/from-hub latency, used by the
    // hub table and by the /sb/hxlat broadcast so hub and clients show identical numbers.
    //   toHub   = uplink one-way + the hub's inbound sink buffer for that player
    //   fromHub = hub->player one-way
    // One-way = RTT/2. If the sink-side (up) ping is unavailable the source-side RTT is
    // used for both directions (symmetric assumption, same as SonoBus's own display).
    // Returns false when there is no ping data at all yet.
    static bool computeHubLatency(float rttUpMs, float rttDownMs, double sinkBufMs, float& toHubMs, float& fromHubMs, float listenerBufMs = -1.0f) {
        // The sink-side (up) ping reads -1 on most LAN hosts and exactly 0.00 on the rest
        // (timetags not filled on that path): treat <= 0 as "no data", never as a real 0 ms.
        const bool haveUp = rttUpMs > 0.0f, haveDn = rttDownMs > 0.0f;
        if (!haveUp && !haveDn) return false;
        const float up = haveUp ? rttUpMs : rttDownMs;
        const float dn = haveDn ? rttDownMs : rttUpMs;
        toHubMs = up * 0.5f + (float) (sinkBufMs > 0.0 ? sinkBufMs : 0.0);
        fromHubMs = dn * 0.5f + (listenerBufMs > 0.0f ? listenerBufMs : 0.0f);   // + the player's own receive buffer when reported
        return true;
    }
private:
};
