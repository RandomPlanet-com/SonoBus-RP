/* Copyright (c) 2010-Now Christof Ressi, Winfried Ritsch and others. 
 * For information on usage and redistribution, and for a DISCLAIMER OF ALL
 * WARRANTIES, see the file, "LICENSE.txt," in this distribution.  */

#include "source.hpp"
#include "aoo/aoo_utils.hpp"

#include <cstring>
#include <algorithm>
#include <random>
#include <cmath>

/*//////////////////// AoO source /////////////////////*/

#define AOO_DATA_HEADERSIZE 80
// address pattern string: max 32 bytes
// typetag string: max. 12 bytes
// args (without blob data): 36 bytes

aoo_source * aoo_source_new(int32_t id) {
    return new aoo::source(id);
}

aoo::source::source(int32_t id)
    : id_(id)
{
    // event queue
    eventqueue_.resize(AOO_EVENTQUEUESIZE, 1);
    // request queues
    formatrequestqueue_.resize(64, 1);
    datarequestqueue_.resize(1024, 1);
}

void aoo_source_free(aoo_source *src){
    // cast to correct type because base class
    // has no virtual destructor!
    delete static_cast<aoo::source *>(src);
}

aoo::source::~source() {}

template<typename T>
T& as(void *p){
    return *reinterpret_cast<T *>(p);
}

#define CHECKARG(type) assert(size == sizeof(type))

int32_t aoo_source_set_option(aoo_source *src, int32_t opt, void *p, int32_t size)
{
    return src->set_option(opt, p, size);
}

