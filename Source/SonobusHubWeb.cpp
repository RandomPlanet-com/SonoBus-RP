// SPDX-License-Identifier: GPLv3-or-later WITH Appstore-exception
// Copyright (C) 2026 Jason Buecker (Random Planet)
// See SonobusHubWeb.h
#include "SbLatencyColours.h"
#include "SonobusHubWeb.h"
#include "SonobusHubServer.h"
#include "SonobusHubSettings.h"
#include "SonobusHubSupport.h"
#include "SonobusHubWebIcon.h"   // Embedded hub icon for GET /icon.png

#include <algorithm>
#include <chrono>
#include <cstring>
#include <sstream>
#include <iomanip>
#include <iostream>
#include <vector>
#include "SonobusHubCompat.h"
#if !defined(_WIN32)
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
#endif

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
    // No password, no page: it would show the players and their locations to anyone who reaches the port.
    if (mPassword.empty()) {
        std::cout << "[HUB-WEB] web page off: no web page password is set (port " + std::to_string(mPort) + " stays closed). Set a Web password in Settings to turn it on.\n";
        return false;
    }
    mListenFd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (mListenFd < 0) { std::cerr << "[HUB-WEB] socket: " << strerror(hub_sockerr()) << "\n"; return false; }
    int one = 1; ::setsockopt(mListenFd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in sa; memset(&sa, 0, sizeof(sa)); sa.sin_family = AF_INET; sa.sin_addr.s_addr = htonl(INADDR_ANY); sa.sin_port = htons((uint16_t) mPort);
    if (::bind(mListenFd, (struct sockaddr*) &sa, sizeof(sa)) < 0 || ::listen(mListenFd, 16) < 0) {
        std::cerr << "[HUB-WEB] bind/listen on port " << mPort << ": " << strerror(hub_sockerr()) << "\n";
        hub_close(mListenFd); mListenFd = -1; return false;
    }
    mRun = true;
    mThread = std::thread([this] { threadMain(); });
    if (!mSharedGeo) mGeo.start([this] { std::lock_guard<std::mutex> c(mCacheLock); mCachedAtMs = 0; });   // Next poll rebuilds with the new location; the GUI's shared resolver is already running
    std::cout << "[HUB-WEB] status page on http://0.0.0.0:" << mPort << "/ (" << (mPassword.empty() ? "no password" : "password set") << ")\n";
    return true;
}

