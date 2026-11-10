// SPDX-License-Identifier: GPLv3-or-later WITH Appstore-exception
// Copyright (C) 2026 Jason Buecker (Random Planet)
// See SonobusHubWeb.h
#include "SbLatencyColours.h"
#include "SonobusHubWeb.h"
#include "SonobusHubServer.h"
#include "SonobusHubSettings.h"
#include "SonobusHubWebIcon.h"   // Embedded hub icon for GET /icon.png

#include <algorithm>
#include <chrono>
#include <cstring>
#include <sstream>
#include <iomanip>
#include <iostream>
#include <vector>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

static long long webNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
static std::string jsonEscape(const std::string& in) {
    std::string o; o.reserve(in.size() + 8);
    for (unsigned char c : in) {
        switch (c) {
            case '"': o += "\\\""; break; case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break; case '\r': o += "\\r"; break; case '\t': o += "\\t"; break;
            default: if (c < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", c); o += b; } else o += (char) c;
        }
    }
    return o;
}
// "Linux 6.8 (Ubuntu 22.04)" / "macOS 15.7" for the page header
static std::string webPlatform() { return hubPlatformString(); }
static std::string b64decode(const std::string& in) {
    static const std::string T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out; int val = 0, bits = -8;
    for (unsigned char c : in) {
        if (c == '=') break;
        size_t p = T.find((char) c); if (p == std::string::npos) continue;
        val = (val << 6) + (int) p; bits += 6;
        if (bits >= 0) { out.push_back((char)((val >> bits) & 0xFF)); bits -= 8; }
    }
    return out;
}

SonobusHubWeb::SonobusHubWeb(SonobusHubServer& hub, int port, std::string password)
    : mHub(hub), mPort(port), mPassword(std::move(password)), mStartedAtMs(webNowMs()) {}
SonobusHubWeb::~SonobusHubWeb() { stop(); if (mUpdateThread.joinable()) mUpdateThread.join(); }

bool SonobusHubWeb::start() {
    if (mPort <= 0) return false;
    mListenFd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (mListenFd < 0) { std::cerr << "[HUB-WEB] socket: " << strerror(errno) << "\n"; return false; }
    int one = 1; ::setsockopt(mListenFd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in sa; memset(&sa, 0, sizeof(sa)); sa.sin_family = AF_INET; sa.sin_addr.s_addr = htonl(INADDR_ANY); sa.sin_port = htons((uint16_t) mPort);
    if (::bind(mListenFd, (struct sockaddr*) &sa, sizeof(sa)) < 0 || ::listen(mListenFd, 16) < 0) {
        std::cerr << "[HUB-WEB] bind/listen on port " << mPort << ": " << strerror(errno) << "\n";
        ::close(mListenFd); mListenFd = -1; return false;
    }
    mRun = true;
    mThread = std::thread([this] { threadMain(); });
    if (!mSharedGeo) mGeo.start([this] { std::lock_guard<std::mutex> c(mCacheLock); mCachedAtMs = 0; });   // Next poll rebuilds with the new location; the GUI's shared resolver is already running
    std::cout << "[HUB-WEB] status page on http://0.0.0.0:" << mPort << "/ (" << (mPassword.empty() ? "no password" : "password set") << ")\n";
    return true;
}

void SonobusHubWeb::stop() {
    if (!mRun.exchange(false)) return;
    if (mListenFd >= 0) { ::shutdown(mListenFd, SHUT_RDWR); ::close(mListenFd); mListenFd = -1; }
    if (mThread.joinable()) mThread.join();
    if (!mSharedGeo) mGeo.stop();
}

void SonobusHubWeb::threadMain() {
    while (mRun) {
        struct pollfd pf; pf.fd = mListenFd; pf.events = POLLIN; pf.revents = 0;
        const int r = ::poll(&pf, 1, 250);
        if (r <= 0) continue;
        struct sockaddr_in ca; socklen_t cl = sizeof(ca);
        const int fd = ::accept(mListenFd, (struct sockaddr*) &ca, &cl);
        if (fd < 0) continue;
        struct timeval tv; tv.tv_sec = 3; tv.tv_usec = 0;
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)); ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        handleClient(fd);
        ::close(fd);
    }
}

bool SonobusHubWeb::authorised(const std::string& headers) const {
    if (mPassword.empty()) return true;
    const std::string key = "authorization: basic ";
    std::string low; low.reserve(headers.size());
    for (char c : headers) low.push_back((char) tolower((unsigned char) c));
    size_t p = low.find(key); if (p == std::string::npos) return false;
    size_t e = headers.find("\r\n", p); if (e == std::string::npos) e = headers.size();
    const std::string cred = b64decode(headers.substr(p + key.size(), e - p - key.size()));
    const size_t colon = cred.find(':'); if (colon == std::string::npos) return false;
    const std::string pw = cred.substr(colon + 1);
    if (pw.size() != mPassword.size()) return false;
    unsigned diff = 0; for (size_t i = 0; i < pw.size(); ++i) diff |= (unsigned)(pw[i] ^ mPassword[i]);
    return diff == 0;
}

static void sendAll(int fd, const std::string& s) {
    size_t off = 0;
    while (off < s.size()) {
        const ssize_t n = ::send(fd, s.data() + off, s.size() - off,
#ifdef MSG_NOSIGNAL
                                 MSG_NOSIGNAL
#else
                                 0
#endif
                                 );
        if (n <= 0) return; off += (size_t) n;
    }
}
static std::string httpResponse(int code, const char* text, const std::string& type, const std::string& body, const std::string& extra = "", const char* cache = "no-store") {
    std::ostringstream o;
    o << "HTTP/1.1 " << code << " " << text << "\r\nContent-Type: " << type << "\r\nContent-Length: " << body.size()
      << "\r\nCache-Control: " << cache << "\r\nConnection: close\r\n" << extra << "\r\n" << body;
    return o.str();
}

