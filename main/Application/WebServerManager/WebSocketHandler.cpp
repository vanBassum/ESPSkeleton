#include "WebSocketHandler.h"
#include "CommandManager.h"
#include "WebServerManager.h"
#include "JsonHelpers.h"
#include "MemoryStream.h"
#include "JsonWriter.h"
#include "esp_log.h"

#include <algorithm>
#include <cinttypes>
#include <cstring>

static constexpr const char* TAG = "WebSocketHandler";

// ESP-IDF internal (declared in the private esp_httpd_priv.h, NOT the public
// esp_http_server.h): parses the next WS frame's first byte (FIN + opcode) into
// req->aux, which httpd_ws_recv_frame then consumes. httpd's own loop calls it
// once per handler invocation before dispatch, so to read frames *beyond* the
// first within a single handler call we must call it ourselves. This is a
// deliberate, documented wart — the price of draining an inbound stream on the
// httpd task without a worker; see docs/backlog/2026-07-03-multiplexed-channels.md
// (removed when the CommandManager worker task lands). A future IDF dropping the
// symbol fails as a clean link error, not silent breakage.
extern "C" esp_err_t httpd_ws_get_frame_type(httpd_req_t* req);

namespace {

// Streams a single command reply over the WS, flushing each buffer-full as a
// WebSocket fragment instead of capping the reply at the buffer size. A reply
// that fits in one buffer is sent as one unfragmented TEXT frame — identical to
// the old fixed-buffer path; larger replies fragment (TEXT fin=0 → CONTINUE
// fin=0 … → CONTINUE fin=1). The caller must hold sendMutex_ for the whole
// message so a broadcast can't interleave between fragments.
class WsResponseStream : public Stream
{
    httpd_req_t* req_;
    char* buf_;
    size_t cap_;
    size_t len_ = 0;
    bool firstSent_ = false;   // at least one fragment already on the wire
    bool failed_ = false;

    void sendFrame(bool final)
    {
        httpd_ws_frame_t frame = {};
        frame.payload = reinterpret_cast<uint8_t*>(buf_);
        frame.len = len_;
        if (!firstSent_ && final)
        {
            // Whole reply fit in one buffer: unfragmented TEXT, as before.
            frame.type = HTTPD_WS_TYPE_TEXT;
        }
        else
        {
            frame.type = firstSent_ ? HTTPD_WS_TYPE_CONTINUE : HTTPD_WS_TYPE_TEXT;
            frame.fragmented = true;
            frame.final = final;
        }
        if (!failed_ && httpd_ws_send_frame(req_, &frame) != ESP_OK)
            failed_ = true;
        firstSent_ = true;
        len_ = 0;
    }

public:
    WsResponseStream(httpd_req_t* req, char* buf, size_t cap)
        : req_(req), buf_(buf), cap_(cap) {}

    size_t write(const void* data, size_t size, TickType_t timeout = portMAX_DELAY) override
    {
        (void)timeout;
        const char* p = static_cast<const char*>(data);
        size_t remaining = size;
        while (remaining > 0)
        {
            size_t n = std::min(cap_ - len_, remaining);
            memcpy(buf_ + len_, p, n);
            len_ += n;
            p += n;
            remaining -= n;
            if (len_ == cap_)          // buffer full → flush a non-final fragment
                sendFrame(false);
        }
        return size;
    }

    size_t read(void*, size_t, TickType_t = portMAX_DELAY) override { return 0; }

    // Discard buffered bytes. Only valid before anything has been flushed —
    // used on the unknown-command path, where Execute wrote nothing.
    void reset() { len_ = 0; }

    // Emit the final frame, closing the (possibly fragmented) message.
    void finish() { sendFrame(true); }

    bool failed() const { return failed_; }
};

// Feeds a command's `Stream& in` from WS frames that arrive AFTER the control
// frame, drained synchronously on the httpd task. The body is one fragmented
// BINARY message: successive BINARY/CONTINUE frames until the frame whose FIN
// bit is set, which marks end-of-body (read() then returns 0). One streamed
// request owns the socket for its whole duration — head-of-line blocking,
// accepted until multiplexing. Each body frame must fit CHUNK bytes; a larger
// frame is a protocol error. Control frames (CLOSE/PING/TEXT) arriving mid-body
// are treated as end-of-body — the controlled browser client never sends them
// mid-stream.
//
// NOTE: not yet wired into dispatch — that arrives with its first consumer
// (firmware upload, docs/backlog/2026-07-09-firmware-upload-over-websocket.md),
// which also defines how a request is flagged as streaming. Unexercised until
// then.
class [[maybe_unused]] WsRequestStream : public Stream
{
    static constexpr size_t CHUNK = 4096;

