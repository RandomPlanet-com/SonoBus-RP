// SPDX-License-Identifier: GPLv3-or-later WITH Appstore-exception
// Copyright (C) 2026 Jason Buecker (Random Planet)
//
// Platform layer for the hub: the socket and system calls that differ on Windows. On macOS and Linux the helpers
// are the plain POSIX calls.

#pragma once

#if defined(_WIN32)

#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#include <iphlpapi.h>
#include <windows.h>
#include <mmsystem.h>
#include <avrt.h>
#include <psapi.h>
#include <intrin.h>
#include <sys/stat.h>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>

typedef SSIZE_T ssize_t;
typedef unsigned long nfds_t;

// winsock reports "would block" for both cases
#undef EWOULDBLOCK
#define EWOULDBLOCK EAGAIN

#define poll WSAPoll
#define popen _popen
#define pclose _pclose
#define strcasecmp _stricmp
#define strncasecmp _strnicmp
#ifndef MSG_DONTWAIT
#define MSG_DONTWAIT 0   // the hub's sockets are non-blocking
#endif
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif
#ifndef SO_REUSEPORT
#define SO_REUSEPORT SO_REUSEADDR
#endif
#ifndef SHUT_RDWR
#define SHUT_RDWR SD_BOTH
#endif
#ifndef IFF_UP
#define IFF_UP 0x1
#endif
#ifndef IFF_LOOPBACK
#define IFF_LOOPBACK 0x8
#endif

namespace hubwin {
struct WsaInit { WsaInit() { WSADATA d; WSAStartup(MAKEWORD(2, 2), &d); } };
inline WsaInit& wsaInit() { static WsaInit w; return w; }
static const WsaInit& gWsaInit = wsaInit();
}

inline int hub_close(int fd) { return ::closesocket((SOCKET) fd); }

// The last socket error as an errno value.
inline int hub_sockerr() {
    const int w = WSAGetLastError();
    switch (w) {
        case 0: return errno;
        case WSAEWOULDBLOCK: return EAGAIN;
        case WSAEINTR: return EINTR;
        case WSAEINPROGRESS: return EINPROGRESS;
        case WSAEALREADY: return EALREADY;
        case WSAECONNREFUSED: return ECONNREFUSED;
        case WSAECONNRESET: return ECONNRESET;
        case WSAECONNABORTED: return ECONNABORTED;
        case WSAEMSGSIZE: return EMSGSIZE;
        case WSAENETUNREACH: return ENETUNREACH;
        case WSAEHOSTUNREACH: return EHOSTUNREACH;
        case WSAEADDRINUSE: return EADDRINUSE;
        case WSAEADDRNOTAVAIL: return EADDRNOTAVAIL;
        case WSAETIMEDOUT: return ETIMEDOUT;
        case WSAENOBUFS: return ENOBUFS;
        case WSAEINVAL: return EINVAL;
        case WSAEACCES: return EACCES;
        case WSAENOTCONN: return ENOTCONN;
        case WSAESHUTDOWN: return EPIPE;
        default: return w;
    }
}
// A non-blocking connect that has started.
inline bool hub_connect_pending(int err) { return err == EINPROGRESS || err == EAGAIN; }

inline int hub_set_nonblock(int fd) { u_long one = 1; return ::ioctlsocket((SOCKET) fd, FIONBIO, &one) == 0 ? 0 : -1; }

// A UDP socket must not fail its next receive because an earlier datagram met a closed port.
inline void hub_udp_setup(int fd) {
    BOOL off = FALSE; DWORD got = 0;
    ::WSAIoctl((SOCKET) fd, SIO_UDP_CONNRESET, &off, sizeof(off), nullptr, 0, &got, nullptr, nullptr);
}

// Option and buffer arguments as on POSIX.
inline int setsockopt(SOCKET s, int level, int name, const void* v, int len) {
    if (level == SOL_SOCKET && (name == SO_RCVTIMEO || name == SO_SNDTIMEO) && len == (int) sizeof(struct timeval)) {
        const struct timeval* tv = (const struct timeval*) v;   // a timeout is milliseconds here
        const DWORD ms = (DWORD) (tv->tv_sec * 1000 + tv->tv_usec / 1000);
        return ::setsockopt(s, level, name, (const char*) &ms, sizeof(ms));
    }
    return ::setsockopt(s, level, name, (const char*) v, len);
}
inline int getsockopt(SOCKET s, int level, int name, void* v, int* len) { return ::getsockopt(s, level, name, (char*) v, len); }
inline int send(SOCKET s, const char* b, size_t n, int flags) { return ::send(s, b, (int) n, flags); }
inline int sendto(SOCKET s, const char* b, size_t n, int flags, const struct sockaddr* to, int tolen) { return ::sendto(s, b, (int) n, flags, to, tolen); }
inline int sendto(SOCKET s, const char* b, size_t n, int flags, const struct sockaddr* to, size_t tolen) { return ::sendto(s, b, (int) n, flags, to, (int) tolen); }
inline int recv(SOCKET s, char* b, size_t n, int flags) { return ::recv(s, b, (int) n, flags); }
inline int recvfrom(SOCKET s, char* b, size_t n, int flags, struct sockaddr* from, int* fromlen) { return ::recvfrom(s, b, (int) n, flags, from, fromlen); }
// buffers that are not char
template <typename L> inline int sendto(SOCKET s, const void* b, size_t n, int flags, const struct sockaddr* to, L tolen) { return ::sendto(s, (const char*) b, (int) n, flags, to, (int) tolen); }
inline int send(SOCKET s, const void* b, size_t n, int flags) { return ::send(s, (const char*) b, (int) n, flags); }
inline int recv(SOCKET s, void* b, size_t n, int flags) { return ::recv(s, (char*) b, (int) n, flags); }

