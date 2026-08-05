#pragma once

#include "CommandContext.h"
#include "JsonScope.h"

// The ArgReader that answers `help` instead of a request.
//
// A command's arguments are declared where they are used — one readArgs call inside
// the handler — so the only thing that knows them is the handler. Rather than keep a
// second, hand-maintained table of the same facts, help RE-DISPATCHES the command
// with this reader in place of the wire one: the declarations arrive at read(), get
// written out, and the sentinel it returns stops the handler at its own
// RETURN_IF_ERROR before the body runs.
//
// It works because a handler cannot tell one reader from another — the same property
// that would let a different wire format convert every command at once.
class DescribeArgReader final : public ArgReader
{
public:
    /// `args` is the array the declarations are appended to, one object each.
    explicit DescribeArgReader(JsonArray& args) : args_(args) {}

    RequestError read(const ArgSpec* specs, size_t count) override
    {
        for (size_t i = 0; i < count; ++i)
        {
            const ArgSpec& s = specs[i];
            JsonObject arg = args_.object();
            arg.field("name", s.name);
            arg.field("type", TypeName(s.type));
            arg.field("required", s.required);
            // The declared capacity, minus the terminator: the length at which this
            // argument is refused, which is the number a caller actually needs.
            if (s.type == ArgType::String && s.cap > 0)
                arg.field("maxLength", static_cast<uint32_t>(s.cap - 1));
        }
        return RequestError::Described;
    }

private:
    JsonArray& args_;

    static const char* TypeName(ArgType t)
    {
        switch (t)   // no default: a new ArgType must be handled here
        {
        case ArgType::String: return "string";
        case ArgType::UInt32: return "uint32";
        case ArgType::Bool:   return "bool";
        }
        return "unknown";
    }
};
