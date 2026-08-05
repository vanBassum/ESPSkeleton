#pragma once

#include "Stream.h"
#include "SessionLink.h"
#include "SessionProtocol.h"
#include "esp_log.h"
#include <cstdint>
#include <cstddef>
#include <algorithm>
#include <cstring>

// A session's stream, and the only thing that crosses the transport/dispatch
// boundary: a transport turns wire bytes into one of these, the dispatcher turns
// it into a handler call. There is no layer in between — a transport constructs a
// Session on its own stack, feeds it the first chunk, and hands it to
// CommandManager::Execute.
//
// read() = the request bytes; write() = the reply, accumulated and flushed as
// binary DATA chunks, closed by finish() with FLAG_FINAL. The reply is assembled
// directly into an EXTERNAL framing buffer (owned by the transport, off the
// task stack): payload goes after the 3-byte header slot, so a flush is one
// SendRaw with no extra copy.
//
// The request is a run of session chunks that share this id: the first is fed
// up front (feedRequest); once it's drained, read() pulls further chunks off
// the link (RecvChunk) until a chunk carries FLAG_FINAL. A small no-body command
// is a single FLAG_FINAL chunk, so read() never blocks; a streamed upload is many
// chunks ending in FLAG_FINAL.
//
// Both buffers are also lent out as-is (Stream::canLend and friends), which is what
// lets a handler stream a firmware image to flash holding no buffer of its own.
class Session : public Stream
{
    uint16_t id_;
    SessionLink& link_;

    const uint8_t* req_ = nullptr;   // current chunk's payload
    size_t reqLen_ = 0;
    size_t reqPos_ = 0;
    size_t consumed_ = 0;            // request bytes handed to the reader, for diagnostics
    bool   reqFinal_ = false;        // current chunk was FLAG_FINAL → no more after it

    uint8_t* inBuf_;      // buffer for pulled continuation chunks [ header | payload ]
    size_t   inCap_;      // capacity of inBuf_ (total, header included)

    uint8_t* buf_;        // external [ header | payload ] buffer (reply)
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

    // Leave req_ holding at least one unread byte, pulling the next chunk off the
    // link when the current one is drained. False = end of the request, either
    // because it ended (reqFinal_) or because the transport broke (failed_).
    bool ensureInput()
    {
        while (reqPos_ >= reqLen_)                      // current chunk drained
        {
            if (reqFinal_ || failed_) return false;    // EOF
            uint16_t sid = 0; uint8_t flags = 0;
            int n = link_.RecvChunk(inBuf_, inCap_, &sid, &flags);
            if (n < 0 || sid != id_)
            {
                // Logged because the caller sees 0, the same as a clean end of
                // stream: without a line here a transport failure is silent, and a
                // reader that trusts 0 to mean "complete" acts on a truncated
                // request.
                ESP_LOGE("Session", "read failed after %u bytes: n=%d sid=%u (want %u)",
                         static_cast<unsigned>(consumed_), n,
                         static_cast<unsigned>(sid), static_cast<unsigned>(id_));
                failed_ = true;
                return false;
            }
            req_ = inBuf_ + session::HEADER_LEN;
            reqLen_ = static_cast<size_t>(n);
            reqPos_ = 0;
            reqFinal_ = (flags & session::FLAG_FINAL) != 0;
        }
        return true;
    }

public:
    Session(uint16_t id, SessionLink& link, uint8_t* buf, size_t payloadCap,
            uint8_t* inBuf, size_t inCap)
        : id_(id), link_(link), inBuf_(inBuf), inCap_(inCap), buf_(buf), cap_(payloadCap) {}

    void feedRequest(const uint8_t* data, size_t len, bool final)
    {
        req_ = data; reqLen_ = len; reqPos_ = 0; reqFinal_ = final;
    }

    // The first chunk's payload, without consuming it — lets the dispatcher read
    // the header line for routing while the handler still reads it as `in`.
    void peekRequest(const uint8_t*& data, size_t& len) const { data = req_; len = reqLen_; }

    size_t read(void* dst, size_t size, TickType_t timeout = portMAX_DELAY) override
    {
        (void)timeout;
        if (!ensureInput()) return 0;
        size_t n = std::min(size, reqLen_ - reqPos_);
        if (n) { memcpy(dst, req_ + reqPos_, n); reqPos_ += n; consumed_ += n; }
        return n;
    }

    // ── Zero-copy handoff (Stream) ────────────────────────────────────────────
    // The request already sits in the transport's inbound buffer and the reply is
    // already assembled in its framing buffer, so a handler moving kilobytes can
    // work in both directly and hold no buffer at all. See Stream.h.

    bool canLend() const override { return true; }

    size_t lendInput(const uint8_t*& data) override
    {
        if (!ensureInput()) { data = nullptr; return 0; }
        const size_t n = reqLen_ - reqPos_;
        data = req_ + reqPos_;
        reqPos_ = reqLen_;
        consumed_ += n;
        return n;
    }

    uint8_t* lendOutput(size_t& avail) override
    {
        if (outLen_ == cap_) emitChunk(0);   // full buffer → send it, then lend it
        if (failed_) { avail = 0; return nullptr; }
        avail = cap_ - outLen_;
        return buf_ + session::HEADER_LEN + outLen_;
    }

    void commitOutput(size_t n) override
    {
        outLen_ += n;
        if (outLen_ == cap_) emitChunk(0);   // full buffer → non-final DATA chunk
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

    // Emit whatever reply bytes are buffered as a NON-final chunk, now. Lets a
    // handler push incremental output (e.g. upload progress) mid-stream instead
    // of it sitting in the buffer until finish(). No-op when nothing is buffered.
    bool flush() override
    {
        if (outLen_ > 0) emitChunk(0);
        return !failed_;
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

    /// A read or write that stopped short because the transport broke, not because
    /// the request ended. Both look like read() == 0 to the caller, so anything that
    /// needs *all* of its input has to ask. An override, so a handler holding only a
    /// `Stream&` can ask too.
    bool failed() const override { return failed_; }
    uint16_t id() const { return id_; }
};
