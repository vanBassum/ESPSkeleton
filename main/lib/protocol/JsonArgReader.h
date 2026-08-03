#pragma once

#include "CommandContext.h"
#include "JsonHelpers.h"
#include <cstring>
#include <cstdlib>

// ArgReader over today's JSON envelope: {"type":"…","partition":"ota_1"}\n[body]
//
// Deliberately simple: it buffers the envelope line and uses the existing JSON helpers
// to look each declared argument up. That keeps a request-sized buffer — about 700 bytes
// of dispatch stack — which is a known cost rather than an oversight. A streaming parse
// would need the whole JSON grammar handled by hand, and the RAM that actually matters on
// this device is elsewhere (the relay's per-frame allocation, the 4 KB upload buffer).
//
// What matters is that handlers cannot tell. They declare their arguments once through
// CommandContext, so a different reader — a single-pass one over a simpler format, parked
// for now — would convert every command at once without touching a handler.
class JsonArgReader final : public ArgReader
{
    static constexpr size_t MAX_ENVELOPE = 512;
    static constexpr size_t MAX_VALUE    = 192;

public:
    /// Consumes the envelope line, leaving `in` positioned at the body.
    explicit JsonArgReader(Stream& in)
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

    /// Fills the declared arguments. Undeclared ones are IGNORED, deliberately:
    /// enumerating an envelope's keys means telling keys from values, handling escapes
    /// and nesting, and not mistaking a key that merely begins with "type" for the
    /// routing key — a mistake I made on the first attempt. Refusing them is the
    /// behaviour we want, but it belongs with a format where an undeclared argument is
    /// unambiguous, and the benefit is thin while the only client is a generated
    /// frontend that cannot mistype. A misspelled OPTIONAL argument therefore leaves its
    /// destination at the default, silently; a misspelled required one still fails.
    RequestError read(const ArgSpec* specs, size_t count) override
    {
        for (size_t i = 0; i < count; ++i)
        {
            const RequestError e = fill(specs[i]);
            if (e != RequestError::Ok) return e;
        }
        return RequestError::Ok;
    }

private:
    char line_[MAX_ENVELOPE] = {};

    RequestError fill(const ArgSpec& s)
    {
        // Branch on the raw field text, NOT on whether a typed extractor succeeded.
        // Probing an extractor cannot tell "absent" from "present but not the type I
        // asked for" — and getting that wrong turned an explicitly empty string into
        // the string "0", which silently corrupted a stored password.
        const char* raw = FindJsonField(line_, s.name);
        if (!raw || *raw == 'n')   // absent, or JSON null
        {
            if (s.required) { failed_ = s.name; return RequestError::MissingArgument; }
            return RequestError::Ok;   // absent: the caller's initialiser stands
        }

        char value[MAX_VALUE] = {};
        if (*raw == '"')
        {
            // A quoted value, possibly empty — an empty string IS a value.
            ExtractJsonString(line_, s.name, value, sizeof(value));
        }
        else
        {
            // A bare token: number, 0x-prefixed hex, or true/false. Copy to the
            // delimiter and let the type decide what it means.
            size_t i = 0;
            for (const char* p = raw;
                 *p && *p != ',' && *p != '}' && *p != ' ' && i < sizeof(value) - 1; ++p)
                value[i++] = *p;
            value[i] = '\0';
        }

        switch (s.type)   // no default: a new ArgType must be handled here
        {
        case ArgType::String:
            if (strlen(value) >= s.cap) { failed_ = s.name; return RequestError::ArgumentTooLong; }
            strcpy(static_cast<char*>(s.dst), value);
            return RequestError::Ok;

        case ArgType::UInt32:
        {
            char* end = nullptr;
            const unsigned long v = strtoul(value, &end, 0);   // 0 → accepts 0x…
            if (end == value || *end != '\0') { failed_ = s.name; return RequestError::MalformedNumber; }
            *static_cast<uint32_t*>(s.dst) = static_cast<uint32_t>(v);
            return RequestError::Ok;
        }

        case ArgType::Bool:
            *static_cast<bool*>(s.dst) =
                (strcmp(value, "true") == 0 || strcmp(value, "1") == 0);
            return RequestError::Ok;
        }
        return RequestError::MalformedRequest;
    }

};
