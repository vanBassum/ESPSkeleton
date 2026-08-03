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

    /// Route the request from its first chunk, without consuming anything. Both parts
    /// come back empty when there is nothing parseable.
    ///
    /// Two request formats, told apart by the first byte:
    ///
    ///   '{'    the JSON envelope: {"type":"partition write",...}\n[body]
    ///   other  the console form:  partition write -flag value …\n[body]
    ///
    /// Either way the route is two words. The console form is where this is going,
    /// because it can be read in a single pass; the JSON branch and the buffer behind
    /// it go away with the format.
    inline void ReadCommandRoute(const Session& session,
                                 char* category, size_t catCap,
                                 char* command, size_t cmdCap)
    {
        category[0] = '\0';
        command[0]  = '\0';

        const uint8_t* head = nullptr;
        size_t headLen = 0;
        session.peekRequest(head, headLen);
        if (headLen == 0) return;

        char route[MAX_COMMAND_NAME * 2];
        size_t n = 0;

        if (head[0] == '{')
        {
            char line[MAX_ENVELOPE];
            size_t k = std::min(headLen, sizeof(line) - 1);
            memcpy(line, head, k);
            line[k] = '\0';
            if (char* nl = strchr(line, '\n')) *nl = '\0';
            if (!ExtractJsonString(line, "type", route, sizeof(route))) return;
            n = strlen(route);
        }
        else
        {
            // Console form: the first two tokens, straight off the chunk.
            while (n < headLen && n < sizeof(route) - 1) { route[n] = static_cast<char>(head[n]); ++n; }
            route[n] = '\0';
        }

        // Split on the first run of whitespace: "partition write" -> two parts.
        auto isSep = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; };

        size_t i = 0;
        while (i < n && !isSep(route[i])) ++i;
        const size_t catLen = std::min(i, catCap - 1);
        memcpy(category, route, catLen);
        category[catLen] = '\0';

        while (i < n && isSep(route[i])) ++i;
        size_t j = i;
        while (j < n && !isSep(route[j])) ++j;
        const size_t cmdLen = std::min(j - i, cmdCap - 1);
        memcpy(command, route + i, cmdLen);
        command[cmdLen] = '\0';
    }

    /// Run one opened session: name it, dispatch it, close or refuse the reply.
    ///
    /// `dispatcher` is duck-typed on
    /// `RequestError Execute(category, command, Stream&, Stream&, const char**)`
    /// — a template rather than an interface so the protocol layer never depends
    /// upward on the dispatcher, and no callback inversion comes back.
    /// `gate` is duck-typed on `bool Allows(const char* category) const` plus
    /// `ConnectionAuth&`-conversion — the transport decides what may run before a
    /// connection has authenticated, and lends the auth state to the handlers that
    /// need it. Checked AFTER routing, because the decision is per-category.
    template <class Dispatcher, class Gate>
    void RunCommandSession(Session& session, Dispatcher& dispatcher, Gate& gate)
    {
        char category[MAX_COMMAND_NAME] = {};
        char command[MAX_COMMAND_NAME]  = {};
        ReadCommandRoute(session, category, sizeof(category), command, sizeof(command));

        if (category[0] == '\0' || command[0] == '\0')
        {
            session.reject("expected: <category> <command>");
            return;
        }

        if (!gate.Allows(category))
        {
            session.reject("unauthorized");
            return;
        }

        // in == out: the handler reads its arguments and any body from the same
        // session it writes its reply to.
        const char* failedArg = nullptr;
        const RequestError err =
            dispatcher.Execute(category, command, session, session, &gate, &failedArg);
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
