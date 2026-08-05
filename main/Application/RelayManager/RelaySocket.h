#pragma once

#include "Mutex.h"
#include "ContextLock.h"
#include <esp_transport.h>
#include <cstdint>
#include <cstddef>

// A WebSocket the owner READS, rather than one that calls the owner back.
//
// This is the whole reason the relay can look like the browser socket. The
// high-level esp_websocket_client owns a task and delivers frames through a
// callback, and a callback cannot be the bottom of a streaming handler: the handler
// asks for its next chunk, that chunk is only read when the client's loop runs
// again, and the loop only runs once the callback returns — which it cannot,
// because the handler is inside it. A second task plus a queue of heap-copied
// frames was the way around that, and the queue is what dropped chunks under load.
//
// One floor down, the same handshake and framing are an ordinary blocking read. So
// this is just connect / ReadFrame / SendBinary, and one task does both halves of a
// request. What the client used to do for us and this now does: pick the URL apart,
// and report a dead link so the caller can reconnect. Control frames stay someone
// else's problem — configured not to propagate them, the transport answers a ping
// and completes a close inside the read we are already making.
class RelaySocket
{
    static constexpr const char* TAG = "RelaySocket";

public:
    RelaySocket() = default;
    ~RelaySocket() { Close(); }

    RelaySocket(const RelaySocket&) = delete;
    RelaySocket& operator=(const RelaySocket&) = delete;

    /// Open `ws://…` or `wss://…` and complete the upgrade. False leaves us closed.
    ///
    /// `extraHeaders` is raw `Key: Value\r\n` lines added to the upgrade request, or
    /// nullptr. It is NOT copied — the caller keeps it alive, because a reconnect
    /// re-sends it. This is how the relay presents its token: a header goes up with
    /// the handshake, so the server can refuse the upgrade outright and the session
    /// protocol above learns nothing about authentication.
    bool Connect(const char* uri, int timeoutMs, const char* extraHeaders = nullptr);
    void Close();

    bool IsConnected() const { return connected_; }

    /// The next data message, whole, into `buf`. Returns its length; 0 when nothing
    /// arrived within `timeoutMs`; -1 when the link is dead and wants reconnecting.
    /// Fragmented messages are stitched together — a message too large for `buf` is
    /// a misconfigured peer, so it closes the link rather than silently dropping a
    /// chunk out of somebody's firmware image.
    int ReadFrame(uint8_t* buf, size_t cap, int timeoutMs);

    /// One binary frame. Safe from any task: a session's reply and a log broadcast
    /// come from different ones, and a frame must not be split down the middle.
    bool SendBinary(const uint8_t* data, size_t len, int timeoutMs);

    /// A keepalive ping. Failing to send one is how a silently dead TCP connection
    /// gets noticed on an otherwise idle pipe — when to send one is the caller's
    /// policy, because that is a question about the relay, not about sockets.
    bool SendPing(int timeoutMs);

private:
    esp_transport_handle_t ws_     = nullptr;   // the layer we read and write
    esp_transport_handle_t parent_ = nullptr;   // tcp or ssl underneath it
    bool connected_ = false;

    Mutex sendMutex_;

    // Parsed out of the URI. The path carries the query string, which is where the
    // relay puts the device id — so it is not decoration.
    char host_[96]  = {};
    char path_[192] = {};
    int  port_ = 0;
    bool tls_  = false;

    bool ParseUri(const char* uri);
    void Destroy();
};
