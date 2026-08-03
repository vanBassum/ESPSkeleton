#pragma once

#include "Stream.h"
#include <cstddef>

// Reads text off a Stream. Non-owning and stateless beyond the Stream reference,
// so it is free to construct at a use site and the Stream stays the single source
// of position — two readers over one Stream read successive text, they do not
// each start over.
//
// Currently just readLine(). Deliberately no writer counterpart and no buffering:
// JsonWriter already covers structured output, and a buffer here would make the
// reader own position, breaking the property above.
class StringReader
{
public:
    explicit StringReader(Stream& in) : in_(in) {}

    StringReader(const StringReader&) = delete;
    StringReader& operator=(const StringReader&) = delete;

    /// Read up to and consuming the next '\n', which is NOT stored. Always
    /// NUL-terminates `out` (given cap > 0) and returns the character count.
    ///
    /// A line longer than `cap - 1` is truncated and the rest of it stays in the
    /// Stream — the caller sees a short line rather than a silent seek, which is
    /// the honest failure for a bounded destination.
    size_t readLine(char* out, size_t cap)
    {
        if (cap == 0) return 0;

        size_t i = 0;
        char c;
        while (i < cap - 1 && in_.read(&c, 1) == 1)
        {
            if (c == '\n') break;
            out[i++] = c;
        }
        out[i] = '\0';
        return i;
    }

private:
    Stream& in_;
};
