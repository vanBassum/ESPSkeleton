#pragma once

#include "Session.h"
#include "CommandContext.h"
#include "JsonHelpers.h"

#include <algorithm>
#include <cstring>

// The request envelope, and the sequence that runs one session against a
// dispatcher. This is protocol, not dispatch: knowing where the envelope ends and
// what the command is called is the wire format's business, and the dispatcher
// stays name-based so it never sees a Session.
//
// A request starts with a single '\n'-terminated line of JSON; the body — if any —
// is whatever bytes follow it in the same session:
//
//     {"type":"updateWrite","partition":"ota_1"}\n<firmware bytes…>
//
// The line is *peeked*, never consumed, because the handler reads that same line
// for its own arguments. (A positional wire format would not need the peek — see
// docs/reasoning/ on why the delimiter search is what forces this.)
namespace protocol
{
    // Bounds the envelope, not the request: bodies stream. Long enough for the
    // argument lists commands actually take.
    inline constexpr size_t MAX_ENVELOPE = 128;

    // Command names are short by convention; a longer one simply won't match.
    inline constexpr size_t MAX_COMMAND_NAME = 32;

    /// Name the request from its first chunk, without consuming anything. Empty when
    /// there is no parseable name.
    ///
    /// Two request formats are accepted, told apart by the first byte:
    ///
    ///   '{'    the JSON envelope — {"type":"name",...}\n[body]
    ///   other  the console form  — name -flag value … [-- body]
    ///
    /// The console form is where this is going, because it can be read in a single
    /// pass (see TokenReader). The JSON form is still here only until every handler
    /// and the frontend have moved over; then this branch and the buffer it needs go
    /// away together.
    inline void ReadCommandName(const Session& session, char* out, size_t cap)
    {
        const uint8_t* head = nullptr;
        size_t headLen = 0;
        session.peekRequest(head, headLen);

        out[0] = '\0';
        if (headLen == 0) return;

        if (head[0] == '{')
        {
            char line[MAX_ENVELOPE];
            size_t n = std::min(headLen, sizeof(line) - 1);
            memcpy(line, head, n);
            line[n] = '\0';
            if (char* nl = strchr(line, '\n')) *nl = '\0';
            ExtractJsonString(line, "type", out, cap);
            return;
        }

        // Console form: the name is simply the first token. No line to find, no
        // buffer beyond the caller's name variable.
        size_t i = 0;
        while (i < headLen && i < cap - 1)
        {
            const char c = static_cast<char>(head[i]);
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') break;
            out[i] = c;
            ++i;
        }
        out[i] = '\0';
    }

    /// Run one opened session: name it, dispatch it, close or refuse the reply.
    ///
    /// `dispatcher` is duck-typed on `bool Execute(const char*, Stream&, Stream&)`
    /// — a template rather than an interface so the protocol layer never depends
    /// upward on the dispatcher, and no callback inversion comes back.
    template <class Dispatcher>
    void RunCommandSession(Session& session, Dispatcher& dispatcher)
    {
        char name[MAX_COMMAND_NAME] = {};
        ReadCommandName(session, name, sizeof(name));

        if (name[0] == '\0')
        {
            session.reject("missing type");
            return;
        }

        // in == out: the handler reads its arguments and any body from the same
        // session it writes its reply to.
        const char* failedArg = nullptr;
        const RequestError err = dispatcher.Execute(name, session, session, &failedArg);
        if (err != RequestError::Ok)
        {
            // Form failures refuse the request. REJECT ends the session like FINAL
            // does, so this composes with anything the handler already wrote — a
            // refusal can always be last.
            char buf[96];
            session.reject(DescribeRequestError(err, failedArg, buf, sizeof(buf)));
            return;
        }

        session.finish();   // FINAL — end of reply
    }
}
