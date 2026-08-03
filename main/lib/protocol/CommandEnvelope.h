#pragma once

#include "Session.h"
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

    /// Copy the envelope line out of the session's first chunk (without consuming
    /// it) and extract the command name. Empty when there is no parseable name.
    inline void ReadCommandName(const Session& session, char* out, size_t cap)
    {
        const uint8_t* head = nullptr;
        size_t headLen = 0;
        session.peekRequest(head, headLen);

        char line[MAX_ENVELOPE];
        size_t n = std::min(headLen, sizeof(line) - 1);
        if (n) memcpy(line, head, n);
        line[n] = '\0';
        if (char* nl = strchr(line, '\n')) *nl = '\0';

        out[0] = '\0';
        ExtractJsonString(line, "type", out, cap);
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
        if (!dispatcher.Execute(name, session, session))
        {
            session.reject(name);   // unknown command
            return;
        }

        session.finish();   // FINAL — end of reply
    }
}
