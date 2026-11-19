// SPDX-License-Identifier: GPLv3-or-later WITH Appstore-exception
// Copyright (C) 2021 Jesse Chappell


#include "ChannelGroup.h"

static String nameKey("name");

static String gainKey("gain");
static String channelStartIndexKey("chanstart");
static String numChannelsKey("numchan");
static String monReverbSendKey("monreverbsend");
static String inReverbSendKey("inreverbsend");
static String monitorLevelKey("monitorlev");
static String peerMonoPanKey("pan");
static String peerPan1Key("span1");
static String peerPan2Key("span2");
static String panDestStartKey("pandeststart");
static String panDestChannelsKey("pandestchans");
static String monDestStartKey("mondeststart");
static String monDestChannelsKey("mondestchans");
static String sendMainMixKey("sendmainmix");
static String invertPolarityKey("invertpolarity");
static String mutedKey("muted");


static String stereoPanListKey("StereoPanners");

static String panStateKey("Panner");
static String panAttrKey("pan");


static String compressorStateKey("CompressorState");
static String expanderStateKey("ExpanderState");
static String limiterStateKey("LimiterState");
static String eqStateKey("ParametricEqState");
static String monDelayStateKey("MonitorDelayState");

static String inputChannelGroupsStateKey("InputChannelGroups");
static String channelGroupStateKey("ChannelGroup");
static String numChanGroupsKey("numChanGroups");

static String layoutGroupsKey("Layout");


#define MAX_DELAY_SAMPLES 240000 // 5 seconds at 48k

using namespace SonoAudio;

// Runs an effect in place. While it is being switched, the direct and the processed signal are crossfaded.
// bufs holds the effect's two channels; the first nbufs of them carry audio.
template <typename Compute>
static void processFaded (FxFade & fade, bool on, float * const * bufs, int nbufs, int numSamples, Compute && compute)
{
    int done = 0;
    while (done < numSamples && fade.fading(on)) {
        const int n = jmin((int) FxFade::chunk, numSamples - done);
        float dry[2][FxFade::chunk];
        float * seg[2] = { bufs[0] + done, bufs[1] + done };
        for (int b = 0; b < nbufs; ++b) FloatVectorOperations::copy(dry[b], seg[b], n);
        compute(seg, n);
        FxFade f = fade;
        for (int b = 0; b < nbufs; ++b) { f = fade; f.mix(seg[b], dry[b], n, on); }
        fade = f;
        done += n;
    }
    if (on && done < numSamples) {
        float * seg[2] = { bufs[0] + done, bufs[1] + done };
        compute(seg, numSamples - done);
    }
}

ChannelGroup::ChannelGroup()
{
}

// copy assignment
void ChannelGroup::copyParametersFrom(const ChannelGroup& other)
{
    params = other.params;

    compressorParamsChanged = true;
    expanderParamsChanged = true;
    eqParamsChanged = true;
    limiterParamsChanged = true;
    monitorDelayParamsChanged = true;

}

void ChannelGroupParams::setToDefaults(bool isplugin)
{
    expanderParams = gateDefaultParams();

    if (compressorParams.automakeupGain) {
        // makeupgain = (-thresh -  abs(thresh/ratio))/2
        compressorParams.makeupGainDb = (-compressorParams.thresholdDb - abs(compressorParams.thresholdDb/compressorParams.ratio)) * 0.5f;
    }

    limiterParams.enabled = !isplugin; // input limiter disabled by default in plugin
    limiterParams.thresholdDb = -1.0f;
    limiterParams.ratio = 4.0f;
    limiterParams.attackMs = 0.01;
    limiterParams.releaseMs = 100.0;

    monitorDelayParams.enabled = false;
    monitorDelayParams.delayTimeMs = 0.0;
}

