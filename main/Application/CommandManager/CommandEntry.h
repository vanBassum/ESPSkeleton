#pragma once

#include "Fatal.h"

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
// Member-handler trampoline.
//
// Handlers are ordinary (non-static, usually private) member
// functions with no ctx in sight:
//
//     void Ping(const char* json, JsonWriter& resp);
//
// The table entry instantiates the ctx-cast trampoline at compile
// time — the owning class is deduced from the method itself, so the
// cast can never target the wrong type:
//
//     { "ping", &InvokeCommand<&SystemManager::Ping> },
//
// The void* plumbing still exists (it is the type erasure that lets
// one chain hold commands of many classes) but it lives only here.
// ──────────────────────────────────────────────────────────────
template <typename T> struct CommandOwner;
template <typename C> struct CommandOwner<void (C::*)(const char*, JsonWriter&)> { using type = C; };

template <auto Method>
void InvokeCommand(void* ctx, const char* json, JsonWriter& resp)
{
    using C = typename CommandOwner<decltype(Method)>::type;
    (static_cast<C*>(ctx)->*Method)(json, resp);
}
