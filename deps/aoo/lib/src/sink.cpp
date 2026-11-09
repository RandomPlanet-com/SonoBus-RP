/* Copyright (c) 2010-Now Christof Ressi, Winfried Ritsch and others. 
 * For information on usage and redistribution, and for a DISCLAIMER OF ALL
 * WARRANTIES, see the file, "LICENSE.txt," in this distribution.  */

#include "sink.hpp"
#include "aoo/aoo_utils.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// Verbose per-packet/per-tick diagnostic logs are gated behind AOO_HUB_DEBUG=1.
// Off by default; the high-signal events ([AOO-FORMAT] format-negotiation,
// [AOO-DISPATCH-D] first-N dispatch results) remain because they're rare and
// load-bearing for connection setup.
static const bool kSinkDebug = []() {
    const char* v = std::getenv("AOO_HUB_DEBUG");
    return v && *v && v[0] != '0';
}();

// Conf-gated sink EVENT log — every audible-

// The taps file lives in a persistent location so it survives a reboot, see SbDataPaths.h.
// /tmp/sb_taps.conf is the last fallback.
#include <cstdio>
#include <cstdlib>
#include <string>
#include "../../../../Source/SbDataPaths.h"
static inline const char* sb_taps_path() {
    static std::string p;
    if (p.empty()) {
        // SB_TAPS_FILE pins the taps path per process. The hub sets it in main() so hub sinks
        // never read the taps of a client running on the same host.
        if (const char* forced = std::getenv("SB_TAPS_FILE"); forced && forced[0]) { p = forced; return p.c_str(); }
        for (const std::string& cand : { sbTapsPath(), sbOldTapsPath() }) {
            if (FILE* f = std::fopen(cand.c_str(), "r")) { std::fclose(f); p = cand; break; }
        }
        if (p.empty()) p = "/tmp/sb_taps.conf";
    }
    return p.c_str();
}
static inline FILE* sb_taps_fopen() { return std::fopen(sb_taps_path(), "r"); }

