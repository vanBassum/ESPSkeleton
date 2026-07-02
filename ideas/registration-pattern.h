#pragma once

// ══════════════════════════════════════════════════════════════
// THE REGISTRATION PATTERN — reference distillation, not built.
//
// How a central manager can dispatch to things owned by other
// managers, with zero heap, zero std::function, no max-count
// constant, and no upward dependencies. First applied in
// CommandManager (see Application/CommandManager/CommandEntry.h);
// candidates for the same treatment: MqttManager's command/
// discovery hooks, settings definitions.
//
// The essence, in five decisions:
//
//  1. ENTRIES ARE THE LINKS. The registry stores nothing; each
//     entry carries its own `next` pointer. No array in the hub,
//     no capacity to size, no allocation ever.
//
//  2. THE OWNER PROVIDES THE MEMORY. Entries live as an
//     `inline static Entry entries_[]` member of the registering
//     class → static storage duration → immortal. The registry
//     only ever holds pointers to memory that cannot die.
//
//  3. CTX IS STAMPED ONCE, AT REGISTRATION. Entries are written
//     as pure data ({name, function}); Register() fills in the
//     owner's `this`. Handlers are captureless-lambda/static-fn
//     trampolines that cast ctx back. That's what keeps
//     std::function (and its heap) out.
//
//  4. MISUSE FAILS ON THE FIRST BOOT, EVERY BOOT. Registration is
//     unconditional straight-line code in Init(), so every guard
//     fires deterministically on the developer's desk:
//       - destroying a registered entry  → dtor logs + abort()
//         (compile-time forbid is impossible: a deleted dtor would
//         propagate up to the global ApplicationContext)
//       - registering the same array twice → assert (would cycle
//         the chain and hang dispatch)
//       - duplicate name → assert
//
//  5. LOCK THE LINKS, NOT THE CALLS. A RecursiveMutex guards the
//     chain (recursive so Register can use Find under its own
//     lock, keeping check-and-insert atomic). Dispatch finds the
//     entry under the lock but runs the handler OUTSIDE it — safe
//     because entries are immortal — so a handler may register or
//     dispatch recursively without deadlock.
// ══════════════════════════════════════════════════════════════

#include <cstdlib>
#include <cstring>
#include <cassert>
#include "esp_log.h"
#include "RecursiveMutex.h"
#include "ContextLock.h"

// Adapt the handler signature per use case; everything else stays.
struct Entry
{
    const char* name;
    void (*handler)(void* ctx /*, payload... */);

    // Managed by Registry::Register() — owners never touch these.
    void* ctx = nullptr;
    Entry* next = nullptr;
    bool registered = false;

    ~Entry()
    {
        if (registered)
        {
            ESP_LOGE("Entry", "registered entry '%s' destroyed — "
                     "entry tables must live for the whole application", name);
            abort();
        }
    }
};

class Registry
{
    static constexpr const char* TAG = "Registry";

    RecursiveMutex mutex_;
    Entry* head_ = nullptr;

public:
    // Array by reference: the count is deduced, so it can never be wrong.
    // Thread-safe and usable from construction (members are initialized
    // before any Init() runs), so registration order doesn't matter.
    template <size_t N>
    void Register(void* ctx, Entry (&entries)[N])
    {
        LOCK(mutex_);
        for (size_t i = 0; i < N; ++i)
        {
            assert(!entries[i].registered && "entry registered twice");
            assert(Find(entries[i].name) == nullptr && "duplicate name");

            entries[i].ctx = ctx;
            entries[i].registered = true;
            entries[i].next = head_;
            head_ = &entries[i];
        }
    }

    void Dispatch(const char* name /*, payload... */)
    {
        const Entry* e = Find(name);
        if (e == nullptr)
            return;
        e->handler(e->ctx);   // outside the lock — see decision 5
    }

private:
    const Entry* Find(const char* name)
    {
        LOCK(mutex_);
        for (Entry* e = head_; e != nullptr; e = e->next)
            if (strcmp(name, e->name) == 0)
                return e;
        return nullptr;
    }
};

// ──────────────────────────────────────────────────────────────
// What an owner writes — the whole ceremony:
// ──────────────────────────────────────────────────────────────

class SomeManager
{
public:
    void Init(Registry& registry)
    {
        registry.Register(this, entries_);
    }

private:
    static void DoThing(void* ctx)
    {
        static_cast<SomeManager*>(ctx)->DoThingImpl();
    }

    void DoThingImpl() { /* real work, full private access */ }

    // ~24 bytes RAM per entry. Delete this class and its entries
    // vanish with it — nothing else references them.
    inline static Entry entries_[] = {
        { "doThing", &SomeManager::DoThing },
    };
};