void ChannelGroup::init(double sampRate)
{
    sampleRate = sampRate;
    
    if (!compressor) {
        compressor = std::make_unique<faustCompressor>();
        compressorControl = std::make_unique<MapUI>();
    }
    compressor->init(sampleRate);
    compressor->buildUserInterface(compressorControl.get());

    //DBG("Compressor Params:");
    //for(int i=0; i < mInputCompressorControl.getParamsCount(); i++){
    //    DBG(mInputCompressorControl.getParamAddress(i));
    //}

    if (!expander) {
        expander = std::make_unique<faustExpander>();
        expanderControl = std::make_unique<MapUI>();
    }

    expander->init(sampleRate);
    gateSampleRate = sampleRate;   // Hard gate
    expander->buildUserInterface(expanderControl.get());

    //DBG("Expander Params:");
    //for(int i=0; i < mInputExpanderControl.getParamsCount(); i++){
    //    DBG(mInputExpanderControl.getParamAddress(i));
    //}

    for (int j=0; j < 2; ++j) {
        if (!eq[j]) {
            eq[j] = std::make_unique<faustParametricEQ>();
            eqControl[j] = std::make_unique<MapUI>();
        }
        eq[j]->init(sampleRate);
        eq[j]->buildUserInterface(eqControl[j].get());
    }

    //DBG("EQ Params:");
    //for(int i=0; i < mInputEqControl[0].getParamsCount(); i++){
    //    DBG(mInputEqControl[0].getParamAddress(i));
    //}

    if (!limiter) {
        limiter = std::make_unique<faustCompressor>();
        limiterControl = std::make_unique<MapUI>();
    }

    limiter->init(sampleRate);
    limiter->buildUserInterface(limiterControl.get());

    //DBG("Limiter Params:");
    //for(int i=0; i < mInputLimiterControl.getParamsCount(); i++){
    //    DBG(mInputLimiterControl.getParamAddress(i));
    //}

    compressorZones.knee = compressorControl->getParamZone("/compressor/knee");
    compressorZones.threshold = compressorControl->getParamZone("/compressor/threshold");
    compressorZones.ratio = compressorControl->getParamZone("/compressor/ratio");
    compressorZones.attack = compressorControl->getParamZone("/compressor/attack");
    compressorZones.release = compressorControl->getParamZone("/compressor/release");
    compressorZones.makeupGain = compressorControl->getParamZone("/compressor/makeup_gain");
    compressorOutputLevel = compressorControl->getParamZone("/compressor/outgain");

    expanderZones.knee = expanderControl->getParamZone("/expander/knee");
    expanderZones.threshold = expanderControl->getParamZone("/expander/threshold");
    expanderZones.ratio = expanderControl->getParamZone("/expander/ratio");
    expanderZones.attack = expanderControl->getParamZone("/expander/attack");
    expanderZones.release = expanderControl->getParamZone("/expander/release");
    expanderOutputGain = expanderControl->getParamZone("/expander/gain");

    limiterZones.threshold = limiterControl->getParamZone("/compressor/threshold");
    limiterZones.ratio = limiterControl->getParamZone("/compressor/ratio");
    limiterZones.attack = limiterControl->getParamZone("/compressor/attack");
    limiterZones.release = limiterControl->getParamZone("/compressor/release");

    static const char * const eqPaths[10] = {
        "/parametric_eq/low_shelf/gain", "/parametric_eq/low_shelf/transition_freq",
        "/parametric_eq/para1/peak_gain", "/parametric_eq/para1/peak_frequency", "/parametric_eq/para1/peak_q",
        "/parametric_eq/para2/peak_gain", "/parametric_eq/para2/peak_frequency", "/parametric_eq/para2/peak_q",
        "/parametric_eq/high_shelf/gain", "/parametric_eq/high_shelf/transition_freq" };
    for (int j=0; j < 2; ++j) {
        for (int k=0; k < 10; ++k) {
            eqZones[j][k] = eqControl[j]->getParamZone(eqPaths[k]);
        }
    }

    compressorFade.prepare(sampleRate);
    expanderFade.prepare(sampleRate);
    eqFade.prepare(sampleRate);
    limiterFade.prepare(sampleRate);
    monitorDelayFade.prepare(sampleRate);
    monitorDelayTimeFade.prepare(sampleRate);

    commitCompressorParams();
    commitExpanderParams();
    commitEqParams();
    commitLimiterParams();
    commitMonitorDelayParams();

}

void ChannelGroup::setMonitoringDelayEnabled(bool enabled, int numchans)
{
    if (enabled) {
        if (!monitorDelayLine) {
            const ScopedLock lock(_monitorDelayLock);
            // need to initialize it
            monitorDelayLine = std::make_unique<juce::dsp::DelayLine<float,juce::dsp::DelayLineInterpolationTypes::None> >(MAX_DELAY_SAMPLES);
            monitorDelayLine->setDelay(_monitorDelayTimeSamples);

            juce::dsp::ProcessSpec delayspec = { sampleRate, 4096, (juce::uint32) numchans };
            monitorDelayLine->prepare(delayspec);

            _monitorDelayChans = numchans;
            delayWorkBuffer.setSize(numchans, 4096, false, false, true);
        }
        else if (_monitorDelayChans != numchans) {
            //
            const ScopedLock lock(_monitorDelayLock);

            juce::dsp::ProcessSpec delayspec = { sampleRate, 4096, (juce::uint32) numchans };
            monitorDelayLine->prepare(delayspec);
            monitorDelayLine->reset();
            _monitorDelayChans = numchans;
            delayWorkBuffer.setSize(numchans, 4096, false, false, true);
        }

        


        // if monitorDelayActive is ever true, it means it has already been initialized
        params.monitorDelayParams.enabled = true;
        _monitorDelayActive = true;
    }
    else {
        params.monitorDelayParams.enabled = false;
        _monitorDelayActive = false;
    }
}

void ChannelGroup::setMonitoringDelayTimeMs(double delayms)
{
    params.monitorDelayParams.delayTimeMs = delayms;
    auto newsamps = 1e-3 * delayms * sampleRate;
    if ( fabs(_monitorDelayTimeSamples - newsamps) > 1) {
        _monitorDelayTimeSamples = jmin(newsamps, (double)MAX_DELAY_SAMPLES);
        _monitorDelayTimeChanged = true;
    }
}


