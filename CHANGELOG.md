# Changelog

What changed in each release of SonoBus RP (the player) and SonoBus RP Hub (the server). Newest first.

## 2026-11-12

### SonoBus RP Hub 1.3.0

- Stage directory: a new option in the hub's Settings lets you list your stage, so players can find it in the
  STAGES tab of the player. Listing is off until you turn it on. You set the city, country, genre and a one-line
  description; a stage with a password is shown as locked. Player names are shown for an open stage and hidden for
  a locked one unless you choose otherwise.
- Stage limits: the most players and the most channels one player may send (20 and 12 unless you change them).
- Remove a player: the player is kept out for one hour.
- The Players table has an Instrument column.
- The hub's buffer for a player on the local network returns to normal within seconds after a stumble.
- "Group" is now called "stage" everywhere.
- Fewer lines in the log after lost packets.

### SonoBus RP 1.3.0

- New STAGES tab in Connect: the public stages with your ping to each one. Sort by any column, open a stage to
  see who is playing, and join from the list.
- The top line shows Ping, Delay and a Link light. Delay is the whole loop: network, buffering and your audio
  interface. Point at the line for the details.
- After a rough patch on the line the receive buffer returns to normal within seconds instead of minutes.
- Recents has its own tab. "Group" is now called "stage" everywhere, and invite links with "stage=" are understood.
- You can set your instrument next to your name. It is shown in the stage list and to the stage's host.
- Monitor delay: "Set From Stage" sets the time from how late the other players hear you.
- Names are limited to 30 characters.
- Windows: the player can install an update itself.
- Fixed: squeezed text in the update window, the cut-off version line in About, and updates that failed to download.

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
