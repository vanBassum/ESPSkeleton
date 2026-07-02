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
//   • Registration is Init()-time only, single-threaded, and the
//     chain is IMMUTABLE afterwards → dispatch needs no mutex.
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

    CommandEntry* head_ = nullptr;

public:
    // Called from a manager's Init(). Takes the array by reference so
    // the count is deduced — it can never be wrong. `ctx` (usually the
    // owner's `this`) is stamped into every entry and handed back to
    // its handler at dispatch.
    //
    // Registration happens during the ordered Init() sequence in
    // main.cpp — single-threaded, and strictly before WebServerManager
    // (last in the sequence) can dispatch anything. After boot the
    // chain never changes, so no locking exists anywhere in here.
    template <size_t N>
    void Register(void* ctx, CommandEntry (&commands)[N])
    {
        for (size_t i = 0; i < N; ++i)
        {
            // Re-registering would re-link an entry that is already in
            // the chain and cycle it → Dispatch() would hang. Make the
            // mistake loud instead: fails on first boot, deterministic.
            assert(!commands[i].registered && "command registered twice");
            assert(Find(commands[i].name) == nullptr && "duplicate command name");

            commands[i].ctx = ctx;
            commands[i].registered = true;
            commands[i].next = head_;
            head_ = &commands[i];
        }
    }

    // Linear walk — ~15 commands in practice, cost is irrelevant.
    bool Dispatch(const char* type, const char* json, JsonWriter& resp)
    {
        for (CommandEntry* e = head_; e != nullptr; e = e->next)
        {
            if (strcmp(type, e->name) != 0)
                continue;
            e->handler(e->ctx, json, resp);
            return true;
        }
        return false;   // unknown command
    }

private:
    const CommandEntry* Find(const char* name) const
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
// OPEN QUESTION — authentication
// ──────────────────────────────────────────────────────────────
// The old table had `bool requiresAuth` per command; this sketch
// deliberately omits it. Two candidate models:
//
//   (a) Session-level auth: the WebSocket session authenticates once
//       (PIN on connect, if set); afterwards every command is allowed,
//       before that none are. Commands know nothing about auth — it is
//       purely a transport concern. Cleanest, but needs a small
//       frontend change and loses unauthenticated read-only access
//       (ping/info before login).
//
//   (b) Keep one `bool requiresAuth` (or `mutates`) per entry as a
//       declared fact about the command, enforced by the dispatcher
//       before the handler runs. Default should be "auth required" so
//       forgetting the flag fails safe.
