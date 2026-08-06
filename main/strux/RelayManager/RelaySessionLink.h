#pragma once

#include "SessionLink.h"
#include "SessionProtocol.h"
#include "RelaySocket.h"

// SessionLink over the outbound relay WebSocket (the second implementation — see
// SessionLink.h).
//
// There is nothing to it, and that is the whole point of the change that produced
// it: the socket underneath is one the owning task READS, so "get me the next
// chunk" is a read, exactly as it is on the browser socket. It used to be a queue
// pop, because frames arrived on a task that was not ours — see RelaySocket for why
// that is gone, along with the per-frame allocation and the dropped chunks.
class RelaySessionLink : public SessionLink
{
    static constexpr int SEND_TIMEOUT_MS = 5000;

    // A body chunk that never arrives must not wedge the task forever; EOF the
    // session instead and let the server retry.
    //
    // Paired with the server's gate idle timeout (SESSION_IDLE_TIMEOUT in
    // relay-server/relay.py), which is deliberately longer. Whichever fires first
    // decides how a stalled session ends, and it has to be this one: the device EOFs
    // its own request, the handler writes a reply, and that reply releases the
    // server's gate in the ordinary way. The other order releases the pipe while this
    // side still believes the session is open — which is the interleaving the gate is
    // there to prevent. Neither timeout limits how LONG a healthy session may run;
    // both measure silence.
    static constexpr int RECV_TIMEOUT_MS = 10000;

    RelaySocket& socket_;

public:
    explicit RelaySessionLink(RelaySocket& socket) : socket_(socket) {}

    bool SendRaw(const uint8_t* frame, size_t len) override
    {
        return socket_.SendBinary(frame, len, SEND_TIMEOUT_MS);
    }

    int RecvChunk(uint8_t* buf, size_t cap, uint16_t* sid, uint8_t* flags) override
    {
        // Idle, dead and too-short all end the request the same way: mid-request
        // there is no such thing as "nothing arrived, carry on".
        const int n = socket_.ReadFrame(buf, cap, RECV_TIMEOUT_MS);
        if (n < static_cast<int>(session::HEADER_LEN)) return -1;

        *sid   = session::readU16(buf);
        *flags = buf[2];
        return n - static_cast<int>(session::HEADER_LEN);
    }
};
