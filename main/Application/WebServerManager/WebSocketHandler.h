#pragma once

#include <esp_http_server.h>
#include "Mutex.h"

class CommandManager;

class WebSocketHandler {
    static constexpr const char* TAG = "WebSocketHandler";
    static constexpr int MAX_WS_CLIENTS = 4;

public:
    void SetCommandManager(CommandManager& commandManager);

    void RegisterRoute(httpd_handle_t server);

    void Broadcast(httpd_handle_t server, const char* json, int len);
    void BroadcastBinary(httpd_handle_t server, const uint8_t* data, size_t len);

    void OnClientDisconnected(int fd);

private:
    CommandManager* commandManager_ = nullptr;

    Mutex wsMutex_;
    int wsClients_[MAX_WS_CLIENTS] = {};
    int consecBinFails_[MAX_WS_CLIENTS] = {};
    static constexpr int MAX_BIN_FAILS = 10;

    char wsBuf_[4096];

    void AddWsClient(int fd);
    void RemoveWsClient(int fd);

    static esp_err_t HandleWs(httpd_req_t* req);
    void DispatchMessage(httpd_req_t* req, int32_t id, const char* type, const char* json);
};