inline int setenv(const char* k, const char* v, int) { return _putenv_s(k, v); }
inline int unsetenv(const char* k) { return _putenv_s(k, ""); }

// Writes the log out four times a second.
inline void hub_start_log_flusher() {
    static HANDLE h = ::CreateThread(nullptr, 0, [](LPVOID) -> DWORD { for (;;) { ::Sleep(250); fflush(stdout); fflush(stderr); } }, nullptr, 0, nullptr);
    (void) h;
}

// A second handle to the same socket.
inline int hub_dup(int fd) {
    WSAPROTOCOL_INFOW pi;
    if (::WSADuplicateSocketW((SOCKET) fd, ::GetCurrentProcessId(), &pi) != 0) return -1;
    const SOCKET s = ::WSASocketW(FROM_PROTOCOL_INFO, FROM_PROTOCOL_INFO, FROM_PROTOCOL_INFO, &pi, 0, 0);
    return s == INVALID_SOCKET ? -1 : (int) s;
}

inline struct tm* localtime_r(const time_t* t, struct tm* out) { return localtime_s(out, t) == 0 ? out : nullptr; }
inline struct tm* gmtime_r(const time_t* t, struct tm* out) { return gmtime_s(out, t) == 0 ? out : nullptr; }
inline int usleep(unsigned us) { ::Sleep((us + 999) / 1000); return 0; }

// Process times, as getrusage reports them.
#define RUSAGE_SELF 0
struct rusage { struct timeval ru_utime, ru_stime; long ru_maxrss; };
inline int getrusage(int, struct rusage* ru) {
    FILETIME c, e, k, u;
    if (!::GetProcessTimes(::GetCurrentProcess(), &c, &e, &k, &u)) return -1;
    auto us = [](const FILETIME& f) { return (((uint64_t) f.dwHighDateTime << 32) | f.dwLowDateTime) / 10ULL; };
    ru->ru_utime.tv_sec = (long) (us(u) / 1000000ULL); ru->ru_utime.tv_usec = (long) (us(u) % 1000000ULL);
    ru->ru_stime.tv_sec = (long) (us(k) / 1000000ULL); ru->ru_stime.tv_usec = (long) (us(k) % 1000000ULL);
    PROCESS_MEMORY_COUNTERS pmc; ru->ru_maxrss = 0;
    if (::GetProcessMemoryInfo(::GetCurrentProcess(), &pmc, sizeof(pmc))) ru->ru_maxrss = (long) (pmc.PeakWorkingSetSize / 1024);
    return 0;
}

// The IPv4 addresses of the machine's adapters, in the shape getifaddrs gives.
struct ifaddrs {
    struct ifaddrs* ifa_next;
    char* ifa_name;
    unsigned int ifa_flags;
    struct sockaddr* ifa_addr;
    struct sockaddr* ifa_netmask;
    struct sockaddr_in addrStore, maskStore;
    char nameStore[64];
};
inline void freeifaddrs(struct ifaddrs* l) { while (l) { auto* n = l->ifa_next; delete l; l = n; } }
inline int getifaddrs(struct ifaddrs** out) {
    *out = nullptr;
    ULONG len = 16 * 1024;
    std::string buf;
    DWORD rc = ERROR_BUFFER_OVERFLOW;
    for (int tries = 0; tries < 3 && rc == ERROR_BUFFER_OVERFLOW; ++tries) {
        buf.resize(len);
        rc = ::GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr,
                                    (IP_ADAPTER_ADDRESSES*) &buf[0], &len);
    }
    if (rc != NO_ERROR) return -1;
    struct ifaddrs* tail = nullptr;
    for (auto* a = (IP_ADAPTER_ADDRESSES*) &buf[0]; a; a = a->Next) {
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
            if (!u->Address.lpSockaddr || u->Address.lpSockaddr->sa_family != AF_INET) continue;
            auto* n = new ifaddrs();
            memset(n, 0, sizeof(*n));
            memcpy(&n->addrStore, u->Address.lpSockaddr, sizeof(struct sockaddr_in));
            n->maskStore.sin_family = AF_INET;
            const unsigned prefix = u->OnLinkPrefixLength > 32 ? 32 : u->OnLinkPrefixLength;
            n->maskStore.sin_addr.s_addr = htonl(prefix == 0 ? 0u : 0xffffffffu << (32 - prefix));
            n->ifa_addr = (struct sockaddr*) &n->addrStore;
            n->ifa_netmask = (struct sockaddr*) &n->maskStore;
            n->ifa_flags = (a->OperStatus == IfOperStatusUp ? IFF_UP : 0) | (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK ? IFF_LOOPBACK : 0);
            strncpy(n->nameStore, a->AdapterName ? a->AdapterName : "", sizeof(n->nameStore) - 1);
            n->ifa_name = n->nameStore;
            if (tail) tail->ifa_next = n; else *out = n;
            tail = n;
        }
    }
    return 0;
}

#else   // macOS, Linux

#include <cerrno>
#include <fcntl.h>
#include <unistd.h>

inline int hub_close(int fd) { return ::close(fd); }
inline int hub_dup(int fd) { return ::dup(fd); }
inline int hub_sockerr() { return errno; }
inline bool hub_connect_pending(int err) { return err == EINPROGRESS; }
inline int hub_set_nonblock(int fd) { const int fl = fcntl(fd, F_GETFL, 0); return fl < 0 ? -1 : fcntl(fd, F_SETFL, fl | O_NONBLOCK); }
inline void hub_udp_setup(int) {}

#endif
