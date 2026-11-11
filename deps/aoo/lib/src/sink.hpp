/* Copyright (c) 2010-Now Christof Ressi, Winfried Ritsch and others. 
 * For information on usage and redistribution, and for a DISCLAIMER OF ALL
 * WARRANTIES, see the file, "LICENSE.txt," in this distribution.  */

#pragma once

#include <chrono>
#include <atomic>

#include "aoo/aoo.hpp"
#include "aoo/aoo_utils.hpp"

#include "time.hpp"
#include "sync.hpp"
#include "common.hpp"
#include "lockfree.hpp"
#include "time_dll.hpp"

#include "oscpack/osc/OscOutboundPacketStream.h"
#include "oscpack/osc/OscReceivedElements.h"

namespace aoo {

struct stream_state {
    stream_state() = default;
    stream_state(stream_state&& other) = delete;
    stream_state& operator=(stream_state&& other) = delete;

    void reset(){
        lost_ = 0;
        reordered_ = 0;
        resent_ = 0;
        gap_ = 0;
        state_ = AOO_SOURCE_STATE_STOP;
        underrun_ = false;
        recover_ = false;
        format_ = false;
        invite_ = NONE;
        pingtime1_ = 0;
        pingtime2_ = 0;
        codecchange_ = false;
    }

    void add_lost(int32_t n) { lost_ += n; lost_since_ping_ += n; }
    int32_t get_lost() { return lost_.exchange(0); }
    int32_t get_lost_since_ping() { return lost_since_ping_.exchange(0); }

    void add_reordered(int32_t n) { reordered_ += n; }
    int32_t get_reordered() { return reordered_.exchange(0); }

    void add_resent(int32_t n) { resent_ += n; }
    int32_t get_resent() { return resent_.exchange(0); }

    void add_gap(int32_t n) { gap_ += n; }
    int32_t get_gap() { return gap_.exchange(0); }

    bool update_state(aoo_source_state state){
        auto last = state_.exchange(state);
        return state != last;
    }
    aoo_source_state get_state(){
        return state_;
    }

    void set_ping(time_tag t1, time_tag t2){
        pingtime1_ = t1.to_uint64();
        pingtime2_ = t2.to_uint64();
    }

    bool need_ping(time_tag& t1, time_tag& t2){
        // check pingtime2 because it ensures that pingtime1 has been set
        auto pingtime2 = pingtime2_.exchange(0);
        if (pingtime2){
            t1 = pingtime1_.load();
            t2 = pingtime2;
            return true;
        } else {
            return false;
        }
    }

    void set_underrun() { underrun_ = true; }
    bool have_underrun() { return underrun_.exchange(false); }

    void request_recover() { recover_ = true; }
    bool need_recover() { return recover_.exchange(false); }

    void request_format() { format_ = true; }
    bool need_format() { return format_.exchange(false); }

    void request_codec_change(const aoo_format& f, const char *options, int32_t size) { 
        memcpy(&codecchange_format_.header, &f, sizeof(aoo_format));
        memcpy(codecchange_format_.data, options, size);
        codecchange_datasize_ = size;
        codecchange_ = true;           
    }
    bool need_codec_change() { return codecchange_.exchange(false); }

    aoo_format_storage &  get_codec_change_format(int32_t & datasize) { datasize = codecchange_datasize_; return codecchange_format_; }
    
    enum invitation_state {
        NONE = 0,
        INVITE = 1,
        UNINVITE = 2,
    };

    void request_invitation(invitation_state state) { invite_ = state; }
    invitation_state get_invitation_state() { return invite_.exchange(NONE); }
private:
    std::atomic<int32_t> lost_since_ping_{0};
    std::atomic<int32_t> lost_{0};
    std::atomic<int32_t> reordered_{0};
    std::atomic<int32_t> resent_{0};
    std::atomic<int32_t> gap_{0};
    std::atomic<aoo_source_state> state_{AOO_SOURCE_STATE_STOP};
    std::atomic<invitation_state> invite_{NONE};
    std::atomic<bool> underrun_{false};
    std::atomic<bool> recover_{false};
    std::atomic<bool> format_{false};
    std::atomic<bool> codecchange_{false};
    std::atomic<uint64_t> pingtime1_;
    std::atomic<uint64_t> pingtime2_;
    
