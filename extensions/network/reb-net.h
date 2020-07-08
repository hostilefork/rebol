//
//  File: %reb-net.h
//  Summary: "Network device definitions"
//  Project: "Revolt Language Interpreter and Run-time Environment"
//  Homepage: https://github.com/metaeducation/ren-c/
//
//=////////////////////////////////////////////////////////////////////////=//
//
// Copyright 2012 REBOL Technologies
// Copyright 2012-2017 Revolt Open Source Contributors
// REBOL is a trademark of REBOL Technologies
//
// See README.md and CREDITS.md for more information.
//
// Licensed under the Lesser GPL, Version 3.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// https://www.gnu.org/licenses/lgpl-3.0.html
//
//=////////////////////////////////////////////////////////////////////////=//
//

EXTERN_C REBDEV Dev_Net;

// REBOL Socket types:
enum socket_types {
    RST_UDP     = 1 << 0,   // TCP or UDP
    RST_LISTEN  = 1 << 8,   // LISTEN
    RST_REVERSE = 1 << 9    // DNS reverse
};

// REBOL Socket Modes (state flags)
enum {
    RSM_OPEN    = 1 << 0,   // socket is allocated
    RSM_ATTEMPT = 1 << 1,   // attempting connection
    RSM_CONNECT = 1 << 2,   // connection is open
    RSM_BIND    = 1 << 3,   // socket is bound to port
    RSM_LISTEN  = 1 << 4,   // socket is listening (TCP)
    RSM_SEND    = 1 << 5,   // sending
    RSM_RECEIVE = 1 << 6,   // receiving
    RSM_ACCEPT  = 1 << 7    // an inbound connection
};

#define IPA(a,b,c,d) (a<<24 | b<<16 | c<<8 | d)

struct devreq_net {
    struct rebol_devreq devreq;
    uint32_t local_ip;      // local address used
    uint32_t local_port;    // local port used
    uint32_t remote_ip;     // remote address
    uint32_t remote_port;   // remote port
    void *host_info;        // for DNS usage
};

inline static struct devreq_net *ReqNet(REBREQ *req) {
    assert(Req(req)->device == &Dev_Net);
    return cast(struct devreq_net*, Req(req));
}

