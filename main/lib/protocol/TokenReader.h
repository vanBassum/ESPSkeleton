#pragma once

#include "Stream.h"
#include <cstddef>
#include <cstring>
#include <cstdlib>

// Single-pass argument reading for the console-style request format:
//
//     update -p ota_1 -a 123456 -- <raw body to end of session>
//
// The command word first, then `-flag value` pairs in any order, then `--` and the
// body if there is one. The body is always last.
//
// Why this exists: parsing JSON into *named* fields needs random access, because a
// field can be anywhere, so the whole request has to be in memory before the first
// field can be read. Reading tokens needs no such thing. Nothing here accumulates —
// each token goes straight into the caller's own variable, so storage is bounded by
// the *schema* (seventeen bytes for a partition label, a few for a number) instead
// of by the request. That is a compile-time known cost rather than one that grows
// with input, which is the whole point.
//
// Order-independence costs nothing: each flag carries its own value, so the reader
// never needs to look ahead. What a handler cannot do is act before it has read all
// its flags — but that is just having its parameters.
//
// `--` is required before a body because raw bytes may legitimately begin with a
// dash, so "the first token that is not a flag" would be ambiguous. Unix settled
// this convention long ago.
class TokenReader
{
public:
    explicit TokenReader(Stream& in) : in_(in) {}

    TokenReader(const TokenReader&)            = delete;
    TokenReader& operator=(const TokenReader&) = delete;

    /// Read the next whitespace-delimited token into `out`. Returns false when the
    /// argument section is over — either `--` was reached (the stream is then
    /// positioned at the first body byte) or the request ended.
    ///
    /// A token longer than `cap - 1` is truncated and the remainder discarded, so an
    /// over-long argument fails to match rather than overrunning anything.
    bool next(char* out, size_t cap)
    {
        if (cap == 0 || bodyReached_) return false;

        char c;
        // Skip leading separators.
        do {
            if (!get(c)) return false;
        } while (isSep(c));

        size_t i = 0;
        for (;;)
        {
            if (i < cap - 1) out[i++] = c;
            if (!get(c)) break;
            if (isSep(c)) break;
        }
        out[i] = '\0';

        // `--` ends the arguments; one separator after it belongs to the framing,
        // and everything from there on is body.
        if (strcmp(out, "--") == 0)
        {
            bodyReached_ = true;
            return false;
        }
        return true;
    }

    /// True once `--` has been consumed, i.e. the stream sits at the body.
    bool atBody() const { return bodyReached_; }

private:
    Stream& in_;
    bool    bodyReached_ = false;
    bool    havePush_    = false;
    char    push_        = 0;

    static bool isSep(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

    bool get(char& c)
    {
        if (havePush_) { c = push_; havePush_ = false; return true; }
        return in_.read(&c, 1) == 1;
    }
};

// Parse a token as an unsigned value. Returns false if it is not all digits, so a
// missing or malformed argument is distinguishable from a legitimate zero.
inline bool ParseUInt(const char* tok, uint32_t& out)
{
    if (!tok || !*tok) return false;
    uint32_t v = 0;
    for (const char* p = tok; *p; ++p)
    {
        if (*p < '0' || *p > '9') return false;
        v = v * 10 + static_cast<uint32_t>(*p - '0');
    }
    out = v;
    return true;
}
