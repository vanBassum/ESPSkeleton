#pragma once

#include "Stream.h"
#include "ReplyWriter.h"
#include <cstddef>
#include <cstdint>

// Everything a command handler gets: its arguments, its request body, its reply.
//
//     RequestError Cmd_Read(CommandContext& ctx)
//     {
//         char     partition[17] = {};
//         uint32_t address = 0, length = 0;
//         bool     ascii = false;
//
//         RETURN_IF_ERROR(ctx.readArgs(
//             Required("partition", partition),
//             Required("address",   address),
//             Required("length",    length),
//             Optional("ascii",     ascii)
//         ));
//         // ctx.in is now positioned at the body (which may be empty).
//     }
//
// Declaring every argument in ONE call is what makes the parse zero-buffer: the
// reader takes a name/value pair off the stream, finds which destination it belongs
// to, writes into it, and keeps nothing. Arbitrary order costs nothing, because
// nothing has to be remembered for a later question. Asking one at a time is what
// used to force a buffer — the parser had to answer questions it had not been asked.
//
// It is also the end-of-arguments marker, so there is no separate call to forget:
// under `help` the reader prints the declarations instead of filling them and returns
// a sentinel, so the handler's body is never reached. A handler that never calls
// readArgs has no arguments at all, which breaks the first time it is used.
//
// The framework validates FORM — is a required argument present, is that number a
// number. The handler validates MEANING — is that address inside this partition.
// Form failures become a REJECT; meaning goes in the reply, where it can carry data.

/// Ways a REQUEST can be unusable. Closed set, owned by the framework — anything a
/// command author wants to add is meaning, and belongs in the reply.
enum class RequestError : uint8_t
{
    Ok = 0,
    UnknownCommand,
    MissingArgument,
    MalformedNumber,
    ArgumentTooLong,
    MalformedRequest,

    /// Not a failure, and the one value a wire reader never produces: `help` swaps in
    /// a reader that prints the declarations instead of filling them, and this is how
    /// it stops the handler at its own RETURN_IF_ERROR before the body runs. It never
    /// escapes the help command, which turns it back into Ok.
    Described,
};

enum class ArgType : uint8_t { String, UInt32, Bool };

/// One declared argument: where to put it and whether it may be absent. Type-erased
/// on purpose — the variadic layer builds an array of these and hands it to one
/// ordinary function, so a command does not instantiate its own copy of the parser.
struct ArgSpec
{
    const char* name;
    void*       dst;
    size_t      cap;       // strings only
    ArgType     type;
    bool        required;
};

// Capacity is deduced from the array, so `sizeof` never appears at a call site.
template <size_t N>
inline ArgSpec Required(const char* name, char (&dst)[N]) { return { name, dst, N, ArgType::String, true }; }
inline ArgSpec Required(const char* name, uint32_t& dst)  { return { name, &dst, 0, ArgType::UInt32, true }; }
inline ArgSpec Required(const char* name, bool& dst)      { return { name, &dst, 0, ArgType::Bool,   true }; }

template <size_t N>
inline ArgSpec Optional(const char* name, char (&dst)[N]) { return { name, dst, N, ArgType::String, false }; }
inline ArgSpec Optional(const char* name, uint32_t& dst)  { return { name, &dst, 0, ArgType::UInt32, false }; }
inline ArgSpec Optional(const char* name, bool& dst)      { return { name, &dst, 0, ArgType::Bool,   false }; }

/// Reads a request's arguments off a stream. One implementation per wire format; a
/// handler never learns which one it got.
class ArgReader
{
public:
    virtual ~ArgReader() = default;

    /// Fill the declared destinations and leave the stream at the first body byte.
    ///
    /// An argument that was not declared is currently IGNORED. Refusing one is the
    /// behaviour we want, but it belongs with a reader whose format makes an undeclared
    /// argument unambiguous; in the JSON envelope it needs a quote- and depth-aware key
    /// scan for a benefit that is thin while the only client is the generated frontend.
    /// See docs/reasoning/ on the console format being parked.
    ///
    /// An absent Optional leaves its destination alone, so the caller's initialiser
    /// stands as the default.
    virtual RequestError read(const ArgSpec* specs, size_t count) = 0;

    /// Which argument a failure was about, for the refusal text. Names are string
    /// literals, so this costs nothing to keep.
    const char* failedArgument() const { return failed_; }

protected:
    const char* failed_ = nullptr;
};

/// The authentication state of the connection a request arrived on, lent by the
/// transport. Per-connection state is transport-specific — a socket has one shape, an
/// outbound pipe another — so a handler that needs it (the `auth` commands, and
/// nothing else) receives it through the context rather than reaching for it.
///
/// Null for a transport that does no gating.
class ConnectionAuth
{
public:
    virtual ~ConnectionAuth() = default;

    /// Mark this connection authenticated, remembering the session key so a
    /// reconnect can resume.
    virtual void authenticate(const char* key) = 0;

    virtual bool isAuthed() const = 0;
};

class CommandContext
{
public:
    CommandContext(ArgReader& reader, ReplyWriter& writer,
                   Stream& request, Stream& response,
                   ConnectionAuth* connection = nullptr)
        : in(request), out(response), reply(writer),
          connection(connection), reader_(reader) {}

    CommandContext(const CommandContext&) = delete;
    CommandContext& operator=(const CommandContext&) = delete;

    Stream& in;    ///< request body, positioned there by readArgs
    Stream& out;   ///< reply, as raw bytes

    /// The reply as structure: `auto resp = ctx.reply.object();`. Writes through to
    /// `out` in the connection's format, so a handler need not name one. Raw writes to
    /// `out` remain legal alongside it — a header record then a file body, say.
    ReplyWriter& reply;

    /// Auth state of the connection this arrived on; null when the transport gates
    /// nothing. Only the `auth` commands have any business touching it.
    ConnectionAuth* const connection;

    /// Declare and read every argument at once. Call it even with none — it is what
    /// advances the stream to the body and what stops a handler under `help`.
    template <typename... Specs>
    RequestError readArgs(Specs... specs)
    {
        ArgSpec list[] = { specs... };
        return reader_.read(list, sizeof...(specs));
    }

    RequestError readArgs() { return reader_.read(nullptr, 0); }

    const char* failedArgument() const { return reader_.failedArgument(); }

private:
    ArgReader& reader_;
};

#define RETURN_IF_ERROR(expr) do { RequestError e_ = (expr);                   \
                                   if (e_ != RequestError::Ok) return e_; } while (0)

/// Human-readable form of a request failure, for the REJECT payload. Written by the
/// framework — handlers never compose error text.
const char* DescribeRequestError(RequestError e, const char* arg, char* buf, size_t cap);
