#pragma once

#include <esp_http_server.h>
#include "Mutex.h"
#include "SessionTable.h"

class CommandManager;
class WebServerManager;

class WebSocketHandler {
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

    char wsBuf_[4096];

    void AddWsClient(int fd, const char* token);
    void RemoveWsClient(int fd);

    static esp_err_t HandleWs(httpd_req_t* req);
    void DispatchMessage(httpd_req_t* req, int32_t id, const char* type, const char* json);
};
