// SPDX-License-Identifier: GPLv3-or-later WITH Appstore-exception
// Phase 3: OSC control-channel messages from a SonoBus client to the hub for
// per-listener overrides of how each *other* peer is mixed into this listener's
// mix-minus-self output. Both messages are sent over the existing per-peer UDP
// channel (sendPeerMessage on the client; networkRecvThread on the hub).
//
// Listener identity: the hub resolves the listener by the UDP source address
// of the inbound packet (which peer's endpoint sent it). The sender being
// controlled is named in the message payload — userName, since clients don't
// know the hub's internal peer IDs.
//
// /sb/hxmute   s:senderUserName  i:muted   (1 = silent in this listener's mix)
// /sb/hxgain   s:senderUserName  f:gain    (linear, 1.0 = unity)
// /sb/hxlevel  s:senderUserName  f:peak    (linear 0..1; sender's pre-mix peak)
// /sb/hxsolo   s:senderUserName  i:soloed  (1 = if any are soloed, only those)
//
// Defaults are mute=false, gain=1.0, soloed=false, which preserves
// pre-Phase-3 behavior (unweighted mix-minus-self). Solo is per-listener
// (Phase 5): if any of a listener's overrides have soloed=true, that
// listener's mix contains only the soloed senders; otherwise standard mix.
//
// Phase 4: hxlevel travels in the opposite direction (hub → listener) so each
// listener can render virtual meters on info-only rows for senders whose audio
// only reaches them mixed into the hub stream. Sent at ~20Hz from a worker
// thread on the hub; never touches the audio thread.

#pragma once

#define SB_HXMSG_DOMAIN     "/sb"
#define SB_HXMSG_DOMAIN_LEN 3

#define SB_HXMSG_MUTE       "/hxmute"
#define SB_HXMSG_MUTE_LEN   7
#define SB_HXMSG_GAIN       "/hxgain"
#define SB_HXMSG_GAIN_LEN   7
#define SB_HXMSG_LEVEL      "/hxlevel"
#define SB_HXMSG_LEVEL_LEN  8
#define SB_HXMSG_SOLO       "/hxsolo"
#define SB_HXMSG_SOLO_LEN   7

// Listener asks the hub to pump <senderUserName>'s audio to the listener's oursink at ourId <N>.
// The hub allocates a per-(listener, sender) aoo::source at the sender's actual recvChannels and adds the
// listener's oursink as its destination. Re-sending with a different ourId reconfigures the existing feed.
//
// /sb/hxfeedreq s:senderUserName i:myOurIdForSender s:myUserName
//
// myUserName lets the hub self-heal its peer table when AOO PEER_JOIN events get lost across rapid hub bounces.
// The hub treats it as optional and defaults to "".
#define SB_HXMSG_FEEDREQ      "/hxfeedreq"
#define SB_HXMSG_FEEDREQ_LEN  10

// Channel layout / per-channel labels. Stock SonoBus uses /sb/clayinfo over the same OSC /sb wire to carry
// per-channel labels in direct P2P (SonobusPluginProcessor SONOBUS_MSGTYPE_LAYOUTINFO). In hub-relay mode the hub
// catches the sender's /sb/clayinfo and forwards a copy to every listener of that sender with the sourceid
// rewritten to the listener's local relayed-peer ourId, so the listener's existing handler finds the right
// RemotePeer and renders the labels.
//
// /sb/clayinfo i:sourceid b:<ValueTree-in-binary layout blob>
#define SB_HXMSG_CLAYINFO     "/clayinfo"
#define SB_HXMSG_CLAYINFO_LEN 9

// Stock SonoBus chat rides the same /sb OSC wire (/sb/chat s:group s:from s:targets s:tags s:message; see
// SonobusPluginProcessor SONOBUS_MSGTYPE_CHAT / sendChatEvent). In HostedMix the hub is the ONLY endpoint clients
// have, so the hub must relay chat to the other peers. The payload is self-describing (receivers attribute by the
// 'from' field, not by UDP source address), so the hub relays the original bytes verbatim, honoring the
// "|"-separated 'targets' field (empty = everyone).
#define SB_HXMSG_CHAT     "/chat"
#define SB_HXMSG_CHAT_LEN 5

// Hub sends ONCE to each peer at PEER_JOIN time: "I'm the hub, here's my user name and audio UDP port." If the
// client missed the AOO PEER_JOIN event for the hub, its handler force-adds the hub peer into its remote-peer list.
// Fires once per join, never periodic, so no steady-state wire activity. See SonobusHubServer::addPeer + the client
// handler at SONOBUS_MSGTYPE_HXHUBHELLO.
//
// /sb/hxhubhello  s:hubUserName  i:hubAudioPort
#define SB_HXMSG_HUBHELLO     "/hxhubhello"
#define SB_HXMSG_HUBHELLO_LEN 11
// Hub -> hosted clients, ~1 Hz per other player:
// /sb/hxlat  s:userName  f:toHubMs  f:fromHubMs
#define SB_HXMSG_LAT     "/hxlat"
#define SB_HXMSG_LAT_LEN 6

#define SB_HXFULLMSG_MUTE      SB_HXMSG_DOMAIN SB_HXMSG_MUTE
#define SB_HXFULLMSG_GAIN      SB_HXMSG_DOMAIN SB_HXMSG_GAIN
#define SB_HXFULLMSG_LEVEL     SB_HXMSG_DOMAIN SB_HXMSG_LEVEL
#define SB_HXFULLMSG_SOLO      SB_HXMSG_DOMAIN SB_HXMSG_SOLO
#define SB_HXFULLMSG_FEEDREQ   SB_HXMSG_DOMAIN SB_HXMSG_FEEDREQ
#define SB_HXFULLMSG_CLAYINFO  SB_HXMSG_DOMAIN SB_HXMSG_CLAYINFO
#define SB_HXFULLMSG_CHAT      SB_HXMSG_DOMAIN SB_HXMSG_CHAT
#define SB_HXFULLMSG_HUBHELLO  SB_HXMSG_DOMAIN SB_HXMSG_HUBHELLO
#define SB_HXFULLMSG_LAT       SB_HXMSG_DOMAIN SB_HXMSG_LAT

#include <cstdint>
#include <string>

// Deterministic feed source id. Hub and listener client compute this independently from the same (listener,
// sender) pair. The listener uses it to oursink->invite_source(hubEndpoint, id, ...); that invite makes the hub
// source emit its format announce immediately (otherwise AOO's source only emits format on add_sink or on a sink
// explicitly requesting it after receiving unsolicited data, which can take ~10s). Range chosen to clear low
// reserved AOO ids.
inline int32_t sb_feedSourceIdFor(const std::string& listenerName,
                                  const std::string& senderName) {
    constexpr uint64_t FNV_OFFSET = 1469598103934665603ull;
    constexpr uint64_t FNV_PRIME  = 1099511628211ull;
    uint64_t h = FNV_OFFSET;
    auto mix = [&](const std::string& s) {
        for (unsigned char c : s) { h ^= c; h *= FNV_PRIME; }
    };
    mix(listenerName);
    mix(std::string("\x1f\xe2\x86\x92\x1f"));  // separator: 0x1f → 0x1f, won't collide
    mix(senderName);
    return (int32_t)(10000 + (h % 1000000));
}
