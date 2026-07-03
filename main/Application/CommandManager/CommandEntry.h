#pragma once

#include "Fatal.h"
#include <type_traits>

class JsonWriter;

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
    const char* name;
    void (*handler)(void* ctx, const char* json, JsonWriter& resp);

    // Managed by CommandManager::Register() — owners never touch these.
    void* ctx = nullptr;
    CommandEntry* next = nullptr;
    bool registered = false;

    // A registered entry is a live link in the dispatch chain; letting it
    // die would leave a dangling pointer in the chain. There is no
    // compile-time way to forbid this (a deleted dtor would propagate up
    // through the owning manager to the global ApplicationContext), so:
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
//     void Ping(const char* json, JsonWriter& resp);
//     { "ping", &InvokeCommand<&SystemManager::Ping> },
//
// or a free/static function (e.g. quick hacking in main.cpp —
// register with ctx = nullptr):
//
//     static void Test(const char* json, JsonWriter& resp);
//     { "test", &InvokeCommand<&Test> },
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
template <typename C> struct CommandOwner<void (C::*)(const char*, JsonWriter&)>       { using type = C; };
template <typename C> struct CommandOwner<void (C::*)(const char*, JsonWriter&) const> { using type = const C; };

template <auto Handler>
void InvokeCommand(void* ctx, const char* json, JsonWriter& resp)
{
    if constexpr (std::is_member_function_pointer_v<decltype(Handler)>)
    {
        using C = typename CommandOwner<decltype(Handler)>::type;
        (static_cast<C*>(ctx)->*Handler)(json, resp);
    }
    else
    {
        Handler(json, resp);   // free/static function — ctx unused
    }
}
