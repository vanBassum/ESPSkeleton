#pragma once

#include <cstddef>
#include <cstdint>

// Argument access for command handlers. Handlers PULL what they want by name; the
// framework decides how it was encoded.
//
// An interface on purpose: the JSON implementation serves today's envelope, and a
// token implementation will serve the console format later. A handler written
// against this converts exactly once and then works with both, so neither format
// ever appears in handler code.
//
// The framework validates FORM — is the command known, is a required argument
// present, is that number a number. The handler validates MEANING — is that address
// inside this partition. Those travel by different routes: form failures become a
// REJECT, meaning goes in the reply where it can carry data alongside.

/// Ways a REQUEST can be unusable. Closed set, owned by the framework. Anything a
/// command author wants to add is meaning, and belongs in the reply — hence the
/// name, which is about request validity rather than "command errors".
enum class RequestError : uint8_t
{
    Ok = 0,
    UnknownCommand,
    MissingArgument,
    MalformedNumber,
    ArgumentTooLong,
};

/// Whether absence is a failure. Passed to the pull, not carried by the macro,
/// because generated help is produced by *running* the pulls — so required-ness has
/// to live where the thing that introspects it can see it.
enum class Arg : uint8_t { Optional, Required };

class Args
{
public:
    virtual ~Args() = default;

    /// Copy a string argument into `dst`. Leaves `dst` untouched when absent and
    /// Optional, so the caller's initialiser is the default.
    virtual RequestError string(const char* name, char* dst, size_t cap, Arg req) = 0;

    /// Decimal or 0x-prefixed hex.
    virtual RequestError uint32(const char* name, uint32_t& dst, Arg req) = 0;

    /// Presence test — no value to malform and absence is the answer, so it cannot
    /// fail and takes no Arg.
    virtual bool flag(const char* name) = 0;

    /// End of the argument block. Nothing today; this is the seam where generated
    /// help stops a handler before its body runs (an implementation that prints each
    /// pull instead of fetching returns non-Ok from here).
    virtual RequestError done() { return RequestError::Ok; }

    /// Which argument a failed pull was looking for, so the framework can turn a bare
    /// enum into "missing required argument: address". Names are string literals, so
    /// the pointer outlives the call and nothing is copied.
    const char* failedArgument() const { return failed_; }

protected:
    const char* failed_ = nullptr;
};

// One macro, because required-ness is in the call. Two macros could disagree with
// their own call, which is a real failure mode rather than a hypothetical one.
#define ARG_CHECK(expr)  do { RequestError e_ = (expr);                     \
                              if (e_ != RequestError::Ok) return e_; } while (0)

#define ARG_DONE(args)   do { RequestError e_ = (args).done();              \
                              if (e_ != RequestError::Ok) return e_; } while (0)

/// Human-readable form of a request failure, for the REJECT payload. `arg` may be
/// null. Written by the framework — handlers never compose error text.
const char* DescribeRequestError(RequestError e, const char* arg, char* buf, size_t cap);
