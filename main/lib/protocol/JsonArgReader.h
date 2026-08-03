#pragma once

#include "CommandContext.h"
#include "JsonHelpers.h"
#include <cstring>
#include <cstdlib>

// ArgReader over today's JSON envelope: {"type":"…","partition":"ota_1"}\n[body]
//
// Deliberately simple: it buffers the envelope line and uses the existing JSON
// helpers to look each declared argument up. Yes, that keeps a request-sized buffer —
// but this format is on its way out, and hand-writing a streaming JSON parser for it
// would be effort spent on something we intend to delete. The zero-buffer parse
// arrives with the token reader, where the format is designed for it.
//
// What matters is that handlers cannot tell: they declare their arguments once
// through CommandContext, so replacing this class converts every command at once.
class JsonArgReader final : public ArgReader
{
    static constexpr size_t MAX_ENVELOPE = 512;
    static constexpr size_t MAX_VALUE    = 192;
    static constexpr size_t MAX_NAME     = 32;

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

    /// Fills the declared arguments. Does NOT refuse undeclared ones, deliberately:
    /// enumerating an envelope's keys means telling keys from values, handling escapes
    /// and nesting, and not mistaking a key that merely starts with "type" for the
    /// routing key. Fiddly, easy to get subtly wrong, and all of it thrown away with
    /// this format. In the token form an undeclared `-name` is unambiguous, so that is
    /// where UnknownArgument gets enforced. Until then a misspelled optional argument
    /// is silently ignored here.
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
    char failedName_[MAX_NAME] = {};

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