int32_t aoo::source::set_option(int32_t opt, void *ptr, int32_t size)
{
    switch (opt){
    // id
    case aoo_opt_id:
    {
        auto newid = as<int32_t>(ptr);
        if (id_.exchange(newid) != newid){
            unique_lock lock(update_mutex_); // writer lock!
            update();
        }
        break;
    }
    // stop
    case aoo_opt_stop:
        play_ = false;
        break;
    // resume
    case aoo_opt_start:
    {
        // Start on a source that is already playing must not call update: a new salt + sequence reset
        // is a full stream restart, so every sink would dump the history buffer (an emergency drain, a
        // click) and the hub's inbound sink would re-request the format. The client calls start() from
        // several housekeeping paths (send-allow/mute restore, hub hello/join retries, peer connect).
        // Only a stopped source needs update().
        if (!play_.load()) {
            unique_lock lock(update_mutex_); // writer lock!
            update();
            play_ = true;
        }
        break;
    }
    // format
    case aoo_opt_format:
        CHECKARG(aoo_format);
        return set_format(as<aoo_format>(ptr));
    // buffersize
    case aoo_opt_buffersize:
    {
        CHECKARG(int32_t);
        auto bufsize = std::max<int32_t>(as<int32_t>(ptr), 0);
        if (bufsize != buffersize_){
            buffersize_ = bufsize;
            unique_lock lock(update_mutex_); // writer lock!
            update();
        }
        break;
    }
    // packetsize
    case aoo_opt_packetsize:
    {
        CHECKARG(int32_t);
        const int32_t minpacketsize = AOO_DATA_HEADERSIZE + 64;
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
    // dynamic resampling
    case aoo_opt_dynamic_resampling:
        CHECKARG(int32_t);
        dynamic_resampling_ = std::max<int32_t>(0, as<int32_t>(ptr));
        break;
    // timefilter bandwidth
    case aoo_opt_timefilter_bandwidth:
        CHECKARG(float);
        // time filter
        bandwidth_ = as<float>(ptr);
        timer_.reset(); // will update
        break;
    // ping interval
    case aoo_opt_ping_interval:
        CHECKARG(int32_t);
        ping_interval_ = std::max<int32_t>(0, as<int32_t>(ptr)) * 0.001;
        break;
    // resend buffer size
    case aoo_opt_resend_buffersize:
    {
        CHECKARG(int32_t);
        // empty buffer is allowed! (no resending)
        auto bufsize = std::max<int32_t>(as<int32_t>(ptr), 0);
        if (bufsize != resend_buffersize_){
            resend_buffersize_ = bufsize;
            unique_lock lock(update_mutex_); // writer lock!
            update_historybuffer();
        }
        break;
    }
    // ping interval
    case aoo_opt_redundancy:
        CHECKARG(int32_t);
        // limit it somehow, 16 times is already very high
        redundancy_ = std::max<int32_t>(1, std::min<int32_t>(16, as<int32_t>(ptr)));
        break;
    case aoo_opt_respect_codec_change_requests:
        CHECKARG(int32_t);
        respect_codec_change_req_ = as<int32_t>(ptr);
        break;
    
    case aoo_opt_idle_mask:
        CHECKARG(int32_t);
        idle_mask_ = as<int32_t>(ptr) ? 1 : 0;
        masking_dirty_ = true;
        break;
    // Synthetic-clock source (hub feed sources only).
    // Relaxed atomic; read once per process() call. See aoo.h for semantics.
    case aoo_opt_synthetic_clock:
        CHECKARG(int32_t);
        synthetic_clock_.store(as<int32_t>(ptr) != 0 ? 1 : 0,
                               std::memory_order_relaxed);
        break;
    // format
    case aoo_opt_userformat:
        return set_userformat(ptr, size);
    // unknown
    default:
        LOG_WARNING("aoo_source: unsupported option " << opt);
        return 0;
    }
    return 1;
}

int32_t aoo_source_get_option(aoo_source *src, int32_t opt, void *p, int32_t size)
{
    return src->get_option(opt, p, size);
}

int32_t aoo::source::get_option(int32_t opt, void *ptr, int32_t size)
{
    switch (opt){
    // id
    case aoo_opt_id:
        CHECKARG(int32_t);
        as<int32_t>(ptr) = id();
        break;
    // format
    case aoo_opt_format:
        CHECKARG(aoo_format_storage);
        if (encoder_){
            shared_lock lock(update_mutex_); // read lock!
            return encoder_->get_format(as<aoo_format_storage>(ptr));
        } else {
            return 0;
        }
        break;
    // buffer size
    case aoo_opt_buffersize:
        CHECKARG(int32_t);
        as<int32_t>(ptr) = buffersize_;
        break;
    // time filter bandwidth
    case aoo_opt_timefilter_bandwidth:
        CHECKARG(float);
        as<float>(ptr) = bandwidth_;
        break;
    // resend buffer size
    case aoo_opt_resend_buffersize:
        CHECKARG(int32_t);
        as<int32_t>(ptr) = resend_buffersize_;
        break;
    // packetsize
    case aoo_opt_packetsize:
        CHECKARG(int32_t);
        as<int32_t>(ptr) = packetsize_;
        break;
    // ping interval
    case aoo_opt_ping_interval:
        CHECKARG(int32_t);
        as<int32_t>(ptr) = ping_interval_ * 1000;
        break;
    // ping interval
    case aoo_opt_redundancy:
        CHECKARG(int32_t);
        as<int32_t>(ptr) = redundancy_;
        break;
    case aoo_opt_idle_mask:
        CHECKARG(int32_t);
        as<int32_t>(ptr) = idle_mask_;
        break;
    // Synthetic-clock flag (symmetry with set_option)
    case aoo_opt_synthetic_clock:
        CHECKARG(int32_t);
        as<int32_t>(ptr) = synthetic_clock_.load(std::memory_order_relaxed);
        break;
    // unknown
    default:
        LOG_WARNING("aoo_source: unsupported option " << opt);
        return 0;
    }
    return 1;
}

int32_t aoo_source_set_sinkoption(aoo_source *src, void *endpoint, int32_t id,
                              int32_t opt, void *p, int32_t size)
{
    return src->set_sinkoption(endpoint, id, opt, p, size);
}

int32_t aoo::source::set_sinkoption(void *endpoint, int32_t id,
                                   int32_t opt, void *ptr, int32_t size)
{
    if (id == AOO_ID_WILDCARD){
        // set option on all sinks on the given endpoint
        switch (opt){
        // channel onset
        case aoo_opt_channelonset:
        {
            CHECKARG(int32_t);
            auto chn = as<int32_t>(ptr);
            shared_lock lock(sink_mutex_); // reader lock!
            for (auto& sink : sinks_){
                if (sink.user == endpoint){
                    sink.channel = chn;
                }
            }
            LOG_VERBOSE("aoo_source: send to all sinks on channel " << chn);
            break;
        }
        // unknown
        default:
            LOG_WARNING("aoo_source: unsupported sink option " << opt);
            return 0;
        }
        return 1;
    } else {
        shared_lock lock(sink_mutex_); // reader lock!
        auto sink = find_sink(endpoint, id);
        if (sink){
            if (sink->id == AOO_ID_WILDCARD){
                LOG_WARNING("aoo_source: can't set individual sink option "
                            << opt << " because of wildcard");
                return 0;
            }

            switch (opt){
            // channel onset
            case aoo_opt_channelonset:
            {
                CHECKARG(int32_t);
                auto chn = as<int32_t>(ptr);
                sink->channel = chn;
                LOG_VERBOSE("aoo_source: send to sink " << sink->id
                            << " on channel " << chn);
                break;
            }
            case aoo_opt_protocol_flags:
            {
                CHECKARG(int32_t);
                auto flags = as<int32_t>(ptr);
                sink->protocol_flags = flags;
                masking_dirty_ = true;   // A sink's capabilities changed
                LOG_VERBOSE("aoo_source: protocol_flags to sink " << sink->id
                            << " flags " << flags);
                break;
            }
            // unknown
            default:
                LOG_WARNING("aoo_source: unknown sink option " << opt);
                return 0;
            }
            return 1;
        } else {
            LOG_ERROR("aoo_source: couldn't set option " << opt
                      << " - sink not found!");
            return 0;
        }
    }
}

int32_t aoo_source_get_sinkoption(aoo_source *src, void *endpoint, int32_t id,
                              int32_t opt, void *p, int32_t size)
{
    return src->get_sinkoption(endpoint, id, opt, p, size);
}

int32_t aoo::source::get_sinkoption(void *endpoint, int32_t id,
                              int32_t opt, void *p, int32_t size)
{
    if (id == AOO_ID_WILDCARD){
        LOG_ERROR("aoo_source: can't use wildcard to get sink option");
        return 0;
    }

    shared_lock lock(sink_mutex_); // reader lock!
    auto sink = find_sink(endpoint, id);
    if (sink){
        switch (opt){
        // channel onset
        case aoo_opt_channelonset:
            CHECKARG(int32_t);
            as<int32_t>(p) = sink->channel;
            break;
        // unknown
        default:
            LOG_WARNING("aoo_source: unsupported sink option " << opt);
            return 0;
        }
        return 1;
    } else {
        LOG_ERROR("aoo_source: couldn't get option " << opt
                  << " - sink " << id << " not found!");
        return 0;
    }
}

int32_t aoo_source_setup(aoo_source *src, int32_t samplerate,
                         int32_t blocksize, int32_t nchannels){
    return src->setup(samplerate, blocksize, nchannels);
}

int32_t aoo::source::setup(int32_t samplerate,
                           int32_t blocksize, int32_t nchannels){
    unique_lock lock(update_mutex_); // writer lock!
    if (samplerate > 0 && blocksize > 0 && nchannels > 0)
    {
        nchannels_ = nchannels;
        samplerate_ = samplerate;
        blocksize_ = blocksize;

        // reset timer + time DLL filter
        timer_.setup(samplerate_, blocksize_);

        update();

        return 1;
    }

    return 0;
}

int32_t aoo_source_add_sink(aoo_source *src, void *sink, int32_t id, aoo_replyfn fn) {
    return src->add_sink(sink, id, fn);
}

int32_t aoo::source::add_sink(void *endpoint, int32_t id, aoo_replyfn fn){
    unique_lock lock(sink_mutex_); // writer lock!
    if (id == AOO_ID_WILDCARD){
        // first remove all sinks on the given endpoint!
        auto it = std::remove_if(sinks_.begin(), sinks_.end(), [&](auto& s){
            return s.user == endpoint;
        });
        sinks_.erase(it, sinks_.end());
    } else {
        // check if sink exists!
        auto result = find_sink(endpoint, id);
        if (result){
            if (result->id == AOO_ID_WILDCARD){
                LOG_WARNING("aoo_source: can't add individual sink "
                            << id << " because of wildcard!");
            } else {
                LOG_WARNING("aoo_source: sink already added!");
            }
            return 0;
        }
    }
    // add sink descriptor
    sinks_.emplace_back(endpoint, fn, id);
    for (auto it = pending_sink_flags_.begin(); it != pending_sink_flags_.end(); ++it) {   // Early invite
        if (it->endpoint == endpoint && it->id == id) {
            sinks_.back().protocol_flags = it->flags;
            fprintf(stderr, "[AOO-SINKFLAGS] source id=%d sink id=%d flags=0x%x (from early invite)\n", (int) this->id(), (int) id, (unsigned)(uint8_t) it->flags);
            pending_sink_flags_.erase(it);
            break;
        }
    }
    masking_dirty_ = true;   // A new sink may not support idle masking
    // notify send_format()
    format_changed_ = true;

    return 1;
}

int32_t aoo_source_remove_sink(aoo_source *src, void *sink, int32_t id) {
    return src->remove_sink(sink, id);
}

int32_t aoo::source::remove_sink(void *endpoint, int32_t id){
    unique_lock lock(sink_mutex_); // writer lock!
    if (id == AOO_ID_WILDCARD){
        // remove all sinks on the given endpoint
        auto it = std::remove_if(sinks_.begin(), sinks_.end(), [&](auto& s){
            return s.user == endpoint;
        });
        sinks_.erase(it, sinks_.end());
        return 1;
    } else {
        for (auto it = sinks_.begin(); it != sinks_.end(); ++it){
            if (it->user == endpoint){
                if (it->id == AOO_ID_WILDCARD){
                    LOG_WARNING("aoo_source: can't remove individual sink "
                                << id << " because of wildcard!");
                    return 0;
                } else if (it->id == id){
                    sinks_.erase(it);
                    return 1;
                }
            }
        }
        LOG_WARNING("aoo_source: sink not found!");
        return 0;
    }
}

void aoo_source_remove_all(aoo_source *src) {
    src->remove_all();
}

void aoo::source::remove_all(){
    unique_lock lock(sink_mutex_); // writer lock!
    sinks_.clear();
}

int32_t aoo_source_handle_message(aoo_source *src, const char *data, int32_t n,
                              void *sink, aoo_replyfn fn) {
    return src->handle_message(data, n, sink, fn);
}

// /aoo/src/<id>/format <sink>
int32_t aoo::source::handle_message(const char *data, int32_t n, void *endpoint, aoo_replyfn fn){
    try {
        osc::ReceivedPacket packet(data, n);
        osc::ReceivedMessage msg(packet);

        int32_t type, src;
        auto onset = aoo_parse_pattern(data, n, &type, &src);
        if (!onset){
            LOG_WARNING("aoo_source: not an AoO message!");
            return 0;
        }
        if (type != AOO_TYPE_SOURCE){
            LOG_WARNING("aoo_source: not a source message!");
            return 0;
        }
        if (src == AOO_ID_WILDCARD){
            LOG_WARNING("aoo_source: can't handle wildcard messages (yet)!");
            return 0;
        }
        if (src != id()){
            LOG_WARNING("aoo_source: wrong source ID!");
            return 0;
        }

        auto pattern = msg.AddressPattern() + onset;
        if (!strcmp(pattern, AOO_MSG_FORMAT)){
            handle_format_request(endpoint, fn, msg);
            return 1;
        } else if (!strcmp(pattern, AOO_MSG_DATA)){
            handle_data_request(endpoint, fn, msg);
            return 1;
        } else if (!strcmp(pattern, AOO_MSG_INVITE)){
            handle_invite(endpoint, fn, msg);
            return 1;
        } else if (!strcmp(pattern, AOO_MSG_UNINVITE)){
            handle_uninvite(endpoint, fn, msg);
            return 1;
        } else if (!strcmp(pattern, AOO_MSG_PING)){
            handle_ping(endpoint, fn, msg);
            return 1;
        } else if (!strcmp(pattern, AOO_MSG_CODEC_CHANGE)){
            handle_codec_change(endpoint, fn, msg);
            return 1;
        } else {
            LOG_WARNING("unknown message " << pattern);
        }
    } catch (const osc::Exception& e){
        LOG_ERROR("aoo_source: exception in handle_message: " << e.what());
    }
    return 0;
}

int32_t aoo_source_send(aoo_source *src) {
    return src->send();
}

// This method reads audio samples from the ringbuffer,
// encodes them and sends them to all sinks.
// We have to aquire both the update lock and the sink list lock
// and release both before calling the sink's send method
// to avoid possible deadlocks in the client code.
// We have to make a local copy of the sink list, but this should be
// rather cheap in comparison to encoding and sending the audio data.
int32_t aoo::source::send(){
    if (!play_.load() && !activeplay_.load()){
        return false;
    }

    bool didsomething = false;

    if (send_format()){
        didsomething = true;
    }

    if (send_data()){
        didsomething = true;
    }

    if (resend_data()){
        didsomething = true;
    }

    if (send_ping()){
        didsomething = true;
    }

    return didsomething;
}

int32_t aoo_source_process(aoo_source *src, const aoo_sample **data, int32_t n, uint64_t t) {
    return src->process(data, n, t);
}

int32_t aoo::source::process(const aoo_sample **data, int32_t n, uint64_t t){
    if (!play_ && !activeplay_){
        return 0; // pausing
    }

    // update time DLL filter
    double error;
    auto state = timer_.update(t, error);
    if (state == timer::state::reset){
        LOG_DEBUG("setup time DLL filter for source");
        dll_.setup(samplerate_, blocksize_, bandwidth_, 0);
    } else if (state == timer::state::error){
        if (synthetic_clock_.load(std::memory_order_relaxed)){
            // Resync, don't skip. This source is driven by a synthetic monotonic
            // block clock (hub feed pump): a timer error here means the pump thread
            // slipped on the wall clock, not that any audio was lost; every produced
            // block is still queued in audioqueue_. Scheduling dropped_ empties would
            // make the sinks discard real audio and (at blocksize 64) permanently
            // starve clients. Instead, mirror the timer::state::reset path: re-setup
            // the DLL from nominal rate and reset the timer, exactly like a fresh
            // stream start. dll_/timer_/bandwidth_ access matches the reset branch
            // above (same thread, same locking discipline). Flag 0 = the path below,
            // used by client sources.
            LOG_VERBOSE("aoo_source: synthetic clock timer error ("
                        << error << " s) — resync DLL, no skip");
            dll_.setup(samplerate_, blocksize_, bandwidth_, 0);
            timer_.reset();
        } else {
            // skip blocks
            double period = (double)blocksize_ / (double)samplerate_;
            int nblocks = error / period + 0.5;
            // Cap dropped_ at the audioqueue capacity in blocks (cached by update()
            // as queue_nbuffers_; audioqueue_ itself must not be touched here, outside
            // update_mutex_). More debt than the queue can even hold audio for is pure
            // starvation credit the sink pays with silence; send_data() drains ALL of
            // dropped_ in one call, so the cap bounds that burst too.
            // CAS loop: race is only vs send_data()'s exchange(0), two threads, no
            // starvation; never resurrects drained debt.
            const int32_t cap = std::max<int32_t>(1,
                queue_nbuffers_.load(std::memory_order_relaxed));
            LOG_VERBOSE("skip " << nblocks << " blocks (cap " << cap << ")");
            int32_t cur = dropped_.load(std::memory_order_relaxed);
            int32_t next;
            do {
                next = cur + std::min<int32_t>(nblocks, cap);
                if (next > cap) next = cap;
            } while (!dropped_.compare_exchange_weak(cur, next,
                                                     std::memory_order_relaxed));
            timer_.reset();
        }
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
    bool ignoredll = !dynamic_resampling_.load();;
    if (fabs(dll_.samplerate() - (double)samplerate_) > 0.1*samplerate_) {
        ignoredll = true;
    }
    
    
    // the mutex should be uncontended most of the time.
    // NOTE: We could use try_lock() and skip the block if we couldn't aquire the lock.
    shared_lock lock(update_mutex_);

    if (!encoder_){
        return 0;
    }

     bool dofadein = play_ && !lastplay_;
     bool dofadeout = !play_ && lastplay_;
     
     if (dofadeout) {
         pushing_silent_frames_ = 4 * encoder_->blocksize();
         LOG_VERBOSE("do play fadeout, pushing silent: " << pushing_silent_frames_);
     }
     if (dofadein) {
         LOG_VERBOSE("do play fadein");
     }
     
     lastplay_ = play_;

     bool pushingSilence = !dofadeout && !play_ && pushing_silent_frames_ > 0;
     
     if (play_) {
         activeplay_ = true;
     } 
     else if (!dofadeout && pushing_silent_frames_ <= 0 && !flushingout_) {
         // already done our fadeout, and we aren't active anymore, so don't do any more processing
         //LOG_WARNING("last play processing");
         //activeplay_ = 0;
         pushing_silent_frames_ = 0;
         flushingout_ = 1;
         // activeplay_ will be set to false in the sending when it runs out of data
         return 0;
     }

    


    
    // non-interleaved -> interleaved
    //auto insamples = blocksize_ * nchannels_;
    auto insamples = n * nchannels_;
    auto outsamples = audioqueue_.blocksize(); // encoder_->blocksize() * nchannels_;
    auto *buf = (aoo_sample *)alloca(insamples * sizeof(aoo_sample));

    if (n > 0 && (dofadein || dofadeout || pushingSilence)) {
        const float fadedelta = dofadeout ? (-1.0f / n) : pushingSilence ? 0.0f : (1.0f / n);
        
        for (int i = 0; i < nchannels_; ++i){
            float gain = dofadeout ? 1.0f : 0.0f;
            for (int j = 0; j < n; ++j){
                buf[j * nchannels_ + i] = data[i][j] * gain;
                gain += fadedelta;
            }
        }
    } else {
        for (int i = 0; i < nchannels_; ++i){
            for (int j = 0; j < n; ++j){
                buf[j * nchannels_ + i] = data[i][j];
            }
        }
    }

    // ALWAYS use resampling buffer, just in case the caller needs to call us 
    // with varying sample counts on occasion. This can happen for various reasons
    // including being used within an audio plugin where the host may split the audio call
    // back calling with fewer samples. More importantly, this allows us to better decouple 
    // the audio process blocksize from the audioqueue blocksize (which matches the codec blocksize).

    //if (encoder_->blocksize() != blocksize_ || encoder_->samplerate() != samplerate_)
    {
        // go through resampler
        auto samplesleft = insamples;
        auto * pbuf = buf;

        auto availsamples = resampler_.write_available();
        
        while (samplesleft > 0) {
            auto usesamples = std::min(samplesleft, availsamples);
            
            resampler_.write(pbuf, usesamples);

            samplesleft -= usesamples;
            pbuf += usesamples;
            
            bool didconsume = false;
            
            while (resampler_.read_available() >= outsamples
                   && audioqueue_.write_available()
                   && srqueue_.write_available())
            {
                // copy audio samples
                resampler_.read(audioqueue_.write_data(), outsamples);
                audioqueue_.write_commit();
                
                // push samplerate
                if (!ignoredll) {
                    auto ratio = (double)encoder_->samplerate() / (double)samplerate_;
                    srqueue_.write(dll_.samplerate() * ratio);
                } else {
                    srqueue_.write(encoder_->samplerate());
                }

                didconsume = true;
            }

            // now update after any processing
            availsamples = resampler_.write_available();
            
            if (!didconsume && samplesleft > availsamples) {
                // didn't consume any, and we can't fit any more
                //LOG_WARNING("resampler could not handle all input samples, " << samplesleft << " unprocessed, avail " << availsamples << " audioqu_wravail: " << audioqueue_.write_available()  << " audqbs: " << audioqueue_.blocksize() << " encbs: " << encoder_->blocksize());
                // We are discarding the caller's input because audioqueue_/srqueue_ are full, but the
                // resampler still holds up to ~2.5 blocks of older samples, which would resume the stream
                // with stale audio (silent latency add) once the queue drains. Drop them too:
                // reset_positions() zeroes rd/wr/balance but preserves ratio_ (clear() would reset ratio_
                // to 1 and break active resampling until the next update()). Four scalar stores, no
                // locks/allocs/IO: audio-thread safe.
                resampler_.reset_positions();
                static std::atomic<int32_t> discard_log_n{0};
                int32_t dn = discard_log_n.fetch_add(1, std::memory_order_relaxed);
                if (dn < 10 || (dn % 500) == 0) {
                    LOG_VERBOSE("aoo_source: input discarded, resampler cleared (#" << dn << ")");
                }
                break;
            }
        }
    } 
#if 0
    else {
        // bypass resampler
        if (audioqueue_.write_available() && srqueue_.write_available()){
            // copy audio samples
            std::copy(buf, buf + outsamples, audioqueue_.write_data());
            audioqueue_.write_commit();

            // push samplerate
            srqueue_.write(dll_.samplerate());
        } else {
            // LOG_DEBUG("couldn't process");
        }
    }
#endif
    
    if (pushing_silent_frames_ > 0) {
        pushing_silent_frames_ -= n;
    }
    
    return 1;
}

int32_t aoo_source_events_available(aoo_source *src){
    return src->events_available();
}

int32_t aoo::source::events_available(){
    return eventqueue_.read_available() > 0;
}

int32_t aoo_source_handle_events(aoo_source *src,
                                aoo_eventhandler fn, void *user){
    return src->handle_events(fn, user);
}

int32_t aoo::source::handle_events(aoo_eventhandler fn, void *user){
    // always thread-safe
    auto n = eventqueue_.read_available();
    if (n > 0){
        // copy events
        auto events = (event *)alloca(sizeof(event) * n);
        for (int i = 0; i < n; ++i){
            eventqueue_.read(events[i]);
        }
        auto vec = (const aoo_event **)alloca(sizeof(aoo_event *) * n);
        for (int i = 0; i < n; ++i){
            vec[i] = (aoo_event *)&events[i];
        }
        // send events
        fn(user, vec, n);
    }
    return n;
}

namespace aoo {

/*//////////////////////////////// endpoint /////////////////////////////////////*/

// /aoo/sink/<id>/data <src> <salt> <seq> <sr> <channel_onset> <totalsize> <nframes> <frame> <data>

void endpoint::send_data(int32_t src, int32_t salt, const aoo::data_packet& d) const{
    // call without lock!

    char buf[AOO_MAXPACKETSIZE];
    osc::OutboundPacketStream msg(buf, sizeof(buf));

    if (id != AOO_ID_WILDCARD){
        const int32_t max_addr_size = AOO_MSG_DOMAIN_LEN
                + AOO_MSG_SINK_LEN + 16 + AOO_MSG_DATA_LEN;
        char address[max_addr_size];
        snprintf(address, sizeof(address), "%s%s/%d%s",
                 AOO_MSG_DOMAIN, AOO_MSG_SINK, id, AOO_MSG_DATA);

        msg << osc::BeginMessage(address);
    } else {
        msg << osc::BeginMessage(AOO_MSG_DOMAIN AOO_MSG_SINK AOO_MSG_WILDCARD AOO_MSG_DATA);
    }

    msg << src << salt << d.sequence << d.samplerate << d.channel
        << d.totalsize << d.nframes << d.framenum
        << osc::Blob(d.data, d.size) << osc::EndMessage;

    LOG_DEBUG("send block: seq = " << d.sequence << ", sr = " << d.samplerate
              << ", chn = " << d.channel << ", totalsize = " << d.totalsize
              << ", nframes = " << d.nframes << ", frame = " << d.framenum << ", size " << d.size << " msgsize: " << msg.Size() << "  overhead = " << (int) (100 * (1.0 - d.size/(double)msg.Size())) << "%");


    send(msg.Data(), (int32_t)msg.Size());
}

// /d <salt> <seq> <data>
// /d <salt> <seq> <srate> <data>

void endpoint::send_data_compact(int32_t src, int32_t salt, const aoo::data_packet& d, bool sendrate) {
    // call without lock!

    char buf[AOO_MAXPACKETSIZE];
    osc::OutboundPacketStream msg(buf, sizeof(buf));
    
    msg << osc::BeginMessage(AOO_MSG_COMPACT_DATA);

    // the salt is how we identify both ourselves and our target
    
    msg << salt << d.sequence;
    
    // only use the 4 argument version (with samplerate as double) if there is big enough divergence from prior samplerate
    if (sendrate) {
        msg << d.samplerate;
    }
    
    msg << osc::Blob(d.data, d.size) << osc::EndMessage;

    LOG_DEBUG("send compact block: seq = " << d.sequence << ", sr = " << d.samplerate
              << ", chn = " << d.channel << ", totalsize = " << d.totalsize
              << ", nframes = " << d.nframes << ", frame = " << d.framenum << ", size " << d.size << " msgsize: " << msg.Size() << "  overhead = " << (int) (100 * (1.0 - d.size/(double)msg.Size())) << "%");


    send(msg.Data(), (int32_t)msg.Size());
}

// /aoo/sink/<id>/format <src> <version> <salt> <numchannels> <samplerate> <blocksize> <codec> <options...> [<userformat..>]

void endpoint::send_format(int32_t src, int32_t salt, uint8_t extraflags, const aoo_format& f,
                            const char *options, int32_t size, const char * userformat, int32_t ufsize) const {
    // call without lock!
    LOG_DEBUG("send format to " << id << " (salt = " << salt << ")");

    char buf[AOO_MAXPACKETSIZE];
    osc::OutboundPacketStream msg(buf, sizeof(buf));

    if (id != AOO_ID_WILDCARD){
        const int32_t max_addr_size = AOO_MSG_DOMAIN_LEN
                + AOO_MSG_SINK_LEN + 16 + AOO_MSG_FORMAT_LEN;
        char address[max_addr_size];
        snprintf(address, sizeof(address), "%s%s/%d%s",
                 AOO_MSG_DOMAIN, AOO_MSG_SINK, id, AOO_MSG_FORMAT);

        msg << osc::BeginMessage(address);
    } else {
        msg << osc::BeginMessage(AOO_MSG_DOMAIN AOO_MSG_SINK AOO_MSG_WILDCARD AOO_MSG_FORMAT);
    }

    msg << src << (int32_t)make_version(AOO_PROTOCOL_FLAG_COMPACT_DATA | extraflags) << salt << f.nchannels << f.samplerate << f.blocksize
    << f.codec << osc::Blob(options, size);

    if (userformat && ufsize > 0) {
        msg << osc::Blob(userformat, ufsize);
    }

    msg << osc::EndMessage;

    send(msg.Data(), (int32_t)msg.Size());
}

// /aoo/sink/<id>/ping <src> <time>

void endpoint::send_ping(int32_t src, time_tag t) const {
    // call without lock!
    LOG_DEBUG("send ping to " << id);

    char buf[AOO_MAXPACKETSIZE];
    osc::OutboundPacketStream msg(buf, sizeof(buf));

    if (id != AOO_ID_WILDCARD){
        const int32_t max_addr_size = AOO_MSG_DOMAIN_LEN
                + AOO_MSG_SINK_LEN + 16 + AOO_MSG_PING_LEN;
        char address[max_addr_size];
        snprintf(address, sizeof(address), "%s%s/%d%s",
                 AOO_MSG_DOMAIN, AOO_MSG_SINK, id, AOO_MSG_PING);

        msg << osc::BeginMessage(address);
    } else {
        msg << osc::BeginMessage(AOO_MSG_DOMAIN AOO_MSG_SINK AOO_MSG_WILDCARD AOO_MSG_PING);
    }

    msg << src << osc::TimeTag(t.to_uint64()) << osc::EndMessage;

    send(msg.Data(), (int32_t)msg.Size());
}

/*///////////////////////// source ////////////////////////////////*/

sink_desc * source::find_sink(void *endpoint, int32_t id){
    for (auto& sink : sinks_){
        if ((sink.user == endpoint) &&
            (sink.id == AOO_ID_WILDCARD || sink.id == id))
        {
            return &sink;
        }
    }
    return nullptr;
}

int32_t source::set_format(aoo_format &f){
    unique_lock lock(update_mutex_); // writer lock!
    if (!encoder_ || strcmp(encoder_->name(), f.codec)){
        auto codec = aoo::find_codec(f.codec);
        if (codec){
            encoder_ = codec->create_encoder();
        } else {
            LOG_ERROR("codec '" << f.codec << "' not supported!");
            return 0;
        }
        if (!encoder_){
            LOG_ERROR("couldn't create encoder!");
            return 0;
        }
    }
    encoder_->set_format(f);

    update();

    return 1;
}

int32_t source::make_salt(){
    thread_local std::random_device dev;
    thread_local std::mt19937 mt(dev());
    std::uniform_int_distribution<int32_t> dist;
    return dist(mt);
}

int32_t source::set_userformat(void * ptr, int32_t size){
    unique_lock lock(update_mutex_); // writer lock!

    if (size <= 0) {
        // clear any user format
        userformat_.clear();
    }
    else {
        auto cptr = static_cast<char*>(ptr);
        userformat_.assign(cptr, cptr+size);
    }
    // Update mints a NEW SALT and resets the sequence (a full stream restart): every sink then treats
    // the source as a new stream and gets the whole history buffer dumped on it, an emergency drain on
    // every listener. The hub pushes each sender's userformat into all of its feed sources on EVERY
    // format event, and the client does the same to its own upstream on parameter changes.
    // A userformat change only needs the /format message re-sent; the sink applies it in place when the
    // salt is unchanged (sink handle_format resets only if salt != salt_). So: flag the resend, keep the stream.
    format_changed_ = true;

    return 1;
}


// always called with update_mutex_ locked!
void source::update(){
    if (!encoder_){
        return;
    }
    assert(encoder_->blocksize() > 0 && encoder_->samplerate() > 0);

    if (blocksize_ > 0){
        assert(samplerate_ > 0 && nchannels_ > 0);
        // setup audio buffer
        auto nsamples = encoder_->blocksize() * nchannels_;
        double bufsize = (double)buffersize_ * encoder_->samplerate() * 0.001;
        bufsize = std::max(bufsize, (double)blocksize_); // needs to be at least one processing blocksize_ worth!
        auto d = div(bufsize, encoder_->blocksize());
        int32_t nbuffers = d.quot + (d.rem != 0); // round up
        nbuffers = std::max<int32_t>(nbuffers, 1); // need at least 1 buffer!
        audioqueue_.resize(nbuffers * nsamples, nsamples);
        srqueue_.resize(nbuffers, 1);
        // Cache capacity-in-blocks for the dropped_
        // cap in process() (which runs outside update_mutex_ and must not
        // touch audioqueue_ directly).
        queue_nbuffers_.store(nbuffers, std::memory_order_relaxed);
        LOG_DEBUG("aoo::source::update: id: " << id_ << " nbuffers = " << nbuffers << " dquot: " << d.quot << " drem: " << d.rem <<  " bufsize: " << bufsize << " bs: " << encoder_->blocksize() << " reqbufms: " << buffersize_);

        // resampler
       // if (blocksize_ != encoder_->blocksize() || samplerate_ != encoder_->samplerate()){
            resampler_.setup(blocksize_, encoder_->blocksize(),
                             samplerate_, encoder_->samplerate(), nchannels_);
            resampler_.update(samplerate_, encoder_->samplerate());
        //} else {
        //    resampler_.clear();
        //}

        // history buffer
        update_historybuffer();
        
        // reset encoder state to avoid old garbage
        encoder_->reset();
        
        // reset time DLL to be on the safe side
        timer_.reset();
        lastpingtime_ = -1000; // force first ping
        lastplay_ = false;
        
        // Start new sequence and resend format.
        // We naturally want to do this when setting the format,
        // but it's good to also do it in setup() to eliminate
        // any timing gaps.
        salt_ = make_salt();
        sequence_ = 0;
        dropped_ = 0;
        {
            shared_lock lock2(sink_mutex_);
            for (auto& sink : sinks_){
                sink.format_changed = true;
            }
            // notify send_format()
            format_changed_ = true;
        }
    }
}

void source::update_historybuffer(){
    if (samplerate_ > 0 && encoder_){
        double bufsize = (double)resend_buffersize_ * 0.001 * samplerate_;
        auto d = div(bufsize, encoder_->blocksize());
        int32_t nbuffers = d.quot + (d.rem != 0); // round up
        history_.resize(nbuffers);
    }
}


// Decide whether this stream is masked. ON only when the option is set, the codec is
// PCM, and EVERY sink advertised AOO_PROTOCOL_FLAG_IDLE_MASK (hub mode: one sink per source, so this is per recipient).
// A change re-salts the stream so the receiver never parses a masked blob with the old rule (or vice versa).
// Send thread only. Lock order matches send_data(): update_mutex_ then sink_mutex_.
void source::refresh_masking(){
    bool want = false;
    {
        shared_lock updatelock(update_mutex_);
        shared_lock listlock(sink_mutex_);
        want = idle_mask_.load() != 0 && encoder_ && !strcmp(encoder_->name(), "pcm") && !sinks_.empty();
        for (auto& s : sinks_) if (!(s.protocol_flags.load() & AOO_PROTOCOL_FLAG_IDLE_MASK)) { want = false; break; }
        if (want == masking_active_) return;
    }
    {
        unique_lock updatelock(update_mutex_);
        shared_lock listlock(sink_mutex_);
        masking_active_ = want;
        salt_ = make_salt();
        sequence_ = 0;
        for (auto& s : sinks_) s.format_changed = true;
        format_changed_ = true;
        fprintf(stderr, "[AOO-MASK] source id=%d idle masking %s (sinks=%d, new salt=0x%x)\n",
                (int) id(), want ? "ON" : "OFF", (int) sinks_.size(), (unsigned) salt_);
    }
}

// Encode the full interleaved block with the untouched codec, then drop the bytes of
// every channel whose samples are all exactly zero. Layout: [mask: 1 byte per 8 channels, bit c = channel c present]
// [samples of the present channels, interleaved]. Byte-level, so it works for any PCM bit depth and the codec keeps no
// state about it. Constant small cost: one compare per sample plus the copy it replaces.
int32_t source::encode_masked(const aoo_sample *s, int32_t n, char *buf, int32_t size){
    const int32_t nch = encoder_->nchannels();
    const int32_t frames = nch > 0 ? n / nch : 0;
    const int32_t maskBytes = (nch + 7) / 8;
    if (nch <= 0 || frames <= 0 || size < maskBytes) return 0;
    uint8_t* mask = (uint8_t*) buf;
    std::memset(mask, 0, (size_t) maskBytes);
    int32_t live = 0;
    // An analog interface never delivers exact zeros (noise floor), so a channel is also idle
    // when its peak stays at/below the hub's own silence threshold (0.0001 = -80 dBFS) for ~50 ms of consecutive
    // blocks; it comes back on the first block above it. Exact digital silence masks immediately.
    constexpr float kQuiet = 0.0001f;
    const int32_t needQuiet = std::max<int32_t>(1, (samplerate_ > 0 && blocksize_ > 0) ? (int32_t)((0.05 * samplerate_ + blocksize_ - 1) / blocksize_) : 1);
    if ((int32_t) quiet_blocks_.size() != nch) quiet_blocks_.assign((size_t) nch, 0);
    for (int32_t c = 0; c < nch; ++c){
        float peak = 0.0f;
        for (int32_t j = 0; j < frames; ++j){ const float v = std::fabs(s[j * nch + c]); if (v > peak) peak = v; }
        bool idle;
        if (peak == 0.0f) { quiet_blocks_[c] = needQuiet; idle = true; }
        else if (peak <= kQuiet) { if (quiet_blocks_[c] < needQuiet) ++quiet_blocks_[c]; idle = quiet_blocks_[c] >= needQuiet; }
        else { quiet_blocks_[c] = 0; idle = false; }
        if (!idle) { mask[c >> 3] |= (uint8_t)(1u << (c & 7)); ++live; }
    }
    if (live == nch){
        const int32_t enc = encoder_->encode(s, n, buf + maskBytes, size - maskBytes);
        return enc > 0 ? enc + maskBytes : 0;
    }
    masked_blocks_.fetch_add(1, std::memory_order_relaxed);
    if (live == 0){
        masked_bytes_saved_.fetch_add((uint64_t) n * 2u, std::memory_order_relaxed);   // telemetry assumes 16-bit (hub + clients run PCM 16)
        return maskBytes;   // every channel idle: the mask alone
    }
    // full encode into scratch, then compact the present channels' bytes
    maskbuf_.resize((size_t) size);
    const int32_t enc = encoder_->encode(s, n, maskbuf_.data(), size);
    if (enc <= 0 || enc % n != 0) return 0;
    const int32_t ss = enc / n;   // bytes per sample
    if (maskBytes + live * frames * ss > size) return 0;
    char* out = buf + maskBytes;
    const char* in = maskbuf_.data();
    for (int32_t j = 0; j < frames; ++j){
        for (int32_t c = 0; c < nch; ++c){
            if (mask[c >> 3] & (1u << (c & 7))) { std::memcpy(out, in, (size_t) ss); out += ss; }
            in += ss;
        }
    }
    masked_bytes_saved_.fetch_add((uint64_t)(nch - live) * (uint64_t) frames * (uint64_t) ss, std::memory_order_relaxed);
    return (int32_t)(out - buf);
}

bool source::send_format(){
    if (masking_dirty_.exchange(false)) refresh_masking();   // May re-salt + set format_changed_
    bool format_changed = format_changed_.exchange(false);
    bool format_requested = formatrequestqueue_.read_available();

    if (!format_changed && !format_requested){
        return false;
    }

    shared_lock updatelock(update_mutex_); // reader lock!

    if (!encoder_){
        return false;
    }

    int32_t salt = salt_;
    const uint8_t extraflags = masking_active_ ? AOO_PROTOCOL_FLAG_IDLE_MASK : 0;   

    aoo_format fmt;
    char settings[AOO_CODEC_MAXSETTINGSIZE];
    auto size = encoder_->write_format(fmt, settings, sizeof(settings));

    updatelock.unlock();

    if (size < 0){
        return false;
    }

    auto userfmt = !userformat_.empty() ? &*userformat_.begin() : nullptr;
    int32_t userfmtsize = (int32_t) userformat_.size();

    if (format_changed){
        // only copy sinks which require a format update!
        shared_lock sinklock(sink_mutex_);
        auto sinks = (aoo::sink_desc *)alloca((sinks_.size() + 1) * sizeof(aoo::endpoint)); // avoid alloca(0)
        int numsinks = 0;
        for (auto& sink : sinks_){
            if (sink.format_changed.exchange(false)){
                new (sinks + numsinks) aoo::endpoint (sink.user, sink.fn, sink.id);
                numsinks++;
            }
        }
        sinklock.unlock();
        // now we don't hold any lock!

        for (int i = 0; i < numsinks; ++i){
            sinks[i].send_format(id(), salt, extraflags, fmt, settings, size, userfmt, userfmtsize);
        }
    }

    if (format_requested){
        while (formatrequestqueue_.read_available()){
            endpoint ep;
            formatrequestqueue_.read(ep);
            ep.send_format(id(), salt, extraflags, fmt, settings, size, userfmt, userfmtsize);
        }
    }

    return true;
}

bool source::resend_data(){
    shared_lock updatelock(update_mutex_); // reader lock!
    if (!history_.capacity()){
        return false;
    }

    bool didsomething = false;

    while (datarequestqueue_.read_available()){
        data_request request;
        datarequestqueue_.read(request);

        auto salt = salt_;
        if (salt != request.salt){
            // outdated request
            continue;
        }

        auto block = history_.find(request.sequence);
        if (block){
            aoo::data_packet d;
            d.sequence = block->sequence;
            d.samplerate = block->samplerate;
            d.channel = block->channel;
            d.totalsize = block->size();
            d.nframes = block->num_frames();
            // We use a buffer on the heap because blocks and even frames
            // can be quite large and we don't want them to sit on the stack.
            if (request.frame < 0){
                // Copy whole block and save frame pointers.
                sendbuffer_.resize(d.totalsize);
                char *buf = sendbuffer_.data();
                char *frameptr[256];
                int32_t framesize[256];
                int32_t onset = 0;

                for (int i = 0; i < d.nframes; ++i){
                    auto nbytes = block->get_frame(i, buf + onset, d.totalsize - onset);
                    if (nbytes > 0){
                        frameptr[i] = buf + onset;
                        framesize[i] = nbytes;
                        onset += nbytes;
                    } else {
                        LOG_ERROR("empty frame!");
                    }
                }
                // unlock before sending
                updatelock.unlock();

                // send frames to sink
                for (int i = 0; i < d.nframes; ++i){
                    d.framenum = i;
                    d.data = frameptr[i];
                    d.size = framesize[i];
                    request.send_data(id(), salt, d);
                }
            } else {
                // Copy a single frame
                if (request.frame >= 0 && request.frame < d.nframes){
                    int32_t size = block->frame_size(request.frame);
                    sendbuffer_.resize(size);
                    block->get_frame(request.frame, sendbuffer_.data(), size);
                    // unlock before sending
                    updatelock.unlock();

                    // send frame to sink
                    d.framenum = request.frame;
                    d.data = sendbuffer_.data();
                    d.size = size;
                    request.send_data(id(), salt, d);
                } else {
                    LOG_ERROR("frame number " << request.frame << " out of range!");
                }
            }
            // lock again
            updatelock.lock();

            didsomething = true;
        } else {
            LOG_VERBOSE("couldn't find block " << request.sequence);
        }
    }

    return didsomething;
}

bool source::send_data(){
    shared_lock updatelock(update_mutex_); // reader lock!
    if (!encoder_){
        return 0;
    }

    data_packet d;
    int32_t salt = salt_;
    bool sentempty = false;   // (F5)
    bool sentaudio = false;   // (F5)

    // *first* check for dropped blocks
    // Drain ALL pending dropped_ empties in one call, then CONTINUE to the
    // normal audio send below in the SAME call: if a skipped block cost two
    // send passes while the skip only added one, a transient timer error would
    // become a permanent emission-rate deficit under a rate-limited pacer.
    // dropped_ is only zeroed here; a concurrent process()-side add lands in
    // the next call. It is capped at the audioqueue block capacity (see
    // process()), so this burst is bounded.
    // Wire safety: each empty is an independent /data datagram built purely
    // from (id, salt, d), see endpoint::send_data, so N empties from one
    // call are byte-identical to N empties over N calls.
    int32_t ndropped = dropped_.exchange(0, std::memory_order_relaxed);
    if (ndropped > 0){
        double nominalsr = encoder_->samplerate(); // use nominal samplerate
        // Reserve the whole sequence range while still holding the reader lock:
        // update() (writer lock) resets sequence_ to 0, and incrementing after
        // unlock would race that reset.
        int32_t seqbase = sequence_;
        sequence_ += ndropped;
        // now we can unlock
        updatelock.unlock();

        // make local copy of sink descriptors
        shared_lock listlock(sink_mutex_);
        int32_t numsinks = (int32_t) sinks_.size();
        auto sinks = (sink_desc *)alloca((numsinks + 1) * sizeof(sink_desc)); // avoid alloca(0)
        std::copy(sinks_.begin(), sinks_.end(), sinks);

        // unlock before sending!
        listlock.unlock();

        for (int32_t b = 0; b < ndropped; ++b){
            // send empty block
            d.sequence = seqbase + b;   // Reserved above under the lock
            d.samplerate = nominalsr;
            d.channel = 0;   // Was uninitialized stack
            d.totalsize = 0;
            d.nframes = 0;
            d.framenum = 0;
            d.data = nullptr;
            d.size = 0;

            // send block to sinks
            for (int i = 0; i < numsinks; ++i){
                sinks[i].send_data(id(), salt, d);
            }

            // Keep per-emission sequence overflow semantics (the tail check below
            // only sees the LAST sequence of this call).
            if (d.sequence == INT32_MAX){
                unique_lock lock2(update_mutex_); // take writer lock
                salt_ = make_salt();
                salt = salt_;
            }
        }
        sentempty = true;

        // Re-acquire for the audio branch below
        // (encoder_/audioqueue_ access requires the reader lock).
        updatelock.lock();
    }

    if (audioqueue_.read_available() && srqueue_.read_available()){
        sentaudio = true;   // May follow an empty-drain in the same call
        // make local copy of sink descriptors
        shared_lock listlock(sink_mutex_);
        int32_t numsinks = (int32_t) sinks_.size();
        auto sinks = (sink_desc *)alloca((numsinks + 1) * sizeof(sink_desc)); // avoid alloca(0)
        std::copy(sinks_.begin(), sinks_.end(), sinks);

        // unlock before sending!
        listlock.unlock();

        d.sequence = sequence_++;
        srqueue_.read(d.samplerate); // always read samplerate from ringbuffer

        // for compact data sending purposes... only send rate when necessary
        bool sendrate = false;
        if (abs(d.samplerate - prev_sent_samplerate_) > 0.1) {
            sendrate = true;
            prev_sent_samplerate_ = d.samplerate;
        }
        
        if (numsinks){
            // copy and convert audio samples to blob data
            auto nchannels = encoder_->nchannels();
            auto blocksize = encoder_->blocksize();
            sendbuffer_.resize(sizeof(double) * nchannels * blocksize + 64); // overallocate (+ idle-mask header)

            // Masked blob when the decision is on (read under the shared update lock)
            d.totalsize = masking_active_
                ? encode_masked(audioqueue_.read_data(), audioqueue_.blocksize(), sendbuffer_.data(), (int32_t) sendbuffer_.size())
                : encoder_->encode(audioqueue_.read_data(), audioqueue_.blocksize(),
                                   sendbuffer_.data(), (int32_t) sendbuffer_.size());
            audioqueue_.read_commit();

            if (d.totalsize > 0){
                // calculate number of frames
                auto maxpacketsize = packetsize_ - AOO_DATA_HEADERSIZE;
                auto dv = div(d.totalsize, maxpacketsize);
                d.nframes = dv.quot + (dv.rem != 0);

                // save block
                history_.push(d.sequence, d.samplerate, sendbuffer_.data(),
                              d.totalsize, d.nframes, maxpacketsize);

                // unlock before sending!
                updatelock.unlock();

                // from here on we don't hold any lock!

                // send a single frame to all sinks
                // /AoO/<sink>/data <src> <salt> <seq> <sr> <channel_onset> <totalsize> <numpackets> <packetnum> <data>
                auto dosend = [&](int32_t frame, const char* data, auto n){
                    d.framenum = frame;
                    d.data = data;
                    d.size = n;
                    for (int i = 0; i < numsinks; ++i){
                        d.channel = sinks[i].channel;
                        // if the protocol_flags allow using the compact data message, use it if appropriate
                        if (d.nframes == 1 && d.channel == 0 && sinks[i].protocol_flags & AOO_PROTOCOL_FLAG_COMPACT_DATA) {
                            sinks[i].send_data_compact(id(), salt, d, sendrate);                
                        } else {
                            sinks[i].send_data(id(), salt, d);
                        }
                    }
                };

                auto ntimes = redundancy_.load();
                for (auto i = 0; i < ntimes; ++i){
                    auto ptr = sendbuffer_.data();
                    // send large frames (might be 0)
                    for (int32_t j = 0; j < dv.quot; ++j, ptr += maxpacketsize){
                        dosend(j, ptr, maxpacketsize);
                    }
                    // send remaining bytes as a single frame (might be the only one!)
                    if (dv.rem){
                        dosend(dv.quot, ptr, dv.rem);
                    }
                }
            } else {
                LOG_WARNING("aoo_source: couldn't encode audio data!");
            }
        } else {
            // drain buffer anyway
            audioqueue_.read_commit();
        }
    } else if (!sentempty) {
        // Flush-out bookkeeping only when this call sent NOTHING.
        // LOG_DEBUG("couldn't send");
        if (!play_.load() && flushingout_.load() ) {
            LOG_VERBOSE("finished flushing out");
            activeplay_ = 0;
            flushingout_ = 0;
        }
        return 0;
    }

    // handle overflow (with 64 samples @ 44.1 kHz this happens every 36 days)
    // for now just force a reset by changing the salt, LATER think how to handle this better
    // Sentaudio guard — empty-drain overflow is already
    // handled per-emission inside the drain loop above.
    if (sentaudio && d.sequence == INT32_MAX){
        unique_lock lock2(update_mutex_); // take writer lock
        salt_ = make_salt();
    }

    return 1;   // Sentempty and/or sentaudio
}

bool source::send_ping(){
    // if stream is stopped, the timer won't increment anyway
    auto elapsed = timer_.get_elapsed();
    auto pingtime = lastpingtime_.load();
    auto interval = ping_interval_.load(); // 0: no ping
    if (interval > 0 && (elapsed - pingtime) >= interval){
        // only copy sinks which require a format update!
        shared_lock sinklock(sink_mutex_);
        int32_t numsinks = (int32_t) sinks_.size();
        auto sinks = (aoo::sink_desc *)alloca((numsinks + 1) * sizeof(aoo::sink_desc)); // avoid alloca(0)
        std::copy(sinks_.begin(), sinks_.end(), sinks);
        sinklock.unlock();

        // Stamp tt1 with the same real clock the reply handler uses for tt3 (aoo_osctime_get(), see
        // handle_ping), not the stream timer, so tt3 - tt1 is a true round-trip.
        time_tag tt(aoo_osctime_get());

        for (int i = 0; i < numsinks; ++i){
            sinks[i].send_ping(id(), tt);
        }

        lastpingtime_ = elapsed;
        return true;
    } else {
        return false;
    }
}

void source::handle_format_request(void *endpoint, aoo_replyfn fn,
                                   const osc::ReceivedMessage& msg)
{
    LOG_DEBUG("handle format request");

    auto it = msg.ArgumentsBegin();

    auto id = (it++)->AsInt32();
    auto version = (it++)->AsInt32();

    // LATER handle this in the sink_desc (e.g. not sending data)
    if (!check_version(version)){
        LOG_ERROR("aoo_source: sink version not supported");
        return;
    }

    // check if sink exists (not strictly necessary, but might help catch errors)
    shared_lock lock(sink_mutex_); // reader lock!
    auto sink = find_sink(endpoint, id);
    lock.unlock();

    if (sink){
        if (sink->protocol_flags.load() != (int8_t)(version & 0xFF))
            fprintf(stderr, "[AOO-SINKFLAGS] source id=%d sink id=%d flags=0x%x (from format request)\n", (int) this->id(), (int) id, (unsigned)(version & 0xFF));
        sink->protocol_flags = version & 0xFF;
        masking_dirty_ = true;   // The sink told us its capabilities
        if (formatrequestqueue_.write_available()){
            formatrequestqueue_.write(aoo::endpoint { endpoint, fn, id });
        }
    } else {
        // Rate-limit this warning. After a client quit/relaunch the hub's outbound feed sources
        // (deterministic source IDs) get re-created but listeners' sinks still hold stale subscriptions
        // and keep sending /format requests; unconditional logging would flood stderr and hammer the
        // hub thread. LATER: proper source/sink lifecycle on reconnect.
        static std::atomic<int> fmt_nofound_log_n{0};
        int n = fmt_nofound_log_n.fetch_add(1);
        if (n < 30 || (n % 200) == 0) {
            LOG_WARNING("ignoring '" << AOO_MSG_FORMAT << "' message: sink not found (#" << n << ")");
        }
    }
}

void source::handle_data_request(void *endpoint, aoo_replyfn fn,
                                 const osc::ReceivedMessage& msg)
{
    auto it = msg.ArgumentsBegin();
    auto id = (it++)->AsInt32();
    auto salt = (it++)->AsInt32();

    LOG_DEBUG("handle data request");

    // check if sink exists (not strictly necessary, but might help catch errors)
    shared_lock lock(sink_mutex_); // reader lock!
    auto sink = find_sink(endpoint, id);
    lock.unlock();

    if (sink){
        // get pairs of [seq, frame]
        int npairs = (msg.ArgumentCount() - 2) / 2;
        while (npairs--){
            auto seq = (it++)->AsInt32();
            auto frame = (it++)->AsInt32();
            if (datarequestqueue_.write_available()){
                datarequestqueue_.write(data_request{ endpoint, fn, id, salt, seq, frame });
            }
        }
    } else {
        // Rate-limit, same reasoning as the /format spam fix above.
        static std::atomic<int> data_nofound_log_n{0};
        int n = data_nofound_log_n.fetch_add(1);
        if (n < 30 || (n % 200) == 0) {
            LOG_WARNING("ignoring '" << AOO_MSG_DATA << "' message: sink not found (#" << n << ")");
        }
    }
}

void source::handle_invite(void *endpoint, aoo_replyfn fn,
                           const osc::ReceivedMessage& msg)
{
    auto it = msg.ArgumentsBegin();
    auto id = (it++)->AsInt32();
    auto flags = it == msg.ArgumentsEnd() ? 0 : it->AsInt32();

    LOG_DEBUG("handle invite");

    // check if sink exists (not strictly necessary, but might help catch errors)
    shared_lock lock(sink_mutex_); // reader lock!
    auto sink = find_sink(endpoint, id);
    lock.unlock();

    if (!sink){
        {   // The hub adds the sink a moment later and never reads invite events —
            // keep the capability flags so add_sink() can apply them.
            unique_lock wlock(sink_mutex_);
            bool found = false;
            for (auto& pf : pending_sink_flags_) if (pf.endpoint == endpoint && pf.id == id) { pf.flags = (int8_t)(flags & 0xFF); found = true; break; }
            if (!found) { if (pending_sink_flags_.size() > 64) pending_sink_flags_.erase(pending_sink_flags_.begin()); pending_sink_flags_.push_back({endpoint, id, (int8_t)(flags & 0xFF)}); }
        }
        // push "invite" event
        if (eventqueue_.write_available()){
            event e;
            e.type = AOO_INVITE_EVENT;
            e.sink.endpoint = endpoint;
            // Use 'id' because we want the individual sink! ('sink.id' might be a wildcard)
            e.sink.id = id;
            e.sink.flags = flags;
            eventqueue_.write(e);
        }
    } else {
        // The hub adds a listener's sink BEFORE the listener's /invite arrives, so this is the only
        // place its capability flags ever reach us. Record them (a change wakes refresh_masking() on
        // the send thread).
        const int32_t newflags = flags & 0xFF;
        if (sink->protocol_flags.load() != newflags) {
            sink->protocol_flags = newflags;
            masking_dirty_ = true;
            fprintf(stderr, "[AOO-SINKFLAGS] source id=%d sink id=%d flags=0x%x (from invite)\n", (int) this->id(), (int) id, (unsigned) newflags);
        }
        LOG_VERBOSE("ignoring '" << AOO_MSG_INVITE << "' message: sink already added");
    }
}

void source::handle_uninvite(void *endpoint, aoo_replyfn fn,
                             const osc::ReceivedMessage& msg)
{
    auto id = msg.ArgumentsBegin()->AsInt32();

    LOG_DEBUG("handle uninvite");

    // check if sink exists (not strictly necessary, but might help catch errors)
    shared_lock lock(sink_mutex_); // reader lock!
    auto sink = find_sink(endpoint, id);
    lock.unlock();

    if (sink){
        // push "uninvite" event
        if (eventqueue_.write_available()){
            event e;
            e.type = AOO_UNINVITE_EVENT;
            e.sink.endpoint = endpoint;
            // Use 'id' because we want the individual sink! ('sink.id' might be a wildcard)
            e.sink.id = id;
            eventqueue_.write(e);
        }
    } else {
        LOG_VERBOSE("ignoring '" << AOO_MSG_UNINVITE << "' message: sink not found");
    }
}

void source::handle_ping(void *endpoint, aoo_replyfn fn,
                         const osc::ReceivedMessage& msg)
{
    auto it = msg.ArgumentsBegin();
    auto id = (it++)->AsInt32();
    time_tag tt1 = (it++)->AsTimeTag();
    time_tag tt2 = (it++)->AsTimeTag();
    int32_t lost_blocks = (it++)->AsInt32();

    LOG_DEBUG("handle ping");

    // check if sink exists (not strictly necessary, but might help catch errors)
    shared_lock lock(sink_mutex_); // reader lock!
    auto sink = find_sink(endpoint, id);
    lock.unlock();

    if (sink){
        // push "ping" event
        if (eventqueue_.write_available()){
            event e;
            e.type = AOO_PING_EVENT;
            e.sink.endpoint = endpoint;
            // Use 'id' because we want the individual sink! ('sink.id' might be a wildcard)
            e.sink.id = id;
            e.ping.tt1 = tt1.to_uint64();
            e.ping.tt2 = tt2.to_uint64();
            e.ping.lost_blocks = lost_blocks;
        #if 0
            e.ping.tt3 = timer_.get_absolute().to_uint64(); // use last stream time
        #else
            e.ping.tt3 = aoo_osctime_get(); // use real system time
        #endif
            eventqueue_.write(e);
        }
    } else {
        LOG_VERBOSE("ignoring '" << AOO_MSG_PING << "' message: sink not found");
    }
}

// /aoo/src/<id>/codecchange <sink> <numchannels> <samplerate> <blocksize> <codec> <options...>

void source::handle_codec_change(void *endpoint, aoo_replyfn fn,
                                 const osc::ReceivedMessage& msg)
{
    if (!respect_codec_change_req_) {
        LOG_WARNING("Not allowing codec change request");
        return;
    }
    
    auto it = msg.ArgumentsBegin();

    int32_t id = (it++)->AsInt32();

    // get requested codec and format options from arguments
    // use our existing format for the other params
    aoo_format f;
    auto chans = (it++)->AsInt32();
    auto srate = (it++)->AsInt32();
    auto bsize = (it++)->AsInt32();
    f.codec = (it++)->AsString();

    f.nchannels = nchannels_; // use existing for now
    f.samplerate = samplerate_; // use existing for now
    f.blocksize = bsize;
    const void *settings;
    osc::osc_bundle_element_size_t size;
    (it++)->AsBlob(settings, size);

    if (id < 0){
        LOG_WARNING("bad ID for " << AOO_MSG_FORMAT << " message");
        return;
    }
    
   
    // check if sink exists (not strictly necessary, but might help catch errors)
    shared_lock lock(sink_mutex_); // reader lock!
    auto sink = find_sink(endpoint, id);
    lock.unlock();

    if (sink){
        { // only if the requesting sink exists we will respect this request
            LOG_DEBUG("handle codec change");
            unique_lock lock(update_mutex_); // writer lock!
            
            if (!encoder_ || strcmp(encoder_->name(), f.codec)){
                auto codec = aoo::find_codec(f.codec);
                if (codec){
                    encoder_ = codec->create_encoder();
                } else {
                    LOG_ERROR("codec '" << f.codec << "' not supported!");
                    return;
                }
                if (!encoder_){
                    LOG_ERROR("couldn't create encoder!");
                    return;
                }
            }
        
            encoder_->read_format(f, (const char *)settings, size);
            update();
        }
               
        
        // push "codec change" event
        if (eventqueue_.write_available()){
            event e;
            e.type = AOO_CHANGECODEC_EVENT;
            e.sink.endpoint = endpoint;
            // Use 'id' because we want the individual sink! ('sink.id' might be a wildcard)
            e.sink.id = id;
            eventqueue_.write(e);
        }
    } else {
        LOG_VERBOSE("ignoring '" << AOO_CHANGECODEC_EVENT << "' message: sink not found");
    }
}



} // aoo
