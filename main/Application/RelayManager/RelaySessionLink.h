#pragma once

#include "SessionLink.h"
#include "SessionProtocol.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include <esp_websocket_client.h>
#include <cstdlib>
#include <cstring>

// One inbound chunk lifted off the WebSocket client's event callback. `data` is
// malloc'd by the producer and freed by whoever pops it. A null `data` is the
// disconnect sentinel: it unblocks a reader waiting on the queue so an in-flight
// session EOFs instead of hanging.
struct RelayFrame
{
    uint8_t* data;
    size_t   len;
};

// SessionLink over the outbound relay WebSocket (the second implementation — see
// SessionLink.h).
//
// The asymmetry that matters: esp_websocket_client is event-callback driven, so
// there is nothing to synchronously read the way httpd allows. Session::read()
// *pulls*, so RecvChunk blocks on a queue that the client's event handler fills.
// While a handler runs it owns that queue — the same task that would pop the next
// request is inside the handler — which is exactly the single-in-flight behaviour
// the local transport has.
//
// No send mutex here (unlike WsSessionLink): esp_websocket_client_send_bin takes
// the client's own lock and writes a whole frame, so a log broadcast from another
// task cannot split a session chunk.
class RelaySessionLink : public SessionLink
{
    static constexpr int SEND_TIMEOUT_MS = 5000;

    // A body chunk that never arrives must not wedge the task forever; EOF the
    // session instead and let the server retry.
    static constexpr int RECV_TIMEOUT_MS = 10000;

    esp_websocket_client_handle_t client_;
    QueueHandle_t queue_;

public:
    RelaySessionLink(esp_websocket_client_handle_t client, QueueHandle_t queue)
        : client_(client), queue_(queue) {}

    bool SendRaw(const uint8_t* frame, size_t len) override
    {
        if (!client_ || !esp_websocket_client_is_connected(client_)) return false;
        int sent = esp_websocket_client_send_bin(
            client_, reinterpret_cast<const char*>(frame), static_cast<int>(len),
            pdMS_TO_TICKS(SEND_TIMEOUT_MS));
        return sent == static_cast<int>(len);
    }

    int RecvChunk(uint8_t* buf, size_t cap, uint16_t* sid, uint8_t* flags) override
    {
        RelayFrame f{};
        if (xQueueReceive(queue_, &f, pdMS_TO_TICKS(RECV_TIMEOUT_MS)) != pdTRUE)
            return -1;                                  // timeout → EOF
        if (!f.data) return -1;                         // disconnect sentinel

        if (f.len < session::HEADER_LEN || f.len > cap)  // malformed / over-long
        {
            free(f.data);
            return -1;
        }

        memcpy(buf, f.data, f.len);
        free(f.data);

        *sid   = session::readU16(buf);
        *flags = buf[2];
        return static_cast<int>(f.len - session::HEADER_LEN);
    }
};
