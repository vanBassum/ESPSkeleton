#pragma once

#include "ServiceProvider.h"
#include "InitState.h"
#include "TypedSettings.h"
#include "Task.h"
#include "CommandEnvelope.h"
#include "SessionProtocol.h"
#include "WsConnection.h"
#include "RelaySocket.h"
#include "RelaySessionLink.h"

class Authenticator;

// The device's outbound connection to a relay server, so the device is reachable
// from outside its LAN without a VPN or a port forward.
//
// It is *only* a transport: a second SessionLink feeding the same Session type the
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

    // This task reads the socket AND runs the command, so its stack has to cover the
    // heaviest handler and, on a wss:// pipe, a TLS handshake — never at the same
    // time, so it is the larger of the two rather than the sum.
    static constexpr int TASK_STACK = 10240;

    static constexpr int CONNECT_TIMEOUT_MS  = 10000;
    static constexpr int RECONNECT_DELAY_MS  = 5000;

    // How long a read waits between requests. Also the tick of the keepalive below.
    static constexpr int IDLE_POLL_MS = 1000;

    // Ping after this many idle polls. Nothing on the pipe means nothing to notice a
    // dead TCP connection by, so we make traffic: a ping that cannot be sent is the
    // signal to reconnect.
    static constexpr int PING_EVERY_IDLE_POLLS = 30;
    static constexpr int PING_TIMEOUT_MS = 2000;

    // A log line is worth less than the task that emits it: never wait long.
    static constexpr int BROADCAST_TIMEOUT_MS = 200;

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

    RelaySocket socket_;
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
    //
    // sessionInbound_ is the ONLY inbound buffer. A frame is read straight into it
    // and the session's first chunk is read out of it in place; further chunks of
    // the same request refill it, which is safe because a session only ever refills
    // once the current chunk is drained. It used to be one of three — a reassembly
    // buffer, a heap copy per frame, and this — because a frame had to survive being
    // handed between two tasks. One task, one buffer.
    uint8_t sessionFrame_[session::HEADER_LEN + SESSION_WINDOW];
    uint8_t sessionInbound_[session::HEADER_LEN + INBOUND_WINDOW];

    // A session whose handler returned before the request's FINAL chunk leaves body
    // bytes on the wire. They are skipped by id rather than blindly discarded, which
    // is only possible now that this task does the reading.
    uint16_t skipSid_ = 0;
    bool     skipping_ = false;

    void BuildUri();
    void ResolveDeviceId();
    void TaskLoop();
    void HandleFrame(const uint8_t* frame, size_t len);

    void OnConnected();
    void OnDisconnected();

    // ── Settings (registered with SettingsManager in Init) ──
    inline static BoolSetting   enabled_  { "relay.enabled",  "Relay Enabled",   false };
    inline static StringSetting url_      { "relay.url",      "Relay Server URL", "" };
    // Empty → derived from the WiFi MAC, so a fresh device registers without
    // being told who it is.
    inline static StringSetting deviceId_setting_{ "relay.deviceId", "Relay Device ID", "" };
};
