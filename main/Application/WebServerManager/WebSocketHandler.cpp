#include "WebSocketHandler.h"
#include "CommandManager.h"
#include "WebServerManager.h"
#include "JsonHelpers.h"
#include "BufferStream.h"
#include "MemoryStream.h"
#include "JsonWriter.h"
#include "esp_log.h"

#include <cinttypes>
#include <cstring>

static constexpr const char* TAG = "WebSocketHandler";

void WebSocketHandler::SetCommandManager(CommandManager& commandManager)
{
    commandManager_ = &commandManager;
}

void WebSocketHandler::SetAuth(WebServerManager& auth)
{
    auth_ = &auth;
}

void WebSocketHandler::RegisterRoute(httpd_handle_t server)
{
    const httpd_uri_t ws_route = {
        .uri = "/ws",
        .method = HTTP_GET,
        .handler = HandleWs,
        .user_ctx = this,
        .is_websocket = true,
        .handle_ws_control_frames = false,
        .supported_subprotocol = nullptr,
    };
    httpd_register_uri_handler(server, &ws_route);
}

// ──────────────────────────────────────────────────────────────
// Client tracking
// ──────────────────────────────────────────────────────────────

bool WebSocketHandler::AddWsClient(int fd, const char* token)
{
    LOCK(wsMutex_);

    for (int i = 0; i < MAX_WS_CLIENTS; i++)
    {
        if (wsClients_[i] == fd) return true;
    }

    for (int i = 0; i < MAX_WS_CLIENTS; i++)
    {
        if (wsClients_[i] == 0)
        {
            wsClients_[i] = fd;
            strlcpy(clientTokens_[i], token, sizeof(clientTokens_[i]));
            ESP_LOGI(TAG, "WS client added: fd=%d slot=%d", fd, i);
            return true;
        }
    }
    ESP_LOGW(TAG, "WS client rejected (max reached): fd=%d", fd);
    return false;
}

void WebSocketHandler::RemoveWsClient(int fd)
{
    LOCK(wsMutex_);
    for (int i = 0; i < MAX_WS_CLIENTS; i++)
    {
        if (wsClients_[i] == fd)
        {
            wsClients_[i] = 0;
            consecBinFails_[i] = 0;
            clientTokens_[i][0] = 0;
            ESP_LOGI(TAG, "WS client removed: fd=%d slot=%d", fd, i);
            return;
        }
    }
}

void WebSocketHandler::TouchClient(int fd)
{
    char token[SessionTable::TOKEN_LEN] = {};
    {
        LOCK(wsMutex_);
        for (int i = 0; i < MAX_WS_CLIENTS; i++)
        {
            if (wsClients_[i] == fd)
            {
                strlcpy(token, clientTokens_[i], sizeof(token));
                break;
            }
        }
    }
    if (token[0] != 0 && auth_)
        auth_->TouchSession(token);   // outside wsMutex_ — TouchSession locks its own table
}

void WebSocketHandler::OnClientDisconnected(int fd)
{
    RemoveWsClient(fd);
}

void WebSocketHandler::Broadcast(httpd_handle_t server, const char* json, int len)
{
    // Snapshot clients under lock, then send outside the lock. Holding wsMutex_
    // across send would deadlock when a broadcaster source (e.g. ConsoleManager)
    // already holds its own mutex and httpd internals call back into us.
    int clients[MAX_WS_CLIENTS];

    {
        LOCK(wsMutex_);
        memcpy(clients, wsClients_, sizeof(clients));
    }

    httpd_ws_frame_t frame = {};
    frame.type = HTTPD_WS_TYPE_TEXT;
    frame.payload = reinterpret_cast<uint8_t*>(const_cast<char*>(json));
    frame.len = len;

    LOCK(sendMutex_);
    for (int i = 0; i < MAX_WS_CLIENTS; i++)
    {
        if (clients[i] != 0)
        {
            if (httpd_ws_send_frame_async(server, clients[i], &frame) != ESP_OK)
            {
                ESP_LOGW(TAG, "Broadcast failed to fd=%d, removing", clients[i]);
                LOCK(wsMutex_);
                wsClients_[i] = 0;
            }
        }
    }
}

