#include "RelayManager.h"
#include "SettingsManager.h"
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
    sink_.SetCommandManager(serviceProvider_.getCommandManager());

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

    inbound_ = xQueueCreate(QUEUE_DEPTH, sizeof(RelayFrame));
    if (!inbound_)
    {
        ESP_LOGE(TAG, "Failed to create inbound queue");
        return;
    }

    esp_websocket_client_config_t cfg = {};
    cfg.uri = uri_;
    cfg.reconnect_timeout_ms = 5000;    // built-in auto-reconnect; no loop of ours
    cfg.network_timeout_ms = 10000;
    cfg.task_stack = 4096;              // the client's own task; ours is separate

    client_ = esp_websocket_client_init(&cfg);
    if (!client_)
    {
        ESP_LOGE(TAG, "Failed to init WebSocket client for %s", uri_);
        return;
    }

    esp_websocket_register_events(client_, WEBSOCKET_EVENT_ANY, EventHandler, this);

    // Command handlers must NOT run on the WebSocket client's task: a handler
    // blocked in Session::read() waits on the queue that only that task fills,
    // which would deadlock. So dispatch gets its own task, stack sized for the
    // heaviest handler.
    task_.Init("relay", 5, TASK_STACK);
    task_.SetHandler([this] { TaskLoop(); });
    if (!task_.Run())
    {
        ESP_LOGE(TAG, "Failed to start relay task");
        return;
    }

    esp_websocket_client_start(client_);

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

// ──────────────────────────────────────────────────────────────
// WebSocket client events (run on the client's own task)
// ──────────────────────────────────────────────────────────────

void RelayManager::EventHandler(void* ctx, esp_event_base_t, int32_t id, void* data)
{
    auto* self = static_cast<RelayManager*>(ctx);
    if (!self) return;

    switch (id)
    {
        case WEBSOCKET_EVENT_CONNECTED:    self->OnConnected();    break;
        case WEBSOCKET_EVENT_DISCONNECTED: self->OnDisconnected(); break;
        case WEBSOCKET_EVENT_DATA:         self->OnData(data);     break;
        case WEBSOCKET_EVENT_ERROR:
            ESP_LOGW(TAG, "WebSocket error");
            break;
        default:
            break;
    }
}

void RelayManager::OnConnected()
{
    DrainQueue();

    // A reconnect is a fresh pipe: drop the old auth state so a new remote user
    // must authenticate again.
    conn_.reset();
    conn_.fd = -1;   // "slot in use" — there is no socket fd on this transport
    conn_.authed = !(auth_ && auth_->AuthRequired());

    asmLen_ = 0;
    asmOverflow_ = false;
    linkUp_ = true;

    ESP_LOGI(TAG, "Connected as '%s'%s", deviceId_,
             conn_.authed ? " (no device password set — pipe is open)" : "");
}

void RelayManager::OnDisconnected()
{
    linkUp_ = false;

    // Unblock a handler waiting in Session::read() so its session EOFs instead of
    // hanging until the recv timeout.
    RelayFrame sentinel{ nullptr, 0 };
    if (inbound_) xQueueSend(inbound_, &sentinel, 0);

    ESP_LOGW(TAG, "Disconnected — will retry");
}