static const char* kPage = R"HTML(<!doctype html><html><head><meta charset="utf-8"><title>SonoBus RP Hub</title>
<link rel="icon" type="image/png" href="icon.png">
<meta name="viewport" content="width=device-width,initial-scale=1">
<style>
:root{--bg:#0e0e0e;--card:#161616;--line:#262626;--text:#e6e6e6;--dim:#9a9a9a;--teal:#3fb8af;--teal2:#1d3b3a}
body{background:var(--bg);color:var(--text);font:14px/1.4 -apple-system,"Segoe UI",Helvetica,Arial,sans-serif;margin:0;padding:18px 20px 40px}
h1{font-size:22px;margin:0;color:var(--teal);font-weight:600;line-height:1.2}
.logo{width:48px;height:48px;border-radius:10px;flex:0 0 auto}
.titles{display:flex;flex-direction:column;gap:2px;min-width:0}
h2{font-size:15px;margin:0 0 10px;color:var(--teal);font-weight:600}
.card{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:14px 16px;margin:0 0 14px}
.head{display:flex;flex-wrap:wrap;gap:6px 14px;align-items:center}
.sub{color:var(--dim);font-size:15px}
.sig{display:inline-block;width:9px;height:9px;border-radius:50%;margin-right:6px;vertical-align:middle}
.sig.on{background:#5fd26b}.sig.off{background:#555}
.kvs{display:flex;flex-wrap:wrap;gap:6px 22px;margin-top:10px}
.kv{color:var(--dim);white-space:nowrap;font-variant-numeric:tabular-nums}.kv b{color:var(--text);font-weight:600}
.tools{display:flex;flex-wrap:wrap;gap:10px 14px;align-items:center;margin-top:12px}
button{background:var(--teal2);color:var(--text);border:1px solid var(--teal);border-radius:6px;padding:6px 14px;font:13px inherit;cursor:pointer}
button:hover{background:#245452}button:disabled{opacity:.5;cursor:default}
button.warn{border-color:#e3c141;background:#3b3417}
.link{color:var(--dim);font-family:ui-monospace,Menlo,Consolas,monospace;font-size:12px;word-break:break-all}
.tabs{display:flex;gap:4px;margin:0 0 12px;border-bottom:1px solid var(--line)}
.tab{color:var(--dim);text-decoration:none;padding:8px 18px;border:1px solid transparent;border-bottom:0;border-radius:8px 8px 0 0;font-weight:600;font-size:14px}
.tab:hover{color:var(--text)}.tab.on{color:var(--teal);background:var(--card);border-color:var(--line)}
.pane{display:none}.pane.on{display:block}
table{border-collapse:collapse;width:100%;table-layout:fixed;font-variant-numeric:tabular-nums}
th,td{padding:7px 6px;text-align:left;white-space:nowrap;border-bottom:1px solid var(--line);overflow:hidden}
th{color:var(--dim);font-weight:600;position:sticky;top:0;background:var(--card);font-size:11.5px;text-transform:uppercase;letter-spacing:.02em;white-space:normal;line-height:1.15;vertical-align:bottom}
th.num,td.num{text-align:right}
#ptable th{cursor:pointer;user-select:none}
td.user{white-space:normal;word-break:break-word}
tr:last-child td{border-bottom:0}
.g{color:#5fd26b}.y{color:#e3c141}.r{color:#ff5e5e}.d{color:#777}.rec{color:#ff5e5e;font-weight:700}
.wrap{overflow:auto}
.foot{display:flex;flex-wrap:wrap;gap:6px 22px;margin-top:10px;color:var(--dim)}
.form{display:grid;grid-template-columns:1fr 1fr;gap:4px 32px}
@media (max-width:900px){.form{grid-template-columns:1fr}}
.col{min-width:0}
.sec{color:var(--teal);font-weight:600;margin:14px 0 6px;font-size:14px}.sec:first-child{margin-top:0}
.row{display:grid;grid-template-columns:minmax(220px,1fr) minmax(160px,220px) 64px;gap:10px;align-items:center;padding:4px 0}
.row label{color:var(--text)}
.row input[type=text],.row input[type=password],.row select{width:100%;box-sizing:border-box;background:#1f1f1f;color:var(--text);border:1px solid #333;border-radius:5px;padding:6px 8px;font:13px inherit}
.row input:focus,.row select:focus{outline:none;border-color:var(--teal)}
.row input[type=checkbox]{width:18px;height:18px;accent-color:var(--teal)}
.tag{font-size:11px;color:var(--dim);text-transform:uppercase;letter-spacing:.04em}
.tag.live{color:#5fd26b}.tag.restart{color:#e3c141}.tag.relaunch{color:#f0a04b}
.msg{margin-left:6px;color:var(--dim)}.msg.ok{color:#5fd26b}.msg.err{color:#ff5e5e}
.notice{color:var(--dim);padding:6px 0}
iframe.log{width:100%;height:70vh;border:0;background:#0e0e0e;border-radius:6px}

button.slink{padding:2px 8px;font-size:12px;margin-right:6px}button.srm{padding:1px 7px;font-size:12px;background:#3a2020;border-color:#5a2a2a}button.srm:hover{background:#6a2a2a}#stable tbody tr[data-g]{cursor:pointer}#stable tbody tr[data-g]:hover td{background:#1b1b1b}#stable tbody tr.sel td{background:var(--teal2)}
td.pl{white-space:normal;word-break:break-word;color:var(--dim)}
#grp{width:230px}#gmsg{width:320px}.tools label{color:var(--dim)}.tools input[type=password],.tools input[type=text]{background:#1f1f1f;color:var(--text);border:1px solid #333;border-radius:5px;padding:6px 8px;font:13px inherit;width:230px}
.tools input[type=text]:focus,.tools input[type=password]:focus{outline:none;border-color:var(--teal)}
</style></head><body>
<div class="card">
 <div class="head"><img class="logo" src="/icon.png" alt=""><div class="titles"><h1 id="title">SonoBus RP Hub</h1><span class="sub" id="where"></span></div></div>
 <div class="kvs" id="hub"><span class="kv"><b id="k_upd"></b></span><span class="kv">uptime <b id="k_up">–</b></span><span class="kv">players <b id="k_n">–</b></span><span class="kv">audio <b id="k_audio">–</b></span><span class="kv">codec <b id="k_codec">–</b></span><span class="kv">hub CPU <b id="k_cpu">–</b></span><span class="kv">server <b id="k_srv">–</b></span><span class="kv">audio UDP <b id="k_udp">–</b></span><span class="kv">version <b id="k_ver">–</b> <span class="d" id="k_build"></span></span><span class="kv r" id="k_err"></span></div>
 <div class="tools">
  <button id="copy" title="Copy this hub's group link for players">Copy Group Link</button><span id="link" class="link"></span>
  <span style="flex:1 1 auto"></span>
  <span id="pcwrap" style="display:flex;flex-direction:column;align-items:flex-end;gap:2px"><span id="pc" class="sub"></span><span id="pcl" class="sub d" style="font-size:12px"></span></span><button id="pcbtn" title="Ask a connected player to probe this hub's ports from outside again">Re-run port check</button>
  <button id="restartbtn" class="warn" title="Restart the hub. Every player drops and rejoins.">Restart Hub</button>
  <button id="updbtn" class="warn" style="display:none" title="Download and install the new hub. The hub restarts, so every player drops and rejoins.">Install Update</button>
  <button id="viewlog" title="Open the hub's connection log in a new window">View Log</button>
 </div>
</div>
<div class="tabs" style="position:relative"><a href="/licenses" target="_blank" class="d" style="position:absolute;right:4px;bottom:6px;font-size:12px" title="Third-party notices for the libraries in this hub">Notices</a><a class="tab" href="#status" data-tab="status" title="Players, group link and recent sessions.">Status</a><a class="tab" href="#settings" data-tab="settings" title="Hub settings.">Settings</a><a class="tab" href="#log" data-tab="log" title="Connection history and the hub log.">Log</a></div>
<div class="card pane" id="pane-status">
 <div class="tools" style="margin:0 0 12px"><label for="grp">Group</label><input type="text" id="grp" title="Type a new group name, then click Apply."><button id="grpbtn" class="warn" title="Switch the hub to this group. The hub restarts and players must rejoin.">Apply (restarts hub)</button><span style="flex:1 1 auto"></span><input type="text" id="gmsg" maxlength="1000" placeholder="Message to the group" title="Sends a chat message to every player, from Hub."><button id="gmsgbtn" title="Send this message to every player.">Send</button><span id="gmsgnote" class="msg"></span></div>
 <h2>Players<span id="pcount" style="color:var(--dim);font-weight:400;font-size:13px;margin-left:12px" title="Players connected right now, and how many different players have joined since the hub started"></span></h2>
 <div class="wrap"><table id="ptable"><colgroup><col style="width:180px"><col style="width:104px"><col style="width:52px"><col style="width:74px"><col style="width:72px"><col style="width:72px"><col style="width:136px"><col style="width:76px"><col style="width:58px"><col style="width:64px"><col style="width:56px"><col style="width:78px"><col style="width:66px"><col style="width:76px"><col style="width:88px"><col style="width:60px"><col style="width:72px"><col style="width:92px"></colgroup><thead><tr>
 <th>Player</th><th title="City / region looked up from the player's public address">Location</th><th class="num">Inputs</th><th title="Signal level of this player's inputs as the hub receives them">Signal</th><th class="num" title="What this player sends into the hub (kb/s or Mb/s, bits)">Up</th><th class="num" title="What the hub sends to this player (kb/s or Mb/s, bits)">Down</th><th>Address</th><th class="num" title="Blocks the hub lost from this player's uplink (late past the hold or never arrived) — hub-side, unlike Dropouts">Lost (hub)</th><th class="num" title="This player's audio reaching the hub (ms): network one-way + the hub's hold on their stream">To hub</th><th class="num" title="The hub's audio reaching this player (ms): network one-way + the player's own receive buffer">From hub</th><th class="num" title="How full the hub's jitter buffer for this player's stream is">JitBuf</th><th class="num" title="Arrival jitter of this player's packets at the hub (p99 of the last window) — the number the hold follows">Net jitter</th><th class="num" title="The hub's hold on this player's incoming stream (ms)">Hub buf</th><th class="num" title="The player's own receive buffer (ms) as it reports it; A = Auto, M = Manual">Player buf</th><th class="num" title="Underruns + pops the player itself heard since it joined (player-side); (+n) = in the last 10 s; xN = CoreAudio overloads in the last 10 s">Dropouts</th><th class="num" title="Age of the last hub latency report the player received">Lat feed</th><th title="The player's audio device buffer (samples) @ sample rate">Device</th><th title="Time since this player joined">Connected for</th></tr></thead><tbody id="rows"><tr id="norows"><td colspan="17" class="d">no players</td></tr></tbody></table></div>
 <div class="foot"><span class="kv">IN <b id="f_in">–</b></span><span class="kv">OUT <b id="f_out">–</b></span></div>

 <h2 style="margin-top:20px" title="Groups this hub has served (newest first). Click one to see its players and to switch the hub back to it with the settings it ran with.">Recent sessions</h2>
 <div class="wrap"><table id="stable"><colgroup><col style="width:220px"><col style="width:150px"><col style="width:150px"><col style="width:70px"><col><col style="width:44px"></colgroup><thead><tr><th>Group</th><th>First used</th><th>Last used</th><th class="num">Players</th><th title="Group link for this session (this hub's address + that group and password)">Group link</th><th></th></tr></thead><tbody id="sessions"><tr><td colspan="6" class="d">loading…</td></tr></tbody></table></div>
 <div id="sdetail" style="display:none">
  <h2 style="margin-top:18px" id="sdtitle">Session</h2>
  <div class="wrap"><table id="rtable"><colgroup><col style="width:180px"><col style="width:150px"><col style="width:150px"><col style="width:150px"><col style="width:56px"><col style="width:56px"><col style="width:40px"></colgroup><thead><tr><th>Player</th><th>Location</th><th>Last joined</th><th>Last left</th><th class="num">Joins</th><th>Net</th><th></th></tr></thead><tbody id="recent"><tr><td colspan="6" class="d">–</td></tr></tbody></table></div>
  <div class="tools">
   <label for="spw">Password</label><input type="password" id="spw" placeholder="(no password)" autocomplete="new-password" title="The password the hub will use for this group — edit it, or clear it for no password">
   <button id="sapply" class="warn" title="Write this session's group, password and hub settings to hub_settings.json and restart the hub — players must rejoin">Apply and restart hub</button>
   <span id="sdmsg" class="msg"></span>
  </div>
 </div>
</div>
<div class="card pane" id="pane-settings">
 <h2>Settings</h2>
 <div id="settingsNote" class="notice">loading…</div>
 <div class="form" id="form" style="display:none"><div class="col" id="col0"></div><div class="col" id="col1"></div></div>
 <div class="tools" id="settingsTools" style="display:none">
  <button id="reset" title="Put every setting back to its default, keeping your group name and password.">Reset to Defaults</button><button id="reload" title="Undo your edits and go back to the last saved settings.">Revert</button><button id="save" title="Save these settings. Some take effect after Save &amp; Restart.">Save</button><button id="saveRestart" class="warn" title="Save these settings and restart the hub. Players must rejoin.">Save &amp; Restart</button>
  <span id="smsg" class="msg"></span>
 </div>
</div>
<div class="card pane" id="pane-log">
 <h2>Log</h2>
 <iframe class="log" id="logframe" title="hub log"></iframe>
</div>
<script>
let prev=null,prevT=0,restarting=false,curTab='';
const $=(id)=>document.getElementById(id);
const api=(p)=>location.origin+p;   // absolute: a page opened as http://user:pw@host/ must not pass the credentials into fetch()
const esc=(s)=>String(s).replace(/[&<>"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]));
const kb=(b)=> b<0?'–':(b*8>=1e6?(b*8/1e6).toFixed(1)+' Mb/s':Math.round(b*8/1000)+' kb/s')
const LATC={g:__LATG__,y:__LATY__,rg:__LATRG__,ry:__LATRY__};   // shared thresholds (SbLatencyColours.h), filled at serve time
const col=(v,g,y)=> v<g?'g':v<y?'y':'r';
const dur=(s)=>{s=Math.max(0,Math.floor(s));const h=Math.floor(s/3600),m=Math.floor(s%3600/60);return (h?h+'h ':'')+String(m).padStart(h?2:1,'0')+'m '+String(s%60).padStart(2,'0')+'s';};

// place only when its text changes (no innerHTML rebuild each second) so nothing shifts and a text selection
// survives the 1-s refresh (copy/paste works).
const setT=(el,t)=>{if(el.textContent!==t)el.textContent=t;};
const setH=(el,h)=>{if(!el||!el.dataset)return;if(el.dataset.h!==h){el.dataset.h=h;el.innerHTML=h;}};   // guard (Safari threw on a missing cell)
const rowMap=new Map();const COLS=18;

// order); numeric columns sort by the value behind the cell ('–'/unknown lowest), text columns case-insensitively.
// The choice is kept in JS state (re-applied after every 1-s rebuild) and per browser in localStorage.
const NUMC=new Set([2,4,5,7,8,9,10,11,12,13,14,15,16,17]);
let sort={c:-1,d:1};try{const s=JSON.parse(localStorage.getItem('sbhub.sort')||'null');if(s&&typeof s.c==='number'&&(s.d===1||s.d===-1))sort=s;}catch(e){}
const rowKeys=new Map();
const ths=[...document.querySelectorAll('#ptable thead th')];
ths.forEach((th,i)=>{th.dataset.label=th.textContent;th.onclick=()=>{if(sort.c!==i)sort={c:i,d:1};else if(sort.d===1)sort={c:i,d:-1};else sort={c:-1,d:1};try{localStorage.setItem('sbhub.sort',JSON.stringify(sort));}catch(e){}drawSortMarks();applySort();};});
function drawSortMarks(){ths.forEach((th,i)=>setT(th,th.dataset.label+(sort.c===i?(sort.d===1?' ▲':' ▼'):'')));}
function applySort(){if(sort.c<0)return;const c=sort.c,d=sort.d;
 const ids=[...rowMap.keys()].sort((a,b)=>{const ka=rowKeys.get(a)[c],kb=rowKeys.get(b)[c];const r=NUMC.has(c)?(ka-kb):String(ka).localeCompare(String(kb),undefined,{sensitivity:'base',numeric:true});return r*d;});
 const tb=$('rows');const cur=[...tb.children].filter(tr=>tr.id!=='norows').map(tr=>Number(tr.dataset.id));
 if(cur.length===ids.length&&cur.every((id,i)=>id===ids[i]))return;   // already in order: touch nothing (stationary page)
 for(const id of ids)tb.appendChild(rowMap.get(id));}

// JSON changes; the selection (by group name) is JS state, so a refresh never loses it. Selecting a row shows that
// session's personnel + a password field (blank = the session's own password) + "Apply and restart hub".
let sessLast='',sessN=0,sessions=[],selGroup=null,pwPrefillGroup=null;
const ft=(ms)=>ms>0?new Date(ms).toLocaleString([],{month:'numeric',day:'numeric',year:'numeric',hour:'numeric',minute:'2-digit'}):'–';
async function loadSessions(force){try{const r=await fetch(api('/api/sessions'),{cache:'no-store'});if(!r.ok)return;const txt=await r.text();if(txt===sessLast&&!force)return;sessLast=txt;sessions=JSON.parse(txt).sessions||[];drawSessions();}catch(e){}}
const sSort={col:-1,desc:false},rSort={col:-1,desc:false},SKEYS=['group','firstUsedMs','lastUsedMs','playerCount'],RKEYS=['user','location','lastJoinMs','lastLeftMs','joins','lan'];
function sortRows(rows,st,keys){if(st.col<0||st.col>=keys.length)return rows;const k=keys[st.col],m=st.desc?-1:1;return rows.slice().sort((a,b)=>{const x=a[k],y=b[k];return m*((typeof x==='string')?String(x).localeCompare(String(y),undefined,{sensitivity:'base'}):(x>y)-(x<y));});}
function hookSort(id,st,keys,draw){const ths=document.querySelectorAll('#'+id+' thead th');const base=[...ths].map(t=>t.textContent);ths.forEach((t,i)=>{if(i>=keys.length)return;t.style.cursor='pointer';t.onclick=()=>{if(st.col!==i){st.col=i;st.desc=false;}else if(!st.desc)st.desc=true;else st.col=-1;ths.forEach((h,j)=>{h.textContent=base[j]+(j===st.col?(st.desc?' ▼':' ▲'):'');});draw();};});}
function drawSessions(){const tb=$('sessions');
 tb.innerHTML=sessions.length?sortRows(sessions,sSort,SKEYS).map(s=>`<tr data-g="${esc(s.group)}" class="${s.group===selGroup?'sel':''}"><td class="user">${esc(s.group)}${s.password?' <span class="d">pw: '+esc(s.password)+'</span>':''}${s.current?' <span class="g" title="the group the hub is serving now">● current</span>':''}</td><td>${ft(s.firstUsedMs)}</td><td>${ft(s.lastUsedMs)}</td><td class="num">${s.playerCount}</td><td class="pl"><button class="slink" data-link="${esc(s.link||'')}" title="Copy this session's group link">Copy link</button> <span class="d link">${esc(s.link||'')}</span></td><td class="num">${s.current?'':'<button class="srm" title="Remove this session from the list">✕</button>'}</td></tr>`).join(''):'<tr><td colspan="6" class="d">no sessions yet</td></tr>';
 tb.querySelectorAll('tr[data-g]').forEach(tr=>{tr.onclick=()=>{selGroup=(selGroup===tr.dataset.g)?null:tr.dataset.g;$('sdmsg').textContent='';drawSessions();};});
 tb.querySelectorAll('button.slink').forEach(b=>{b.onclick=async(ev)=>{ev.stopPropagation();const ok=await copyText(b.dataset.link);b.textContent=ok?'Copied':'Copy failed';setTimeout(()=>b.textContent='Copy link',1500);};});
 tb.querySelectorAll('button.srm').forEach(b=>{b.onclick=async(ev)=>{ev.stopPropagation();const g=b.closest('tr').dataset.g;if(!confirm('Remove the session "'+g+'" from Recent sessions?'))return;
   try{const r=await fetch(api('/api/sessions/remove'),{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({group:g})});const d=await r.json();if(!d.ok){alert(d.error||'remove failed');return;}if(selGroup===g)selGroup=null;loadSessions(true);}catch(e){alert('remove failed');}};});
 drawDetail();}
function drawDetail(){const picked=sessions.find(x=>x.group===selGroup),s=picked||sessions.find(x=>x.current);const d=$('sdetail');if(!s){d.style.display='none';return;}d.style.display='';
 $('spw').closest('.tools').style.display=picked?'':'none';
 setT($('sdtitle'),(picked?'Players seen in "'+s.group+'"':'Players seen in this session')+' ('+s.playerCount+')');
 $('recent').innerHTML=s.players.length?sortRows(s.players,rSort,RKEYS).map(p=>`<tr><td class="user">${esc(p.user)}</td><td>${p.location?esc(p.location):'<span class="d">–</span>'}</td><td>${ft(p.lastJoinMs)}</td><td>${p.lastLeftMs>=p.lastJoinMs?ft(p.lastLeftMs):'<span class="g">connected</span>'}</td><td class="num">${p.joins}</td><td>${p.lan?'LAN':'WAN'}</td><td>${p.lastLeftMs>=p.lastJoinMs?`<button class="srm prm" data-u="${esc(p.user)}" title="Remove this player from the session's list">✕</button>`:''}</td></tr>`).join(''):'<tr><td colspan="7" class="d">no players recorded in this session</td></tr>';
 $('recent').querySelectorAll('button.prm').forEach(b=>{b.onclick=async()=>{const u=b.dataset.u;if(!confirm('Remove "'+u+'" from this session\'s players?'))return;
   try{const r=await fetch(api('/api/sessions/removeplayer'),{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({group:s.group,user:u})});const d=await r.json();if(!d.ok){alert(d.error||'remove failed');return;}loadSessions(true);}catch(e){alert('remove failed');}};});
 if(pwPrefillGroup!==selGroup){$('spw').value=s.password||'';pwPrefillGroup=selGroup;}$('spw').placeholder=s.password?'':'(no password — type one to set it)';
 $('sapply').disabled=restarting;}
$('sapply').onclick=async()=>{const s=sessions.find(x=>x.group===selGroup);if(!s)return;
 if(!confirm('Switch the hub to group "'+s.group+'" with the settings it ran with, and restart? Every player will have to rejoin.'))return;
 const m=$('sdmsg');m.textContent='applying…';m.className='msg';
 try{const r=await fetch(api('/api/sessions/apply'),{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({group:s.group,password:$('spw').value})});const d=await r.json();
 if(!d.ok){m.textContent=d.error||('HTTP '+r.status);m.className='msg err';return;}
 restarting=true;$('spw').value='';$('sapply').disabled=true;m.textContent=`applied "${d.group}" (${d.applied} settings${d.passwordOverridden?', new password':''}) — hub restarting, players must rejoin…`;m.className='msg ok';}
 catch(e){m.textContent='apply failed: '+e;m.className='msg err';}};
function showTab(t){if(!['status','settings','log'].includes(t))t='status';curTab=t;
 document.querySelectorAll('.tab').forEach(a=>a.classList.toggle('on',a.dataset.tab===t));
 document.querySelectorAll('.pane').forEach(p=>p.classList.toggle('on',p.id==='pane-'+t));
 if(t==='log'&&!$('logframe').src)$('logframe').src=api('/log?embed=1');
 if(t==='settings')loadSettings();
 if(location.hash!=='#'+t)history.replaceState(null,'','#'+t);}
window.addEventListener('hashchange',()=>showTab(location.hash.slice(1)));
async function tick(){
 try{const r=await fetch(api('/api/status'),{cache:'no-store'});if(r.status==401){setT($('k_err'),'password required (reload and sign in)');return;}
 const d=await r.json();const now=Date.now();const dt=prev?(now-prevT)/1000:0;const h=d.hub;
 if(restarting){restarting=false;$('smsg').textContent='hub is back up';$('smsg').className='msg ok';loadSettings();$('sdmsg').textContent='hub is back up';$('sdmsg').className='msg ok';$('sapply').disabled=false;loadSessions(true);}
 setT($('title'),'SonoBus RP Hub — '+h.group);
 const tabTitle=(h.group?h.group+' · ':'')+'SonoBus RP Hub';if(document.title!==tabTitle)document.title=tabTitle;
 setT($('pcount'),d.peers.length+' now · '+(h.playersSinceStart||0)+' since hub start');
 setT($('where'),(h.platform||'')+(h.location?'  ·  '+h.location:''));
 setT($('link'),h.joinLink||'');
 const pcc=h.portCheckState==1?'g':h.portCheckState==2?'r':'d';
 const pcat=h.portVerifiedAtMs>0?' at '+new Date(h.portVerifiedAtMs).toLocaleTimeString([],{hour:'numeric',minute:'2-digit'}):'';
 setH($('pc'),`<span class="${pcc}">${esc((h.portCheck||'').replace(')',pcat+')'))}</span>`);
 setH($('pcl'),esc(h.portLocal||''));
 setT($('k_up'),dur(h.uptimeSec));setT($('k_n'),String(d.peers.length));setT($('k_audio'),`${h.sampleRate} Hz / ${h.blockSize} samples`);setT($('k_codec'),h.codec);
 setT($('k_cpu'),h.cpuPct.toFixed(1)+'%');setT($('k_srv'),`${h.publicAddress}:${h.serverPort}`);setT($('k_udp'),String(h.udpPort));
 updUrl=h.updateUrl||'';$('k_upd').style.cursor=updUrl?'pointer':'';if(document.activeElement!==$('grp'))$('grp').value=h.group||'';$('updbtn').style.display=h.updateReady?'':'none';setT($('k_upd'),h.update||'');setT($('k_ver'),'Hub '+(h.hubVersion||''));setT($('k_build'),`(build ${h.build||''}${h.commit?', '+h.commit:''})`);setT($('k_err'),'');
 if(curTab==='status'){
  const seen=new Set();
  for(const p of d.peers){seen.add(p.id);const pp=prev&&prev.peers.find(x=>x.id===p.id);
   const up=(pp&&dt>0)?(p.bytesIn-pp.bytesIn)/dt:-1, dn=(pp&&dt>0)?(p.bytesOut-pp.bytesOut)/dt:-1;
   const sig=p.muted?'<span class="sig off"></span><span class="d" title="this player muted their send">muted</span>':(p.blocksNonZero>0?'<span class="sig on"></span>sending':'<span class="sig off"></span><span class="d">silent</span>');
   const dc=p.drops==0?'d':p.drops<100?'y':'r';
   const lat=(v)=> v<0?'<span class="d">–</span>':`<span class="${col(v,LATC.g,LATC.y)}">${v.toFixed(1)}</span>`;
   const nj=p.jitP99<0?'–':`<span class="${p.jitP99<=1.5?'g':p.jitP99<=3?'y':'r'}">${p.jitP99.toFixed(1)}</span> <span class="d">${p.lan?'LAN':'WAN'}</span>`;
   const win=Math.max(p.rxUnderruns,0)+Math.max(p.rxPops,0);const gl=(p.rxUnderruns<0)?'<span class="d">–</span>':`<span class="${(p.rxUnderrunsTotal+p.rxPopsTotal)==0?'d':''}">${p.rxUnderrunsTotal+p.rxPopsTotal}</span>${win>0?` <span class="y">(+${win})</span>`:''}${p.xruns>0?` <span class="r">x${p.xruns}</span>`:''}`;
   const jb=p.jitBufPct<0?'–':`<span class="${p.jitBufPct>=90?'g':p.jitBufPct>=50?'y':'r'}">${p.jitBufPct.toFixed(0)}%</span>`;
   const lf=(p.latFeedAgeS===undefined||p.latFeedAgeS<0)?'<span class="d">–</span>':(p.latFeedAgeS<=35?`<span class="g">${p.latFeedAgeS}s</span>`:`<span class="r">dark ${p.latFeedAgeS}s</span>`);
   const loc=p.location===''?'<span class="d">…</span>':esc(p.location);
   const dev=p.devBuf>0?`${p.devBuf} @ ${Math.round(p.devSr/1000)}k`:'–';
   const name=(p.recording?'<span class="rec">● </span>':'')+esc(p.user);
   const inputs=`${p.inputs}`;
   const hb=(p.sinkBufMs<0)?'<span class="d">–</span>':`${p.sinkBufMs.toFixed(1)}${p.holdAtCeiling?' ⚠':''}`;
   const pbm=(p.recvBufMode===1)?' <span class="d">A</span>':(p.recvBufMode===0)?' <span class="d">M</span>':'';
   const pb=(p.clientRecvBufMs===undefined||p.clientRecvBufMs<0)?'<span class="d">–</span>':`${p.clientRecvBufMs.toFixed(1)}${pbm}`;
   const cells=[name,loc,inputs,sig,kb(up),kb(dn),esc(p.addr),`<span class="${dc}">${p.drops}</span>`,lat(p.toHubMs),lat(p.fromHubMs),jb,nj,hb,pb,gl,lf,dev,dur((now-p.joinedAtMs)/1000)];

   rowKeys.set(p.id,[p.user.toLowerCase(),p.location||'',p.inputs,p.muted?'muted':(p.blocksNonZero>0?'sending':'silent'),up,dn,p.addr,p.drops,p.toHubMs,p.fromHubMs,p.jitBufPct,p.jitP99,p.sinkBufMs,(p.clientRecvBufMs===undefined?-1:p.clientRecvBufMs),(p.rxUnderruns<0)?-1:(p.rxUnderrunsTotal+p.rxPopsTotal),(p.latFeedAgeS===undefined?-1:p.latFeedAgeS),p.devBuf,(now-p.joinedAtMs)]);
   let tr=rowMap.get(p.id);
   if(!tr){tr=document.createElement('tr');tr.dataset.id=p.id;for(let i=0;i<COLS;i++){const td=document.createElement('td');if([2,4,5,7,8,9,10,11,12,13,14,15].includes(i))td.className='num';if(i===0)td.className='user';tr.appendChild(td);}$('rows').appendChild(tr);rowMap.set(p.id,tr);}
   cells.forEach((c,i)=>setH(tr.children[i],c));}
  for(const [id,tr] of rowMap){if(!seen.has(id)){tr.remove();rowMap.delete(id);rowKeys.delete(id);}}
  applySort();
  $('norows').style.display=rowMap.size?'none':'';
  if((sessN++%10)===0)loadSessions(false);
  const tin=(prev&&dt>0)?(h.bytesInTotal-prev.hub.bytesInTotal)/dt:-1,tout=(prev&&dt>0)?(h.bytesOutTotal-prev.hub.bytesOutTotal)/dt:-1;
  setT($('f_in'),kb(tin));setT($('f_out'),kb(tout));}
 prev=d;prevT=now;}catch(e){setT($('k_err'),restarting?'hub restarting…':'hub not responding: '+e);}
}
async function copyText(t){let ok=false;if(navigator.clipboard&&window.isSecureContext){try{await navigator.clipboard.writeText(t);ok=true;}catch(e){}}if(!ok){const ta=document.createElement('textarea');ta.value=t;ta.setAttribute('readonly','');ta.style.position='fixed';ta.style.opacity='0';document.body.appendChild(ta);ta.select();ta.setSelectionRange(0,t.length);try{ok=document.execCommand('copy');}catch(e){}document.body.removeChild(ta);}return ok;}
$('copy').onclick=async()=>{const ok=await copyText($('link').textContent);$('copy').textContent=ok?'Copied':'Copy failed';setTimeout(()=>$('copy').textContent='Copy Group Link',1500);};
$('pcbtn').onclick=async()=>{const r=await fetch(api('/api/portcheck'),{method:'POST'});if(r.status==403){$('pc').innerHTML='<span class="y">set web_password to use this</span>';}};
$('viewlog').onclick=()=>{window.open(api('/log'),'_blank');};
const post=(p,b)=>fetch(api(p),{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(b)});
const needPw=(r,d)=>{if(!r.ok||!d.ok)alert(d.error||('Set a Web password to use this (HTTP '+r.status+')'));};
$('restartbtn').onclick=async()=>{if(!confirm('Restart the hub? Every player drops and rejoins.'))return;try{const r=await post('/api/settings',{values:{},restart:true});needPw(r,await r.json().catch(()=>({})));}catch(e){}};
$('grpbtn').onclick=async()=>{const g=$('grp').value.trim();if(!g)return;if(!confirm('Switch the hub to group "'+g+'"? The hub restarts and players must rejoin.'))return;try{const r=await post('/api/settings',{values:{group:g},restart:true});needPw(r,await r.json().catch(()=>({})));}catch(e){}};
$('gmsgbtn').onclick=async()=>{const t=$('gmsg').value.trim();if(!t)return;try{const r=await post('/api/chat',{message:t});const d=await r.json().catch(()=>({}));needPw(r,d);if(d.ok){$('gmsg').value='';$('gmsgnote').textContent='sent to '+d.sent;setTimeout(()=>$('gmsgnote').textContent='',3000);}}catch(e){}};
$('gmsg').onkeydown=(e)=>{if(e.key==='Enter')$('gmsgbtn').click();};
$('k_upd').onclick=()=>{if(updUrl)window.open(updUrl,'_blank');};
$('updbtn').onclick=async()=>{if(!confirm('Install the update? The hub restarts, so every player drops and rejoins.'))return;
 try{const r=await fetch(api('/api/update/install'),{method:'POST'});const d=await r.json().catch(()=>({}));if(!r.ok||!d.ok)alert(d.error||('Set a Web password to install updates (HTTP '+r.status+')'));}catch(e){alert('update failed: '+e);}};
let fields=[],detected='',updUrl='';
function fieldHtml(f){
 const id='f_'+f.key,tip=esc(f.tip||'');let inp;
 if(f.kind==1) inp=`<input type="checkbox" id="${id}" ${f.value=='1'?'checked':''} title="${tip}">`;
 else if(f.kind==2) inp=`<select id="${id}" title="${tip}">${f.choices.split(',').map(c=>`<option value="${esc(c)}" ${c==f.value?'selected':''}>${esc(c)}</option>`).join('')}</select>`;
 else if(f.kind==3) inp=`<input type="password" id="${id}" value="" placeholder="${f.set?'(set — blank = keep)':'(none)'}" autocomplete="new-password" title="${tip}">`;
 else{const ph=f.key=='public_address'?(detected?detected+' (auto-detected)':'Not detected, enter your address'):'';inp=`<input type="text" id="${id}" value="${esc(f.value)}" placeholder="${esc(ph)}" title="${tip}">`;}
 const when=f.tier=='live'?'Takes effect as soon as you save.':'Takes effect after Save & Restart.';
 return `<div class="row"><label for="${id}" title="${tip}">${esc(f.label)}</label>${inp}<span class="tag ${f.tier}" title="${esc(when)}">${f.tier=='relaunch'?'restart':f.tier}</span></div>`;
}
async function loadSettings(){
 try{const r=await fetch(api('/api/settings'),{cache:'no-store'});const d=await r.json();
 if(!d.enabled){$('settingsNote').textContent=d.reason||'settings disabled';$('form').style.display='none';$('settingsTools').style.display='none';return;}
 fields=d.fields;detected=d.detected||'';$('settingsNote').textContent='Settings file: '+d.file+(d.haveFile?'':'. Not saved yet. Click Save to keep these settings.');
 const cols=['',''];for(const f of d.fields){cols[f.col]+=f.key?fieldHtml(f):`<div class="sec">${esc(f.label)}</div>`;}
 $('col0').innerHTML=cols[0];$('col1').innerHTML=cols[1];$('form').style.display='grid';$('settingsTools').style.display='flex';$('smsg').textContent='';
 }catch(e){$('settingsNote').textContent='settings unavailable: '+e;}
}
async function save(restart){
 const values={};for(const f of fields){if(!f.key)continue;const el=$('f_'+f.key);if(!el)continue;values[f.key]=f.kind==1?(el.checked?'1':'0'):el.value;}
 $('smsg').textContent='saving…';$('smsg').className='msg';
 try{const r=await fetch(api('/api/settings'),{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({values,restart})});const d=await r.json();
 if(!d.ok){$('smsg').textContent=d.error||('HTTP '+r.status);$('smsg').className='msg err';return;}
 if(d.restarting){restarting=true;$('smsg').textContent='saved — hub restarting, players must rejoin…';$('smsg').className='msg ok';return;}
 $('smsg').textContent=d.changed.length?`saved ${d.changed.length} change${d.changed.length>1?'s':''}`+(d.needRestart?` — ${d.needRestart} need Save & Restart`:''):'no changes';
 $('smsg').className='msg ok';loadSettings();}catch(e){$('smsg').textContent='save failed: '+e;$('smsg').className='msg err';}
}
$('save').onclick=()=>save(false);
$('saveRestart').onclick=()=>{if(confirm('Save and restart the hub? Every player will have to rejoin.'))save(true);};
$('reload').onclick=loadSettings;
$('reset').onclick=()=>{if(!confirm('Put every setting back to its default? Your group name and password stay. Nothing changes until you click Save.'))return;
 for(const f of fields){if(!f.key||f.key=='group'||f.kind==3)continue;const el=$('f_'+f.key);if(!el)continue;if(f.kind==1)el.checked=f.def=='1';else el.value=f.def;}
 $('smsg').textContent='Defaults restored. Click Save to keep them.';$('smsg').className='msg';};
const TIPS={Player:'Player name',Inputs:'How many channels this player sends','First used':'When this group was first hosted','Last used':'When this group was last hosted',Players:'How many players have joined this group',Group:'The group name players join','Last joined':'When this player last joined','Last left':'When this player last left',Joins:'How many times this player has joined',Net:'Whether this player is on the local network or the internet'};
document.querySelectorAll('th:not([title])').forEach(t=>{const m=TIPS[t.textContent.trim()];if(m)t.title=m;});
const KV={k_upd:'Whether a newer hub is available',k_up:'How long the hub has been running',k_n:'Players connected right now',k_audio:'Sample rate and block size',k_codec:'Audio format',k_cpu:'CPU used by the hub',k_srv:'The address and port players connect to',f_in:'Total audio coming into the hub',f_out:'Total audio going out of the hub'};
for(const k in KV){const e=$(k);if(e&&e.parentElement)e.parentElement.title=KV[k];}
hookSort('stable',sSort,SKEYS,drawSessions);hookSort('rtable',rSort,RKEYS,drawDetail);
drawSortMarks();
showTab(location.hash.slice(1));tick();setInterval(tick,1000);
</script></body></html>
)HTML";

// The page carries the shared latency colour thresholds (SbLatencyColours.h) so the web page, the Mac GUI and
// the client colour the same number the same way.
static std::string pageWithConstants() {
    std::string p(kPage);
    auto sub = [&p](const char* tok, float v) {
        const std::string t(tok); char b[32]; snprintf(b, sizeof b, "%g", (double) v);
        for (size_t at = p.find(t); at != std::string::npos; at = p.find(t, at)) p.replace(at, t.size(), b);
    };
    sub("__LATG__", sbplus::kLatOneWayGreenMs); sub("__LATY__", sbplus::kLatOneWayYellowMs);
    sub("__LATRG__", sbplus::kLatRttGreenMs);   sub("__LATRY__", sbplus::kLatRttYellowMs);
    return p;
}


// GET /log — the connection log (and the full hub.log tail) in a readable window of its own:
// monospace, newest at the bottom, 2-s refresh, "connections only" filter, last-N selector. Embedded in the page's
// Log tab with ?embed=1. Password-gated like Settings (/api/log answers 403 without web_password).
static const char* kLogPage = R"HTML(<!doctype html><html><head><meta charset="utf-8"><title>SonoBus RP Hub — Log</title>
<link rel="icon" type="image/png" href="icon.png">
<style>
:root{--bg:#0e0e0e;--card:#161616;--line:#262626;--text:#e6e6e6;--dim:#9a9a9a;--teal:#3fb8af;--teal2:#1d3b3a}
body{background:var(--bg);color:var(--text);font:14px/1.4 -apple-system,"Segoe UI",Helvetica,Arial,sans-serif;margin:0;padding:14px 16px;display:flex;flex-direction:column;height:100vh;box-sizing:border-box}
h1{font-size:18px;margin:0 0 10px;color:var(--teal);font-weight:600}
.tools{display:flex;flex-wrap:wrap;gap:10px 18px;align-items:center;margin:0 0 10px;color:var(--dim)}
.tools label{display:flex;align-items:center;gap:6px;cursor:pointer}
input[type=checkbox]{width:16px;height:16px;accent-color:var(--teal)}
select,button{background:var(--teal2);color:var(--text);border:1px solid var(--teal);border-radius:6px;padding:5px 10px;font:13px inherit;cursor:pointer}
pre{flex:1 1 auto;margin:0;overflow:auto;background:var(--card);border:1px solid var(--line);border-radius:8px;padding:10px 12px;font:12.5px/1.45 ui-monospace,Menlo,Consolas,monospace;white-space:pre;color:var(--text)}
.conn{color:#e6e6e6}.refused{color:#ff5e5e}.ok{color:#5fd26b}.odd{color:#e3c141}.dim{color:#777}
.note{color:var(--dim);margin-top:8px;font-size:12px}
</style></head><body>
<h1 id="h">Hub log</h1>
<div class="tools">
 <label><input type="checkbox" id="conn" checked> Connections only</label>
 <label>Last <select id="n"><option>200</option><option selected>500</option><option>2000</option></select> lines</label>
 <label><input type="checkbox" id="auto" checked> Auto-refresh (2 s)</label>
 <button id="now">Refresh</button>
 <span id="msg"></span>
</div>
<pre id="log">loading…</pre>
<div class="note" id="note"></div>
<script>
const $=(id)=>document.getElementById(id);const api=(p)=>location.origin+p;
if(location.search.includes('embed=1')){$('h').style.display='none';}
else fetch(api('/api/status'),{cache:'no-store'}).then(r=>r.json()).then(d=>{if(d&&d.hub&&d.hub.group)document.title=d.hub.group+' · Log';}).catch(()=>{});
const esc=(s)=>String(s).replace(/[&<>"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]));
function cls(l){if(l.includes('-refused')||l.includes('bad-message'))return 'refused';if(l.includes(' join ')||l.includes(' login ')||l.includes(' peer-audio '))return 'ok';if(l.includes('udp-unknown'))return 'odd';if(l.startsWith('[HUB-CONN]'))return 'conn';return 'dim';}
let last='';
async function load(){
 try{const r=await fetch(api('/api/log?filter='+($('conn').checked?'conn':'all')+'&n='+$('n').value),{cache:'no-store'});
 if(r.status==403){$('log').textContent='set web_password in the hub settings to view the log from the web page';return;}
 if(r.status==401){$('log').textContent='password required (reload and sign in)';return;}
 const d=await r.json();const pre=$('log');const atBottom=pre.scrollHeight-pre.scrollTop-pre.clientHeight<40;
 const html=d.lines.length?d.lines.map(l=>`<span class="${cls(l)}">${esc(l)}</span>`).join('\n'):'<span class="dim">(nothing yet)</span>';
 if(html!==last){last=html;pre.innerHTML=html;if(atBottom)pre.scrollTop=pre.scrollHeight;}
 $('note').textContent=(d.file?'file: '+d.file+'  —  ':'')+($('conn').checked?'connection/security events kept by the hub (last 2000); untick to see the whole hub log tail':'last '+$('n').value+' lines of the hub log')+'  —  '+new Date().toLocaleTimeString();
 }catch(e){$('msg').textContent='log unavailable: '+e;}
}
$('conn').onchange=load;$('n').onchange=load;$('now').onclick=load;
load();setInterval(()=>{if($('auto').checked)load();},2000);
</script></body></html>
)HTML";


std::string SonobusHubWeb::statusJson() const {
    {   // Serve the same JSON to everyone for 500 ms; the audio path is never asked twice a second
        std::lock_guard<std::mutex> g(mCacheLock);
        const long long now = webNowMs();
        if (!mCachedJson.empty() && now - mCachedAtMs < 500) return mCachedJson;
    }
    std::string built = buildStatusJson();
    std::lock_guard<std::mutex> g(mCacheLock); mCachedJson = built; mCachedAtMs = webNowMs();
    return built;
}

// The same link the client's own "Copy Group Link" makes — sonobusrp://host:port/?g=GROUP&p=PASSWORD
// (JUCE URL::withParameter percent-encoding), password included when the room has one.
std::string SonobusHubWeb::joinLink(const SonobusHubServer::UiSnapshot& s) {
    return joinLinkFor(s.publicAddressHost.empty() ? s.advertisedPublicHost : s.publicAddressHost, s.aooServerPort, s.groupName, s.groupPassword);
}
// The same link for ANY group/password (Recent sessions rows)
std::string SonobusHubWeb::joinLinkFor(const std::string& host, int port, const std::string& group, const std::string& password) {
    juce::URL url(juce::String("sonobusrp://") + juce::String(host) + ":" + juce::String(port) + "/");
    url = url.withParameter("g", juce::String::fromUTF8(group.c_str()));
    if (!password.empty()) url = url.withParameter("p", juce::String::fromUTF8(password.c_str()));
    return url.toString(true).toStdString();
}

std::string SonobusHubWeb::buildStatusJson() const {
    const auto s = mHub.snapshotForUI();
    std::ostringstream o; o << std::fixed << std::setprecision(2);
    const long long nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    o << "{\"hub\":{"
      << "\"group\":\"" << jsonEscape(s.groupName) << "\","
      << "\"sampleRate\":" << s.sampleRate << ",\"blockSize\":" << s.blockSize << ",\"channels\":" << s.channels << ","
      << "\"codec\":\"" << (s.useOpus ? "Opus " + std::to_string(s.opusBitrate / 1000) + " kbps" : std::string("PCM 16-bit")) << "\","
      << "\"uptimeSec\":" << (s.hubStartedAtMs > 0 ? (nowMs - s.hubStartedAtMs) / 1000 : (webNowMs() - mStartedAtMs) / 1000) << ","
      << "\"cpuPct\":" << s.hubCpuPct << ","
      << "\"publicAddress\":\"" << jsonEscape(s.publicAddressHost.empty() ? s.advertisedPublicHost : s.publicAddressHost) << "\","
      << "\"serverPort\":" << s.aooServerPort << ",\"udpPort\":" << s.localUdpPort << ","
      << "\"bytesInTotal\":" << s.bytesInTotal << ",\"bytesOutTotal\":" << s.bytesOutTotal << ","
      << "\"portCheck\":\"" << jsonEscape(s.portCheckText) << "\",\"portCheckState\":" << s.portCheckState << ",\"portLocal\":\"" << jsonEscape(s.portLocalText) << "\","
      << "\"version\":\"" << jsonEscape(std::string(SONOBUS_HUB_VERSION)) << "\","   // Marketing version (was upstream 1.7.2)
      // The hub's OWN version (independent of upstream SonoBus 1.7.2) + build id
      << "\"hubVersion\":\"" << jsonEscape(std::string(SONOBUS_HUB_VERSION)) << "\",\"build\":\"" << jsonEscape(hubBuildId()) << "\",\"commit\":\"" << jsonEscape(hubCommitId()) << "\",\"built\":\"" << jsonEscape(std::string(__DATE__)) << "\","
      << "\"update\":\"" << jsonEscape(updateLine()) << "\",\"updateUrl\":\"" << jsonEscape(mUpdateCheck != nullptr ? mUpdateCheck->url().toStdString() : std::string()) << "\",\"updateReady\":" << (updateReady() ? "true" : "false") << ","
      << "\"geoip\":" << (geoEnabled() ? "true" : "false") << ","
      << "\"platform\":\"" << jsonEscape(webPlatform()) << "\",\"location\":\"" << jsonEscape(mLocation) << "\","
      << "\"settingsEnabled\":" << ((mArgs && !mPassword.empty() && mSettingsFile != juce::File()) ? "true" : "false") << ",\"pollMs\":1000,"
      << "\"portVerifiedAtMs\":" << s.portCheckVerifiedAtMs << ","
      << "\"playersSinceStart\":" << mHub.playersSinceStart() << ","
      << "\"joinLink\":\"" << jsonEscape(SonobusHubWeb::joinLink(s)) << "\"},\"peers\":[";
    bool first = true;
    for (const auto& p : s.peers) {
        float toHub = -1.0f, fromHub = -1.0f;
        const bool have = SonobusHubServer::computeHubLatency(p.smoothedRttUpMs, p.smoothedRttMs, p.sinkBufMs, toHub, fromHub, p.clientRecvBufMs);
        if (!first) o << ","; first = false;
        o << "{\"id\":" << p.id << ",\"user\":\"" << jsonEscape(p.userName) << "\",\"inputs\":" << p.recvChannels
          << ",\"addr\":\"" << jsonEscape(p.ipAddr) << ":" << p.port << "\",\"lan\":" << (p.isLan ? "true" : "false")
          << ",\"blocks\":" << p.lastBlocks << ",\"blocksNonZero\":" << p.lastBlocksNonZero << ",\"drops\":" << p.lostBlocks
          << ",\"bytesIn\":" << p.bytesIn << ",\"bytesOut\":" << p.bytesOut
          << ",\"toHubMs\":" << (have ? toHub : -1.0f) << ",\"fromHubMs\":" << (have ? fromHub : -1.0f)
          << ",\"rttUpMs\":" << p.smoothedRttUpMs << ",\"rttDownMs\":" << p.smoothedRttMs
          << ",\"jitBufPct\":" << p.jitterBufferFillPct << ",\"jitP50\":" << p.jitP50Ms << ",\"jitP95\":" << p.jitP95Ms
          << ",\"jitP99\":" << p.jitP99Ms << ",\"jitMax\":" << p.jitMaxMs << ",\"rate\":" << p.pktRate << ",\"gapPct\":" << p.gapPct
          << ",\"sinkBufMs\":" << p.sinkBufMs << ",\"holdAtCeiling\":" << (p.holdAtCeiling ? "true" : "false")
          << ",\"clientRecvBufMs\":" << p.clientRecvBufMs
          << ",\"rxUnderruns\":" << p.clientRxUnderruns10s << ",\"rxPops\":" << p.clientRxPops10s << ",\"xruns\":" << p.clientXruns10s
          << ",\"rxUnderrunsTotal\":" << p.clientRxUnderrunsTotal << ",\"rxPopsTotal\":" << p.clientRxPopsTotal   
          << ",\"latFeedAgeS\":" << p.clientLatFeedAgeS   // Player-reported age of the hub's latency feed
          << ",\"recvBufMode\":" << p.clientRecvBufMode   // 0 Manual / 1 Auto / -1 unknown
          << ",\"channels\":[";
        for (size_t ci = 0; ci < p.channelNames.size(); ++ci) o << (ci ? "," : "") << "\"" << jsonEscape(p.channelNames[ci]) << "\"";   
        o << "]"
          << ",\"location\":\"" << jsonEscape(locationFor(p.ipAddr, p.isLan)) << "\""   
          << ",\"recording\":" << (p.isRecording ? "true" : "false") << ",\"muted\":" << (p.isSendMuted ? "true" : "false") << ",\"devBuf\":" << p.clientDevBuf << ",\"devSr\":" << p.clientDevSr
          << ",\"joinedAtMs\":" << p.joinedAtMs << "}";
    }
    o << "]}";
    return o.str();
}

void SonobusHubWeb::handleClient(int fd) {
    std::string req; char buf[2048];
    while (req.size() < 16384) {
        const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        req.append(buf, (size_t) n);
        if (req.find("\r\n\r\n") != std::string::npos) break;
    }
    if (req.empty()) return;
    const size_t sp1 = req.find(' '), sp2 = sp1 == std::string::npos ? std::string::npos : req.find(' ', sp1 + 1);
    const std::string method = sp1 == std::string::npos ? "" : req.substr(0, sp1);
    std::string path = (sp1 == std::string::npos || sp2 == std::string::npos) ? "/" : req.substr(sp1 + 1, sp2 - sp1 - 1);
    std::string query;   // /api/log?filter=…&n=…
    const size_t q = path.find('?'); if (q != std::string::npos) { query = path.substr(q + 1); path = path.substr(0, q); }
    if (method != "GET" && method != "HEAD" && method != "POST") { sendAll(fd, httpResponse(405, "Method Not Allowed", "text/plain", "GET/POST only\n")); return; }
    // The logo is not secret, and browsers often ask for the tab icon without the password.
    if (method != "POST" && (path == "/icon.png" || path == "/favicon.ico")) {   // /favicon.ico = the same PNG
        sendAll(fd, httpResponse(200, "OK", "image/png", std::string((const char*) kHubWebIconPng, kHubWebIconPngSize), "", "public, max-age=86400"));
        return;
    }
    if (!authorised(req)) { sendAll(fd, httpResponse(401, "Unauthorized", "text/plain", "password required\n", "WWW-Authenticate: Basic realm=\"SonoBus RP Hub\"\r\n")); return; }
    if (method == "POST") {
        // Settings + port-check actions. Both need a configured password: an open status
        // page must never let the internet change the hub.
        const size_t hdrEnd = req.find("\r\n\r\n");
        size_t clen = 0;
        { std::string low; for (char c : req.substr(0, hdrEnd == std::string::npos ? req.size() : hdrEnd)) low.push_back((char) tolower((unsigned char) c));
          const size_t cp = low.find("content-length:"); if (cp != std::string::npos) clen = (size_t) atol(low.c_str() + cp + 15); }
        std::string body = hdrEnd == std::string::npos ? "" : req.substr(hdrEnd + 4);
        while (body.size() < clen && body.size() < 65536) { const ssize_t n = ::recv(fd, buf, sizeof(buf), 0); if (n <= 0) break; body.append(buf, (size_t) n); }
        if (mPassword.empty() || !mArgs || mSettingsFile == juce::File()) { sendAll(fd, httpResponse(403, "Forbidden", "application/json", "{\"ok\":false,\"error\":\"set web_password (and a settings file) to enable settings from the web page\"}")); return; }
        if (path == "/api/portcheck") { mHub.requestPortCheck(); sendAll(fd, httpResponse(200, "OK", "application/json", "{\"ok\":true}")); return; }
        if (path == "/api/settings") { bool ok = false; const std::string reply = applySettings(body, ok); sendAll(fd, httpResponse(ok ? 200 : 400, ok ? "OK" : "Bad Request", "application/json", reply)); return; }
        if (path == "/api/chat") { bool ok = false; const std::string reply = chatRequest(body, ok); sendAll(fd, httpResponse(ok ? 200 : 400, ok ? "OK" : "Bad Request", "application/json", reply)); return; }
        if (path == "/api/update/install") { bool ok = false; const std::string reply = startUpdate(ok); sendAll(fd, httpResponse(ok ? 200 : 400, ok ? "OK" : "Bad Request", "application/json", reply)); return; }
        if (path == "/api/sessions/removeplayer") { bool ok = false; const std::string reply = removePlayerRequest(body, ok); sendAll(fd, httpResponse(ok ? 200 : 400, ok ? "OK" : "Bad Request", "application/json", reply)); return; }
        if (path == "/api/sessions/remove") { bool ok = false; const std::string reply = removeSessionRequest(body, ok); sendAll(fd, httpResponse(ok ? 200 : 400, ok ? "OK" : "Bad Request", "application/json", reply)); return; }   // The X on a Recent sessions row
        if (path == "/api/sessions/apply") { bool ok = false; const std::string reply = applySessionRequest(body, ok); sendAll(fd, httpResponse(ok ? 200 : 400, ok ? "OK" : "Bad Request", "application/json", reply)); return; }   // Recent sessions — Apply & Restart
        sendAll(fd, httpResponse(404, "Not Found", "text/plain", "not found\n")); return;
    }
    if (path == "/api/status") sendAll(fd, httpResponse(200, "OK", "application/json", statusJson()));
    else if (path == "/api/log") {   // Connection log / hub.log tail; gated like Settings
        if (mPassword.empty()) sendAll(fd, httpResponse(403, "Forbidden", "application/json", "{\"ok\":false,\"error\":\"set web_password to view the log from the web page\"}"));
        else sendAll(fd, httpResponse(200, "OK", "application/json", logJson(query)));
    }
    else if (path == "/licenses") sendAll(fd, httpResponse(200, "OK", "text/plain; charset=utf-8", licensesText()));
    else if (path == "/log") sendAll(fd, httpResponse(200, "OK", "text/html; charset=utf-8", kLogPage));
    else if (path == "/api/settings") sendAll(fd, httpResponse(200, "OK", "application/json", settingsJson()));
    else if (path == "/" || path == "/index.html") sendAll(fd, httpResponse(200, "OK", "text/html; charset=utf-8", pageWithConstants()));
    else if (path == "/healthz") sendAll(fd, httpResponse(200, "OK", "text/plain", "ok\n"));
    else if (path == "/api/recent") sendAll(fd, httpResponse(200, "OK", "application/json", recentJson()));   // Recent players (not password gated)
    else if (path == "/api/sessions") sendAll(fd, httpResponse(200, "OK", "application/json", sessionsJson()));   // Recent sessions (not password gated; passwords never included)
    else sendAll(fd, httpResponse(404, "Not Found", "text/plain", "not found\n"));
}

// ─── log endpoint ───────────────────────────────────────────────────────────────────────────
// Last n lines of a text file without reading all of it (hub.log grows to many MB): read the tail in 64 KB steps
// until n newlines are seen or 2 MB has been read.
static std::vector<std::string> tailFile(const std::string& path, int n) {
    std::vector<std::string> out;
    FILE* f = fopen(path.c_str(), "rb"); if (!f) return out;
    fseek(f, 0, SEEK_END); long size = ftell(f); if (size < 0) { fclose(f); return out; }
    std::string buf; long pos = size; int newlines = 0;
    while (pos > 0 && newlines <= n && buf.size() < (2u << 20)) {
        const long step = std::min<long>(65536, pos); pos -= step;
        std::string chunk((size_t) step, '\0'); fseek(f, pos, SEEK_SET);
        if (fread(&chunk[0], 1, (size_t) step, f) != (size_t) step) break;
        newlines += (int) std::count(chunk.begin(), chunk.end(), '\n');
        buf.insert(0, chunk);
    }
    fclose(f);
    size_t start = 0;
    while (true) { const size_t nl = buf.find('\n', start); if (nl == std::string::npos) { if (start < buf.size()) out.push_back(buf.substr(start)); break; } out.push_back(buf.substr(start, nl - start)); start = nl + 1; }
    if ((int) out.size() > n) out.erase(out.begin(), out.end() - n);
    return out;
}
static std::string queryParam(const std::string& query, const std::string& key) {
    size_t p = 0;
    while (p < query.size()) {
        size_t amp = query.find('&', p); if (amp == std::string::npos) amp = query.size();
        const std::string kv = query.substr(p, amp - p); const size_t eq = kv.find('=');
        if (kv.substr(0, eq) == key) return eq == std::string::npos ? "" : kv.substr(eq + 1);
        p = amp + 1;
    }
    return "";
}
std::string SonobusHubWeb::logJson(const std::string& query) const {
    const std::string filter = queryParam(query, "filter");
    int n = atoi(queryParam(query, "n").c_str()); if (n <= 0) n = 500; if (n > 5000) n = 5000;
    std::string file;
    { std::lock_guard<std::mutex> g(mSettingsLock); if (mArgs) file = mArgs->logFile; }
    std::vector<std::string> lines;
    if (filter == "all" && !file.empty()) lines = tailFile(file, n);
    else lines = SonobusHubServer::connLogLines(n);
    std::ostringstream o;
    o << "{\"ok\":true,\"filter\":\"" << (filter == "all" ? "all" : "conn") << "\",\"file\":\"" << jsonEscape(filter == "all" ? file : std::string()) << "\",\"lines\":[";
    bool first = true; for (const auto& l : lines) { if (!first) o << ","; first = false; o << "\"" << jsonEscape(l) << "\""; }
    o << "]}";
    return o.str();
}

// ─── GET /api/recent — the hub's recent-players list (SonobusHubServer::recentPlayers), newest
// first, locations from the shared resolver. Not password gated (same class of data as /api/status).
std::string SonobusHubWeb::recentJson() const {
    const auto recent = mHub.recentPlayers(30);
    std::ostringstream o; o << "{\"players\":[";
    bool first = true;
    for (const auto& r : recent) {
        if (!first) o << ","; first = false;
        o << "{\"user\":\"" << jsonEscape(r.user) << "\",\"location\":\"" << jsonEscape(locationFor(r.ip, r.lan)) << "\",\"lan\":" << (r.lan ? "true" : "false")
          << ",\"firstSeenMs\":" << r.firstSeenMs << ",\"lastJoinMs\":" << r.lastJoinMs << ",\"lastLeftMs\":" << r.lastLeftMs << ",\"joins\":" << r.joins << "}";
    }
    o << "]}";
    return o.str();
}

// ─── Settings section ────────────────────────────────────────────────────────────
namespace hs = sb_hub_gui::hub_settings;

// ─── GET /api/sessions — the hub's recent GROUP sessions (SonobusHubServer::sessions), newest
// first. {"sessions":[{"group","firstUsedMs","lastUsedMs","hasPassword","current","playerCount","players":[...same
// shape as /api/recent...],"settings":{key:text}}]}. No password in clear: group_password is left out and every
// password-kind settings field and the port checker address are omitted.
static bool isPasswordKey(const std::string& key) {
    for (const auto& sp : hs::formSpecs()) if (sp.key && key == sp.key) return sp.kind == 3;
    return key == "group_password" || key == "web_password" || key == "port_check_helper";
}
std::string SonobusHubWeb::sessionsJson() const {
    const auto sessions = mHub.sessions();
    const auto snap = mHub.snapshotForUI();   // Host/port for the per-session links
    const std::string current = mHub.snapshotForUI().groupName;
    std::ostringstream o; o << "{\"sessions\":[";
    bool first = true;
    for (const auto& sd : sessions) {
        if (!first) o << ","; first = false;
        o << "{\"group\":\"" << jsonEscape(sd.group) << "\",\"firstUsedMs\":" << sd.firstUsedMs << ",\"lastUsedMs\":" << sd.lastUsedMs
          << ",\"hasPassword\":" << (sd.password.empty() ? "false" : "true") << ",\"password\":\"" << jsonEscape(sd.password) << "\"" << ",\"current\":" << (sd.group == current ? "true" : "false")
          << ",\"link\":\"" << jsonEscape(joinLinkFor(snap.publicAddressHost.empty() ? snap.advertisedPublicHost : snap.publicAddressHost, snap.aooServerPort, sd.group, sd.password)) << "\""
          << ",\"playerCount\":" << sd.players.size() << ",\"players\":[";
        bool f2 = true;
        for (auto it = sd.players.rbegin(); it != sd.players.rend(); ++it) {   // newest first, like /api/recent
            const auto& r = *it;
            if (!f2) o << ","; f2 = false;
            o << "{\"user\":\"" << jsonEscape(r.user) << "\",\"location\":\"" << jsonEscape(locationFor(r.ip, r.lan)) << "\",\"lan\":" << (r.lan ? "true" : "false")
              << ",\"firstSeenMs\":" << r.firstSeenMs << ",\"lastJoinMs\":" << r.lastJoinMs << ",\"lastLeftMs\":" << r.lastLeftMs << ",\"joins\":" << r.joins << "}";
        }
        o << "],\"settings\":{";
        f2 = true;
        for (const auto& kv : sd.settings) {
            if (isPasswordKey(kv.first)) continue;
            if (!f2) o << ","; f2 = false;
            o << "\"" << jsonEscape(kv.first) << "\":\"" << jsonEscape(kv.second) << "\"";
        }
        o << "}}";
    }
    o << "]}";
    return o.str();
}

// ─── POST /api/sessions/apply — body {"group":"…","password":"<override or empty>"}. Gated like
// the Settings POST (handleClient already answered 403 without web_password). Overlays the stored session's settings
// + group + password (the override when non-empty, else the password remembered for that session) onto the live
// model through hub_settings::applySession, writes hub_settings.json exactly like the Settings POST does, then asks
// for the restart (mRequestRestart) = "Apply & Restart".
std::string SonobusHubWeb::applySessionRequest(const std::string& body, bool& ok) {
    ok = false;
    std::lock_guard<std::mutex> g(mSettingsLock);
    if (!mArgs) return "{\"ok\":false,\"error\":\"settings not available\"}";
    const juce::var root = juce::JSON::parse(juce::String::fromUTF8(body.c_str(), (int) body.size()));
    auto* obj = root.getDynamicObject();
    if (obj == nullptr) return "{\"ok\":false,\"error\":\"body is not a JSON object\"}";
    const std::string group = obj->getProperty("group").toString().toStdString();
    const std::string pwOverride = obj->getProperty("password").toString().toStdString();
    if (group.empty()) return "{\"ok\":false,\"error\":\"no group given\"}";
    SonobusHubServer::SessionRecord rec;
    if (!mHub.sessionFor(group, rec)) return "{\"ok\":false,\"error\":\"no recent session for group '" + jsonEscape(group) + "'\"}";
    sb_hub_gui::HubLaunchArgs n = *mArgs;
    int applied = 0; std::string err;
    if (!hs::applySession(n, rec.group, pwOverride, rec.settings, applied, err))   // The field is authoritative: empty = NO password (it is prefilled with the session's password)
        return "{\"ok\":false,\"error\":\"" + jsonEscape(err) + "\"}";
    if (!hs::save(mSettingsFile, n, err)) return "{\"ok\":false,\"error\":\"save failed: " + jsonEscape(err) + "\"}";
    if (n.logFile != mArgs->logFile && !n.logFile.empty()) { std::cout << "[HUB-WEB] log file -> " << n.logFile << " (live)\n"; hs::redirectLogFile(n.logFile); }
    n.dumpPeer = mArgs->dumpPeer; n.dumpSecs = mArgs->dumpSecs;
    *mArgs = n;
    std::cout << "[HUB-SESSION] apply group='" << rec.group << "' (" << applied << " settings) -> restart\n";
    std::ostringstream o;
    o << "{\"ok\":true,\"group\":\"" << jsonEscape(rec.group) << "\",\"applied\":" << applied << ",\"passwordOverridden\":" << (pwOverride.empty() ? "false" : "true") << ",\"restarting\":true}";
    if (mRequestRestart) mRequestRestart();
    ok = true;
    return o.str();
}

// POST /api/chat, body {"message":"..."}, gated like the other POSTs.
std::string SonobusHubWeb::chatRequest(const std::string& body, bool& ok) {
    ok = false;
    const juce::var root = juce::JSON::parse(juce::String::fromUTF8(body.c_str(), (int) body.size()));
    auto* obj = root.getDynamicObject();
    if (obj == nullptr) return "{\"ok\":false,\"error\":\"body is not a JSON object\"}";
    const std::string text = obj->getProperty("message").toString().trim().toStdString();
    if (text.empty() || text.size() > 1000) return "{\"ok\":false,\"error\":\"message must be 1 to 1000 characters\"}";
    const int sent = mHub.sendGroupMessage(text);
    ok = true;
    return "{\"ok\":true,\"sent\":" + std::to_string(sent) + "}";
}

std::string SonobusHubWeb::licensesText() const {
    const auto exe = juce::File::getSpecialLocation(juce::File::currentExecutableFile).getParentDirectory();
    for (const auto& f : { exe.getChildFile("LICENSES-HUB.txt"), exe.getSiblingFile("Resources").getChildFile("LICENSES-HUB.txt") })
        if (f.existsAsFile()) return f.loadFileAsString().toStdString();
    return "Third-party notices were not found with this hub.\n";
}

bool SonobusHubWeb::updateReady() const {
    return mUpdateCheck != nullptr && mUpdateCheck->zip().url.isNotEmpty() && sb_hub_gui::hubCanUpdateInPlace();
}

std::string SonobusHubWeb::updateLine() const {
    { std::lock_guard<std::mutex> g(mUpdateLock); if (!mUpdateState.empty()) return mUpdateState; }
    return mUpdateCheck != nullptr ? mUpdateCheck->line().upToFirstOccurrenceOf("  (", false, false).toStdString() : std::string();
}

// POST /api/update/install, gated like the other POSTs. Runs on its own thread, then restarts the hub.
std::string SonobusHubWeb::startUpdate(bool& ok) {
    ok = false;
    if (!updateReady()) return "{\"ok\":false,\"error\":\"no update to install\"}";
    if (mUpdateRunning.exchange(true)) return "{\"ok\":false,\"error\":\"an update is already running\"}";
    if (mUpdateThread.joinable()) mUpdateThread.join();
    mUpdateThread = std::thread([this, zip = mUpdateCheck->zip()]() {
        const auto result = sb_hub_gui::hubInstallUpdate(zip, [this](const juce::String& s) {
            std::lock_guard<std::mutex> g(mUpdateLock);
            mUpdateState = s.toStdString();
            return true;
        });
        std::cout << "[HUB-UPDATE] " << (result.wasOk() ? juce::String("installed, restarting") : result.getErrorMessage()) << "\n";
        {
            std::lock_guard<std::mutex> g(mUpdateLock);
            mUpdateState = result.wasOk() ? std::string("Update installed, hub restarting") : "Update failed: " + result.getErrorMessage().toStdString();
        }
        mUpdateRunning = false;
        if (result.wasOk() && mRequestRestart) mRequestRestart();
    });
    ok = true;
    return "{\"ok\":true}";
}

// POST /api/sessions/removeplayer, body {"group":"...","user":"..."}, gated like the other POSTs. Connected players stay.
std::string SonobusHubWeb::removePlayerRequest(const std::string& body, bool& ok) {
    ok = false;
    const juce::var root = juce::JSON::parse(juce::String::fromUTF8(body.c_str(), (int) body.size()));
    auto* obj = root.getDynamicObject();
    if (obj == nullptr) return "{\"ok\":false,\"error\":\"body is not a JSON object\"}";
    const std::string group = obj->getProperty("group").toString().toStdString(), user = obj->getProperty("user").toString().toStdString();
    if (group.empty() || user.empty()) return "{\"ok\":false,\"error\":\"group and user are required\"}";
    std::string err;
    if (!mHub.removeSessionPlayer(group, user, err)) return "{\"ok\":false,\"error\":\"" + jsonEscape(err) + "\"}";
    ok = true;
    return "{\"ok\":true}";
}

// POST /api/sessions/remove — body {"group":"…"}; gated like the other POSTs. The current session stays.
std::string SonobusHubWeb::removeSessionRequest(const std::string& body, bool& ok) {
    ok = false;
    const juce::var root = juce::JSON::parse(juce::String::fromUTF8(body.c_str(), (int) body.size()));
    auto* obj = root.getDynamicObject();
    if (obj == nullptr) return "{\"ok\":false,\"error\":\"body is not a JSON object\"}";
    const std::string group = obj->getProperty("group").toString().toStdString();
    if (group.empty()) return "{\"ok\":false,\"error\":\"no group given\"}";
    std::string err;
    if (!mHub.removeSession(group, err)) return "{\"ok\":false,\"error\":\"" + jsonEscape(err) + "\"}";
    ok = true;
    return "{\"ok\":true,\"group\":\"" + jsonEscape(group) + "\"}";
}

void SonobusHubWeb::setSettings(sb_hub_gui::HubLaunchArgs* args, juce::File file, std::function<void()> requestRestart) {
    std::lock_guard<std::mutex> g(mSettingsLock);
    mArgs = args; mSettingsFile = file; mRequestRestart = std::move(requestRestart);
}

static const char* tierName(hs::Tier t) { return t == hs::Tier::Live ? "live" : t == hs::Tier::Restart ? "restart" : "relaunch"; }
static const hs::FormSpec* specFor(const std::string& key) {
    for (const auto& sp : hs::formSpecs()) if (sp.key && key == sp.key) return &sp;
    return nullptr;
}

std::string SonobusHubWeb::settingsJson() const {
    std::lock_guard<std::mutex> g(mSettingsLock);
    std::ostringstream o;
    const bool enabled = mArgs && !mPassword.empty() && mSettingsFile != juce::File();
    o << "{\"enabled\":" << (enabled ? "true" : "false")
      << ",\"reason\":\"" << jsonEscape(!mArgs ? "settings not wired on this platform" : mSettingsFile == juce::File() ? "hub started with --no-settings" : mPassword.empty() ? "set web_password (hub_settings.json or --web-password) to enable Settings on the web page" : "") << "\""
      << ",\"file\":\"" << jsonEscape(mSettingsFile == juce::File() ? std::string() : mSettingsFile.getFullPathName().toStdString())
      << "\",\"haveFile\":" << (mSettingsFile.existsAsFile() ? "true" : "false")
      << ",\"detected\":\"" << jsonEscape(mHub.snapshotForUI().advertisedPublicHost) << "\",\"fields\":[";
    sb_hub_gui::HubLaunchArgs defaults;
    hs::applyDefaults(defaults);
    bool first = true;
    for (const auto& sp : hs::formSpecs()) {
        if (!first) o << ","; first = false;
        o << "{\"col\":" << sp.col << ",\"key\":" << (sp.key ? "\"" + jsonEscape(sp.key) + "\"" : "null")
          << ",\"tip\":\"" << jsonEscape(sp.key ? hs::tipFor(sp.key) : "") << "\""
          << ",\"label\":\"" << jsonEscape(sp.label) << "\",\"kind\":" << sp.kind
          << ",\"choices\":\"" << jsonEscape(sp.choices ? sp.choices : "") << "\"";
        if (sp.key && enabled) {
            const std::string v = hs::getValueString(*mArgs, sp.key).toStdString();
            o << ",\"def\":\"" << jsonEscape(hs::getValueString(defaults, sp.key).toStdString()) << "\"";
            o << ",\"tier\":\"" << tierName(hs::tierForKey(sp.key)) << "\",\"value\":\"" << jsonEscape(sp.kind == 3 ? std::string() : v) << "\",\"set\":" << (v.empty() ? "false" : "true");
        }
        o << "}";
    }
    o << "]}";
    return o.str();
}

// Body: {"values":{key:"text",...}, "restart":true|false}. Password-kind fields arrive as "" = unchanged.
std::string SonobusHubWeb::applySettings(const std::string& body, bool& ok) {
    ok = false;
    std::lock_guard<std::mutex> g(mSettingsLock);
    if (!mArgs) return "{\"ok\":false,\"error\":\"settings not available\"}";
    const juce::var root = juce::JSON::parse(juce::String::fromUTF8(body.c_str(), (int) body.size()));
    auto* obj = root.getDynamicObject();
    if (obj == nullptr) return "{\"ok\":false,\"error\":\"body is not a JSON object\"}";
    const juce::var values = obj->getProperty("values");
    const bool wantRestart = (bool) obj->getProperty("restart");
    sb_hub_gui::HubLaunchArgs n = *mArgs;
    if (auto* vo = values.getDynamicObject()) {
        for (const auto& prop : vo->getProperties()) {
            const std::string key = prop.name.toString().toStdString();
            const auto* sp = specFor(key);
            if (!sp) return "{\"ok\":false,\"error\":\"unknown key: " + jsonEscape(key) + "\"}";
            const juce::String text = prop.value.toString();
            if (sp->kind == 3 && text.isEmpty()) continue;   // password left blank = keep the current one
            if (!hs::setValueFromString(n, key, text))
                return "{\"ok\":false,\"error\":\"bad value for " + jsonEscape(sp->label) + ": '" + jsonEscape(text.toStdString()) + "'\"}";
        }
    }
    if (const std::string problem = hs::validate(n); !problem.empty()) return "{\"ok\":false,\"error\":\"" + jsonEscape(problem) + "\"}";
    const auto changed = hs::diffKeys(*mArgs, n);
    std::string err;
    if (!hs::save(mSettingsFile, n, err)) return "{\"ok\":false,\"error\":\"save failed: " + jsonEscape(err) + "\"}";
    int needRestart = 0;
    for (const auto& k : changed) {
        if (hs::tierForKey(k) == hs::Tier::Live) {
            if (k == "log_file" && !n.logFile.empty()) { std::cout << "[HUB-WEB] log file -> " << n.logFile << " (live)\n"; hs::redirectLogFile(n.logFile); }
        } else ++needRestart;
    }
    n.dumpPeer = mArgs->dumpPeer; n.dumpSecs = mArgs->dumpSecs;
    *mArgs = n;
    std::ostringstream o;
    o << "{\"ok\":true,\"changed\":[";
    for (size_t i = 0; i < changed.size(); ++i) o << (i ? "," : "") << "\"" << jsonEscape(changed[i]) << "\"";
    o << "],\"needRestart\":" << needRestart << ",\"restarting\":" << (wantRestart ? "true" : "false") << "}";
    std::cout << "[HUB-WEB] settings saved to " << mSettingsFile.getFullPathName() << " (" << changed.size() << " changed" << (wantRestart ? ", restart requested" : "") << ")\n";
    if (wantRestart && mRequestRestart) mRequestRestart();
    ok = true;
    return o.str();
}

// ─── player locations (lookup lives in SonobusHubGeo.cpp, shared with the Mac GUI) ──
bool SonobusHubWeb::geoEnabled() const {
    std::lock_guard<std::mutex> g(mSettingsLock);
    return mArgs ? mArgs->geoip : true;
}

std::string SonobusHubWeb::locationFor(const std::string& ip, bool lan) const {
    return (mSharedGeo ? *mSharedGeo : mGeo).locationFor(ip, lan, geoEnabled());   // One cache with the Mac GUI
}
