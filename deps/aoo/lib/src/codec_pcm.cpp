/* Copyright (c) 2010-Now Christof Ressi, Winfried Ritsch and others. 
 * For information on usage and redistribution, and for a DISCLAIMER OF ALL
 * WARRANTIES, see the file, "LICENSE.txt," in this distribution.  */

#include "aoo/aoo_pcm.h"
#include "aoo/aoo_utils.hpp"

#include <cassert>
#include <cstring>
#include <algorithm>
#include <vector>
#include <atomic>
#include <cstdlib>

namespace {

// conversion routines between aoo_sample and PCM data
union convert {
    int8_t b[8];
    int16_t i16;
    int32_t i32;
    int64_t i64;
    float f;
    double d;
};

int32_t bytes_per_sample(int32_t bd)
{
    switch (bd){
    case AOO_PCM_INT16:
        return 2;
    case AOO_PCM_INT24:
        return 3;
    case AOO_PCM_FLOAT32:
        return 4;
    case AOO_PCM_FLOAT64:
        return 8;
    default:
        assert(false);
        return 0;
    }
}

void sample_to_int16(aoo_sample in, char *out)
{
    convert c;
    int32_t temp = in * 0x7fff + 0.5f;
    c.i16 = (temp > INT16_MAX) ? INT16_MAX : (temp < INT16_MIN) ? INT16_MIN : temp;
#if BYTE_ORDER == BIG_ENDIAN
    memcpy(out, c.b, 2); // optimized away
#else
    out[0] = c.b[1];
    out[1] = c.b[0];
#endif
}

void sample_to_int24(aoo_sample in, char *out)
{
    convert c;
    int32_t temp = in * 0x7fffffff + 0.5f;
    c.i32 = (temp > INT32_MAX) ? INT32_MAX : (temp < INT32_MIN) ? INT32_MIN : temp;
    // only copy the highest 3 bytes!
#if BYTE_ORDER == BIG_ENDIAN
    out[0] = c.b[0];
    out[1] = c.b[1];
    out[2] = c.b[2];
#else
    out[0] = c.b[3];
    out[1] = c.b[2];
    out[2] = c.b[1];
#endif
}

void sample_to_float32(aoo_sample in, char *out)
{
    aoo::to_bytes<float>(in, out);
}

void sample_to_float64(aoo_sample in, char *out)
{
    aoo::to_bytes<double>(in, out);
}

aoo_sample int16_to_sample(const char *in){
    convert c;
#if BYTE_ORDER == BIG_ENDIAN
    memcpy(c.b, in, 2); // optimized away
#else
    c.b[0] = in[1];
    c.b[1] = in[0];
#endif
    return(aoo_sample)c.i16 / 32768.f;
}

aoo_sample int24_to_sample(const char *in)
{
    convert c;
    // copy to the highest 3 bytes!
#if BYTE_ORDER == BIG_ENDIAN
    c.b[0] = in[0];
    c.b[1] = in[1];
    c.b[2] = in[2];
    c.b[3] = 0;
#else
    c.b[0] = 0;
    c.b[1] = in[2];
    c.b[2] = in[1];
    c.b[3] = in[0];
#endif
    return (aoo_sample)c.i32 / 0x7fffffff;
}

aoo_sample float32_to_sample(const char *in)
{
    return aoo::from_bytes<float>(in);
}

aoo_sample float64_to_sample(const char *in)
{
    return aoo::from_bytes<double>(in);
}

void print_settings(const aoo_format_pcm& f)
{
    LOG_VERBOSE("PCM settings: "
                << "nchannels = " << f.header.nchannels
                << ", blocksize = " << f.header.blocksize
                << ", samplerate = " << f.header.samplerate
                << ", bitdepth = " << bytes_per_sample(f.bitdepth));
}

/*//////////////////// codec //////////////////////////*/

struct codec {
    codec(){
        memset(&format, 0, sizeof(aoo_format_pcm));
    }
    aoo_format_pcm format;

