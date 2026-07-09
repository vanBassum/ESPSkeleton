#include "WebSocketHandler.h"
#include "CommandManager.h"
#include "WebServerManager.h"
#include "JsonHelpers.h"
#include "MemoryStream.h"
#include "esp_log.h"

#include <algorithm>
#include <cstring>

static constexpr const char* TAG = "WebSocketHandler";

// The inbound frame-drain primitive (the private httpd_ws_get_frame_type wart)
// now lives in WsSessionLink::RecvChunk; Session::read() pulls streamed request
// bodies through it. See WsSessionLink.h.

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

    // Broadcast as a binary session chunk on the reserved broadcast session 0,
    // so the socket carries ONE uniform chunk format for replies and broadcasts
    // alike (no TEXT frames). Clients allocate session ids from 1, so 0 never
    // collides with a command.
    uint8_t buf[session::HEADER_LEN + 256];
    int cap = static_cast<int>(sizeof(buf) - session::HEADER_LEN);
    if (len > cap) len = cap;
    session::writeHeader(buf, session::BROADCAST_SESSION, session::FLAG_FINAL);
    memcpy(buf + session::HEADER_LEN, json, len);

    httpd_ws_frame_t frame = {};
    frame.type = HTTPD_WS_TYPE_BINARY;
    frame.payload = buf;
    frame.len = session::HEADER_LEN + len;

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

    if (frame.type == HTTPD_WS_TYPE_BINARY)
    {
        if (frame.len >= session::HEADER_LEN)
            self->HandleBinary(req, buf, frame.len);
        return ESP_OK;
    }

    // Inbound TEXT frames are no longer used: requests are binary session
    // chunks and no client sends text. Ignore any stray text frame.
    return ESP_OK;
}

// ──────────────────────────────────────────────────────────────
// Binary session transport. One inbound binary frame = one
// session chunk; step-1 requests are a single chunk dispatched synchronously.
// ──────────────────────────────────────────────────────────────

void WebSocketHandler::HandleBinary(httpd_req_t* req, const uint8_t* frame, size_t len)
{
    uint16_t sid   = session::readU16(frame);
    uint8_t  flags = frame[2];
    const uint8_t* payload = frame + session::HEADER_LEN;
    size_t plen = len - session::HEADER_LEN;

    WsSessionLink link(req, sendMutex_);
    SessionMux mux(link, *this, sessionFrame_, SESSION_WINDOW,
                   sessionInbound_, sizeof(sessionInbound_));
    mux.OnChunk(sid, flags, payload, plen);
}

void WebSocketHandler::OnSessionOpened(Session& session)
{
    // The request's first chunk carries the header line — {"type":"...",...args}
    // terminated by '\n' — followed (for a streamed command) by the body. Peek
    // it (without consuming) to route on "type"; the handler then reads the same
    // line for its own args and the body from the same session (in == out).
    const uint8_t* head = nullptr;
    size_t headLen = 0;
    session.peekRequest(head, headLen);

    char line[128];
    size_t n = std::min(headLen, sizeof(line) - 1);
    memcpy(line, head, n);
    line[n] = '\0';
    if (char* nl = strchr(line, '\n')) *nl = '\0';

    char type[32] = {};
    ExtractJsonString(line, "type", type, sizeof(type));

    if (type[0] == '\0')
    {
        session.reject("missing type");
        return;
    }

    if (!commandManager_ || !commandManager_->Execute(type, session, session))
    {
        session.reject(type);   // unknown command
        return;
    }
    session.finish();   // FINAL — end of reply
}
