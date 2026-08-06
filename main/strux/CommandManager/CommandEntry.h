#pragma once

#include "Fatal.h"
#include "CommandContext.h"
#include <type_traits>

class Stream;

// ──────────────────────────────────────────────────────────────
// One command in the CommandManager registry.
//
// Entries are the links of an intrusive chain. Owners declare them
// as an `inline static CommandEntry commands_[]` class member
// (static storage duration) and hand the array to
// CommandManager::Register(), which stamps ctx and links them.
// Owners never touch ctx/next/registered.
//
// Handlers are plain function pointers — no heap, no std::function.
// The usual shape is a static member "trampoline" that casts ctx
// back to the owning manager and calls a private method.
// ──────────────────────────────────────────────────────────────
struct CommandEntry
{
    // Two-part route: `partition write`, `system ping`. The category is the domain the
    // owning manager already claims, which is the one piece of structure a flat
    // registry threw away — and it is what lets `list` and `status` exist in several
    // places without colliding. Two levels, fixed: deeper nesting would turn dispatch
    // into a tree walk for nothing.
    const char* category;
    const char* name;
    RequestError (*handler)(void* ctx, CommandContext& c);

    // Managed by CommandManager::Register() — owners never touch these.
    void* ctx = nullptr;
    CommandEntry* next = nullptr;
    bool registered = false;

    // A registered entry is a live link in the dispatch chain; letting it
    // die would leave a dangling pointer in the chain. There is no
    // compile-time way to forbid this (a deleted dtor would propagate up
    // through the owning manager to the global AppContext), so:
    // abort. The device resets with a clear message on the very first run
    // of the offending code.
    ~CommandEntry()
    {
        if (registered)
            FATAL("registered command '%s' destroyed — command tables must "
                  "live for the whole application", name);
    }
};

// ──────────────────────────────────────────────────────────────
// Handler trampoline.
//
// Handlers are ordinary functions with no ctx in sight — either a
// (usually private, non-static) member of the owning manager:
//
//     RequestError Cmd_Ping(CommandContext& ctx);
//     { "system", "ping", &InvokeCommand<&SystemManager::Cmd_Ping> },
//
// or a free/static function (e.g. quick hacking in main.cpp —
// register with ctx = nullptr).
//
// Arguments arrive already parsed and validated; `in` is positioned at the body
// (empty for most commands), and the handler writes its reply to `out`. Returning
// anything but Ok makes the framework refuse the request — a handler never writes
// error text and never names a framework error.
//
// The trampoline is instantiated at compile time; for members the
// owning class is deduced from the method pointer itself, so the
// ctx cast can never target the wrong type. The if constexpr branch
// resolves during instantiation — there is no runtime check.
//
// The void* plumbing still exists (it is the type erasure that lets
// one chain hold commands of many classes) but it lives only here.
// ──────────────────────────────────────────────────────────────
template <typename T> struct CommandOwner;
template <typename C> struct CommandOwner<RequestError (C::*)(CommandContext&)>       { using type = C; };
template <typename C> struct CommandOwner<RequestError (C::*)(CommandContext&) const> { using type = const C; };

template <auto Handler>
RequestError InvokeCommand(void* ctx, CommandContext& c)
{
    if constexpr (std::is_member_function_pointer_v<decltype(Handler)>)
    {
        using C = typename CommandOwner<decltype(Handler)>::type;
        return (static_cast<C*>(ctx)->*Handler)(c);
    }
    else
    {
        return Handler(c);
    }
}
