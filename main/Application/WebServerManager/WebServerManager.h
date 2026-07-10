#pragma once

#include <esp_http_server.h>
#include "ServiceProvider.h"
#include "InitState.h"
#include "StaticFileHandler.h"
#include "WebSocketHandler.h"
#include "SessionTable.h"
#include "TypedSettings.h"
#include "Mutex.h"

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

    // ── Transport-edge auth (also used by WebSocketHandler) ──
    /// Full check: detects a changed web.password (clears all sessions),
    /// then validates + refreshes the token. Reads NVS — use at request/
    /// upgrade granularity, not per WS frame.
    bool ValidateToken(const char* token);
    /// Refresh only — no settings read. Safe per WS frame. A token that
    /// no longer exists is a silent no-op (live connections stay trusted
    /// for their lifetime; see spec).
    void TouchSession(const char* token);

    /// True when a password is set (auth on). Empty web.password ⇒ open.
    bool AuthRequired();
    /// Password-epoch check, then compare `pw` to web.password.
    bool CheckPassword(const char* pw);
    /// Mint a session key into `out` (must hold SessionTable::TOKEN_LEN bytes).
    void MintKey(char* out);
    /// Device name for the pre-auth `hello`.
    void GetDeviceName(char* out, size_t maxLen);

private:
    ServiceProvider& serviceProvider_;

    InitState initState;
    httpd_handle_t server_ = nullptr;

    StaticFileHandler staticFileHandler_;
    WebSocketHandler wsHandler_;

    // ── Auth state ────────────────────────────────────────────
    inline static StringSetting webPassword_{ "web.password", "Web Password", "" };
    SessionTable sessions_;
    char passwordSnapshot_[64] = {};   // last-seen password; mismatch → sessions cleared
    Mutex authMutex_;                  // guards passwordSnapshot_

    /// Compare web.password against the snapshot; on change, clear all
    /// sessions and take a new snapshot. The "hook" for password edits —
    /// SettingsManager has no change notification, so we detect lazily
    /// in every HTTP auth path (login, /api/command, WS upgrade).
    void CheckPasswordEpoch();
    bool CheckBearer(httpd_req_t* req);

    static esp_err_t HandleLoginGet(httpd_req_t* req);
    static esp_err_t HandleLoginPost(httpd_req_t* req);
    static void SendUnauthorized(httpd_req_t* req);

    void MountFatPartition();
    void StartServer();
    void RegisterRoutes();

    static esp_err_t HandleApiCommand(httpd_req_t* req);
    static esp_err_t HandleCorsPreflight(httpd_req_t* req);

    static void SetCorsHeaders(httpd_req_t* req);
};
