#pragma once

#include <cstdlib>
#include "esp_log.h"

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
        {
            ESP_LOGE("CommandEntry", "registered command '%s' destroyed — "
                     "command tables must live for the whole application", name);
            abort();
        }
    }
};
