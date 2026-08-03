#pragma once
#include "CommandContext.h"
#include "WsConnection.h"
#include "Authenticator.h"
#include <cstring>

// The pre-authentication gate, and nothing else.
//
// It used to parse hello/login/auth out of the first chunk and write its own framed
// replies — a second command dispatcher living below the real one. Those verbs are now
// ordinary commands in the `auth` category (WebServerManager owns them, because it owns
// the Authenticator), so what remains here is a whitelist: before a connection has
// authenticated, only that one category dispatches.
//
// Which is what the name always claimed. It is a gate.
//
// This is also where per-transport authentication stays honest. A transport that proves
// its peer at the link layer — Bluetooth pairing in the KC1245 fork — marks its
// connection authed when bonding completes and never sees the `auth` category at all.
// No login handshake and no separate code path: a policy difference rather than a
// structural one. See docs/reasoning/ on authentication belonging to the transport.
class AuthGate final : public ConnectionAuth
{
public:
    static constexpr const char* CATEGORY = "auth";

    AuthGate(WsConnection& conn, Authenticator& auth) : conn_(conn), auth_(auth) {}

    /// May this category run yet? Everything once authenticated, everything while no
    /// password is set, and `auth` always.
    ///
    /// "No password set" is asked HERE rather than trusted from connect time. A
    /// connection that came up while a password was configured used to stay
    /// unauthenticated for its whole life even after the password was cleared —
    /// which is easy to miss on the browser socket (reconnects are frequent) and
    /// obvious on the relay pipe, which stays up for days.
    bool Allows(const char* category) const
    {
        return conn_.authed
            || !auth_.AuthRequired()
            || strcmp(category, CATEGORY) == 0;
    }

    // ── ConnectionAuth: what the `auth` handlers are lent through the context ──
    void authenticate(const char* key) override { conn_.authenticate(key); }
    bool isAuthed() const override { return conn_.authed; }

private:
    WsConnection&  conn_;
    Authenticator& auth_;
};
