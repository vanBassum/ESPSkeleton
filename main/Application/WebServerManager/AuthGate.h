#pragma once
#include <cstdint>
#include <cstddef>
#include "SessionLink.h"

class Authenticator;
struct WsConnection;

// The pre-auth handshake + authed/not routing decision, self-contained.
// Depends only on the Authenticator. Owns the reply framing.
//
// Transport-agnostic: it replies through a SessionLink, so the same handshake
// serves the local browser socket and the relay pipe. A relayed frontend gets
// device login for free — see the remote-access relay design.
class AuthGate {
public:
    explicit AuthGate(Authenticator& auth) : auth_(auth) {}

    enum class Disposition { Handled, Dispatch, Rejected };

    // Parse the first chunk's `type`. hello/login/auth → handled here (reply via
    // `link`, flip conn.authed on success), returns Handled. An authed non-verb
    // → Dispatch (the caller opens a Session and runs it). An unauthenticated non-verb → REJECT reply, Rejected.
    Disposition Handle(WsConnection& conn, SessionLink& link,
                       uint16_t sid, const uint8_t* payload, size_t len);

private:
    Authenticator& auth_;
    void SendReply(SessionLink& link, uint16_t sid, const char* json);
    void SendReject(SessionLink& link, uint16_t sid, const char* reason);
};
