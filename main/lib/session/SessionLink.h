#pragma once

#include <cstdint>
#include <cstddef>

// The transport seam under SessionMux: turn an already-framed session chunk into
// wire bytes, and hand inbound wire bytes back to the mux as chunks. This is the
// ONLY per-transport code — SessionMux, Session, and every command handler are
// written against this interface and are identical across transports.
//
// This header, SessionProtocol.h and SessionMux.h/.cpp are the whole of the
// session layer, and they depend on nothing but Stream. They live in lib/ rather
// than under a transport because the transports depend on them, not the reverse.
//
// Implementations, each owned by the manager that owns its transport:
//   WsSessionLink    — the local browser socket. One chunk = one WS binary frame;
//                      inbound frames are read synchronously on the httpd task.
//   RelaySessionLink — the outbound socket to the relay server.
//                      esp_websocket_client is event-callback driven, so its
//                      RecvChunk blocks on a queue the callback fills. See
//                      docs/superpowers/specs/2026-08-02-remote-access-relay-design.md.
//
// SendRaw takes a WHOLE pre-framed chunk rather than (session, flags, payload):
// Session assembles the 3-byte header and the payload into one external buffer,
// so a flush is a single send with no extra copy. A (session, flags, payload)
// signature would reintroduce that copy on every chunk — including every chunk
// of a multi-MB firmware image.
class SessionLink
{
public:
    virtual ~SessionLink() = default;

    // `frame` is [session|flags|payload]; `len` is the total (header + payload).
    virtual bool SendRaw(const uint8_t* frame, size_t len) = 0;

    // Receive the next inbound chunk into `buf` (capacity `cap`), blocking until
    // one is available. On success returns the payload length (>= 0), fills
    // *sid / *flags from the 3-byte header, and leaves the payload at
    // buf + HEADER_LEN. Returns -1 on error, an over-long chunk, or end of
    // stream — the caller treats -1 as EOF.
    virtual int RecvChunk(uint8_t* buf, size_t cap, uint16_t* sid, uint8_t* flags) = 0;
};
