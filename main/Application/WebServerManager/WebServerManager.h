#pragma once

#include <esp_http_server.h>
#include "ServiceProvider.h"
#include "InitState.h"
#include "StaticFileHandler.h"
#include "WebSocketHandler.h"
#include "Authenticator.h"

class WebServerManager {
    static constexpr const char* TAG = "WebServerManager";

public:
    explicit WebServerManager(ServiceProvider& serviceProvider);

    WebServerManager(const WebServerManager&) = delete;
    WebServerManager& operator=(const WebServerManager&) = delete;
    WebServerManager(WebServerManager&&) = delete;
    WebServerManager& operator=(WebServerManager&&) = delete;

    void Init();

    void Broadcast(const char* json, int len);
    void BroadcastBinary(const uint8_t* data, size_t len);

private:
    ServiceProvider& serviceProvider_;

    InitState initState;
    httpd_handle_t server_ = nullptr;

    StaticFileHandler staticFileHandler_;
    WebSocketHandler wsHandler_;

    // Credential authority (owned here; used by the WS auth gate). No HTTP auth
    // surface remains — the WebSocket carries all device interaction.
    Authenticator auth_;

    void MountFatPartition();
    void StartServer();
    void RegisterRoutes();
};