    httpd_req_t* req_;
    uint8_t buf_[CHUNK];        // current frame's unmasked payload
    size_t len_ = 0;            // valid bytes in buf_
    size_t pos_ = 0;            // bytes already handed to the caller
    bool finalSeen_ = false;    // FIN frame delivered → no more body
    bool failed_ = false;

    // Pull the next body frame into buf_. False at end-of-body or on error.
    bool pullFrame()
    {
        if (finalSeen_ || failed_) return false;

        // Parse the next frame's first byte into req->aux, then read its header.
        if (httpd_ws_get_frame_type(req_) != ESP_OK) { failed_ = true; return false; }

        httpd_ws_frame_t f = {};                       // len==0 → header-only read
        if (httpd_ws_recv_frame(req_, &f, 0) != ESP_OK) { failed_ = true; return false; }

        if (f.type != HTTPD_WS_TYPE_BINARY && f.type != HTTPD_WS_TYPE_CONTINUE)
        {
            finalSeen_ = true;                         // CLOSE/PING/TEXT ends the body
            return false;
        }
        if (f.len > CHUNK) { failed_ = true; return false; }

        len_ = f.len;
        pos_ = 0;
        if (f.len > 0)
        {
            f.payload = buf_;                          // f.len set → this read grabs the payload
            if (httpd_ws_recv_frame(req_, &f, CHUNK) != ESP_OK) { failed_ = true; return false; }
        }
        if (f.final) finalSeen_ = true;                // last fragment consumed
        return true;
    }

public:
    explicit WsRequestStream(httpd_req_t* req) : req_(req) {}

    size_t read(void* dst, size_t size, TickType_t timeout = portMAX_DELAY) override
    {
        (void)timeout;
        while (pos_ >= len_)                           // skip past any empty frames
        {
            if (!pullFrame()) return 0;                // EOF or error
        }
        size_t n = std::min(size, len_ - pos_);
        memcpy(dst, buf_ + pos_, n);
        pos_ += n;
        return n;
    }

    size_t write(const void*, size_t, TickType_t = portMAX_DELAY) override { return 0; }

    bool failed() const { return failed_; }
};

} // namespace

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

    if (frame.type == HTTPD_WS_TYPE_BINARY)
    {
        if (frame.len >= session::HEADER_LEN)
            self->HandleBinary(req, buf, frame.len);
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
    // Hold sendMutex_ for the whole (possibly fragmented) reply: the handler
    // streams fragments through `out` as it runs, and a log broadcast on the
    // ConsoleBroadcast task must not interleave its own frame between ours.
    LOCK(sendMutex_);

    WsResponseStream out(req, wsBuf_, sizeof(wsBuf_));

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
        // Unknown command: Execute wrote nothing and the head is still buffered
        // (unflushed), so we can still turn the reply into an error object.
        out.reset();
        JsonWriter err(out);   // reuse JsonWriter's escaping for the type echo
        err.beginObject();
        err.field("id", id);
        err.field("error", type);
        err.endObject();
    }

    out.finish();
}

// ──────────────────────────────────────────────────────────────
// Binary session transport (new path). One inbound binary frame = one
// session chunk; step-1 requests are a single chunk dispatched synchronously.
// ──────────────────────────────────────────────────────────────

void WebSocketHandler::HandleBinary(httpd_req_t* req, const uint8_t* frame, size_t len)
{
    uint16_t sid   = session::readU16(frame);
    uint8_t  flags = frame[2];
    const uint8_t* payload = frame + session::HEADER_LEN;
    size_t plen = len - session::HEADER_LEN;

    WsSessionLink link(req, sendMutex_);
    SessionMux mux(link, *this, sessionFrame_, SESSION_WINDOW);
    mux.OnChunk(sid, flags, payload, plen);
}

void WebSocketHandler::OnSessionOpened(Session& session)
{
    // Read the request envelope (single chunk in step 1). The header line is
    // the whole envelope JSON: {"type":"...",...args}. A trailing '\n' (added
    // by the client for step-2 forward-compat) is stripped; anything after it
    // would be the body (none in step 1).
    char envelope[512];
    size_t n = session.read(envelope, sizeof(envelope) - 1);
    envelope[n] = '\0';
    if (char* nl = strchr(envelope, '\n')) *nl = '\0';

    char type[32] = {};
    ExtractJsonString(envelope, "type", type, sizeof(type));

    if (type[0] == '\0')
    {
        session.reject("missing type");
        return;
    }

    MemoryStream in(envelope, strlen(envelope));   // envelope = args (as today)
    if (!commandManager_ || !commandManager_->Execute(type, in, session))
    {
        session.reject(type);   // unknown command
        return;
    }
    session.finish();   // FINAL — end of reply
}
