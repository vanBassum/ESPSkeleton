#pragma once

#include "ServiceProvider.h"
#include "InitState.h"
#include "TypedSettings.h"
#include "Task.h"
#include "SessionMux.h"
#include "SessionProtocol.h"
#include "WsConnection.h"
#include "RelaySessionLink.h"

#include <esp_websocket_client.h>

class Authenticator;

// The device's outbound connection to a relay server, so the device is reachable
// from outside its LAN without a VPN or a port forward.
//
// It is *only* a transport: a second SessionLink under the same SessionMux the
// local browser socket uses, so AuthGate, CommandManager and every command
// handler are reached unchanged and know nothing about the relay. The server asks
// for frontend files with the ordinary `getWebFile` command and relays browser
// traffic as opaque session chunks.
//
// Design: docs/superpowers/specs/2026-08-02-remote-access-relay-design.md
class RelayManager
{
    static constexpr const char* TAG = "RelayManager";

    // Reply window. Larger than the local socket's 512 because every chunk is a
    // WAN round trip's worth of framing, and frontend bundles are the common
    // payload. NOT payload-proportional — a file of any size streams window by
    // window.
    static constexpr size_t SESSION_WINDOW = 1024;

    // Must match the largest chunk a client sends (the frontend sizes upload
    // chunks to the local transport's 4096).
    static constexpr size_t INBOUND_WINDOW = 4096;

    // Deep enough that a firmware push keeps streaming while the consumer pauses to
    // write flash (~32 ms per 4 KB sector). Depth 8 was sized for request/reply; the
    // KC1245 fork measured chunk loss at that depth on a continuous upload, and a
    // dropped chunk is invisible to the session layer — it silently corrupts the
    // image rather than failing.
    static constexpr int QUEUE_DEPTH = 16;
    static constexpr int TASK_STACK  = 8192;

public:
    explicit RelayManager(ServiceProvider& serviceProvider);

    RelayManager(const RelayManager&) = delete;
    RelayManager& operator=(const RelayManager&) = delete;
    RelayManager(RelayManager&&) = delete;
    RelayManager& operator=(RelayManager&&) = delete;

    void Init();

    /// Push a log line down the relay pipe as a session-0 broadcast chunk — the
    /// same shape the local socket sends, so a relayed frontend gets live logs
    /// with no protocol difference. No-op while disconnected or disabled.
    void BroadcastLog(const char* json, int len);

    bool IsConnected() const { return linkUp_; }

private:
    ServiceProvider& serviceProvider_;
    InitState initState_;

    esp_websocket_client_handle_t client_ = nullptr;
    QueueHandle_t inbound_ = nullptr;
    Task task_;
    volatile bool linkUp_ = false;

    Authenticator* auth_ = nullptr;

    // Auth state for the pipe. One WsConnection because the pipe IS one
    // connection: every browser the server relays shares it, so a login by one
    // remote user authenticates the pipe for all of them. Acceptable for a single
    // trusted operator; splitting per-browser needs the server to carry a client
    // identity alongside the session id.
    WsConnection conn_;

    // Sized for the worst case BuildUri() can produce: url + id + fw version.
    char uri_[256] = {};
    char deviceId_[48] = {};

    // Framing buffers, members rather than stack: this task's stack is sized for
    // the heaviest command handler and must not also carry these.
    uint8_t sessionFrame_[session::HEADER_LEN + SESSION_WINDOW];
    uint8_t sessionInbound_[session::HEADER_LEN + INBOUND_WINDOW];

    // WebSocket frame reassembly. esp_websocket_client can deliver one frame as
    // several DATA events; a chunk must be whole before it is queued or the mux
    // gets a truncated header.
    uint8_t asmBuf_[session::HEADER_LEN + INBOUND_WINDOW];
    size_t  asmLen_ = 0;
    bool    asmActive_ = false;
    bool    asmOverflow_ = false;

    void BuildUri();
    void ResolveDeviceId();
    void TaskLoop();
    void HandleFrame(const uint8_t* frame, size_t len);

    void OnConnected();
    void OnDisconnected();
    void OnData(const void* eventData);
    /// Discard everything queued; returns the number of real chunks dropped
    /// (sentinels excluded) so a caller can report residue.
    size_t DrainQueue();

    static void EventHandler(void* ctx, esp_event_base_t base, int32_t id, void* data);

    // ── Settings (registered with SettingsManager in Init) ──
    inline static BoolSetting   enabled_  { "relay.enabled",  "Relay Enabled",   false };
    inline static StringSetting url_      { "relay.url",      "Relay Server URL", "" };
    // Empty → derived from the WiFi MAC, so a fresh device registers without
    // being told who it is.
    inline static StringSetting deviceId_setting_{ "relay.deviceId", "Relay Device ID", "" };
};
