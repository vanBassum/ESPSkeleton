#pragma once

#include "Mutex.h"
#include "SessionProtocol.h"
#include <esp_http_server.h>
#include <cstdint>
#include <cstddef>

// ESP-IDF internal (declared in the private esp_httpd_priv.h, NOT the public
// esp_http_server.h): parses the next WS frame's first byte (FIN + opcode) into
// req->aux, which httpd_ws_recv_frame then consumes. httpd's own loop calls it
// once per handler invocation before dispatch, so to read frames *beyond* the
// first within a single handler call we must call it ourselves. This is a
// deliberate, documented wart — the price of draining an inbound stream on the
// httpd task without a worker; removed when the CommandManager worker task lands
// (docs/backlog/2026-07-03-multiplexed-channels.md). A future IDF dropping the
// symbol fails as a clean link error, not silent breakage.
extern "C" esp_err_t httpd_ws_get_frame_type(httpd_req_t* req);

// Transport link for the WebSocket. Outbound: sends one already-framed session
// chunk ([session|flags|payload], assembled by Session) as one WS binary frame.
// Inbound: pulls the NEXT WS binary frame off the socket (RecvChunk), so a
// streamed request body — arriving as several session chunks that share one id —
// can be drained within a single httpd handler call. The shared send mutex
// serializes whole outbound frames so a log broadcast can't split a chunk.
class WsSessionLink
{
    static constexpr const char* TAG = "WsSessionLink";   // referenced by the LOCK macro

    httpd_req_t* req_;
    Mutex& sendMutex_;

public:
    WsSessionLink(httpd_req_t* req, Mutex& sendMutex) : req_(req), sendMutex_(sendMutex) {}

    // `frame` is [session|flags|payload]; `len` is the total (header + payload).
    bool SendRaw(const uint8_t* frame, size_t len)
    {
        httpd_ws_frame_t f = {};
        f.type = HTTPD_WS_TYPE_BINARY;
        f.payload = const_cast<uint8_t*>(frame);
        f.len = len;

        LOCK(sendMutex_);
        return httpd_ws_send_frame(req_, &f) == ESP_OK;
    }

    // Receive the next inbound WS frame into `buf` (capacity `cap`) as a session
    // chunk. On success returns the payload length (>= 0), fills *sid / *flags
    // from the 3-byte header, and leaves the payload at buf + HEADER_LEN.
    // Returns -1 on error, an over-long frame, or a non-data frame (CLOSE/PING) —
    // the caller treats -1 as end-of-stream.
    int RecvChunk(uint8_t* buf, size_t cap, uint16_t* sid, uint8_t* flags)
    {
        if (httpd_ws_get_frame_type(req_) != ESP_OK) return -1;

        httpd_ws_frame_t f = {};                       // len==0 → header-only read
        if (httpd_ws_recv_frame(req_, &f, 0) != ESP_OK) return -1;

        if (f.type != HTTPD_WS_TYPE_BINARY && f.type != HTTPD_WS_TYPE_CONTINUE)
            return -1;                                 // CLOSE/PING/TEXT → end of stream
        if (f.len < session::HEADER_LEN || f.len > cap) return -1;

        f.payload = buf;
        if (httpd_ws_recv_frame(req_, &f, cap) != ESP_OK) return -1;

        *sid   = session::readU16(buf);
        *flags = buf[2];
        return static_cast<int>(f.len - session::HEADER_LEN);
    }
};