void SonobusHubWeb::stop() {
    if (!mRun.exchange(false)) return;
    if (mListenFd >= 0) { ::shutdown(mListenFd, SHUT_RDWR); hub_close(mListenFd); mListenFd = -1; }
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
        hub_close(fd);
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

static void sendAll(int fd, const char* data, size_t size) {
    size_t off = 0;
    while (off < size) {
        const ssize_t n = ::send(fd, data + off, size - off,
#ifdef MSG_NOSIGNAL
                                 MSG_NOSIGNAL
#else
                                 0
#endif
                                 );
        if (n <= 0) return; off += (size_t) n;
    }
}
static void sendAll(int fd, const std::string& s) { sendAll(fd, s.data(), s.size()); }
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
.kv b.slot{display:inline-block;font-variant-numeric:tabular-nums}#k_up{min-width:12.5ch}#k_n{min-width:3ch}#k_audio{min-width:20ch}#k_codec{min-width:12ch}#k_cpu{min-width:6.5ch}#k_srv{min-width:24ch}#k_udp{min-width:5.5ch}#k_ver{min-width:10ch}
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
th.num,td.num{text-align:left}
.u{color:var(--dim);font-size:11px;margin-left:2px}.sl{margin:0 4px}.sw{white-space:nowrap}.sa{color:#fff;margin-left:4px;font-size:10px}
#ptable{width:1791px}#rtable{width:auto}#optable{width:auto}#ptable th{cursor:pointer;user-select:none}#ptable th.nosort{cursor:default}.nc{text-transform:none}th.ctr,td.ctr{text-align:left}.tip{display:none;position:absolute;left:100%;top:50%;transform:translateY(-50%);margin-left:-12px;z-index:6;background:#262626;border:1px solid #4a4a4a;border-radius:5px;padding:3px 8px;font-size:12px;color:#cfcfcf;white-space:nowrap}td.user:hover .tip{display:block}td.ifc{position:relative;overflow:visible}td.ifc:hover::after{content:attr(data-tip);position:absolute;left:100%;top:50%;transform:translateY(-50%);margin-left:-12px;z-index:6;background:#262626;border:1px solid #4a4a4a;border-radius:5px;padding:3px 8px;font-size:12px;color:#cfcfcf;white-space:nowrap}
td.user{white-space:normal;word-break:break-word;position:relative;overflow:visible}td.ins{white-space:normal;word-break:break-word}
tr:last-child td{border-bottom:0}
.g{color:#5fd26b}.y{color:#e3c141}.r{color:#ff5e5e}.d{color:#777}.rec{color:#ff5e5e;font-weight:700}
.wrap{overflow:auto}
.foot{display:flex;flex-wrap:wrap;gap:6px 22px;margin-top:10px;color:var(--dim)}
.form{display:grid;grid-template-columns:1fr 1fr;gap:4px 32px}
@media (max-width:900px){.form{grid-template-columns:1fr}}
.col{min-width:0}
.sec{color:var(--teal);font-weight:600;margin:14px 0 6px;font-size:14px}.sec:first-child{margin-top:0}
.hint{color:var(--dim);font-size:12px;margin:-2px 0 6px}
.pwdots{letter-spacing:2px}.pwact{display:flex;flex-wrap:wrap;gap:8px 10px;align-items:center;justify-content:flex-end;padding:4px 0 8px}.pwact .msg{margin:0 auto 0 0}.pwact button{padding:4px 12px}
.row{display:grid;grid-template-columns:minmax(220px,1fr) minmax(160px,220px) 64px;gap:10px;align-items:center;padding:4px 0}
.row label{color:var(--text)}
.row input[type=text],.row input[type=password],.row select{width:100%;box-sizing:border-box;background:#1f1f1f;color:var(--text);border:1px solid #333;border-radius:5px;padding:6px 8px;font:13px inherit}
.row input:focus,.row select:focus,.row textarea:focus{outline:none;border-color:var(--teal)}
.row.ml{grid-template-columns:1fr 64px}.row.ml label{grid-column:auto}.row.ml .hint{grid-column:1/-1;margin:0}
.row textarea{grid-column:1/-1;width:100%;box-sizing:border-box;min-height:96px;max-height:70vh;overflow-y:auto;resize:vertical;background:#1f1f1f;color:var(--text);border:1px solid #333;border-radius:5px;padding:6px 8px;font-family:inherit;font-size:13px;line-height:1.45;white-space:pre-wrap;overflow-wrap:anywhere}
.row input[type=checkbox]{width:18px;height:18px;accent-color:var(--teal)}
.tag{font-size:11px;color:var(--dim);text-transform:uppercase;letter-spacing:.04em}
.tag.live{color:#5fd26b}.tag.restart{color:#e3c141}.tag.relaunch{color:#f0a04b}
.mask{-webkit-text-security:disc}.eye{padding:2px 8px;font-size:12px;margin-left:-8px}
.pill{display:inline-block;background:var(--teal2);color:var(--teal);border:1px solid var(--teal);border-radius:10px;padding:0 8px;font-size:12px;font-weight:600;margin-right:6px}
.msg{margin-left:6px;color:var(--dim)}.msg.ok{color:#5fd26b}.msg.err{color:#ff5e5e}
.notice{color:var(--dim);padding:6px 0}
iframe.log{width:100%;height:70vh;border:0;background:#0e0e0e;border-radius:6px}

button.kick{padding:1px 6px;font-size:12px;background:#3a2020;border-color:#5a2a2a}button.kick:hover{background:#6a2a2a}
button.slink{padding:2px 8px;font-size:12px;margin-right:6px}button.srm{padding:1px 7px;font-size:12px;background:#3a2020;border-color:#5a2a2a}button.srm:hover{background:#6a2a2a}#stable tbody tr[data-g]{cursor:pointer}#stable tbody tr[data-g]:hover td{background:#1b1b1b}#stable tbody tr.sel td{background:var(--teal2)}
td.pl{white-space:normal;word-break:break-word;color:var(--dim)}
#pvtable{width:auto}td.rmst{color:#e3c141}.aka{display:block;font-size:11.5px;color:var(--dim)}button.pvx:disabled{opacity:.35;cursor:not-allowed}td.ver{white-space:normal;line-height:1.2}td.ver .vh{display:block;font-size:11.5px;color:var(--dim)}
.ovl{position:fixed;top:0;left:0;right:0;bottom:0;background:rgba(0,0,0,.6);display:flex;align-items:center;justify-content:center;z-index:20}
.dlg{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:16px 18px;min-width:300px;max-width:90vw}
.dlg label{display:block;padding:4px 0;cursor:pointer}.dlg input[type=radio]{accent-color:var(--teal);margin-right:8px}.dlg .hint{margin:8px 0 0}
.updbar{position:sticky;top:0;z-index:15;display:flex;flex-wrap:wrap;align-items:center;gap:10px 28px;margin:-18px -20px 16px;padding:14px 24px;background:#e3c141;color:#17130a;border-bottom:2px solid #8a7420}
.mainbar{margin:0 0 12px}.mainbar button{padding:8px 18px;font-size:14px;font-weight:600}
.pcbar{margin:-18px -20px 16px;padding:10px 24px;background:#b3261e;color:#fff;font-size:16px;font-weight:700;text-align:center}
.updbar .ut{flex:1 1 420px;min-width:0}.updbar .u1{font-size:21px;font-weight:700;line-height:1.25}.updbar .u1 small{font-size:13px;font-weight:500;margin-left:10px;opacity:.8}
.updbar .u2{font-size:14px;margin-top:3px}.updbar a{color:#17130a;font-weight:700;text-decoration:underline;margin-left:10px}
.updbar button{background:#17130a;color:#f3d657;border:1px solid #17130a;border-radius:7px;padding:11px 26px;font-size:16px;font-weight:700;cursor:pointer}.updbar button:hover{background:#2c2510}.updbar button:disabled{opacity:.6;cursor:default}
#grp{width:230px}#gmsg{width:320px}.tools label{color:var(--dim)}.tools input[type=password],.tools input[type=text]{background:#1f1f1f;color:var(--text);border:1px solid #333;border-radius:5px;padding:6px 8px;font:13px inherit;width:230px}
.tools input[type=text]:focus,.tools input[type=password]:focus{outline:none;border-color:var(--teal)}
)HTML" R"HTML(html{-webkit-text-size-adjust:100%}
@media (max-width:700px){
body{padding:12px 10px 32px}.card{padding:12px}.updbar,.pcbar{margin:-12px -10px 12px;padding:12px 14px}.updbar button{width:100%}
.kv{white-space:normal}.kv b.slot{min-width:0!important}#pcwrap{align-items:flex-start!important}
.tools>span[style*="flex:1"]{display:none}#grp,#gmsg,.tools input[type=text],.tools input[type=password]{width:100%;flex:1 1 100%}#grp{flex:1 1 0%;width:auto;min-width:60%}#grplock{margin-left:-4px!important}
.tabs{overflow-x:auto}.tab{padding:8px 12px;white-space:nowrap}
.row{grid-template-columns:1fr auto;gap:4px 10px;padding:6px 0}.row label{grid-column:1/-1}
.pwact{justify-content:flex-start}
#ptable th:first-child,#ptable td:first-child{position:sticky;left:0;z-index:3;background:var(--card);box-shadow:1px 0 0 var(--line)}
#ptable th:first-child{z-index:4}
#osnew .tools input[type=text],#osnew .tools input[type=password]{width:100%!important;flex:1 1 100%}
.cards,.cards tbody{display:block;width:100%!important}.cards thead,.cards colgroup{display:none}
.cards tr{display:flex;flex-wrap:wrap;align-items:baseline;column-gap:14px;row-gap:4px;padding:10px 2px;border-bottom:1px solid var(--line)}
.cards tr:last-child{border-bottom:0}.cards td{display:block;padding:0;border:0;white-space:normal;overflow:visible;width:auto;background:none!important}
.cards td:empty{display:none}.cards td.user{flex-basis:100%;font-size:15px;font-weight:600}.cards td.user .aka{font-weight:400}
.cards td.pl{flex-basis:100%}.cards button{padding:4px 12px;font-size:13px}
#pvtable td:nth-child(3):before{content:"last seen ";color:var(--dim)}#pvtable td:nth-child(3):has(.g):before{content:none}#pvtable td:nth-child(4):after{content:" joins";color:var(--dim)}
#stable td:nth-child(2):before{content:"first used ";color:var(--dim)}#stable td:nth-child(3):before{content:"last used ";color:var(--dim)}#stable td:nth-child(4):after{content:" players";color:var(--dim)}
#stable tbody tr.sel{background:var(--teal2)}
#rtable td:nth-child(3):before{content:"joined ";color:var(--dim)}#rtable td:nth-child(4):before{content:"left ";color:var(--dim)}#rtable td:nth-child(4):has(.g):before{content:none}#rtable td:nth-child(5):after{content:" joins";color:var(--dim)}
#optable td:nth-child(4):before{content:"connected ";color:var(--dim)}#ostable td.pl{font-size:12px}#ostable td:nth-child(3):before{content:"ports ";color:var(--dim)}#ostable td:nth-child(5):before{content:"players ";color:var(--dim)}#ostable td:nth-child(6){flex-basis:100%}
}
</style></head><body>
<div class="pcbar" id="pcbar" style="display:none"></div>
<div class="updbar" id="updbar" style="display:none"><div class="ut"><div class="u1"><span id="upd_t"></span><small id="upd_s"></small></div>
<div class="u2"><span id="upd_l"></span><a id="upd_n" href="#" target="_blank" rel="noopener" title="Opens the list of changes on the website"></a></div></div>
<button id="updgo"></button></div>
<div class="mainbar" id="mainbar" style="display:none"><button id="mainbtn" title="Back to the main hub's page: its stages, players and settings">&larr; Main hub page</button></div>
<div class="card">
 <div class="head"><img class="logo" src="/icon.png" alt=""><div class="titles"><h1 id="title">SonoBus RP Hub</h1><span class="sub" id="where"></span><span class="sub" id="stagenote"></span></div></div>
 <div class="kvs" id="hub"><span class="kv">uptime <b class="slot" id="k_up">–</b></span><span class="kv">players <b class="slot" id="k_n">–</b></span><span class="kv">audio <b class="slot" id="k_audio">–</b></span><span class="kv">codec <b class="slot" id="k_codec">–</b></span><span class="kv">hub CPU <b class="slot" id="k_cpu">–</b></span><span class="kv">server <b class="slot" id="k_srv">–</b></span><span class="kv">audio UDP <b class="slot" id="k_udp">–</b></span><span class="kv">version <b class="slot" id="k_ver">–</b> <span class="d" id="k_build"></span></span><span class="kv r" id="k_err"></span></div>
 <div class="tools">
  <button id="copy" title="Copy this hub's stage link for players">Copy Stage Link</button><span id="link" class="link"></span>
  <span style="flex:1 1 auto"></span>
  <span id="pcwrap" style="display:flex;flex-direction:column;align-items:flex-end;gap:2px"><span id="pc" class="sub"></span><span id="pcl" class="sub d" style="font-size:12px"></span><span id="sdir" class="sub" style="font-size:12px"></span><span id="slim" class="sub d" style="font-size:12px"></span></span><button id="pcbtn" title="Ask a connected player to probe this hub's ports from outside again">Re-run port check</button>
  <button id="restartbtn" class="warn" title="Restart the hub. Every player drops and rejoins.">Restart Hub</button>
  <button id="viewlog" title="Open the hub's connection log in a new window">View Log</button>
  <button id="dllogs" title="A zip of this hub's logs (and its crash reports) to send for support. No passwords inside.">Download Logs</button>
 </div>
</div>
<div class="tabs" style="position:relative"><a href="/licenses" target="_blank" class="d" style="position:absolute;right:4px;bottom:6px;font-size:12px" title="Third-party notices for the libraries in this hub">Notices</a><a class="tab" href="#status" data-tab="status" title="Players, stage link and recent stages.">Status</a><a class="tab" href="#settings" data-tab="settings" title="Hub settings.">Settings</a><a class="tab" href="#log" data-tab="log" title="Connection history and the hub log.">Log</a><a class="tab" href="#stages" data-tab="stages" id="stagestab" style="display:none" title="Other stages this computer hosts, each with its own settings folder and ports.">Other stages</a></div>
<div class="card pane" id="pane-status">
 <div class="tools" style="margin:0 0 12px"><label for="grp">Stage</label><input type="text" id="grp" title="Type a new stage name, then click Apply."><span id="grplock" style="display:none;margin-left:-6px" title="This stage has a password">&#128274;</span><button id="grpbtn" class="warn" title="Switch the hub to this stage. The hub restarts and players must rejoin.">Apply (restarts hub)</button><span style="flex:1 1 auto"></span><input type="text" id="gmsg" maxlength="1000" placeholder="Message to the stage" title="Sends a chat message to every player, from the Stage Admin."><button id="gmsgbtn" title="Send this message to every player.">Send</button><span id="gmsgnote" class="msg"></span></div>
 <h2 title="Everyone connected to this hub right now. The heading counts the players connected now and the different players who have joined since the hub started.">Players<span id="pcount" style="color:var(--dim);font-weight:400;font-size:13px;margin-left:12px" title="Players connected right now, and how many different players have joined since the hub started"></span></h2>
 <div id="srvtotal" class="sub" style="margin:-4px 0 8px" title="Everyone on this hub server: this stage and its other stages"></div>
 <div class="wrap"><table id="ptable"><colgroup><col style="width:190px"><col style="width:140px"><col style="width:150px"><col style="width:70px"><col style="width:82px"><col style="width:88px"><col style="width:60px"><col style="width:130px"><col style="width:68px"><col style="width:94px"><col style="width:152px"><col style="width:104px"><col style="width:58px"><col style="width:88px"><col style="width:101px"><col style="width:160px"><col style="width:56px"></colgroup><thead><tr>
 <th title="Point at a name to see the player's address">Player</th><th title="The instrument this player has set; empty when not set">Instrument</th><th title="City / region looked up from the player's public address">Location</th><th class="num">Inputs</th><th title="Signal level of this player's inputs as the hub receives them">Signal</th><th class="ctr" title="What this player sends into the hub / what the hub sends to this player, in Mb/s (bits)">Up / Down <span class="nc">Mb/s</span></th><th class="num" title="Blocks the hub lost from this player's uplink (late past the hold or never arrived) — hub-side, unlike Dropouts">Lost (hub)</th><th class="ctr" title="This player's audio reaching the hub / the hub's audio reaching this player (ms): network one-way + the buffer at the receiving end">To / From hub</th><th class="num" title="How full the hub's jitter buffer for this player's stream is">JitBuf</th><th class="num" title="Arrival jitter of this player's packets at the hub (p99 of the last window) — the number the hold follows">Net jitter</th><th class="ctr" title="The hub's hold on this player's incoming stream / the player's own receive buffer as it reports it (ms); A = Auto, M = Manual">Hub / Player buf</th><th class="ctr" title="Underruns + pops the player itself heard since it joined (player-side); (+n) = in the last 10 s; xN = CoreAudio overloads in the last 10 s">Dropouts</th><th class="num" title="Age of the last hub latency report the player received">Lat feed</th><th title="The player's audio device buffer (samples) @ sample rate">Device</th><th title="Time since this player joined">Connected</th><th title="The player's version, system and plugin format; a plugin's host on the second line (hover for the build). Older players do not report it">Version</th><th class="nosort"></th></tr></thead><tbody id="rows"><tr id="norows"><td colspan="17" class="d">no players</td></tr></tbody></table></div>
)HTML" R"HTML( <div class="foot"><span class="kv">IN <b id="f_in">–</b></span><span class="kv">OUT <b id="f_out">–</b></span></div>
 <div id="osall" style="display:none"><h2 style="margin-top:20px" title="Who is on the other stages of this hub server, as each stage reports it (a few seconds old)">Players on other stages<span id="opcount" style="color:var(--dim);font-weight:400;font-size:13px;margin-left:12px"></span></h2>
 <div class="wrap"><table id="optable" class="cards"><colgroup><col style="width:220px"><col style="width:230px"><col style="width:160px"><col style="width:110px"><col style="width:60px"></colgroup><thead><tr><th>Player</th><th>Stage</th><th>Instrument</th><th>Connected</th><th>Net</th></tr></thead><tbody id="oprows"></tbody></table></div></div>
 <h2 style="margin-top:20px" title="Everyone who joined this hub, newest first (the last 1000). Removed players stay at the top until you allow them back. Remove keeps a player out, like Kick; the X only takes a name off this list.">Previous players<span id="pvcount" style="color:var(--dim);font-weight:400;font-size:13px;margin-left:12px"></span><button id="pvclear" class="srm" style="display:none;font-weight:400;margin-left:16px;vertical-align:2px" title="Take everyone who is not connected off this list. Removed players stay removed.">Clear list</button></h2>
 <div class="wrap"><table id="pvtable" class="cards"><colgroup><col style="width:220px"><col style="width:190px"><col style="width:190px"><col style="width:60px"><col style="width:52px"><col style="width:236px"><col style="width:104px"><col style="width:40px"></colgroup><thead><tr><th title="The name the player joined with">Player</th><th title="City / region looked up from the player's last address">Location</th><th title="When this player was last on the stage">Last seen</th><th class="num" title="How many times this player has joined">Joins</th><th title="Whether this player is on the local network or the internet">Net</th><th title="For a player you removed: how long the removal lasts. Point at it to see the address.">Status</th><th></th><th></th></tr></thead><tbody id="prevrows"><tr><td colspan="8" class="d">loading…</td></tr></tbody></table></div>

 <h2 style="margin-top:20px" title="Stages this hub has served (newest first). Click one to see its players and to switch the hub back to it with the settings it ran with.">Recent stages</h2>
 <div class="wrap"><table id="stable" class="cards"><colgroup><col style="width:220px"><col style="width:190px"><col style="width:190px"><col style="width:70px"><col><col style="width:44px"></colgroup><thead><tr><th>Stage</th><th>First used</th><th>Last used</th><th class="num">Players</th><th title="Stage link (this hub's address + that stage name and password)">Stage link</th><th></th></tr></thead><tbody id="sessions"><tr><td colspan="6" class="d">loading…</td></tr></tbody></table></div>
 <div id="sdetail" style="display:none">
  <h2 style="margin-top:18px" id="sdtitle">Stage</h2>
  <div class="wrap"><table id="rtable" class="cards"><colgroup><col style="width:200px"><col style="width:170px"><col style="width:176px"><col style="width:176px"><col style="width:52px"><col style="width:52px"><col style="width:36px"></colgroup><thead><tr><th>Player</th><th>Location</th><th>Last joined</th><th>Last left</th><th class="num">Joins</th><th>Net</th><th></th></tr></thead><tbody id="recent"><tr><td colspan="6" class="d">–</td></tr></tbody></table></div>
  <div class="tools">
   <label for="spw">Password</label><input type="password" id="spw" placeholder="(no password)" autocomplete="new-password" title="The password the hub will use for this stage — edit it, or clear it for no password">
   <button id="sapply" class="warn" title="Write this stage's name, password and hub settings to hub_settings.json and restart the hub — players must rejoin">Apply and restart hub</button>
   <span id="sdmsg" class="msg"></span>
  </div>
 </div>
</div>
<div class="card pane" id="pane-settings">
 <h2>Settings</h2>
 <div id="settingsNote" class="notice">loading…</div>
 <div class="form" id="form" style="display:none"><div class="col" id="col0"></div><div class="col" id="col1"></div></div>
 <div class="tools" id="settingsTools" style="display:none">
  <button id="reset" title="Put every setting back to its default, keeping your stage name and password.">Reset to Defaults</button><button id="reload" title="Undo your edits and go back to the last saved settings.">Revert</button><button id="save" title="Save these settings. Some take effect after Save &amp; Restart.">Save</button><button id="saveRestart" class="warn" title="Save these settings and restart the hub. Players must rejoin.">Save &amp; Restart</button>
  <span id="smsg" class="msg"></span>
 </div>
</div>
)HTML" R"HTML(<div class="card pane" id="pane-stages">
 <h2 title="Other stages this computer hosts. New stage makes one with this hub's audio settings and the next ports; Delete moves its folder to stages-deleted; a stage added by its path can only be taken off the list.">Other stages on this computer</h2>
 <div class="hint" style="margin:0 0 10px">Each stage runs as its own instance on this hub server, with its own settings folder, ports, log and web page. Players join each stage by its own link.</div>
 <div id="osfwd" style="margin:0 0 10px;font-weight:600" title="Open these ports for players outside this network: the main stage and every stage in this list."></div>
 <div id="oswarn" class="y" style="display:none;margin:0 0 10px"></div>
 <div class="wrap"><table id="ostable" class="cards"><colgroup><col style="width:230px"><col><col style="width:200px"><col style="width:120px"><col style="width:190px"><col style="width:430px"></colgroup><thead><tr><th title="The stage name in that stage's settings">Stage</th><th title="That stage's folder: its settings, log and lists">Folder</th><th title="Connection port (TCP and UDP) / audio port (UDP) / web page port">Ports</th><th>State</th><th title="Players on that stage and what it receives and sends now">Players, in / out</th><th></th></tr></thead><tbody id="osrows"><tr><td colspan="6" class="d">loading…</td></tr></tbody></table></div>
 <div id="osact" class="msg" style="margin:8px 0 0;min-height:18px"></div>
 <div id="osnew" style="display:none;margin-top:14px">
  <h2 style="font-size:15px">New stage</h2>
  <div class="tools" style="flex-wrap:wrap;gap:8px 14px"><label for="osname">Stage name</label><input type="text" id="osname" style="width:200px" maxlength="60" title="The name players join. Its folder gets this name."><label for="ospw">Stage password (players)</label><input type="text" id="ospw" style="width:140px" placeholder="none" autocomplete="off" title="Players need it to join. Leave empty for an open stage."><label title="Show this stage in the Stages Directory once its ports are open to players"><input type="checkbox" id="oslist"> Listed in the Stages Directory</label><label for="oscity">City</label><input type="text" id="oscity" style="width:150px" title="The stage's city, shown on its page and in the Stages Directory"><label for="osregion">State / Province / Region</label><input type="text" id="osregion" style="width:90px" list="osrgl" title="For the United States and Canada the two-letter code, for example GA or ON; elsewhere as you write it"><datalist id="osrgl"></datalist><label for="oswpw">Web page password (this stage's own page)</label><input type="text" class="mask" id="oswpw" style="width:140px" placeholder="same as this hub" autocomplete="off" autocapitalize="off" spellcheck="false" data-1p-ignore data-lpignore="true" title="The password of the new stage's own web page. Leave empty to use this hub's."><button type="button" class="eye" data-for="oswpw" title="Show or hide what you typed">show</button><button id="osnewbtn" title="Make the stage with this hub's audio settings and the next free ports, and start it.">Create stage</button></div>
  <div id="osmsg" class="msg" style="margin-top:8px;white-space:pre-line"></div>
 </div>
 <div id="osnote" class="notice" style="display:none"></div>
</div>
<div class="card pane" id="pane-log">
 <h2>Log</h2>
 <iframe class="log" id="logframe" title="hub log"></iframe>
</div>
<div id="kdlg" class="ovl" style="display:none"><div class="dlg">
 <h2 id="kdtitle">Remove player</h2>
 <div>Keep them out:</div>
 <label><input type="radio" name="kkeep" value="hour" checked>1 hour</label>
 <label><input type="radio" name="kkeep" value="restart">Until the hub restarts</label>
 <label><input type="radio" name="kkeep" value="permanent">Permanently</label>
 <div class="hint">You can allow them back at any time in the Previous players list.</div>
 <div class="tools"><span style="flex:1 1 auto"></span><button id="kdcancel">Cancel</button><button id="kdok" class="warn">Remove</button></div>
</div></div>
<script>
let prev=null,prevT=0,restarting=false,curTab='';
const $=(id)=>document.getElementById(id);
const api=(p)=>location.origin+p;   // absolute: a page opened as http://user:pw@host/ must not pass the credentials into fetch()
const esc=(s)=>String(s).replace(/[&<>"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]));
const kb=(b)=> b<0?'–':(b*8>=1e6?(b*8/1e6).toFixed(1)+' Mb/s':Math.round(b*8/1000)+' kb/s')
const LATC={g:__LATG__,y:__LATY__,rg:__LATRG__,ry:__LATRY__};   // shared thresholds (SbLatencyColours.h), filled at serve time
const col=(v,g,y)=> v<g?'g':v<y?'y':'r';
// Upload a full stage needs: every player gets every other player's stream, 2 channels each at about 2.4 Mbit/s.
const stageLimits=(mp,mc,out)=>{const pl=mp>0?mp+' players':'no player limit',ch=mc>0?mc+' channels each':'no channel limit';
 if(mp<=0)return `Limits: ${pl}, ${ch}. Upload now ${kb(out)}.`;
 const x=mp*(mp-1)*2.4,xs=x<999.5?Math.round(x)+' Mbit/s':(x/1000).toFixed(1)+' Gbit/s';
 return `Limits: ${pl}, ${ch}. A full stage needs about ${xs} upload (2 channels each); now ${kb(out)}.`;};
const dur=(s)=>{s=Math.max(0,Math.floor(s));const h=Math.floor(s/3600),m=Math.floor(s%3600/60);return (h?h+'h ':'')+String(m).padStart(h?2:1,'0')+'m '+String(s%60).padStart(2,'0')+'s';};

// place only when its text changes (no innerHTML rebuild each second) so nothing shifts and a text selection
// survives the 1-s refresh (copy/paste works).
const setT=(el,t)=>{if(el.textContent!==t)el.textContent=t;};
const setH=(el,h)=>{if(!el||!el.dataset)return;if(el.dataset.h!==h){el.dataset.h=h;el.innerHTML=h;}};   // guard (Safari threw on a missing cell)
const rowMap=new Map();const COLS=17,VERC=15;

// order); numeric columns sort by the value behind the cell ('–'/unknown lowest), text columns case-insensitively.
// The choice is kept in JS state (re-applied after every 1-s rebuild) and per browser in localStorage.
const NUMC=new Set([3,5,6,7,8,9,10,11,12,13,14]);
let sort={c:-1,d:1};try{const s=JSON.parse(localStorage.getItem('sbhub.sort2')||'null');if(s&&typeof s.c==='number'&&(s.d===1||s.d===-1))sort=s;}catch(e){}
const rowKeys=new Map();
const ths=[...document.querySelectorAll('#ptable thead th:not(.nosort)')];
ths.forEach((th,i)=>{th.dataset.label=th.textContent;th.onclick=()=>{if(sort.c!==i)sort={c:i,d:1};else if(sort.d===1)sort={c:i,d:-1};else sort={c:-1,d:1};try{localStorage.setItem('sbhub.sort2',JSON.stringify(sort));}catch(e){}drawSortMarks();applySort();};});
const unitCase=(h)=>h.replace(/Mb\/s/g,'<span class="nc">Mb/s</span>');   // headers are upper-cased; a unit keeps its own case
const sortHead=(label,mark)=>{if(!mark)return unitCase(esc(label));const k=label.lastIndexOf(' ');return unitCase(esc(label.slice(0,k+1)))+`<span class="sw">${unitCase(esc(label.slice(k+1)))}<span class="sa">${mark}</span></span>`;};
function drawSortMarks(){ths.forEach((th,i)=>setH(th,sortHead(th.dataset.label,sort.c===i?(sort.d===1?'▲':'▼'):'')));}
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
function hookSort(id,st,keys,draw){const ths=document.querySelectorAll('#'+id+' thead th');const base=[...ths].map(t=>t.textContent);ths.forEach((t,i)=>{if(i>=keys.length)return;t.style.cursor='pointer';t.onclick=()=>{if(st.col!==i){st.col=i;st.desc=false;}else if(!st.desc)st.desc=true;else st.col=-1;ths.forEach((h,j)=>{h.innerHTML=sortHead(base[j],j===st.col?(st.desc?'▼':'▲'):'');});draw();};});}
function drawSessions(){const tb=$('sessions');
 tb.innerHTML=sessions.length?sortRows(sessions,sSort,SKEYS).map(s=>`<tr data-g="${esc(s.group)}" class="${s.group===selGroup?'sel':''}"><td class="user">${esc(s.group)}${s.hasPassword?'&nbsp;&#128274;':''}${s.password?' <span class="d">pw: '+esc(s.password)+'</span>':''}${s.current?' <span class="g" title="the stage the hub is serving now">● current</span>':''}</td><td>${ft(s.firstUsedMs)}</td><td>${ft(s.lastUsedMs)}</td><td class="num">${s.playerCount}</td><td class="pl"><button class="slink" data-link="${esc(s.link||'')}" title="Copy this stage's link">Copy link</button> <span class="d link">${esc(s.link||'')}</span></td><td class="num">${s.current?'':'<button class="srm" title="Remove this stage from the list">✕</button>'}</td></tr>`).join(''):'<tr><td colspan="6" class="d">no stages yet</td></tr>';
 tb.querySelectorAll('tr[data-g]').forEach(tr=>{tr.onclick=()=>{selGroup=(selGroup===tr.dataset.g)?null:tr.dataset.g;$('sdmsg').textContent='';drawSessions();};});
 tb.querySelectorAll('button.slink').forEach(b=>{b.onclick=async(ev)=>{ev.stopPropagation();const ok=await copyText(b.dataset.link);b.textContent=ok?'Copied':'Copy failed';setTimeout(()=>b.textContent='Copy link',1500);};});
 tb.querySelectorAll('button.srm').forEach(b=>{b.onclick=async(ev)=>{ev.stopPropagation();const g=b.closest('tr').dataset.g;if(!confirm('Remove the stage "'+g+'" from Recent stages?'))return;
   try{const r=await fetch(api('/api/sessions/remove'),{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({group:g})});const d=await r.json();if(!d.ok){alert(d.error||'remove failed');return;}if(selGroup===g)selGroup=null;loadSessions(true);}catch(e){alert('remove failed');}};});
 drawDetail();}
function drawDetail(){const picked=sessions.find(x=>x.group===selGroup),s=picked;const d=$('sdetail');if(!s){d.style.display='none';return;}d.style.display='';
 $('spw').closest('.tools').style.display=picked?'':'none';
 setT($('sdtitle'),(picked?'Players seen on "'+s.group+'"':'Players seen on this stage')+' ('+s.playerCount+')');
 $('recent').innerHTML=s.players.length?sortRows(s.players,rSort,RKEYS).map(p=>`<tr><td class="user">${esc(p.user)}</td><td>${p.location?esc(p.location):'<span class="d">–</span>'}</td><td>${ft(p.lastJoinMs)}</td><td>${p.lastLeftMs>=p.lastJoinMs?ft(p.lastLeftMs):'<span class="g">connected</span>'}</td><td class="num">${p.joins}</td><td>${p.lan?'LAN':'WAN'}</td><td>${p.lastLeftMs>=p.lastJoinMs?`<button class="srm prm" data-u="${esc(p.user)}" title="Remove this player from this stage's list">✕</button>`:''}</td></tr>`).join(''):'<tr><td colspan="7" class="d">no players recorded on this stage</td></tr>';
 $('recent').querySelectorAll('button.prm').forEach(b=>{b.onclick=async()=>{const u=b.dataset.u;if(!confirm('Remove "'+u+'" from this stage\'s players?'))return;
   try{const r=await fetch(api('/api/sessions/removeplayer'),{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({group:s.group,user:u})});const d=await r.json();if(!d.ok){alert(d.error||'remove failed');return;}loadSessions(true);}catch(e){alert('remove failed');}};});
)HTML" R"HTML( if(pwPrefillGroup!==selGroup){$('spw').value=s.password||'';pwPrefillGroup=selGroup;}$('spw').placeholder=s.password?'':'(no password — type one to set it)';
 $('sapply').disabled=restarting;}
$('sapply').onclick=async()=>{const s=sessions.find(x=>x.group===selGroup);if(!s)return;
 if(!confirm('Switch the hub to stage "'+s.group+'" with the settings it ran with, and restart? Every player will have to rejoin.'))return;
 const m=$('sdmsg');m.textContent='applying…';m.className='msg';
 try{const r=await fetch(api('/api/sessions/apply'),{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({group:s.group,password:$('spw').value})});const d=await r.json();
 if(!d.ok){m.textContent=d.error||('HTTP '+r.status);m.className='msg err';return;}
 restarting=true;$('spw').value='';$('sapply').disabled=true;m.textContent=`applied "${d.group}" (${d.applied} settings${d.passwordOverridden?', new password':''}) — hub restarting, players must rejoin…`;m.className='msg ok';}
 catch(e){m.textContent='apply failed: '+e;m.className='msg err';}};
function showTab(t){if(!['status','settings','log','stages'].includes(t))t='status';curTab=t;
 document.querySelectorAll('.tab').forEach(a=>a.classList.toggle('on',a.dataset.tab===t));
 document.querySelectorAll('.pane').forEach(p=>p.classList.toggle('on',p.id==='pane-'+t));
 if(t==='log'&&!$('logframe').src)$('logframe').src=api('/log?embed=1');
 if(t==='settings')loadSettings();
 if(t==='stages')loadStages();
 if(location.hash!=='#'+t)history.replaceState(null,'','#'+t);}
window.addEventListener('hashchange',()=>showTab(location.hash.slice(1)));
async function tick(){
 try{const r=await fetch(api('/api/status'),{cache:'no-store'});if(r.status==401){setT($('k_err'),'password required (reload and sign in)');return;}
 const d=await r.json();const now=Date.now();const dt=prev?(now-prevT)/1000:0;const h=d.hub;
 if(restarting){restarting=false;$('smsg').textContent='hub is back up';$('smsg').className='msg ok';loadSettings();$('sdmsg').textContent='hub is back up';$('sdmsg').className='msg ok';$('sapply').disabled=false;loadSessions(true);}
 setT($('title'),'SonoBus RP Hub — '+h.group+(h.locked?'\u00A0\uD83D\uDD12':''));$('grplock').style.display=h.locked?'':'none';
 setH($('stagenote'),h.isStage?'<span class="pill">Other stage</span> This page is for the stage “'+esc(h.group)+'” only, one of the other stages on this hub server.':'');
 $('mainbar').style.display=(h.isStage&&h.mainWebPort>0)?'':'none';$('mainbar').dataset.port=h.mainWebPort||0;
 const tabTitle=(h.group?h.group+' · ':'')+(h.isStage?'other stage · ':'')+'SonoBus RP Hub';if(document.title!==tabTitle)document.title=tabTitle;
 npeers=d.peers.length;setT($('pcount'),d.peers.length+' now · '+(h.playersSinceStart||0)+' since hub start');drawOtherPlayers();
 setT($('where'),(h.platform||'')+(h.location?'  ·  '+h.location:''));
 setT($('link'),h.joinLink||'');
 const pcc=h.portCheckState==1?'g':h.portCheckState==2?'r':'d';
 const pcat=h.portVerifiedAtMs>0?' at '+new Date(h.portVerifiedAtMs).toLocaleTimeString([],{hour:'numeric',minute:'2-digit'}):'';
 setH($('pc'),`<span class="${pcc}">${esc((h.portCheck||'').replace(')',pcat+')'))}</span>`);
 setH($('pcl'),esc(h.portLocal||''));
 {const pb=$('pcbar'),on=!!h.portCheckBlocked||!!h.publicAddressUnknown;if(on)setT(pb,h.publicAddressUnknown?'Players outside your network cannot join: the hub could not find its public address. Check the internet connection, then restart the hub.':'Players outside your network cannot join: '+(h.portCheckFailed||'a required port')+' is closed.'+((h.stageListingState|0)?' The stage is not listed until the port check passes.':''));
  if((pb.style.display!=='none')!==on)pb.style.display=on?'block':'none';}
 const tin=(prev&&dt>0)?(h.bytesInTotal-prev.hub.bytesInTotal)/dt:-1,tout=(prev&&dt>0)?(h.bytesOutTotal-prev.hub.bytesOutTotal)/dt:-1;
 const sls=h.stageListingState|0,sdc=sls==1?'g':(sls==2||sls==4)?'y':sls==3?'r':'d';
 setH($('sdir'),`<span class="${sdc}">${esc(sls==0?'Stage directory: not listed (turn on List this stage in Settings)':'Stage directory: '+(h.stageListingText||''))}</span>`);
 setT($('slim'),stageLimits(h.stageMaxPlayers|0,h.stageMaxChannels|0,tout));
 setT($('k_up'),dur(h.uptimeSec));setT($('k_n'),String(d.peers.length));setT($('k_audio'),`${h.sampleRate} Hz / ${h.blockSize} samples`);setT($('k_codec'),h.codec);
 setT($('k_cpu'),h.cpuPct.toFixed(1)+'%');setT($('k_srv'),`${h.publicAddress}:${h.serverPort}`);setT($('k_udp'),String(h.udpPort));
 if(h.settingsEnabled&&osManage===null)loadStages();if((curTab==='stages'&&(osTick++%2)===0)||(curTab==='status'&&osManage&&(osTick++%3)===0))loadStages();
 drawUpdate(h);if(document.activeElement!==$('grp'))$('grp').value=h.group||'';setT($('k_ver'),'Hub '+(h.hubVersion||''));setT($('k_build'),`(build ${h.build||''}${h.commit?', '+h.commit:''})`);setT($('k_err'),'');
 if(curTab==='status'){
  const seen=new Set();
  for(const p of d.peers){seen.add(p.id);const pp=prev&&prev.peers.find(x=>x.id===p.id);
   const up=(pp&&dt>0)?(p.bytesIn-pp.bytesIn)/dt:-1, dn=(pp&&dt>0)?(p.bytesOut-pp.bytesOut)/dt:-1;
   const sig=p.muted?'<span class="sig off"></span><span class="d" title="this player muted their send">muted</span>':(p.blocksNonZero>0?'<span class="sig on"></span>sending':'<span class="sig off"></span><span class="d">silent</span>');
   const dc=p.drops==0?'d':p.drops<100?'y':'r';
   const lat=(v)=> v<0?'<span class="d">–</span>':`<span class="${col(v,LATC.g,LATC.y)}">${v.toFixed(1)}</span><span class="u">ms</span>`;
   const nj=p.jitP99<0?'–':`<span class="${p.jitP99<=1.5?'g':p.jitP99<=3?'y':'r'}">${p.jitP99.toFixed(1)}</span><span class="u">ms</span> <span class="d">${p.lan?'LAN':'WAN'}</span>`;
   const win=Math.max(p.rxUnderruns,0)+Math.max(p.rxPops,0);const tot=p.rxUnderrunsTotal+p.rxPopsTotal;
   const gl=`${(p.rxUnderruns<0)?'<span class="d">–</span>':`<span class="${tot==0?'d':''}">${tot}</span>`}${(p.rxUnderruns>=0&&win>0)?` <span class="y">(+${win})</span>`:''}${p.xruns>0?` <span class="r">x${p.xruns}</span>`:''}`;
   const jb=p.jitBufPct<0?'–':`<span class="${p.jitBufPct>=90?'g':p.jitBufPct>=50?'y':'r'}">${p.jitBufPct.toFixed(0)}%</span>`;
   const lf=(p.latFeedAgeS===undefined||p.latFeedAgeS<0)?'<span class="d">–</span>':(p.latFeedAgeS<=35?`<span class="g">${p.latFeedAgeS}s</span>`:`<span class="r">dark ${p.latFeedAgeS}s</span>`);
   const loc=p.location===''?'<span class="d">…</span>':esc(p.location);
   const dev=p.devBuf>0?`${p.devBuf} @ ${Math.round(p.devSr/1000)}k`:'–';
   const name=(p.recording?'<span class="rec">● </span>':'')+esc(p.user);
   const inputs=`${p.inputs}`;
   const hb=(p.sinkBufMs<0)?'<span class="d">–</span>':`${p.sinkBufMs.toFixed(1)}<span class="u">ms</span>${p.holdAtCeiling?' ⚠':''}`;
   const pbm=(p.recvBufMode===1)?' <span class="d">A</span>':(p.recvBufMode===0)?' <span class="d">M</span>':'';
   const pb=(p.clientRecvBufMs===undefined||p.clientRecvBufMs<0)?'<span class="d">–</span>':`${p.clientRecvBufMs.toFixed(1)}<span class="u">ms</span>${pbm}`;
   // Columns are fixed, so a value that gains a digit moves nothing outside its own cell.
   const pair=(x,y)=>`${x}<span class="d sl">/</span>${y}`;
   const mb=(v)=> v<0?'–':(v*8/1e6).toFixed(1);
   const bare=/^(Mac|Windows|Linux|other)$/.test(p.os||''),host=(!bare&&p.plug&&p.plug.includes(' in '))?p.plug.slice(p.plug.indexOf(' in ')+4):'';
   const verShort=p.ver?`${esc(p.ver.split(' (')[0])}${p.ver.includes('-dev')?' dev':''}${p.os?' · '+esc(p.os):''}${p.plug&&bare?' · '+esc(p.plug):''}${host?'<span class="vh">'+esc(host)+'</span>':''}`:'<span class="d">–</span>';
   const cells=[`${name}<span class="tip">${esc(p.addr)}</span>`,esc(p.inst||''),loc,inputs,sig,pair(mb(up),mb(dn)),`<span class="${dc}">${p.drops}</span>`,pair(lat(p.toHubMs),lat(p.fromHubMs)),jb,nj,pair(hb,pb),gl,lf,dev,dur((now-p.joinedAtMs)/1000),verShort];

   rowKeys.set(p.id,[p.user.toLowerCase(),(p.inst||'').toLowerCase(),p.location||'',p.inputs,p.muted?'muted':(p.blocksNonZero>0?'sending':'silent'),up,p.drops,p.toHubMs,p.jitBufPct,p.jitP99,p.sinkBufMs,(p.rxUnderruns<0)?-1:(p.rxUnderrunsTotal+p.rxPopsTotal),(p.latFeedAgeS===undefined?-1:p.latFeedAgeS),p.devBuf,(now-p.joinedAtMs),(p.ver||'')]);
   let tr=rowMap.get(p.id);
   if(!tr){tr=document.createElement('tr');tr.dataset.id=p.id;for(let i=0;i<COLS;i++){const td=document.createElement('td');if([3,6,8,9,12].includes(i))td.className='num';if([5,7,10,11].includes(i))td.className='ctr';if(i===7)td.className='ctr ifc';if(i===0)td.className='user';if(i===1)td.className='ins';if(i===VERC)td.className='ver';tr.appendChild(td);}$('rows').appendChild(tr);rowMap.set(p.id,tr);}
   cells.forEach((c,i)=>setH(tr.children[i],c));
   {const it=(p.ifaceMs===undefined||p.ifaceMs<0)?'Audio interface: not known':'Audio interface '+p.ifaceMs.toFixed(1)+' ms';const ic=tr.children[7];if(ic.dataset.tip!==it)ic.dataset.tip=it;}   // the tip lives on the stable cell and is set only when its text changes
   setH(tr.children[COLS-1],h.settingsEnabled?`<button class="kick" data-u="${esc(p.user)}" title="Remove this player from the stage. You choose how long they stay out.">Kick</button>`:'');   // rebuilt only when the name or the setting changes
   if(tr.children[VERC].title!==(p.ver||''))tr.children[VERC].title=p.ver||'';}
  for(const [id,tr] of rowMap){if(!seen.has(id)){tr.remove();rowMap.delete(id);rowKeys.delete(id);}}
  applySort();
  $('norows').style.display=rowMap.size?'none':'';
  if((sessN++%10)===0)loadSessions(false);
  if((sessN%5)===1||h.settingsEnabled!==pvEnabled||(h.removedCount|0)!==pvRem){pvEnabled=h.settingsEnabled;pvRem=h.removedCount|0;loadPrev();}
  setT($('f_in'),kb(tin));setT($('f_out'),kb(tout));}
 prev=d;prevT=now;}catch(e){setT($('k_err'),restarting?'hub restarting…':'hub not responding: '+e);}
}
async function copyText(t){let ok=false;if(navigator.clipboard&&window.isSecureContext){try{await navigator.clipboard.writeText(t);ok=true;}catch(e){}}if(!ok){const ta=document.createElement('textarea');ta.value=t;ta.setAttribute('readonly','');ta.style.position='fixed';ta.style.opacity='0';document.body.appendChild(ta);ta.select();ta.setSelectionRange(0,t.length);try{ok=document.execCommand('copy');}catch(e){}document.body.removeChild(ta);}return ok;}
$('copy').onclick=async()=>{const ok=await copyText($('link').textContent);$('copy').textContent=ok?'Copied':'Copy failed';setTimeout(()=>$('copy').textContent='Copy Stage Link',1500);};
$('pcbtn').onclick=async()=>{const r=await fetch(api('/api/portcheck'),{method:'POST'});if(r.status==403){$('pc').innerHTML='<span class="y">set web_password to use this</span>';}};
$('viewlog').onclick=()=>{window.open(api('/log'),'_blank');};
$('dllogs').onclick=()=>{location.assign(api('/api/logs.zip'));};
const post=(p,b)=>fetch(api(p),{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(b)});
const needPw=(r,d)=>{if(!r.ok||!d.ok)alert(d.error||('Set a Web password to use this (HTTP '+r.status+')'));};
let kickUser='',kickBtn=null,kickPath='/api/kick';
function openRemove(u,b,path){kickUser=u;kickBtn=b;kickPath=path;
 setT($('kdtitle'),path==='/api/kick'?'Remove '+u+' from the stage?':'Keep '+u+' out of the stage?');document.querySelector('input[name=kkeep][value=hour]').checked=true;$('kdlg').style.display='flex';}
$('rows').addEventListener('click',(ev)=>{const b=ev.target.closest('button.kick');if(!b)return;openRemove(b.dataset.u,b,'/api/kick');});
$('kdcancel').onclick=()=>{$('kdlg').style.display='none';};
$('kdlg').onclick=(ev)=>{if(ev.target===$('kdlg'))$('kdcancel').click();};
document.addEventListener('keydown',(ev)=>{if(ev.key==='Escape'&&$('kdlg').style.display!=='none')$('kdcancel').click();});
$('kdok').onclick=async()=>{const keep=document.querySelector('input[name=kkeep]:checked').value,u=kickUser,b=kickBtn,path=kickPath;$('kdlg').style.display='none';
 b.disabled=true;try{const r=await post(path,{user:u,keep});const d=await r.json().catch(()=>({}));
 if(!r.ok||!d.ok){alert(d.error||('Remove failed (HTTP '+r.status+')'));b.disabled=false;return;}b.textContent=path==='/api/kick'?'Kicked':'Removed';loadPrev();}
 catch(e){alert('remove failed: '+e);b.disabled=false;}};
)HTML" R"HTML(// Other stages on this computer (main hub only): New stage, Start / Stop, Delete (a stage this hub made) or Forget (one added by its path).
let osManage=null,osTick=0,osList=[],osInfo={},osPre=false;
const osRate=(b)=>b<0?'–':(b*8<1e6?Math.round(b*8/1000)+' kb/s':(b*8/1e6).toFixed(1)+' Mb/s');
// Main hub: everyone on the other stages, and the total for the whole server
function drawOtherPlayers(){const box=$('osall');const st=(osManage&&osList.length)?osList:[];
 if(!st.length){box.style.display='none';setH($('srvtotal'),'');return;}
 const rows=[],parts=[];let total=0;const lk=(x)=>x.locked?'&nbsp;&#128274;':'';
 st.forEach(x=>{const w=x.running?(x.who||[]):[];total+=w.length;parts.push(esc(x.group||'–')+lk(x)+' '+w.length);
  w.forEach(p=>rows.push(`<tr><td class="user">${esc(p.user)}</td><td>${esc(x.group||'–')}${lk(x)}</td><td>${esc(p.inst||'')}</td><td>${p.joinedMs>0?dur((Date.now()-p.joinedMs)/1000):''}</td><td>${p.lan?'LAN':'WAN'}</td></tr>`));});
 box.style.display='';setT($('opcount'),total+' now');
 setH($('oprows'),rows.length?rows.join(''):'<tr><td colspan="5" class="d">nobody on the other stages right now</td></tr>');
 setH($('srvtotal'),'On this server: <b style="color:var(--text)">'+(npeers+total)+'</b> players &middot; this stage '+npeers+' &middot; '+parts.join(' &middot; '));}
)HTML" R"HTML(async function loadStages(){try{const r=await fetch(api('/api/stages'),{cache:'no-store'});if(r.status==403){osManage=false;return;}if(!r.ok)return;const d=await r.json();osManage=!!d.manage;osList=d.stages||[];osInfo=d;drawOtherPlayers();
 $('stagestab').style.display=osManage?'':'none';if(!osManage&&curTab==='stages')showTab('status');
 $('osnew').style.display=d.control?'':'none';const n=$('osnote');n.style.display=d.control?'none':'';if(!d.control)setT(n,'Only the main hub makes and starts other stages.');
 setT($('osfwd'),d.forward||'');
 if(!osPre){osPre=true;$('oscity').value=d.city||'';$('osregion').value=d.region||'';$('osrgl').innerHTML=(d.regionCodes||'').split(',').filter(c=>c).map(c=>'<option value="'+c+'">').join('');}
 const w=$('oswarn');w.style.display=d.warning?'':'none';setT(w,d.warning||'');
 setH($('osrows'),osList.length?osList.map((x,i)=>`<tr><td class="user">${x.group?esc(x.group):'<span class="d">–</span>'}${x.locked?'&nbsp;&#128274;':''}</td><td class="pl"><span class="link" title="${esc(x.file)}">${esc(x.file.replace(/[\\/]hub_settings\.json$/,''))}</span>${x.managed?'':' <span class="d" title="This stage was added by its path. This hub starts and stops it; its folder is not this hub\'s to move.">(added by path)</span>'}${x.problem?' <span class="r">'+esc(x.problem)+'</span>':''}</td><td>${x.serverPort} / ${x.udpPort} / ${x.webPort>0?`${x.webPort}`:'<span class="d">off</span>'}</td><td>${x.stopping?'<span class="y">stopping…</span>':x.running?'<span class="g">running</span>':'<span class="d">stopped</span>'}${x.running&&!x.stopping&&x.portState>=0?`<div style="font-size:12px" class="${x.portState===1?'g':x.portState===2?'r':'d'}" title="${esc(x.portText||'')}">${x.portState===1?'ports open':x.portState===2?'ports closed':'ports: no verdict yet'}</div>`:''}</td><td>${x.running&&x.players>=0?x.players+' · '+osRate(x.inBps)+' / '+osRate(x.outBps):'<span class="d">–</span>'}</td><td>${d.control?(x.running?`<button class="osb warn" data-a="stop" data-i="${i}" title="Quit that stage cleanly. Its players drop.">Stop</button> <button class="osb" data-a="checkports" data-i="${i}" title="Test now whether players outside this network can reach this stage.">Check ports</button>`:`<button class="osb" data-a="start" data-i="${i}" title="Start that stage">Start</button>`)+' '+(x.webPort>0?`<button class="osb" data-a="open" data-i="${i}" title="Open this stage's own page here: its Status, Settings and Log.">Open</button>`:`<button class="osb" disabled title="This stage has no web page (no web page password). Set one in its hub_settings.json, or use its window.">Open</button>`)+' '+(x.managed?`<button class="osb srm" data-a="delete" data-i="${i}" title="Stop this stage, take it out of the Stages Directory and move its folder to stages-deleted.">Delete</button>`:(x.running?'':`<button class="osb srm" data-a="forget" data-i="${i}" title="Take it off this list. Its folder and files stay.">Forget</button>`))+(x.moveTo>0&&!x.stopping?` <button class="osb" data-a="moveports" data-i="${i}" title="Move this stage to the next free ports of the row, ${x.moveTo} / ${x.moveTo+1}, so one range is forwarded for all stages. Its name, players and directory identity stay.">Move ports</button>`:''):''}</td></tr>`).join(''):'<tr><td colspan="6" class="d">no other stages yet</td></tr>');osCheck();}catch(e){}}
$('osrows').addEventListener('click',async(ev)=>{const b=ev.target.closest('button.osb');if(!b)return;const x=osList[Number(b.dataset.i)],a=b.dataset.a;if(!x)return;const nm=x.group||x.file;
 if(a==='open'){location.href='http://'+location.hostname+':'+x.webPort+'/';return;}
 if(a==='stop'&&!confirm('Stop the stage "'+nm+'"? Its players drop.'))return;
 if(a==='start'&&osInfo.warningOneMore&&!confirm(osInfo.warningOneMore+'\n\nStart "'+nm+'" anyway?'))return;
 if(a==='moveports'&&!confirm('Move "'+nm+'" from ports '+x.serverPort+' / '+x.udpPort+' to '+x.moveTo+' / '+(x.moveTo+1)+'?\n\n'+(x.running?'The stage restarts: its players drop and must join again with the new stage link. ':'Its stage link changes. ')+'Forward the new ports before players join.'))return;
 if(a==='forget'&&!confirm('Take "'+nm+'" off this list? Its folder and files stay.'))return;
 if(a==='delete'&&!confirm('Delete the stage "'+nm+'"?\n\n'+(x.running?'It stops now and its players drop. ':'')+'It leaves the Stages Directory and this list. Its folder is kept in "stages-deleted" in this hub\'s data folder; moving it back to "stages" brings the stage back.'))return;
 b.disabled=true;let d={},okr=false;try{const r=await post('/api/stages/'+a,{file:x.file});d=await r.json().catch(()=>({}));okr=r.ok&&d.ok;if(!okr&&!d.error)d.error='HTTP '+r.status;}catch(e){d={error:String(e)};}
 osPend=null;
 if(!okr)osSay(({start:'Could not start ',stop:'Could not stop ',delete:'Could not delete ',moveports:'Could not move the ports of ',checkports:'Could not check the ports of ',forget:'Could not take off the list: '})[a]+nm+': '+d.error,2);
 else if(a==='forget')osSay(nm+' taken off the list',1);
 else{osPend={file:x.file,a,nm,at:Date.now(),target:x.moveTo,runs:x.portRuns};osSay(({start:'Starting ',stop:'Stopping ',delete:'Deleting ',moveports:'Moving the ports of ',checkports:'Checking the ports of '})[a]+nm+'…',0);}
 $('osrows').dataset.h='';setTimeout(loadStages,a==='forget'?0:1000);});
// The status text under the list: "Starting X…" while an action runs, then its result for 5 s; a failure stays until
// the next action; no result after 15 s = "No answer from X".
document.addEventListener('click',(ev)=>{const b=ev.target.closest('button.eye');if(!b)return;const i=$(b.dataset.for);if(!i)return;const shown=i.classList.toggle('mask')===false;b.textContent=shown?'hide':'show';});
let osPend=null,osClr=0;
function osSay(t,k){const e=$('osact');e.textContent=t;e.className='msg'+(k===2?' err':k===1?' ok':'');clearTimeout(osClr);if(k===1)osClr=setTimeout(()=>{e.textContent='';},5000);}
function osCheck(){if(!osPend)return;const p=osPend,st=osList.find(x=>x.file===p.file);let done='';
 if(p.a==='start'&&st&&st.running&&!st.stopping)done=p.nm+' started';
 else if(p.a==='stop'&&st&&!st.running)done=p.nm+' stopped';
 else if(p.a==='delete'&&!st)done=p.nm+' deleted: its folder is kept in stages-deleted';
 else if(p.a==='moveports'&&st&&st.serverPort===p.target&&!st.stopping&&(st.running||Date.now()-p.at>1500))done=p.nm+' moved to ports '+st.serverPort+' / '+st.udpPort;
 if(p.a==='checkports'&&st&&st.portRuns!==p.runs){osPend=null;if(st.portState===1)osSay(p.nm+': ports open',1);else osSay(p.nm+': '+(st.portText||'ports closed'),st.portState===2?2:1);return;}
 if(done){osPend=null;osSay(done,1);return;}
 if(Date.now()-p.at>(p.a==='checkports'?30000:15000)){osPend=null;osSay('No answer from '+p.nm,2);return;}
 setTimeout(loadStages,1000);}
$('osnewbtn').onclick=async()=>{const m=$('osmsg'),name=$('osname').value.trim(),listed=$('oslist').checked,city=$('oscity').value.trim(),region=$('osregion').value.trim();
 if(!name){m.textContent='Enter the stage name.';m.className='msg err';return;}
 if(!city){m.textContent='Enter the city.';m.className='msg err';return;}
 if(!region){m.textContent='Enter the state, province or region.';m.className='msg err';return;}
 if(osInfo.warningOneMore&&!confirm(osInfo.warningOneMore+'\n\nCreate and start "'+name+'" anyway?'))return;
 $('osnewbtn').disabled=true;
 try{const r=await post('/api/stages/new',{name,password:$('ospw').value.trim(),listed,city,region,webPassword:$('oswpw').value});const d=await r.json().catch(()=>({}));
  if(!r.ok||!d.ok){m.textContent=d.error||('HTTP '+r.status);m.className='msg err';}
  else{const st=d.stage;['osname','ospw','oswpw'].forEach(i=>$(i).value='');$('oslist').checked=false;$('oswpw').classList.add('mask');
   m.textContent='"'+st.group+'" is made'+(st.problem?' ('+st.problem+')':' and starting')+'. Ports '+st.serverPort+' / '+st.udpPort+(st.webPort>0?', web page on port '+st.webPort+'. Web page password: '+(d.webPasswordOwn?'the one you typed':"same as this hub's"):', no web page (this hub has no web page password)')+'.\n'+(d.forward||'')+(listed?'\nIt shows in the Stages Directory once its port check passes.':'');m.className='msg ok';setTimeout(()=>{if(m.className==='msg ok')m.textContent='';},30000);}
 }catch(e){m.textContent='create failed: '+e;m.className='msg err';}
 $('osnewbtn').disabled=false;$('osrows').dataset.h='';loadStages();setTimeout(loadStages,2500);};
$('osname').onkeydown=(e)=>{if(e.key==='Enter')$('osnewbtn').click();};
// Previous players, the one list: removed players first (how long, the address, Allow back; no X until allowed back),
// then everyone else with Remove (the Kick choices, for someone who is gone or still here) and X (off this list only).
let pvList=[],pvEnabled=false,pvRem=-1;
const lastSeen=(p)=>p.lastLeftMs<p.lastJoinMs?'<span class="g">connected</span>':ft(p.lastLeftMs);
const rmText=(p)=>p.kind==='permanent'?'Removed for good':p.kind==='restart'?'Removed until the hub restarts':'Removed · '+Math.max(1,Math.ceil(p.leftSec/60))+' min left';
const pvName=(p)=>`<td class="user">${p.user?esc(p.user):'<span class="d">–</span>'}${p.aka?`<span class="aka" title="The same player joined under these names too">also seen as ${esc(p.aka)}</span>`:''}</td>`;
function pvRow(p,i){const seen=p.joins>0,gone=p.lastLeftMs>=p.lastJoinMs;
 const head=`${pvName(p)}<td>${p.location?esc(p.location):'<span class="d">–</span>'}</td><td>${seen?lastSeen(p):'<span class="d">–</span>'}</td><td class="num">${seen?p.joins:'<span class="d">–</span>'}</td><td>${seen?(p.lan?'LAN':'WAN'):'<span class="d">–</span>'}</td>`;
 if(p.removed){const full=rmText(p)+(p.addr?' · address '+p.addr:'')+' · removed '+ft(p.removedAtMs)+(p.removedUser&&p.removedUser!==p.user?' as '+p.removedUser:'');
  return `<tr class="rm">${head}<td class="rmst" title="${esc(full)}">${rmText(p)}</td><td>${pvEnabled?`<button class="slink allow" data-i="${i}" title="Let this player join again now">Allow back</button>`:''}</td><td title="Allow back first">${pvEnabled?'<button class="srm pvx" disabled title="Allow back first">✕</button>':''}</td></tr>`;}
 return `<tr>${head}<td></td><td>${pvEnabled?`<button class="kick prv" data-i="${i}" title="Keep this player out of the stage. You choose how long.">Remove</button>`:''}</td><td>${pvEnabled&&gone?`<button class="srm pvx" data-i="${i}" title="Take this player off the list">✕</button>`:''}</td></tr>`;}
async function loadPrev(){try{const r=await fetch(api('/api/previous'),{cache:'no-store'});if(!r.ok)return;pvList=(await r.json()).players||[];
 setT($('pvcount'),pvList.length?String(pvList.length):'');$('pvclear').style.display=pvEnabled&&pvList.some(p=>!p.removed&&p.lastLeftMs>=p.lastJoinMs)?'':'none';
 setH($('prevrows'),pvList.length?pvList.map(pvRow).join(''):'<tr><td colspan="8" class="d">nobody has joined yet</td></tr>');}catch(e){}}
$('prevrows').addEventListener('click',async(ev)=>{const a=ev.target.closest('button.allow');if(a){const p=pvList[Number(a.dataset.i)];if(!p)return;a.disabled=true;
  try{const r=await post('/api/unkick',{user:p.removedUser,addr:p.addr||''});const d=await r.json().catch(()=>({}));if(!r.ok||!d.ok)alert(d.error||('Allow back failed (HTTP '+r.status+')'));}catch(e){alert('allow back failed: '+e);}
  $('prevrows').dataset.h='';loadPrev();return;}
 const x=ev.target.closest('button.pvx');if(x){const p=pvList[Number(x.dataset.i)];if(!p||!confirm('Take "'+p.user+'" off the previous players list?'))return;
  try{const r=await post('/api/previous/forget',{user:p.user});const d=await r.json().catch(()=>({}));if(!r.ok||!d.ok)alert(d.error||('HTTP '+r.status));}catch(e){}loadPrev();return;}
 const b=ev.target.closest('button.prv');if(!b)return;const p=pvList[Number(b.dataset.i)];if(p)openRemove(p.user,b,'/api/remove');});
$('pvclear').onclick=async()=>{if(!confirm('Clear the previous players list? Everyone who is not connected leaves it. Removed players stay removed.'))return;
 try{const r=await post('/api/previous/clear',{});const d=await r.json().catch(()=>({}));if(!r.ok||!d.ok)alert(d.error||('HTTP '+r.status));}catch(e){}loadPrev();};
$('restartbtn').onclick=async()=>{if(!confirm('Restart the hub? Every player drops and rejoins.'))return;try{const r=await post('/api/settings',{values:{},restart:true});needPw(r,await r.json().catch(()=>({})));}catch(e){}};
$('grpbtn').onclick=async()=>{const g=$('grp').value.trim();if(!g)return;if(!confirm('Switch the hub to stage "'+g+'"? The hub restarts and players must rejoin.'))return;try{const r=await post('/api/settings',{values:{group:g},restart:true});needPw(r,await r.json().catch(()=>({})));}catch(e){}};
$('gmsgbtn').onclick=async()=>{const t=$('gmsg').value.trim();if(!t)return;try{const r=await post('/api/chat',{message:t});const d=await r.json().catch(()=>({}));needPw(r,d);if(d.ok){$('gmsg').value='';$('gmsgnote').textContent='sent to '+d.sent;setTimeout(()=>$('gmsgnote').textContent='',3000);}}catch(e){}};
$('gmsg').onkeydown=(e)=>{if(e.key==='Enter')$('gmsgbtn').click();};
$('mainbtn').onclick=()=>{const p=$('mainbar').dataset.port;if(p>0)location.href='http://'+location.hostname+':'+p+'/#stages';};
// Update banner: shown on every tab while a newer hub is on the update server.
function drawUpdate(h){const bar=$('updbar');updUrl=h.updateUrl||'';updReady=!!h.updateReady;
 if(!h.updateAvailable){if(bar.style.display!=='none')bar.style.display='none';return;}
 const v=h.updateVersion||h.updateBuild||'',named=!!h.updateBuild&&v!==h.updateBuild,run='this hub runs '+(h.hubVersion||'');
 setT($('upd_t'),'Update available: SonoBus RP Hub '+v);setT($('upd_s'),named?'build '+h.updateBuild+' \u00b7 '+run:run);
 setT($('upd_l'),h.updateState||'Players on the stage will be disconnected for a few seconds.');
 const n=$('upd_n');setT(n,named?"What's new in "+v:"What's new");if(n.getAttribute('href')!==(h.updateNotesUrl||'#'))n.setAttribute('href',h.updateNotesUrl||'#');
 const b=$('updgo');setT(b,updReady?'Install and restart':'Download');b.disabled=!!h.updateRunning||(!updReady&&!updUrl);
 b.title=updReady?'Download and install the new hub, then restart it.':'Open the download page for the new hub.';
 if(bar.style.display!=='flex')bar.style.display='flex';}
$('updgo').onclick=async()=>{if(!updReady){if(updUrl)window.open(updUrl,'_blank');return;}
 if(!confirm('Install the update? The hub restarts, so every player drops and rejoins.'))return;
 try{const r=await fetch(api('/api/update/install'),{method:'POST'});const d=await r.json().catch(()=>({}));if(!r.ok||!d.ok)alert(d.error||('Set a Web password to install updates (HTTP '+r.status+')'));}catch(e){alert('update failed: '+e);}};
)HTML" R"HTML(let fields=[],detected='',updUrl='',updReady=false,npeers=0,regions={};
function rglFill(){const c=$('f_stage_country'),l=$('rgl');if(!c||!l)return;l.innerHTML=(regions[c.value.trim().toUpperCase()]||'').split(',').filter(x=>x).map(x=>'<option value="'+x+'">').join('');}
// A text of several lines is as tall as its text, up to the limit of its style; beyond that it scrolls.
function taFit(e){e.style.height='auto';e.style.height=(e.scrollHeight+2)+'px';}
function fieldHtml(f){
 const id='f_'+f.key,tip=esc(f.tip||'');let inp;
 if(f.kind==4) return `<div class="row ml"><label for="${id}" title="${tip}">${esc(f.label)}</label><span class="tag ${f.tier}" title="Takes effect as soon as you save.">${f.tier}</span><textarea id="${id}" rows="5" maxlength="2000" title="${tip}" oninput="taFit(this)">${esc(f.value)}</textarea>${f.hint?`<div class="hint">${esc(f.hint)}</div>`:''}</div>`;
 if(f.kind==1) inp=`<input type="checkbox" id="${id}" ${f.value=='1'?'checked':''} title="${tip}">`;
 else if(f.kind==2) inp=`<select id="${id}" title="${tip}">${f.choices.split(',').map(c=>`<option value="${esc(c)}" ${c==f.value?'selected':''}>${esc(c)}</option>`).join('')}</select>`;
 else if(f.kind==3&&f.key=='group_password') return pwBlock(f);
 else if(f.kind==3) inp=`<span style="display:flex;gap:8px;align-items:center"><input type="text" class="mask" id="${id}" value="" placeholder="${f.set?'Type a new password to change it':'(no password)'}" autocomplete="off" autocapitalize="off" spellcheck="false" data-1p-ignore data-lpignore="true" style="flex:1 1 auto" title="${tip} To change it: type the new one, then Save &amp; Restart."><button type="button" class="eye" style="margin-left:0" data-for="${id}" title="Show or hide what you typed">show</button></span>`;
 else if(f.key=='location_region'||f.key=='location_city'){const miss=!f.value;inp=`<input type="text" id="${id}" value="${esc(f.value)}" ${f.key=='location_region'?'list="rgl" ':''}placeholder="${miss?'needed':''}" style="${miss?'border-color:#ff5e5e':''}" title="${tip}">`+(f.key=='location_region'?'<datalist id="rgl"></datalist>':'');}
 else{const ph=f.key=='public_address'?(detected?detected+' (auto-detected)':'Not detected, enter your address'):'';inp=`<input type="text" id="${id}" value="${esc(f.value)}" placeholder="${esc(ph)}" title="${tip}"${f.key=='stage_country'?' oninput="rglFill()"':''}>`;}
 const when=f.tier=='live'?'Takes effect as soon as you save.':'Takes effect after Save & Restart.';
 return `<div class="row"><label for="${id}" title="${tip}">${esc(f.label)}</label>${inp}<span class="tag ${f.tier}" title="${esc(when)}">${f.tier=='relaunch'?'restart':f.tier}</span></div>`;
}
// The stage password is never sent to the page: dots when one is saved, its own Set and Remove actions.
const PWHINT='Applies after the hub restarts.';
const pwState=(f)=>f.set?'<span class="pwdots" title="A password is set. It is never shown here.">\u2022\u2022\u2022\u2022\u2022\u2022\u2022\u2022</span>':'<span class="d">(no password)</span>';
function pwBlock(f){const tip=esc(f.tip||'');
 return `<div class="row"><label title="${tip}">${esc(f.label)}</label><span id="pwstate">${pwState(f)}</span><span class="tag ${f.tier}" title="Takes effect after Save &amp; Restart.">restart</span></div>`
 +`<div class="row"><label for="pwnew">New password</label><input type="password" id="pwnew" value="" autocomplete="new-password" title="Type the new stage password, then click Set password."><span></span></div>`
 +`<div class="pwact"><span id="pwmsg" class="msg">${f.pending?'Saved. ':''}${PWHINT}</span><button id="pwrm" style="${f.set?'':'display:none'}" title="Removes the stage password. ${PWHINT}">Remove password</button><button id="pwset" title="Saves the new stage password. ${PWHINT}">Set password</button></div>`;}
async function pwApply(body,done){const m=$('pwmsg');m.textContent='saving…';m.className='msg';
 try{const r=await post('/api/stagepassword',body);const d=await r.json().catch(()=>({}));
 if(!r.ok||!d.ok){m.textContent=d.error||('HTTP '+r.status);m.className='msg err';return;}
 const f=fields.find(x=>x.key=='group_password');if(f){f.set=d.set;f.pending=d.pending;$('pwstate').innerHTML=pwState(f);}
 $('pwnew').value='';$('pwrm').style.display=d.set?'':'none';m.textContent=done+(d.pending?' '+PWHINT:'');m.className='msg ok';}
 catch(e){m.textContent='save failed: '+e;m.className='msg err';}}
function hookPw(){if(!$('pwset'))return;
 $('pwset').onclick=()=>{const v=$('pwnew').value.trim();if(!v){$('pwmsg').textContent='Type a new password first.';$('pwmsg').className='msg err';return;}pwApply({password:v},'Password set.');};
 $('pwnew').onkeydown=(e)=>{if(e.key==='Enter')$('pwset').click();};
 $('pwrm').onclick=()=>{if(confirm('Remove the password? Anyone with the stage name can then join.'))pwApply({remove:true},'Password removed.');};}
async function loadSettings(){
 try{const r=await fetch(api('/api/settings'),{cache:'no-store'});const d=await r.json();
 if(!d.enabled){$('settingsNote').textContent=d.reason||'settings disabled';$('form').style.display='none';$('settingsTools').style.display='none';return;}
 fields=d.fields;detected=d.detected||'';$('settingsNote').textContent='Settings file: '+d.file+(d.haveFile?'':'. Not saved yet. Click Save to keep these settings.');
 const cols=['',''];for(const f of d.fields){cols[f.col]+=f.key?fieldHtml(f):`<div class="sec">${esc(f.label)}</div>`+(f.hint?`<div class="hint">${esc(f.hint)}</div>`:'');}
 regions={US:d.regionsUS||'',CA:d.regionsCA||''};$('col0').innerHTML=cols[0];$('col1').innerHTML=cols[1];rglFill();hookPw();$('form').style.display='grid';$('settingsTools').style.display='flex';$('smsg').textContent='';
 document.querySelectorAll('.row textarea').forEach(taFit);
 }catch(e){$('settingsNote').textContent='settings unavailable: '+e;}
}
async function save(restart){
 const values={};for(const f of fields){if(!f.key)continue;const el=$('f_'+f.key);if(!el)continue;values[f.key]=f.kind==1?(el.checked?'1':'0'):el.value;}
 const pn=$('pwnew');if(pn&&pn.value.trim())values.group_password=pn.value.trim();   // a new stage password typed but not set yet goes with the rest
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
$('reset').onclick=()=>{if(!confirm('Put every setting back to its default? Your stage name and password stay. Nothing changes until you click Save.'))return;
 for(const f of fields){if(!f.key||f.key=='group'||f.kind==3)continue;const el=$('f_'+f.key);if(!el)continue;if(f.kind==1)el.checked=f.def=='1';else el.value=f.def;}
 $('smsg').textContent='Defaults restored. Click Save to keep them.';$('smsg').className='msg';};
const TIPS={Player:'Player name',Inputs:'How many channels this player sends','First used':'When this stage was first hosted','Last used':'When this stage was last hosted',Players:'How many players have joined this stage',Stage:'The stage name players join','Last joined':'When this player last joined','Last left':'When this player last left',Joins:'How many times this player has joined',Net:'Whether this player is on the local network or the internet'};
document.querySelectorAll('th:not([title])').forEach(t=>{const m=TIPS[t.textContent.trim()];if(m)t.title=m;});
const KV={k_up:'How long the hub has been running',k_n:'Players connected right now',k_audio:'Sample rate and block size',k_codec:'Audio format',k_cpu:'CPU used by the hub',k_srv:'The address and port players connect to',f_in:'Total audio coming into the hub',f_out:'Total audio going out of the hub'};
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
<meta name="viewport" content="width=device-width,initial-scale=1">
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
html{-webkit-text-size-adjust:100%}
@media (max-width:700px){body{padding:10px 8px}pre{white-space:pre-wrap;word-break:break-word;font-size:11.5px;padding:8px}}
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
 $('note').textContent=(d.file?'file: '+d.file+'  —  ':'')+($('conn').checked?'connection/security events kept by the hub (last 2000); untick Connections only to see the whole hub log tail':'last '+$('n').value+' lines of the hub log')+'  —  '+new Date().toLocaleTimeString();
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
      << "\"group\":\"" << jsonEscape(s.groupName) << "\",\"locked\":" << (s.groupPassword.empty() ? "false" : "true") << ","
      << "\"sampleRate\":" << s.sampleRate << ",\"blockSize\":" << s.blockSize << ",\"channels\":" << s.channels << ","
      << "\"codec\":\"" << (s.useOpus ? "Opus " + std::to_string(s.opusBitrate / 1000) + " kbps" : std::string("PCM 16-bit")) << "\","
      << "\"uptimeSec\":" << (s.hubStartedAtMs > 0 ? (nowMs - s.hubStartedAtMs) / 1000 : (webNowMs() - mStartedAtMs) / 1000) << ","
      << "\"cpuPct\":" << s.hubCpuPct << ","
      << "\"publicAddress\":\"" << jsonEscape(s.publicAddressHost.empty() ? s.advertisedPublicHost : s.publicAddressHost) << "\","
      << "\"serverPort\":" << s.aooServerPort << ",\"udpPort\":" << s.localUdpPort << ","
      << "\"bytesInTotal\":" << s.bytesInTotal << ",\"bytesOutTotal\":" << s.bytesOutTotal << ","
      << "\"portCheck\":\"" << jsonEscape(s.portCheckText) << "\",\"portCheckState\":" << s.portCheckState << ",\"portCheckBlocked\":" << (s.portCheckBlocked ? "true" : "false") << ",\"publicAddressUnknown\":" << (s.publicAddressUnknown ? "true" : "false") << ",\"portCheckFailed\":\"" << jsonEscape(s.portCheckFailed) << "\"" << ",\"portLocal\":\"" << jsonEscape(s.portLocalText) << "\","
      << "\"version\":\"" << jsonEscape(std::string(SONOBUS_HUB_VERSION)) << "\","   // Marketing version
      // The hub's OWN version (independent of upstream SonoBus 1.7.2) + build id
      << "\"hubVersion\":\"" << jsonEscape(std::string(SONOBUS_HUB_VERSION)) << "\",\"build\":\"" << jsonEscape(hubBuildId()) << "\",\"commit\":\"" << jsonEscape(hubCommitId()) << "\",\"built\":\"" << jsonEscape(std::string(__DATE__)) << "\","
      << "\"update\":\"" << jsonEscape(updateLine()) << "\",\"updateUrl\":\"" << jsonEscape(mUpdateCheck != nullptr ? mUpdateCheck->url().toStdString() : std::string()) << "\",\"updateReady\":" << (updateReady() ? "true" : "false") << ","
      << updateInfoJson()
      << "\"geoip\":" << (geoEnabled() ? "true" : "false") << ","
      << "\"platform\":\"" << jsonEscape(webPlatform()) << "\",\"location\":\"" << jsonEscape(mLocation) << "\","
      << "\"settingsEnabled\":" << ((mArgs && !mPassword.empty() && mSettingsFile != juce::File()) ? "true" : "false") << ",\"pollMs\":1000,"
      << "\"portVerifiedAtMs\":" << s.portCheckVerifiedAtMs << ","
      << "\"playersSinceStart\":" << mHub.playersSinceStart() << ",\"removedCount\":" << mHub.removedPlayerCount() << ","
      << "\"stageMaxPlayers\":" << s.stageMaxPlayers << ",\"stageMaxChannels\":" << s.stageMaxChannels << ","
      << "\"isStage\":" << (sb_hub_gui::hub_settings::isStage() ? "true" : "false") << ",\"mainWebPort\":" << sb_hub_gui::hub_settings::mainHubWebPort() << ","
      << "\"stageListingState\":" << s.stageListingState << ",\"stageListingText\":\"" << jsonEscape(s.stageListingText) << "\","
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
          << ",\"ifaceMs\":" << p.clientIfaceMs   // Player's audio interface in + out, ms (-1 not reported)
          << ",\"channels\":[";
        for (size_t ci = 0; ci < p.channelNames.size(); ++ci) o << (ci ? "," : "") << "\"" << jsonEscape(p.channelNames[ci]) << "\"";   
        o << "]"
          << ",\"location\":\"" << jsonEscape(locationFor(p.ipAddr, p.isLan)) << "\""   
          << ",\"recording\":" << (p.isRecording ? "true" : "false") << ",\"muted\":" << (p.isSendMuted ? "true" : "false") << ",\"devBuf\":" << p.clientDevBuf << ",\"devSr\":" << p.clientDevSr
          << ",\"ver\":\"" << jsonEscape(p.clientVersion) << "\",\"os\":\"" << jsonEscape(p.clientOs) << "\""
          << ",\"inst\":\"" << jsonEscape(p.clientInstrument) << "\",\"plug\":\"" << jsonEscape(p.clientPlugin) << "\""
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
        if (path == "/api/stagepassword") { bool ok = false; const std::string reply = stagePasswordRequest(body, ok); sendAll(fd, httpResponse(ok ? 200 : 400, ok ? "OK" : "Bad Request", "application/json", reply)); return; }
        if (path == "/api/kick") { bool ok = false; const std::string reply = kickRequest(body, ok); sendAll(fd, httpResponse(ok ? 200 : 400, ok ? "OK" : "Bad Request", "application/json", reply)); return; }
        if (path.rfind("/api/stages/", 0) == 0) { bool ok = false; const std::string reply = stagesRequest(path.substr(12), body, ok); sendAll(fd, httpResponse(ok ? 200 : 400, ok ? "OK" : "Bad Request", "application/json", reply)); return; }
        if (path == "/api/previous/forget" || path == "/api/previous/clear") {   // the X on a previous player, and Clear list
            const juce::var root = juce::JSON::parse(juce::String::fromUTF8(body.c_str(), (int) body.size()));
            std::string err, reply = "{\"ok\":true}";
            bool ok = true;
            if (path == "/api/previous/clear") reply = "{\"ok\":true,\"forgotten\":" + std::to_string(mHub.clearPreviousPlayers()) + "}";
            else if (!mHub.forgetPreviousPlayer(root.getProperty("user", juce::var()).toString().toStdString(), err)) { ok = false; reply = "{\"ok\":false,\"error\":\"" + jsonEscape(err) + "\"}"; }
            sendAll(fd, httpResponse(ok ? 200 : 400, ok ? "OK" : "Bad Request", "application/json", reply)); return;
        }
        if (path == "/api/remove") { bool ok = false; const std::string reply = removeRequest(body, ok); sendAll(fd, httpResponse(ok ? 200 : 400, ok ? "OK" : "Bad Request", "application/json", reply)); return; }
        if (path == "/api/unkick") { bool ok = false; const std::string reply = unkickRequest(body, ok); sendAll(fd, httpResponse(ok ? 200 : 400, ok ? "OK" : "Bad Request", "application/json", reply)); return; }
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
    else if (path == "/api/logs.zip") {   // logs for support; gated like /api/log
        if (mPassword.empty()) sendAll(fd, httpResponse(403, "Forbidden", "application/json", "{\"ok\":false,\"error\":\"set web_password to download the logs from the web page\"}"));
        else sendLogsZip(fd);
    }
    else if (path == "/api/removed") {   // names and addresses: gated like /api/kick
        if (mPassword.empty() || !mArgs || mSettingsFile == juce::File()) sendAll(fd, httpResponse(403, "Forbidden", "application/json", "{\"ok\":false,\"error\":\"set web_password (and a settings file) to see removed players from the web page\"}"));
        else sendAll(fd, httpResponse(200, "OK", "application/json", removedJson()));
    }
    else if (path == "/licenses") sendAll(fd, httpResponse(200, "OK", "text/plain; charset=utf-8", licensesText()));
    else if (path == "/log") sendAll(fd, httpResponse(200, "OK", "text/html; charset=utf-8", kLogPage));
    else if (path == "/api/settings") sendAll(fd, httpResponse(200, "OK", "application/json", settingsJson()));
    else if (path == "/" || path == "/index.html") sendAll(fd, httpResponse(200, "OK", "text/html; charset=utf-8", pageWithConstants()));
    else if (path == "/healthz") sendAll(fd, httpResponse(200, "OK", "text/plain", "ok\n"));
    else if (path == "/api/stages") {   // file paths: gated like /api/removed
        if (mPassword.empty() || !mArgs || mSettingsFile == juce::File()) sendAll(fd, httpResponse(403, "Forbidden", "application/json", "{\"ok\":false,\"error\":\"set web_password (and a settings file) to see other stages from the web page\"}"));
        else sendAll(fd, httpResponse(200, "OK", "application/json", stagesJson()));
    }
    else if (path == "/api/previous") sendAll(fd, httpResponse(200, "OK", "application/json", previousJson()));   // Previous players (a removal's address only with the web password set)
    else if (path == "/api/recent") sendAll(fd, httpResponse(200, "OK", "application/json", recentJson()));   // Recent players (not password gated)
    else if (path == "/api/sessions") sendAll(fd, httpResponse(200, "OK", "application/json", sessionsJson()));   // Recent sessions (not password gated; passwords never included)
    else sendAll(fd, httpResponse(404, "Not Found", "text/plain", "not found\n"));
}

// GET /api/logs.zip: the support zip (SonobusHubSupport.h), built here on the web thread and sent as a download.
void SonobusHubWeb::sendLogsZip(int fd) {
    hub_support::Info info;
    const auto s = mHub.snapshotForUI();
    info.stageName = s.groupName;
    info.serverPort = s.aooServerPort;
    info.udpPort = s.localUdpPort;
    info.webPort = mPort;
    const long long nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    info.uptimeSec = s.hubStartedAtMs > 0 ? (nowMs - s.hubStartedAtMs) / 1000 : (webNowMs() - mStartedAtMs) / 1000;
    { std::lock_guard<std::mutex> g(mSettingsLock); if (mArgs && !mArgs->logFile.empty()) info.logFile = juce::File(juce::String::fromUTF8(mArgs->logFile.c_str())); }
    juce::MemoryOutputStream zip;
    std::string err;
    if (!hub_support::writeLogsZip(info, zip, err)) {
        sendAll(fd, httpResponse(500, "Internal Server Error", "text/plain; charset=utf-8", "The logs could not be collected: " + err + "\n"));
        return;
    }
    const std::string name = hub_support::zipFileName(info.stageName).toStdString();
    std::ostringstream h;
    h << "HTTP/1.1 200 OK\r\nContent-Type: application/zip\r\nContent-Length: " << zip.getDataSize()
      << "\r\nContent-Disposition: attachment; filename=\"" << name << "\"\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n";
    // A large zip on a slow line: each send may wait longer than a page request does.
    struct timeval tv; tv.tv_sec = 30; tv.tv_usec = 0;
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    sendAll(fd, h.str());
    sendAll(fd, (const char*) zip.getData(), zip.getDataSize());
    std::cout << "[HUB-WEB] logs for support downloaded: " << name << ", " << zip.getDataSize() << " bytes\n";
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
    if (!mHub.sessionFor(group, rec)) return "{\"ok\":false,\"error\":\"no recent stage named '" + jsonEscape(group) + "'\"}";
    sb_hub_gui::HubLaunchArgs n = *mArgs;
    int applied = 0; std::string err;
    if (!hs::applySession(n, rec.group, pwOverride, rec.settings, applied, err))   // The field is authoritative: empty = NO password (it is prefilled with the session's password)
        return "{\"ok\":false,\"error\":\"" + jsonEscape(err) + "\"}";
    if (!hs::save(mSettingsFile, n, err)) return "{\"ok\":false,\"error\":\"save failed: " + jsonEscape(err) + "\"}";
    if (n.logFile != mArgs->logFile && !n.logFile.empty()) { std::cout << "[HUB-WEB] log file -> " << n.logFile << " (live)\n"; hs::redirectLogFile(n.logFile); }
    n.dumpPeer = mArgs->dumpPeer; n.dumpSecs = mArgs->dumpSecs;
    *mArgs = n;
    hs::applyStageToHub(mHub, n);
    std::cout << "[HUB-SESSION] apply group='" << rec.group << "' (" << applied << " settings) -> restart\n";
    std::ostringstream o;
    o << "{\"ok\":true,\"group\":\"" << jsonEscape(rec.group) << "\",\"applied\":" << applied << ",\"passwordOverridden\":" << (pwOverride.empty() ? "false" : "true") << ",\"restarting\":true}";
    if (mRequestRestart) mRequestRestart();
    ok = true;
    return o.str();
}

// POST /api/stagepassword, body {"password":"..."} to set or {"remove":true} to remove, gated like the other POSTs.
// Only the password changes in the settings file. The running hub keeps its password until it restarts.
std::string SonobusHubWeb::stagePasswordRequest(const std::string& body, bool& ok) {
    ok = false;
    std::lock_guard<std::mutex> g(mSettingsLock);
    if (!mArgs) return "{\"ok\":false,\"error\":\"settings not available\"}";
    const juce::var root = juce::JSON::parse(juce::String::fromUTF8(body.c_str(), (int) body.size()));
    auto* obj = root.getDynamicObject();
    if (obj == nullptr) return "{\"ok\":false,\"error\":\"body is not a JSON object\"}";
    const bool remove = (bool) obj->getProperty("remove");
    const std::string pw = remove ? std::string() : obj->getProperty("password").toString().trim().toStdString();
    if (!remove && pw.empty()) return "{\"ok\":false,\"error\":\"Type a new password first.\"}";
    std::string err;
    if (!hs::saveStagePassword(mSettingsFile, *mArgs, pw, err)) return "{\"ok\":false,\"error\":\"save failed: " + jsonEscape(err) + "\"}";
    const bool pending = mHub.snapshotForUI().groupPassword != mArgs->groupPassword;
    std::cout << "[HUB-WEB] stage password " << (pw.empty() ? "removed" : "set") << (pending ? " (applies after the hub restarts)" : "") << "\n";
    ok = true;
    return std::string("{\"ok\":true,\"set\":") + (pw.empty() ? "false" : "true") + ",\"pending\":" + (pending ? "true" : "false") + "}";
}

// POST /api/kick, body {"user":"...","keep":"hour"|"restart"|"permanent"}, gated like the other POSTs.
// Removes the player and keeps them out: 1 hour (the default), until the hub restarts, or until allowed back.
std::string SonobusHubWeb::kickRequest(const std::string& body, bool& ok) {
    ok = false;
    const juce::var root = juce::JSON::parse(juce::String::fromUTF8(body.c_str(), (int) body.size()));
    auto* obj = root.getDynamicObject();
    if (obj == nullptr) return "{\"ok\":false,\"error\":\"body is not a JSON object\"}";
    const std::string user = obj->getProperty("user").toString().toStdString();
    if (user.empty()) return "{\"ok\":false,\"error\":\"no player given\"}";
    const std::string keep = obj->hasProperty("keep") ? obj->getProperty("keep").toString().toStdString() : std::string("hour");
    constexpr int kBlockMinutes = 60;
    SonobusHubServer::RemovalKind kind = SonobusHubServer::RemovalKind::Timed;
    if (keep == "restart") kind = SonobusHubServer::RemovalKind::UntilRestart;
    else if (keep == "permanent") kind = SonobusHubServer::RemovalKind::Permanent;
    else if (keep != "hour") return "{\"ok\":false,\"error\":\"keep must be hour, restart or permanent\"}";
    if (!mHub.kickPlayer(user, kBlockMinutes, kind)) return "{\"ok\":false,\"error\":\"no such player\"}";
    std::cout << "[HUB-WEB] kick '" << user << "' (kept out: " << (keep == "hour" ? "60 min" : keep == "restart" ? "until the hub restarts" : "permanently") << ")\n";
    ok = true;
    return "{\"ok\":true}";
}

// POST /api/remove, body {"user":"...","keep":"hour"|"restart"|"permanent"}, gated like the other POSTs. A previous
// player, here or gone: the hub keeps the name and the last address it saw out, as Kick does.
std::string SonobusHubWeb::removeRequest(const std::string& body, bool& ok) {
    ok = false;
    const juce::var root = juce::JSON::parse(juce::String::fromUTF8(body.c_str(), (int) body.size()));
    auto* obj = root.getDynamicObject();
    if (obj == nullptr) return "{\"ok\":false,\"error\":\"body is not a JSON object\"}";
    const std::string user = obj->getProperty("user").toString().toStdString();
    if (user.empty()) return "{\"ok\":false,\"error\":\"no player given\"}";
    bool known = false;
    for (const auto& r : mHub.previousPlayers()) if (r.user == user) { known = true; break; }
    if (!known) return "{\"ok\":false,\"error\":\"that player is not in the previous players list\"}";
    const std::string keep = obj->hasProperty("keep") ? obj->getProperty("keep").toString().toStdString() : std::string("hour");
    constexpr int kBlockMinutes = 60;
    SonobusHubServer::RemovalKind kind = SonobusHubServer::RemovalKind::Timed;
    if (keep == "restart") kind = SonobusHubServer::RemovalKind::UntilRestart;
    else if (keep == "permanent") kind = SonobusHubServer::RemovalKind::Permanent;
    else if (keep != "hour") return "{\"ok\":false,\"error\":\"keep must be hour, restart or permanent\"}";
    const bool here = mHub.kickPlayer(user, kBlockMinutes, kind);
    std::cout << "[HUB-WEB] remove previous player '" << user << "' (kept out: " << (keep == "hour" ? "60 min" : keep == "restart" ? "until the hub restarts" : "permanently") << (here ? ", was connected" : "") << ")\n";
    ok = true;
    return std::string("{\"ok\":true,\"connected\":") + (here ? "true" : "false") + "}";
}

// GET /api/stages: {"manage":bool,"control":bool,"cores":n,"running":n,"warning":"","stages":[{"file","group",
// "serverPort","udpPort","webPort","running","stopping","managed","players","inBps","outBps","moveTo","problem"}],
// "forward":"..."}. manage = this is the main hub (a stage with its own folder lists nothing). running counts this hub
// too. players and the two rates (bytes per second) are -1 when not known. moveTo = the first port of a lower free pair
// (0 = none). forward = what to open for players, all stages in one line (router ranges, or the firewall command).
static std::string stageJson(const sb_hub_gui::hub_settings::OtherStage& st) {
    std::ostringstream o;
    o << "{\"file\":\"" << jsonEscape(st.file.getFullPathName().toStdString()) << "\",\"group\":\"" << jsonEscape(st.group) << "\",\"locked\":" << (st.locked ? "true" : "false")
      << ",\"serverPort\":" << st.serverPort << ",\"udpPort\":" << st.udpPort << ",\"webPort\":" << st.webPort
      << ",\"running\":" << (st.running ? "true" : "false") << ",\"stopping\":" << (st.stopping ? "true" : "false")
      << ",\"managed\":" << (st.managed ? "true" : "false") << ",\"players\":" << st.players
      << ",\"inBps\":" << (long long) st.inBytesPerSec << ",\"outBps\":" << (long long) st.outBytesPerSec
      << ",\"moveTo\":" << st.moveServerPort
      << ",\"portState\":" << st.portState << ",\"portRuns\":" << st.portRuns << ",\"portText\":\"" << jsonEscape(st.portText) << "\""
      << ",\"problem\":\"" << jsonEscape(st.problem) << "\",\"who\":[";
    for (size_t k = 0; k < st.who.size(); ++k)
        o << (k ? "," : "") << "{\"user\":\"" << jsonEscape(st.who[k].user) << "\",\"inst\":\"" << jsonEscape(st.who[k].instrument)
          << "\",\"joinedMs\":" << st.who[k].joinedMs << ",\"lan\":" << (st.who[k].lan ? "true" : "false") << "}";
    o << "]}";
    return o.str();
}
std::string SonobusHubWeb::stagesJson() const {
    std::ostringstream o;
    std::string city, region;   // this hub's own place: what New stage starts from
    if (mArgs) { std::lock_guard<std::mutex> g(mSettingsLock); hs::splitLocation(mArgs->location, city, region); }
    o << "{\"manage\":" << (hs::canManageOtherStages() ? "true" : "false") << ",\"control\":" << (hs::canStartOtherStages() ? "true" : "false")
      << ",\"cores\":" << hs::stageCores() << ",\"running\":" << hs::stagesRunning()
      << ",\"forward\":\"" << jsonEscape(hs::stagesForwardHelp()) << "\""
      << ",\"city\":\"" << jsonEscape(city) << "\",\"region\":\"" << jsonEscape(region) << "\",\"regionCodes\":\"" << hs::regionCodes(mArgs ? mArgs->stageCountry : std::string()) << "\""
      << ",\"warning\":\"" << jsonEscape(hs::stageLoadWarning(0)) << "\",\"warningOneMore\":\"" << jsonEscape(hs::stageLoadWarning(1)) << "\",\"stages\":[";
    bool first = true;
    for (const auto& st : hs::otherStages()) {
        if (!first) o << ","; first = false;
        o << stageJson(st);
    }
    o << "]}";
    return o.str();
}

// POST /api/stages/new, body {"name","password","listed":bool,"city","webPassword"}: answers {"ok":true,"stage":{...}}.
// POST /api/stages/start|stop|delete|forget|add|moveports|checkports, body {"file":"<path of its hub_settings.json>"}. Gated like the other POSTs.
std::string SonobusHubWeb::stagesRequest(const std::string& action, const std::string& body, bool& ok) {
    ok = false;
    const juce::var root = juce::JSON::parse(juce::String::fromUTF8(body.c_str(), (int) body.size()));
    auto* obj = root.getDynamicObject();
    if (obj == nullptr) return "{\"ok\":false,\"error\":\"body is not a JSON object\"}";
    std::string err;
    if (action == "new") {
        hs::NewStage in;
        in.name = obj->getProperty("name").toString().toStdString();
        in.password = obj->getProperty("password").toString().toStdString();
        in.city = obj->getProperty("city").toString().toStdString();
        in.region = obj->getProperty("region").toString().toStdString();
        in.webPassword = obj->getProperty("webPassword").toString().toStdString();
        in.listed = (bool) obj->getProperty("listed");
        hs::OtherStage made;
        if (!hs::createStage(in, made, err)) return "{\"ok\":false,\"error\":\"" + jsonEscape(err) + "\"}";
        ok = true;
        std::cout << "[HUB-STAGES] new stage request: web page password " << (in.webPassword.empty() ? "not given (this hub's is used)" : "given") << "\n";
        return "{\"ok\":true,\"stage\":" + stageJson(made) + ",\"webPasswordOwn\":" + (in.webPassword.empty() ? "false" : "true") + ",\"forward\":\"" + jsonEscape(hs::stagesForwardHelp()) + "\"}";
    }
    const juce::String path = obj->getProperty("file").toString().trim();
    if (path.isEmpty() || !juce::File::isAbsolutePath(path)) return "{\"ok\":false,\"error\":\"give the full path of the stage's hub_settings.json\"}";
    const juce::File file(path);
    bool done = false;
    if (action == "add") done = hs::addOtherStage(file, err);
    else if (action == "start") done = hs::startOtherStage(file, err);
    else if (action == "stop") done = hs::stopOtherStage(file, err);
    else if (action == "forget") done = hs::forgetOtherStage(file, err);
    else if (action == "delete") done = hs::deleteStage(file, err);
    else if (action == "moveports") done = hs::moveStagePorts(file, err);
    else if (action == "checkports") done = hs::checkStagePorts(file, err);
    else return "{\"ok\":false,\"error\":\"unknown action\"}";
    if (!done) return "{\"ok\":false,\"error\":\"" + jsonEscape(err) + "\"}";
    ok = true;
    return "{\"ok\":true}";
}

// GET /api/previous: {"players":[{"user","location","lan","lastJoinMs","lastLeftMs","joins","removed","aka"}]}: removed
// players first (newest removal first), then newest first. A removed row also has "kind" ("timed"|"restart"|
// "permanent"), "leftSec" (timed only, else -1), "removedAtMs", "removedUser" (the name the removal is under) and, only
// where /api/removed answers (web password set), "addr". No other addresses.
std::string SonobusHubWeb::previousJson() const {
    const auto list = mHub.previousRows();
    const bool withAddr = !mPassword.empty() && mArgs && mSettingsFile != juce::File();
    const long long nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    std::ostringstream o; o << "{\"players\":[";
    bool first = true;
    for (const auto& row : list) {
        const auto& r = row.player;
        if (!first) o << ","; first = false;
        o << "{\"user\":\"" << jsonEscape(r.user) << "\",\"location\":\"" << jsonEscape(r.joins > 0 ? locationFor(r.ip, r.lan) : std::string()) << "\",\"lan\":" << (r.lan ? "true" : "false")
          << ",\"lastJoinMs\":" << r.lastJoinMs << ",\"lastLeftMs\":" << r.lastLeftMs << ",\"joins\":" << r.joins << ",\"removed\":" << (row.removed ? "true" : "false");
        if (!row.alsoSeenAs.empty()) o << ",\"aka\":\"" << jsonEscape(row.alsoSeenAs) << "\"";
        if (row.removed) {
            const auto& b = row.removal;
            const bool timed = b.kind == SonobusHubServer::RemovalKind::Timed;
            o << ",\"kind\":\"" << (timed ? "timed" : b.kind == SonobusHubServer::RemovalKind::UntilRestart ? "restart" : "permanent") << "\""
              << ",\"leftSec\":" << (timed ? std::max<long long>(0, (b.untilMs - nowMs + 999) / 1000) : -1LL)
              << ",\"removedAtMs\":" << b.removedAtMs << ",\"removedUser\":\"" << jsonEscape(b.user) << "\"";
            if (withAddr) o << ",\"addr\":\"" << jsonEscape(b.ip) << "\"";
        }
        o << "}";
    }
    o << "]}";
    return o.str();
}

// POST /api/unkick, body {"user":"...","addr":"..."} as listed by /api/removed, gated like the other POSTs.
// Lifts that removal at once, whatever its kind.
std::string SonobusHubWeb::unkickRequest(const std::string& body, bool& ok) {
    ok = false;
    const juce::var root = juce::JSON::parse(juce::String::fromUTF8(body.c_str(), (int) body.size()));
    auto* obj = root.getDynamicObject();
    if (obj == nullptr) return "{\"ok\":false,\"error\":\"body is not a JSON object\"}";
    const std::string user = obj->getProperty("user").toString().toStdString(), addr = obj->getProperty("addr").toString().toStdString();
    if (user.empty() && addr.empty()) return "{\"ok\":false,\"error\":\"no player given\"}";
    if (!mHub.allowBack(user, addr)) return "{\"ok\":false,\"error\":\"that player is not in the removed list\"}";
    std::cout << "[HUB-WEB] allow back '" << user << "'\n";
    ok = true;
    return "{\"ok\":true}";
}

// GET /api/removed: {"ok":true,"removed":[{"user","addr","removedAtMs","kind":"timed"|"restart"|"permanent","leftSec"}]},
// newest first. leftSec counts for "timed" only (-1 otherwise).
std::string SonobusHubWeb::removedJson() const {
    const auto list = mHub.removedPlayers();
    const long long nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    std::ostringstream o; o << "{\"ok\":true,\"removed\":[";
    bool first = true;
    for (const auto& r : list) {
        const bool timed = r.kind == SonobusHubServer::RemovalKind::Timed;
        if (!first) o << ","; first = false;
        o << "{\"user\":\"" << jsonEscape(r.user) << "\",\"addr\":\"" << jsonEscape(r.ip) << "\",\"removedAtMs\":" << r.removedAtMs
          << ",\"kind\":\"" << (timed ? "timed" : r.kind == SonobusHubServer::RemovalKind::UntilRestart ? "restart" : "permanent") << "\""
          << ",\"leftSec\":" << (timed ? std::max<long long>(0, (r.untilMs - nowMs + 999) / 1000) : -1LL) << "}";
    }
    o << "]}";
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

// Fields for the update banner. updateState carries the install progress while one runs.
std::string SonobusHubWeb::updateInfoJson() const {
    const auto info = mUpdateCheck != nullptr ? mUpdateCheck->info() : sb_hub_gui::HubUpdateInfo{};
    std::string state;
    { std::lock_guard<std::mutex> g(mUpdateLock); state = mUpdateState; }
    return std::string("\"updateAvailable\":") + (info.available ? "true" : "false")
         + ",\"updateVersion\":\"" + jsonEscape(info.version.toStdString()) + "\",\"updateBuild\":\"" + jsonEscape(info.build.toStdString())
         + "\",\"updateNotesUrl\":\"" + jsonEscape(info.notesUrl.toStdString()) + "\",\"updateState\":\"" + jsonEscape(state) + "\",\"updateRunning\":" + (mUpdateRunning.load() ? "true" : "false") + ",";
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
    const auto snap = mHub.snapshotForUI();
    const bool enabled = mArgs && !mPassword.empty() && mSettingsFile != juce::File();
    o << "{\"enabled\":" << (enabled ? "true" : "false")
      << ",\"reason\":\"" << jsonEscape(!mArgs ? "settings not wired on this platform" : mSettingsFile == juce::File() ? "hub started with --no-settings" : mPassword.empty() ? "set web_password (hub_settings.json or --web-password) to enable Settings on the web page" : "") << "\""
      << ",\"file\":\"" << jsonEscape(mSettingsFile == juce::File() ? std::string() : mSettingsFile.getFullPathName().toStdString())
      << "\",\"haveFile\":" << (mSettingsFile.existsAsFile() ? "true" : "false")
      << ",\"detected\":\"" << jsonEscape(snap.advertisedPublicHost) << "\",\"regionsUS\":\"" << hs::regionCodes("US") << "\",\"regionsCA\":\"" << hs::regionCodes("CA") << "\",\"fields\":[";
    sb_hub_gui::HubLaunchArgs defaults;
    hs::applyDefaults(defaults);
    bool first = true;
    for (const auto& sp : hs::formSpecs()) {
        if (!first) o << ","; first = false;
        o << "{\"col\":" << sp.col << ",\"key\":" << (sp.key ? "\"" + jsonEscape(sp.key) + "\"" : "null")
          << ",\"tip\":\"" << jsonEscape(sp.key ? hs::tipFor(sp.key) : "") << "\""
          << ",\"label\":\"" << jsonEscape(sp.label) << "\",\"kind\":" << sp.kind
          << ",\"choices\":\"" << jsonEscape(sp.choices ? sp.choices : "") << "\"";
        if (sp.hint) o << ",\"hint\":\"" << jsonEscape(sp.hint) << "\"";
        if (sp.key && enabled) {
            const std::string v = hs::getValueString(*mArgs, sp.key).toStdString();
            o << ",\"def\":\"" << jsonEscape(hs::getValueString(defaults, sp.key).toStdString()) << "\"";
            o << ",\"tier\":\"" << tierName(hs::tierForKey(sp.key)) << "\",\"value\":\"" << jsonEscape(sp.kind == 3 ? std::string() : v) << "\",\"set\":" << (v.empty() ? "false" : "true");
            if (std::strcmp(sp.key, "group_password") == 0) o << ",\"pending\":" << (v != snap.groupPassword ? "true" : "false");   // saved, not live until the hub restarts
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
    bool locationSent = false;
    if (auto* vo = values.getDynamicObject()) {
        for (const auto& prop : vo->getProperties()) {
            const std::string key = prop.name.toString().toStdString();
            if (key == "location_city" || key == "location_region") locationSent = true;
            const auto* sp = specFor(key);
            if (!sp) return "{\"ok\":false,\"error\":\"unknown key: " + jsonEscape(key) + "\"}";
            const juce::String text = prop.value.toString();
            if (sp->kind == 3 && text.isEmpty()) continue;   // password left blank = keep the current one
            if (!hs::setValueFromString(n, key, text))
                return "{\"ok\":false,\"error\":\"bad value for " + jsonEscape(sp->label) + ": '" + jsonEscape(text.toStdString()) + "'\"}";
        }
    }
    if (const std::string problem = hs::validate(n); !problem.empty()) return "{\"ok\":false,\"error\":\"" + jsonEscape(problem) + "\"}";
    if (locationSent) {   // a save of the settings form: the city and the state, province or region are both needed
        hs::normaliseLocation(n);
        if (const std::string problem = hs::locationProblem(n); !problem.empty()) return "{\"ok\":false,\"error\":\"" + jsonEscape(problem) + "\"}";
    }
    if (const std::string problem = hs::publicAddressProblem(n.publicAddressHost); !problem.empty()) return "{\"ok\":false,\"error\":\"" + jsonEscape(problem) + "\"}";
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
    hs::applyStageToHub(mHub, n);   // stage limits and listing are live
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
