#include "RelayManager.h"
#include "SettingsManager.h"
#include "NetworkManager.h"
#include "WebServerManager.h"
#include "CommandManager.h"
#include "AuthGate.h"
#include "Authenticator.h"

#include "SystemManager.h"

#include <esp_log.h>
#include <esp_mac.h>
#include <esp_app_desc.h>
#include <esp_random.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cstring>
#include <cstdio>
#include <cstdlib>

namespace {

inline uint32_t NowMs() { return pdTICKS_TO_MS(xTaskGetTickCount()); }

} // namespace

RelayManager::RelayManager(StruxProvider& strux)
    : strux_(strux)
{
}

void RelayManager::Init()
{
    auto initAttempt = initState_.TryBeginInit();
    if (!initAttempt)
    {
        ESP_LOGW(TAG, "Already initialized or initializing");
        return;
    }

    strux_.getSettingsManager().Register(
        { &enabled_, &url_, &deviceId_setting_, &token_setting_ });

    auth_ = &strux_.getWebServerManager().GetAuthenticator();

    if (!enabled_.Get())
    {
        ESP_LOGI(TAG, "Disabled (set relay.enabled to connect)");
        initAttempt.SetReady();
        return;
    }

    char url[128] = {};
    url_.Get(url, sizeof(url));
    if (url[0] == '\0')
    {
        ESP_LOGW(TAG, "relay.enabled is set but relay.url is empty — not connecting");
        initAttempt.SetReady();
        return;
    }

    ResolveDeviceId();
    ResolveToken();
    BuildUri();

    // Built once: RelaySocket keeps the pointer and re-sends these on every
    // reconnect, so a stack buffer here would be a dangling one.
    snprintf(headers_, sizeof(headers_), "X-Strux-Token: %s\r\n", token_);

    // Two IDF components narrate every single connect attempt, and this task
    // reconnects for the lifetime of the device. The certificate bundle announces each
    // successful validation at INFO, which is only news the first time. transport_ws
    // reports a refused upgrade as an ERROR about a missing handshake header, which
    // describes the symptom one layer below the cause — ReportConnectFailure() says
    // the same thing as "the relay refused this device", with the status and the id
    // needed to act on it. Raise either one when debugging the transport itself.
    esp_log_level_set("esp-x509-crt-bundle", ESP_LOG_WARN);
    esp_log_level_set("transport_ws", ESP_LOG_NONE);

    // One task connects, reads the socket, and runs the commands it reads. That is
    // the whole transport: there is no second task and no queue between them, because
    // the socket underneath is one this task reads rather than one that calls it back
    // (see RelaySocket). Reconnecting is part of the same loop.
    task_.Init("relay", 5, TASK_STACK);
    task_.SetHandler([this] { TaskLoop(); });
    if (!task_.Run())
    {
        ESP_LOGE(TAG, "Failed to start relay task");
        return;
    }

    ESP_LOGI(TAG, "Connecting to %s", uri_);
    initAttempt.SetReady();
}

