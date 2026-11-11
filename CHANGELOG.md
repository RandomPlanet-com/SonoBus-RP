# Changelog

What changed in each release of SonoBus RP (the player) and SonoBus RP Hub (the server). Newest first.

## 2026-11-11

### SonoBus RP Hub 1.2.9

- Lower latency for players joining over the internet: the hub's buffer for each of them is about 1.3 ms smaller.
- After a network stall, the hub's buffer returns to normal within about two minutes instead of ten or more.
- The Players list shows each player's version and system.
- The "To hub" figure for players on the local network now follows the real buffer.
- A player who joins with the send muted is no longer treated as stalled.
- The Players table is tidier and holds still, with the same columns in the hub window and on the web page:
  values on the left with their unit, pairs such as To / From hub in one column, the player's address shown
  when you point at the name, and a clear arrow on the column you sort by.
- Linux: the copy of the previous version kept by an update can be run again for a rollback.

### SonoBus RP 1.2.8

- Fixed a short dropout on every incoming stream about six minutes after joining.
- Starting the app rejoins your last room only if you were in it when you quit. If you disconnected first, it
  stays out.
- A muted send is now obvious: a red SEND MUTED banner appears and the input meters dim.
- A stereo pair shows both channels on its output-side meter.
- The meters are labelled IN and OUT.
- Double-clicking a room in Recents connects to it.
- The player reports its version to the hub, so the host can see who needs an update.
- First beta for Windows (64-bit; needs an audio interface with its own ASIO driver).

## 2026-11-09

### SonoBus RP Hub 1.2.8

- The Players heading shows how many players are connected now and how many different players have joined since
  the hub started.
- The hub's web page shows the room name in the browser tab, so several hubs are easy to tell apart.
- The hub's icon appears in the browser tab without asking for the password first.
- A player who mutes their send is no longer treated as a stalled stream.
- Steadier receive buffers when a stream starts.
- Linux on ARM servers: Install Update now picks the ARM program from the download.
- Linux: the update check and download try again when the server does not answer at first.
- Clearer `--help` for people running a hub on a server.

## 2026-11-07

### SonoBus RP Hub 1.2.7

- The outside port check now uses its own server name, so it keeps working if that server moves.
- Code cleanup for public release, with no change to how the hub sounds or behaves.

### SonoBus RP 1.2.7

- Settings and logs moved to the standard Mac folders. Existing files move automatically.
- Updated the About box.
- First release for Linux (Intel and ARM, Ubuntu 20.04 or newer, Debian 11 or newer).
