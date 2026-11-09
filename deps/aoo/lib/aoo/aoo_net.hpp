/* Copyright (c) 2010-Now Christof Ressi, Winfried Ritsch and others.
 * For information on usage and redistribution, and for a DISCLAIMER OF ALL
 * WARRANTIES, see the file, "LICENSE.txt," in this distribution.  */

#pragma once

#include "aoo_net.h"

#include <memory>

namespace aoo {
namespace net {

// NOTE: aoo::iserver and aoo::iclient don't define virtual destructors
// and have to be destroyed with their respective destroy() method.
// We provide a custom deleter and shared pointer to automate this task.
//
// The absence of a virtual destructor allows for ABI independent
// C++ interfaces on Windows (where the vtable layout is stable
// because of COM) and usually also on other platforms.
// (Compilers use different strategies for virtual destructors,
// some even put more than 1 entry in the vtable.)
// Also, we only use standard C types as function parameters
// and return types.
//
// In practice this means you only have to build 'aoo' once as a
// shared library and can then use its C++ interface in applications
// built with different compilers resp. compiler versions.
//
// If you want to be on the safe safe, use the C interface :-)

/*//////////////////////// AoO server ///////////////////////*/

class iserver {
public:
    class deleter {
    public:
        void operator()(iserver *x){
            destroy(x);
        }
    };
    // smart pointer for AoO source instance
    using pointer = std::unique_ptr<iserver, deleter>;

    // create a new AoO source instance
    static iserver * create(int port, int32_t *err);

    // destroy the AoO source instance
    static void destroy(iserver *server);

    // run the AOO server; this function blocks indefinitely.
    virtual int32_t run() = 0;

    // quit the AOO server from another thread
    virtual int32_t quit() = 0;

    // get number of pending events (always thread safe)
    virtual int32_t events_available() = 0;

    // handle events (threadsafe, but not reentrant)
    // will call the event handler function one or more times
    virtual int32_t handle_events(aoo_eventhandler fn, void *user) = 0;

    // LATER add methods to add/remove users and groups
    // and set/get server options, group options and user options
    
    // get number of currently active groups
    virtual int32_t get_group_count() const = 0;

    // get number of currently active users
    virtual int32_t get_user_count() const = 0;

    // Configure a LAN subnet in CIDR notation (e.g. "192.0.2.199/24"). When set,
    // the server classifies each peer by the actual TCP source IP it observed
    // at accept() time. When broadcasting a peer's addresses to other group
    // members, recipients whose source IP falls within the LAN subnet receive
    // the source peer's LAN (local) address in BOTH the public and local fields
    // of the PEER_JOIN message. WAN recipients receive the defaults (claimed
    // public + claimed local). This lets a hub launched with --public-address
    // <WAN-IP> remain reachable from both LAN peers (LAN-only handshake) and
    // WAN peers (WAN handshake) without trade-off. Pass empty string to clear.
    // Returns 1 on success, 0 on parse error.
    virtual int32_t set_lan_subnet(const char *cidr) = 0;

protected:
    ~iserver(){} // non-virtual!
};

inline iserver * iserver::create(int port, int32_t *err){
    return aoonet_server_new(port, err);
}

inline void iserver::destroy(iserver *server){
    aoonet_server_free(server);
}

/*//////////////////////// AoO client ///////////////////////*/

class iclient {
public:
    class deleter {
    public:
        void operator()(iclient *x){
            destroy(x);
        }
    };
    // smart pointer for AoO sink instance
    using pointer = std::unique_ptr<iclient, deleter>;

    // create a new AoO sink instance
    static iclient * create(void *socket, aoo_sendfn fn, int port);

    // destroy the AoO sink instance
    static void destroy(iclient *client);

    // run the AOO client; this function blocks indefinitely.
    virtual int32_t run() = 0;

    // quit the AOO client from another thread
    virtual int32_t quit() = 0;

    // connect AOO client to a AOO server (always thread safe)
    virtual int32_t connect(const char *host, int port,
                            const char *username, const char *pwd) = 0;

    // disconnect AOO client from AOO server (always thread safe)
    virtual int32_t disconnect() = 0;

    // join an AOO group
    virtual int32_t group_join(const char *group, const char *pwd, bool is_public=false) = 0;

    // leave an AOO group
    virtual int32_t group_leave(const char *group) = 0;

    // register interest in public groups
    virtual int32_t group_watch_public(bool watch) = 0;

    // handle messages from peers (threadsafe, but not reentrant)
    // 'addr' should be sockaddr *
    virtual int32_t handle_message(const char *data, int32_t n, void *addr) = 0;

    // send outgoing messages to peers (threadsafe, but not reentrant)
    virtual int32_t send() = 0;

    // get number of pending events (always thread safe)
    virtual int32_t events_available() = 0;

    // handle events (threadsafe, but not reentrant)
    // will call the event handler function one or more times
    virtual int32_t handle_events(aoo_eventhandler fn, void *user) = 0;

    // Override the public address advertised in the LOGIN message to the AOO
    // server. Useful when the AOO server is reached via a NAT path that does
    // not preserve the WAN IP (e.g. asymmetric hairpin NAT, where the router
    // rewrites the hairpinned source to the LAN gateway address). Other peers
    // are then told to reach this client at <host>:<port> instead. Pass an
    // empty host to clear the override. Must be called BEFORE connect().
    virtual int32_t set_self_public_address(const char *host, int port) = 0;

    // Override the LOCAL (LAN) address
    // advertised in the LOGIN message. The library derives it from the TCP
    // socket used to reach the AOO server; when that server is reached over
    // loopback (the hub's built-in server at 127.0.0.1) the derived value is
    // 127.0.0.1 and same-LAN peers would be told to reach us there. Empty host
    // clears the override. Port <= 0 keeps the client's UDP port. Must be
    // called BEFORE connect().
    virtual int32_t set_self_local_address(const char *host, int port) = 0;
    // Only establish a UDP peer connection with this user; every other peer the server
    // introduces is "virtual" (no handshake, no pings; join/leave events still delivered). "" = off.
    virtual int32_t set_peer_filter(const char *only_user) = 0;

    // LATER add API functions to set options and do additional peer communication (chat, OSC messages, etc.)
protected:
    ~iclient(){} // non-virtual!
};

inline iclient * iclient::create(void *socket, aoo_sendfn fn, int port){
    return aoonet_client_new(socket, fn, port);
}

inline void iclient::destroy(iclient *client){
    aoonet_client_free(client);
}

} // net
} // aoo