    aoo_format_storage codecchange_format_;
    int32_t codecchange_datasize_ = 0;
};

struct block_info {
    double sr;
    int32_t channel;
};

class sink;

class source_desc {
public:
    typedef union event
    {
        aoo_event_type type;
        aoo_source_event source;
        aoo_ping_event ping;
        aoo_source_state_event source_state;
        aoo_block_lost_event block_loss;
        aoo_block_reordered_event block_reorder;
        aoo_block_resent_event block_resend;
        aoo_block_gap_event block_gap;
    } event;

    source_desc(void *endpoint, aoo_replyfn fn, int32_t id, int32_t salt);
    source_desc(const source_desc& other) = delete;
    source_desc& operator=(const source_desc& other) = delete;

    // getters
    int32_t id() const { return id_; }

    void *endpoint() const { return endpoint_; }

    bool has_events() const { return  eventqueue_.read_available() > 0; }

    int32_t get_format(aoo_format_storage& format);
    
    int32_t get_buffer_fill_ratio(float &ratio);
    int32_t get_resampler_backlog(float &samples);

    int32_t get_userformat(char * buf, int32_t size);
    int32_t get_newest_seq() const { return newest_pub_.load(std::memory_order_relaxed); }
    int32_t get_depth_blocks() const { return depth_pub_.load(std::memory_order_relaxed); }
    int32_t get_rx_underruns() const { return (int32_t) rx_underruns_pub_.load(std::memory_order_relaxed); }   
    // Adaptive-depth decision ring for the client's universal auto log (aoo_opt_adapt_events).
    void adapt_events(aoo_adapt_events& out) const;
    int32_t get_rx_pops() const { return (int32_t) rx_pops_pub_.load(std::memory_order_relaxed); }   // Current depth target (blocks), lock-free   // lock-free

    int32_t get_current_salt() const { return salt_; }
    
    void set_protocol_flags(int32_t flags) { protocol_flags_ = flags; }
    int32_t protocol_flags() const { return protocol_flags_; }   
    
    // methods
    void update(const sink& s);

    int32_t handle_format(const sink& s, int32_t salt, const aoo_format& f,
                          const char *settings, int32_t size, int32_t version, const char *userformat=nullptr, int32_t ufsize=0);

    int32_t handle_data(const sink& s, int32_t salt,
                                     const aoo::data_packet& d);
    // Expand a masked blob to a full PCM block (bytes), then the codec decodes as usual
    int32_t decode_masked(const char *data, int32_t size, aoo_sample *out, int32_t nsamples);
    std::vector<char> maskexp_;        // network thread scratch
    int32_t mask_samplesize_ = 2;      // last seen bytes/sample (PCM 16 default) for all-idle blocks
    bool mask_req_done_ = false; int32_t mask_req_salt_ = 0;   // one capability request per stream salt
    std::atomic<uint64_t> masked_blocks_rx_{0};
    std::atomic<uint64_t> rx_bytes_{0};   // Wire bytes of this stream (data packets), see aoo_opt_rx_bytes

    int32_t handle_ping(const sink& s, time_tag tt);

    int32_t handle_events(aoo_eventhandler fn, void *user);

    bool send(const sink& s);

    bool process(const sink& s, aoo_sample *buffer, int32_t stride, int32_t numsampleframes);

    void request_recover(){ streamstate_.request_recover(); }

    void request_format(){ streamstate_.request_format(); }

    void request_codec_change(aoo_format & f);

    void request_invite(){ streamstate_.request_invitation(stream_state::INVITE); }

    void request_uninvite(){ streamstate_.request_invitation(stream_state::UNINVITE); }
private:
    struct data_request {
        int32_t sequence;
        int32_t frame;
    };
    void do_update(const sink& s);
    // handle messages
    bool check_packet(const data_packet& d);

