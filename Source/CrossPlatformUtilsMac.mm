// SPDX-License-Identifier: GPLv3-or-later WITH Appstore-exception
// Copyright (C) 2020 Jesse Chappell
// Modifications Copyright (C) 2026 Jason Buecker (Random Planet)


#include "CrossPlatformUtils.h"

#include <juce_core/system/juce_TargetPlatform.h>

#if JUCE_MAC


//#import <Foundation/Foundation.h>

#import <Cocoa/Cocoa.h>
#import <AVFoundation/AVFoundation.h>
#include <juce_events/juce_events.h>


void getSafeAreaInsets(void * component, float & top, float & bottom, float & left, float & right)
{
    top = bottom = left = right = 0;
}


void disableAppNap() {
    // Does the App Nap API even exist on this Mac?
    if ([[NSProcessInfo processInfo] respondsToSelector:@selector(beginActivityWithOptions:reason:)]) {
        // If the API exists, then disable App Nap...

        // From NSProcessInfo.h:
        // NSActivityIdleSystemSleepDisabled = (1ULL << 20),
        // NSActivityUserInitiated = (0x00FFFFFFULL | NSActivityIdleSystemSleepDisabled),
        // NSActivityLatencyCritical = 0xFF00000000ULL

        uint64_t options = (0x00FFFFFFULL | (1ULL << 20)) | 0xFF00000000ULL;

        // NSActivityLatencyCritical | NSActivityUserInitiated
        [[NSProcessInfo processInfo] beginActivityWithOptions:options
                                                       reason:@"avoiding audio hiccups and reducing latency"];
    }
}

#endif

// See CrossPlatformUtils.h — microphone access decided BEFORE the device opens.
void sbRequestMicrophoneAccess(std::function<void(bool granted)> done)
{
    if (@available(macOS 10.14, *)) {
        const AVAuthorizationStatus st = [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio];
        if (st == AVAuthorizationStatusNotDetermined) {
            [AVCaptureDevice requestAccessForMediaType:AVMediaTypeAudio completionHandler:^(BOOL granted) {
                juce::MessageManager::callAsync([done, granted]() { done(granted != NO); });
            }];
            return;
        }
        done(st == AVAuthorizationStatusAuthorized);
        return;
    }
    done(true);
}

#if JUCE_MAC
#include <CoreAudio/CoreAudio.h>
#include <unistd.h>
#include <dlfcn.h>
#include <libproc.h>
#include <vector>
#include <cmath>
#include <algorithm>

