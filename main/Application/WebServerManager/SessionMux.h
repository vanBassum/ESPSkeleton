#pragma once

#include "Stream.h"
#include "WsSessionLink.h"
#include "SessionProtocol.h"
#include <cstdint>
#include <cstddef>
#include <algorithm>
#include <cstring>

// A session's stream. read() = the request bytes fed by the mux; write() = the
// reply, accumulated and flushed as binary DATA chunks, closed by finish() with
// FLAG_FINAL. The reply is assembled directly into an EXTERNAL framing buffer
// (owned by the transport, off the httpd-task stack): payload goes after the
// 3-byte header slot, so a flush is one SendRaw with no extra copy. Step 1: the
// request is a single chunk fed up front (no blocking); step 2 will make read()
// pull further inbound chunks for large bodies.
class Session : public Stream
{
    uint16_t id_;
    WsSessionLink& link_;

    const uint8_t* req_ = nullptr;   // request bytes (borrowed; valid during dispatch)
    size_t reqLen_ = 0;
    size_t reqPos_ = 0;

    uint8_t* buf_;        // external [ header | payload ] buffer
    size_t   cap_;        // payload capacity (buf_ size minus HEADER_LEN)
    size_t   outLen_ = 0; // payload bytes buffered so far
    bool     failed_ = false;

    // Frame [id|flags|payload] into buf_ and send it; reset the payload cursor.
    void emitChunk(uint8_t flags)
    {
        session::writeHeader(buf_, id_, flags);
        if (!link_.SendRaw(buf_, session::HEADER_LEN + outLen_)) failed_ = true;
        outLen_ = 0;
    }

public:
    Session(uint16_t id, WsSessionLink& link, uint8_t* buf, size_t payloadCap)
        : id_(id), link_(link), buf_(buf), cap_(payloadCap) {}

    void feedRequest(const uint8_t* data, size_t len) { req_ = data; reqLen_ = len; reqPos_ = 0; }

    size_t read(void* dst, size_t size, TickType_t timeout = portMAX_DELAY) override
    {
        (void)timeout;
        size_t n = std::min(size, reqLen_ - reqPos_);
        if (n) { memcpy(dst, req_ + reqPos_, n); reqPos_ += n; }
        return n;
    }

    size_t write(const void* data, size_t size, TickType_t timeout = portMAX_DELAY) override
    {
        (void)timeout;
        const uint8_t* p = static_cast<const uint8_t*>(data);
        size_t remaining = size;
        while (remaining > 0)
        {
            size_t n = std::min(cap_ - outLen_, remaining);
            memcpy(buf_ + session::HEADER_LEN + outLen_, p, n);
            outLen_ += n; p += n; remaining -= n;
            if (outLen_ == cap_) emitChunk(0);   // full buffer → non-final DATA chunk
        }
        return size;
    }

    // Emit the final chunk, closing the reply direction.
    void finish() { emitChunk(session::FLAG_FINAL); }

    // Transport/framework refusal (unknown command, busy): one REJECT chunk
    // whose payload is the reason text.
    void reject(const char* reason)
    {
        size_t n = std::min(cap_, strlen(reason));
        memcpy(buf_ + session::HEADER_LEN, reason, n);
        outLen_ = n;
        emitChunk(session::FLAG_REJECT);
    }

    bool failed() const { return failed_; }
    uint16_t id() const { return id_; }
};

// Routes inbound chunks to sessions. Step 1: every inbound chunk is a complete
// single-chunk request, dispatched synchronously; no persistent state, no
// busy-gate (a session lives only for the duration of OnChunk). Step 2 adds a
// slot table + busy-refuse + multi-chunk request bodies. The framing buffer is
// supplied by the transport (off the httpd-task stack) and lent to each Session.
class SessionMux
{
public:
    struct Sink
    {
        virtual ~Sink() = default;
        virtual void OnSessionOpened(Session& session) = 0;
    };

    SessionMux(WsSessionLink& link, Sink& sink, uint8_t* buf, size_t payloadCap)
        : link_(link), sink_(sink), buf_(buf), payloadCap_(payloadCap) {}

    void OnChunk(uint16_t id, uint8_t flags, const uint8_t* payload, size_t len);

private:
    WsSessionLink& link_;
    Sink& sink_;
    uint8_t* buf_;
    size_t payloadCap_;
};