    bool add_packet(const data_packet& d);

    void process_blocks(const sink& s);

    // Network-thread helpers, see sink.cpp.
    // Refill the audioqueue with silence up to (target - headroom) blocks.
    // The queue's capacity is not the target, so every "fill to full" loop
    // goes through here. Returns the number of blocks written.
    int32_t refill_silence_to_target(int32_t headroom_blocks);
    // Advance next_ by n sequence numbers (blocks that have NOT arrived yet
    // are discarded on arrival by check_packet), pop anything queued below
    // the new next_, arm the fade splice. The depth reduction happens as the
    // skipped sequences arrive, one per block period, because the excess
    // lives in the audioqueue, which only its consumer can drain.
    void skip_ahead(int32_t n);

    void check_outdated_blocks();

    void check_missing_blocks(const sink& s);
    // send messages
    bool send_format_request(const sink& s);
    bool send_codec_change_request(const sink& s);

    int32_t send_data_request(const sink& s);

    bool send_notifications(const sink& s);

    void dosend(const char *data, int32_t n){
        fn_(endpoint_, data, n);
    }
    // data
    void * const endpoint_;
    const aoo_replyfn fn_;
    const int32_t id_;
    int32_t salt_;
    // audio decoder
    std::unique_ptr<aoo::decoder> decoder_;
    // state
    int32_t newest_ = 0; // sequence number of most recent incoming block
    std::atomic<int32_t> newest_pub_{0};   // Published mirror of newest_ (read by the hub's clock-lock)
    std::atomic<int32_t> depth_pub_{ 0 };   // Mirrors governor_target_blocks_ for readers off the audio thread
    std::atomic<uint32_t> rx_underruns_pub_{ 0 };   // Cumulative recovery_underrun count (audio thread, relaxed)
    std::atomic<uint32_t> rx_pops_pub_{ 0 };        // Cumulative governor_pop count
    int32_t next_ = 0; // next outgoing block
    int32_t nextneedsfadein_ = -1; // sequence number that needs fadein
    int32_t channel_ = 0; // recent channel onset
    double samplerate_ = 0; // recent samplerate
    int32_t protocol_flags_ = 0; // protocol flags sent from the remote source
    // Committed/manual buffer target in
    // BLOCKS — captured in do_update() from s.buffersize() (the value the
    // client sets via aoo_opt_buffersize / set_buffersize), i.e. the same
    // plumbing the audioqueue sizing already rides. Network thread only.
    int32_t governor_target_blocks_ = 0;
    double governor_last_drain_time_ = -1e9; // rate limiter (seconds, sink elapsed_time)
    double governor_last_depth_log_ = -1e9;  // SB_AOO_DEPTH_TRACE once-per-second depth log
    std::atomic<uint32_t> trace_calls_{0}, trace_reads_{0}, trace_moves_{0}, trace_dry_{0};   // Audio-thread counters for the trace (relaxed, 1 add each)
    bool governor_armed_ = false;            // hysteresis latch
    // Target-scaled thresholds, all computed in do_update() (rebuild time,
    // writer lock) and only READ on the network thread. Plain PODs, no
    // allocation, never touched by the audio thread.
    bool    governor_v4_ = false;            // SB_AOO_GOV_V4 != 0 (default on); false = v3 exactly
    bool    governor_v4_logged_ = false;     // 'governor_v4' evlog emitted once per rebuild
    int32_t governor_slack_blocks_ = 0;      // blocks of committed depth ABOVE the ms pin (target includes it)
    int32_t iso_blocks_ = 0;                 // SB_AOO_ISO_BLOCKS > 0 = LAN-isochronous: FIXED depth in blocks, no slack, ms ignored
    // ─── WAN ADAPTIVE receive depth (aoo_opt_adapt / SB_AOO_ADAPT_*) ───
    // JackTrip-Regulator-style policy:
    // per-packet arrival lateness vs a linear schedule -> 125 ms window max/sd ->
    // asymmetric EWMA (fast up, tau down) -> target every 1 s; grow = PLC refill,
    // shrink = one governor splice; glitch-budget headroom with decay. ALL of it
    // runs on the network thread inside handle_data(); plain PODs, no allocation,
    // never touched by the audio thread.
    bool    adapt_on_ = false;
    int32_t adapt_floor_blocks_ = 0, adapt_max_blocks_ = 0;
    int32_t adapt_headroom_blocks_ = 0;      // glitch-budget headroom (blocks), decays when clean
    int32_t adapt_headroom_prev_ = 0;        // headroom at the previous retarget (detects a budget grow)
    bool    adapt_warm_ = false;             // warm-up (launch storm) elapsed; stats re-anchored
    double  adapt_warm_at_ = -1;             // When warm-up ended (settle window starts)
    bool    adapt_committed_ = false;        // Settle window over -> grow-only
    int32_t adapt_commit_blocks_ = 0;        // Depth at commit (after the one commit-time shrink)
    double  adapt_last_shadow_ = -1e9;       // Shadow-shrink log pacing (must not touch adapt_last_shrink_)
    // The hub's per-player hold model, in the player: a 60-s histogram of arrival gaps
    // (6 × 10-s slices, 1-ms buckets 0..64 + overflow) and a budget of late blocks per minute. "Need" = the
    // smallest gap H with at most <budget> gaps/min above it, + margin. A single burst cannot push it to the ceiling.
    static constexpr int32_t kAdaptHistSlices = 6, kAdaptHistBuckets = 66;   // bucket 65 = >= 65 ms
    uint16_t adapt_hist_[kAdaptHistSlices][kAdaptHistBuckets] = {};
    int32_t adapt_hist_slice_ = 0, adapt_hist_filled_ = 0;
    double  adapt_hist_slice_t0_ = -1, adapt_last_arrival_ = -1;
    int32_t adapt_last_seq_ = -1;
    bool    adapt_zero_armed_ = false;   // LAN zero tolerance armed (after warm-up + arm window)
    int32_t adapt_zero_floor_ = 0;       // Grow-only need floor since arming   // sequence of the last fresh block binned
    int32_t adapt_hist_blocks_ = 0;          // latest histogram-derived need (blocks), for logging
    // STALL vs MODERATE split, the hub's rule. A gap longer than max(2 x depth, depth + 10 ms) is a
    // STALL: it is kept out of the need histogram unless stalls RECUR (> 2/min over the last 2 min);
    // moderate lateness alone drives the regular grow. Stall arrival times, last 32.
    static constexpr int32_t kAdaptStallRing = 32;
    double  adapt_stall_ts_[kAdaptStallRing] = {};  int32_t adapt_stall_idx_ = 0, adapt_stall_n_ = 0;
    int32_t adapt_stalls_excluded_ = 0;      // stalls kept out of the histogram since the last reset (logging)
    int32_t adapt_up_secs_ = 0;              // Consecutive seconds the need sat above cur + hysteresis
    double  adapt_last_up_ = -1e9;           // Last stats-driven growth step (hub-style ×1.33 per 10 s)
    // ─── DRIFT ASRC (SB_AOO_DRIFT_ASRC, default on): for hosts the word clock can't reach ───
    // Network thread estimates the source-vs-sink rate mismatch from sequence advance vs consumed
    // frames, one step per window, and publishes a ratio the audio thread applies in the resampler
    // (HQ sinc). Skew needs no splices; the governor stays for bursts. State is plain PODs + one
    // atomic; nothing else touches the audio thread.
    bool    drift_on_ = false;
    bool    drift_no_drain_ = false;        // SB_AOO_DRIFT_NO_DRAIN (default 0): governor drains stay on under the ASRC
    double  drift_last_t_ = -1;  int32_t drift_last_depth_ = 0;
    double  drift_ewma_bps_ = 0;             // blocks/s the depth grows (+ = source faster than sink)
    double  drift_level_ewma_ = -1;          // slow depth average (blocks)
    double  drift_ppm_ = 0;                  // applied correction (ppm of source rate)
    int32_t drift_dirty_sec_ = 0;            // drains/recoveries this second -> skip the estimator
    // V2: windowed least-squares slope (one correction per window, hub-lock style)
    double  drift_win_t0_ = -1;  int32_t drift_n_ = 0;
    int32_t drift_win_drained_ = 0;   // (v2.1, unused by v2.2 — kept for the evlog)
    // Rate from sequence vs consumption (the hub clock-lock method): immune to drains, refills and jitter bursts
    int64_t drift_frames_ = 0;        // sink frames consumed for this source (process() calls × frames)
    bool    adapt_lan_profile_ = false;   // LAN adaptive profile (floor 3 / max 12 blocks, warm-up 15 s)
    double  adapt_strike_win_t0_ = -1e9;  int32_t adapt_strike_win_n_ = 0;   // LAN rolling 30-s strike window
    bool    refill_drained_ = true;       // The one-shot overshoot drain for the last refill has fired
    std::atomic<int32_t> refill_pending_{0};   // A silence refill just happened (network thread) → the governor drains any overshoot at once
    double  refill_seen_t_ = -1e9;             // audio thread: when it first saw the flag
    int32_t drift_seq0_ = 0;  int64_t drift_frames0_ = 0;
    // Trace-only tallies of deliberate level moves: silence added by adaptive grows, blocks removed by
    // adaptive shrinks. Logged per window; the rate estimate does not use them.
    int64_t drift_silence_frames_ = 0;  int64_t drift_silence0_ = 0;
    int64_t drift_frames_entry_ = 0;    int64_t drift_fentry0_ = 0;
    int32_t drift_applied_windows_ = 0;  // Windows since (re)start; the first is discarded. drift_frames_entry_ counts frames at process() entry, including starved calls
    int64_t drift_skipped_blocks_ = 0;  int64_t drift_skipped0_ = 0;
    double  drift_sx_ = 0, drift_sy_ = 0, drift_sxx_ = 0, drift_sxy_ = 0;
    std::atomic<double> drift_ratio_{1.0};   // read by process(): source rate multiplier
    void drift_tick(const sink& s, double now, int32_t depth, int32_t target);
    int32_t adapt_base_blocks_ = 0;          // stats-derived depth (blocks) before headroom
    double  adapt_block_s_ = 0;              // nominal block period (s)
    double  adapt_t0_ = -1;  int32_t adapt_seq0_ = 0;   // arrival schedule anchor
    double  adapt_run_min_ = 0;              // running minimum lateness (s): re-anchors down, creeps up (clock drift)
    double  adapt_win_start_ = -1;
    double  adapt_win_min_ = 0, adapt_win_max_ = 0, adapt_win_sum_ = 0, adapt_win_sumsq_ = 0;
    int32_t adapt_win_n_ = 0;
    double  adapt_ewma_max_ = -1, adapt_ewma_sd_ = 0;   // s; ewma_max < 0 = no window yet
    double  adapt_last_retarget_ = -1, adapt_last_shrink_ = -1e9;
    int32_t adapt_underruns_sec_ = 0, adapt_strike_secs_ = 0, adapt_clean_secs_ = 0;
    int32_t adapt_stall_clean_secs_ = 0;   // Seconds since the last STALL-class gap or any grow (WAN); isolated pops do not reset it
    // Underrun-driven EMERGENCY grow.
    double  adapt_emerg_win_t0_ = -1e9;  int32_t adapt_emerg_win_n_ = 0;  double adapt_last_emerg_ = -1e9;
    double  adapt_emerg_late_ms_ = 0;    int32_t adapt_emerg_win_grown_ = 0;   // Worst late gap in the window; blocks grown in the window
    // decision ring (writer: the retarget on the receive path; reader: the client's health tick via get_sourceoption)
    aoo_adapt_event adapt_ev_[16] = {};  std::atomic<uint32_t> adapt_ev_seq_{0};
    void adapt_note(int32_t kind, int32_t blocks, float late_ms = 0.0f);
    bool    adapt_shrink_pending_ = false;   // governor drains the excess in ONE splice on the next packet
    // Fast recovery after a line storm (WAN profile, SB_AOO_ADAPT_FAST_RECOVER=1).
    std::atomic<bool> adapt_glide_active_{false};   // the excess above the target is being played out slightly fast, not spliced
    double  adapt_glide_g_ = 0;              // current speed-up (audio thread only)
    int32_t adapt_fast_wait_s_ = 0;          // stall-free seconds required before the next fast decision (doubles on a relapse)
    double  adapt_fast_last_t_ = -1e9;       // time of the last fast decision
    int32_t adapt_fast_depth_ = 0;           // depth that decision chose
    bool    adapt_glide_request_ = false;    // set by the retarget just before a shrink that should glide
    void adapt_track(const sink& s, const data_packet& d, bool fresh);
    void adapt_retarget(const sink& s, double now);
    void adapt_apply_target(int32_t target);
    int32_t governor_arm_blocks_ = 2;        // H: arm when depth > target + H
    int32_t governor_emerg_lead_ = 5;        // E: overflow emergency when completed excess >= E
    double  governor_armed_since_ = 0;       // dwell start (seconds, sink elapsed_time)
    double  governor_last_emerg_time_ = -1e9; // emergency-drain rate limiter (seconds)
    // The jitter-buffer target in samples, decoupled from audioqueue_.capacity()
    // (which is target + blockqueue capacity, so completed blocks are never parked
    // out of the audio thread's reach). Written in do_update() under the writer
    // lock; read lock-free by get_buffer_fill_ratio() (audio thread + hub sampler):
    // one relaxed atomic load.
    std::atomic<int32_t> audioqueue_target_samples_{0};
    stream_state streamstate_;
    std::vector<char> userformat_;
    // queues and buffers
    block_queue blockqueue_;
    block_ack_list ack_list_;
    lockfree::queue<aoo_sample> audioqueue_;
    lockfree::queue<block_info> infoqueue_;
    lockfree::queue<data_request> resendqueue_;
    lockfree::queue<event> eventqueue_;
    spinlock eventqueuelock_;
    void push_event(const event& e){
        scoped_lock<spinlock> l(eventqueuelock_);
        if (eventqueue_.write_available()){
            eventqueue_.write(e);
        }
    }
    dynamic_resampler resampler_;
    // thread synchronization
    aoo::shared_mutex mutex_; // LATER replace with a spinlock?
};

class sink final : public isink {
public:
    sink(int32_t id)
        : id_(id) {}

