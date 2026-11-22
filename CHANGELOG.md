# Changelog

What changed in each release of SonoBus RP (the player) and SonoBus RP Hub (the server). Newest first.

## 2026-11-22

### SonoBus RP 1.3.8

- Your name and instrument are at the top of the Stages, Recents and Private Stage tabs, so you see and can change them before you join.
- Change your name while on a stage: click your name under the stage name, type the new one and press Enter. Everyone on the stage sees it at once.
- Chat opens when you join a stage, so you see the stage's welcome message.
- Each player has one chat color, the same on everyone's screen, kept through a name change or a rejoin.
- At your own hub server's site, the player app connects to the hub server through your local network by itself, even when you use the public stage link.
- Connect window: tabs and content stay better aligned in the window; Recents is a list with columns; one click selects, double-click or Connect joins.
- All drop-down lists open in the same pop-up style.

### SonoBus RP Plugins 1.3.8

- The changes of SonoBus RP 1.3.8.

### SonoBus RP Hub 1.3.7

- Players can change their name while on the stage; the hub window, the web page and the stages list show the new name.
- The hub gives each player one chat color for everyone on the stage.
- A player at the hub's own site is sent to the local network by itself; the old chat hint is gone.
- The web page refuses an address for 15 minutes after 5 wrong passwords.
- A player name AND  instrument name can contain special characters '&#$' etc
- Linux: the port help also says to forward the ports on the router.

## 2026-11-21

### SonoBus RP 1.3.7

- Chat opens with the app.
- Chat: bold, italic and underline in messages.
- Chat: each player has one color for name and text, different from everyone else on the stage.
- Chat: the stage's welcome message comes first when you join, then the players on the stage.
- Chat: player joins and leaves are quiet grey lines.
- Player rows show each player's instrument: "Name (Instrument)".
- Stages list: Copy invite link is a button, and "Link copied" shows right beside it.
- Clicking a player's name no longer hides that player's details; clicks next to the "SonoBus RP" title no longer open Settings.

### SonoBus RP Plugins 1.3.7

- The changes of SonoBus RP 1.3.7.

### SonoBus RP Hub 1.3.6

- *New* Welcome message: set one per stage in Settings; each player who joins sees it in the chat when they first connect.
- Save Logs for Support (window) and Download Logs (web page): one zip to send for help, with no passwords inside.
- The main hub's Status shows everyone on its other stages and the total on the server.
- A padlock marks stages with a password.
- The web pages are now mobile optimized to fit better on a phone.
- Other stages: Open takes you to the stage's own page; each stage page has a Main hub page button.
- A stage that crashes is started again by the main hub.
- Players who run their own Stage/HUB Server at home are told the hub's local address, so they do not loop through the router.
- "Port 12XXX is already in use" is said plainly when the hub cannot start.
- Listing a stage needs city, state or province and country; "California" is saved as "CA".
- Log files that grew without end now stop at 8 MB and keep one old copy.

## 2026-11-20

### SonoBus RP 1.3.6

- Default User Level now sets the starting level of each new player on a stage; players you have set before keep your level.
- Stages list: stages open with their players shown; Expand all / Collapse all in the header corner.
- Tick boxes take clicks on the box and its text only.
- The sort arrow in table headers is easy to see.
- Sends the hub an anonymous install id, so a removal by the host still holds after a name change.

### SonoBus RP Plugins 1.3.6

- The changes of SonoBus RP 1.3.6.

### SonoBus RP Hub 1.3.5

- Previous players: kept (up to 1000); remove with the same choices as Kick; forget one or clear the list.
- A removed player stays out under another name (players 1.3.6 and newer); Allow back from the list.
- Optional: forget players not seen for a number of days (off by default).
- Run MULTIPLE stages on one server: New stage, Start, Stop, Settings, Move ports, Delete, from the window or the web page.
- Stages take the next ports in a row, so one port range covers them all.
- Stages come back by themselves after a restart or an update.
- A stage's place is City and State / Region.
- Wider version column; plugin players show Mac AU, Mac VST3 or Win VST3.

## 2026-11-19

### SonoBus RP Plugins 1.3.5

- SonoBus RP inside your DAW: AU and VST3 (beta)
- Same stages, hubs and low-latency engine as the app.
- Use 48 kHz and a 32 or 64 sample buffer

### SonoBus RP 1.3.5

- No audio artifacts when switching an effect on or off.
- Noise gate no longer lowers the existing level.
- EQ: removed audio artifacts; band 2 width is saved.
- Text fields now maintain their full names
- Input Mixer: PAN, WIDTH and MONITOR knobs; aligned columns.
- Wider settings window.
- Cleaner Private Stage tab / section.
- Remembers stage passwords.
- Reliable joins right after launch.

