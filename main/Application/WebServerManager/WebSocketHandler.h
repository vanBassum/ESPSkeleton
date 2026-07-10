#pragma once

#include <esp_http_server.h>
#include "Mutex.h"
#include "SessionTable.h"
#include "SessionMux.h"

class CommandManager;
class Authenticator;

class WebSocketHandler : public SessionMux::Sink {
    static constexpr const char* TAG = "WebSocketHandler";
    static constexpr int MAX_WS_CLIENTS = 4;

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

    Mutex wsMutex_;

    // Serializes ALL outgoing frame writes. Broadcasts run on the
    // ConsoleManager task while command responses are written by the
    // httpd task — unserialized, their bytes interleave on the socket
    // and corrupt the WS framing (client sees "Invalid frame header").
    Mutex sendMutex_;

    int wsClients_[MAX_WS_CLIENTS] = {};
    int consecBinFails_[MAX_WS_CLIENTS] = {};
    static constexpr int MAX_BIN_FAILS = 10;

    // Per-connection auth state (replaces the ?token= upgrade check). authed is
    // set by the in-band login/auth handshake (see HandlePreAuth), or at connect
    // when web.password is empty. clientTokens_ holds the session key once authed,
    // so TouchClient can keep it alive in the SessionTable for reconnect-resume.
    char    clientTokens_[MAX_WS_CLIENTS][SessionTable::TOKEN_LEN] = {};
    bool    clientAuthed_[MAX_WS_CLIENTS] = {};
    int64_t clientConnectedAt_[MAX_WS_CLIENTS] = {};
    static constexpr int64_t PRE_AUTH_TIMEOUT_US = 10LL * 1000 * 1000;   // reap idle un-authed sockets

    void TouchClient(int fd);

    /// True if the connection on `fd` is authenticated.
    bool IsAuthed(int fd);

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
    // streams back as chunks on the same session id.
    void HandleBinary(httpd_req_t* req, const uint8_t* frame, size_t len);
    void OnSessionOpened(Session& session) override;

    // Transport handshake verbs (hello / login / auth), handled by the gate for
    // ANY connection — the mux and CommandManager never see them. `line` is the
    // request's header line (NUL-terminated, newline stripped).
    void HandlePreAuth(httpd_req_t* req, int fd, uint16_t sid, const char* line);
    void SetAuthed(int fd, const char* key);
    void SendReply(httpd_req_t* req, uint16_t sid, const char* json);
    void SendReplyN(httpd_req_t* req, uint16_t sid, const void* data, size_t len);
    void SendReject(httpd_req_t* req, uint16_t sid, const char* reason);
};
