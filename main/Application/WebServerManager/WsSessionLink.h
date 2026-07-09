#pragma once

#include "SessionProtocol.h"
#include "Mutex.h"
#include <esp_http_server.h>
#include <cstring>

// Transport link for the WebSocket: one session chunk == one WS binary frame
// ([session|flags|payload]). Step 1 sends synchronously on the httpd task using
// the live request; the shared send mutex serializes whole frames so a log
// broadcast can't split a chunk's bytes (chunks between frames may interleave
// harmlessly — the client routes by session id). Step 2 (async worker) will
// switch to httpd_ws_send_frame_async(server, fd).
class WsSessionLink
{
    static constexpr const char* TAG = "WsSessionLink";   // referenced by the LOCK macro

    httpd_req_t* req_;
    Mutex& sendMutex_;
    uint8_t frame_[session::HEADER_LEN + 4096];   // header + one chunk payload

public:
    static constexpr size_t MAX_PAYLOAD = 4096;

    WsSessionLink(httpd_req_t* req, Mutex& sendMutex) : req_(req), sendMutex_(sendMutex) {}

    bool SendChunk(uint16_t session, uint8_t flags, const void* payload, size_t len)
    {
        if (len > MAX_PAYLOAD) return false;
        session::writeHeader(frame_, session, flags);
        if (len) memcpy(frame_ + session::HEADER_LEN, payload, len);

        httpd_ws_frame_t f = {};
        f.type = HTTPD_WS_TYPE_BINARY;
        f.payload = frame_;
        f.len = session::HEADER_LEN + len;

        LOCK(sendMutex_);
        return httpd_ws_send_frame(req_, &f) == ESP_OK;
    }
};