void ChannelGroup::processBlock (AudioBuffer<float>& frombuffer,
                                 AudioBuffer<float>& tobuffer, int destStartChan, int destNumChans,
                                 AudioBuffer<float>& silentBuffer,
                                 int numSamples, float gainfactor, ProcessState * oprocstate,
                                 AudioBuffer<float> * reverbbuffer, int revStartChan, int revNumChans, bool revEnabled, float revgainfactor, ProcessState * orevprocstate)
{
    // called from audio thread context

    auto & procstate = oprocstate != nullptr ? *oprocstate : mainProcState;
    auto & revprocstate = orevprocstate != nullptr ? *orevprocstate : inRevProcState;

    int chstart = params.chanStartIndex;
    int numchan = params.numChannels;
    const int frombufNumChan = frombuffer.getNumChannels();
    const int tobufNumChan = tobuffer.getNumChannels();
    // apply input gain


    float dogain = (params.muted ? 0.0f : params.gain) * gainfactor;
    //dogain = 0.0f;

    dogain *= params.invertPolarity ? -1.0f : 1.0f;

    if (&frombuffer == &tobuffer) {
        // inplace, just apply gain, ignore destchans
        for (int i = chstart; i < chstart+numchan && i < frombufNumChan ; ++i) {
            tobuffer.applyGainRamp(i, 0, numSamples, procstate.lastlevel, dogain);
        }
    }
    else {
        for (int i = chstart, desti=destStartChan; i < chstart+numchan && i < frombufNumChan && desti < destStartChan+destNumChans && desti < tobufNumChan; ++i, ++desti) {
            tobuffer.addFromWithRamp(desti, 0, frombuffer.getReadPointer(i), numSamples, procstate.lastlevel, dogain);
        }
    }


    procstate.lastlevel = dogain;

    // these all operate ONLY when the channel group has 1 or 2 channels (and when the effects have been initialized)
    if (params.numChannels > 0 && params.numChannels <= 2 && compressor)
    {
        // the effect's two channels (mono: the second one is a silent dummy)
        float * bufs[2] = { nullptr, nullptr };
        int nbufs = 0;
        if (tobufNumChan - destStartChan > 1 && numchan == 2 && destNumChans >= 2) {
            bufs[0] = tobuffer.getWritePointer(destStartChan);
            bufs[1] = tobuffer.getWritePointer(destStartChan+1);
            nbufs = 2;
        } else if (destStartChan < tobufNumChan) {
            bufs[0] = tobuffer.getWritePointer(destStartChan);
            bufs[1] = silentBuffer.getWritePointer(0);
            nbufs = 1;
        }

        // Each effect: switched on, it starts from a cleared state; on and off are crossfaded with the direct signal.

        // apply input expander
        if (expanderParamsChanged) {
            commitExpanderParams();
            expanderParamsChanged = false;
        }
        const bool expanderOn = params.expanderParams.enabled;
        const bool hardGate = expanderOn && params.expanderParams.gate;
        if (nbufs > 0 && expanderFade.running(expanderOn)) {
            if (expanderOn && expanderFade.isOff()) {
                expander->instanceClear();
                gateGain = 1.0f; gateOpen = true; gateHoldLeft = 0;
            }
            processFaded(expanderFade, expanderOn, bufs, nbufs, numSamples, [&](float ** seg, int n) {
                expander->compute(n, seg, seg);
                if (hardGate || gateGain < 1.0f) applyHardGate(seg, nbufs, n, hardGate);
            });
        }
        if (!hardGate) gateStatePub.store(-1, std::memory_order_relaxed);


        // apply input compressor
        if (compressorParamsChanged) {
            commitCompressorParams();
            compressorParamsChanged = false;
        }
        const bool compressorOn = params.compressorParams.enabled;
        if (nbufs > 0 && compressorFade.running(compressorOn)) {
            if (compressorOn && compressorFade.isOff()) compressor->instanceClear();
            processFaded(compressorFade, compressorOn, bufs, nbufs, numSamples, [&](float ** seg, int n) {
                compressor->compute(n, seg, seg);
            });
        }


        // apply input EQ
        if (eqParamsChanged) {
            commitEqParams();
            eqParamsChanged = false;
        }
        const bool eqOn = params.eqParams.enabled;
        if (nbufs > 0 && eqFade.running(eqOn)) {
            if (eqOn && eqFade.isOff()) {
                for (int j=0; j < 2; ++j) {
                    eq[j]->instanceClear();
                    eq[j]->settleControls();
                }
            }
            // only 2 channels support for now... TODO
            processFaded(eqFade, eqOn, bufs, nbufs, numSamples, [&](float ** seg, int n) {
                eq[0]->compute(n, &seg[0], &seg[0]);
                if (nbufs > 1) eq[1]->compute(n, &seg[1], &seg[1]);
            });
        }


        // apply input limiter
        if (limiterParamsChanged) {
            commitLimiterParams();
            limiterParamsChanged = false;
        }
        const bool limiterOn = params.limiterParams.enabled;
        if (nbufs > 0 && limiterFade.running(limiterOn)) {
            if (limiterOn && limiterFade.isOff()) limiter->instanceClear();
            processFaded(limiterFade, limiterOn, bufs, nbufs, numSamples, [&](float ** seg, int n) {
                limiter->compute(n, seg, seg);
            });
        }
    }
    
    // apply to reverb buffer
    if (reverbbuffer) {
        processReverbSend(tobuffer, destStartChan, jmin(params.numChannels, destNumChans), *reverbbuffer, revStartChan, revNumChans, numSamples, revEnabled, true, revgainfactor, &revprocstate);
    }
}