    // ─── Packet-loss concealment (decoder side) ───
    // Instead of zero-filling a lost block (an audible click), do NetEQ-style "expand": keep ~85 ms of
    // decoded history, on loss find the pitch period and continue the waveform periodically with a fade
    // to silence, and fade back in on the first good block. Zeros only when history is insufficient
    // (stream start, buffer priming).
    // History is allocated in codec_setformat (control path, never the audio thread).
    // Encoders share the struct but never touch this state.
    std::vector<aoo_sample> plcHist;   // interleaved ring, kHistFrames frames
    int plcWrite = 0;                  // ring write position (frames)
    int plcFilled = 0;                 // frames of valid history
    int plcConcealed = 0;              // consecutive concealed frames
    int plcAgo = 0;                    // replay cursor: frames behind write head
    int plcPeriod = 0;                 // detected period (frames)

    static constexpr int kHistFrames  = 4096;  // ~85ms @48k
    static constexpr int kMinHistory  = 1024;  // don't conceal on <21ms history
    // Full-quality conceal for ~2 ms, then a fast fade to silence by ~8 ms: single-block losses stay
    // covered, long refills go quiet (phase-mismatched loop grains sound worse than silence).
    static constexpr int kFullFrames  = 96;    // full-gain conceal (~2ms)
    static constexpr int kRampFrames  = 384;   // then fade to 0 by ~8ms total
    static constexpr int kXfadeFrames = 32;    // recovery de-click fade-in (~0.7ms)
};

static bool plc_disabled();

// Append decoded frames into the PLC history ring.
static void plc_append(codec *c, const aoo_sample *s, int frames, int ch)
{
    // With PLC off (hub relay: SB_PLC_OFF set in-process) skip history
    // recording too — the ring copy on every decoded block is pure waste
    // when conceal can never run.
    if (plc_disabled() || c->plcHist.empty()) return;
    for (int f = 0; f < frames; ++f){
        aoo_sample *dst = c->plcHist.data() + (size_t)c->plcWrite * ch;
        for (int k = 0; k < ch; ++k) dst[k] = s[(size_t)f * ch + k];
        c->plcWrite = (c->plcWrite + 1) % codec::kHistFrames;
        if (c->plcFilled < codec::kHistFrames) c->plcFilled++;
    }
}

// Read one history frame located `ago` frames behind the write head.
static inline const aoo_sample* plc_frame_ago(const codec *c, int ago, int ch)
{
    int idx = c->plcWrite - ago;
    while (idx < 0) idx += codec::kHistFrames;
    return c->plcHist.data() + (size_t)idx * ch;
}

// Pitch-period estimate over recent history: normalized autocorrelation on a
// 4:1-downsampled mono mix. Cost = tens of µs, paid only at the START of a
// loss burst — never in steady state.
static int plc_find_period(const codec *c, int ch, int srate)
{
    const int stride = 4;
    const int winFrames = std::min(c->plcFilled, (srate * 40) / 1000); // ≤40ms
    const int n = winFrames / stride;
    if (n < 96) return 0;
    float mono[512];
    const int N = std::min(n, 512);
    for (int i = 0; i < N; ++i){
        int ago = (N - i) * stride;
        const aoo_sample *fr = plc_frame_ago(c, ago, ch);
        float v = 0; for (int k = 0; k < ch; ++k) v += (float)fr[k];
        mono[i] = v;
    }
    const int minLag = std::max(2,  (srate * 2)  / 1000 / stride);  //  2ms
    const int maxLag = std::min(N/2,(srate * 15) / 1000 / stride);  // 15ms
    if (maxLag <= minLag) return 0;
    float bestScore = 0.f; int bestLag = 0;
    for (int lag = minLag; lag < maxLag; ++lag){
        float num = 0, den = 0;
        for (int i = N - 1; i >= lag && i >= N - 256; --i){
            num += mono[i] * mono[i - lag];
            den += mono[i] * mono[i] + mono[i - lag] * mono[i - lag];
        }
        if (den <= 1e-9f) continue;
        float score = 2.f * num / den;    // 1.0 = perfectly periodic
        if (score > bestScore){ bestScore = score; bestLag = lag; }
    }
    if (bestScore < 0.25f) return 0;      // aperiodic → caller uses grain fallback
    return bestLag * stride;
}

// Kill switch + observability. SB_PLC_OFF=1 disables concealment (stock
// zero-fill) for A/B without rebuilds. Counters are process-wide totals,
// exposed via aoo_pcm_plc_stats() for the host's stats surfaces.
static bool plc_disabled(){
    static const bool off = [](){ const char* e = getenv("SB_PLC_OFF");
                                  return e && e[0] == '1'; }();
    return off;
}
static std::atomic<uint64_t> g_plcConcealEvents{0};   // burst starts
static std::atomic<uint64_t> g_plcConcealFrames{0};   // total frames concealed

extern "C" void aoo_pcm_plc_stats(uint64_t* events, uint64_t* frames){
    if (events) *events = g_plcConcealEvents.load(std::memory_order_relaxed);
    if (frames) *frames = g_plcConcealFrames.load(std::memory_order_relaxed);
}

// Synthesize `frames` frames of concealment into `s` (interleaved).
static void plc_conceal(codec *c, aoo_sample *s, int frames, int ch, int srate)
{
    if (plc_disabled() || c->plcHist.empty() || c->plcFilled < codec::kMinHistory){
        for (int i = 0; i < frames * ch; ++i) s[i] = 0;   // stock behavior
        return;
    }
    if (c->plcConcealed == 0){
        g_plcConcealEvents.fetch_add(1, std::memory_order_relaxed);
        c->plcPeriod = plc_find_period(c, ch, srate);
        if (c->plcPeriod <= 0){
            // aperiodic material: loop the last ~10ms as a grain
            c->plcPeriod = std::min(c->plcFilled - 1, (srate * 10) / 1000);
        }
        c->plcPeriod = std::min(c->plcPeriod, c->plcFilled - 1);
        if (c->plcPeriod < 8) { // pathological — fall back to silence
            for (int i = 0; i < frames * ch; ++i) s[i] = 0;
            return;
        }
        c->plcAgo = c->plcPeriod;
    }
    for (int f = 0; f < frames; ++f){
        float gain = 1.f;
        if (c->plcConcealed > codec::kFullFrames) {
            gain = 1.f - (float)(c->plcConcealed - codec::kFullFrames)
                         / (float)codec::kRampFrames;
            if (gain < 0.f) gain = 0.f;
        }
        const aoo_sample *src = plc_frame_ago(c, c->plcAgo, ch);
        for (int k = 0; k < ch; ++k) s[(size_t)f * ch + k] = src[k] * gain;
        if (--c->plcAgo <= 0) c->plcAgo = c->plcPeriod;   // periodic wrap
        c->plcConcealed++;
    }
    g_plcConcealFrames.fetch_add((uint64_t)frames, std::memory_order_relaxed);
    // Synthesized audio is NOT appended to history — long losses keep looping
    // clean pre-loss material instead of compounding artifacts.
}

int32_t codec_setformat(void *enc, aoo_format *f)
{
    if (strcmp(f->codec, AOO_CODEC_PCM)){
        return 0;
    }
    auto c = static_cast<codec *>(enc);
    auto fmt = reinterpret_cast<aoo_format_pcm *>(f);

    // validate blocksize
    if (fmt->header.blocksize <= 0){
        LOG_WARNING("PCM: bad blocksize " << fmt->header.blocksize
                    << ", using 64 samples");
        fmt->header.blocksize = 64;
    }
    // validate samplerate
    if (fmt->header.samplerate <= 0){
        LOG_WARNING("PCM: bad samplerate " << fmt->header.samplerate
                    << ", using 44100");
        fmt->header.samplerate = 44100;
    }
    // validate channels
    if (fmt->header.nchannels <= 0 || fmt->header.nchannels > 255){
        LOG_WARNING("PCM: bad channel count " << fmt->header.nchannels
                    << ", using 1 channel");
        fmt->header.nchannels = 1;
    }
    // validate bitdepth
    if (fmt->bitdepth < 0 || fmt->bitdepth > AOO_PCM_BITDEPTH_SIZE){
        LOG_WARNING("PCM: bad bitdepth, using 32bit float");
        fmt->bitdepth = AOO_PCM_FLOAT32;
    }

    // save and print settings
    memcpy(&c->format, fmt, sizeof(aoo_format_pcm));
    c->format.header.codec = AOO_CODEC_PCM; // !
    print_settings(c->format);

    // (re)allocate + reset history here — the
    // control path — so the audio thread never allocates. A format change
    // also invalidates old history (rate/channels may differ), so reset.
    {
        int ch = std::max(1, (int)c->format.header.nchannels);
        c->plcHist.assign((size_t)codec::kHistFrames * ch, 0);
        c->plcWrite = 0; c->plcFilled = 0;
        c->plcConcealed = 0; c->plcAgo = 0; c->plcPeriod = 0;
    }

    return 1;
}

int32_t encoder_readformat(void *enc, aoo_format *fmt,
                           const char *buf, int32_t size)
{
    if (size >= 4){
        auto c = static_cast<codec *>(enc);
        // TODO validate
        if (!strcmp(fmt->codec, AOO_CODEC_PCM) && fmt->blocksize > 0
                && fmt->samplerate > 0 && fmt->blocksize > 0)
        {
            memcpy(&c->format.header, fmt, sizeof(aoo_format));
            c->format.bitdepth = (aoo_pcm_bitdepth)aoo::from_bytes<int32_t>(buf);
            c->format.header.codec = AOO_CODEC_PCM; // !
            //print_settings(c->format);
            
            if (codec_setformat(enc, &c->format.header)) {     
                // it could have been modified during validation, need to re-write the base format of 
                // passed in value
                memcpy(fmt, &c->format.header, sizeof(aoo_format));
                return 4;
            }
            else {
                return -1;
            }
        } else {
            LOG_ERROR("PCM: bad format!");
        }
    } else {
        LOG_ERROR("PCM: couldn't read format - not enough data!");
    }
    return -1;
}

int32_t codec_reset(void *enc) {
    auto c = static_cast<codec *>(enc);
    if (c){
        return 1;
    }
    return 0;
}

int32_t codec_getformat(void *x, aoo_format_storage *f)
{
    auto c = static_cast<codec *>(x);
    if (c->format.header.codec){
        memcpy(f, &c->format, sizeof(aoo_format_pcm));
        return sizeof(aoo_format_pcm);
    } else {
        return 0;
    }
}

void *encoder_new(){
    return new codec;
}

void encoder_free(void *enc){
    delete (codec *)enc;
}

int32_t encoder_encode(void *enc,
                       const aoo_sample *s, int32_t n,
                       char *buf, int32_t size)
{
    auto bitdepth = static_cast<codec *>(enc)->format.bitdepth;
    auto samplesize = bytes_per_sample(bitdepth);

    if (size < (n * samplesize)){
        return 0;
    }

    auto samples_to_blob = [&](auto fn){
        auto b = buf;
        for (int i = 0; i < n; ++i){
            fn(s[i], b);
            b += samplesize;
        }
    };

    switch (bitdepth){
    case AOO_PCM_INT16: {
        // Bulk path: a tight loop that auto-vectorizes, because per-sample conversion is a hotspot
        // on the hub. Same bytes on the wire (big-endian int16).
        auto* b = (uint8_t*)buf;
        for (int i = 0; i < n; ++i){
            int32_t t = (int32_t)(s[i] * 32767.f + 0.5f);  // matches legacy rounding exactly
            if (t > 32767) t = 32767; else if (t < -32768) t = -32768;
            uint16_t u = (uint16_t)(int16_t)t;
            b[2*i]   = (uint8_t)(u >> 8);
            b[2*i+1] = (uint8_t)(u & 0xff);
        }
        break;
    }
    case AOO_PCM_INT24:
        samples_to_blob(sample_to_int24);
        break;
    case AOO_PCM_FLOAT32:
        samples_to_blob(sample_to_float32);
        break;
    case AOO_PCM_FLOAT64:
        samples_to_blob(sample_to_float64);
        break;
    default:
        // unknown bitdepth
        break;
    }

    return n * samplesize;
}

int32_t encoder_writeformat(void *enc, aoo_format *fmt,
                            char *buf, int32_t size)
{
    if (size >= 4){
        aoo_format_pcm * ofmt;
        if (enc == nullptr) {
            ofmt = reinterpret_cast<aoo_format_pcm *>(fmt);            
        }
        else {
            auto c = static_cast<codec *>(enc);
            ofmt = &c->format;
            memcpy(fmt, &ofmt->header, sizeof(aoo_format));
        }
        aoo::to_bytes<int32_t>(ofmt->bitdepth, buf);

        return 4;
    } else {
        LOG_ERROR("PCM: couldn't write settings - buffer too small!");
        return -1;
    }
}

void *decoder_new(){
    return new codec;
}

void decoder_free(void *dec){
    delete (codec *)dec;
}

int32_t decoder_decode(void *dec,
                       const char *buf, int32_t size,
                       aoo_sample *s, int32_t n)
{
    auto c = static_cast<codec *>(dec);
    assert(c->format.header.blocksize != 0);

    const int ch = std::max(1, (int)c->format.header.nchannels);
    const int srate = c->format.header.samplerate > 0
                        ? c->format.header.samplerate : 48000;

    if (!buf){
        // Lost block → waveform continuation
        // with progressive fade instead of stock zero-fill (click).
        plc_conceal(c, s, n / ch, ch, srate);
        return 0;
    }

    auto samplesize = bytes_per_sample(c->format.bitdepth);

    if (n < (size / samplesize)){
        return 0;
    }

    auto blob_to_samples = [&](auto convfn){
        auto b = buf;
        for (int i = 0; i < n; ++i, b += samplesize){
            s[i] = convfn(b);
        }
    };

    switch (c->format.bitdepth){
    case AOO_PCM_INT16: {
        // Bulk decode path, mirrors encode.
        auto* b = (const uint8_t*)buf;
        for (int i = 0; i < n; ++i){
            int16_t t = (int16_t)((uint16_t)((b[2*i] << 8) | b[2*i+1]));
            s[i] = (aoo_sample)t / 32768.f;
        }
        break;
    }
    case AOO_PCM_INT24:
        blob_to_samples(int24_to_sample);
        break;
    case AOO_PCM_FLOAT32:
        blob_to_samples(float32_to_sample);
        break;
    case AOO_PCM_FLOAT64:
        blob_to_samples(float64_to_sample);
        break;
    default:
        // unknown bitdepth
        return 0;
    }

    // First good block after a loss: de-click the transition from concealment to the real signal.
    const int frames = n / ch;
    if (c->plcConcealed > 0){
        // Fade in the real signal. A crossfade against the loop continuation would be phase-mismatched
        // and sound like chorus on frequent small conceals.
        const int xf = std::min(frames, codec::kXfadeFrames);
        for (int f = 0; f < xf; ++f){
            float w = (float)(f + 1) / (float)xf;
            for (int k = 0; k < ch; ++k) s[(size_t)f * ch + k] *= w;
        }
    }
    c->plcConcealed = 0;
    plc_append(c, s, frames, ch);

    return size / samplesize;
}

int32_t decoder_readformat(void *dec, aoo_format *fmt,
                           const char *buf, int32_t size)
{
    if (size >= 4){
        auto c = static_cast<codec *>(dec);
        // TODO validate
        if (!strcmp(fmt->codec, AOO_CODEC_PCM) && fmt->blocksize > 0
                && fmt->samplerate > 0 && fmt->blocksize > 0)
        {
            memcpy(&c->format.header, fmt, sizeof(aoo_format));
            c->format.bitdepth = (aoo_pcm_bitdepth)aoo::from_bytes<int32_t>(buf);
            c->format.header.codec = AOO_CODEC_PCM; // !
            print_settings(c->format);

            // Decoders learn their real format
            // HERE (from the wire), not via codec_setformat — (re)allocate
            // history for the actual channel count. Runs on format
            // negotiation (control moment), not steady-state audio.
            {
                int ch = std::max(1, (int)c->format.header.nchannels);
                c->plcHist.assign((size_t)codec::kHistFrames * ch, 0);
                c->plcWrite = 0; c->plcFilled = 0;
                c->plcConcealed = 0; c->plcAgo = 0; c->plcPeriod = 0;
            }

            return 4;
        } else {
            LOG_ERROR("PCM: bad format!");
        }
    } else {
        LOG_ERROR("PCM: couldn't read format - not enough data!");
    }
    return -1;
}

aoo_codec codec_class = {
    AOO_CODEC_PCM,
    encoder_new,
    encoder_free,
    codec_setformat,
    codec_getformat,
    encoder_readformat,
    encoder_writeformat,
    encoder_encode,
    codec_reset,
    decoder_new,
    decoder_free,
    codec_setformat,
    codec_getformat,
    decoder_readformat,
    decoder_decode,
    codec_reset
};

} // namespace

void aoo_codec_pcm_setup(aoo_codec_registerfn fn){
    fn(AOO_CODEC_PCM, &codec_class);
}

