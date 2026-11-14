// SPDX-License-Identifier: GPLv3-or-later WITH Appstore-exception
// Copyright (C) 2026 Jason Buecker (Random Planet)

#pragma once

#include <chrono>
#include <cstdio>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

// Test aid. Holds outgoing datagrams for a while once per period, then sends them back to back, like a line that
// stalls. Off unless configured.
struct SbTxHold {
    int holdMs = 0, everyMs = 0, maxMs = 0;

    // spec: "<hold ms>,<every ms>[,<longest hold ms>]". Anything else leaves it off.
    void configure(const char* spec) {
        int h = 0, e = 0, m = 0;
        if (spec != nullptr && spec[0] != 0) std::sscanf(spec, "%d,%d,%d", &h, &e, &m);
        if (h > 0 && e > h) { holdMs = h; everyMs = e; maxMs = m; }
    }
    bool on() const { return holdMs > 0; }

    // sendNow(data, size) sends one datagram. It is kept for the held ones, so it must capture its destination by
    // value. Held datagrams go out in order and under the lock: nothing overtakes them.
    template <typename Fn>
    int send(const char* data, int size, Fn&& sendNow) {
        const double now = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
        std::lock_guard<std::mutex> l(lock);
        if (nextAt < 0) nextAt = now + everyMs;
        if (now >= nextAt) {
            rnd = rnd * 1664525u + 1013904223u;
            const int len = maxMs > holdMs ? holdMs + (int) ((rnd >> 8) % (unsigned) (maxMs - holdMs + 1)) : holdMs;
            holdUntil = now + len;
            nextAt = now + everyMs;
        }
        if (now < holdUntil) {
            held.push_back({ std::string(data, (size_t) size), std::function<int(const char*, int)>(sendNow) });
            return size;
        }
        for (auto& h : held) h.send(h.bytes.data(), (int) h.bytes.size());
        held.clear();
        return sendNow(data, size);
    }

private:
    struct Held { std::string bytes; std::function<int(const char*, int)> send; };
    std::mutex lock;
    std::vector<Held> held;
    double holdUntil = 0, nextAt = -1;
    unsigned rnd = 12345;
};
