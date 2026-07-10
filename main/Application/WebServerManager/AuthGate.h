#pragma once
#include <esp_http_server.h>
#include <cstdint>
#include <cstddef>
#include "WsSessionLink.h"

class Authenticator;
struct WsConnection;

// The pre-auth handshake + authed/not routing decision, self-contained.
// Depends only on the Authenticator. Owns the reply framing.
class AuthGate {
public:
    explicit AuthGate(Authenticator& auth) : auth_(auth) {}

    enum class Disposition { Handled, PassToMux, Rejected };

    // Parse the first chunk's `type`. hello/login/auth → handled here (reply via
    // `link`, flip conn.authed on success), returns Handled. An authed non-verb
    // → PassToMux. An unauthenticated non-verb → REJECT reply, Rejected.
    Disposition Handle(WsConnection& conn, WsSessionLink& link,
                       uint16_t sid, const uint8_t* payload, size_t len);

private:
    Authenticator& auth_;
    void SendReply(WsSessionLink& link, uint16_t sid, const char* json);
    void SendReject(WsSessionLink& link, uint16_t sid, const char* reason);
};