    ~sink(){}

    int32_t setup(int32_t samplerate, int32_t blocksize, int32_t nchannels) override;

    int32_t invite_source(void *endpoint, int32_t id, aoo_replyfn fn) override;

    int32_t uninvite_source(void *endpoint, int32_t id, aoo_replyfn fn) override;
                             
    int32_t uninvite_all() override;

    int32_t handle_message(const char *data, int32_t n,
                           void *endpoint, aoo_replyfn fn) override;

    int32_t send() override;

    int32_t process(aoo_sample **data, int32_t nsampframes, uint64_t t) override;

    int32_t events_available() override;

    int32_t handle_events(aoo_eventhandler fn, void *user) override;

    int32_t set_option(int32_t opt, void *ptr, int32_t size) override;

    int32_t get_option(int32_t opt, void *ptr, int32_t size) override;

    int32_t set_sourceoption(void *endpoint, int32_t id,
                             int32_t opt, void *ptr, int32_t size) override;

    int32_t get_sourceoption(void *endpoint, int32_t id,
                             int32_t opt, void *ptr, int32_t size) override;
                             
    int32_t request_source_codec_change(void *endpoint, int32_t id, aoo_format & f) override;

    int32_t request_source_format(void *endpoint, int32_t id) override;


    // getters
    int32_t id() const { return id_.load(std::memory_order_relaxed); }

