#pragma once

#include "Mutex.h"
#include <esp_http_server.h>
#include <cstdint>
#include <cstddef>

// Transport link for the WebSocket: sends one already-framed session chunk
// ([session|flags|payload], assembled by Session) as one WS binary frame. The
// shared send mutex serializes whole frames so a log broadcast can't split a
// chunk's bytes (chunks between frames may interleave harmlessly — the client
// routes by session id). Holds no buffer of its own; step 2 (async worker) will
// switch to httpd_ws_send_frame_async(server, fd).
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
};
