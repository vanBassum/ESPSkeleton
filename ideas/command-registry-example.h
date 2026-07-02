#pragma once

// ══════════════════════════════════════════════════════════════
// DESIGN SKETCH — not built, not included anywhere.
//
// Command registry without dynamic memory: the entries themselves
// are the links of an intrusive chain, owned as `inline static`
// members by the manager that implements them.
//
// Key properties:
//   • No heap, no std::function, no max-commands constant.
//   • Owner never touches linking — Register() does the chaining.
//   • Thread-safe by contract, like every manager: a Mutex guards
//     the chain, so Register() may be called from any task at any
//     time. Handlers run OUTSIDE the lock, so even a handler that
//     registers more commands cannot deadlock.
//   • Misuse is boot-deterministic: destroying a registered entry
//     or registering twice aborts (device resets) on the first
//     run, every run — never a surprise in the field.
//   • Rip-out property: delete a manager's folder and its commands
//     go with it; nothing else references them.
// ══════════════════════════════════════════════════════════════

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <cassert>
#include "esp_log.h"
#include "JsonWriter.h"
#include "Mutex.h"
#include "ContextLock.h"

// ──────────────────────────────────────────────────────────────
// CommandManager side
// ──────────────────────────────────────────────────────────────

struct CommandEntry
{
    const char* name;
    void (*handler)(void* ctx, const char* json, JsonWriter& resp);

    // Managed by CommandManager::Register() — owners never touch these.
    void* ctx = nullptr;
    CommandEntry* next = nullptr;
    bool registered = false;

    // A registered entry is a live link in the dispatch chain; letting
    // it die would leave a dangling pointer in the chain. There is no
    // compile-time way to forbid this (a deleted dtor would propagate
    // up through the owning manager to the global ApplicationContext),
    // so: abort. The device resets with a clear message on the very
    // first run of the offending code — unmistakably "not supposed to
    // be used like that."
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

class CommandManager
{
    static constexpr const char* TAG = "CommandManager";

    Mutex mutex_;
    CommandEntry* head_ = nullptr;

public:
    // Called from a manager's Init(). Takes the array by reference so
    // the count is deduced — it can never be wrong. `ctx` (usually the
    // owner's `this`) is stamped into every entry and handed back to
    // its handler at dispatch.
    //
    // Thread-safe: may be called from any task at any time, not just
    // the Init() sequence. Entries can be added late; they can only
    // never be DESTROYED (see ~CommandEntry).
    template <size_t N>
    void Register(void* ctx, CommandEntry (&commands)[N])
    {
        LOCK(mutex_);
        for (size_t i = 0; i < N; ++i)
        {
            // Re-registering would re-link an entry that is already in
            // the chain and cycle it → Dispatch() would hang. Make the
            // mistake loud instead: fails on first boot, deterministic.
            assert(!commands[i].registered && "command registered twice");
            assert(FindLocked(commands[i].name) == nullptr && "duplicate command name");

            // Fields are fully written BEFORE the entry becomes
            // reachable through head_, so a concurrent Dispatch can
            // never observe a half-initialized entry.
            commands[i].ctx = ctx;
            commands[i].registered = true;
            commands[i].next = head_;
            head_ = &commands[i];
        }
    }

    // Locks internally. Handing the pointer out after the lock is
    // released is safe because entries are immortal (dtor guard) and
    // name/handler/ctx are written once, before the entry is linked.
    const CommandEntry* Find(const char* name)
    {
        LOCK(mutex_);
        return FindLocked(name);
    }

    // Linear walk — ~15 commands in practice, cost is irrelevant.
    //
    // The handler runs OUTSIDE the lock, so a handler can call
    // Register() — or dispatch nested commands — without deadlocking.
    bool Dispatch(const char* type, const char* json, JsonWriter& resp)
    {
        const CommandEntry* e = Find(type);
        if (e == nullptr)
            return false;   // unknown command
        e->handler(e->ctx, json, resp);
        return true;
    }

private:
    // The lock-free core — callers hold mutex_. Register() needs this
    // (instead of the public Find) so its duplicate check and insert
    // happen under ONE continuous lock; a Find that locks internally
    // would make that check-then-act racy, and recursively re-locking
    // would need a recursive mutex (different FreeRTOS API, and lock
    // ownership becomes unreviewable). Public = locks, *Locked =
    // caller does: every function has one answer.
    const CommandEntry* FindLocked(const char* name) const
    {
        for (CommandEntry* e = head_; e != nullptr; e = e->next)
            if (strcmp(name, e->name) == 0)
                return e;
        return nullptr;
    }
};

// ──────────────────────────────────────────────────────────────
// Owner side — what a manager writes
// ──────────────────────────────────────────────────────────────

class NetworkManager
{
public:
    void Init(/* ServiceProvider& sp */)
    {
        // CommandManager& cmdMan = sp.getCommandManager();
        // cmdMan.Register(this, commands);
        //
        // That's it. No linking, no lifetime management. Delete this
        // manager's folder and its commands vanish with it.
    }

private:
    // Static trampoline casts ctx back to the owner. Being a member,
    // it may call private methods.
    static void DoWifiScan(void* ctx, const char* json, JsonWriter& resp)
    {
        static_cast<NetworkManager*>(ctx)->DoWifiScanImpl(json, resp);
    }

    void DoWifiScanImpl(const char* json, JsonWriter& resp)
    {
        // ... actual work
    }

    // `inline static` member → static storage duration → lives for the
    // whole program, which is exactly what the dtor guard demands.
    // RAM cost: ~24 bytes per command. ctx/next/registered are filled
    // in by Register().
    inline static CommandEntry commands[] = {
        { "wifiScan", &NetworkManager::DoWifiScan },
        // { "wifiStatus", &NetworkManager::DoWifiStatus },
    };
};

// ──────────────────────────────────────────────────────────────
// The failure mode, and why it cannot ship
// ──────────────────────────────────────────────────────────────
//
//  void BADBADEXAMPLE(CommandManager& cmdMan)
//  {
//      CommandEntry testCommands[] = {                 // ← stack!
//          { "justtesting", &SomeHandler },
//      };
//      cmdMan.Register(nullptr, testCommands);
//      // ... scope ends → ~CommandEntry() sees registered == true
//      //     → ESP_LOGE + abort() → device resets.
//      // Fails on the FIRST run of this code path, every run. The
//      // mistake cannot survive a single test on the desk.
//  }
//
// ──────────────────────────────────────────────────────────────
// DECIDED — authentication stays OUT of the registry
// ──────────────────────────────────────────────────────────────
// The old table had `bool requiresAuth` per command. That mixes a
// security policy into a routing table — cross-contamination of
// responsibilities — so the registry knows nothing about auth.
//
// Interim, to preserve today's PIN behavior: handlers that guard
// state-changing operations call the existing CheckAuth helper
// themselves, first thing in the handler body. Auth stays available
// as a utility; commands that need it opt in where the work happens.
//
// If a richer model is ever needed (session auth, permission levels),
// it should be flags/enum on the SESSION or transport — not a bool
// per command entry.
