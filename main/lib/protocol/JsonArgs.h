#pragma once

#include "Args.h"
#include "Stream.h"
#include "JsonHelpers.h"
#include <cstring>
#include <cstdlib>

// Args over today's JSON envelope: {"type":"…","partition":"ota_1"}\n[body]
//
// It buffers the envelope line, because looking a field up by name needs random
// access — that is the cost of JSON on the request side, not of JSON being wordy.
// Which is also the point of doing this now: once handlers pull instead of parsing,
// this class is the ONLY thing holding a request-sized buffer, and swapping in a
// token implementation deletes it without touching a single handler.
class JsonArgs final : public Args
{
    static constexpr size_t MAX_ENVELOPE = 512;

public:
    /// Consumes the envelope line from `in`, leaving it positioned at the body.
    explicit JsonArgs(Stream& in)
    {
        size_t i = 0;
        char c;
        while (i < sizeof(line_) - 1 && in.read(&c, 1) == 1)
        {
            if (c == '\n') break;
            line_[i++] = c;
        }
        line_[i] = '\0';
    }

    RequestError string(const char* name, char* dst, size_t cap, Arg req) override
    {
        char tmp[MAX_ENVELOPE];
        if (!ExtractJsonString(line_, name, tmp, sizeof(tmp)) || tmp[0] == '\0')
            return absent(name, req);

        if (strlen(tmp) >= cap) { failed_ = name; return RequestError::ArgumentTooLong; }
        strlcpy(dst, tmp, cap);
        return RequestError::Ok;
    }

    RequestError uint32(const char* name, uint32_t& dst, Arg req) override
    {
        char tmp[32];
        if (!ExtractJsonString(line_, name, tmp, sizeof(tmp)) || tmp[0] == '\0')
        {
            // Numbers are unquoted in JSON, so fall back to the numeric extractor.
            // -1 as a sentinel: the helper cannot report absence.
            const int32_t v = ExtractJsonInt(line_, name, -1);
            if (v < 0) return absent(name, req);
            dst = static_cast<uint32_t>(v);
            return RequestError::Ok;
        }

        char* end = nullptr;
        const unsigned long v = strtoul(tmp, &end, 0);   // 0 → accepts 0x…
        if (end == tmp || *end != '\0') { failed_ = name; return RequestError::MalformedNumber; }
        dst = static_cast<uint32_t>(v);
        return RequestError::Ok;
    }

    bool flag(const char* name) override
    {
        char tmp[8] = {};
        if (ExtractJsonString(line_, name, tmp, sizeof(tmp)) && tmp[0] != '\0')
            return strcmp(tmp, "true") == 0 || strcmp(tmp, "1") == 0;
        return ExtractJsonInt(line_, name, 0) != 0;
    }

    /// The envelope line, for handlers not yet converted to pulls.
    const char* line() const { return line_; }

private:
    char line_[MAX_ENVELOPE] = {};

    RequestError absent(const char* name, Arg req)
    {
        if (req == Arg::Required) { failed_ = name; return RequestError::MissingArgument; }
        return RequestError::Ok;   // caller's initialiser stands as the default
    }
};
