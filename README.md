# SonoBus RP

SonoBus RP is a modified version of the original SonoBus 1.7.2 by Jesse Chappell (Sonosaurus), for musicians who
play together over the internet in real time. It adds a dedicated hub server (SonoBus RP Hub): each player sends one
stream to the hub, and the hub sends every player's separate streams to the others, with per-player jitter buffers
held at the hub so one lossy link does not degrade everyone else.

This repository holds both halves:

- **SonoBus RP**, the player (`Source/`, the `SonoBus` targets). Runs on macOS and Linux; Windows is in beta.
- **SonoBus RP Hub**, the server (`Source/SonobusHub*`, the `SonoBusHub` target). Runs on macOS (window) and
  Linux (headless, with a built-in web page for status and settings). Setup and hosting guide:
  [SonoBus-RP-Hub](https://github.com/RandomPlanet-com/SonoBus-RP-Hub).

Downloads and install instructions: <https://randomplanet.com>

What changed in each release: [CHANGELOG.md](CHANGELOG.md).

## Credits

SonoBus RP is built with [JUCE](https://juce.com) and [AOO](https://git.iem.at/cm/aoo) (Audio over OSC), both
included under `deps/`.

## Building

You need CMake 3.22 or newer and a C++17 compiler.

**macOS** (Xcode command line tools):

    cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
    cmake --build build --target SonoBus_Standalone SonoBusHub

**Linux** (Ubuntu or Debian): install the packages with `linux/deb_get_prereqs.sh`, then:

    cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
    cmake --build build --target SonoBus_Standalone SonoBusHub

**Windows** (beta, the player only; Visual Studio 2022 with its CMake, 64-bit): put Steinberg's ASIO SDK, version
2.3.4 or newer (it can be used under the GPL version 3), in a folder named `asiosdk` next to this one, then:

    cmake -B build -G "Visual Studio 17 2022" -A x64 -DBUILD_TESTING=OFF
    cmake --build build --config Release --target SonoBus_Standalone

The Windows player needs an audio interface with its own ASIO driver. ASIO is a trademark and software of Steinberg
Media Technologies GmbH.

The player is built as `SonoBus_artefacts/Release/Standalone/`, the hub as `SonoBusHub_artefacts/Release/`.
To build only one of them, name only that target: `--target SonoBusHub` for the hub, `--target SonoBus_Standalone`
for the player.

Memory: a few source files need more than 1 GB each to compile, and Ninja runs several jobs at once. On a machine
with less than about 4 GB free, build one job at a time by running `export CMAKE_BUILD_PARALLEL_LEVEL=1` before the
first command; the full build then needs about 2 GB. The hub alone builds on a 1 GB machine that has swap space.

## Notes

- Use wired Ethernet rather than Wi-Fi for the lowest latency, and headphones when a microphone is live: there is
  no echo cancellation.
- Audio is not encrypted.

## License

Copyright (C) 2026 Jason Buecker (Random Planet), based on SonoBus by Jesse Chappell.
GNU General Public License version 3 (see `LICENSE` and `LICENSE_EXCEPTION`). Third-party notices for the hub are in
`LICENSES-HUB.txt`.
