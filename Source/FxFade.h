// SPDX-License-Identifier: GPLv3-or-later WITH Appstore-exception
// Copyright (C) 2026 Jason Buecker (Random Planet)

#pragma once

#include <algorithm>

namespace SonoAudio {

// Click-free switch for an effect: a short fade, counted per sample and carried across blocks.
// Position 0 = effect out, 1 = effect in. Audio thread only; no allocation.
struct FxFade
{
    static constexpr int chunk = 64;

    void prepare (double sampleRate, double fadeMs = 8.0)
    {
        step = (float) (1.0 / std::max (1.0, sampleRate * fadeMs * 1e-3));
    }

    void snapTo (bool on)               { pos = on ? 1.0f : 0.0f; }

    bool isOff() const                  { return pos <= 0.0f; }
    bool isOn() const                   { return pos >= 1.0f; }
    bool fading (bool on) const         { return on ? pos < 1.0f : pos > 0.0f; }
    // The effect has to be computed: it is on, or still fading out
    bool running (bool on) const        { return on || pos > 0.0f; }

    // Fills g[0..n) with the next fade gains towards on / off (it stays at the end value once reached)
    void ramp (float* g, int n, bool on)
    {
        const float target = on ? 1.0f : 0.0f;
        float p = pos;
        for (int i = 0; i < n; ++i) {
            p = on ? std::min (target, p + step) : std::max (target, p - step);
            g[i] = p * p * (3.0f - 2.0f * p);
        }
        pos = p;
    }

    // x *= fade gain
    void gain (float* x, int n, bool on)
    {
        float g[chunk];
        for (int done = 0; done < n; done += chunk) {
            const int m = std::min (chunk, n - done);
            ramp (g, m, on);
            for (int i = 0; i < m; ++i) x[done + i] *= g[i];
        }
    }

    // wet = dry + fade gain * (wet - dry)
    void mix (float* wet, const float* dry, int n, bool on)
    {
        float g[chunk];
        for (int done = 0; done < n; done += chunk) {
            const int m = std::min (chunk, n - done);
            ramp (g, m, on);
            for (int i = 0; i < m; ++i) wet[done + i] = dry[done + i] + g[i] * (wet[done + i] - dry[done + i]);
        }
    }

    float pos = 0.0f;
    float step = 1.0f / 384.0f;
};

}
