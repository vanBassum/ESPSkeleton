#include "RelayManager.h"
#include "SettingsManager.h"
#include "NetworkManager.h"
#include "WebServerManager.h"
#include "CommandManager.h"
#include "AuthGate.h"
#include "Authenticator.h"

#include <esp_log.h>
#include <esp_mac.h>
#include <esp_app_desc.h>
#include <cstring>
#include <cstdio>
#include <cstdlib>

RelayManager::RelayManager(ServiceProvider& serviceProvider)
    : serviceProvider_(serviceProvider)
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

    serviceProvider_.getSettingsManager().Register({ &enabled_, &url_, &deviceId_setting_ });

    auth_ = &serviceProvider_.getWebServerManager().GetAuthenticator();

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
    BuildUri();

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

void RelayManager::BuildUri()
{
    char url[128] = {};
    url_.Get(url, sizeof(url));

    // Registration rides the connect URL's query string rather than a protocol
    // message: the server knows who connected before the first chunk, and the
    // session protocol gains no relay-specific verb. Firmware version travels with
    // it so the server can key a file cache on it later.
    const esp_app_desc_t* app = esp_app_get_description();
    const char* sep = strchr(url, '?') ? "&" : "?";
    snprintf(uri_, sizeof(uri_), "%s%sid=%s&fw=%s",
             url, sep, deviceId_, app ? app->version : "unknown");
}

// ──────────────────────────────────────────────────────────
// The pipe: connect, read, dispatch, repeat
// ──────────────────────────────────────────────────────────

void RelayManager::TaskLoop()
{
    int idlePolls = 0;

    for (;;)
    {
        if (!socket_.IsConnected())
        {
            // Nothing to dial with yet. This task starts during Init, while WiFi is
            // still associating, so without this every boot spends a connect attempt
            // it cannot win and logs three ERROR lines from the TLS and transport
            // layers on the way out. Same check covers WiFi dropping later.
            if (!serviceProvider_.getNetworkManager().HasIpv4())
            {
                vTaskDelay(pdMS_TO_TICKS(NO_NETWORK_DELAY_MS));
                continue;
            }

            if (!socket_.Connect(uri_, CONNECT_TIMEOUT_MS))
            {
                vTaskDelay(pdMS_TO_TICKS(RECONNECT_DELAY_MS));
                continue;
            }
            OnConnected();
            idlePolls = 0;
        }

        // Between requests this is where the task sits. Mid-request the same read
        // happens under the session, one layer down (RelaySessionLink) — same socket,
        // same task, which is the property that removed the queue.
        const int n = socket_.ReadFrame(sessionInbound_, sizeof(sessionInbound_),
                                       IDLE_POLL_MS);
        if (n < 0)
        {
            OnDisconnected();
            continue;
        }

        if (n == 0)
        {
            // Silence. Make some traffic occasionally: a ping that will not go out is
            // how an otherwise idle pipe finds out its TCP connection is gone.
            if (++idlePolls >= PING_EVERY_IDLE_POLLS)
            {
                idlePolls = 0;
                if (!socket_.SendPing(PING_TIMEOUT_MS))
                {
                    ESP_LOGW(TAG, "keepalive ping failed");
                    OnDisconnected();
                }
            }
            continue;
        }

        idlePolls = 0;
        HandleFrame(sessionInbound_, static_cast<size_t>(n));
    }
}

void RelayManager::OnConnected()
{
    // A reconnect is a fresh pipe: drop the old auth state so a new remote user
    // must authenticate again.
    conn_.reset();
    conn_.fd = -1;   // "slot in use" — there is no socket fd on this transport
    conn_.authed = !(auth_ && auth_->AuthRequired());

    skipping_ = false;
    linkUp_ = true;

    ESP_LOGI(TAG, "Connected as '%s'%s", deviceId_,
             conn_.authed ? " (no device password set — pipe is open)" : "");
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
    protocol::RunCommandSession(s, serviceProvider_.getCommandManager(), gate);

    // Returned without reaching FINAL — a refusal, or a handler that read less than
    // was sent. The rest is still coming down the socket.
    if (!s.requestEnded() && socket_.IsConnected())
    {
        skipSid_  = sid;
        skipping_ = true;
    }
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
