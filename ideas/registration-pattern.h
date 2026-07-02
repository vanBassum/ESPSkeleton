#pragma once

// ══════════════════════════════════════════════════════════════
// THE REGISTRATION PATTERN — bare minimum, reference only.
//
// How a central hub can call into things owned by other managers,
// with zero heap, zero std::function, no max-count constant, and
// no upward dependencies. First applied in CommandManager; also
// fits MQTT hooks, settings definitions, observers, log sinks.
//
// The essence, in five decisions:
//
//  1. ENTRIES ARE THE LINKS. The hub stores nothing but a head
//     pointer; each entry carries its own `next`. No array in the
//     hub, no capacity to size, no allocation ever.
//
//  2. THE OWNER PROVIDES THE MEMORY. Entries live as an
//     `inline static Entry entries_[]` member of the registering
//     class → static storage duration → immortal. The hub only
//     ever points at memory that cannot die.
//
//  3. CTX IS STAMPED ONCE, AT REGISTRATION. Entries are written as
//     pure data; Register() fills in the owner's `this`. Handlers
//     are static-function trampolines that cast ctx back. That is
//     what keeps std::function (and its heap) out.
//
//  4. MISUSE FAILS ON THE FIRST BOOT, EVERY BOOT. Registration is
//     unconditional straight-line code in Init(), so the guards
//     fire deterministically on the developer's desk:
//       - destroying a registered entry → dtor logs + abort()
//         (a deleted dtor is impossible: it would propagate up to
//         the global ApplicationContext)
//       - registering the same array twice → assert (would cycle
//         the chain)
//
//  5. LOCK THE LINKS, NOT THE CALLS. A mutex guards the chain;
//     handlers run OUTSIDE it — safe because entries are immortal —
//     so a handler may itself register without deadlock.
// ══════════════════════════════════════════════════════════════

#include <cstdlib>
#include <cassert>
#include "esp_log.h"
#include "Mutex.h"
#include "ContextLock.h"

struct Entry
{
    void (*handler)(void* ctx);   // adapt the signature per use case

    // Managed by Registry::Register() — owners never touch these.
    void* ctx = nullptr;
    Entry* next = nullptr;
    bool registered = false;

    ~Entry()
    {
        if (registered)
        {
            ESP_LOGE("Entry", "registered entry destroyed — entry tables "
                     "must live for the whole application");
            abort();
        }
    }
};

class Registry
{
    static constexpr const char* TAG = "Registry";

    Mutex mutex_;
    Entry* head_ = nullptr;

public:
    // Array by reference: the count is deduced, so it can never be wrong.
    // Thread-safe, and usable from construction — registration order
    // between managers doesn't matter.
    template <size_t N>
    void Register(void* ctx, Entry (&entries)[N])
    {
        LOCK(mutex_);
        for (size_t i = 0; i < N; ++i)
        {
            assert(!entries[i].registered && "entry registered twice");

            entries[i].ctx = ctx;
            entries[i].registered = true;
            entries[i].next = head_;
            head_ = &entries[i];
        }
    }

    void InvokeAll()
    {
        Entry* head;
        {
            LOCK(mutex_);
            head = head_;
        }
        // Outside the lock — entries are immortal and next-pointers are
        // never rewritten after linking, so the walk is safe.
        for (Entry* e = head; e != nullptr; e = e->next)
            e->handler(e->ctx);
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

    // ~16 bytes RAM per entry. Delete this class and its entries
    // vanish with it — nothing else references them.
    inline static Entry entries_[] = {
        { &SomeManager::DoThing },
    };
};
