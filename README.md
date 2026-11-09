# SonoBus RP

SonoBus RP is a modified version of the original SonoBus 1.7.2 by Jesse Chappell (Sonosaurus), for musicians who
play together over the internet in real time. It adds a dedicated hub server (SonoBus RP Hub): each player sends one
stream to the hub, and the hub sends every player's separate streams to the others, with per-player jitter buffers
held at the hub so one lossy link does not degrade everyone else.

This repository holds both halves:

- **SonoBus RP**, the player (`Source/`, the `SonoBus` targets). Runs on macOS and Linux.
- **SonoBus RP Hub**, the server (`Source/SonobusHub*`, the `SonoBusHub` target). Runs on macOS (window) and
  Linux (headless, with a built-in web page for status and settings).

Downloads and install instructions: <https://randomplanet.com>

## Credits

SonoBus RP is built with [JUCE](https://juce.com) and [AOO](https://git.iem.at/cm/aoo) (Audio over OSC), both
included under `deps/`.

## Building

You need CMake 3.22 or newer and a C++17 compiler.

**macOS** (Xcode command line tools):

    cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
    cmake --build build --target SonoBus_Standalone SonoBusHub

**Linux** (Ubuntu or Debian; see `linux/deb_get_prereqs.sh` for the package list, plus `libopus-dev`):

    cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
    cmake --build build --target SonoBus_Standalone SonoBusHub

The player is built as `SonoBus_artefacts/Release/Standalone/`, the hub as `SonoBusHub_artefacts/Release/`.

## Notes

- Use wired Ethernet rather than Wi-Fi for the lowest latency, and headphones when a microphone is live: there is
  no echo cancellation.
- Audio is not encrypted.

## License

GNU General Public License version 3 (see `LICENSE` and `LICENSE_EXCEPTION`). Third-party notices for the hub are in
`LICENSES-HUB.txt`.