void ChannelGroup::processPan (AudioBuffer<float>& frombuffer, int fromStartChan,
                               AudioBuffer<float>& tobuffer, int destStartChan, int destNumChans,
                               int numSamples, float gainfactor, ProcessState * oprocstate,
                               const float * balGain, const float * balLastGain)
{
    int fromNumChan = frombuffer.getNumChannels();
    int toNumChan = tobuffer.getNumChannels();

    if (fromNumChan == 0) return;

    auto & procstate = oprocstate != nullptr ? *oprocstate : mainProcState;


    if (destNumChans == 2) {
        //tobuffer.clear(0, numSamples);

        for (int channel = destStartChan; channel < destStartChan + destNumChans && channel < toNumChan; ++channel) {
            const float bal = balGain ? balGain[channel - destStartChan] : 1.0f;
            const float lastbal = balLastGain ? balLastGain[channel - destStartChan] : bal;
            int pani = 0;
            for (int i=fromStartChan; i < fromStartChan + params.numChannels && i < fromNumChan; ++i, ++pani) {
                const float upan = (params.numChannels != 2 ? params.pan[pani] : i==fromStartChan ? params.panStereo[0] : params.panStereo[1]);
                const float lastpan = (params.numChannels != 2 ? procstate.lastpan[pani] : i==fromStartChan ? procstate.laststereopan[0] : procstate.laststereopan[1]);

                // -1 is left, 1 is right
                float pgain = channel == destStartChan ? (upan >= 0.0f ? (1.0f - upan) : 1.0f) : (upan >= 0.0f ? 1.0f : (1.0f+upan)) ;

                // apply pan law
                pgain *= params.centerPanLaw + (fabsf(upan) * (1.0f - params.centerPanLaw));
                pgain *= gainfactor * bal;

                if (fabsf(upan - lastpan) > 0.00001f || bal != lastbal) {
                    float plastgain = channel == destStartChan ? (lastpan >= 0.0f ? (1.0f - lastpan) : 1.0f) : (lastpan >= 0.0f ? 1.0f : (1.0f+lastpan));
                    plastgain *= params.centerPanLaw + (fabsf(lastpan) * (1.0f - params.centerPanLaw));
                    plastgain *= gainfactor * lastbal;

                    tobuffer.addFromWithRamp(channel, 0, frombuffer.getReadPointer(i), numSamples, plastgain, pgain);
                } else {
                    tobuffer.addFrom (channel, 0, frombuffer, i, 0, numSamples, pgain);
                }
            }
        }
    }
    else if (destNumChans == 1){
        // sum all into destChan
        int channel = destStartChan;
        for (int srcchan = fromStartChan; srcchan < fromStartChan + params.numChannels && srcchan < fromNumChan && channel < toNumChan; ++srcchan) {

            tobuffer.addFrom(channel, 0, frombuffer, srcchan, 0, numSamples, gainfactor);
        }
    }
    else {
        // straight thru to dests - no panning
        int srcchan = fromStartChan;
        for (int channel = destStartChan; srcchan < fromStartChan + params.numChannels && srcchan < fromNumChan && channel < toNumChan; ++channel, ++srcchan) {
            //int srcchan = channel < mainBusInputChannels ? channel : mainBusInputChannels - 1;
            //int srcchan = chanStartIndex  channel < mainBusInputChannels ? channel : mainBusInputChannels - 1;

            tobuffer.addFrom(channel, 0, frombuffer, srcchan, 0, numSamples, gainfactor);
        }
    }
    

    procstate.laststereopan[0] = params.panStereo[0];
    procstate.laststereopan[1] = params.panStereo[1];
    for (int pani=0; pani < params.numChannels; ++pani) {
        procstate.lastpan[pani] = params.pan[pani];
    }
}