    int32_t nchannels() const { return nchannels_; }

    int32_t samplerate() const { return samplerate_; }

    double real_samplerate() const { return ignore_dll_ ? samplerate_ : dll_.samplerate(); }

    int32_t blocksize() const { return blocksize_; }

    int32_t buffersize() const { return buffersize_; }
    int32_t iso_blocks() const { return iso_blocks_.load(std::memory_order_relaxed); }   // Per-sink (= per peer) iso depth, 0 = off
    int32_t adapt() const { return adapt_.load(std::memory_order_relaxed); }             // Per-sink adaptive depth, 0 = off, 1 = WAN profile, 2 = LAN profile
    bool dynamic_resampling_on() const { return dynamic_resampling_.load(std::memory_order_relaxed) != 0; }   // Trace

    int32_t packetsize() const { return packetsize_; }

    float resend_interval() const { return resend_interval_; }

    int32_t resend_limit() const { return resend_limit_; }

    int32_t resend_maxnumframes() const { return resend_maxnumframes_; }

    double elapsed_time() const { return timer_.get_elapsed(); }
    // Audio-clock time of the last source add / format rebuild in this sink; strike grows
    // are suppressed for SB_AOO_ADAPT_JOIN_GUARD_S after it (a hub feed rebuild at a join is a transient, not the link).
    void note_source_change() const { last_source_change_.store(timer_.get_elapsed(), std::memory_order_relaxed); }
    double last_source_change() const { return last_source_change_.load(std::memory_order_relaxed); }
    mutable std::atomic<double> last_source_change_{-1e9};