// Every audible-event site (recovery, drop, fade splice, governor drain) appends a
// timestamped line to SB_SINK_EVLOG (env or taps file).
#ifdef _WIN32
// No <sys/time.h>/gettimeofday on MSVC — struct timeval comes
// from winsock2.h and the wall clock from std::chrono.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <chrono>
static inline int gettimeofday(struct timeval* tv, void*)
{
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    tv->tv_sec = (long)(us / 1000000); tv->tv_usec = (long)(us % 1000000); return 0;
}
#else
#include <sys/time.h>
#endif
// Producers (audio + network threads) push fixed records into a lock-free ring; ONE background
// writer thread drains it to the file. Nothing on the audio path touches the file system.
// Ring overflow drops records and reports the count as an "evlog_dropped" line.
#include <thread>
#include <chrono>
// The sink's elapsed_time only advances when the audio callback runs, so a bunched or stalled
// callback makes on-time packets look like 4-8 ms arrival gaps. The gap histogram (the LAN "need")
// therefore uses a monotonic wall clock for arrivals; SB_AOO_ADAPT_WALLCLOCK=0 selects the audio clock.
// Everything else in the tracker keeps the audio clock (consistent with the tick).
static double sb_wall_seconds(){
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
namespace {
struct SbEvRec { std::atomic<uint64_t> seq{0}; long sec; int msec; const char* tag; int32_t detail; int32_t sink; int32_t src; };
// Which stream an event belongs to. Set at the source_desc entry points (any thread);
// -1 = not inside a stream. Logged as s=<sink id>:<source id>; the hub prints "evlog s=<id>" per player.
thread_local int32_t g_sbEvSink = -1;
thread_local int32_t g_sbEvSrc = -1;
constexpr uint32_t kSbEvRing = 16384;   // power of two
SbEvRec g_sbEvRing[kSbEvRing];
std::atomic<uint64_t> g_sbEvHead{0};        // next index to claim (producers)
std::atomic<uint64_t> g_sbEvDropped{0};
std::atomic<int> g_sbEvState{0};            // 0 = unresolved, 1 = enabled, 2 = disabled
FILE* g_sbEvFile = nullptr;
std::string g_sbEvPath;   // Kept for rotation
// Rotate the evlog at 32 MB: <path>.1 keeps the previous slice, older is dropped. Writer thread only.
constexpr long kSbEvRotateBytes = 32L * 1024 * 1024;
void sb_evlog_rotate_if_needed(){
    if (!g_sbEvFile || g_sbEvPath.empty()) return;
    long pos = ftell(g_sbEvFile);
    if (pos < kSbEvRotateBytes) return;
    fclose(g_sbEvFile); g_sbEvFile = nullptr;
    const std::string old = g_sbEvPath + ".1";
    std::remove(old.c_str());
    std::rename(g_sbEvPath.c_str(), old.c_str());
    g_sbEvFile = fopen(g_sbEvPath.c_str(), "a");
    if (g_sbEvFile) { struct timeval tv; gettimeofday(&tv, nullptr); fprintf(g_sbEvFile, "%ld.%03d evlog_rotated %ld\n", (long)tv.tv_sec, (int)(tv.tv_usec/1000), pos); }
}
void sb_evlog_writer(){
    uint64_t next = 0;
    uint64_t lastDropReport = 0;
    for (;;){
        if (!g_sbEvFile) { std::this_thread::sleep_for(std::chrono::milliseconds(100)); continue; }
        bool wrote = false;
        for (int n = 0; n < 4096; ++n){
            SbEvRec& r = g_sbEvRing[next & (kSbEvRing - 1)];
            if (r.seq.load(std::memory_order_acquire) != next + 1) break;   // not yet published
            if (r.sink >= 0) fprintf(g_sbEvFile, "%ld.%03d %s %d s=%d:%d\n", r.sec, r.msec, r.tag, r.detail, r.sink, r.src);
            else             fprintf(g_sbEvFile, "%ld.%03d %s %d\n", r.sec, r.msec, r.tag, r.detail);
            ++next; wrote = true;
        }
        const uint64_t d = g_sbEvDropped.load(std::memory_order_relaxed);
        if (d != lastDropReport){
            struct timeval tv; gettimeofday(&tv, nullptr);
            fprintf(g_sbEvFile, "%ld.%03d evlog_dropped %lld\n", (long)tv.tv_sec, (int)(tv.tv_usec/1000), (long long)(d - lastDropReport));
            lastDropReport = d; wrote = true;
        }
        if (wrote) { fflush(g_sbEvFile); sb_evlog_rotate_if_needed(); }
        std::this_thread::sleep_for(std::chrono::milliseconds(wrote ? 20 : 100));
    }
}
} // namespace
static void sink_evlog(const char* tag, int32_t detail){
    int st = g_sbEvState.load(std::memory_order_acquire);
    if (st == 0){
        // First call per process (setup time, not steady-state audio): resolve the path,
        // open the file, start the writer.
        const char* p = getenv("SB_SINK_EVLOG");
        char pathbuf[256] = {0};
        if (!p){
            FILE* cf = sb_taps_fopen();
            if (cf){
                char line[256];
                while (fgets(line, sizeof(line), cf)){
                    if (!strncmp(line, "SB_SINK_EVLOG=", 14)){
                        strncpy(pathbuf, line + 14, sizeof(pathbuf) - 1);
                        char* nl = strchr(pathbuf, '\n'); if (nl) *nl = 0;
                        p = pathbuf;
                        break;
                    }
                }
                fclose(cf);
            }
        }
        g_sbEvFile = p && *p ? fopen(p, "a") : nullptr;
        if (g_sbEvFile) g_sbEvPath = p;   // For rotation
        int expected = 0;
        if (g_sbEvFile){
            if (g_sbEvState.compare_exchange_strong(expected, 1, std::memory_order_acq_rel)){
                std::thread(sb_evlog_writer).detach();
            }
        } else {
            g_sbEvState.compare_exchange_strong(expected, 2, std::memory_order_acq_rel);
        }
        st = g_sbEvState.load(std::memory_order_acquire);
    }
    if (st != 1) return;
    struct timeval tv; gettimeofday(&tv, nullptr);
    const uint64_t idx = g_sbEvHead.fetch_add(1, std::memory_order_acq_rel);
    SbEvRec& r = g_sbEvRing[idx & (kSbEvRing - 1)];
    // Overflow guard: the slot is still unread if its seq is not the previous lap's.
    // (seq == idx+1-kSbEvRing means the writer consumed it; 0 = never used.)
    const uint64_t have = r.seq.load(std::memory_order_acquire);
    if (idx >= kSbEvRing && have != idx + 1 - kSbEvRing){ g_sbEvDropped.fetch_add(1, std::memory_order_relaxed); return; }
    r.sec = (long)tv.tv_sec; r.msec = (int)(tv.tv_usec/1000); r.tag = tag; r.detail = detail; r.sink = g_sbEvSink; r.src = g_sbEvSrc;
    r.seq.store(idx + 1, std::memory_order_release);
}

// ONE reader for integer taps: env first, then the taps file (open()-launched GUI apps have
// no env). Returns 'def' when unset; *set (optional) reports whether a value was found. Call sites
// cache the result in function statics (read once per process); rebuild/setup time only, never
// on the audio thread, never on the per-packet path.
static int32_t sink_tap_int(const char* name, int32_t def, bool* set){
    if (set) *set = false;
    const char* e = getenv(name);
    if (e && *e){
        if (set) *set = true;
        return atoi(e);
    }
    const size_t n = strlen(name);
    FILE* cf = sb_taps_fopen();
    if (cf){
        char line[256];
        while (fgets(line, sizeof(line), cf)){
            if (!strncmp(line, name, n) && line[n] == '='){
                if (set) *set = true;
                def = atoi(line + n + 1);
                break;
            }
        }
        fclose(cf);
    }
    return def;
}

/*//////////////////// aoo_sink /////////////////////*/

aoo_sink * aoo_sink_new(int32_t id) {
    return new aoo::sink(id);
}

void aoo_sink_free(aoo_sink *sink) {
    // cast to correct type because base class
    // has no virtual destructor!
    delete static_cast<aoo::sink *>(sink);
}

int32_t aoo_sink_setup(aoo_sink *sink, int32_t samplerate,
                       int32_t blocksize, int32_t nchannels) {
    return sink->setup(samplerate, blocksize, nchannels);
}

int32_t aoo::sink::setup(int32_t samplerate,
                         int32_t blocksize, int32_t nchannels){
    if (samplerate > 0 && blocksize > 0 && nchannels > 0)
    {
        nchannels_ = nchannels;
        samplerate_ = samplerate;
        blocksize_ = blocksize;

        buffer_.resize(blocksize_ * nchannels_);

        // reset timer + time DLL filter
        timer_.setup(samplerate_, blocksize_);

        // don't need to lock
        update_sources();

        return 1;
    }
    return 0;
}

int32_t aoo_sink_invite_source(aoo_sink *sink, void *endpoint,
                              int32_t id, aoo_replyfn fn)
{
    return sink->invite_source(endpoint, id, fn);
}

int32_t aoo::sink::invite_source(void *endpoint, int32_t id, aoo_replyfn fn){
    // try to find existing source
    auto src = find_source(endpoint, id);
    if (!src){
        // discard data message, add source and request format!
        sources_.emplace_front(endpoint, fn, id, 0);
        src = &sources_.front();
        src->set_protocol_flags(protocol_flags_);
    }
    src->request_invite();

    return 1;
}

int32_t aoo_sink_uninvite_source(aoo_sink *sink, void *endpoint,
                                int32_t id, aoo_replyfn fn)
{
    return sink->uninvite_source(endpoint, id, fn);
}

int32_t aoo::sink::uninvite_source(void *endpoint, int32_t id, aoo_replyfn fn){
    // try to find existing source
    auto src = find_source(endpoint, id);
    if (src){
        src->request_uninvite();
        return 1;
    } else {
        return 0;
    }
}

int32_t aoo_sink_uninvite_all(aoo_sink *sink){
    return sink->uninvite_all();
}

int32_t aoo::sink::uninvite_all(){
    for (auto& src : sources_){
        src.request_uninvite();
    }
    return 1;
}

int32_t aoo::sink::request_source_codec_change(void *endpoint, int32_t id, aoo_format & f)
{
    auto src = find_source(endpoint, id);
    if (src){
        src->request_codec_change(f);
        return 1;
    } else {
        return 0;
    }
}

// See isink::request_source_format.
int32_t aoo::sink::request_source_format(void *endpoint, int32_t id)
{
    auto src = find_source(endpoint, id);
    if (src){
        src->request_format();
        return 1;
    } else {
        return 0;
    }
}


// Per-reason counters for every streamstate_.add_lost site,
// process-wide, read by the client's 30-s [SB-DROPS] log. 0 recovery_dropped  1 underrun_recovery  2 skim  3 overflow_flush(nqueued)
// 4 skipped_range  5 drop_unresent  6 outdated_pop. Relaxed atomics, no locks, audio-thread safe.
static std::atomic<uint64_t> g_sbLostSite[8];
extern "C" void aoo_sb_lost_sites(uint64_t* out, int n){ for (int k = 0; k < n && k < 8; ++k) out[k] = g_sbLostSite[k].load(std::memory_order_relaxed); }

namespace aoo {

template<typename T>
T& as(void *p){
    return *reinterpret_cast<T *>(p);
}

} // aoo

#define CHECKARG(type) assert(size == sizeof(type))

int32_t aoo_sink_set_option(aoo_sink *sink, int32_t opt, void *p, int32_t size)
{
    return sink->set_option(opt, p, size);
}

int32_t aoo::sink::set_option(int32_t opt, void *ptr, int32_t size)
{
    switch (opt){
    // id
    case aoo_opt_id:
    {
        CHECKARG(int32_t);
        auto newid = as<int32_t>(ptr);
        if (id_.exchange(newid) != newid){
            // LATER think of a way to safely clear source list
            update_sources();
            timer_.reset();
        }
        break;
    }
    // reset
    case aoo_opt_reset:
        update_sources();
        // reset time DLL
        timer_.reset();
        break;
    // buffer size
    case aoo_opt_buffersize:
    {
        CHECKARG(int32_t);
        auto bufsize = std::max<int32_t>(0, as<int32_t>(ptr));
        if (bufsize != buffersize_){
            // In LAN-isochronous mode (SB_AOO_ISO_BLOCKS>0) the
            // depth is fixed in blocks by do_update(); a ms change would only
            // trigger a queue rebuild (heap allocs under the writer lock +
            // silence re-prime) — the exact "rebuild storm" the ratchet causes.
            // Accept the FIRST value (so do_update runs once with a sane ms)
            // and ignore all later changes.
            static const int32_t iso_tap = sink_tap_int("SB_AOO_ISO_BLOCKS", 0, nullptr);
            // Same rule for the WAN adaptive sink (depth owned by adapt_retarget)
            if ((iso_tap > 0 || iso_blocks_.load(std::memory_order_relaxed) > 0
                 || adapt_.load(std::memory_order_relaxed) > 0) && buffersize_ > 0){
                break;
            }
            buffersize_ = bufsize;
            update_sources();
        }
        break;
    }
    // Per-sink LAN-isochronous depth (blocks). Idempotent;
    // a real change rebuilds the source queues ONCE.
    case aoo_opt_iso_blocks:
    {
        CHECKARG(int32_t);
        auto n = std::max<int32_t>(0, as<int32_t>(ptr));
        if (n != iso_blocks_.load(std::memory_order_relaxed)){
            iso_blocks_.store(n, std::memory_order_relaxed);
            update_sources();
        }
        break;
    }
    // Per-sink WAN adaptive depth (0/1). Idempotent; a real
    // change rebuilds the source queues ONCE (capacity sized for the max depth).
    case aoo_opt_adapt:
    {
        CHECKARG(int32_t);
        auto n = std::max<int32_t>(0, std::min<int32_t>(2, as<int32_t>(ptr)));   // 1 = WAN profile, 2 = LAN profile
        if (n != adapt_.load(std::memory_order_relaxed)){
            adapt_.store(n, std::memory_order_relaxed);
            update_sources();
        }
        break;
    }
    // dynamic resampling
    case aoo_opt_dynamic_resampling:
        CHECKARG(int32_t);
        dynamic_resampling_ = std::max<int32_t>(0, as<int32_t>(ptr));
        fprintf(stderr, "[AOO-DYNRES] sink id=%d dynamic_resampling <- %d\n", (int) id(), (int) dynamic_resampling_.load());   // Who sets it?
        break;
    // timefilter bandwidth
    case aoo_opt_timefilter_bandwidth:
        CHECKARG(float);
        bandwidth_ = std::max<double>(0, std::min<double>(1, as<float>(ptr)));
        timer_.reset(); // will update time DLL and reset timer
        break;
    // packetsize
    case aoo_opt_packetsize:
    {
        CHECKARG(int32_t);
        const int32_t minpacketsize = 64;
        auto packetsize = as<int32_t>(ptr);
        if (packetsize < minpacketsize){
            LOG_WARNING("packet size too small! setting to " << minpacketsize);
            packetsize_ = minpacketsize;
        } else if (packetsize > AOO_MAXPACKETSIZE){
            LOG_WARNING("packet size too large! setting to " << AOO_MAXPACKETSIZE);
            packetsize_ = AOO_MAXPACKETSIZE;
        } else {
            packetsize_ = packetsize;
        }
        break;
    }
    // resend limit
    case aoo_opt_resend_limit:
        CHECKARG(int32_t);
        resend_limit_ = std::max<int32_t>(0, as<int32_t>(ptr));
        break;
    // resend interval
    case aoo_opt_resend_interval:
        CHECKARG(int32_t);
        resend_interval_ = std::max<int32_t>(0, as<int32_t>(ptr)) * 0.001;
        break;
    // resend maxnumframes
    case aoo_opt_resend_maxnumframes:
        CHECKARG(int32_t);
        resend_maxnumframes_ = std::max<int32_t>(1, as<int32_t>(ptr));
        break;
    // buffer size
    case aoo_opt_protocol_flags:
        CHECKARG(int32_t);
        protocol_flags_ = as<int32_t>(ptr) & 0xff;
        break;
    // unknown
    default:
        LOG_WARNING("aoo_sink: unsupported option " << opt);
        return 0;
    }
    return 1;
}

int32_t aoo_sink_get_option(aoo_sink *sink, int32_t opt, void *p, int32_t size)
{
    return sink->get_option(opt, p, size);
}

int32_t aoo::sink::get_option(int32_t opt, void *ptr, int32_t size)
{
    switch (opt){
    // id
    case aoo_opt_id:
        as<int32_t>(ptr) = id();
        break;
    // buffer size
    case aoo_opt_buffersize:
        CHECKARG(int32_t);
        as<int32_t>(ptr) = buffersize_;
        break;
    // timefilter bandwidth
    case aoo_opt_timefilter_bandwidth:
        CHECKARG(float);
        as<float>(ptr) = bandwidth_;
        break;
    // resend packetsize
    case aoo_opt_packetsize:
        CHECKARG(int32_t);
        as<int32_t>(ptr) = packetsize_;
        break;
    // resend limit
    case aoo_opt_resend_limit:
        CHECKARG(int32_t);
        as<int32_t>(ptr) = resend_limit_;
        break;
    // resend interval
    case aoo_opt_resend_interval:
        CHECKARG(int32_t);
        as<int32_t>(ptr) = resend_interval_ * 1000;
        break;
    // resend maxnumframes
    case aoo_opt_resend_maxnumframes:
        CHECKARG(int32_t);
        as<int32_t>(ptr) = resend_maxnumframes_;
        break;
    case aoo_opt_protocol_flags:
        CHECKARG(int32_t);
        as<int32_t>(ptr) = protocol_flags_;
        break;
    // unknown
    default:
        LOG_WARNING("aoo_sink: unsupported option " << opt);
        return 0;
    }
    return 1;
}

int32_t aoo_sink_set_sourceoption(aoo_sink *sink, void *endpoint, int32_t id,
                              int32_t opt, void *p, int32_t size)
{
    return sink->set_sourceoption(endpoint, id, opt, p, size);
}

int32_t aoo::sink::set_sourceoption(void *endpoint, int32_t id,
                                   int32_t opt, void *ptr, int32_t size)
{
    auto src = find_source(endpoint, id);
    if (src){
        switch (opt){
        // reset
        case aoo_opt_reset:
            src->update(*this);
            break;
        // unsupported
        default:
            LOG_WARNING("aoo_sink: unsupported source option " << opt);
            return 0;
        }
        return 1;
    } else {
        return 0;
    }
}

int32_t aoo_sink_get_sourceoption(aoo_sink *sink, void *endpoint, int32_t id,
                              int32_t opt, void *p, int32_t size)
{
    return sink->get_sourceoption(endpoint, id, opt, p, size);
}

int32_t aoo::sink::get_sourceoption(void *endpoint, int32_t id,
                              int32_t opt, void *p, int32_t size)
{
    auto src = find_source(endpoint, id);
    if (src){
        switch (opt){
        // format
        case aoo_opt_format:
            CHECKARG(aoo_format_storage);
            return src->get_format(as<aoo_format_storage>(p));
        case aoo_opt_resampler_backlog:
            CHECKARG(float);
            return src->get_resampler_backlog(as<float>(p));
        case aoo_opt_buffer_fill_ratio:
            CHECKARG(float);
            return src->get_buffer_fill_ratio(as<float>(p));
        case aoo_opt_userformat:
            return src->get_userformat(static_cast<char*>(p), size);
        case aoo_opt_newest_seq:   // Sender's clock in blocks (read-only)
            CHECKARG(int32_t);
            as<int32_t>(p) = src->get_newest_seq();
            return 1;
        case aoo_opt_depth_blocks:   // Current receive depth target in blocks (read-only)
            CHECKARG(int32_t);
            as<int32_t>(p) = src->get_depth_blocks();
            return 1;
        case aoo_opt_rx_underruns:   // (read-only)
            CHECKARG(int32_t);
            as<int32_t>(p) = src->get_rx_underruns();
            return 1;
        case aoo_opt_protocol_flags:   // The flags this source announced in its format version
            CHECKARG(int32_t);
            as<int32_t>(p) = src->protocol_flags();
            return 1;
        case aoo_opt_rx_bytes:         // Wire bytes received on this stream (wraps at 2^32)
            CHECKARG(int32_t);
            as<int32_t>(p) = (int32_t)(uint32_t) src->rx_bytes_.load(std::memory_order_relaxed);
            return 1;
        case aoo_opt_adapt_events:   // Adaptive-depth decisions since `since`
            CHECKARG(aoo_adapt_events);
            src->adapt_events(as<aoo_adapt_events>(p));
            return 1;
        case aoo_opt_rx_pops:        // (read-only)
            CHECKARG(int32_t);
            as<int32_t>(p) = src->get_rx_pops();
            return 1;
        // unsupported
        default:
            LOG_WARNING("aoo_sink: unsupported source option " << opt);
            return 0;
        }
        return 1;
    } else {
        return 0;
    }
}

int32_t aoo_sink_handle_message(aoo_sink *sink, const char *data, int32_t n,
                                void *src, aoo_replyfn fn) {
    return sink->handle_message(data, n, src, fn);
}

int32_t aoo::sink::handle_message(const char *data, int32_t n,
                                  void *endpoint, aoo_replyfn fn) {
    try {
        osc::ReceivedPacket packet(data, n);
        osc::ReceivedMessage msg(packet);

        if (samplerate_ == 0){
            return 0; // not setup yet
        }

        int32_t type, sinkid;
        auto onset = aoo_parse_pattern(data, n, &type, &sinkid);
        if (!onset){
            LOG_WARNING("not an AoO message!");
            return 0;
        }
        
        if (type != AOO_TYPE_SINK){
            LOG_WARNING("not a sink message!");
            return 0;
        }
        if (sinkid == AOO_ID_NONE) {
            // special case, this is a be a compact data message
            // use the salt to see if it matches the current salt for us
            // using salt as unique token instead of dealing with a long OSC message and arguments
            auto it = msg.ArgumentsBegin();
            //auto id = (it++)->AsInt32();
            auto salt = (it++)->AsInt32();
            auto src = find_source_by_salt(endpoint, salt);
            if (src){
                static int cd_ok_logged = 0;
                if (cd_ok_logged < 5) {
                    fprintf(stderr, "[AOO-DISPATCH-D] OK ep=%p salt=0x%x (sample %d)\n",
                            endpoint, (unsigned)salt, ++cd_ok_logged);
                }
                return handle_compact_data_message(endpoint, fn, msg);
            }
            else {
                static int cd_drop_logged = 0;
                if (cd_drop_logged < 8) {
                    fprintf(stderr, "[AOO-DISPATCH-D] DROP ep=%p salt=0x%x (sources_=%d) (sample %d)\n",
                            endpoint, (unsigned)salt, (int)sources_.size(), ++cd_drop_logged);
                    for (auto& s : sources_) {
                        fprintf(stderr, "    existing src: ep=%p id=%d salt=0x%x\n",
                                s.endpoint(), s.id(), (unsigned)s.get_current_salt());
                    }
                }
                return 0;
            }
        }
        if (sinkid != id() && sinkid != AOO_ID_WILDCARD){
            LOG_WARNING("wrong sink ID!");
            return 0;
        }

        auto pattern = msg.AddressPattern() + onset;
        if (!strcmp(pattern, AOO_MSG_FORMAT)){
            return handle_format_message(endpoint, fn, msg);
        } else if (!strcmp(pattern, AOO_MSG_DATA)){
            return handle_data_message(endpoint, fn, msg);
        } else if (!strcmp(pattern, AOO_MSG_PING)){
            return handle_ping_message(endpoint, fn, msg);
        } else {
            LOG_WARNING("unknown message " << pattern);
        }
    } catch (const osc::Exception& e){
        LOG_ERROR("aoo_sink: exception in handle_message: " << e.what());
    }
    return 0;
}

int32_t aoo_sink_send(aoo_sink *sink){
    return sink->send();
}

int32_t aoo::sink::send(){
    bool didsomething = false;
    for (auto& s: sources_){
        if (s.send(*this)){
            didsomething = true;
        }
    }
    return didsomething;
}

int32_t aoo_sink_process(aoo_sink *sink, aoo_sample **data,
                         int32_t nsamples, uint64_t t) {
    return sink->process(data, nsamples, t);
}

#define AOO_MAXNUMEVENTS 256

int32_t aoo::sink::process(aoo_sample **data, int32_t nsampframes, uint64_t t){
    // we need to respect the nframes passed in here, which may be smaller than
    // the blocksize (the host may be splitting the processing, etc)
    std::fill(buffer_.begin(), buffer_.end(), 0);

    bool didsomething = false;

    // update time DLL filter
    // TODO deal with when we are called with less than the blocksize for this
    double error;
    auto state = timer_.update(t, error);
    
   
    if (state == timer::state::reset){
        LOG_DEBUG("setup time DLL filter for sink");
        dll_.setup(samplerate_, blocksize_, bandwidth_, 0);
    } else if (state == timer::state::error){
        // recover sources
        for (auto& s : sources_){
            s.request_recover();
        }
        timer_.reset();
    } else {
        auto elapsed = timer_.get_elapsed();
        dll_.update(elapsed);
    #if AOO_DEBUG_DLL
        DO_LOG("time elapsed: " << elapsed << ", period: " << dll_.period()
               << ", samplerate: " << dll_.samplerate());
    #endif
    }

    // if the DLL samplerate is any more than +/- 10% of our nominal, we'll ignore it
    // some shenanigans are going on
    bool ignoredll = !dynamic_resampling_.load();
    if (!ignoredll && fabs(dll_.samplerate() - ((double)samplerate_)) > 0.1*samplerate_) {
        ignoredll = true;
    }
    ignore_dll_ = ignoredll;
    
    // the mutex is uncontended most of the time, but LATER we might replace
    // this with a lockless and/or waitfree solution
    int alive = 0;
    int produced = 0;
    for (auto& src : sources_){
        if (src.process(*this, buffer_.data(), blocksize_, nsampframes)){
            didsomething = true;
            produced++;
        }
        alive++;
    }

    if (kSinkDebug) {
        static std::atomic<uint64_t> proc_n{0};
        uint64_t n = proc_n.fetch_add(1);
        if ((n & 0xFF) == 0) {  // every 256 process() calls (~0.7 sec at 375/s)
            float peak = 0.0f;
            for (auto v : buffer_) {
                float a = std::fabs(v);
                if (a > peak) peak = a;
            }
            std::cerr << "[SINK-PROCESS #" << n << "] sink=" << (void*)this
                      << " sources=" << alive
                      << " produced=" << produced
                      << " nsampframes=" << nsampframes
                      << " buffer_peak=" << peak
                      << " didsomething=" << didsomething << "\n";
            int idx = 0;
            for (auto& src : sources_) {
                std::cerr << "  [SD-STATE] idx=" << idx
                          << " sd=" << (void*)&src
                          << " id=" << src.id()
                          << " salt=0x" << std::hex << src.get_current_salt() << std::dec
                          << "\n";
                idx++;
            }
        }
    }

    if (didsomething){
    #if AOO_CLIP_OUTPUT
        for (auto it = buffer_.begin(); it != buffer_.end(); ++it){
            if (*it > 1.0){
                *it = 1.0;
            } else if (*it < -1.0){
                *it = -1.0;
            }
        }
    #endif
        // copy buffers
        for (int i = 0; i < nchannels_; ++i){
            auto buf = &buffer_[i * blocksize_];
            std::copy(buf, buf + nsampframes, data[i]);
        }
        return 1;
    } else {
        return 0;
    }
}
int32_t aoo_sink_events_available(aoo_sink *sink){
    return sink->events_available();
}

int32_t aoo::sink::events_available(){
    for (auto& src : sources_){
        if (src.has_events()){
            return true;
        }
    }
    return false;
}

int32_t aoo_sink_handle_events(aoo_sink *sink,
                              aoo_eventhandler fn, void *user){
    return sink->handle_events(fn, user);
}

#define EVENT_THROTTLE 1000

int32_t aoo::sink::handle_events(aoo_eventhandler fn, void *user){
    if (!fn){
        return 0;
    }
    int total = 0;
    // handle_events() and the source list itself are both lock-free!
    // NOTE: the source descs are never freed, so they are always valid
    for (auto& src : sources_){
        total += src.handle_events(fn, user);
        if (total > EVENT_THROTTLE){
            break;
        }
    }
    return total;
}

namespace aoo {

aoo::source_desc * sink::find_source(void *endpoint, int32_t id){
    for (auto& src : sources_){
        if ((src.endpoint() == endpoint) && (src.id() == id)){
            return &src;
        }
    }
    return nullptr;
}

aoo::source_desc * sink::find_source_by_salt(void *endpoint, int32_t salt){
    for (auto& src : sources_){
        if ((src.endpoint() == endpoint) && (src.get_current_salt() == salt)){
            return &src;
        }
    }
    return nullptr;
}

void sink::update_sources(){
    for (auto& src : sources_){
        src.update(*this);
    }
}

int32_t sink::handle_format_message(void *endpoint, aoo_replyfn fn,
                                    const osc::ReceivedMessage& msg)
{
    auto it = msg.ArgumentsBegin();

    int32_t id = (it++)->AsInt32();
    int32_t version = (it++)->AsInt32();

    // LATER handle this in the source_desc (e.g. ignoring further messages)
    if (!check_version(version)){
        LOG_ERROR("aoo_sink: source version not supported");
        return 0;
    }

    int32_t salt = (it++)->AsInt32();
    // get format from arguments
    aoo_format f;
    f.nchannels = (it++)->AsInt32();
    f.samplerate = (it++)->AsInt32();
    f.blocksize = (it++)->AsInt32();
    f.codec = (it++)->AsString();
    const void *settings;
    osc::osc_bundle_element_size_t size;
    (it++)->AsBlob(settings, size);

    const void *userfmt = nullptr;
    osc::osc_bundle_element_size_t ufsize = 0;

    if (msg.ArgumentCount() > 8) {
        (it++)->AsBlob(userfmt, ufsize);
    }

    if (id < 0){
        LOG_WARNING("bad ID for " << AOO_MSG_FORMAT << " message");
        return 0;
    }
    // try to find existing source
    auto src = find_source(endpoint, id);

    fprintf(stderr, "[AOO-FORMAT] ep=%p id=%d salt=0x%x ver=0x%x codec='%s' nch=%d sr=%d bs=%d settings_size=%d existing_src=%p\n",
            endpoint, id, (unsigned)salt, (unsigned)version,
            f.codec, f.nchannels, f.samplerate, f.blocksize, (int)size, (void*)src);

    if (!src){
        // not found - add new source
        sources_.emplace_front(endpoint, fn, id, salt);
        src = &sources_.front();
        src->set_protocol_flags(protocol_flags_);
        fprintf(stderr, "[AOO-FORMAT] ADDED new source_desc: ep=%p id=%d salt=0x%x (sources_ now=%d)\n",
                endpoint, id, (unsigned)salt, (int)sources_.size());
    }
    note_source_change();   // A new or re-announced source starts a churn window for every source in this sink

    return src->handle_format(*this, salt, f, (const char *)settings, size, version, (const char *) userfmt, ufsize);
}

int32_t sink::handle_data_message(void *endpoint, aoo_replyfn fn,
                                  const osc::ReceivedMessage& msg)
{
    auto it = msg.ArgumentsBegin();

    auto id = (it++)->AsInt32();
    auto salt = (it++)->AsInt32();
    aoo::data_packet d;
    d.sequence = (it++)->AsInt32();
    d.samplerate = (it++)->AsDouble();
    d.channel = (it++)->AsInt32();
    d.totalsize = (it++)->AsInt32();
    d.nframes = (it++)->AsInt32();
    d.framenum = (it++)->AsInt32();
    const void *blobdata;
    osc::osc_bundle_element_size_t blobsize;
    (it++)->AsBlob(blobdata, blobsize);
    d.data = (const char *)blobdata;
    d.size = blobsize;

    if (id < 0){
        LOG_WARNING("bad ID for " << AOO_MSG_DATA << " message");
        return 0;
    }
    // try to find existing source
    auto src = find_source(endpoint, id);
    if (src){
        return src->handle_data(*this, salt, d);
    } else {
        // discard data message, add source and request format!
        sources_.emplace_front(endpoint, fn, id, salt);
        src = &sources_.front();
        src->set_protocol_flags(protocol_flags_);
        src->request_format();
        return 0;
    }
}

int32_t sink::handle_compact_data_message(void *endpoint, aoo_replyfn fn,
                                          const osc::ReceivedMessage& msg)
{
    // /d <i:salt> <i:seq> <b:data>
    // /d <i:salt> <i:seq> <f:srate> <b:data>
    auto it = msg.ArgumentsBegin();

    aoo::data_packet d;

    auto salt = (it++)->AsInt32();
    d.sequence = (it++)->AsInt32();
    if (msg.ArgumentCount() == 4) {
        d.samplerate = (it++)->AsDouble();
    }
    else {
        d.samplerate = 0; // marker to use last
    }
    const void *blobdata;
    osc::osc_bundle_element_size_t blobsize;
    (it++)->AsBlob(blobdata, blobsize);
    // reconstruct the rest from prior format
    d.channel = 0 ;
    d.nframes = 1;
    d.framenum = 0;
    d.data = (const char *)blobdata;
    d.size = blobsize;
    d.totalsize = d.size;

    // try to find existing source by salt
    auto src = find_source_by_salt(endpoint, salt);
    if (src){
        if (kSinkDebug) {
            fprintf(stderr, "[AOO-COMPACT-DATA] OK sink=%p ep=%p salt=0x%x seq=%d size=%d srcs=%d\n",
                    (void*)this, endpoint, (unsigned)salt, d.sequence, d.size, (int)sources_.size());
        }
        return src->handle_data(*this, salt, d);
    } else {
        fprintf(stderr, "[AOO-COMPACT-DATA] DROP ep=%p salt=0x%x seq=%d (no matching source_desc; current sources_=%d)\n",
                endpoint, (unsigned)salt, d.sequence, (int)sources_.size());
        for (auto& s : sources_) {
            fprintf(stderr, "    existing src: ep=%p id=%d salt=0x%x\n",
                    s.endpoint(), s.id(), (unsigned)s.get_current_salt());
        }
        return 0;
    }
}

int32_t sink::handle_ping_message(void *endpoint, aoo_replyfn fn,
                                  const osc::ReceivedMessage& msg)
{
    auto it = msg.ArgumentsBegin();

    auto id = (it++)->AsInt32();
    time_tag tt = (it++)->AsTimeTag();

    if (id < 0){
        LOG_WARNING("bad ID for " << AOO_MSG_PING << " message");
        return 0;
    }
    // try to find existing source
    auto src = find_source(endpoint, id);
    if (src){
        return src->handle_ping(*this, tt);
    } else {
        LOG_WARNING("couldn't find source " << id << " for " << AOO_MSG_PING << " message");
        return 0;
    }
}

/*////////////////////////// source_desc /////////////////////////////*/

source_desc::source_desc(void *endpoint, aoo_replyfn fn, int32_t id, int32_t salt)
    : endpoint_(endpoint), fn_(fn), id_(id), salt_(salt)
{
    eventqueue_.resize(AOO_EVENTQUEUESIZE, 1);
    // push "add" event
    event e;
    e.ping.type = AOO_SOURCE_ADD_EVENT;
    e.ping.endpoint = endpoint;
    e.ping.id = id;
    eventqueue_.write(e); // no need to lock
    LOG_DEBUG("add new source with id " << id);
    resendqueue_.resize(256, 1);
}

int32_t source_desc::get_format(aoo_format_storage &format){
    // synchronize with handle_format() and update()!
    shared_lock lock(mutex_);
    if (decoder_){
        return decoder_->get_format(format);
    } else {
        return 0;
    }
}

void source_desc::request_codec_change(aoo_format & f)
{    
    auto c = aoo::find_codec(f.codec);
    if (c){
        char buf[256];
        int32_t optsize = c->serialize_format(f, buf, sizeof(buf));
        streamstate_.request_codec_change(f, buf, optsize);     
    } else {
        LOG_ERROR("codec '" << f.codec << "' not supported!");
        return;
    }
}

int32_t source_desc::get_resampler_backlog(float &samples){
    // Expose the resampler's held samples (per channel).
    samples = (float)resampler_.read_available() / (float)std::max(1, decoder_ ? decoder_->nchannels() : 1);
    return 1;
}

int32_t source_desc::get_buffer_fill_ratio(float &ratio){
    // Ratio against the target, not the physical capacity (capacity is target + blockqueue
    // headroom, see do_update). Clamped to 1.0 for every consumer (AutoFull / Smart Pin MEASURE
    // statistics, JitterBufferMeter, hub peak-fill sampler): depth beyond the target reads as 1.0.
    // Lock-free: one acquire load (read_available) + one relaxed load.
    const int32_t target = audioqueue_target_samples_.load(std::memory_order_relaxed);
    if (target > 0) {
        ratio = (audioqueue_.read_available() * audioqueue_.blocksize()) / (float)target;
        if (ratio > 1.0f) ratio = 1.0f;
    } else {
        ratio = 0.0f;
    }
    return 1;
}

int32_t source_desc::get_userformat(char *buf, int32_t size){
    shared_lock lock(mutex_);
    if (userformat_.empty()) return 0;

    if (size < userformat_.size()) {
        // return how much space is needed in buffer (negative)
        return -(int32_t)userformat_.size();
    }
    std::copy(userformat_.begin(), userformat_.end(), buf);
    return (int32_t)userformat_.size();
}


void source_desc::update(const sink &s){
    g_sbEvSink = s.id(); g_sbEvSrc = id_;   // Evlog stream identity
    // take writer lock!
    unique_lock lock(mutex_);
    do_update(s);
}

void source_desc::do_update(const sink &s){
    // Observability — every (re)configuration of this source's
    // sink-side state (buffersize change, format receipt) is a queue rebuild.
    sink_evlog("do_update", (int32_t) s.buffersize());   // ms
    s.note_source_change();   // A rebuild is churn, not lateness
    // resize audio ring buffer
    if (decoder_ && decoder_->blocksize() > 0 && decoder_->samplerate() > 0){
        // recalculate buffersize from ms to samples
        double bufsize = (double)s.buffersize() * decoder_->samplerate() * 0.001;
        bufsize = std::max(bufsize, (double)s.blocksize()); // needs to be at least one processing blocksize worth!
        auto d = div(bufsize, decoder_->blocksize());
        int32_t nbuffers = d.quot + (d.rem != 0); // round up
        nbuffers = std::max<int32_t>(1, nbuffers); // e.g. if buffersize_ is 0
        // Slack: blocks of committed depth above the ms pin. The pin (nbuffers, ceil(ms/block)) is what
        // the UI / Smart Pin asked for; slack is hidden depth that absorbs a late packet at the cost of
        // one block of latency each (0.67 ms @32smp, 1.33 ms @64smp). It is applied here, at the ms->blocks
        // conversion, so audioqueue/infoqueue capacity, the blockqueue and the governor target all see
        // target = pin + slack, and the governor never bleeds the slack away.
        //   SB_AOO_SLACK_BLOCKS=<n>  explicit slack (default 0)
        //   SB_AOO_GOV_V4=0          fixed-threshold governor
        // Read once per process (env, then taps file).
        {
            static int32_t v4_tap = -1;      // -1 = not read yet
            static int32_t slack_tap = -1;   // -1 = unset (auto), else explicit >= 0
            if (v4_tap < 0){
                bool set = false;
                v4_tap = sink_tap_int("SB_AOO_GOV_V4", 1, nullptr) != 0 ? 1 : 0;
                const int32_t sv = sink_tap_int("SB_AOO_SLACK_BLOCKS", -1, &set);
                slack_tap = set ? std::max<int32_t>(0, sv) : -1;
                fprintf(stderr, "aoo_sink: governor v4 = %d, slack blocks = %d (-1 = auto)\n",
                        v4_tap, slack_tap);
            }
            governor_v4_ = v4_tap != 0;
            governor_slack_blocks_ = slack_tap >= 0 ? slack_tap : 0;   // Default 0: the iso sink carries the margin
            nbuffers += governor_slack_blocks_;
        }
        // ─── LAN-ISOCHRONOUS depth (SB_AOO_ISO_BLOCKS=<n>) ───
        // The hub's pacer delivers one block per slot; on a clean LAN the
        // receive depth can be a FIXED small number of blocks. The ms API
        // (set_buffersize, int32 ms) cannot express sub-ms depths (2.67 and
        // 2.0 both truncate to 2 ms = 3 blocks @32smp) — so in iso mode the
        // depth is pinned HERE in blocks, slack is 0, and later ms changes
        // (the AutoFull/MCP ratchet) are ignored by the option handler, so
        // an underrun recovery conceals (PLC refill to target) and never grows.
        {
            static const int32_t iso_tap = sink_tap_int("SB_AOO_ISO_BLOCKS", 0, nullptr);
            // per-sink option (SonoBus sets it for LAN peers) wins over the global experiment tap
            iso_blocks_ = s.iso_blocks() > 0 ? s.iso_blocks() : std::max<int32_t>(0, iso_tap);
            if (iso_blocks_ > 0){
                governor_slack_blocks_ = 0;
                nbuffers = iso_blocks_;
                static bool logged = false;
                if (!logged){ logged = true; fprintf(stderr, "aoo_sink: LAN-ISOCHRONOUS depth = %d blocks (ms pin ignored)\n", iso_blocks_); }
            }
        }
        // ─── WAN ADAPTIVE depth (aoo_opt_adapt, or SB_AOO_ADAPT=1 global) ───
        // The depth is OWNED by adapt_retarget() from here on: initial depth =
        // SB_AOO_ADAPT_INIT_MS (default: the floor), clamped to [SB_AOO_ADAPT_FLOOR_MS (4),
        // SB_AOO_ADAPT_MAX_MS (100)]; slack 0; the ms pin is ignored (option
        // handler). Queues are sized ONCE for the max depth so a retarget never
        // rebuilds them (no do_update storms, no writer-lock allocations).
        {
            static const int32_t adapt_tap = sink_tap_int("SB_AOO_ADAPT", 0, nullptr);
            adapt_on_ = (s.adapt() > 0 || adapt_tap > 0) && iso_blocks_ <= 0;
            if (adapt_on_){
                const double block_ms = 1000.0 * decoder_->blocksize() / decoder_->samplerate();
                static const int32_t floor_ms = sink_tap_int("SB_AOO_ADAPT_FLOOR_MS", 4, nullptr);
                static const int32_t max_ms   = sink_tap_int("SB_AOO_ADAPT_MAX_MS", 100, nullptr);
                // A WAN stream starts at the floor (4 ms); the quick-jump below covers what the first seconds show.
                static const int32_t init_ms  = sink_tap_int("SB_AOO_ADAPT_INIT_MS", floor_ms, nullptr);
                // LAN profile: same controller, LAN numbers. Grows on repeated strikes up to 12 blocks (8 ms),
                // releases slowly.
                static const int32_t lan_floor_blocks = std::max<int32_t>(2, sink_tap_int("SB_AOO_ADAPT_LAN_FLOOR_BLOCKS", 2, nullptr));   // 2-block floor: at 1 block, hub inbound streams conceal constantly
                static const int32_t lan_max_blocks   = std::max<int32_t>(lan_floor_blocks + 1, sink_tap_int("SB_AOO_ADAPT_LAN_MAX_BLOCKS", 12, nullptr));
                adapt_lan_profile_ = (s.adapt() == 2);
                if (adapt_lan_profile_){
                    adapt_floor_blocks_ = lan_floor_blocks; adapt_max_blocks_ = lan_max_blocks;
                } else {
                    adapt_floor_blocks_ = std::max<int32_t>(2, (int32_t) std::ceil(floor_ms / block_ms));
                    adapt_max_blocks_   = std::max<int32_t>(adapt_floor_blocks_ + 1, (int32_t) std::ceil(max_ms / block_ms));
                }
                // Start a LAN stream at SB_AOO_ADAPT_LAN_WARM_BLOCKS (6 = 4 ms), not at the floor, so it does not
                // underrun on every stall until the stats catch up; the commit at 30 s trims to need.
                static const int32_t lan_warm_blocks = std::max<int32_t>(lan_floor_blocks, sink_tap_int("SB_AOO_ADAPT_LAN_WARM_BLOCKS", 6, nullptr));
                const int32_t init  = adapt_lan_profile_ ? lan_warm_blocks : (int32_t) std::ceil(init_ms / block_ms);
                nbuffers = std::min<int32_t>(std::max<int32_t>(init, adapt_floor_blocks_), adapt_max_blocks_);
                governor_slack_blocks_ = 0;
                adapt_headroom_blocks_ = 0; adapt_base_blocks_ = nbuffers;
                adapt_block_s_ = block_ms * 1e-3;
                adapt_t0_ = -1; adapt_win_start_ = -1; adapt_ewma_max_ = -1; adapt_ewma_sd_ = 0;
                adapt_last_retarget_ = -1; adapt_last_shrink_ = -1e9;
                adapt_underruns_sec_ = 0; adapt_strike_secs_ = 0; adapt_clean_secs_ = 0; adapt_shrink_pending_ = false; adapt_headroom_prev_ = 0; adapt_warm_ = false; adapt_warm_at_ = -1; adapt_committed_ = false; adapt_commit_blocks_ = 0; adapt_last_shadow_ = -1e9;
                memset(adapt_hist_, 0, sizeof(adapt_hist_)); adapt_hist_slice_ = 0; adapt_hist_filled_ = 0; adapt_hist_slice_t0_ = -1; adapt_last_arrival_ = -1; adapt_hist_blocks_ = 0; adapt_up_secs_ = 0; adapt_last_up_ = -1e9; adapt_emerg_late_ms_ = 0; adapt_emerg_win_grown_ = 0; memset(adapt_stall_ts_, 0, sizeof(adapt_stall_ts_)); adapt_stall_idx_ = 0; adapt_stall_n_ = 0; adapt_stalls_excluded_ = 0; adapt_stall_clean_secs_ = 0;
                static bool logged = false;
                if (!logged){ logged = true; fprintf(stderr, "aoo_sink: %s-ADAPTIVE depth: init %d floor %d max %d blocks (%.2f ms/block)\n", adapt_lan_profile_ ? "LAN" : "WAN", nbuffers, adapt_floor_blocks_, adapt_max_blocks_, block_ms); }
                sink_evlog("adapt_init", nbuffers);
            }
        }
        // ─── DRIFT ASRC + HQ resampler taps ───
        {
            static const int32_t drift_tap = sink_tap_int("SB_AOO_DRIFT_ASRC", 1, nullptr);
            static const int32_t hq_tap    = sink_tap_int("SB_AOO_RESAMP_HQ", 0, nullptr);
            drift_on_ = drift_tap > 0;
            { static const int32_t nd = sink_tap_int("SB_AOO_DRIFT_NO_DRAIN", 0, nullptr); drift_no_drain_ = nd > 0; }   // default 0: the governor owns the level, the ASRC owns the rate
            resampler_.set_hq(hq_tap > 0 || drift_on_);   // drift correction always uses the HQ path
            drift_last_t_ = -1; drift_ewma_bps_ = 0; drift_level_ewma_ = -1; drift_ppm_ = 0; drift_dirty_sec_ = 0;
            drift_ratio_.store(1.0, std::memory_order_relaxed);
            if (drift_on_ || hq_tap > 0){
                static bool logged = false;
                if (!logged){ logged = true; fprintf(stderr, "aoo_sink: DRIFT ASRC = %d, HQ resampler = %d\n", (int) drift_on_, (int) resampler_.hq()); aoo::dynamic_resampler::warm_sinc(); }
            }
        }
        // (audioqueue/infoqueue are sized below, once the blockqueue headroom
        // is known. Lead hand-off: capacity = target + bqcap.)
        auto nsamples = decoder_->nchannels() * decoder_->blocksize();
    #if 0
        // don't touch the event queue once constructed
        eventqueue_.reset();
    #endif
        // setup resampler
        resampler_.setup(decoder_->blocksize(), s.blocksize(),
                            decoder_->samplerate(), s.samplerate(), decoder_->nchannels());
        // resize block queue
        // Blocks accumulate in this queue under producer/consumer clock skew (the audioqueue stays full,
        // so the fill metric reads healthy) until it saturates at +extra blocks: hidden latency at every
        // sink crossing (+21.3 ms at 128smp/48k). Set by SB_AOO_BLOCKQ_EXTRA (env first, then the taps
        // file for open()-launched GUI apps), default 8.
        int32_t bq_extra = 8;
        {
            static int32_t cached = -1;
            if (cached < 0){
                const char* e = getenv("SB_AOO_BLOCKQ_EXTRA");
                if (e && *e){
                    cached = std::max(1, atoi(e));
                } else {
                    cached = 8;
                    FILE* cf = sb_taps_fopen();
                    if (cf){
                        char line[256];
                        while (fgets(line, sizeof(line), cf)){
                            if (!strncmp(line, "SB_AOO_BLOCKQ_EXTRA=", 20)){
                                cached = std::max(1, atoi(line + 20));
                                break;
                            }
                        }
                        fclose(cf);
                    }
                }
                fprintf(stderr, "aoo_sink: blockqueue extra = %d\n", cached);
            }
            bq_extra = cached;
        }
        // Target-scaled thresholds.
        //   T = nbuffers (pin + slack)         = governor target (blocks)
        //   H = max(2, ceil(T/2))              = arm band: arm when depth > T + H
        //   E = max(H + 1, bqcap - 2)          = emergency: completed lead >= E
        //   bq_extra >= H + 3                  = the hysteresis band always fits
        //                                        in the blockqueue (else the
        //                                        mass-clear overflow path would
        //                                        fire before the governor arms)
        // Fixed thresholds (SB_AOO_GOV_V4=0): H = 2, E = 5 (lead > 4), bq_extra untouched.
        governor_arm_blocks_ = governor_v4_ ? std::max<int32_t>(2, (nbuffers + 1) / 2) : 2;
        if (adapt_on_) governor_arm_blocks_ = 2;   // The adaptive sink owns its depth; the skew bleeder stays tight
        // SB_AOO_GOV_ARM_BLOCKS=<n> overrides the arm band (env/taps, read once per process).
        // Hub LAN sinks see little arrival jitter, so a 1-block band keeps their operating depth
        // at target instead of riding up to target+H.
        {
            static const int32_t armOverride = sink_tap_int("SB_AOO_GOV_ARM_BLOCKS", -1, nullptr);
            if (armOverride >= 1) governor_arm_blocks_ = std::min<int32_t>(armOverride, std::max<int32_t>(1, nbuffers));
        }
        if (governor_v4_){
            bq_extra = std::max<int32_t>(bq_extra, governor_arm_blocks_ + 3);
        }
        blockqueue_.resize((adapt_on_ ? adapt_max_blocks_ : nbuffers) + bq_extra);   // Adapt = sized once for the max depth
        governor_emerg_lead_ = governor_v4_
            ? std::max<int32_t>(governor_arm_blocks_ + 1, (adapt_on_ ? (int32_t) blockqueue_.capacity() : nbuffers + bq_extra) - 2)
            : 5;
        // Remember the committed target depth
        // in blocks for the depth governor's hysteresis band.
        governor_target_blocks_ = nbuffers;
        depth_pub_.store(nbuffers, std::memory_order_relaxed);   

        // ─── LEAD HAND-OFF: audioqueue capacity = target + bqcap ───
        // The target is decoupled from capacity. process_blocks() runs at completion time, but its
        // transfer loop is gated on audioqueue_.write_available(). With capacity = target, every completed
        // block beyond the target would park in the blockqueue, which only the network thread can touch, and
        // a network-thread scheduling gap >= T block periods would underrun with playable audio in hand.
        // So the audioqueue (an SPSC ring the audio thread already reads lock-free) gets target + blockqueue
        // capacity blocks: process_blocks hands off every completed consecutive block the moment it
        // completes, and the blockqueue holds only incomplete blocks and blocks behind a hole. Target
        // semantics (prefill, recovery refill, fill ratio, governor thresholds) are expressed against
        // governor_target_blocks_ / audioqueue_target_samples_, not capacity.
        // Ownership: network thread writes, audio thread reads.
        // SB_AOO_HANDOFF=0 sizes the audioqueue to the target only. Default on.
        static const int32_t handoffOn = sink_tap_int("SB_AOO_HANDOFF", 1, nullptr);
        const int32_t aq_blocks = (adapt_on_ ? adapt_max_blocks_ : nbuffers) + (handoffOn ? (int32_t) blockqueue_.capacity() : 0);   // Adapt = max depth + bqcap
        audioqueue_.resize(aq_blocks * nsamples, nsamples);
        infoqueue_.resize(aq_blocks, 1);
        audioqueue_target_samples_.store(nbuffers * nsamples, std::memory_order_relaxed);
        // initial prefill: exactly the target depth in silence, not to capacity
        int count = 0;
        while (count < nbuffers && audioqueue_.write_available() && infoqueue_.write_available()){
            audioqueue_.write_commit();
            // push nominal samplerate + default channel (0)
            block_info i;
            i.sr = decoder_->samplerate();
            i.channel = 0;
            infoqueue_.write(i);
            count++;
        }
        LOG_VERBOSE("reset source queues to " << nbuffers << " buffers (audioqueue capacity "
                    << aq_blocks << " blocks)");
        sink_evlog("aq_capacity", aq_blocks);
        governor_armed_ = false;
        governor_armed_since_ = 0;
        governor_last_drain_time_ = -1e9;
        governor_last_emerg_time_ = -1e9;
        governor_v4_logged_ = false;
        newest_ = 0;
        next_ = -1;
        nextneedsfadein_ = 0;
        channel_ = 0;
        samplerate_ = decoder_->samplerate();
        streamstate_.reset();
        ack_list_.set_limit(s.resend_limit());
        ack_list_.clear();

        // start in a need recovery state so the buffer is re-filled when we get the first data
        streamstate_.request_recover();

        LOG_DEBUG("update source " << id_ << ": sr = " << decoder_->samplerate()
                    << ", blocksize = " << decoder_->blocksize() << ", nchannels = "
                    << decoder_->nchannels() << ", bufsize = " << nbuffers * nsamples);
    }
}

// /aoo/sink/<id>/format <src> <salt> <numchannels> <samplerate> <blocksize> <codec> <settings...>


// [mask][present channels' samples] → full interleaved PCM bytes (zeros for the
// absent channels) → the untouched codec decodes a normal block, so its PLC history stays consistent.
int32_t source_desc::decode_masked(const char *data, int32_t size, aoo_sample *out, int32_t nsamples){
    const int32_t nch = decoder_->nchannels();
    const int32_t frames = nch > 0 ? nsamples / nch : 0;
    const int32_t maskBytes = (nch + 7) / 8;
    if (nch <= 0 || frames <= 0 || size < maskBytes) return -1;
    const uint8_t* mask = (const uint8_t*) data;
    int32_t live = 0;
    for (int32_t c = 0; c < nch; ++c) if (mask[c >> 3] & (1u << (c & 7))) ++live;
    if (live == nch) return decoder_->decode(data + maskBytes, size - maskBytes, out, nsamples);
    masked_blocks_rx_.fetch_add(1, std::memory_order_relaxed);
    int32_t ss = mask_samplesize_;
    if (live > 0){
        const int32_t payload = size - maskBytes;
        if (payload <= 0 || payload % (live * frames) != 0) return -1;
        ss = payload / (live * frames);
        mask_samplesize_ = ss;
    }
    const size_t full = (size_t) nsamples * (size_t) ss;
    if (maskexp_.size() < full) maskexp_.resize(full);
    char* dst = maskexp_.data();
    const char* src = data + maskBytes;
    for (int32_t j = 0; j < frames; ++j){
        for (int32_t c = 0; c < nch; ++c){
            if (mask[c >> 3] & (1u << (c & 7))) { std::memcpy(dst, src, (size_t) ss); src += ss; }
            else std::memset(dst, 0, (size_t) ss);
            dst += ss;
        }
    }
    return decoder_->decode(maskexp_.data(), (int32_t) full, out, nsamples);
}

int32_t source_desc::handle_format(const sink& s, int32_t salt, const aoo_format& f,
                                   const char *settings, int32_t size, int32_t version,
                                   const char *userformat, int32_t ufsize){
    g_sbEvSink = s.id(); g_sbEvSrc = id_;   // Evlog stream identity
    sink_evlog("format_recv", salt);   // Observability
    // take writer lock!
    unique_lock lock(mutex_);

    salt_ = salt;

    // create/change decoder if needed
    if (!decoder_ || strcmp(decoder_->name(), f.codec)){
        auto c = aoo::find_codec(f.codec);
        if (c){
            decoder_ = c->create_decoder();
        } else {
            LOG_ERROR("codec '" << f.codec << "' not supported!");
            return 0;
        }
        if (!decoder_){
            LOG_ERROR("couldn't create decoder!");
            return 0;
        }
    }

    // see what protocol flags are in the LSB of the version
    protocol_flags_ = (0xFF & version);
    // The source only learns our capabilities from a format REQUEST (or an invite the
    // app forwards). When we can decode masked blocks but this stream is not masked, ask once per salt: a new source
    // re-salts with masking on; an old one re-sends the same salt and we stay quiet.
    if ((s.protocol_flags() & AOO_PROTOCOL_FLAG_IDLE_MASK) && !(protocol_flags_ & AOO_PROTOCOL_FLAG_IDLE_MASK)
        && !(mask_req_done_ && mask_req_salt_ == salt)) {
        mask_req_done_ = true; mask_req_salt_ = salt;
        streamstate_.request_format();
    }
    
    // read format
    decoder_->read_format(f, settings, size);

    // user format
    if (userformat) {
        userformat_.assign(userformat, userformat+ufsize);
    }

    do_update(s);

    // push event
    event e;
    e.type = AOO_SOURCE_FORMAT_EVENT;
    e.source.endpoint = endpoint_;
    e.source.id = id_;
    push_event(e);

    return 1;
}

// /aoo/sink/<id>/data <src> <salt> <seq> <sr> <channel_onset> <totalsize> <numpackets> <packetnum> <data>

int32_t source_desc::handle_data(const sink& s, int32_t salt, const aoo::data_packet& d){
    g_sbEvSink = s.id(); g_sbEvSrc = id_;   // Evlog stream identity
    // synchronize with update()!
    shared_lock lock(mutex_);

    rx_bytes_.fetch_add((uint64_t) d.size + 52u, std::memory_order_relaxed);   // Kbps readout (payload + headers)
    // the source format might have changed and we haven't noticed,
    // e.g. because of dropped UDP packets.
    if (salt != salt_){
        streamstate_.request_format();
        return 0;
    }

#if 1
    if (!decoder_){
        LOG_DEBUG("ignore data message");
        return 0;
    }
#else
    assert(decoder_ != nullptr);
#endif
    LOG_DEBUG("got block: seq = " << d.sequence << ", sr = " << d.samplerate
              << ", chn = " << d.channel << ", totalsize = " << d.totalsize
              << ", nframes = " << d.nframes << ", frame = " << d.framenum << ", size " << d.size);

    if (next_ < 0){
        next_ = d.sequence;
        nextneedsfadein_ = next_;
    }

    // WAN adaptive depth — arrival tracker (fresh = first time we see a sequence)
    const bool adapt_fresh = adapt_on_ && d.sequence > newest_;

    // check data packet
    if (!check_packet(d)){
        return 0;
    }

    if (adapt_on_) adapt_track(s, d, adapt_fresh);

    // add data packet
    if (!add_packet(d)){
        return 0;
    }

    // process blocks and send audio
    process_blocks(s);

#if 1
    check_outdated_blocks();
#endif

    // check and resend missing blocks
    check_missing_blocks(s);

#if AOO_DEBUG_BLOCK_BUFFER
    DO_LOG("block buffer: size = " << blockqueue_.size() << " / " << blockqueue_.capacity());
#endif

    return 1;
}

// /aoo/sink/<id>/ping <src> <time>

int32_t source_desc::handle_ping(const sink &s, time_tag tt){
#if 1
    if (streamstate_.get_state() != AOO_SOURCE_STATE_PLAY){
        return 0;
    }
#endif

#if 0
    time_tag tt2 = s.absolute_time(); // use last stream time
#else
    time_tag tt2 = aoo_osctime_get(); // use real system time
#endif

    streamstate_.set_ping(tt, tt2);

    // push "ping" event
    event e;
    e.type = AOO_PING_EVENT;
    e.ping.endpoint = endpoint_;
    e.ping.id = id_;
    e.ping.tt1 = tt.to_uint64();
    e.ping.tt2 = tt2.to_uint64();
    e.ping.tt3 = 0;
    push_event(e);

    return 1;
}

bool source_desc::send(const sink& s){
    bool didsomething = false;

    if (send_format_request(s)){
        didsomething = true;
    }
    if (send_codec_change_request(s)){
        didsomething = true;
    }
    if (send_data_request(s)){
        didsomething = true;
    }
    if (send_notifications(s)){
        didsomething = true;
    }
    return didsomething;
}

bool source_desc::process(const sink& s, aoo_sample *buffer, int32_t stride, int32_t numsampleframes){
    g_sbEvSink = s.id(); g_sbEvSrc = id_;   // Evlog stream identity
    // synchronize with handle_format() and update()!
    // the mutex should be uncontended most of the time.
    // NOTE: We could use try_lock() and skip the block if we couldn't aquire the lock.
    shared_lock lock(mutex_);
    drift_frames_entry_ += numsampleframes;   // The sink's callback clock, regardless of data availability

    if (kSinkDebug) {
        static std::atomic<uint64_t> sd_n{0};
        uint64_t cnt = sd_n.fetch_add(1);
        bool live = (decoder_ != nullptr);
        bool log_it = false;
        if (live && (cnt % 256) == 0) log_it = true;
        if (!live) {
            static std::atomic<uint64_t> seen_bits{0};
            uint64_t bit = ((uintptr_t)this >> 4) & 63;
            uint64_t mask = (1ULL << bit);
            if ((seen_bits.fetch_or(mask) & mask) == 0) log_it = true;
        }
        if (log_it) {
            std::cerr << "[SD-PROCESS #" << cnt << "] sd=" << (void*)this
                      << " salt=0x" << std::hex << salt_ << std::dec
                      << " decoder=" << (live ? "Y" : "N")
                      << " bq_size=" << blockqueue_.size()
                      << " aq_ravail=" << audioqueue_.read_available()
                      << " rs_ravail=" << resampler_.read_available()
                      << " rs_wavail=" << resampler_.write_available()
                      << " next=" << next_
                      << " newest=" << newest_
                      << "\n";
        }
    }

    if (!decoder_){
        return false;
    }

    // record stream state
    int32_t lost = streamstate_.get_lost();
    int32_t reordered = streamstate_.get_reordered();
    int32_t resent = streamstate_.get_resent();
    int32_t gap = streamstate_.get_gap();

    event e;
    e.source.endpoint = endpoint_;
    e.source.id = id_;
    if (lost > 0){
        // push packet loss event
        e.type = AOO_BLOCK_LOST_EVENT;
        e.block_loss.count = lost;
        push_event(e);
    }
    if (reordered > 0){
        // push packet reorder event
        e.type = AOO_BLOCK_REORDERED_EVENT;
        e.block_reorder.count = reordered;
        push_event(e);
    }
    if (resent > 0){
        // push packet resend event
        e.type = AOO_BLOCK_RESENT_EVENT;
        e.block_resend.count = resent;
        push_event(e);
    }
    if (gap > 0){
        // push packet gap event
        e.type = AOO_BLOCK_GAP_EVENT;
        e.block_gap.count = gap;
        push_event(e);
    }

    // don't process anything until the first few blocks are recv'd into the blockqueue
    // after a reset to keep the jitter buffer as full as possible at the start
    //if (streamstate_.get_blocks_recvd() <  std::min(infoqueue_.capacity()/2, 10)) {
    //    LOG_VERBOSE("waiting for some blocks: " << streamstate_.get_blocks_recvd());
    //    return false;
    //}
    
    int32_t nsamples = audioqueue_.blocksize();
    trace_calls_.fetch_add(1, std::memory_order_relaxed);   // Trace

    // read samples from resampler
    auto nchannels = decoder_->nchannels();
    // we need to respect the sample frame size passed in this method
    // because it may be less than the sink blocksize
    auto readsamples = numsampleframes * nchannels;

#if 0
    auto capacity = audioqueue_.capacity() / audioqueue_.blocksize();
    DO_LOG("audioqueue: " << audioqueue_.read_available() << " / " << capacity);
#endif

    while (audioqueue_.read_available() && infoqueue_.read_available()
           && readsamples > resampler_.read_available() && resampler_.write_available() >= nsamples){

        // get block info and set current channel + samplerate
        block_info info;
        infoqueue_.read(info);
        channel_ = info.channel;
        samplerate_ = info.sr;

        // Sources default
        // dynamic_resampling=1 and stamp packets with their drifting DLL
        // rate estimate; ratio = ours/theirs wanders off 1.0 and the
        // resampler linear-interpolates the whole stream even when OUR
        // dynamic resampling is off — grit that grows with stream age.
        // Snap near-nominal rates (<0.1%, crystal tolerance) to the
        // decoder's nominal so the ratio is exactly 1.0 (bit-exact path).
        if (decoder_){
            const double nom = (double)decoder_->samplerate();
            if (nom > 0 && samplerate_ > 0 && samplerate_ != nom
                && std::fabs(samplerate_ - nom) < nom * 0.001){
                static std::atomic<uint32_t> srlogcnt{0};
                if ((srlogcnt.fetch_add(1, std::memory_order_relaxed) % 4096u) == 0u){
                    fprintf(stderr, "[AOO-SR] raw=%.6f nominal=%.0f (snapped)\n",
                            samplerate_, nom);
                }
                samplerate_ = nom;
            }
        }

        // write audio into resampler
        resampler_.write(audioqueue_.read_data(), nsamples);

        audioqueue_.read_commit();
        trace_moves_.fetch_add(1, std::memory_order_relaxed);   // Trace


    }
    // update resampler
    // Drift ASRC — the network thread's ratio scales the SOURCE rate (source faster
    // than us -> consume source samples faster -> ratio_ < 1 in the resampler's terms).
    {
        const double dr = drift_on_ ? drift_ratio_.load(std::memory_order_relaxed) : 1.0;
        resampler_.update(samplerate_ * dr, s.real_samplerate());
    }
    // read samples from resampler
    
    //LOG_VERBOSE("s.blocksize: " << s.blocksize() << "  size: " << numsampleframes << "  stride: " << stride << " readsamp: " << readsamples << " ravail: " << resampler_.read_available() << " wavail: " << resampler_.write_available());
    
    if (resampler_.read_available() >= readsamples){
        auto buf = (aoo_sample *)alloca(readsamples * sizeof(aoo_sample));
        resampler_.read(buf, readsamples);
        trace_reads_.fetch_add(1, std::memory_order_relaxed);   // Trace
        drift_frames_ += numsampleframes;   // Counted only when frames are really delivered, not on early returns

        // sum source into sink (interleaved -> non-interleaved),
        // starting at the desired sink channel offset.
        // out of bound source channels are silently ignored.
        for (int i = 0; i < nchannels; ++i){
            auto chn = i + channel_;
            // ignore out-of-bound source channels!
            if (chn < s.nchannels()){
                auto n = numsampleframes;
                auto out = buffer + stride * chn;
                for (int j = 0; j < n; ++j){
                    out[j] += buf[j * nchannels + i];
                }
            }
        }

        // LOG_DEBUG("read samples from source " << id_);

        if (streamstate_.update_state(AOO_SOURCE_STATE_PLAY)){
            // push "start" event
            event e;
            e.type = AOO_SOURCE_STATE_EVENT;
            e.source_state.endpoint = endpoint_;
            e.source_state.id = id_;
            e.source_state.state = AOO_SOURCE_STATE_PLAY;
            push_event(e);
        }

        return true;
    } else {
        // buffer ran out -> push "stop" event
        if (streamstate_.update_state(AOO_SOURCE_STATE_STOP)){
            event e;
            e.type = AOO_SOURCE_STATE_EVENT;
            e.source_state.endpoint = endpoint_;
            e.source_state.id = id_;
            e.source_state.state = AOO_SOURCE_STATE_STOP;
            push_event(e);

            LOG_VERBOSE("UNDERRUN resampler avail " << resampler_.read_available() << "  readsamp: " << readsamples);
            trace_dry_.fetch_add(1, std::memory_order_relaxed);   // Trace (state-change only)

            // this doesn't do anything if the stream simply stopped
            streamstate_.set_underrun();
        }

        return false;
    }
}

int32_t source_desc::handle_events(aoo_eventhandler fn, void *user){
    // copy events - always lockfree! (the eventqueue is never resized)
    auto n = eventqueue_.read_available();
    if (n > 0){
        auto events = (event *)alloca(sizeof(event) * n);
        for (int i = 0; i < n; ++i){
            eventqueue_.read(events[i]);
        }
        auto vec = (const aoo_event **)alloca(sizeof(aoo_event *) * n);
        for (int i = 0; i < n; ++i){
            vec[i] = (aoo_event *)&events[i];
        }
        fn(user, vec, n);
    }
    return n;
}

// Network thread only. Writes silence blocks into the audioqueue until it holds
// (target - headroom) blocks.
int32_t source_desc::refill_silence_to_target(int32_t headroom_blocks){
    // Rate-limit the per-slot decode-failed warning (first 30, then every 200th, like
    // [SINK-RECOVERY]). A catastrophic-loss recovery iterates every slot of the jitter buffer, and
    // unconditional stderr writes would slow the hub thread enough to cause more underruns.
    static std::atomic<int> decfail_log_n{0};
    const int32_t want = std::max<int32_t>(0, governor_target_blocks_ - headroom_blocks);
    const auto nsamples = audioqueue_.blocksize();
    int32_t count = 0;
    while ((int32_t) audioqueue_.read_available() < want
           && audioqueue_.write_available() && infoqueue_.write_available()){
        auto ptr = audioqueue_.write_data();
        if (!decoder_->decode(nullptr, 0, ptr, nsamples)) {
            int n = decfail_log_n.fetch_add(1);
            if (n < 30 || (n % 200) == 0) {
                LOG_WARNING("decode failed nsamples: " << nsamples << " audioqavail: " << audioqueue_.write_available() << " (#" << n << ")");
            }
        }
        audioqueue_.write_commit();
        // push nominal samplerate + current channel
        block_info i;
        i.sr = decoder_->samplerate();
        i.channel = channel_;
        infoqueue_.write(i);
        count++;
    }
    return count;
}

// Network thread only. Skip n sequence
// numbers starting at next_: whatever of them is physically queued is popped
// (ack entries released), the rest are discarded by check_packet ("discarded
// old block") when they arrive — including any redundancy duplicates and
// resends. The first block after the skipped range gets the fade-in splice.
void source_desc::skip_ahead(int32_t n){
    if (n <= 0) return;
    next_ += n;
    while (!blockqueue_.empty() && blockqueue_.front().sequence < next_){
        ack_list_.remove(blockqueue_.front().sequence);
        blockqueue_.pop_front();
    }
    nextneedsfadein_ = next_; sink_evlog("fade_arm", next_);
}

bool source_desc::check_packet(const data_packet &d){
    if (d.sequence < next_){
        // block too old, discard!
        LOG_VERBOSE("discarded old block " << d.sequence);
        return false;
    }
    auto diff = d.sequence - newest_;

    // check for large gap between incoming block and most recent block
    // (either network problem or stream has temporarily stopped.)
    bool large_gap = newest_ > 0 && diff > blockqueue_.capacity();

    // check if we need to recover
    bool recover = streamstate_.need_recover();

    // check for empty block (= skipped)
    bool dropped = d.totalsize == 0;

    // check for buffer underrun
    bool underrun = streamstate_.have_underrun();

    // check and update newest sequence number
    if (diff < 0){
        // TODO the following distinction doesn't seem to work reliably.
        if (ack_list_.find(d.sequence)){
            LOG_DEBUG("resent block " << d.sequence);
            streamstate_.add_resent(1);
        } else {
            LOG_VERBOSE("block " << d.sequence << " out of order!");
            streamstate_.add_reordered(1);
        }
    } else {
        if (newest_ > 0 && diff > 1){
            LOG_VERBOSE("skipped " << (diff - 1) << " blocks");
        }
        // update newest sequence number
        newest_ = d.sequence;
        newest_pub_.store(newest_, std::memory_order_relaxed);   
    }

    // A source-side skip ("dropped", totalsize==0: the sender has nothing for ONE slot) is not a
    // buffer failure. Full recovery would clear the backlog and refill with silence, so handle it
    // as a single-block splice: advance, fade, one lost, done.
    if (dropped && !large_gap && !recover && !underrun){
        sink_evlog("recovery_dropped", (int32_t)blockqueue_.size());
        streamstate_.add_lost(1);
        g_sbLostSite[0].fetch_add((uint64_t)(1), std::memory_order_relaxed);   // Per-reason lost counter (SB-DROPS)
        next_++;
        nextneedsfadein_ = next_; sink_evlog("fade_arm", next_);
        return false;
    }

    if (large_gap || recover || dropped || underrun){
        sink_evlog(large_gap ? "recovery_large_gap" : recover ? "recovery_recover"
                   : dropped ? "recovery_dropped" : "recovery_underrun", (int32_t)blockqueue_.size());
        if (!dropped) rx_underruns_pub_.fetch_add(1, std::memory_order_relaxed);   // Receive-health counter
        if (kSinkDebug) {
            static std::atomic<int> recov_log_n{0};
            int n = recov_log_n.fetch_add(1);
            if (n < 30 || (n % 200) == 0) {
                std::cerr << "[SINK-RECOVERY] #" << n
                          << " reason=" << (large_gap ? "large_gap " : "")
                                        << (recover   ? "recover "   : "")
                                        << (dropped   ? "dropped "   : "")
                                        << (underrun  ? "underrun "  : "")
                          << "diff=" << diff
                          << " seq=" << d.sequence
                          << " newest=" << newest_
                          << " bq_cap=" << blockqueue_.capacity()
                          << " bq_size=" << blockqueue_.size()
                          << " aq_avail=" << audioqueue_.write_available()
                          << " totalsize=" << d.totalsize
                          << "\n";
            }
        }
        // Completed depth the audio thread could still reach at this moment. An underrun recovery should
        // always log 0 here; a non-zero value means the consumer starved with audio in the ring
        // (resampler-side issue).
        sink_evlog("recovery_aq", (int32_t) audioqueue_.read_available());
        if (underrun){   // Had arrivals stopped, or did consumption outrun them?
            sink_evlog("underrun_since_fresh_ms", adapt_last_arrival_ >= 0 ? (int32_t) ((sb_wall_seconds() - adapt_last_arrival_) * 1e3) : -1);
            sink_evlog("underrun_seq_gap", (int32_t) (d.sequence - newest_));
        }
        // Adaptive depth — only TRUE starvation feeds the glitch budget
        // (loss = drop_unresent, PLC only; source skips = 'dropped', not counted).
        if (adapt_on_ && underrun) adapt_underruns_sec_++;
        drift_dirty_sec_++;   // Recovery refill is not skew
        // record dropped blocks
        // Report exactly 1 so underrun recoveries (empty blockqueue) are visible to the auto-buffer
        // control loop, never the queue size: size-proportional reports form a positive feedback loop
        // (bigger buffer -> bigger reported loss -> harder AutoFull growth). One recovery = one grow
        // event. Gap accounting below is separate.
        streamstate_.add_lost(1);
        g_sbLostSite[1].fetch_add((uint64_t)(1), std::memory_order_relaxed);   // Per-reason lost counter (SB-DROPS)
        if (diff > 1){
            // record gap (measured in blocks)
            streamstate_.add_gap(diff - 1);
        }
        // clear the block queue and fill audio buffer with zeros.
        blockqueue_.clear();
        ack_list_.clear();
        next_ = d.sequence;
        // Refill to the target depth, not the ring's capacity (see refill_silence_to_target()).
        // Tiny buffers (target <= 2) refill fully, because keeping a slot free would leave them nearly
        // empty and re-dry at once; incoming blocks are absorbed by the blockqueue. Normal buffers keep
        // one slot of headroom so the next real block enters immediately.
        const int count = refill_silence_to_target((governor_target_blocks_ <= 2) ? 0 : 1);
        if (count > 0) refill_pending_.store(1, std::memory_order_relaxed);   // Overshoot after this refill is drained at once

        if (count > 0){
            auto reason = large_gap ? "transmission gap"
                          : recover ? "sink xrun"
                          : dropped ? "source xrun"
                          : underrun ? "buffer underrun"
                          : "?";
            LOG_VERBOSE("wrote " << count << " empty blocks for " << reason);
            //if (large_gap > 0) {
            nextneedsfadein_ = next_; sink_evlog("fade_arm", next_);
            //}
        }

        if (dropped){
            next_++;
            nextneedsfadein_ = next_; sink_evlog("fade_arm", next_);
            return false;
        }
    }
    return true;
}

bool source_desc::add_packet(const data_packet& d){
    auto block = blockqueue_.find(d.sequence);
    if (!block){
        if (blockqueue_.full()){
            // if the queue is full, we have to drop a block;
            // in this case we send a block of zeros to the audio buffer.
            auto old = blockqueue_.front().sequence;
            // first we check if the first (complete) block is about to be read next,
            // which means that we have a buffer overflow (the source is too fast)
            if (old == next_ && blockqueue_.front().complete()){
                // Trickle skim: producer-faster-than-consumer skew (~44 ppm = one 128smp block per minute)
                // lands here. Skim exactly ONE oldest block per detection: a single-block trim, no zero-refill,
                // no mass drop, so latency oscillates +-1 block around the working point.
                // SB_AOO_SKIM=0 disables it (env or taps file).
                static int skim_enabled = -1;
                if (skim_enabled < 0){
                    const char* e = getenv("SB_AOO_SKIM");
                    if (e && *e){ skim_enabled = atoi(e) != 0; }
                    else {
                        skim_enabled = 0;  // DEFAULT OFF — first shape held depth AT PEAK (see commit)
                        FILE* cf = sb_taps_fopen();
                        if (cf){
                            char line[256];
                            while (fgets(line, sizeof(line), cf)){
                                if (!strncmp(line, "SB_AOO_SKIM=", 12)){
                                    skim_enabled = atoi(line + 12) != 0;
                                    break;
                                }
                            }
                            fclose(cf);
                        }
                    }
                    fprintf(stderr, "aoo_sink: trickle skim = %d\n", skim_enabled);
                }
                if (skim_enabled){
                    ack_list_.remove(blockqueue_.front().sequence);
                    blockqueue_.pop_front();
                    next_ = old + 1;
                    streamstate_.add_lost(1);
                    g_sbLostSite[2].fetch_add((uint64_t)(1), std::memory_order_relaxed);   // Per-reason lost counter (SB-DROPS)
                    LOG_VERBOSE("skimmed 1 block to relieve skew overrun");
                } else {
                // clear the block queue and fill audio buffer with zeros.
                // Reached only when the audioqueue itself is full (target + bqcap blocks) AND the blockqueue
                // is full behind it, i.e. the governor is off (SB_AOO_DRAIN=0) and the source runs fast.
                // The excess sits in the audioqueue, which this thread cannot pop, so the depth is reset to the
                // target by skipping ahead: the in-flight sequences that would refill it are discarded on arrival.
                const int32_t nqueued = blockqueue_.size();
                blockqueue_.clear();
                ack_list_.clear();
                // push empty blocks to keep the buffer at the target (headroom
                // rule as in check_packet) — a no-op here, the ring is full.
                const int count = refill_silence_to_target((governor_target_blocks_ <= 2) ? 0 : 1);
                // record dropped blocks
                streamstate_.add_lost(nqueued);
                g_sbLostSite[3].fetch_add((uint64_t)(nqueued), std::memory_order_relaxed);   // Per-reason lost counter (SB-DROPS)
                // update 'next'!
                next_ = d.sequence;
                LOG_VERBOSE("dropped " << nqueued << " blocks to handle buffer overrun ("
                            << count << " silence blocks written)");
                sink_evlog("overflow_clear", nqueued);
                const int32_t excess = (int32_t) audioqueue_.read_available() - governor_target_blocks_;
                if (excess > 0){
                    skip_ahead(excess);
                    sink_evlog("overflow_skip", excess);
                }
                if (d.sequence < next_){
                    // this packet is inside the skipped range
                    return false;
                }
                }
            } else {
                if (audioqueue_.write_available() && infoqueue_.write_available()){
                    auto ptr = audioqueue_.write_data();
                    auto nsamples = audioqueue_.blocksize();
                    decoder_->decode(nullptr, 0, ptr, nsamples);
                    audioqueue_.write_commit();
                    // push nominal samplerate + current channel
                    block_info i;
                    i.sr = decoder_->samplerate();
                    i.channel = channel_;
                    infoqueue_.write(i);
                }
                // record dropped block
                streamstate_.add_lost(1);
                g_sbLostSite[4].fetch_add((uint64_t)(1), std::memory_order_relaxed);   // Per-reason lost counter (SB-DROPS)
                // remove block from acklist
                ack_list_.remove(old);
                // update 'next'!
                if (next_ <= old){
                    next_ = old + 1;
                }
                LOG_VERBOSE("dropped block " << old << " (queue full)");
            }
        }
        // add new block
        double srate = d.samplerate > 0 ? d.samplerate : samplerate_;
        int chan = d.channel >= 0 ? d.channel : channel_;
        block = blockqueue_.insert(d.sequence, srate,
                                   chan, d.totalsize, d.nframes);
    } else if (block->has_frame(d.framenum)){
        LOG_VERBOSE("frame " << d.framenum << " of block " << d.sequence << " already received!");
        return false;
    }

    // add frame to block
    block->add_frame(d.framenum, (const char *)d.data, d.size);

#if 0
    if (block->complete()){
        // remove block from acklist as early as possible
        ack_list_.remove(block->sequence);
    }
#endif
    return true;
}

// True if any block after 'next' in the queue is complete (frames of the head block that are
// still missing at that point are lost, not in flight). blockqueue_ is sorted by sequence; it holds a handful of blocks.
template <typename It>
static bool sb_newer_block_complete_impl(It from, It end, int32_t next){
    for (auto it = from; it != end; ++it){
        if (it->sequence > next && it->complete()) return true;
    }
    return false;
}

void source_desc::process_blocks(const sink& s){
    g_sbEvSink = s.id(); g_sbEvSrc = id_;   // Evlog stream identity
    // Transfer all consecutive complete blocks as long as
    // no previous (expected) blocks are missing.
    if (blockqueue_.empty()){
        return;
    }

    auto b = blockqueue_.begin();
    int32_t next = next_;
    while (b != blockqueue_.end() && audioqueue_.write_available())
    {
        const char *data;
        int32_t size;
        block_info i;
        const bool dofadein = b->sequence == nextneedsfadein_;
        
        if (b->sequence == next && b->complete()){
            // block is ready
            LOG_DEBUG("write samples (" << b->sequence << ")");
            data = b->data();
            size = b->size();
            i.sr = b->samplerate;
            i.channel = b->channel;

            b++;
        } else if (!ack_list_.get(next).remaining()
                   && (s.resend_limit() > 0 || sb_newer_block_complete_impl(b, blockqueue_.end(), next))){
            // block won't be resent, just drop it
            // With resend off, remaining is 0 from the start, so the first frame of a multi-frame block would
            // make the still-incomplete head block "unresent". Instead, with resend off, an incomplete head
            // block is given up only once a newer block has been fully received: its missing frames are then
            // really lost. A missing single-frame block is unaffected (the newer block is already there).
            data = nullptr;
            size = 0;
            i.sr = decoder_->samplerate();
            i.channel = channel_;

            if (b->sequence == next){
                b++;
            }

            sink_evlog("drop_unresent", next);
            LOG_VERBOSE("dropped block " << next);
            streamstate_.add_lost(1);
            g_sbLostSite[5].fetch_add((uint64_t)(1), std::memory_order_relaxed);   // Per-reason lost counter (SB-DROPS)
        } else {
            // wait for block
            break;
        }

        next++;

        // decode data and push samples
        auto ptr = audioqueue_.write_data();
        auto nsamples = audioqueue_.blocksize();
        // decode audio data
        // Masked stream → expand the blob first (PLC path with data == nullptr unchanged)
        const int32_t decres = (data && (protocol_flags_ & AOO_PROTOCOL_FLAG_IDLE_MASK))
            ? decode_masked(data, size, ptr, nsamples)
            : decoder_->decode(data, size, ptr, nsamples);
        if (decres < 0){
            LOG_WARNING("aoo_sink: couldn't decode block!");
            // decoder failed - fill with zeros
            std::fill(ptr, ptr + nsamples, 0);
        }
        else if (dofadein) {
            // fade the samples in
            LOG_VERBOSE("fading in block");
            auto nchannels = decoder_->nchannels();
            const int sframes = nsamples/nchannels;
            for (int i = 0; i < nchannels; ++i){
                float gain = 0.0f;
                const float gaindelta = 1.0f / sframes;
                for (int j = 0; j < sframes; ++j){
                    ptr[j*nchannels+i] *= gain;
                    gain += gaindelta;
                }
            }                   
            
            nextneedsfadein_ = -1;
        }
        audioqueue_.write_commit();

        // push info
        infoqueue_.write(i);
    }
    next_ = next;
    // pop blocks
    auto count = b - blockqueue_.begin();
    while (count--){
    #if 1
        // remove block from acklist
        ack_list_.remove(blockqueue_.front().sequence);
    #endif
        // pop block
        LOG_DEBUG("pop block " << blockqueue_.front().sequence);
        blockqueue_.pop_front();
    }
    LOG_DEBUG("next: " << next_);

    // ─── DEPTH GOVERNOR (SB_AOO_DRAIN, default on) ───
    // ~44 ppm producer/consumer clock skew deposits one complete block per minute behind a full
    // audioqueue; left alone, that is an RTT sawtooth. The governor bleeds the skew on the network
    // thread, with thresholds scaled to the target (computed in do_update, see there):
    //   * HYSTERESIS: arm when total completed depth exceeds target + H, H = max(2, ceil(target/2));
    //     once armed, bleed down and disarm at depth <= target. Target = committed/manual buffer size
    //     in blocks (governor_target_blocks_, captured in do_update from s.buffersize()).
    //   * DWELL: a non-emergency drain needs the armed state to have persisted >= 250 ms. Jitter
    //     un-arms within one callback period and never splices; skew / recovery excess is permanent
    //     and drains.
    //   * RATE LIMIT: at most 1 non-emergency drain per 5 s.
    //   * EMERGENCY only when completed lead >= E = max(H+1, bqcap-2), i.e. the blockqueue is about
    //     to hit the mass-clear overflow path; rate-limited to >= 250 ms apart unless one block from
    //     overflow.
    //   * ONE SPLICE PER DRAIN: pop the whole excess (down to target) with a single fade, using the
    //     sink's own splice softening (nextneedsfadein_).
    // SB_AOO_GOV_V4=0 selects fixed thresholds: H = 2, emergency at lead > 4, one block per drain,
    // no dwell.
    {
        static int drain_enabled = -1;
        if (drain_enabled < 0){
            const char* e = getenv("SB_AOO_DRAIN");
            if (e && *e){ drain_enabled = atoi(e) != 0; }
            else {
                // Default on: a player with no taps file must get the governor.
                // SB_AOO_DRAIN=0 (env or taps) disables it.
                drain_enabled = 1;
                FILE* cf = sb_taps_fopen();
                if (cf){
                    char line[256];
                    while (fgets(line, sizeof(line), cf)){
                        if (!strncmp(line, "SB_AOO_DRAIN=", 13)){
                            drain_enabled = atoi(line + 13) != 0;
                            break;
                        }
                    }
                    fclose(cf);
                }
            }
            fprintf(stderr, "aoo_sink: depth governor = %d\n", drain_enabled);
        }
        // ─── LEAD HAND-OFF: the governor's "lead" is EXCESS ───
        // With audioqueue capacity = target + bqcap (do_update), completed
        // consecutive blocks never wait in the blockqueue: the decode loop
        // above hands every one of them to the audio thread at completion.
        // So the quantity to trim is the completed depth above the target:
        //   excess = qblocks + lead - pending - target.
        //   * qblocks  audioqueue blocks (what the consumer can reach)
        //   * lead     completed consecutive blocks still in the blockqueue:
        //              only ever > 0 when the audioqueue is physically full
        //              (governor off / burst beyond capacity)
        //   * pending  sequences already skipped by a previous drain that have
        //              not arrived yet (next_ - newest_ - 1, clipped at 0): a
        //              skip-ahead drain takes effect one block per period as
        //              the skipped sequences arrive and are discarded, so the
        //              depth it already committed to remove is subtracted here;
        //              otherwise the emergency bypass would re-fire on every
        //              packet of the window and chain-splice.
        // DRAIN: the excess cannot be popped from the blockqueue (it is in the
        // SPSC ring, owned by its consumer). npop blocks are removed as the
        // CONTIGUOUS sequences next_ .. next_+npop-1: whatever of them is
        // physically queued (lead) is popped, the remainder is skipped ahead
        // (skip_ahead) and discarded on arrival, with a single fade splice at
        // next_+npop. The latency drop is realised over npop block periods.
        static const int32_t depth_trace_tap = sink_tap_int("SB_AOO_DEPTH_TRACE", 0, nullptr);   // Trace/ASRC run with the governor off too
        if (drain_enabled || depth_trace_tap > 0 || drift_on_){
            int32_t lead = 0;
            if (!blockqueue_.empty() && blockqueue_.front().sequence == next_){
                int32_t seq = next_;
                for (auto it = blockqueue_.begin();
                     it != blockqueue_.end() && it->sequence == seq && it->complete();
                     ++it, ++seq) ++lead;
            }
            const int32_t qblocks = audioqueue_.read_available();
            const int32_t target = governor_target_blocks_ > 0 ? governor_target_blocks_ : qblocks;
            const int32_t pending = std::max<int32_t>(0, next_ - newest_ - 1);
            const int32_t depth = qblocks + lead - pending;
            const int32_t excess = depth - target;
            // Once-per-second depth trace (SB_AOO_DEPTH_TRACE=1) —
            // qblocks / lead / pending / target, to see the SHAPE of the excess the governor trims
            // (sawtooth = rate mismatch, steps = bursts or insertions). Network thread, rate-limited.
            {
                static const int32_t depth_trace = sink_tap_int("SB_AOO_DEPTH_TRACE", 0, nullptr);
                if (depth_trace > 0){
                    const double nowt = s.elapsed_time();
                    if (nowt - governor_last_depth_log_ >= 1.0){
                        governor_last_depth_log_ = nowt;
                        // detail = (source id mod 90) * 1000 + value: tells the feeds apart in the shared evlog
                        const int32_t tag = (int32_t)(((id_ % 90) + 90) % 90) * 1000;
                        sink_evlog("aq_depth", tag + std::min<int32_t>(999, std::max<int32_t>(0, depth)));
                        sink_evlog("aq_lead", tag + std::min<int32_t>(999, lead));
                        sink_evlog("aq_pending", tag + std::min<int32_t>(999, pending));
                        sink_evlog("aq_target", tag + std::min<int32_t>(999, target));
                        // feed tag (id mod 90) * 10^7 + sequence (fits int32 for ~1.8 h of stream); counters: tag*100000 + count
                        const int32_t ftag = (int32_t)(((id_ % 90) + 90) % 90);
                        sink_evlog("aq_seq", ftag * 10000000 + (newest_ % 10000000));
                        sink_evlog("aq_calls", ftag * 100000 + (int32_t) std::min<uint32_t>(99999u, trace_calls_.exchange(0, std::memory_order_relaxed)));
                        sink_evlog("aq_reads", ftag * 100000 + (int32_t) std::min<uint32_t>(99999u, trace_reads_.exchange(0, std::memory_order_relaxed)));
                        sink_evlog("aq_moves", ftag * 100000 + (int32_t) std::min<uint32_t>(99999u, trace_moves_.exchange(0, std::memory_order_relaxed)));
                        sink_evlog("aq_dry",   ftag * 100000 + (int32_t) std::min<uint32_t>(99999u, trace_dry_.exchange(0, std::memory_order_relaxed)));
                        // The resampler ratio actually in force (ppm), the source's stamped rate and the sink's real rate (mHz offsets from 48000)
                        sink_evlog("aq_ratio_ppm", (int32_t) std::lround((resampler_.ratio() - 1.0) * 1e6));
                        sink_evlog("aq_srfrom_mhz", (int32_t) std::lround((samplerate_ - (double) s.samplerate()) * 1000.0));
                        sink_evlog("aq_srto_mhz", (int32_t) std::lround((s.real_samplerate() - (double) s.samplerate()) * 1000.0));
                        sink_evlog("aq_dyn", s.dynamic_resampling_on() ? 1 : 0);
                    }
                }
            }
            if (adapt_shrink_pending_ && excess <= 0) adapt_shrink_pending_ = false;   // Nothing to drain
            if (drift_on_) drift_tick(s, s.elapsed_time(), depth, target);   
            if (drain_enabled && excess > 0){
                const bool v4 = governor_v4_;
                // Fixed thresholds (+2 band, emergency at excess > 4) unless v4, which uses the
                // target-scaled thresholds computed once per rebuild in do_update().
                const int32_t arm_band = v4 ? governor_arm_blocks_ : 2;
                const int32_t emerg_lead = v4 ? governor_emerg_lead_ : 5;
                const int32_t bqcap = (int32_t) blockqueue_.capacity();
                const bool hardfull = excess >= emerg_lead; // overflow emergency
                const double now = s.elapsed_time(); // relaxed atomic load

                if (v4 && !governor_v4_logged_){
                    // first governed call after a rebuild: publish the thresholds
                    governor_v4_logged_ = true;
                    sink_evlog("governor_v4", target);
                    sink_evlog("governor_v4_slack", governor_slack_blocks_);
                    sink_evlog("governor_v4_arm", arm_band);
                    sink_evlog("governor_v4_emerg", emerg_lead);
                    sink_evlog("governor_v4_bqcap", bqcap);
                }

                if (depth > target + arm_band){
                    if (!governor_armed_){
                        governor_armed_ = true;
                        governor_armed_since_ = now;
                    }
                } else if (depth <= target){
                    governor_armed_ = false;
                }

                bool drain = false;
                if (adapt_shrink_pending_){
                    // Adaptive retarget DOWN — one splice, now
                    drain = true;
                } else if (hardfull){
                    // Without v4 the emergency bypasses every limit. With v4, >= 250 ms between
                    // emergency drains (a 2-3 block burst can never chain), except when the depth
                    // is ONE block from the mass-clear overflow path in add_packet(), which is
                    // strictly worse than a splice.
                    drain = !v4 || excess >= bqcap - 1
                            || (now - governor_last_emerg_time_) >= 0.25;
                } else if (drift_on_ && drift_no_drain_){
                    // SB_AOO_DRIFT_NO_DRAIN: with the drift ASRC on, skip non-emergency drains, which
                    // would only splice jitter clumps. Emergency (near overflow) still drains.
                    drain = false;
                } else if (governor_armed_ && (now - governor_last_drain_time_) >= 5.0){
                    // v4 DWELL: the excess must have persisted >= 250 ms. A late
                    // audio callback or an arrival clump un-arms within one
                    // callback period (depth <= target at the next arrival) and
                    // never reaches here; skew / recovery excess is permanent.
                    drain = !v4 || (now - governor_armed_since_) >= 0.25;
                }
                // Excess that appears within 2 s of a silence refill is the delayed burst landing behind the
                // silence: refill overshoot, not skew. Drain it at once instead of parking the extra latency
                // for the 5-s rate limit.
                if (refill_pending_.exchange(0, std::memory_order_relaxed)){ refill_seen_t_ = now; refill_drained_ = false; }
                // One drain per refill, after a 100-ms settle so the delayed burst has fully landed.
                if (!drain && !refill_drained_ && excess > 0 && (now - refill_seen_t_) >= 0.1 && (now - refill_seen_t_) < 2.0){
                    drain = true; refill_drained_ = true; sink_evlog("governor_refill_overshoot", excess);
                }
                if (!refill_drained_ && (now - refill_seen_t_) >= 2.0) refill_drained_ = true;

                if (drain){
                    // One block per call, or with v4 the whole excess (down to target) in ONE splice:
                    // one fade instead of a chain of them. Bounded by excess <= bqcap (the audioqueue
                    // holds at most target + bqcap). Physically queued lead is popped (move-only
                    // pop_front + ack mark-deleted); the rest of the contiguous range is skipped ahead.
                    int32_t npop = 1;
                    if (v4){
                        npop = excess;
                        if (npop < 1) npop = 1;
                    }
                    const int32_t nphys = std::min<int32_t>(npop, lead);
                    // Only level drains are excluded from the rate estimate; rate drains stay in
                    if (adapt_shrink_pending_) drift_skipped_blocks_ += npop;
                    int32_t lastseq = next_ - 1;
                    for (int32_t k = 0; k < nphys; ++k){
                        lastseq = blockqueue_.front().sequence;
                        ack_list_.remove(lastseq);
                        blockqueue_.pop_front();
                    }
                    next_ = lastseq + 1;
                    // skip_ahead() sets next_ += (npop - nphys), pops anything
                    // queued below it and arms the fade at the new next_. When
                    // npop == nphys it still arms the fade (n == 0 short-circuits,
                    // so arm explicitly for that case).
                    if (npop > nphys){
                        skip_ahead(npop - nphys);
                    } else {
                        nextneedsfadein_ = next_; sink_evlog("fade_arm", next_);
                    }
                    // Do NOT book the drain as a lost block. A governor drain is clock-skew maintenance:
                    // its rate is set by ppm skew, not buffer size, so reporting it via add_lost() would
                    // feed the auto-buffer loop a grow signal it can never satisfy. Real starvation still
                    // reports through the recovery paths; drains remain observable via the governor_drain
                    // evlog tag.
                    sink_evlog("governor_drain", excess);
                    // A governor drain is a known-size splice (npop blocks), tallied per window.
                    // The rate estimate does not read this tally.
                    drift_win_drained_ += npop;
                    { static const int32_t dtr = sink_tap_int("SB_AOO_DEPTH_TRACE", 0, nullptr);
                      if (dtr > 0) sink_evlog("aq_drain_tag", (int32_t)(((id_ % 90) + 90) % 90) * 1000 + std::min<int32_t>(999, npop)); }
                    if (v4){
                        sink_evlog("governor_pop", npop);            // blocks removed in this ONE splice
                        rx_pops_pub_.fetch_add(1, std::memory_order_relaxed);   // Receive-health counter
                        if (hardfull) sink_evlog("governor_emergency", excess);
                    }
                    if (npop > nphys){
                        sink_evlog("governor_skip", npop - nphys);   // of which skipped ahead (in flight)
                    }
                    LOG_VERBOSE("depth governor: drained " << npop << " block(s), " << nphys
                                << " popped, " << (npop - nphys) << " skipped (excess=" << excess
                                << " depth=" << depth << " target=" << target
                                << (v4 ? " v4" : " v3")
                                << (hardfull ? " EMERGENCY" : "") << ")");
                    governor_last_drain_time_ = now;
                    if (adapt_shrink_pending_){ adapt_shrink_pending_ = false; sink_evlog("adapt_shrink", npop); }
                    if (hardfull){
                        governor_last_emerg_time_ = now;
                    }
                    if (excess - npop <= 0){
                        governor_armed_ = false;
                    }
                }
            }
        }
    }
}

// ─── DRIFT ASRC controller (network thread, checked once per second) ───
//   Every SB_AOO_DRIFT_WIN_S (180) seconds the measured rate residual applies one step of
//   SB_AOO_DRIFT_GAIN_PCT (60) to the ratio. A step is limited by SB_AOO_DRIFT_SLEW_PPM_S (5 ppm/s)
//   and SB_AOO_DRIFT_STEP_MAX_PPM (15); the ratio clamps at +/-SB_AOO_DRIFT_MAX_PPM (500).
//   ppm > 0 = we consume the source faster.
void source_desc::drift_tick(const sink& s, double now, int32_t depth, int32_t target){
    (void) depth; (void) target;
    // Same method as the hub clock lock:
    //   source audio-time delivered (newest sequence advance × source block period)
    //   vs sink audio-time consumed (frames consumed / sink rate), over a WIN_S window, under the CURRENT ratio.
    //   residual = src_time / (sink_time × ratio) - 1 -> one gained step per window. No depth, no events:
    //   drains, PLC refills and jitter bursts cannot fool it.
    static const int32_t win_s    = std::max<int32_t>(20, sink_tap_int("SB_AOO_DRIFT_WIN_S", 180, nullptr));
    static const int32_t gain_pct = std::max<int32_t>(0, std::min<int32_t>(100, sink_tap_int("SB_AOO_DRIFT_GAIN_PCT", 60, nullptr)));   // 0 = MEASURE ONLY (log asrc_win_ppm, apply nothing)
    static const int32_t slew     = std::max<int32_t>(1, sink_tap_int("SB_AOO_DRIFT_SLEW_PPM_S", 5, nullptr));
    static const int32_t max_ppm  = std::max<int32_t>(10, sink_tap_int("SB_AOO_DRIFT_MAX_PPM", 500, nullptr));
    static const int32_t trace    = sink_tap_int("SB_AOO_DEPTH_TRACE", 0, nullptr);
    if (!decoder_) return;
    const double src_block_s = (double) decoder_->blocksize() / (double) std::max<int32_t>(1, decoder_->samplerate());
    const double sink_sr = (double) std::max<int32_t>(1, s.samplerate());
    auto reset_win = [&](){ drift_win_t0_ = now; drift_seq0_ = newest_pub_.load(std::memory_order_relaxed); drift_frames0_ = drift_frames_; drift_fentry0_ = drift_frames_entry_; drift_silence0_ = drift_silence_frames_; drift_skipped0_ = drift_skipped_blocks_; drift_win_drained_ = 0; drift_dirty_sec_ = 0; };
    if (drift_last_t_ < 0){ drift_last_t_ = now; reset_win(); return; }
    if (now - drift_last_t_ < 1.0) return;
    drift_last_t_ = now;
    if (now - drift_win_t0_ < (double) win_s) return;
    if (trace > 0){   // Raw per-window deltas for offline analysis
        sink_evlog("win_dseq",   (int32_t)(newest_pub_.load(std::memory_order_relaxed) - drift_seq0_));
        sink_evlog("win_fread",  (int32_t)((drift_frames_ - drift_frames0_) / std::max<int32_t>(1, audioqueue_.blocksize())));
        sink_evlog("win_fentry", (int32_t)((drift_frames_entry_ - drift_fentry0_) / std::max<int32_t>(1, audioqueue_.blocksize())));
        sink_evlog("win_sil",    (int32_t)((drift_silence_frames_ - drift_silence0_) / std::max<int32_t>(1, audioqueue_.blocksize())));
        sink_evlog("win_skip",   (int32_t)(drift_skipped_blocks_ - drift_skipped0_));
    }
    // The sink clock is the callback count at process() entry: starved callbacks are sink time too.
    // Counting only resampler reads would under-count every stall while the late burst still advances
    // the sequence, a positive bias proportional to the stall rate.
    // Source time and sink time are two independent clocks and neither moves when the queue level does,
    // so level moves are not subtracted: that would bias the estimate into a refill/drain feedback loop.
    const int64_t dseq = (int64_t)(newest_pub_.load(std::memory_order_relaxed) - drift_seq0_);
    const int64_t dframes = drift_frames_entry_ - drift_fentry0_;
    if (dseq > 0 && dframes > 0){
        const double src_time  = (double) dseq * src_block_s;
        const double sink_time = (double) dframes / sink_sr;
        const double ratio_now = 1.0 + drift_ppm_ * 1e-6;
        const double residual_ppm = (src_time / (sink_time * ratio_now) - 1.0) * 1e6;   // + = source faster → consume faster
        if (std::fabs(residual_ppm) < 2000.0){   // sanity: a re-sync / format change inside the window is not skew
            // The first window after a (re)start is discarded (it holds the join transient), every applied
            // window uses the gain, and one window may move the ratio by at most SB_AOO_DRIFT_STEP_MAX_PPM,
            // so a bad window cannot do lasting damage.
            static const int32_t step_max_ppm = std::max<int32_t>(1, sink_tap_int("SB_AOO_DRIFT_STEP_MAX_PPM", 15, nullptr));   // 15: a 44-ppm crystal lands in ~3 windows (9 min)
            if (drift_applied_windows_++ == 0){ reset_win(); return; }
            double want = drift_ppm_ + residual_ppm * (gain_pct / 100.0);
            want = std::max(-(double) max_ppm, std::min((double) max_ppm, want));
            const double lim = std::min((double) slew * (double) win_s, (double) step_max_ppm);
            drift_ppm_ += std::max(-lim, std::min(lim, want - drift_ppm_));
            drift_ratio_.store(1.0 + drift_ppm_ * 1e-6, std::memory_order_relaxed);
            if (trace > 0){ sink_evlog("asrc_ppm", (int32_t) std::lround(drift_ppm_)); sink_evlog("asrc_win_ppm", (int32_t) std::lround(residual_ppm)); }
        }
    }
    reset_win();
}

// ─── WAN ADAPTIVE receive depth ───
// Network thread only (called from handle_data under the shared lock, i.e.
// never concurrently with do_update). No allocation, no audio-thread state.
//
// adapt_track(): per fresh block (first frame of a sequence we have not seen)
// lateness = arrival - (t0 + (seq - seq0) * block period). A 125 ms window
// collects min/max/sd; at the window end the running minimum re-anchors DOWN
// immediately and creeps UP at SB_AOO_ADAPT_CREEP_US_PER_S (source-clock drift
// follower); window jitter = max - running min. EWMA: up-moves at
// SB_AOO_ADAPT_UP_PCT per window (bursts are captured fast), down-moves with
// tau = SB_AOO_ADAPT_TAU_S (slow release, JackTrip's 60 s idea).
void source_desc::adapt_track(const sink& s, const data_packet& d, bool fresh){
    if (!fresh || d.framenum != 0 || d.totalsize == 0) return;
    const double now = s.elapsed_time();
    static const bool wallclock = sink_tap_int("SB_AOO_ADAPT_WALLCLOCK", 1, nullptr) != 0;
    const double anow = wallclock ? sb_wall_seconds() : now;   // arrival clock for the gap histogram
    {   // Arrival-gap histogram (hub hold model). Runs from the first packet, warm-up or not;
        // the slice rotation drops anything older than 60 s, so the launch storm ages out by itself.
        if (adapt_hist_slice_t0_ < 0) adapt_hist_slice_t0_ = now;
        if (now - adapt_hist_slice_t0_ >= 10.0){
            adapt_hist_slice_ = (adapt_hist_slice_ + 1) % kAdaptHistSlices;
            memset(adapt_hist_[adapt_hist_slice_], 0, sizeof(adapt_hist_[0]));
            adapt_hist_slice_t0_ = now;
            adapt_hist_filled_ = std::min<int32_t>(kAdaptHistSlices, adapt_hist_filled_ + 1);
        }
        if (adapt_last_arrival_ >= 0){
            const double raw_gap_ms = (anow - adapt_last_arrival_) * 1e3;
            if (raw_gap_ms > 3.0){   // Every big arrival gap with its sequence jump
                sink_evlog("adapt_gap_ms10", (int32_t) (raw_gap_ms * 10.0));
                sink_evlog("adapt_gap_seqjump", adapt_last_seq_ >= 0 ? (int32_t) (d.sequence - adapt_last_seq_) : -1);
            }
            // Lateness is measured against the block's own schedule: late = gap - (jump - 1) x period.
            // Skipped or lost sequences are discarded on arrival and never reach this tracker, so the next
            // fresh block would otherwise show a gap of (jump x period) although it arrived on time, and one
            // underrun would count as two gaps. SB_AOO_ADAPT_SEQNORM=0 bins the raw gap.
            double gap_ms = raw_gap_ms;
            static const bool seqnorm = sink_tap_int("SB_AOO_ADAPT_SEQNORM", 1, nullptr) != 0;
            if (seqnorm && adapt_last_seq_ >= 0 && adapt_block_s_ > 0){
                const int32_t jump = d.sequence - adapt_last_seq_;
                if (jump > 1){
                    gap_ms = std::max(0.0, raw_gap_ms - (double) (jump - 1) * adapt_block_s_ * 1e3);
                    if (raw_gap_ms > 3.0) sink_evlog("adapt_gap_late10", (int32_t) (gap_ms * 10.0));   // what is binned instead
                }
            }
            // On the LAN profile log every gap of
            // two block periods or more (the ones the 3-ms instrument above never shows), so a 2-block need can be traced
            // to its arrivals. evlog-gated; off when SB_SINK_EVLOG is not set.
            if (adapt_lan_profile_ && adapt_block_s_ > 0 && raw_gap_ms <= 3.0 && gap_ms >= 2.0 * adapt_block_s_ * 1e3){
                sink_evlog("adapt_gap_lan10", (int32_t) (gap_ms * 10.0));
                sink_evlog("adapt_gap_lan_seqjump", adapt_last_seq_ >= 0 ? (int32_t) (d.sequence - adapt_last_seq_) : -1);
            }
            const int32_t b = gap_ms < 0 ? 0 : std::min<int32_t>(kAdaptHistBuckets - 1, (int32_t) gap_ms);
            // A gap longer than max(2 x hold, hold + 10 ms) is a STALL, not lateness (the hub's rule). Stalls
            // stay OUT of the need histogram unless they RECUR (> 2/min averaged over 2 min = more than 4 in the
            // last 120 s), so isolated stalls cannot walk the depth to the ceiling; gaps the ceiling could never
            // cover are always out. The emergency measured step still sees every stall through
            // adapt_emerg_late_ms_ (capped per window), so an isolated stall costs at most the cap. During the
            // QUICK window (first 10 s after warm-up) every gap counts, as the hub's QUICK covers every gap seen.
            // WAN profile only.
            bool count_it = true;
            // Under ARMED LAN zero tolerance a gap more than SB_AOO_ADAPT_LAN_STALL_MS (5) beyond the hold is a
            // host STALL: excluded from the need unless stalls recur (> 2/min over 2 min), the same rule as WAN.
            if (adapt_lan_profile_ && adapt_zero_armed_ && adapt_block_s_ > 0 && adapt_warm_){
                static const double lan_stall_extra_ms = std::max(0.0, sink_tap_int("SB_AOO_ADAPT_LAN_STALL_MS", 5, nullptr) * 1.0);
                const double depth_ms = governor_target_blocks_ * adapt_block_s_ * 1e3;
                if (gap_ms > depth_ms + lan_stall_extra_ms){
                    adapt_stall_ts_[adapt_stall_idx_] = now; adapt_stall_idx_ = (adapt_stall_idx_ + 1) % kAdaptStallRing;
                    adapt_stall_n_ = std::min<int32_t>(kAdaptStallRing, adapt_stall_n_ + 1);
                    int32_t recent = 0; for (int32_t k = 0; k < adapt_stall_n_; ++k) if (now - adapt_stall_ts_[k] <= 120.0) recent++;
                    count_it = recent > 4;   // recurring (> 2/min over 2 min) → it is the link, cover it
                    if (!count_it){ adapt_stalls_excluded_++; sink_evlog("adapt_stall_excluded", (int32_t) gap_ms); }
                    else sink_evlog("adapt_stall_counted", (int32_t) gap_ms);
                }
            }
            if (!adapt_lan_profile_ && adapt_block_s_ > 0 && adapt_warm_){
                static const double stall_extra_ms = std::max(0.0, sink_tap_int("SB_AOO_ADAPT_STALL_EXTRA_MS", 10, nullptr) * 1.0);
                static const double stall_recur_per_min = std::max(0.0, sink_tap_int("SB_AOO_ADAPT_STALL_RECUR_PER_MIN", 2, nullptr) * 1.0);
                static const int32_t quick_s2 = std::max<int32_t>(0, sink_tap_int("SB_AOO_ADAPT_QUICK_S", 10, nullptr));
                const double depth_ms = governor_target_blocks_ * adapt_block_s_ * 1e3;
                const double stall_ms = std::max(2.0 * depth_ms, depth_ms + stall_extra_ms);
                const double ceil_ms  = adapt_max_blocks_ * adapt_block_s_ * 1e3;
                const bool quick_win  = adapt_warm_at_ >= 0 && (now - adapt_warm_at_) < (double) quick_s2;
                if (gap_ms > stall_ms){
                    adapt_stall_clean_secs_ = 0;   // A stall-class event restarts the decay wait; a plain pop does not
                    adapt_stall_ts_[adapt_stall_idx_] = now; adapt_stall_idx_ = (adapt_stall_idx_ + 1) % kAdaptStallRing;
                    adapt_stall_n_ = std::min<int32_t>(kAdaptStallRing, adapt_stall_n_ + 1);
                    int32_t recent = 0; for (int32_t k = 0; k < adapt_stall_n_; ++k) if (now - adapt_stall_ts_[k] <= 120.0) recent++;
                    const bool recurring = (double) recent > stall_recur_per_min * 2.0;   // > 2/min over 2 min
                    if (gap_ms >= ceil_ms) count_it = false;                              // uncoverable: never sets the need
                    else count_it = recurring || quick_win;
                    if (!count_it){ adapt_stalls_excluded_++; sink_evlog("adapt_stall_excluded", (int32_t) gap_ms); }
                    else sink_evlog("adapt_stall_counted", (int32_t) gap_ms);
                }
            }
            if (count_it && adapt_hist_[adapt_hist_slice_][b] < 65535) adapt_hist_[adapt_hist_slice_][b]++;
            // Remember the worst gap that exceeded the depth in force — this is the
            // lateness a GROW-EMERGENCY has to cover (the block arrived gap_ms after its predecessor; the buffer held
            // depth_ms of it). Reset with the 10-s emergency window.
            if (adapt_block_s_ > 0){
                const double depth_ms = governor_target_blocks_ * adapt_block_s_ * 1e3;
                if (gap_ms > depth_ms && gap_ms > adapt_emerg_late_ms_) adapt_emerg_late_ms_ = gap_ms;
            }
        }
        adapt_last_arrival_ = anow; adapt_last_seq_ = d.sequence;
    }
    if (adapt_t0_ < 0){
        adapt_t0_ = now; adapt_seq0_ = d.sequence; adapt_run_min_ = 0;
        adapt_win_start_ = now; adapt_win_n_ = 0; adapt_win_min_ = 1e9; adapt_win_max_ = -1e9;
        adapt_win_sum_ = adapt_win_sumsq_ = 0; adapt_last_retarget_ = now;
        return;
    }
    // A freshly launched client clumps its receive thread, giving lateness that is NOT the link.
    // Ignore the first SB_AOO_ADAPT_WARMUP_S (5, WAN) or SB_AOO_ADAPT_LAN_WARMUP_S (15) seconds
    // (depth stays at init, no strikes), then re-anchor the schedule and start the stats fresh.
    // The WAN warm-up covers the join transient only, as the hub clears its gap histogram; a launch
    // storm that inflates the depth is trimmed by the commit.
    static const int32_t warmup_s_wan = std::max<int32_t>(0, sink_tap_int("SB_AOO_ADAPT_WARMUP_S", 5, nullptr) /* Was 60 */);
    static const int32_t warmup_s_lan = std::max<int32_t>(0, sink_tap_int("SB_AOO_ADAPT_LAN_WARMUP_S", 15, nullptr));
    const int32_t warmup_s = adapt_lan_profile_ ? warmup_s_lan : warmup_s_wan;
    if (!adapt_warm_ && now - adapt_t0_ < (double) warmup_s){
        adapt_underruns_sec_ = 0;
        return;
    }
    if (!adapt_warm_){
        adapt_warm_ = true;
        adapt_warm_at_ = now;      // Settle window starts
        adapt_t0_ = -1;            // re-anchor on the next fresh block
        adapt_ewma_max_ = -1; adapt_ewma_sd_ = 0; adapt_headroom_blocks_ = 0; adapt_headroom_prev_ = 0;
        adapt_strike_secs_ = 0; adapt_clean_secs_ = 0; adapt_underruns_sec_ = 0;
        sink_evlog("adapt_warm", warmup_s);
        return;
    }
    const double late = now - (adapt_t0_ + (double)(d.sequence - adapt_seq0_) * adapt_block_s_);
    if (late < adapt_win_min_) adapt_win_min_ = late;
    if (late > adapt_win_max_) adapt_win_max_ = late;
    adapt_win_sum_ += late; adapt_win_sumsq_ += late * late; adapt_win_n_++;
    static const int32_t win_ms   = sink_tap_int("SB_AOO_ADAPT_WIN_MS", 125, nullptr);
    static const int32_t tau_s    = std::max<int32_t>(1, sink_tap_int("SB_AOO_ADAPT_TAU_S", 30, nullptr));
    static const int32_t up_pct   = sink_tap_int("SB_AOO_ADAPT_UP_PCT", 25, nullptr);
    static const int32_t creep_us = sink_tap_int("SB_AOO_ADAPT_CREEP_US_PER_S", 200, nullptr);
    static const int32_t trace    = sink_tap_int("SB_AOO_ADAPT_TRACE", 0, nullptr);
    const double elapsed = now - adapt_win_start_;
    if (elapsed >= win_ms * 1e-3 && adapt_win_n_ >= 2){
        adapt_run_min_ = std::min(adapt_win_min_, adapt_run_min_ + creep_us * 1e-6 * elapsed);
        const double jit  = std::max(0.0, adapt_win_max_ - adapt_run_min_);
        const double mean = adapt_win_sum_ / adapt_win_n_;
        const double sd   = std::sqrt(std::max(0.0, adapt_win_sumsq_ / adapt_win_n_ - mean * mean));
        if (adapt_ewma_max_ < 0){
            adapt_ewma_max_ = jit; adapt_ewma_sd_ = sd;
        } else {
            const double a_dn = std::min(1.0, elapsed / (double) tau_s);
            const double a_up = std::min(1.0, std::max(0.0, up_pct / 100.0));
            adapt_ewma_max_ += (jit > adapt_ewma_max_ ? a_up : a_dn) * (jit - adapt_ewma_max_);
            adapt_ewma_sd_  += a_dn * (sd - adapt_ewma_sd_);
        }
        if (trace >= 2) sink_evlog("adapt_wjit_us", (int32_t) (jit * 1e6));
        adapt_win_start_ = now; adapt_win_n_ = 0; adapt_win_min_ = 1e9; adapt_win_max_ = -1e9;
        adapt_win_sum_ = adapt_win_sumsq_ = 0;
    }
    if (now - adapt_last_retarget_ >= 1.0) adapt_retarget(s, now);
}

// adapt_retarget(): once per second.
//   base     = ceil((ewma_max + SD_PCT/100 * ewma_sd) / block) + 1
//   headroom = glitch budget: a second with >= SB_AOO_ADAPT_BUDGET_PER_S (1)
//              underrun recoveries is a strike; two consecutive strikes (or
//              >= 3x budget in one second) grow headroom by
//              SB_AOO_ADAPT_GROW_BLOCKS (3); after SB_AOO_ADAPT_DECAY_S (30)
//              clean seconds it decays one block every SB_AOO_ADAPT_DECAY_STEP_S (5).
//   target   = clamp(base + headroom, floor, max)
//   UP applies immediately (PLC refill, one seam); DOWN needs >= 2 blocks of
//   excess and >= SB_AOO_ADAPT_SHRINK_HOLD_S (5) since the last shrink.
// LAN zero tolerance switch (default ON). Read once per process; never on the audio thread's hot path.
static bool adapt_lan_zero(){
    static const bool on = sink_tap_int("SB_AOO_ADAPT_LAN_ZERO", 1, nullptr) != 0;   // Default on for the LAN profile (player LAN legs + hub LAN sinks); SB_AOO_ADAPT_LAN_ZERO=0 = the 2/min budget
    return on;
}
void source_desc::adapt_retarget(const sink& s, double now){
    (void) s;
    adapt_last_retarget_ = now;
    static const int32_t sd_pct        = sink_tap_int("SB_AOO_ADAPT_SD_PCT", 100, nullptr);
    static const int32_t grow_blocks   = std::max<int32_t>(1, sink_tap_int("SB_AOO_ADAPT_GROW_BLOCKS", 3, nullptr));
    static const int32_t budget        = std::max<int32_t>(1, sink_tap_int("SB_AOO_ADAPT_BUDGET_PER_S", 1, nullptr));
    static const int32_t decay_s_wan   = std::max<int32_t>(1, sink_tap_int("SB_AOO_ADAPT_DECAY_S", 30, nullptr));
    static const int32_t decay_step_wan= std::max<int32_t>(1, sink_tap_int("SB_AOO_ADAPT_DECAY_STEP_S", 5, nullptr));
    // Release slowly: 90 s clean before the first step, one block per 15 s after, so a stall
    // every ~50-100 s keeps the depth where it was needed.
    static const int32_t decay_s_lan   = std::max<int32_t>(1, sink_tap_int("SB_AOO_ADAPT_LAN_DECAY_S", 90, nullptr));
    static const int32_t decay_step_lan= std::max<int32_t>(1, sink_tap_int("SB_AOO_ADAPT_LAN_DECAY_STEP_S", 15, nullptr));
    const int32_t decay_s      = adapt_lan_profile_ ? decay_s_lan : decay_s_wan;
    const int32_t decay_step_s = adapt_lan_profile_ ? decay_step_lan : decay_step_wan;
    static const int32_t shrink_hold_s = std::max<int32_t>(0, sink_tap_int("SB_AOO_ADAPT_SHRINK_HOLD_S", 5, nullptr));
    static const int32_t trace         = sink_tap_int("SB_AOO_ADAPT_TRACE", 0, nullptr);

    const int32_t u = adapt_underruns_sec_; adapt_underruns_sec_ = 0;
    if (u >= budget){ adapt_strike_secs_++; adapt_clean_secs_ = 0; }
    else            { adapt_strike_secs_ = 0; adapt_clean_secs_++; }
    // On WAN the decay clocks run on STALL-free seconds, not pop-free seconds, so a link with one pop
    // every minute or two does not just ratchet up. The clock restarts on a stall-class gap (arrival
    // path) and on any grow (below); an isolated underrun only feeds the need.
    adapt_stall_clean_secs_++;
    {   // The queue's fill once a second — drains slowly (drift) or falls off a cliff?
        sink_evlog("aq_fill", (int32_t) audioqueue_.read_available());
        sink_evlog("bq_size", (int32_t) blockqueue_.size());
        sink_evlog("aq_target", governor_target_blocks_);
    }
    const int32_t clean_for_decay = adapt_lan_profile_ ? adapt_clean_secs_ : adapt_stall_clean_secs_;
    // A loaded LAN host stalls every few seconds, never two seconds in a row:
    // count strikes in a rolling 30-s window and grow on the SECOND one.
    if (adapt_lan_profile_){
        if (now - adapt_strike_win_t0_ >= 30.0){ adapt_strike_win_t0_ = now; adapt_strike_win_n_ = 0; }
        if (u >= budget) adapt_strike_win_n_++;
    }
    // Under ARMED zero tolerance every strike second grows one block at once: the first pop
    // is the last one at that depth, and the clean-time recovery below brings the hold back down.
    const bool lan_grow = adapt_lan_profile_ && ((adapt_zero_armed_ && adapt_lan_zero()) ? (u >= budget) : adapt_strike_win_n_ >= 2);
    if (lan_grow) adapt_strike_win_n_ = 0;
    // JOIN GUARD — no strike-driven grow within
    // SB_AOO_ADAPT_JOIN_GUARD_S (60) of a source add / feed rebuild in this sink; the hub's churn at a join is a transient.
    static const double join_guard_s = std::max(0.0, sink_tap_int("SB_AOO_ADAPT_JOIN_GUARD_S", 60, nullptr) * 1.0);
    const bool join_guard = (now - s.last_source_change()) < join_guard_s;
    if (join_guard && (adapt_strike_secs_ >= 2 || u >= 3 * budget || lan_grow)){
        adapt_strike_secs_ = 0; adapt_strike_win_n_ = 0; sink_evlog("adapt_grow_join_guarded", u);
    } else
    if (adapt_strike_secs_ >= 2 || u >= 3 * budget || lan_grow){
        adapt_headroom_blocks_ += (adapt_lan_profile_ && adapt_zero_armed_) ? 1 : grow_blocks;   // Armed LAN zero tolerance grows ONE block per strike window (converges to the minimal cover)
        adapt_strike_secs_ = 0; adapt_stall_clean_secs_ = 0;
        sink_evlog("adapt_grow", adapt_headroom_blocks_);
    } else if (adapt_lan_profile_ && adapt_lan_zero() && adapt_zero_armed_){
        // FAST RECOVERY: after SB_AOO_ADAPT_LAN_ZERO_RECOVER_S (120) clean seconds drop in ONE step to what
        // the histogram says the link needs now (headroom 0, floor released); the risk of that step is exactly
        // the risk the histogram's own need already accepts. Repeats every further 120 clean s if still above
        // need. A bad minute cannot pin the session; a chronic host keeps what it needs.
        static const int32_t zrecover_s = std::max<int32_t>(1, sink_tap_int("SB_AOO_ADAPT_LAN_ZERO_RECOVER_S", 120, nullptr));
        if (clean_for_decay >= zrecover_s && (clean_for_decay % zrecover_s) == 0 && (adapt_headroom_blocks_ > 0 || adapt_zero_floor_ > 0)){
            adapt_headroom_blocks_ = 0; adapt_zero_floor_ = 0; sink_evlog("adapt_zero_recover", adapt_hist_blocks_);
        }
    } else if (adapt_headroom_blocks_ > 0 && clean_for_decay >= decay_s
               && ((clean_for_decay - decay_s) % decay_step_s) == 0){
        adapt_headroom_blocks_--;
        sink_evlog("adapt_decay", adapt_headroom_blocks_);
    }
    {   // Need from the arrival-gap histogram, the hub's per-player hold rule — the smallest
        // Need = the smallest gap H (ms) with at most SB_AOO_ADAPT_LATE_PER_MIN (2, the hub's budget) gaps per
        // minute above it, plus one block of margin (+2 ms on WAN, as the hub does). One burst does not move
        // the need; two a minute do.
        static const double late_per_min = std::max(0.0, sink_tap_int("SB_AOO_ADAPT_LATE_PER_MIN", 2, nullptr) * 1.0);
        const double window_s = 10.0 * adapt_hist_filled_ + std::max(0.0, now - adapt_hist_slice_t0_);
        // The histogram drives the target only once it holds >= 30 s, and always tolerates at least one gap,
        // so one launch gap in a short window cannot park the buffer at the ceiling.
        if (window_s >= 30.0 && adapt_block_s_ > 0){
            // LAN ZERO TOLERANCE. On the LAN profile the allowance is 0 late arrivals: the hold covers the worst
            // gap in the window + 1 block, and the need is grow-only for the session (the 60-s histogram forgets,
            // the need does not). SB_AOO_ADAPT_LAN_ZERO=0 selects the 2/min budget.
            // The join transient (launch clump) must never set a zero-tolerance need. Zero tolerance ARMS only
            // once the sink is warm and the 60-s histogram has rotated the transient out (warm-up +
            // SB_AOO_ADAPT_LAN_ZERO_ARM_S); the grow-only floor starts from the first need computed after arming,
            // not from anything seen before.
            static const double zero_arm_s = std::max(0.0, sink_tap_int("SB_AOO_ADAPT_LAN_ZERO_ARM_S", 60, nullptr) * 1.0);
            const bool lan_zero = adapt_lan_profile_ && adapt_lan_zero() && adapt_warm_ && adapt_warm_at_ >= 0
                                  && (now - adapt_warm_at_) >= zero_arm_s;
            if (lan_zero && !adapt_zero_armed_){
                // Arming is a fresh start: the histogram still holds the join transient's gaps, and pre-arming
                // strike grows (+3) sit in the headroom; both would be inherited by the zero-tolerance floor.
                adapt_zero_armed_ = true; adapt_zero_floor_ = 0; adapt_headroom_blocks_ = 0;
                for (int32_t sl = 0; sl < kAdaptHistSlices; ++sl) memset(adapt_hist_[sl], 0, sizeof(adapt_hist_[0]));
                adapt_hist_filled_ = 0; adapt_hist_slice_t0_ = now; adapt_hist_blocks_ = adapt_floor_blocks_;
                sink_evlog("adapt_lan_zero_armed", governor_target_blocks_);
            }
            const double allow = lan_zero ? 0.0 : std::max(1.0, late_per_min * window_s / 60.0);
            int32_t above[kAdaptHistBuckets + 1]; above[kAdaptHistBuckets] = 0;
            for (int32_t b = kAdaptHistBuckets - 1; b >= 0; --b){
                int32_t c = 0; for (int32_t sl = 0; sl < kAdaptHistSlices; ++sl) c += adapt_hist_[sl][b];
                above[b] = above[b + 1] + c;              // gaps of >= b ms
            }
            int32_t H = kAdaptHistBuckets - 1;            // gaps > H ms are the ones the buffer cannot cover
            for (int32_t h = 0; h < kAdaptHistBuckets - 1; ++h){ if (above[h + 1] <= allow){ H = h; break; } }
            const double margin_ms = adapt_block_s_ * 1e3 + (adapt_lan_profile_ ? 0.0 : 2.0);
            int32_t nb = (int32_t) std::ceil((H + margin_ms) / (adapt_block_s_ * 1e3));
            if (lan_zero){ nb = std::max<int32_t>(nb, adapt_zero_floor_); adapt_zero_floor_ = nb; }   // grow-only from the arming point
            if (nb != adapt_hist_blocks_){ adapt_hist_blocks_ = nb; sink_evlog("adapt_need", nb); }   // logged on change only
            adapt_base_blocks_ = std::max<int32_t>(adapt_floor_blocks_, adapt_hist_blocks_);
        } else if (adapt_ewma_max_ >= 0 && adapt_block_s_ > 0){   // first seconds only
            const double need_s = adapt_ewma_max_ + adapt_ewma_sd_ * (sd_pct / 100.0);
            adapt_base_blocks_ = (int32_t) std::ceil(need_s / adapt_block_s_) + 1;
        }
    }
    // headroom can never push past the max (else decay would be invisible for minutes)
    adapt_headroom_blocks_ = std::max<int32_t>(0, std::min<int32_t>(adapt_headroom_blocks_, adapt_max_blocks_ - adapt_base_blocks_));
    int32_t T = adapt_base_blocks_ + adapt_headroom_blocks_;
    T = std::max<int32_t>(adapt_floor_blocks_, std::min<int32_t>(T, adapt_max_blocks_));
    const int32_t cur = governor_target_blocks_;
    // Underrun-driven emergency grow, mirror of the hub's GROW-EMERGENCY. The strike rule needs two
    // CONSECUTIVE strike seconds (or 3 underruns in one second), but WAN stalls land one underrun every
    // few seconds. Once the sink's own underruns in a rolling 10-s window reach
    // SB_AOO_ADAPT_EMERG_UNDERRUNS_10S (2), grow AT ONCE, capped at the ceiling, at most one step per
    // SB_AOO_ADAPT_EMERG_GAP_S (10).
    // The step is MEASURED: grow to cover the worst late gap seen in the window + 1 block (new depth =
    // ceil(late/block) + 1); a pop with no measurable late gap (the block never arrived = loss) grows
    // exactly ONE block; the growth per 10-s window is capped at SB_AOO_ADAPT_EMERG_CAP_BLOCKS (4) so a
    // burst cannot run away. Sustained roughness is still the histogram grow's job.
    // The headroom is raised with it so the commit/decay logic sees the new depth as needed, and the
    // clean-minute count restarts here (decay only counts clean minutes after the last grow). WAN only;
    // the LAN profile keeps its 30-s rule.
    bool emerg_done = false;
    {
        // TWO underruns in the rolling 10 s, not one: one pop in a window is noise and would ratchet the
        // depth on a path with occasional pops; two means the depth really is short.
        static const int32_t emerg_thr   = std::max<int32_t>(1, sink_tap_int("SB_AOO_ADAPT_EMERG_UNDERRUNS_10S", 2, nullptr));
        static const int32_t emerg_gap_s = std::max<int32_t>(1, sink_tap_int("SB_AOO_ADAPT_EMERG_GAP_S", 10, nullptr));
        static const int32_t emerg_cap   = std::max<int32_t>(1, sink_tap_int("SB_AOO_ADAPT_EMERG_CAP_BLOCKS", 4, nullptr));
        if (!adapt_lan_profile_){
            if (now - adapt_emerg_win_t0_ >= 10.0){ adapt_emerg_win_t0_ = now; adapt_emerg_win_n_ = 0; adapt_emerg_win_grown_ = 0; adapt_emerg_late_ms_ = 0; }
            adapt_emerg_win_n_ += u;
            if (adapt_warm_ && adapt_emerg_win_n_ >= emerg_thr && (now - adapt_last_emerg_) >= (double) emerg_gap_s
                && cur < adapt_max_blocks_ && adapt_emerg_win_grown_ < emerg_cap){
                const double late_ms = adapt_emerg_late_ms_;
                int32_t want = cur + 1;                                   // unmeasured (loss): exactly one block
                if (late_ms > 0 && adapt_block_s_ > 0)
                    want = std::max<int32_t>(cur + 1, (int32_t) std::ceil(late_ms / (adapt_block_s_ * 1e3)) + 1);   // cover the gap + 1 block
                const int32_t room = emerg_cap - adapt_emerg_win_grown_;
                const int32_t E = std::min<int32_t>(adapt_max_blocks_, std::min<int32_t>(want, cur + room));
                adapt_emerg_win_grown_ += (E - cur);
                adapt_last_emerg_ = now; adapt_last_up_ = now;
                adapt_emerg_win_n_ = 0; adapt_emerg_late_ms_ = 0;         // the window itself keeps running (per-window cap)
                adapt_clean_secs_ = 0; adapt_stall_clean_secs_ = 0;
                adapt_headroom_blocks_ = std::max<int32_t>(adapt_headroom_blocks_, E - adapt_base_blocks_);
                adapt_headroom_prev_ = adapt_headroom_blocks_;   // not a 'budget grow' on top of this step
                T = std::max<int32_t>(adapt_floor_blocks_, std::min<int32_t>(adapt_base_blocks_ + adapt_headroom_blocks_, adapt_max_blocks_));
                adapt_apply_target(E);
                sink_evlog("adapt_grow_emergency", E);
                adapt_note(4, E, (float) late_ms);
                emerg_done = true;
            }
        }
    }
    // Stats-driven growth needs SB_AOO_ADAPT_UP_HYST_BLOCKS (2) of deficit, so it cannot flip-flop
    // against the 2-block shrink rule; a glitch-budget grow (headroom step) still applies immediately.
    static const int32_t up_hyst = std::max<int32_t>(1, sink_tap_int("SB_AOO_ADAPT_UP_HYST_BLOCKS", 2, nullptr));
    const bool budget_grow = adapt_headroom_blocks_ > adapt_headroom_prev_;
    adapt_headroom_prev_ = adapt_headroom_blocks_;
    static const int32_t grow_only = sink_tap_int("SB_AOO_ADAPT_GROW_ONLY", 1, nullptr);
    {   // Settle window (both directions) after warm-up, then grow-only.
        // Commit at the measured need + 1 block, not at whatever the launch period inflated the depth to.
        // One splice at the commit, before anyone is playing.
        static const int32_t settle_s_lan = std::max<int32_t>(0, sink_tap_int("SB_AOO_ADAPT_SETTLE_S", 30, nullptr));
        static const int32_t settle_s_wan = std::max<int32_t>(0, sink_tap_int("SB_AOO_ADAPT_WAN_SETTLE_S", 70, nullptr));   // WAN commits at ~75 s like the hub (5 s warm-up + 70 s)
        const int32_t settle_s = adapt_lan_profile_ ? settle_s_lan : settle_s_wan;
        if (!adapt_committed_ && adapt_warm_ && adapt_warm_at_ >= 0 && now - adapt_warm_at_ >= (double) settle_s){
            adapt_committed_ = true;
            const int32_t C = std::max<int32_t>(adapt_floor_blocks_, std::min<int32_t>(cur, T + 1));
            if (grow_only && C < cur){ adapt_last_shrink_ = now; adapt_apply_target(C); sink_evlog("adapt_commit_shrink", C); }
            adapt_commit_blocks_ = governor_target_blocks_;
            sink_evlog("adapt_commit", adapt_commit_blocks_);
            adapt_note(2, adapt_commit_blocks_);
        }
    }
    // Stats-driven growth is persistent and stepped, like the hub's hold: the need must sit above
    // cur + hysteresis for 3 consecutive seconds, then the depth grows at most ×1.33 (min +1 block) per 10 s.
    // A glitch-budget grow (headroom step after real underruns) still applies immediately.
    adapt_up_secs_ = (T >= cur + up_hyst) ? adapt_up_secs_ + 1 : 0;
    // For the first SB_AOO_ADAPT_QUICK_S (10) seconds after the warm-up a WAN
    // stream jumps straight to the measured need (no hysteresis, no stepping) — the fast climb from the 4-ms start.
    static const int32_t quick_s = std::max<int32_t>(0, sink_tap_int("SB_AOO_ADAPT_QUICK_S", 10, nullptr));
    const bool quick = !adapt_lan_profile_ && adapt_warm_ && adapt_warm_at_ >= 0 && (now - adapt_warm_at_) < (double) quick_s;
    if (emerg_done){
        // handled above this second
    } else if (T > cur && (budget_grow || quick)){
        if (quick && !budget_grow) sink_evlog("adapt_quick", T);
        adapt_stall_clean_secs_ = 0;
        adapt_apply_target(T);
        adapt_note(quick && !budget_grow ? 1 : 3, T);
    } else if (T > cur && adapt_up_secs_ >= 3 && (now - adapt_last_up_) >= 10.0){
        const int32_t step = std::max<int32_t>(cur + 1, (int32_t) std::ceil(cur * 1.33));
        adapt_last_up_ = now; adapt_stall_clean_secs_ = 0;
        adapt_apply_target(std::min<int32_t>(T, step));
        adapt_note(3, std::min<int32_t>(T, step));
    } else if (T <= cur - 2 && (now - adapt_last_shrink_) >= (double) shrink_hold_s && !(grow_only && adapt_committed_)){
        adapt_last_shrink_ = now;
        adapt_apply_target(T);   // Regular shrink (grow-only off, or still in the settle window)
    } else if (grow_only && adapt_committed_){
        // No automatic shrink after the commit: every shrink is an audible splice.
        // The regular shrink's target is logged instead (adapt_shadow_shrink).
        if (T <= cur - 2 && (now - adapt_last_shadow_) >= (double) shrink_hold_s){ adapt_last_shadow_ = now; sink_evlog("adapt_shadow_shrink", T); }   // Own clock, so it cannot starve the decay
        // Decay toward the MEASURED need. "Clean" = no STALL-class gap and no grow for
        // SB_AOO_ADAPT_DECAY_CLEAN_S (120 s; 0 turns decay off, pure grow-only); isolated pops do not reset
        // the wait, they only feed the histogram need. Never below need + SB_AOO_ADAPT_DECAY_MARGIN_BLOCKS (2).
        // A shrink is a splice, which is the reason for any wait at all.
        static const int32_t decay_clean_s = std::max<int32_t>(0, sink_tap_int("SB_AOO_ADAPT_DECAY_CLEAN_S", 120, nullptr));
        static const int32_t decay_margin  = std::max<int32_t>(0, sink_tap_int("SB_AOO_ADAPT_DECAY_MARGIN_BLOCKS", 2, nullptr));
        const int32_t Tfloor = std::min<int32_t>(adapt_max_blocks_, T + decay_margin);
        // One step of /1.33 (min one block) per SB_AOO_ADAPT_DECAY_MIN_GAP_S (120), never below
        // need + margin. WAN only; the LAN profile uses a 60-s halving rule.
        static const int32_t decay_gap_s = std::max<int32_t>(0, sink_tap_int("SB_AOO_ADAPT_DECAY_MIN_GAP_S", 120, nullptr));
        const double min_gap = adapt_lan_profile_ ? 60.0 : (double) decay_gap_s;
        const int32_t clean_now = adapt_lan_profile_ ? adapt_clean_secs_ : adapt_stall_clean_secs_;   // WAN = stall-free seconds
        if (decay_clean_s > 0 && cur > Tfloor && clean_now >= decay_clean_s && (now - adapt_last_shrink_) >= min_gap){
            const int32_t D = adapt_lan_profile_ ? std::max<int32_t>(Tfloor, cur - std::max<int32_t>(1, (cur - Tfloor) / 2))
                                                 : std::max<int32_t>(Tfloor, std::min<int32_t>(cur - 1, (int32_t) std::ceil(cur / 1.33)));
            adapt_last_shrink_ = now;
            adapt_apply_target(D);
            sink_evlog("adapt_guarded_decay", D);
            adapt_note(5, D, (float) (Tfloor * adapt_block_s_ * 1e3));   // The need (+margin) the step decayed toward
        }
    }
    if (trace >= 1){
        sink_evlog("adapt_jit_us", (int32_t) (std::max(0.0, adapt_ewma_max_) * 1e6));
        sink_evlog("adapt_sd_us", (int32_t) (adapt_ewma_sd_ * 1e6));
        sink_evlog("adapt_headroom", adapt_headroom_blocks_);
        sink_evlog("adapt_depth", governor_target_blocks_);
        sink_evlog("adapt_underruns", u);
    }
}

// adapt_apply_target(): change the committed depth WITHOUT a queue rebuild.
// UP: append PLC (concealment) blocks up to the new target — the same
// refill path an underrun recovery uses — and arm the fade at the seam.
// DOWN: lower the target and let the governor's v4 drain remove the excess in
// ONE splice on the next packet (adapt_shrink_pending_ bypasses dwell/rate).
// Decision ring for the client's universal auto log. Writer = the retarget (receive path), one
// slot per decision; the reader copies what it has not seen. Wall clock via system_clock (a vDSO read, decisions are rare).
void source_desc::adapt_note(int32_t kind, int32_t blocks, float late_ms){
    const uint32_t seq = adapt_ev_seq_.load(std::memory_order_relaxed) + 1;
    aoo_adapt_event& e = adapt_ev_[seq % 16];
    e.wall_s = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
    e.kind = kind; e.blocks = blocks; e.block_ms = (float) (adapt_block_s_ * 1e3); e.late_ms = late_ms; e.seq = seq;
    adapt_ev_seq_.store(seq, std::memory_order_release);
}
void source_desc::adapt_events(aoo_adapt_events& out) const {
    const uint32_t latest = adapt_ev_seq_.load(std::memory_order_acquire);
    out.latest = latest; out.count = 0;
    uint32_t first = out.since + 1;
    if (latest - first >= 16) first = latest - 15;                 // older entries were overwritten
    for (uint32_t q = first; q <= latest && out.count < AOO_ADAPT_EVENTS_MAX; ++q){
        if (q == 0) continue;
        const aoo_adapt_event& e = adapt_ev_[q % 16];
        if (e.seq != q) continue;                                   // torn/overwritten slot: skip
        out.ev[out.count++] = e;
    }
}

void source_desc::adapt_apply_target(int32_t T){
    const int32_t old = governor_target_blocks_;
    if (T == old || T <= 0) return;
    governor_target_blocks_ = T;
    depth_pub_.store(T, std::memory_order_relaxed);   
    audioqueue_target_samples_.store(T * (int32_t) audioqueue_.blocksize(), std::memory_order_relaxed);
    sink_evlog("adapt_target", T);
    if (T > old){
        const int32_t n = refill_silence_to_target(0);
        drift_silence_frames_ += (int64_t) n * audioqueue_.blocksize();   // A level move, not the sink running ahead
        nextneedsfadein_ = next_; sink_evlog("fade_arm", next_);
        sink_evlog("adapt_refill", n);
        adapt_shrink_pending_ = false;
    } else {
        adapt_shrink_pending_ = true;
    }
}

void source_desc::check_outdated_blocks(){
    // pop outdated blocks (shouldn't really happen...)
    while (!blockqueue_.empty() &&
           (newest_ - blockqueue_.front().sequence) >= blockqueue_.capacity())
    {
        auto old = blockqueue_.front().sequence;
        LOG_VERBOSE("pop outdated block " << old);
        // remove block from acklist
        ack_list_.remove(old);
        // pop block
        blockqueue_.pop_front();
        // update 'next'
        if (next_ <= old){
            next_ = old + 1;
        }
        // record dropped block
        streamstate_.add_lost(1);
        g_sbLostSite[6].fetch_add((uint64_t)(1), std::memory_order_relaxed);   // Per-reason lost counter (SB-DROPS)
    }
}

#define AOO_BLOCKQUEUE_CHECK_THRESHOLD 3

// deal with "holes" in block queue
void source_desc::check_missing_blocks(const sink& s){
    if (blockqueue_.empty()){
        if (!ack_list_.empty()){
            LOG_WARNING("bug: acklist not empty");
            ack_list_.clear();
        }
        return;
    }
    // don't check below a certain threshold,
    // because we might just experience packet reordering.
    // TODO find something better
    if (blockqueue_.size() < AOO_BLOCKQUEUE_CHECK_THRESHOLD){
        return;
    }
#if LOGLEVEL >= 4
    std::cerr << queue << std::endl;
#endif
    int32_t numframes = 0;

    // resend incomplete blocks except for the last block
    LOG_DEBUG("resend incomplete blocks");
    for (auto it = blockqueue_.begin(); it != (blockqueue_.end() - 1); ++it){
        if (!it->complete() && resendqueue_.write_available()){
            // insert ack (if needed)
            auto& ack = ack_list_.get(it->sequence);
            if (ack.update(s.elapsed_time(), s.resend_interval())){
                for (int i = 0; i < it->num_frames(); ++i){
                    if (!it->has_frame(i)){
                        if (numframes < s.resend_maxnumframes()){
                            resendqueue_.write(data_request { it->sequence, i });
                            numframes++;
                        } else {
                            goto resend_incomplete_done;
                        }
                    }
                }
            }
        }
    }
resend_incomplete_done:

    // resend missing blocks before any (half)completed blocks
    LOG_DEBUG("resend missing blocks");
    int32_t next = next_;
    for (auto it = blockqueue_.begin(); it != blockqueue_.end(); ++it){
        auto missing = it->sequence - next;
        if (missing > 0){
            for (int i = 0; i < missing && resendqueue_.write_available(); ++i){
                // insert ack (if necessary)
                auto& ack = ack_list_.get(next + i);
                if (ack.update(s.elapsed_time(), s.resend_interval())){
                    if (numframes + it->num_frames() <= s.resend_maxnumframes()){
                        resendqueue_.write(data_request { next + i, -1 }); // whole block
                        numframes += it->num_frames();
                    } else {
                        goto resend_missing_done;
                    }
                }
            }
        } else if (missing < 0){
            LOG_VERBOSE("bug: sequence = " << it->sequence << ", next = " << next);
            assert(false);
        }
        next = it->sequence + 1;
    }
resend_missing_done:

    assert(numframes <= s.resend_maxnumframes());
    if (numframes > 0){
        LOG_DEBUG("requested " << numframes << " frames");
    }

#if 1
    // clean ack list
    auto removed = ack_list_.remove_before(next_);
    if (removed > 0){
        LOG_DEBUG("block_ack_list: removed " << removed << " outdated items");
    }
#endif

#if LOGLEVEL >= 4
    std::cerr << acklist << std::endl;
#endif
}

// /aoo/src/<id>/format <version> <sink>

bool source_desc::send_format_request(const sink& s) {
    if (streamstate_.need_format()){
        LOG_VERBOSE("request format for source " << id_);
        char buf[AOO_MAXPACKETSIZE];
        osc::OutboundPacketStream msg(buf, sizeof(buf));

        // make OSC address pattern
        const int32_t max_addr_size = AOO_MSG_DOMAIN_LEN +
                AOO_MSG_SOURCE_LEN + 16 + AOO_MSG_FORMAT_LEN;
        char address[max_addr_size];
        snprintf(address, sizeof(address), "%s%s/%d%s",
                 AOO_MSG_DOMAIN, AOO_MSG_SOURCE, id_, AOO_MSG_FORMAT);

        msg << osc::BeginMessage(address) << s.id() << (int32_t)make_version(s.protocol_flags())
            << osc::EndMessage;

        dosend(msg.Data(), (int32_t)msg.Size());

        return true;
    } else {
        return false;
    }
}


// /aoo/src/<id>/codecchange <sink> <numchannels> <samplerate> <blocksize> <codec> <options...>

bool source_desc::send_codec_change_request(const sink& s) {
    if (streamstate_.need_codec_change()){
        LOG_VERBOSE("request change of codec for source " << id_);
        char buf[AOO_MAXPACKETSIZE];
        osc::OutboundPacketStream msg(buf, sizeof(buf));

        int32_t size = 0;
        const aoo_format_storage & f = streamstate_.get_codec_change_format(size);
        
        // make OSC address pattern
        const int32_t max_addr_size = AOO_MSG_DOMAIN_LEN +
                AOO_MSG_SOURCE_LEN + 16 + AOO_MSG_CODEC_CHANGE_LEN;
        char address[max_addr_size];
        snprintf(address, sizeof(address), "%s%s/%d%s",
                 AOO_MSG_DOMAIN, AOO_MSG_SOURCE, id_, AOO_MSG_CODEC_CHANGE);

        msg << osc::BeginMessage(address) << s.id() << f.header.nchannels << f.header.samplerate << f.header.blocksize << f.header.codec << osc::Blob(f.data, size)
            << osc::EndMessage;

        dosend(msg.Data(), (int32_t)msg.Size());

        return true;
    } else {
        return false;
    }
}

// /aoo/src/<id>/data <sink> <salt> <seq0> <frame0> <seq1> <frame1> ...

int32_t source_desc::send_data_request(const sink &s){
    // called without lock!
    shared_lock lock(mutex_);
    int32_t salt = salt_;
    lock.unlock();

    int32_t numrequests = 0;
    while ((numrequests = resendqueue_.read_available()) > 0){
        // send request messages
        char buf[AOO_MAXPACKETSIZE];
        osc::OutboundPacketStream msg(buf, sizeof(buf));

        // make OSC address pattern
        const int32_t maxaddrsize = AOO_MSG_DOMAIN_LEN +
                AOO_MSG_SOURCE_LEN + 16 + AOO_MSG_DATA_LEN;
        char address[maxaddrsize];
        snprintf(address, sizeof(address), "%s%s/%d%s",
                 AOO_MSG_DOMAIN, AOO_MSG_SOURCE, id_, AOO_MSG_DATA);

        const int32_t maxdatasize = s.packetsize() - maxaddrsize - 16; // id + salt + padding
        const int32_t maxrequests = maxdatasize / 10; // 2 * (int32_t + typetag)
        auto d = div(numrequests, maxrequests);

        auto dorequest = [&](int32_t n){
            msg << osc::BeginMessage(address) << s.id() << salt;
            while (n--){
                data_request request;
                resendqueue_.read(request);
                msg << request.sequence << request.frame;
            }
            msg << osc::EndMessage;

            dosend(msg.Data(), (int32_t)msg.Size());
        };

        for (int i = 0; i < d.quot; ++i){
            dorequest(maxrequests);
        }
        if (d.rem > 0){
            dorequest(d.rem);
        }
    }
    return numrequests;
}

// AoO/<id>/ping <sink>

bool source_desc::send_notifications(const sink& s){
    // called without lock!
    bool didsomething = false;

    time_tag pingtime1;
    time_tag pingtime2;
    if (streamstate_.need_ping(pingtime1, pingtime2)){
    #if 1
        // only send ping if source is active
        if (streamstate_.get_state() == AOO_SOURCE_STATE_PLAY){
    #else
        {
    #endif
            auto lost_blocks = streamstate_.get_lost_since_ping();

            char buffer[AOO_MAXPACKETSIZE];
            osc::OutboundPacketStream msg(buffer, sizeof(buffer));

            // make OSC address pattern
            const int32_t max_addr_size = AOO_MSG_DOMAIN_LEN
                    + AOO_MSG_SOURCE_LEN + 16 + AOO_MSG_PING_LEN;
            char address[max_addr_size];
            snprintf(address, sizeof(address), "%s%s/%d%s",
                     AOO_MSG_DOMAIN, AOO_MSG_SOURCE, id_, AOO_MSG_PING);

            msg << osc::BeginMessage(address) << s.id()
                << osc::TimeTag(pingtime1.to_uint64())
                << osc::TimeTag(pingtime2.to_uint64())
                << lost_blocks
                << osc::EndMessage;

            dosend(msg.Data(), (int32_t)msg.Size());

            LOG_DEBUG("send /ping to source " << id_);
            didsomething = true;
        }
    }

    auto invitation = streamstate_.get_invitation_state();
    if (invitation == stream_state::INVITE){
        char buffer[AOO_MAXPACKETSIZE];
        osc::OutboundPacketStream msg(buffer, sizeof(buffer));

        // make OSC address pattern
        const int32_t max_addr_size = AOO_MSG_DOMAIN_LEN
                + AOO_MSG_SOURCE_LEN + 16 + AOO_MSG_INVITE_LEN;
        char address[max_addr_size];
        snprintf(address, sizeof(address), "%s%s/%d%s",
                 AOO_MSG_DOMAIN, AOO_MSG_SOURCE, id_, AOO_MSG_INVITE);

        // The invite must carry OUR capabilities (the sink's flags), not the flags we last heard from
        // the remote source: echoing those back would undo the idle-mask handshake.
        msg << osc::BeginMessage(address) << s.id() << (int32_t) s.protocol_flags() << osc::EndMessage;

        dosend(msg.Data(), (int32_t)msg.Size());

        LOG_DEBUG("send /invite to source " << id_ << " flags: " << s.protocol_flags());

        didsomething = true;
    } else if (invitation == stream_state::UNINVITE){
        char buffer[AOO_MAXPACKETSIZE];
        osc::OutboundPacketStream msg(buffer, sizeof(buffer));

        // make OSC address pattern
        const int32_t max_addr_size = AOO_MSG_DOMAIN_LEN
                + AOO_MSG_SOURCE_LEN + 16 + AOO_MSG_UNINVITE_LEN;
        char address[max_addr_size];
        snprintf(address, sizeof(address), "%s%s/%d%s",
                 AOO_MSG_DOMAIN, AOO_MSG_SOURCE, id_, AOO_MSG_UNINVITE);

        msg << osc::BeginMessage(address) << s.id() << osc::EndMessage;

        dosend(msg.Data(), (int32_t)msg.Size());

        LOG_DEBUG("send /uninvite source " << id_);

        didsomething = true;
    }

    return didsomething;
}

} // aoo