void ChannelGroup::processMonitor (AudioBuffer<float>& frombuffer, int fromStartChan,
                                   AudioBuffer<float>& tobuffer, int destStartChan, int destNumChans,
                                   int numSamples, float gainfactor, ProcessState * oprocstate,
                                   AudioBuffer<float> * reverbbuffer, int revStartChan, int revNumChans, bool revEnabled, float revgainfactor, ProcessState * orevprocstate)
{

    // apply monitor level

    float targmon = params.monitor * gainfactor;

    int fromNumChan = frombuffer.getNumChannels();
    int toNumChan = tobuffer.getNumChannels();

    auto & procstate = oprocstate != nullptr ? *oprocstate : monProcState;
    auto & revprocstate = orevprocstate != nullptr ? *orevprocstate : revProcState;


    if (monitorDelayParamsChanged) {
        commitMonitorDelayParams();
        monitorDelayParamsChanged = false;
    }

    const bool domondelay = _monitorDelayActive.load();

    auto * usefrombuffer = &frombuffer;
    auto useFromStartChan = fromStartChan;
    auto useFromNumChan = fromNumChan;

    if (monitorDelayFade.running(domondelay)) {
        const ScopedTryLock lock(_monitorDelayLock);
        // should match dest num channels, but we need to make sure of it
        if (lock.isLocked() && monitorDelayLine && params.numChannels == _monitorDelayChans && numSamples <= delayWorkBuffer.getNumSamples()) {

            if (domondelay && monitorDelayFade.isOff()) {
                // a fresh start
                _monitorDelayTimeChanged = false;
                _monitorDelayTimeChanging = false;
                monitorDelayLine->reset();
                monitorDelayLine->setDelay(_monitorDelayTimeSamples);
                monitorDelayTimeFade.snapTo(true);
            }
            else if (_monitorDelayTimeChanging) {
                if (monitorDelayTimeFade.isOff()) {
                    // faded out, now apply the new delay time and fade back in
                    monitorDelayLine->setDelay(_monitorDelayTimeSamples);
                    _monitorDelayTimeChanging = false;
                }
            }
            else if (domondelay && _monitorDelayTimeChanged.load()) {
                _monitorDelayTimeChanged = false;
                _monitorDelayTimeChanging = true;
            }

            const bool timeOn = !_monitorDelayTimeChanging;
            const bool fading = monitorDelayFade.fading(domondelay);
            const int nch = jmin(params.numChannels, delayWorkBuffer.getNumChannels(), fromNumChan - fromStartChan);
            FxFade inFade = monitorDelayFade, outFade = monitorDelayFade, timeFade = monitorDelayTimeFade;

            // going in: faded with the switch, so the delayed signal starts from silence
            for (int chan = 0; chan < nch; ++chan) {
                delayWorkBuffer.copyFrom(chan, 0, frombuffer.getReadPointer(fromStartChan + chan), numSamples);
                if (fading) {
                    inFade = monitorDelayFade;
                    inFade.gain(delayWorkBuffer.getWritePointer(chan), numSamples, domondelay);
                }
            }

            auto delayinoutBlock = dsp::AudioBlock<float>(delayWorkBuffer).getSubsetChannelBlock(0, (size_t)params.numChannels).getSubBlock(0, numSamples);
            monitorDelayLine->process(dsp::ProcessContextReplacing<float>(delayinoutBlock));

            // coming out: crossfade between the direct and the delayed signal
            for (int chan = 0; chan < nch; ++chan) {
                float * delayed = delayWorkBuffer.getWritePointer(chan);
                if (!timeOn || !monitorDelayTimeFade.isOn()) {
                    timeFade = monitorDelayTimeFade;
                    timeFade.gain(delayed, numSamples, timeOn);
                }
                if (fading) {
                    outFade = monitorDelayFade;
                    outFade.mix(delayed, frombuffer.getReadPointer(fromStartChan + chan), numSamples, domondelay);
                }
            }

            if (nch > 0) {
                monitorDelayFade = outFade;
                monitorDelayTimeFade = timeFade;
            } else {
                monitorDelayFade.snapTo(domondelay);
            }

            usefrombuffer = &delayWorkBuffer;
            useFromStartChan = 0;
            useFromNumChan = nch;
        }
    }

    if (useFromNumChan > 0 && destNumChans == 2) {
        //tobuffer.clear(0, numSamples);

        for (int channel = destStartChan; channel < destStartChan + destNumChans && channel < toNumChan; ++channel) {
            int pani = 0;
            for (int i=useFromStartChan; i < useFromStartChan + params.numChannels && i < useFromNumChan; ++i, ++pani) {
                const float upan = (params.numChannels != 2 ? params.pan[pani] : i==useFromStartChan ? params.panStereo[0] : params.panStereo[1]);
                const float lastpan = (params.numChannels != 2 ? procstate.lastpan[pani] : i==useFromStartChan ? procstate.laststereopan[0] : procstate.laststereopan[1]);

                // -1 is left, 1 is right
                float pgain = channel == destStartChan ? (upan >= 0.0f ? (1.0f - upan) : 1.0f) : (upan >= 0.0f ? 1.0f : (1.0f+upan)) ;

                // apply pan law
                pgain *= params.centerPanLaw + (fabsf(upan) * (1.0f - params.centerPanLaw));
                pgain *= targmon;

                if (fabsf(upan - lastpan) > 0.00001f || fabsf(procstate.lastlevel - targmon) > 0.00001f) {
                    float plastgain = channel == destStartChan ? (lastpan >= 0.0f ? (1.0f - lastpan) : 1.0f) : (lastpan >= 0.0f ? 1.0f : (1.0f+lastpan));
                    plastgain *= params.centerPanLaw + (fabsf(lastpan) * (1.0f - params.centerPanLaw));
                    plastgain *= procstate.lastlevel;

                    tobuffer.addFromWithRamp(channel, 0, usefrombuffer->getReadPointer(i), numSamples, plastgain, pgain);
                } else {
                    tobuffer.addFrom (channel, 0, *usefrombuffer, i, 0, numSamples, pgain);
                }
            }
        }
    }
    else if (useFromNumChan > 0 && destNumChans == 1){
        // sum all into destChan
        int channel = destStartChan;
        for (int srcchan = useFromStartChan; srcchan < useFromStartChan + params.numChannels && srcchan < useFromNumChan && channel < toNumChan; ++srcchan) {

            tobuffer.addFromWithRamp(channel, 0, usefrombuffer->getReadPointer(srcchan), numSamples, procstate.lastlevel, targmon);

        }
    }
    else if (useFromNumChan > 0){
        // straight thru to dests - no panning
        int srcchan = useFromStartChan;
        for (int channel = destStartChan; srcchan < useFromStartChan + params.numChannels && srcchan < useFromNumChan && channel < toNumChan; ++channel, ++srcchan) {
//        for (int channel = destStartChan + chanStartIndex; channel < destStartChan + destNumChans && srcchan < chanStartIndex + numChannels && srcchan < fromNumChan && channel < toNumChan; ++channel, ++srcchan) {
            //int srcchan = channel < mainBusInputChannels ? channel : mainBusInputChannels - 1;
            //int srcchan = chanStartIndex  channel < mainBusInputChannels ? channel : mainBusInputChannels - 1;

            tobuffer.addFromWithRamp(channel, 0, usefrombuffer->getReadPointer(srcchan), numSamples, procstate.lastlevel, targmon);
        }
    }

    // apply to reverb buffer
    if (reverbbuffer) {
        processReverbSend(*usefrombuffer, useFromStartChan, jmin(params.numChannels, useFromNumChan), *reverbbuffer, revStartChan, revNumChans, numSamples, revEnabled, false, targmon * revgainfactor, &revprocstate);
    }

    procstate.laststereopan[0] = params.panStereo[0];
    procstate.laststereopan[1] = params.panStereo[1];
    for (int pani=0; pani < params.numChannels; ++pani) {
        procstate.lastpan[pani] = params.pan[pani];
    }

    procstate.lastlevel = targmon;

}

