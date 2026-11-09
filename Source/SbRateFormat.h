// SbRateFormat.h: one bandwidth format for the client, the hub Mac window and (mirrored in JS) the hub web page. Bits
// everywhere, so users can compare with their upload speed; whole kb/s below 1 Mb/s, one decimal above.
//   328 kb/s · 2.6 Mb/s · "-" when unknown. Input is bits per second.
#pragma once
#include "JuceHeader.h"
#include <cmath>
namespace sbplus {
    inline juce::String rateText(double bitsPerSec) {
        if (bitsPerSec < 0.0) return "-";
        if (bitsPerSec < 1.0e6) return juce::String((int) std::lround(bitsPerSec / 1000.0)) + " kb/s";
        return juce::String(bitsPerSec / 1.0e6, 1) + " Mb/s";
    }
    inline juce::String rateTextBytes(double bytesPerSec) { return bytesPerSec < 0.0 ? juce::String("-") : rateText(bytesPerSec * 8.0); }
}