namespace {
template <typename T>
bool sbCaGet(AudioObjectID obj, AudioObjectPropertySelector sel, AudioObjectPropertyScope scope, T & v)
{
    AudioObjectPropertyAddress a { sel, scope, 0 };
    UInt32 sz = sizeof(T);
    return AudioObjectHasProperty(obj, &a) && AudioObjectGetPropertyData(obj, &a, 0, nullptr, &sz, &v) == noErr;
}

std::vector<AudioObjectID> sbCaGetIds(AudioObjectID obj, AudioObjectPropertySelector sel, AudioObjectPropertyScope scope)
{
    AudioObjectPropertyAddress a { sel, scope, 0 };
    UInt32 sz = 0;
    if (!AudioObjectHasProperty(obj, &a) || AudioObjectGetPropertyDataSize(obj, &a, 0, nullptr, &sz) != noErr || sz == 0) return {};
    std::vector<AudioObjectID> ids(sz / sizeof(AudioObjectID));
    if (AudioObjectGetPropertyData(obj, &a, 0, nullptr, &sz, ids.data()) != noErr) return {};
    ids.resize(sz / sizeof(AudioObjectID));
    return ids;
}

std::string sbCaName(AudioObjectID dev)
{
    CFStringRef cf = nullptr;
    if (!sbCaGet(dev, kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal, cf) || cf == nullptr) return "?";
    char buf[256] = {0};
    CFStringGetCString(cf, buf, sizeof(buf), kCFStringEncodingUTF8);
    CFRelease(cf);
    return buf;
}

std::string sbPidName(pid_t pid)
{
    char path[PROC_PIDPATHINFO_MAXSIZE] = {0};
    if (proc_pidpath(pid, path, sizeof(path)) <= 0) return "?";
    const char* slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

// Same sum as the app's device figure (JUCE CoreAudio): device latency + safety offset + IO buffer + first stream's
// latency. ioFrames > 0 replaces the device's buffer size property: that property answers for the calling process
// only, and for a sub-device of an aggregate it is not the host's I/O buffer at all.
bool sbCaScopeMs(AudioObjectID dev, bool input, double hostRate, int ioFrames, float & ms)
{
    const AudioObjectPropertyScope scope = input ? kAudioObjectPropertyScopeInput : kAudioObjectPropertyScopeOutput;
    const auto streams = sbCaGetIds(dev, kAudioDevicePropertyStreams, scope);
    if (streams.empty()) return false;
    UInt32 lat = 0, safety = 0, frames = 0, slat = 0;
    sbCaGet(dev, kAudioDevicePropertyLatency, scope, lat);
    sbCaGet(dev, kAudioDevicePropertySafetyOffset, scope, safety);
    sbCaGet(dev, kAudioDevicePropertyBufferFrameSize, kAudioObjectPropertyScopeWildcard, frames);
    if (ioFrames > 0) frames = (UInt32) ioFrames;
    sbCaGet(streams.front(), kAudioStreamPropertyLatency, scope, slat);
    Float64 rate = 0.0;
    sbCaGet(dev, kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, rate);
    if (rate <= 0.0) rate = hostRate;
    if (rate <= 0.0) return false;
    ms = (float) (1000.0 * (double) (lat + safety + frames + slat) / rate);
    return true;
}

bool sbCaIsAggregate(AudioObjectID dev)
{
    UInt32 t = 0;
    return sbCaGet(dev, kAudioDevicePropertyTransportType, kAudioObjectPropertyScopeGlobal, t) && t == kAudioDeviceTransportTypeAggregate;
}

// A process that runs an aggregate device lists its sub-devices: the running aggregate that holds all of them
// (the fewest extra), so the figure and the name are the aggregate's, as in the app.
AudioObjectID sbCaAggregateOf(const std::vector<AudioObjectID>& devs)
{
    if (devs.empty()) return kAudioObjectUnknown;
    AudioObjectID best = kAudioObjectUnknown; size_t bestN = 0;
    for (AudioObjectID agg : sbCaGetIds(kAudioObjectSystemObject, kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal)) {
        UInt32 running = 0;
        if (!sbCaIsAggregate(agg) || !sbCaGet(agg, kAudioDevicePropertyDeviceIsRunningSomewhere, kAudioObjectPropertyScopeGlobal, running) || !running) continue;
        const auto subs = sbCaGetIds(agg, kAudioAggregateDevicePropertyActiveSubDeviceList, kAudioObjectPropertyScopeGlobal);
        bool all = true;
        for (auto d : devs) if (std::find(subs.begin(), subs.end(), d) == subs.end()) { all = false; break; }
        if (all && (best == kAudioObjectUnknown || subs.size() < bestN)) { best = agg; bestN = subs.size(); }
    }
    return best;
}

// Of the devices a process lists for one direction: an aggregate if there is one (listed, or the one holding the
// listed sub-devices), else the slowest.
bool sbCaPick(const std::vector<AudioObjectID>& devs, bool input, double hostRate, int ioFrames, float & ms, std::string & name)
{
    for (auto d : devs) if (sbCaIsAggregate(d) && sbCaScopeMs(d, input, hostRate, ioFrames, ms)) { name = sbCaName(d); return true; }
    if (devs.size() > 1) {
        const AudioObjectID agg = sbCaAggregateOf(devs);
        if (agg != kAudioObjectUnknown && sbCaScopeMs(agg, input, hostRate, ioFrames, ms)) { name = sbCaName(agg); return true; }
    }
    bool any = false;
    for (auto d : devs) {
        float m = 0.0f;
        if (sbCaScopeMs(d, input, hostRate, ioFrames, m) && (!any || m > ms)) { ms = m; name = sbCaName(d); any = true; }
    }
    return any;
}

// "Logic Pro" from /Applications/Logic Pro.app/Contents/MacOS/Logic Pro (else the executable's name)
std::string sbAppNameForPid(pid_t pid)
{
    char path[PROC_PIDPATHINFO_MAXSIZE] = {0};
    if (proc_pidpath(pid, path, sizeof(path)) <= 0) return {};
    std::string p(path);
    const size_t app = p.find(".app/");
    if (app != std::string::npos) { const size_t sl = p.rfind('/', app); const size_t b = sl == std::string::npos ? 0 : sl + 1; return p.substr(b, app - b); }
    const size_t sl = p.rfind('/');
    return sl == std::string::npos ? p : p.substr(sl + 1);
}

// A plugin hosted out of process runs in a helper named after its host: "AUHostingService (Logic Pro)" -> "Logic Pro"
std::string sbHelperHostName()
{
    NSString* n = [[NSRunningApplication currentApplication] localizedName];
    if (n == nil) return {};
    std::string s([n UTF8String]);
    const size_t o = s.find('('), c = s.rfind(')');
    if (s.rfind("AUHostingService", 0) != 0 || o == std::string::npos || c == std::string::npos || c <= o + 1) return {};
    return s.substr(o + 1, c - o - 1);
}

std::string sbCaList(const std::vector<AudioObjectID>& devs)
{
    std::string r = "[";
    for (size_t i = 0; i < devs.size(); ++i) { if (i) r += ", "; r += sbCaName(devs[i]); if (sbCaIsAggregate(devs[i])) r += " (aggregate)"; }
    return r + "]";
}
}

int sbProcessInterfaceLatency(double hostRate, int hostBlock, float & inMs, float & outMs, std::string & deviceName, std::string & trace)
{
    char t[160];
    // The process that runs the host's audio: this one, or (plugin loaded out of process, e.g. AUHostingService
    // for Logic) the process responsible for this one.
    std::vector<pid_t> pids { getpid() };
    using RespFn = pid_t (*)(pid_t);
    if (auto fn = (RespFn) dlsym(RTLD_DEFAULT, "responsibility_get_pid_responsible_for_pid")) {
        const pid_t r = fn(getpid());
        if (r > 0 && r != getpid()) pids.push_back(r);
    }
    if (__builtin_available(macOS 14.0, *)) {
        // And the audio processes of the application the helper is named after
        const std::string helperHost = sbHelperHostName();
        if (!helperHost.empty()) {
            for (AudioObjectID po : sbCaGetIds(kAudioObjectSystemObject, kAudioHardwarePropertyProcessObjectList, kAudioObjectPropertyScopeGlobal)) {
                pid_t ppid = 0;
                if (!sbCaGet(po, kAudioProcessPropertyPID, kAudioObjectPropertyScopeGlobal, ppid) || ppid <= 0) continue;
                if (std::find(pids.begin(), pids.end(), ppid) == pids.end() && sbAppNameForPid(ppid) == helperHost) pids.push_back(ppid);
            }
        }
        for (pid_t pid : pids) {
            snprintf(t, sizeof(t), "%spid %d %s: ", trace.empty() ? "" : "; ", (int) pid, sbPidName(pid).c_str()); trace += t;
            AudioObjectPropertyAddress a { kAudioHardwarePropertyTranslatePIDToProcessObject, kAudioObjectPropertyScopeGlobal, 0 };
            AudioObjectID proc = kAudioObjectUnknown;
            UInt32 sz = sizeof(proc);
            pid_t qpid = pid;
            const OSStatus st = AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, sizeof(qpid), &qpid, &sz, &proc);
            if (st != noErr || proc == kAudioObjectUnknown) { snprintf(t, sizeof(t), "no process object (%d)", (int) st); trace += t; continue; }
            UInt32 rin = 0, rout = 0;
            sbCaGet(proc, kAudioProcessPropertyIsRunningInput, kAudioObjectPropertyScopeGlobal, rin);
            sbCaGet(proc, kAudioProcessPropertyIsRunningOutput, kAudioObjectPropertyScopeGlobal, rout);
            const auto ins = sbCaGetIds(proc, kAudioProcessPropertyDevices, kAudioObjectPropertyScopeInput);
            const auto outs = sbCaGetIds(proc, kAudioProcessPropertyDevices, kAudioObjectPropertyScopeOutput);
            snprintf(t, sizeof(t), "object %u running in=%u out=%u, ", (unsigned) proc, (unsigned) rin, (unsigned) rout); trace += t;
            trace += "input " + sbCaList(ins) + " output " + sbCaList(outs);
            // The IO buffer is the host's real callback size, in or out of process: the buffer property of a listed
            // (sub-)device is not the host's I/O buffer.
            const int ioFrames = hostBlock;
            float o = 0.0f, i = 0.0f; std::string on, in;
            const bool haveOut = sbCaPick(outs.empty() ? ins : outs, false, hostRate, ioFrames, o, on);
            const bool haveIn = sbCaPick(ins.empty() ? outs : ins, true, hostRate, ioFrames, i, in);
            if (!haveOut && !haveIn) continue;
            inMs = haveIn ? i : 0.0f; outMs = haveOut ? o : 0.0f;
            deviceName = (haveIn && haveOut && in != on) ? in + " / " + on : (haveOut ? on : in);
            snprintf(t, sizeof(t), " -> %s: in %.2f ms out %.2f ms (IO buffer = host block %d)", deviceName.c_str(), (double) inMs, (double) outMs, hostBlock); trace += t;
            return 1;
        }
    } else {
        trace += "macOS before 14: no process objects";
    }
    // Otherwise: the one running device at the host's rate and buffer size
    if (hostRate <= 0.0 || hostBlock <= 0) return 0;
    AudioObjectID match = kAudioObjectUnknown;
    int nmatch = 0;
    std::string seen;
    for (AudioObjectID dev : sbCaGetIds(kAudioObjectSystemObject, kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal)) {
        UInt32 running = 0, frames = 0; Float64 rate = 0.0;
        if (!sbCaGet(dev, kAudioDevicePropertyDeviceIsRunningSomewhere, kAudioObjectPropertyScopeGlobal, running) || !running) continue;
        sbCaGet(dev, kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, rate);
        sbCaGet(dev, kAudioDevicePropertyBufferFrameSize, kAudioObjectPropertyScopeWildcard, frames);
        snprintf(t, sizeof(t), "%s%s %u@%.0f", seen.empty() ? "" : ", ", sbCaName(dev).c_str(), (unsigned) frames, rate); seen += t;
        if (std::fabs(rate - hostRate) > 1.0 || (int) frames != hostBlock) continue;
        if (sbCaGetIds(dev, kAudioDevicePropertyStreams, kAudioObjectPropertyScopeOutput).empty()) continue;
        match = dev; ++nmatch;
    }
    snprintf(t, sizeof(t), "; running devices: [%s], %d match %d@%.0f", seen.c_str(), nmatch, hostBlock, hostRate); trace += t;
    if (nmatch == 1) {
        float i = 0.0f, o = 0.0f;
        if (sbCaScopeMs(match, false, hostRate, hostBlock, o)) {
            if (!sbCaScopeMs(match, true, hostRate, hostBlock, i)) i = 0.0f;
            inMs = i; outMs = o; deviceName = sbCaName(match);
            return 2;
        }
    }
    return 0;
}

std::string sbHostAppName()
{
    const std::string helperHost = sbHelperHostName();   // Out of process: the helper carries the host's name
    if (!helperHost.empty()) return helperHost;
    pid_t pid = getpid();
    using RespFn = pid_t (*)(pid_t);
    if (auto fn = (RespFn) dlsym(RTLD_DEFAULT, "responsibility_get_pid_responsible_for_pid")) { const pid_t r = fn(pid); if (r > 0) pid = r; }
    return sbAppNameForPid(pid);
}
#endif
