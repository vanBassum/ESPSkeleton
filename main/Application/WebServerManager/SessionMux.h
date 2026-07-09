#pragma once

#include "Stream.h"
#include "WsSessionLink.h"
#include "SessionProtocol.h"
#include <cstdint>
#include <cstddef>
#include <algorithm>
#include <cstring>

// A session's stream. read() = the request bytes fed by the mux; write() = the
// reply, buffered and flushed as binary DATA chunks, closed by finish() with
// FLAG_FINAL. Step 1: the request is a single chunk fed up front (no blocking).
// Step 2 will make read() pull further inbound chunks for large bodies.
class Session : public Stream
{
    uint16_t id_;
    WsSessionLink& link_;

    const uint8_t* req_ = nullptr;   // request bytes (borrowed; valid during dispatch)
    size_t reqLen_ = 0;
    size_t reqPos_ = 0;

    uint8_t out_[WsSessionLink::MAX_PAYLOAD];   // reply chunk buffer
    size_t outLen_ = 0;
    bool failed_ = false;

    // NOTE: named emitChunk, not flush — Stream declares virtual bool flush(),
    // and an unrelated flush overload here would hide it (-Werror=overloaded-virtual).
    void emitChunk(uint8_t flags)
    {
        if (!link_.SendChunk(id_, flags, out_, outLen_)) failed_ = true;
        outLen_ = 0;
    }

public:
    Session(uint16_t id, WsSessionLink& link) : id_(id), link_(link) {}

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
            size_t n = std::min(sizeof(out_) - outLen_, remaining);
            memcpy(out_ + outLen_, p, n);
            outLen_ += n; p += n; remaining -= n;
            if (outLen_ == sizeof(out_)) emitChunk(0);   // full buffer → non-final DATA chunk
        }
        return size;
    }

    // Emit the final chunk, closing the reply direction.
    void finish() { emitChunk(session::FLAG_FINAL); }

    // Transport/framework refusal (unknown command, busy): one REJECT chunk.
    void reject(const char* reason) { link_.SendChunk(id_, session::FLAG_REJECT, reason, strlen(reason)); }

    bool failed() const { return failed_; }
    uint16_t id() const { return id_; }
};

// Routes inbound chunks to sessions. Step 1: every inbound chunk is a complete
// single-chunk request, dispatched synchronously; no persistent state, no
// busy-gate (a session lives only for the duration of OnChunk). Step 2 adds a
// slot table + busy-refuse + multi-chunk request bodies.
class SessionMux
{
public:
    struct Sink
    {
        virtual ~Sink() = default;
        virtual void OnSessionOpened(Session& session) = 0;
    };

    SessionMux(WsSessionLink& link, Sink& sink) : link_(link), sink_(sink) {}

    void OnChunk(uint16_t id, uint8_t flags, const uint8_t* payload, size_t len);

private:
    WsSessionLink& link_;
    Sink& sink_;
};
