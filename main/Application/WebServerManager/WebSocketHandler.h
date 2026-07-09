#pragma once

#include <esp_http_server.h>
#include "Mutex.h"
#include "SessionTable.h"
#include "SessionMux.h"

class CommandManager;
class WebServerManager;

class WebSocketHandler : public SessionMux::Sink {
    static constexpr const char* TAG = "WebSocketHandler";
    static constexpr int MAX_WS_CLIENTS = 4;

public:
    void SetCommandManager(CommandManager& commandManager);
    void SetAuth(WebServerManager& auth);

    void RegisterRoute(httpd_handle_t server);

    void Broadcast(httpd_handle_t server, const char* json, int len);
    void BroadcastBinary(httpd_handle_t server, const uint8_t* data, size_t len);

    void OnClientDisconnected(int fd);

private:
    CommandManager* commandManager_ = nullptr;
    WebServerManager* auth_ = nullptr;

    Mutex wsMutex_;

    // Serializes ALL outgoing frame writes. Broadcasts run on the
    // ConsoleManager task while command responses are written by the
    // httpd task — unserialized, their bytes interleave on the socket
    // and corrupt the WS framing (client sees "Invalid frame header").
    Mutex sendMutex_;

    int wsClients_[MAX_WS_CLIENTS] = {};
    int consecBinFails_[MAX_WS_CLIENTS] = {};
    static constexpr int MAX_BIN_FAILS = 10;

    // Session token bound to each connection at upgrade time. Frames
    // refresh the session by token; if the session was meanwhile
    // evicted/cleared, the refresh is a no-op — the live connection
    // stays trusted for its lifetime (spec), it just can't reconnect.
    char clientTokens_[MAX_WS_CLIENTS][SessionTable::TOKEN_LEN] = {};

    void TouchClient(int fd);

    // Fragment buffer for streamed command replies: filled and flushed as a WS
    // fragment each time it fills, so a reply is no longer capped at this size
    // (see WsResponseStream in the .cpp). One reply owns the socket for its
    // duration — head-of-line blocking, accepted until multiplexing lands.
    // (Legacy TEXT path; removed in the session-transport cutover.)
    char wsBuf_[4096];

    // Session reply flush window (off the httpd-task stack; reused, single
    // session at a time). NOT payload-proportional — a small batch buffer that
    // amortizes JsonWriter's tiny writes into WS frames; a reply of any size
    // streams out window-by-window. Layout: [ 3-byte chunk header | payload ].
    static constexpr size_t SESSION_WINDOW = 512;
    uint8_t sessionFrame_[session::HEADER_LEN + SESSION_WINDOW];

    /// False when the client table is full — caller refuses the upgrade.
    bool AddWsClient(int fd, const char* token);
    void RemoveWsClient(int fd);

    static esp_err_t HandleWs(httpd_req_t* req);
    void DispatchMessage(httpd_req_t* req, int32_t id, const char* type, const char* json);

    // New binary session transport. A request is one binary chunk; the reply
    // streams back as chunks on the same session id. Runs alongside the TEXT
    // path until the frontend is migrated (then the TEXT request path is removed).
    void HandleBinary(httpd_req_t* req, const uint8_t* frame, size_t len);
    void OnSessionOpened(Session& session) override;
};