void RelayManager::OnData(const void* eventData)
{
    auto* d = static_cast<const esp_websocket_event_data_t*>(eventData);

    // op_code 0x02 = binary, 0x00 = continuation. Everything else (text, ping,
    // pong, close) is not a session chunk — the pipe carries binary only, same as
    // the local socket.
    if (d->op_code != 0x02 && d->op_code != 0x00) return;

    // A payload larger than the client's internal buffer arrives as several DATA
    // events with advancing payload_offset; offset 0 starts a new message.
    if (d->payload_offset == 0)
    {
        asmLen_ = 0;
        asmOverflow_ = false;
    }

    size_t end = static_cast<size_t>(d->payload_offset) + static_cast<size_t>(d->data_len);
    if (end > sizeof(asmBuf_))
    {
        if (!asmOverflow_)
            ESP_LOGW(TAG, "Inbound frame too large (%d bytes > %u), dropping",
                     d->payload_len, static_cast<unsigned>(sizeof(asmBuf_)));
        asmOverflow_ = true;
    }
    else if (d->data_len > 0)
    {
        memcpy(asmBuf_ + d->payload_offset, d->data_ptr, d->data_len);
    }
    asmLen_ = end;

    if (asmLen_ < static_cast<size_t>(d->payload_len)) return;   // more to come
    if (asmOverflow_ || asmLen_ < session::HEADER_LEN) return;

    // Hand the dispatch task its own copy — the event buffer is reused at once.
    auto* copy = static_cast<uint8_t*>(malloc(asmLen_));
    if (!copy)
    {
        ESP_LOGE(TAG, "Out of memory for a %u-byte chunk", static_cast<unsigned>(asmLen_));
        return;
    }
    memcpy(copy, asmBuf_, asmLen_);

    RelayFrame f{ copy, asmLen_ };
    if (xQueueSend(inbound_, &f, pdMS_TO_TICKS(500)) != pdTRUE)
    {
        ESP_LOGW(TAG, "Inbound queue full, dropped session %u",
                 session::readU16(copy));
        free(copy);
    }
}

void RelayManager::DrainQueue()
{
    if (!inbound_) return;
    RelayFrame f{};
    while (xQueueReceive(inbound_, &f, 0) == pdTRUE)
        free(f.data);   // free(nullptr) is a no-op, so sentinels are fine
}

// ──────────────────────────────────────────────────────────────
// Dispatch task
// ──────────────────────────────────────────────────────────────

void RelayManager::TaskLoop()
{
    for (;;)
    {
        RelayFrame f{};
        if (xQueueReceive(inbound_, &f, pdMS_TO_TICKS(1000)) != pdTRUE) continue;
        if (!f.data) continue;   // disconnect sentinel — nothing to dispatch

        HandleFrame(f.data, f.len);
        free(f.data);
    }
}

void RelayManager::HandleFrame(const uint8_t* frame, size_t len)
{
    if (len < session::HEADER_LEN) return;

    uint16_t sid   = session::readU16(frame);
    uint8_t  flags = frame[2];
    const uint8_t* payload = frame + session::HEADER_LEN;
    size_t plen = len - session::HEADER_LEN;

    // Identical to the local transport's frame path (WebSocketHandler::
    // HandleBinary) because everything above SessionLink is shared: the gate
    // handles hello/login/auth, the mux turns the chunk into a Session, and
    // CommandSink runs the command.
    RelaySessionLink link(client_, inbound_);
    AuthGate gate(*auth_);
    switch (gate.Handle(conn_, link, sid, payload, plen))
    {
        case AuthGate::Disposition::PassToMux:
        {
            SessionMux mux(link, sink_, sessionFrame_, SESSION_WINDOW,
                           sessionInbound_, sizeof(sessionInbound_));
            mux.OnChunk(sid, flags, payload, plen);
            break;
        }
        case AuthGate::Disposition::Handled:
        case AuthGate::Disposition::Rejected:
            break;
    }
}

// ──────────────────────────────────────────────────────────────
// Log fan-out
// ──────────────────────────────────────────────────────────────

void RelayManager::BroadcastLog(const char* json, int len)
{
    if (!client_ || !linkUp_) return;

    // Same session-0 chunk the local socket broadcasts. No send mutex needed:
    // esp_websocket_client_send_bin holds the client's lock for a whole frame, so
    // this cannot interleave with a session's chunks.
    uint8_t buf[session::HEADER_LEN + 256];
    int cap = static_cast<int>(sizeof(buf) - session::HEADER_LEN);
    if (len > cap) len = cap;
    if (len < 0) return;

    session::writeHeader(buf, session::BROADCAST_SESSION, session::FLAG_FINAL);
    memcpy(buf + session::HEADER_LEN, json, static_cast<size_t>(len));

    // Short timeout and no logging on failure — this runs on the console
    // broadcast task, and a log line here would feed itself.
    esp_websocket_client_send_bin(client_, reinterpret_cast<const char*>(buf),
                                  static_cast<int>(session::HEADER_LEN) + len,
                                  pdMS_TO_TICKS(200));
}