void WebSocketHandler::BroadcastBinary(httpd_handle_t server, const uint8_t* data, size_t len)
{
    int clients[MAX_WS_CLIENTS];

    {
        LOCK(wsMutex_);
        memcpy(clients, wsClients_, sizeof(clients));
    }

    httpd_ws_frame_t frame = {};
    frame.type = HTTPD_WS_TYPE_BINARY;
    frame.payload = const_cast<uint8_t*>(data);
    frame.len = len;

    LOCK(sendMutex_);
    for (int i = 0; i < MAX_WS_CLIENTS; i++)
    {
        if (clients[i] == 0) continue;

        if (httpd_ws_send_frame_async(server, clients[i], &frame) == ESP_OK)
        {
            LOCK(wsMutex_);
            consecBinFails_[i] = 0;
            continue;
        }

        // Push failed — usually EAGAIN (TCP send buffer momentarily full) under
        // load. Don't remove the client on a single failure; the socket-close
        // callback cleans up real disconnects. Only after many consecutive
        // failures do we give up on this client.
        LOCK(wsMutex_);
        if (++consecBinFails_[i] >= MAX_BIN_FAILS)
        {
            ESP_LOGW(TAG, "BroadcastBinary giving up on fd=%d after %d consecutive failures",
                     clients[i], consecBinFails_[i]);
            wsClients_[i] = 0;
            consecBinFails_[i] = 0;
        }
    }
}

// ──────────────────────────────────────────────────────────────
// WebSocket frame handling
// ──────────────────────────────────────────────────────────────

esp_err_t WebSocketHandler::HandleWs(httpd_req_t* req)
{
    auto* self = static_cast<WebSocketHandler*>(req->user_ctx);

    if (req->method == HTTP_GET)
    {
        // Auth happens HERE, once. esp_http_server has already sent the
        // 101 handshake before invoking us; returning ESP_FAIL makes
        // httpd close the socket immediately, which is how an upgrade
        // is "refused". The frontend can't read a close reason — it
        // discriminates bad-token from network failure via an HTTP
        // ping before connecting (see backend.ts).
        char query[96] = {};
        char token[SessionTable::TOKEN_LEN] = {};
        if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
            httpd_query_key_value(query, "token", token, sizeof(token)) != ESP_OK ||
            !self->auth_ || !self->auth_->ValidateToken(token))
        {
            ESP_LOGW(TAG, "WS upgrade refused: missing/invalid token");
            return ESP_FAIL;
        }
        // A client beyond the table would stay open but untracked: no
        // broadcasts, no session refresh — a half-alive tab that GCs
        // after 30 min. Refuse instead so it hits the reconnect loop.
        if (!self->AddWsClient(httpd_req_to_sockfd(req), token))
            return ESP_FAIL;
        return ESP_OK;
    }

    httpd_ws_frame_t frame = {};
    uint8_t buf[512] = {};
    frame.payload = buf;
    esp_err_t ret = httpd_ws_recv_frame(req, &frame, sizeof(buf) - 1);
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "WS recv failed: %s", esp_err_to_name(ret));
        self->RemoveWsClient(httpd_req_to_sockfd(req));
        return ret;
    }

    // Any inbound frame (heartbeat included) keeps the session alive —
    // an open tab never logs out; see spec.
    self->TouchClient(httpd_req_to_sockfd(req));

    if (frame.type == HTTPD_WS_TYPE_CLOSE)
    {
        self->RemoveWsClient(httpd_req_to_sockfd(req));
        return ESP_OK;
    }

    if (frame.type != HTTPD_WS_TYPE_TEXT || frame.len == 0)
        return ESP_OK;

    buf[frame.len] = '\0';
    const char* json = reinterpret_cast<const char*>(buf);

    int32_t id = ExtractJsonInt(json, "id");
    char type[32] = {};
    ExtractJsonString(json, "type", type, sizeof(type));

    if (id <= 0 || type[0] == '\0')
        return ESP_OK;

    self->DispatchMessage(req, id, type, json);
    return ESP_OK;
}

void WebSocketHandler::DispatchMessage(httpd_req_t* req, int32_t id, const char* type, const char* json)
{
    BufferStream out(wsBuf_, sizeof(wsBuf_));

    // Envelope by concatenation: the handler writes one complete JSON
    // object into `out`; we wrap it as {"id":N,"payload":<object>}.
    char head[48];
    int n = snprintf(head, sizeof(head), "{\"id\":%" PRId32 ",\"payload\":", id);
    out.write(head, n);

    MemoryStream in(json, strlen(json));

    if (commandManager_ && commandManager_->Execute(type, in, out))
    {
        out.write("}", 1);
    }
    else
    {
        out.reset();
        JsonWriter err(out);   // reuse JsonWriter's escaping for the type echo
        err.beginObject();
        err.field("id", id);
        err.field("error", type);
        err.endObject();
    }

    httpd_ws_frame_t frame = {};
    frame.type = HTTPD_WS_TYPE_TEXT;
    frame.payload = reinterpret_cast<uint8_t*>(const_cast<char*>(out.data()));
    frame.len = out.length();

    LOCK(sendMutex_);
    httpd_ws_send_frame(req, &frame);
}
