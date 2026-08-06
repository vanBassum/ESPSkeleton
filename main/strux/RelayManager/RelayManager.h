#pragma once

#include "StruxServices.h"
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
// Open work: docs/backlog/2026-07-03-remote-access.md
// Why it reads its own socket: docs/reasoning/2026-08-05-13h55-owning-the-read-removes-the-buffer.md
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

    // 16 random bytes as hex. Long enough that guessing is not a strategy, short
    // enough to read off a screen when pairing a board by hand.
    static constexpr size_t TOKEN_BYTES   = 16;
    static constexpr size_t TOKEN_HEX_LEN = TOKEN_BYTES * 2;

    static constexpr int CONNECT_TIMEOUT_MS  = 10000;

    // Retry pacing, and two different kinds of waiting. An unreachable server is
    // usually transient — WiFi, DNS, a restart — so the first retry is quick and then
    // doubles, because the tenth attempt is no more likely than the ninth and costs a
    // TLS handshake. A refused upgrade is not transient at all: it waits on a person
    // approving this device, so it retries slowly, which is still often enough to keep
    // the device in the server's pending list and to pick up an approval promptly.
    static constexpr int RECONNECT_DELAY_MS     = 5000;
    static constexpr int RECONNECT_DELAY_MAX_MS = 60000;
    static constexpr int REFUSED_DELAY_MS       = 30000;

    // Short, because this is just waiting for WiFi to finish coming up.
    static constexpr int NO_NETWORK_DELAY_MS = 1000;

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
    explicit RelayManager(StruxServices& strux);

    RelayManager(const RelayManager&) = delete;
    RelayManager& operator=(const RelayManager&) = delete;
    RelayManager(RelayManager&&) = delete;
    RelayManager& operator=(RelayManager&&) = delete;

    void Init();

    /// Push a log line down the relay pipe as a session-0 broadcast chunk — the
    /// same shape the local socket sends, so a relayed frontend gets live logs
    /// with no protocol difference. No-op while disconnected or disabled.
    void BroadcastLog(const char* json, int len);

    /// Push one line of Influx line protocol on the telemetry session. Same mechanism
    /// as BroadcastLog and the same lack of guarantees — fire and forget, no reply — but
    /// a different reserved session, because the server sends these to a database
    /// rather than to every attached browser. False when it could not go out.
    bool BroadcastTelemetry(const char* line, int len);

    bool IsConnected() const { return linkUp_; }

    /// The id this device registers under. Telemetry tags points with it so a
    /// measurement says which board it came from.
    const char* GetDeviceId() const { return deviceId_; }

private:
    StruxServices& strux_;
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

    // Sized for the worst case BuildUri() can produce: url + id + fw version +
    // percent-encoded name and project name.
    char uri_[384] = {};
    char deviceId_[48] = {};

    // 32 hex + NUL. Proves to the server that this device is the id it claims.
    char token_[TOKEN_HEX_LEN + 1] = {};

    // The upgrade request's extra headers, built once in Init and pointed at by
    // RelaySocket on every reconnect — so it must outlive Connect().
    char headers_[96] = {};

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
    void ResolveToken();
    void TaskLoop();
    void HandleFrame(const uint8_t* frame, size_t len);

    /// Log this task's stack headroom when it reaches a new low. Called after every
    /// command, because a command handler runs on this task and is the deepest thing
    /// that ever will — as is, on a wss:// pipe, the TLS handshake.
    void CheckStackHeadroom();
    size_t stackLow_ = SIZE_MAX;

    void OnConnected();
    void OnDisconnected();

    /// Report a failed connect attempt and return how long to wait before the next
    /// one. Logs at most once per distinct reason — see the definition for why that
    /// matters more here than the usual "log every failure" instinct.
    int ReportConnectFailure(RelaySocket::ConnectResult result);

    // Connect-failure bookkeeping for the above: what went wrong last time, and how
    // many identical failures have gone unmentioned since.
    RelaySocket::ConnectResult lastFailure_ = RelaySocket::ConnectResult::Ok;
    int      lastFailureStatus_  = 0;
    uint32_t suppressedFailures_ = 0;
    int      reconnectDelayMs_   = RECONNECT_DELAY_MS;

    // ── Settings (registered with SettingsManager in Init) ──
    inline static BoolSetting   enabled_  { "relay.enabled",  "Relay Enabled",   false };
    inline static StringSetting url_      { "relay.url",      "Relay Server URL", "" };
    // Empty → derived from the WiFi MAC, so a fresh device registers without being
    // told who it is. The MAC is the *technical* identity; what a human reads in the
    // relay's device list is device.name, which travels alongside it for display only.
    // Set this only to pin an id that should outlive the board it started on.
    inline static StringSetting deviceId_setting_{ "relay.deviceId", "Relay Device ID", "" };

    // The device's proof that it is the id it claims. Empty → generated on first
    // Init and stored, so the secret is created here and never travels except
    // inside TLS. There is no server→device message that can set it.
    inline static StringSetting token_setting_{ "relay.token", "Relay Device Token", "" };
};
