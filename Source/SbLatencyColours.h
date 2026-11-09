// SbLatencyColours.h: one set of latency colour thresholds shared by the client (peer rows + top bar), the hub web
// page and the hub Mac GUI. Green = a real, playable case; yellow = marginal; red = almost unplayable.
#pragma once
namespace sbplus {
    constexpr float kLatOneWayGreenMs  = 25.0f;   // one-way leg (to hub / from hub): green below this
    constexpr float kLatOneWayYellowMs = 40.0f;   // yellow up to this, red above
    constexpr float kLatRttGreenMs     = 50.0f;   // round trip (ping): green below this
    constexpr float kLatRttYellowMs    = 80.0f;   // yellow up to this, red above
    // Player rows show end-to-end figures (player -> hub -> player, buffers included), which are naturally larger
    // than one hub leg, so they get their own bands.
    constexpr float kLatE2EGreenMs     = 40.0f;   // one direction player->hub->player: green below this
    constexpr float kLatE2EYellowMs    = 60.0f;   // yellow up to this, red above
    constexpr float kLatE2ERttGreenMs  = 80.0f;   // both directions (ping): green below this
    constexpr float kLatE2ERttYellowMs = 120.0f;  // yellow up to this, red above
}
