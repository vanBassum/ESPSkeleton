#pragma once

#include "Stream.h"
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
    UnknownArgument,
    MalformedNumber,
    ArgumentTooLong,
    MalformedRequest,
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
    /// An argument that was not declared is a refusal (UnknownArgument), which is
    /// what catches a misspelled optional one — the failure that would otherwise
    /// silently change behaviour. Refusing rather than skipping also means the reader
    /// never has to guess an unknown argument's arity, since it stops there.
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

class CommandContext
{
public:
    CommandContext(ArgReader& reader, Stream& request, Stream& reply)
        : in(request), out(reply), reader_(reader) {}

    CommandContext(const CommandContext&) = delete;
    CommandContext& operator=(const CommandContext&) = delete;

    Stream& in;    ///< request body, positioned there by readArgs
    Stream& out;   ///< reply

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
