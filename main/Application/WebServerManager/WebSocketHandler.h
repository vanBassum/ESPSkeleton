#pragma once

#include <esp_http_server.h>
#include "Mutex.h"
#include "SessionTable.h"
#include "SessionMux.h"
#include "ConnectionRegistry.h"

class CommandManager;
class Authenticator;

class WebSocketHandler : public SessionMux::Sink {
    static constexpr const char* TAG = "WebSocketHandler";

public:
    void SetCommandManager(CommandManager& commandManager);
    void SetAuth(Authenticator& auth);

    void RegisterRoute(httpd_handle_t server);

    void Broadcast(httpd_handle_t server, const char* json, int len);
    void BroadcastBinary(httpd_handle_t server, const uint8_t* data, size_t len);

    void OnClientDisconnected(int fd);

private:
    CommandManager* commandManager_ = nullptr;
    Authenticator* auth_ = nullptr;

    // Serializes ALL outgoing frame writes. Broadcasts run on the
    // ConsoleManager task while command responses are written by the
    // httpd task — unserialized, their bytes interleave on the socket
    // and corrupt the WS framing (client sees "Invalid frame header").
    Mutex sendMutex_;

    // Per-connection auth state (replaces the ?token= upgrade check). authed is
    // set by the in-band login/auth handshake (see AuthGate), or at connect
    // when web.password is empty. WsConnection::key holds the session key once
    // authed, so TouchClient can keep it alive in the SessionTable for
    // reconnect-resume. registry_ owns the fixed slot table and the pre-auth
    // reaper (see ConnectionRegistry).
    ConnectionRegistry registry_;
    static constexpr int MAX_BIN_FAILS = 10;

    void TouchClient(int fd);

    /// False when the client table is full (after reaping stale un-authed slots).
    bool AddWsClient(int fd);

    // Session reply flush window (off the httpd-task stack; reused, single
    // session at a time). NOT payload-proportional — a small batch buffer that
    // amortizes JsonWriter's tiny writes into WS frames; a reply of any size
    // streams out window-by-window. Layout: [ 3-byte chunk header | payload ].
    static constexpr size_t SESSION_WINDOW = 512;
    uint8_t sessionFrame_[session::HEADER_LEN + SESSION_WINDOW];

    // Inbound window for streamed request bodies: read() pulls continuation
    // session chunks into here, so each body chunk's payload may be up to
    // INBOUND_WINDOW bytes (the frontend sizes upload chunks to match).
    static constexpr size_t INBOUND_WINDOW = 4096;
    uint8_t sessionInbound_[session::HEADER_LEN + INBOUND_WINDOW];

    void RemoveWsClient(int fd);

    static esp_err_t HandleWs(httpd_req_t* req);

    // Binary session transport. A request is one binary chunk; the reply
    // streams back as chunks on the same session id. The pre-auth handshake
    // verbs (hello/login/auth) and the authed/not routing decision are
    // delegated to AuthGate, constructed locally per frame (it only holds an
    // Authenticator&, so this is cheap) — see AuthGate.h.
    void HandleBinary(httpd_req_t* req, const uint8_t* frame, size_t len);
    void OnSessionOpened(Session& session) override;
};