void ChannelGroup::processReverbSend (AudioBuffer<float>& frombuffer, int fromStartChan, int fromNumChans,
                                      AudioBuffer<float>& tobuffer, int destStartChan, int destNumChans,
                                      int numSamples, bool revEnabled, bool inSend, float gainfactor,  ProcessState * oprocstate)
{
    int fromMaxChans = frombuffer.getNumChannels();
    int destMaxChans = tobuffer.getNumChannels();

    auto & procstate = oprocstate != nullptr ? *oprocstate : revProcState;

    const float targrevgain = gainfactor * (inSend ? params.inReverbSend : params.monReverbSend) * (revEnabled ? 1.0f : 0.0f);
    const float lastrevgain = procstate.lastlevel;

    if (fromNumChans > 0 && destNumChans == 2) {
        //tobuffer.clear(0, numSamples);

        for (int channel = destStartChan; channel < destStartChan + destNumChans && channel < destMaxChans; ++channel) {
            int pani = 0;
            for (int i=fromStartChan; i < fromStartChan + fromNumChans && i < fromMaxChans; ++i, ++pani) {
                const float upan = (fromNumChans != 2 ? params.pan[pani] : i==fromStartChan ? params.panStereo[0] : params.panStereo[1]);
                const float lastpan = (params.numChannels != 2 ? procstate.lastpan[pani] : i==fromStartChan ? procstate.laststereopan[0] : procstate.laststereopan[1]);

                // -1 is left, 1 is right
                float pgain = channel == destStartChan ? (upan >= 0.0f ? (1.0f - upan) : 1.0f) : (upan >= 0.0f ? 1.0f : (1.0f+upan)) ;

                // apply pan law
                pgain *= params.centerPanLaw + (fabsf(upan) * (1.0f - params.centerPanLaw));
                pgain *= targrevgain;

                if (fabsf(upan - lastpan) > 0.00001f || fabsf(lastrevgain - targrevgain) > 0.00001f) {
                    float plastgain = channel == destStartChan ? (lastpan >= 0.0f ? (1.0f - lastpan) : 1.0f) : (lastpan >= 0.0f ? 1.0f : (1.0f+lastpan));
                    plastgain *= params.centerPanLaw + (fabsf(lastpan) * (1.0f - params.centerPanLaw));
                    plastgain *= lastrevgain;

                    tobuffer.addFromWithRamp(channel, 0, frombuffer.getReadPointer(i), numSamples, plastgain, pgain);
                } else {
                    tobuffer.addFrom (channel, 0, frombuffer, i, 0, numSamples, pgain);
                }
            }
        }
    }
    else if (fromNumChans > 0 && destNumChans == 1){
        // sum all into destChan
        int channel = destStartChan;
        for (int srcchan = fromStartChan; srcchan < fromStartChan + fromNumChans && srcchan < fromMaxChans && channel < destMaxChans; ++srcchan) {

            tobuffer.addFromWithRamp(channel, 0, frombuffer.getReadPointer(srcchan), numSamples, lastrevgain, targrevgain);

        }
    }
    else if (fromNumChans > 0){
        // straight thru to dests - no panning
        int srcchan = fromStartChan;
        for (int channel = destStartChan; srcchan < fromStartChan + fromNumChans && srcchan < fromMaxChans && channel < destMaxChans; ++channel, ++srcchan) {
//        for (int channel = destStartChan + chanStartIndex; channel < destStartChan + destNumChans && srcchan < chanStartIndex + numChannels && srcchan < fromNumChan && channel < toNumChan; ++channel, ++srcchan) {
            //int srcchan = channel < mainBusInputChannels ? channel : mainBusInputChannels - 1;
            //int srcchan = chanStartIndex  channel < mainBusInputChannels ? channel : mainBusInputChannels - 1;

            tobuffer.addFromWithRamp(channel, 0, frombuffer.getReadPointer(srcchan), numSamples, lastrevgain, targrevgain);
        }
    }

    procstate.laststereopan[0] = params.panStereo[0];
    procstate.laststereopan[1] = params.panStereo[1];
    for (int pani=0; pani < params.numChannels; ++pani) {
        procstate.lastpan[pani] = params.pan[pani];
    }

    procstate.lastlevel = targrevgain;
}



String ChannelGroupParams::getValueTreeKey() const
{
    return channelGroupStateKey;
}

ValueTree ChannelGroupParams::getValueTree() const
{
    ValueTree channelGroupTree(channelGroupStateKey);

    channelGroupTree.setProperty(gainKey, gain, nullptr);
    channelGroupTree.setProperty(channelStartIndexKey, chanStartIndex, nullptr);
    channelGroupTree.setProperty(numChannelsKey, numChannels, nullptr);
    channelGroupTree.setProperty(mutedKey, muted, nullptr);

    channelGroupTree.setProperty(peerPan1Key, panStereo[0], nullptr);
    channelGroupTree.setProperty(peerPan2Key, panStereo[1], nullptr);
    channelGroupTree.setProperty(monReverbSendKey, monReverbSend, nullptr);
    channelGroupTree.setProperty(inReverbSendKey, inReverbSend, nullptr);
    channelGroupTree.setProperty(monitorLevelKey, monitor, nullptr);

    channelGroupTree.setProperty(monDestStartKey, monDestStartIndex, nullptr);
    channelGroupTree.setProperty(monDestChannelsKey, monDestChannels, nullptr);
    channelGroupTree.setProperty(panDestStartKey, panDestStartIndex, nullptr);
    channelGroupTree.setProperty(panDestChannelsKey, panDestChannels, nullptr);


    channelGroupTree.setProperty(sendMainMixKey, sendMainMix, nullptr);
    channelGroupTree.setProperty(invertPolarityKey, invertPolarity, nullptr);

    channelGroupTree.setProperty(nameKey, name, nullptr);

    ValueTree panListTree(stereoPanListKey);
    for (int i=0; i < numChannels && i < MAX_CHANNELS; ++i) {
        ValueTree panner(panStateKey);
        panner.setProperty(panAttrKey, pan[i], nullptr);
        panListTree.appendChild(panner, nullptr);
    }

    channelGroupTree.appendChild(panListTree, nullptr);


    channelGroupTree.appendChild(compressorParams.getValueTree(compressorStateKey), nullptr);
    channelGroupTree.appendChild(expanderParams.getValueTree(expanderStateKey), nullptr);
    channelGroupTree.appendChild(limiterParams.getValueTree(limiterStateKey), nullptr);
    channelGroupTree.appendChild(eqParams.getValueTree(), nullptr);
    channelGroupTree.appendChild(monitorDelayParams.getValueTree(monDelayStateKey), nullptr);

    return channelGroupTree;
}

