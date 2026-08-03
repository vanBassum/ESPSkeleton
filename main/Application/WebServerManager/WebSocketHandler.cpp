#include "WebSocketHandler.h"
#include "CommandManager.h"
#include "Authenticator.h"
#include "AuthGate.h"
#include "WsSessionLink.h"   // the concrete SessionLink for this transport
#include "esp_log.h"
#include "esp_timer.h"

#include <cstring>

static constexpr const char* TAG = "WebSocketHandler";

// The inbound frame-drain primitive (the private httpd_ws_get_frame_type wart)
// now lives in WsSessionLink::RecvChunk; Session::read() pulls streamed request
// bodies through it. See WsSessionLink.h.

void WebSocketHandler::SetCommandManager(CommandManager& commandManager)
{
    commandManager_ = &commandManager;
}

void WebSocketHandler::SetAuth(Authenticator& auth)
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
    bool authed = !(auth_ && auth_->AuthRequired());   // empty password ⇒ authed at connect
    return registry_.add(fd, authed, esp_timer_get_time()) != nullptr;
}

void WebSocketHandler::RemoveWsClient(int fd)
{
    registry_.remove(fd);
}

void WebSocketHandler::TouchClient(int fd)
{
    if (auto* c = registry_.find(fd); c && c->authed)
        auth_->TouchKey(c->key);   // TouchKey locks its own table
}

void WebSocketHandler::OnClientDisconnected(int fd)
{
    RemoveWsClient(fd);
}

void WebSocketHandler::Broadcast(httpd_handle_t server, const char* json, int len)
{
    // Snapshot authed client fds under the registry lock, then send outside it.
    // Holding the lock across send would deadlock when a broadcaster source
    // (e.g. ConsoleManager) already holds its own mutex and httpd internals
    // call back into us.
    int clients[ConnectionRegistry::MAX];
    int count = 0;
    registry_.forEach([&](const WsConnection& c) {
        if (c.authed && count < ConnectionRegistry::MAX) clients[count++] = c.fd;
    });

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
    for (int i = 0; i < count; i++)
    {
        if (httpd_ws_send_frame_async(server, clients[i], &frame) != ESP_OK)
        {
            ESP_LOGW(TAG, "Broadcast failed to fd=%d, removing", clients[i]);
            registry_.remove(clients[i]);
        }
    }
}

void WebSocketHandler::BroadcastBinary(httpd_handle_t server, const uint8_t* data, size_t len)
{
    int clients[ConnectionRegistry::MAX];
    int count = 0;
    registry_.forEach([&](const WsConnection& c) {
        if (c.authed && count < ConnectionRegistry::MAX) clients[count++] = c.fd;
    });

    httpd_ws_frame_t frame = {};
    frame.type = HTTPD_WS_TYPE_BINARY;
    frame.payload = const_cast<uint8_t*>(data);
    frame.len = len;

    LOCK(sendMutex_);
    for (int i = 0; i < count; i++)
    {
        int fd = clients[i];

        if (httpd_ws_send_frame_async(server, fd, &frame) == ESP_OK)
        {
            if (auto* c = registry_.find(fd)) c->consecBinFails = 0;
            continue;
        }

        // Push failed — usually EAGAIN (TCP send buffer momentarily full) under
        // load. Don't remove the client on a single failure; the socket-close
        // callback cleans up real disconnects. Only after many consecutive
        // failures do we give up on this client.
        if (auto* c = registry_.find(fd))
        {
            if (++c->consecBinFails >= MAX_BIN_FAILS)
            {
                ESP_LOGW(TAG, "BroadcastBinary giving up on fd=%d after %d consecutive failures",
                         fd, c->consecBinFails);
                registry_.remove(fd);
            }
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
        // (see AuthGate). esp_http_server has already sent the 101; we
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

    // All inbound frames are processed single-threaded on the httpd task, so the
    // AuthGate below is the only writer of this connection's state (authed/key).
    // Broadcast runs on another task and may reset (remove) a slot concurrently,
    // but it only ever *clears* a slot — it never sets `authed` — so the auth gate
    // can't be defeated by that race, and a cleared slot reads as empty (self-
    // healing). If a worker task ever consumes these pointers (step 6), this needs
    // real locking (copy-under-lock, as the pre-refactor code did).
    WsConnection* conn = registry_.find(fd);
    if (!conn) return;   // unknown fd (closed mid-frame)

    WsSessionLink link(req, sendMutex_);
    AuthGate gate(*auth_);
    switch (gate.Handle(*conn, link, sid, payload, plen))
    {
        case AuthGate::Disposition::PassToMux:
        {
            SessionMux mux(link, *commandManager_, sessionFrame_, SESSION_WINDOW,
                           sessionInbound_, sizeof(sessionInbound_));
            mux.OnChunk(sid, flags, payload, plen);
            break;
        }
        case AuthGate::Disposition::Handled:
        case AuthGate::Disposition::Rejected:
            break;
    }
}
