#pragma once

#include <esp_http_server.h>
#include "StruxServices.h"
#include "InitState.h"
#include "CommandEntry.h"
#include "TypedSettings.h"
#include "StaticFileHandler.h"
#include "WebSocketHandler.h"
#include "Authenticator.h"

class Stream;

class WebServerManager {
    static constexpr const char* TAG = "WebServerManager";

public:
    explicit WebServerManager(StruxServices& strux);

    WebServerManager(const WebServerManager&) = delete;
    WebServerManager& operator=(const WebServerManager&) = delete;
    WebServerManager(WebServerManager&&) = delete;
    WebServerManager& operator=(WebServerManager&&) = delete;

    void Init();

    void Broadcast(const char* json, int len);
    void BroadcastBinary(const uint8_t* data, size_t len);

    /// The credential authority, shared with any other transport that carries the
    /// auth handshake (the relay pipe). Owned here because HTTP/WS auth started
    /// here; it is transport-neutral.
    Authenticator& GetAuthenticator() { return auth_; }

private:
    StruxServices& strux_;

    InitState initState;
    httpd_handle_t server_ = nullptr;

    StaticFileHandler staticFileHandler_;
    WebSocketHandler wsHandler_;

    // ── Settings (registered with SettingsManager in Init) ──
    // Empty means no login is required at all.
    inline static StringSetting webPassword_{ "web.password", "Web Password", "" };

    // Credential authority (owned here; used by the WS auth gate). No HTTP auth
    // surface remains — the WebSocket carries all device interaction. Reads
    // webPassword_ live by reference — see Authenticator.h.
    Authenticator auth_{ webPassword_ };

    void MountFatPartition();
    void StartServer();
    void RegisterRoutes();

    // ── Commands (registered with CommandManager in Init) ──

    // Serve one frontend file by logical path. This is the whole of the relay's
    // access to the device's frontend: the server asks for "/index.html" and
    // never learns that it lives gzipped on a FAT partition called www. Reply is
    // a header line then the raw bytes:
    //
    //   {"ok":true,"status":200,"contentType":"...","contentEncoding":"gzip"}\n<bytes>
    RequestError Cmd_GetWebFile(CommandContext& ctx);

    // ── auth: the handshake, as ordinary commands ──
    //
    // These live here because this manager owns the Authenticator and declares
    // web.password. They are the only commands a connection may run before
    // authenticating (AuthGate whitelists the category), and the only ones that touch
    // ctx.connection.

    /// Does this device want a password at all? Answered before login, so a client
    /// knows whether to prompt.
    RequestError Cmd_AuthHello(CommandContext& ctx);

    /// Password in, session key out. The key lets a reconnect resume without
    /// re-prompting.
    RequestError Cmd_AuthLogin(CommandContext& ctx);

    /// Resume with a key minted earlier.
    RequestError Cmd_AuthResume(CommandContext& ctx);

    inline static CommandEntry commands_[] = {
        { "web",  "read",   &InvokeCommand<&WebServerManager::Cmd_GetWebFile> },
        { "auth", "hello",  &InvokeCommand<&WebServerManager::Cmd_AuthHello>  },
        { "auth", "login",  &InvokeCommand<&WebServerManager::Cmd_AuthLogin>  },
        { "auth", "resume", &InvokeCommand<&WebServerManager::Cmd_AuthResume> },
    };
};