void ChannelGroupParams::setFromValueTree(const ValueTree & channelGroupTree)
{
    gain = channelGroupTree.getProperty(gainKey, gain);
    chanStartIndex = channelGroupTree.getProperty(channelStartIndexKey, chanStartIndex);
    numChannels = channelGroupTree.getProperty(numChannelsKey, numChannels);
    muted = channelGroupTree.getProperty(mutedKey, muted);
    //pan = channelGroupTree.getProperty(peerMonoPanKey, pan);
    panStereo[0] = channelGroupTree.getProperty(peerPan1Key, panStereo[0]);
    panStereo[1] = channelGroupTree.getProperty(peerPan2Key, panStereo[1]);
    monReverbSend = channelGroupTree.getProperty(monReverbSendKey, monReverbSend);
    inReverbSend = channelGroupTree.getProperty(inReverbSendKey, inReverbSend);
    monitor = channelGroupTree.getProperty(monitorLevelKey, monitor);

    panDestStartIndex = channelGroupTree.getProperty(panDestStartKey, panDestStartIndex);
    panDestChannels = channelGroupTree.getProperty(panDestChannelsKey, panDestChannels);

    monDestStartIndex = channelGroupTree.getProperty(monDestStartKey, monDestStartIndex);
    monDestChannels = channelGroupTree.getProperty(monDestChannelsKey, monDestChannels);

    sendMainMix = channelGroupTree.getProperty(sendMainMixKey, sendMainMix);
    invertPolarity = channelGroupTree.getProperty(invertPolarityKey, invertPolarity);

    name = channelGroupTree.getProperty(nameKey, name);

    ValueTree panListTree = channelGroupTree.getChildWithName(stereoPanListKey);
    if (panListTree.isValid()) {

        int i=0;
        for (auto panner : panListTree) {
            if (panner.isValid() && i < MAX_CHANNELS) {
                pan[i] = panner.getProperty(panAttrKey, pan[i]);
            }

            ++i;
        }
    }

    ValueTree compressorTree = channelGroupTree.getChildWithName(compressorStateKey);
    if (compressorTree.isValid()) {
        compressorParams.setFromValueTree(compressorTree);
    }
    ValueTree expanderTree = channelGroupTree.getChildWithName(expanderStateKey);
    if (expanderTree.isValid()) {
        expanderParams.setFromValueTree(expanderTree);
        // a gate saved unused with the compressor's start values gets the gate's own
        if (!expanderParams.enabled && expanderParams.thresholdDb == -16.0f && expanderParams.ratio == 2.0f
            && expanderParams.attackMs == 10.0f && expanderParams.releaseMs == 80.0f) {
            expanderParams = gateDefaultParams();
        }
    }
    ValueTree limiterTree = channelGroupTree.getChildWithName(limiterStateKey);
    if (limiterTree.isValid()) {
        limiterParams.setFromValueTree(limiterTree);
    }
    ValueTree eqTree = channelGroupTree.getChildWithName(eqStateKey);
    if (eqTree.isValid()) {
        eqParams.setFromValueTree(eqTree);
    }
    ValueTree mondelayTree = channelGroupTree.getChildWithName(monDelayStateKey);
    if (mondelayTree.isValid()) {
        monitorDelayParams.setFromValueTree(mondelayTree);
    }
}

ValueTree ChannelGroupParams::getChannelLayoutValueTree()
{
    ValueTree channelGroupTree(layoutGroupsKey);

    channelGroupTree.setProperty(channelStartIndexKey, chanStartIndex, nullptr);
    channelGroupTree.setProperty(numChannelsKey, numChannels, nullptr);
    channelGroupTree.setProperty(nameKey, name, nullptr);
    channelGroupTree.setProperty(peerPan1Key, panStereo[0], nullptr);
    channelGroupTree.setProperty(peerPan2Key, panStereo[1], nullptr);

    ValueTree panListTree(stereoPanListKey);
    for (int i=0; i < numChannels && i < MAX_CHANNELS; ++i) {
        ValueTree panner(panStateKey);
        panner.setProperty(panAttrKey, pan[i], nullptr);
        panListTree.appendChild(panner, nullptr);
    }
    channelGroupTree.appendChild(panListTree, nullptr);

    return channelGroupTree;
}