void RelayManager::ResolveDeviceId()
{
    char configured[sizeof(deviceId_)] = {};
    deviceId_setting_.Get(configured, sizeof(configured));
    if (configured[0] != '\0')
    {
        strlcpy(deviceId_, configured, sizeof(deviceId_));
        return;
    }

    // No configured id → derive one from the MAC so a fresh device registers
    // without being told who it is. esp_read_mac works before WiFi starts.
    uint8_t mac[6] = {};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(deviceId_, sizeof(deviceId_), "esp32-%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

void RelayManager::ResolveToken()
{
    char stored[sizeof(token_)] = {};
    token_setting_.Get(stored, sizeof(stored));
    if (stored[0] != '\0')
    {
        strlcpy(token_, stored, sizeof(token_));
        return;
    }

    // First boot with the relay enabled: mint one and keep it. esp_fill_random is
    // the hardware RNG, seeded well enough for this once WiFi/BT is up — and it is,
    // because Init runs after NetworkManager.
    uint8_t raw[TOKEN_BYTES] = {};
    esp_fill_random(raw, sizeof(raw));
    for (size_t i = 0; i < TOKEN_BYTES; ++i)
        snprintf(token_ + i * 2, 3, "%02x", raw[i]);

    if (!token_setting_.Set(token_))
    {
        // Not fatal, but say so plainly: a token that is not stored is a new identity
        // on every boot, which means re-approving the device after every reboot.
        ESP_LOGE(TAG, "failed to store relay.token — it will change on reboot");
        return;
    }
    ESP_LOGI(TAG, "generated a relay token for this device");
}

// Percent-encodes everything that is not unreserved, which is the safe side of the
// line for a value going into a query string: device.name is set by a human and may
// hold spaces, '&' or '='.
static void AppendEncoded(char* out, size_t cap, const char* in)
{
    static const char* HEX = "0123456789abcdef";
    size_t n = strlen(out);
    for (; *in && n + 4 < cap; ++in)
    {
        const unsigned char c = static_cast<unsigned char>(*in);
        const bool safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                          (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                          c == '.' || c == '~';
        if (safe)
        {
            out[n++] = static_cast<char>(c);
        }
        else
        {
            out[n++] = '%';
            out[n++] = HEX[c >> 4];
            out[n++] = HEX[c & 0x0f];
        }
    }
    out[n] = '\0';
}

void RelayManager::BuildUri()
{
    char url[128] = {};
    url_.Get(url, sizeof(url));

    // Registration rides the connect URL's query string rather than a protocol
    // message: the server knows who connected before the first chunk, and the
    // session protocol gains no relay-specific verb. Firmware version travels with it
    // for the device list — NOT as a cache key: `www` is replaced independently of the
    // app, so the version does not describe the frontend (2026-08-11-11h19), and the
    // relay caches per connection instead, which needs nothing from here.
    //
    // Two identities go up here, and they are not the same thing. `id` is technical
    // and is what the token proves — it addresses the device in every URL. `name` and
    // `project` are for a human reading the relay's device list, and are display-only:
    // nothing is ever keyed on them, so a rename cannot cost a device its approval.
    const esp_app_desc_t* app = esp_app_get_description();
    const char* sep = strchr(url, '?') ? "&" : "?";
    snprintf(uri_, sizeof(uri_), "%s%sid=%s&fw=%s",
             url, sep, deviceId_, app ? app->version : "unknown");

    char name[48] = {};
    strux_.getSystemManager().GetDeviceName(name, sizeof(name));
    strlcat(uri_, "&name=", sizeof(uri_));
    AppendEncoded(uri_, sizeof(uri_), name);

    strlcat(uri_, "&project=", sizeof(uri_));
    AppendEncoded(uri_, sizeof(uri_), app ? app->project_name : "unknown");
}

// ──────────────────────────────────────────────────────────
// The pipe: connect, read, dispatch, repeat
// ──────────────────────────────────────────────────────────

void RelayManager::TaskLoop()
{
    uint32_t nextPingAt = 0;

    for (;;)
    {
        if (!socket_.IsConnected())
        {
            // Nothing to dial with yet. This task starts during Init, while WiFi is
            // still associating, so without this every boot spends a connect attempt
            // it cannot win and logs three ERROR lines from the TLS and transport
            // layers on the way out. Same check covers WiFi dropping later.
            if (!strux_.getNetworkManager().HasUpstream())
            {
                vTaskDelay(pdMS_TO_TICKS(NO_NETWORK_DELAY_MS));
                continue;
            }

            const auto result = socket_.Connect(uri_, CONNECT_TIMEOUT_MS, headers_);
            if (result == RelaySocket::ConnectResult::BadUri)
            {
                // uri_ is built once in Init and cannot change without a reboot, so
                // retrying a URL the parser already rejected would only reprint its
                // complaint forever. Stop the task instead; the settings UI is where
                // this gets fixed, and the fix takes effect on the next boot.
                ESP_LOGE(TAG, "relay.url is not usable — not retrying until reboot");
                return;
            }
            if (result != RelaySocket::ConnectResult::Ok)
            {
                vTaskDelay(pdMS_TO_TICKS(ReportConnectFailure(result)));
                continue;
            }

            if (suppressedFailures_ > 0)
                ESP_LOGI(TAG, "connected after %u further failed attempt%s",
                         static_cast<unsigned>(suppressedFailures_),
                         suppressedFailures_ == 1 ? "" : "s");
            lastFailure_        = RelaySocket::ConnectResult::Ok;
            lastFailureStatus_  = 0;
            suppressedFailures_ = 0;
            reconnectDelayMs_   = RECONNECT_DELAY_MS;

            OnConnected();
            // Right after connect, because on a wss:// pipe the TLS handshake just
            // ran on this stack and is one of the two things it has to fit.
            CheckStackHeadroom();
            nextPingAt = NowMs() + PING_INTERVAL_MS;
        }

        // Between requests this is where the task sits — blocked until a frame arrives
        // or the next keepalive comes due, whichever happens first. Mid-request the
        // same read happens under the session, one layer down (RelaySessionLink) —
        // same socket, same task, which is the property that removed the queue.
        int32_t untilPing = static_cast<int32_t>(nextPingAt - NowMs());
        if (untilPing < 0) untilPing = 0;

        const int n = socket_.ReadFrame(sessionInbound_, sizeof(sessionInbound_),
                                        untilPing);
        if (n < 0)
        {
            OnDisconnected();
            continue;
        }

        if (n == 0)
        {
            // The read ran its whole deadline out with nothing to show, which is what
            // being idle looks like — so the ping is due. A ping that will not go out
            // is how an otherwise idle pipe finds out its TCP connection is gone.
            nextPingAt = NowMs() + PING_INTERVAL_MS;
            if (!socket_.SendPing(PING_TIMEOUT_MS))
            {
                ESP_LOGW(TAG, "keepalive ping failed");
                OnDisconnected();
            }
            continue;
        }

        HandleFrame(sessionInbound_, static_cast<size_t>(n));

        // After the request rather than before it: a session that took a while has
        // just proven the pipe alive, and the next keepalive is owed from here.
        nextPingAt = NowMs() + PING_INTERVAL_MS;
    }
}

void RelayManager::OnConnected()
{
    // A reconnect is a fresh pipe: drop the old session state.
    conn_.reset();
    conn_.fd = -1;   // "slot in use" — there is no socket fd on this transport

    // Authentication belongs to the INTERFACE, and this one authenticates by
    // existing: the device dialled OUT, over TLS, to a URL its owner configured,
    // presenting a token it generated itself. That is proof of peer at the link
    // layer — the same basis as a bonded Bluetooth transport, which AuthGate
    // already describes as "a policy difference rather than a structural one" —
    // so the pipe is authed the moment it is up and never sees `auth`.
    //
    // This line used to read `!(auth_ && auth_->AuthRequired())`, which is the
    // WEB interface's policy. Setting web.password — a LAN concern — therefore
    // locked the relay out of `web read`, leaving the asset proxy unable to fetch
    // even the login page that would have unlocked it, and turning `ui modules`
    // into a refusal indistinguishable from old firmware.
    conn_.authed = true;

    skipping_ = false;
    linkUp_ = true;

    ESP_LOGI(TAG, "Connected as '%s'%s", deviceId_,
             conn_.authed ? " (no device password set — pipe is open)" : "");
}

int RelayManager::ReportConnectFailure(RelaySocket::ConnectResult result)
{
    // Log a REASON, not an attempt. This loop retries forever, so a line per attempt
    // is a line per attempt forever: a device left un-approved used to emit three of
    // them every five seconds — one here plus two from the TLS and websocket layers —
    // and a console that scrolls that is a console nobody reads. What is worth saying
    // is said once, when it changes, and the count of what went unsaid is reported by
    // the connect that eventually succeeds.
    const int status = socket_.LastHttpStatus();
    if (result == lastFailure_ && status == lastFailureStatus_)
    {
        suppressedFailures_++;
    }
    else
    {
        lastFailure_        = result;
        lastFailureStatus_  = status;
        suppressedFailures_ = 0;

        if (result == RelaySocket::ConnectResult::Refused && status == 403)
            ESP_LOGW(TAG, "relay refused this device — approve '%s' on the relay "
                          "server; retrying every %ds",
                     deviceId_, REFUSED_DELAY_MS / 1000);
        else if (result == RelaySocket::ConnectResult::Refused)
            ESP_LOGW(TAG, "relay refused the upgrade with HTTP %d", status);
        else
            ESP_LOGW(TAG, "cannot reach the relay at %s — retrying, backing off to %ds",
                     uri_, RECONNECT_DELAY_MAX_MS / 1000);
    }

    if (result != RelaySocket::ConnectResult::Unreachable)
        return REFUSED_DELAY_MS;

    // Double up to the cap, and hand back the delay this attempt earned.
    const int delay = reconnectDelayMs_;
    reconnectDelayMs_ = (reconnectDelayMs_ >= RECONNECT_DELAY_MAX_MS / 2)
                          ? RECONNECT_DELAY_MAX_MS
                          : reconnectDelayMs_ * 2;
    return delay;
}

void RelayManager::OnDisconnected()
{
    linkUp_ = false;
    skipping_ = false;
    socket_.Close();

    // Nothing to unblock: a handler waiting for its next chunk is waiting on a read
    // of this same socket, on this same task, so it has already returned by now.
    ESP_LOGW(TAG, "Disconnected — will retry");
    vTaskDelay(pdMS_TO_TICKS(RECONNECT_DELAY_MS));
}

void RelayManager::HandleFrame(const uint8_t* frame, size_t len)
{
    if (len < session::HEADER_LEN) return;

    uint16_t sid   = session::readU16(frame);
    uint8_t  flags = frame[2];
    const uint8_t* payload = frame + session::HEADER_LEN;
    size_t plen = len - session::HEADER_LEN;
    const bool final = (flags & session::FLAG_FINAL) != 0;

    // Residue: the tail of a request whose handler already returned. Read as a fresh
    // chunk it would be taken for a request header, and a command invented out of
    // firmware bytes. Skipped by id, so an unrelated session is never caught in it.
    if (skipping_)
    {
        if (sid == skipSid_)
        {
            if (final)
            {
                skipping_ = false;
                ESP_LOGW(TAG, "session %u: skipped to the end of an abandoned body",
                         static_cast<unsigned>(sid));
            }
            return;
        }
        skipping_ = false;   // a different session: whatever was left is behind us
    }

    // Identical to the local transport's frame path (WebSocketHandler::HandleBinary),
    // because everything above SessionLink is shared: the gate says what may run yet,
    // the chunk becomes a Session, and CommandManager runs the command.
    RelaySessionLink link(socket_);
    AuthGate gate(conn_, *auth_);

    Session s(sid, link, sessionFrame_, SESSION_WINDOW,
              sessionInbound_, sizeof(sessionInbound_));
    s.feedRequest(payload, plen, final);
    protocol::RunCommandSession(s, strux_.getCommandManager(), gate);

    // Returned without reaching FINAL — a refusal, or a handler that read less than
    // was sent. The rest is still coming down the socket.
    if (!s.requestEnded() && socket_.IsConnected())
    {
        skipSid_  = sid;
        skipping_ = true;
    }

    // The handler just ran on this stack; if it was the deepest one yet, say so.
    CheckStackHeadroom();
}

void RelayManager::CheckStackHeadroom()
{
    // ESP-IDF returns bytes here, not words as vanilla FreeRTOS does.
    const size_t free = uxTaskGetStackHighWaterMark(nullptr);
    if (free >= stackLow_) return;
    stackLow_ = free;

    // A quarter left is the point where the next slightly deeper handler, or a TLS
    // handshake against a server with a longer certificate chain, stops fitting.
    if (free < TASK_STACK / 4)
        ESP_LOGW(TAG, "stack headroom down to %u of %d bytes",
                 static_cast<unsigned>(free), TASK_STACK);
    else
        ESP_LOGI(TAG, "stack headroom %u of %d bytes",
                 static_cast<unsigned>(free), TASK_STACK);
}

// ──────────────────────────────────────────────────────────────
// Log fan-out
// ──────────────────────────────────────────────────────────────

void RelayManager::BroadcastLog(const char* json, int len)
{
    if (!linkUp_) return;

    // Same session-0 chunk the local socket broadcasts. This runs on the console
    // task, so it can collide with a session's reply on the relay task —
    // RelaySocket's send lock is what keeps a frame from being split in half.
    uint8_t buf[session::HEADER_LEN + 256];
    int cap = static_cast<int>(sizeof(buf) - session::HEADER_LEN);
    if (len > cap) len = cap;
    if (len < 0) return;

    session::writeHeader(buf, session::BROADCAST_SESSION, session::FLAG_FINAL);
    memcpy(buf + session::HEADER_LEN, json, static_cast<size_t>(len));

    // Short timeout and no logging on failure — this runs on the console
    // broadcast task, and a log line here would feed itself.
    socket_.SendBinary(buf, session::HEADER_LEN + static_cast<size_t>(len),
                       BROADCAST_TIMEOUT_MS);
}

bool RelayManager::BroadcastTelemetry(const char* line, int len)
{
    if (!linkUp_) return false;
    if (len <= 0) return false;

    // Sized for one line, which TelemetryManager already bounded. A point that does
    // not fit here would be a formatting bug there, so it is refused rather than cut.
    uint8_t buf[session::HEADER_LEN + 384];
    if (static_cast<size_t>(len) > sizeof(buf) - session::HEADER_LEN)
    {
        ESP_LOGW(TAG, "telemetry line too long (%d) — dropped", len);
        return false;
    }

    session::writeHeader(buf, session::TELEMETRY_SESSION, session::FLAG_FINAL);
    memcpy(buf + session::HEADER_LEN, line, static_cast<size_t>(len));

    // Same send lock as everything else on this socket: this is called from whichever
    // task took the measurement, which is not the relay task.
    return socket_.SendBinary(buf, session::HEADER_LEN + static_cast<size_t>(len),
                              BROADCAST_TIMEOUT_MS);
}
