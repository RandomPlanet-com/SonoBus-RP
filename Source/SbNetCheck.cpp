// SPDX-License-Identifier: GPLv3-or-later WITH Appstore-exception
// Copyright (C) 2026 Jason Buecker (Random Planet)
// One-time check at app start: does this computer reach the internet over Wi-Fi? (No timers, no audio thread.)
#include <string>
#if defined(_WIN32)
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>
#include <vector>
#pragma comment(lib, "iphlpapi.lib")
#elif defined(__APPLE__)
#include <SystemConfiguration/SystemConfiguration.h>
#else
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#endif

// True when the interface that carries the default route is Wi-Fi; name = that interface (for the log).
bool sbDefaultRouteIsWifi (std::string& name)
{
    name.clear();
#if defined(_WIN32)
    // The adapter Windows would use for an internet address, then its type.
    DWORD best = 0;
    sockaddr_in dst {}; dst.sin_family = AF_INET; dst.sin_addr.s_addr = htonl (0x08080808);   // any public address
    if (GetBestInterfaceEx ((sockaddr*) &dst, &best) != NO_ERROR) return false;
    ULONG len = 16 * 1024; std::vector<unsigned char> buf (len);
    ULONG rc = GetAdaptersAddresses (AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr, (IP_ADAPTER_ADDRESSES*) buf.data(), &len);
    if (rc == ERROR_BUFFER_OVERFLOW) { buf.resize (len); rc = GetAdaptersAddresses (AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr, (IP_ADAPTER_ADDRESSES*) buf.data(), &len); }
    if (rc != NO_ERROR) return false;
    for (auto* a = (IP_ADAPTER_ADDRESSES*) buf.data(); a != nullptr; a = a->Next)
    {
        if (a->IfIndex != best) continue;
        if (a->FriendlyName != nullptr) { char n[256] = {}; WideCharToMultiByte (CP_UTF8, 0, a->FriendlyName, -1, n, sizeof (n) - 1, nullptr, nullptr); name = n; }
        return a->IfType == IF_TYPE_IEEE80211;
    }
    return false;
#elif defined(__APPLE__)
    // The primary interface the system routes through, then whether it is a Wi-Fi (IEEE 802.11) interface.
    bool wifi = false;
    SCDynamicStoreRef store = SCDynamicStoreCreate (nullptr, CFSTR ("SonoBusRP"), nullptr, nullptr);
    if (store == nullptr) return false;
    CFStringRef primary = nullptr;
    if (auto dict = (CFDictionaryRef) SCDynamicStoreCopyValue (store, CFSTR ("State:/Network/Global/IPv4")))
    {
        if (auto p = (CFStringRef) CFDictionaryGetValue (dict, CFSTR ("PrimaryInterface"))) primary = (CFStringRef) CFRetain (p);
        CFRelease (dict);
    }
    CFRelease (store);
    if (primary == nullptr) return false;
    char n[64] = {}; CFStringGetCString (primary, n, sizeof (n), kCFStringEncodingUTF8); name = n;
    if (CFArrayRef all = SCNetworkInterfaceCopyAll())
    {
        for (CFIndex i = 0; i < CFArrayGetCount (all); ++i)
        {
            auto ifc = (SCNetworkInterfaceRef) CFArrayGetValueAtIndex (all, i);
            CFStringRef bsd = SCNetworkInterfaceGetBSDName (ifc);
            if (bsd != nullptr && CFStringCompare (bsd, primary, 0) == kCFCompareEqualTo)
            {
                CFStringRef t = SCNetworkInterfaceGetInterfaceType (ifc);
                wifi = t != nullptr && CFStringCompare (t, kSCNetworkInterfaceTypeIEEE80211, 0) == kCFCompareEqualTo;
                break;
            }
        }
        CFRelease (all);
    }
    CFRelease (primary);
    return wifi;
#else
    // The default route in /proc/net/route, then whether that interface is wireless.
    std::ifstream f ("/proc/net/route"); std::string line;
    std::getline (f, line);   // header
    while (std::getline (f, line))
    {
        std::istringstream s (line); std::string ifc, dest;
        if (! (s >> ifc >> dest)) continue;
        if (dest != "00000000") continue;
        name = ifc;
        struct stat st {};
        return stat (("/sys/class/net/" + ifc + "/wireless").c_str(), &st) == 0
            || stat (("/sys/class/net/" + ifc + "/phy80211").c_str(), &st) == 0;
    }
    return false;
#endif
}