void ChannelGroupParams::setFromChannelLayoutValueTree(const ValueTree & layoutval)
{
    chanStartIndex = layoutval.getProperty(channelStartIndexKey, chanStartIndex);
    numChannels = layoutval.getProperty(numChannelsKey, numChannels);
    name = layoutval.getProperty(nameKey, name);
    panStereo[0] = layoutval.getProperty(peerPan1Key, panStereo[0]);
    panStereo[1] = layoutval.getProperty(peerPan2Key, panStereo[1]);

    ValueTree panListTree = layoutval.getChildWithName(stereoPanListKey);
    if (panListTree.isValid()) {
        int i=0;
        for (auto panner : panListTree) {
            if (panner.isValid() && i < MAX_CHANNELS) {
                pan[i] = panner.getProperty(panAttrKey, pan[i]);
            }
            ++i;
        }
    }
}



void ChannelGroup::commitAllParams()
{
    commitCompressorParams();
    commitLimiterParams();
    commitEqParams();
    commitExpanderParams();
    commitMonitorDelayParams();
}


void ChannelGroup::commitCompressorParams()
{
    if (!compressorControl) return;
    const auto & z = compressorZones;
    if (z.knee) *z.knee = 2.0f;
    if (z.threshold) *z.threshold = params.compressorParams.thresholdDb;
    if (z.ratio) *z.ratio = params.compressorParams.ratio;
    if (z.attack) *z.attack = params.compressorParams.attackMs * 1e-3f;
    if (z.release) *z.release = params.compressorParams.releaseMs * 1e-3f;
    if (z.makeupGain) *z.makeupGain = params.compressorParams.makeupGainDb;
}


// Hard gate after the expander. Block-peak detector, no lookahead, so the signal is never delayed: a block over
// threshold opens the gate inside that block (attackMs ramp on the samples in hand). After holdMs under threshold the
// gain ramps down over releaseMs and the output becomes exact zeros, which the idle-channel mask leaves out of the
// packet. Audio thread only; no allocation.
void ChannelGroup::applyHardGate(float* const* bufs, int nbufs, int numSamples, bool active)
{
    if (active) {
        const float thr = juce::Decibels::decibelsToGain(params.expanderParams.thresholdDb);
        float peak = 0.0f;
        for (int b = 0; b < nbufs; ++b) for (int i = 0; i < numSamples; ++i) peak = jmax(peak, std::abs(bufs[b][i]));
        const int holdSamples = (int)(jmax(0.0f, params.expanderParams.holdMs) * 1e-3 * gateSampleRate);
        if (peak > thr) { gateOpen = true; gateHoldLeft = holdSamples; }
        else if (gateHoldLeft > 0) { gateHoldLeft -= numSamples; }
        else { gateOpen = false; }
    }
    else { gateOpen = true; gateHoldLeft = 0; }   // switched off while closed: back up over the attack time
    const float target = gateOpen ? 1.0f : 0.0f;
    if (active) gateStatePub.store((gateOpen || gateGain > 0.0f) ? 1 : 0, std::memory_order_relaxed);   // Indicator (closed only once fully silent)
    if (gateGain == target) {
        if (target == 0.0f) for (int b = 0; b < nbufs; ++b) FloatVectorOperations::clear(bufs[b], numSamples);
        return;
    }
    const float rampMs = gateOpen ? jmax(0.1f, params.expanderParams.attackMs) : jmax(1.0f, params.expanderParams.releaseMs);
    const float step = (float)(1.0 / jmax(1.0, rampMs * 1e-3 * gateSampleRate));
    float g = gateGain;
    for (int i = 0; i < numSamples; ++i) {
        if (g < target) g = jmin(target, g + step); else if (g > target) g = jmax(target, g - step);
        for (int b = 0; b < nbufs; ++b) bufs[b][i] *= g;
    }
    gateGain = g;
    if (!gateOpen && g <= 1e-4f) { gateGain = 0.0f; }   // closed: exact zeros from the next block on
}

void ChannelGroup::commitExpanderParams()
{
    if (!expanderControl) return;
    const auto & z = expanderZones;
    if (z.knee) *z.knee = 3.0f;
    if (z.threshold) *z.threshold = params.expanderParams.thresholdDb;
    if (z.ratio) *z.ratio = params.expanderParams.ratio;
    if (z.attack) *z.attack = params.expanderParams.attackMs * 1e-3f;
    if (z.release) *z.release = params.expanderParams.releaseMs * 1e-3f;
}

void ChannelGroup::commitLimiterParams()
{
    if (!limiterControl) return;
    const auto & z = limiterZones;
    if (z.threshold) *z.threshold = params.limiterParams.thresholdDb;
    if (z.ratio) *z.ratio = params.limiterParams.ratio;
    if (z.attack) *z.attack = params.limiterParams.attackMs * 1e-3f;
    if (z.release) *z.release = params.limiterParams.releaseMs * 1e-3f;
}


void ChannelGroup::commitEqParams()
{
    if (!eqControl[0]) return;

    const float vals[10] = {
        params.eqParams.lowShelfGain, params.eqParams.lowShelfFreq,
        params.eqParams.para1Gain, params.eqParams.para1Freq, params.eqParams.para1Q,
        params.eqParams.para2Gain, params.eqParams.para2Freq, params.eqParams.para2Q,
        params.eqParams.highShelfGain, params.eqParams.highShelfFreq };

    for (int i=0; i < 2; ++i) {
        for (int k=0; k < 10; ++k) {
            if (eqZones[i][k]) *eqZones[i][k] = vals[k];
        }
    }
}

void ChannelGroup::commitMonitorDelayParams()
{
    setMonitoringDelayTimeMs(params.monitorDelayParams.delayTimeMs);
    setMonitoringDelayEnabled(params.monitorDelayParams.enabled, params.numChannels);
}