    time_tag absolute_time() const { return timer_.get_absolute(); }

    int32_t protocol_flags() const { return protocol_flags_; }

private:
    // settings
    std::atomic<int32_t> id_;
    int32_t nchannels_ = 0;
    int32_t samplerate_ = 0;
    int32_t blocksize_ = 0;
    // buffer for summing source audio output
    std::vector<aoo_sample> buffer_;
    // options
    std::atomic<int32_t> buffersize_{ AOO_SINK_BUFSIZE };
    std::atomic<int32_t> iso_blocks_{ 0 };   // Aoo_opt_iso_blocks (set by SonoBus for LAN peers only)
    std::atomic<int32_t> adapt_{ 0 };        // Aoo_opt_adapt (1 = WAN profile; 2 = LAN profile)
    std::atomic<int32_t> packetsize_{ AOO_PACKETSIZE };
    std::atomic<int32_t> resend_limit_{ AOO_RESEND_LIMIT };
    std::atomic<float> resend_interval_{ AOO_RESEND_INTERVAL * 0.001 };
    std::atomic<int32_t> resend_maxnumframes_{ AOO_RESEND_MAXNUMFRAMES };
    std::atomic<int32_t> protocol_flags_{ 0 };
    // the sources
    lockfree::list<source_desc> sources_;
    // timing
    std::atomic<int32_t> dynamic_resampling_{ 1 };
    std::atomic<float> bandwidth_{ AOO_TIMEFILTER_BANDWIDTH };
    time_dll dll_;
    bool ignore_dll_ = false;
    timer timer_;
    // helper methods
    source_desc *find_source(void *endpoint, int32_t id);
    source_desc *find_source_by_salt(void *endpoint, int32_t salt);

    void update_sources();

    int32_t handle_format_message(void *endpoint, aoo_replyfn fn,
                                  const osc::ReceivedMessage& msg);

    int32_t handle_data_message(void *endpoint, aoo_replyfn fn,
                                const osc::ReceivedMessage& msg);

    int32_t handle_compact_data_message(void *endpoint, aoo_replyfn fn,
                                        const osc::ReceivedMessage& msg);

    int32_t handle_ping_message(void *endpoint, aoo_replyfn fn,
                                const osc::ReceivedMessage& msg);
};

} // aoo
