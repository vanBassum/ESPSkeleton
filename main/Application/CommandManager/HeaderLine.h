#pragma once

#include "Stream.h"
#include <cstddef>

// The command envelope convention: a request (and a reply that carries a body)
// starts with a single '\n'-terminated header line of JSON, and the body — if
// any — is whatever bytes follow it in the same session.
//
//     {"type":"updateWrite","partition":"ota_1"}\n<firmware bytes…>
//
// Handlers call this on line one to consume their own envelope; the dispatcher
// has only *peeked* it for routing. Reads a byte at a time — the line always
// lives in the request's first chunk, so this never blocks on the transport.
inline void ReadHeaderLine(Stream& in, char* out, size_t cap)
{
    size_t i = 0;
    char c;
    while (i < cap - 1 && in.read(&c, 1) == 1)
    {
        if (c == '\n') break;
        out[i++] = c;
    }
    out[i] = '\0';
}
