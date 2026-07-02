#pragma once

// ══════════════════════════════════════════════════════════════
// DESIGN SKETCH — not built, not included anywhere.
//
// Command registry without dynamic memory, without std::function,
// and with command tables that live in FLASH (.rodata), so they
// can never be destroyed, never dangle, and never be corrupted.
//
// The three ideas, in one sentence each:
//   1. CommandEntry holds no runtime data (no `this`, no `next`),
//      so tables can be `static constexpr` → stored in flash.
//   2. `ctx` (the owner's `this`) is bound ONCE per table at
//      Register() time, not per entry.
//   3. Register() boot-asserts the table pointer is in flash, so
//      a stack/heap table fails on the first call, every time.
// ══════════════════════════════════════════════════════════════

#include <cstddef>
#include "esp_log.h"
#include "esp_memory_utils.h"   // esp_ptr_in_drom()
#include "JsonWriter.h"

// ──────────────────────────────────────────────────────────────
// CommandManager side
// ──────────────────────────────────────────────────────────────

// One command. A literal type: two pointers and a bool, nothing that
// needs constructing or destroying. Handlers are plain function
// pointers — captureless lambdas decay to these at compile time.
struct CommandEntry
{
    const char* name;
    bool requiresAuth;
    void (*handler)(void* ctx, const char* json, JsonWriter& resp);
};

class CommandManager
{
    static constexpr const char* TAG = "CommandManager";

    // One registered table (a "block"). One slot per registering
    // manager — NOT per command — so the pool is bounded by how many
    // managers exist, a number that changes rarely. Overflow asserts
    // on first boot, deterministically.
    struct Block
    {
        const CommandEntry* entries;
        size_t count;
        void* ctx;
    };

    static constexpr size_t kMaxBlocks = 16;
    Block blocks_[kMaxBlocks] = {};
    size_t blockCount_ = 0;

public:
    // Called from a manager's Init(). `entries` MUST point to a
    // static constexpr table (flash). `ctx` is handed back to every
    // handler in the table — pass `this`.
    void Register(const CommandEntry* entries, size_t count, void* ctx)
    {
        // Lifetime is not a convention here, it is a checked invariant:
        // constexpr tables live in DROM (flash rodata). Anything on the
        // stack or heap is in DRAM and fails immediately — see
        // BADBADEXAMPLE below. If it registered, it is immortal.
        assert(esp_ptr_in_drom(entries) && "command table must be static constexpr (flash)");

        assert(blockCount_ < kMaxBlocks && "raise kMaxBlocks");

        // Registration runs unconditionally at Init(), so a duplicate
        // name is caught on the very first boot after it is introduced.
        for (size_t i = 0; i < count; i++)
            assert(Find(entries[i].name) == nullptr && "duplicate command name");

        blocks_[blockCount_++] = { entries, count, ctx };
    }

    // Dispatch: linear walk over blocks then entries. ~15 commands
    // total in practice — lookup cost is irrelevant.
    bool Dispatch(const char* type, const char* json, JsonWriter& resp)
    {
        for (size_t b = 0; b < blockCount_; b++)
            for (size_t i = 0; i < blocks_[b].count; i++)
            {
                const CommandEntry& e = blocks_[b].entries[i];
                if (strcmp(type, e.name) != 0)
                    continue;
                if (e.requiresAuth && !CheckAuth(json, resp))
                    return true;
                e.handler(blocks_[b].ctx, json, resp);
                return true;
            }
        return false;   // unknown command
    }

private:
    const CommandEntry* Find(const char* name) const; // omitted
    bool CheckAuth(const char* json, JsonWriter& resp); // unchanged from today
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
        // cmdMan.Register(kCommands, std::size(kCommands), this);
        //
        // That's it. No linking, no unregistering, no lifetime to
        // manage. Delete this manager's folder and its commands
        // vanish with it.
    }

private:
    void CmdWifiScan(const char* json, JsonWriter& resp)
    {
        // ... actual work, a normal private member with full access
    }

    // The table. `static constexpr` on a type with no runtime data
    // puts this in .rodata → flash. Zero bytes of RAM per command.
    // The lambdas are captureless, so they convert to plain function
    // pointers at compile time; `ctx` arrives at dispatch and is cast
    // back to the owner. Being defined in-class, the lambda may call
    // private members.
    static constexpr CommandEntry kCommands[] = {
        { "wifiScan", false,
          [](void* ctx, const char* json, JsonWriter& resp)
              { static_cast<NetworkManager*>(ctx)->CmdWifiScan(json, resp); } },

        // { "wifiStatus", false, ... },   // more commands: more lines,
        //                                 // zero more RAM
    };
};

// ──────────────────────────────────────────────────────────────
// The failure mode, and why it cannot ship
// ──────────────────────────────────────────────────────────────
//
//  void BADBADEXAMPLE(CommandManager& cmdMan)
//  {
//      CommandEntry testCommands[] = {                 // ← stack (DRAM)
//          { "justtesting", false, someHandler },
//      };
//      cmdMan.Register(testCommands, 1, nullptr);
//      // ^ asserts HERE, on the first call, every boot:
//      //   esp_ptr_in_drom(testCommands) is false for stack, heap,
//      //   and even static non-const RAM arrays. Only genuinely
//      //   immortal flash tables get in, so the registry can never
//      //   hold a dangling pointer. The bad example is not
//      //   discouraged — it is impossible.
//  }
//
// Conditional registration (rare): a manager that only exposes some
// commands in some configs splits them into two constexpr tables and
// calls Register() twice behind an `if`. Blocks are cheap (12 bytes).