### SonoBus RP Hub 1.3.4

- A stage is not listed while its ports are blocked.
- Clearer log lines.
- No web page without a password.
- New-version banner; stage name in the window title.
- Windows: steadier timing on slower PCs.

## 2026-11-18

### SonoBus RP 1.3.4

- SonoBus RP 1.3.4: fixes the FX button on player rows, which closed right after opening.

## 2026-11-17

### SonoBus RP 1.3.3

Sound
- Fewer short cuts: the audio no longer waits on the app's list of players.
- Smoother buffer changes: when the receive buffer is adjusted, the sound is blended across the change instead of cut.
- Flat treble to 20 kHz: the clock-drift correction no longer rolls off the top end, and adds no latency.
- After a network storm the buffer returns to low latency in one step, and reacts to the storm faster.
- Cleaner start when you join: your outgoing stream no longer restarts several times.

Window
- Player rows redesigned: every volume slider starts at the same place, MUTE, SOLO and FX sit on the lower line next to the latency, and the Output button sits beside the meter.
- PAN knob on every player. Stereo players also get a WIDTH knob (100% as sent, 0% mono). Players with several instruments get a PAN that moves their whole mix.
- Meters use the same bar size everywhere. A player with more channels shows more bars, not thinner ones.
- The top line says Latency (the Overall Delay, hover your mouse here to see details) and shows the server address without "Hub". Each player row shows a quality light and one Latency figure, the round trip through that player with both audio interfaces included. Hover your mouse over it for the details.
- Audio interface latency is now counted correctly.
- Set your instrument right next to your name at the top, any time, even while connected.
- Point at a channel on a player's meter to see its name.
- Text is never squeezed: names that do not fit wrap or end with "…".
- A redesigned Stages list with one Connect button and a table of the players on each stage.
- Joining a stage lists the players already there in one block, one name per line.
- Chat stays with its stage. A removed player is told why. Drop counts never show below zero.

Fixes
- No crash when you disconnect or another player leaves.
- Resetting a row's drop count resets that player, not another one.
- The Windows fix from 1.3.2 is now in every player.

### SonoBus RP Hub 1.3.3

- Set or remove the stage password with clear buttons (takes effect when the hub restarts).
- The advanced settings sit under "For advanced users only".
- A list of removed players with Allow back, and a choice of how long they stay out: one hour, until the hub restarts, or for good.
- A player's audio interface latency shows instantly when you point at the player.
- Steadier buffers for players on bursty connections.
- Mute, solo and gain changes fade instead of clicking.
- A player on the hub's own computer can join by 127.0.0.1 or localhost.

## 2026-11-16

### SonoBus RP 1.3.2

- Windows: the player no longer crashes when you disconnect from a stage with other players on it.

## 2026-11-14

### SonoBus RP Hub 1.3.1

- A lost packet from a player no longer makes a click. The hub fills the gap with the sound just before it and
  blends back into the real audio.
- When a player's stream arrives late, the late part is no longer played and then thrown away a moment later.
  The others hear one short gap with soft edges instead of two gaps with clicks.
- The hub measures the difference between each player's audio clock and its own and corrects it within about a
  minute. Before, a player with a slightly fast or slow audio interface could be cut by a block every few seconds.
- A player's buffer at the hub settles about a second after the player joins when the line needs more than the
  minimum. Before, this took 15 to 20 seconds, with dropouts for the others until then. The buffer still starts at
  the minimum.
- When the hub changes a player's buffer it no longer restarts that player's stream.
- A player's buffer no longer swings back and forth on a line with long stalls.
- The log no longer prints "decode failed" for a filled gap.

### SonoBus RP 1.3.1

- A lost packet no longer makes a click. The gap is filled with the sound just before it and blended back into
  the real audio.
- After a late patch on the line you hear one short gap with soft edges: the last sound rings out and the audio
  fades back in. Before, the late audio was played and then thrown away, which gave two gaps with clicks.
- Clock differences between players are measured and corrected within about a minute. Before, a small
  difference between two audio interfaces could cause a cut every few seconds.
- The receive buffer keeps its level more steadily, with fewer small corrections on a quiet line.
- With a larger audio buffer size the sound comes back a few milliseconds sooner after a late patch.
- New Stages button next to Disconnect: open the stage list while you stay connected and keep playing. Your own
  stage is marked "You are here". Joining another stage asks first.
- The stage list rests when you are not looking at it: no refreshing while it is closed, and a slower refresh
  when it has been left open and untouched for two minutes.

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
