#pragma once
#include "SessionTable.h"
#include <cstdint>
#include <cstring>

// One live WebSocket connection's state. Value/state object — no I/O.
struct WsConnection {
    int      fd = 0;                                 // 0 = empty slot
    bool     authed = false;
    char     key[SessionTable::TOKEN_LEN] = {};      // session key once authed
    int64_t  connectedAt = 0;
    int      consecBinFails = 0;

    bool active() const { return fd != 0; }
    int64_t age(int64_t now) const { return now - connectedAt; }
    void authenticate(const char* k)
    {
        authed = true;
        strlcpy(key, k ? k : "", sizeof(key));
    }
    void reset()
    {
        fd = 0; authed = false; key[0] = 0; connectedAt = 0; consecBinFails = 0;
    }
};
