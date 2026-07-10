#include "WebSocketHandler.h"
#include "CommandManager.h"
#include "WebServerManager.h"
#include "JsonHelpers.h"
#include "JsonWriter.h"
#include "BufferStream.h"
#include "MemoryStream.h"
#include "esp_log.h"
#include "esp_timer.h"

#include <algorithm>
#include <cstdio>
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

bool WebSocketHandler::AddWsClient(int fd)
{
    LOCK(wsMutex_);

    for (int i = 0; i < MAX_WS_CLIENTS; i++)
        if (wsClients_[i] == fd) return true;

    int64_t now = esp_timer_get_time();

    int slot = -1;
    for (int i = 0; i < MAX_WS_CLIENTS; i++)
        if (wsClients_[i] == 0) { slot = i; break; }

    if (slot < 0)   // full: reap a stale UN-authenticated squatter to make room
    {
        for (int i = 0; i < MAX_WS_CLIENTS; i++)
            if (!clientAuthed_[i] && now - clientConnectedAt_[i] > PRE_AUTH_TIMEOUT_US)
            { slot = i; break; }
    }
    if (slot < 0) { ESP_LOGW(TAG, "WS client rejected (table full): fd=%d", fd); return false; }

    wsClients_[slot]        = fd;
    clientAuthed_[slot]     = !(auth_ && auth_->AuthRequired());   // empty password ⇒ authed at connect
    clientConnectedAt_[slot] = now;
    clientTokens_[slot][0]  = 0;
    consecBinFails_[slot]   = 0;
    ESP_LOGI(TAG, "WS client added: fd=%d slot=%d authed=%d", fd, slot, (int)clientAuthed_[slot]);
    return true;
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
            clientAuthed_[i] = false;
            clientConnectedAt_[i] = 0;
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

bool WebSocketHandler::IsAuthed(int fd)
{
    LOCK(wsMutex_);
    for (int i = 0; i < MAX_WS_CLIENTS; i++)
        if (wsClients_[i] == fd) return clientAuthed_[i];
    return false;
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
    bool authed[MAX_WS_CLIENTS];

    {
        LOCK(wsMutex_);
        memcpy(clients, wsClients_, sizeof(clients));
        memcpy(authed, clientAuthed_, sizeof(authed));
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
        if (clients[i] != 0 && authed[i])
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
    bool authed[MAX_WS_CLIENTS];

    {
        LOCK(wsMutex_);
        memcpy(clients, wsClients_, sizeof(clients));
        memcpy(authed, clientAuthed_, sizeof(authed));
    }

    httpd_ws_frame_t frame = {};
    frame.type = HTTPD_WS_TYPE_BINARY;
    frame.payload = const_cast<uint8_t*>(data);
    frame.len = len;

    LOCK(sendMutex_);
    for (int i = 0; i < MAX_WS_CLIENTS; i++)
    {
        if (clients[i] == 0 || !authed[i]) continue;

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
        // The WS now opens UNAUTHENTICATED — auth is an in-band handshake
        // (see HandlePreAuth). esp_http_server has already sent the 101; we
        // only need a client slot. A full table (after reaping stale un-authed
        // sockets) refuses the upgrade so the client hits its reconnect loop.
        if (!self->AddWsClient(httpd_req_to_sockfd(req)))
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
    int fd = httpd_req_to_sockfd(req);

    // Peek the command type off the first chunk's header line. hello/login/auth
    // are transport handshake verbs the gate handles for ANY connection (an
    // empty-password connection is authed at connect but still needs hello, and
    // those verbs are not mux commands). Everything else needs an authenticated
    // connection and goes to the mux. Safe to peek here: HandleBinary only ever
    // sees a session's first chunk — a streamed body's continuation chunks are
    // pulled inside the handler via RecvChunk, never re-entering HandleBinary.
    char line[160];
    size_t n = std::min(plen, sizeof(line) - 1);
    memcpy(line, payload, n);
    line[n] = '\0';
    if (char* nl = strchr(line, '\n')) *nl = '\0';

    char type[16] = {};
    ExtractJsonString(line, "type", type, sizeof(type));

    if (strcmp(type, "hello") == 0 || strcmp(type, "login") == 0 || strcmp(type, "auth") == 0)
    {
        HandlePreAuth(req, fd, sid, line);
        return;
    }
    if (IsAuthed(fd))
    {
        WsSessionLink link(req, sendMutex_);
        SessionMux mux(link, *this, sessionFrame_, SESSION_WINDOW,
                       sessionInbound_, sizeof(sessionInbound_));
        mux.OnChunk(sid, flags, payload, plen);
        return;
    }
    // Unauthenticated + not a handshake verb → transport refusal.
    SendReject(req, sid, "unauthorized");
}

void WebSocketHandler::SetAuthed(int fd, const char* key)
{
    LOCK(wsMutex_);
    for (int i = 0; i < MAX_WS_CLIENTS; i++)
        if (wsClients_[i] == fd)
        {
            clientAuthed_[i] = true;
            strlcpy(clientTokens_[i], key ? key : "", sizeof(clientTokens_[i]));
            return;
        }
}

void WebSocketHandler::SendReply(httpd_req_t* req, uint16_t sid, const char* json)
{
    SendReplyN(req, sid, json, strlen(json));
}

void WebSocketHandler::SendReplyN(httpd_req_t* req, uint16_t sid, const void* data, size_t len)
{
    uint8_t buf[session::HEADER_LEN + 160];
    size_t n = len;
    if (n > sizeof(buf) - session::HEADER_LEN) n = sizeof(buf) - session::HEADER_LEN;
    session::writeHeader(buf, sid, session::FLAG_FINAL);
    memcpy(buf + session::HEADER_LEN, data, n);
    WsSessionLink link(req, sendMutex_);
    link.SendRaw(buf, session::HEADER_LEN + n);
}

void WebSocketHandler::SendReject(httpd_req_t* req, uint16_t sid, const char* reason)
{
    uint8_t buf[session::HEADER_LEN + 32];
    size_t n = strlen(reason);
    if (n > sizeof(buf) - session::HEADER_LEN) n = sizeof(buf) - session::HEADER_LEN;
    session::writeHeader(buf, sid, session::FLAG_REJECT);
    memcpy(buf + session::HEADER_LEN, reason, n);
    WsSessionLink link(req, sendMutex_);
    link.SendRaw(buf, session::HEADER_LEN + n);
}

void WebSocketHandler::HandlePreAuth(httpd_req_t* req, int fd, uint16_t sid, const char* line)
{
    // `line` is the request's header line, already NUL-terminated with the
    // trailing newline stripped by HandleBinary. HandleBinary only routes the
    // three handshake verbs here, for authed and unauthed connections alike.
    char type[16] = {};
    ExtractJsonString(line, "type", type, sizeof(type));

    if (strcmp(type, "hello") == 0)
    {
        char name[33] = {};
        auth_->GetDeviceName(name, sizeof(name));
        char body[96];
        BufferStream bs(body, sizeof(body));
        JsonWriter json(bs);
        json.beginObject();
        json.field("name", name);
        json.field("authRequired", auth_->AuthRequired());
        json.endObject();
        SendReplyN(req, sid, bs.data(), bs.length());
        return;
    }
    if (strcmp(type, "login") == 0)
    {
        char pw[64] = {};
        ExtractJsonString(line, "password", pw, sizeof(pw));
        if (auth_->CheckPassword(pw))
        {
            char key[SessionTable::TOKEN_LEN] = {};
            auth_->MintKey(key);
            SetAuthed(fd, key);
            char reply[64];
            snprintf(reply, sizeof(reply), "{\"ok\":true,\"key\":\"%s\"}", key);
            SendReply(req, sid, reply);
        }
        else SendReply(req, sid, "{\"ok\":false}");
        return;
    }
    if (strcmp(type, "auth") == 0)
    {
        char key[SessionTable::TOKEN_LEN] = {};
        ExtractJsonString(line, "key", key, sizeof(key));
        if (auth_->ValidateToken(key))
        {
            SetAuthed(fd, key);
            SendReply(req, sid, "{\"ok\":true}");
        }
        else SendReply(req, sid, "{\"ok\":false}");
        return;
    }
    // HandleBinary only routes the three verbs here; nothing else reaches this.
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
