#pragma once

#include "ServiceProvider.h"
#include "InitState.h"
#include "CommandEntry.h"
#include "RecursiveMutex.h"
#include "ContextLock.h"
#include <cstring>
#include <cassert>
#include <cstddef>

class JsonWriter;

class CommandManager {
    static constexpr const char* TAG = "CommandManager";

public:
    explicit CommandManager(ServiceProvider& serviceProvider);

    CommandManager(const CommandManager&) = delete;
    CommandManager& operator=(const CommandManager&) = delete;
    CommandManager(CommandManager&&) = delete;
    CommandManager& operator=(CommandManager&&) = delete;

    void Init();

    /// Register a table of commands. `commands` MUST have static storage
    /// duration (see ~CommandEntry). `ctx` (usually the owner's `this`) is
    /// stamped into every entry and handed back to its handler at dispatch.
    ///
    /// Thread-safe, and usable from construction — managers whose Init()
    /// runs before CommandManager's may register safely.
    template <size_t N>
    void Register(void* ctx, CommandEntry (&commands)[N])
    {
        LOCK(mutex_);
        for (size_t i = 0; i < N; ++i)
        {
            // Re-registering would re-link an entry already in the chain
            // and cycle it → Execute() would hang. Fail loudly on first
            // boot instead.
            assert(!commands[i].registered && "command registered twice");
            assert(Find(commands[i].name) == nullptr && "duplicate command name");

            commands[i].ctx = ctx;
            commands[i].registered = true;
            commands[i].next = head_;
            head_ = &commands[i];
        }
    }

    /// Execute a command by type name. Writes response fields into the JsonWriter.
    /// The caller is responsible for the outer object and transport-specific fields (e.g. "id").
    /// Returns true if the command was recognized.
    bool Execute(const char* type, const char* json, JsonWriter& resp);

    /// PIN check helper for handlers that guard state-changing commands.
    /// Call as the FIRST line of such handlers. Returns true if authorized;
    /// otherwise writes {ok:false, error:"auth"} into resp.
    /// (Auth is deliberately NOT part of the registry — see the design spec.)
    bool CheckAuth(const char* json, JsonWriter& resp);

private:
    ServiceProvider& serviceProvider_;
    InitState initState_;

    RecursiveMutex mutex_;
    CommandEntry* head_ = nullptr;

    // Locks internally (recursive, so Register may call it under its own
    // lock). Handing the pointer out after unlock is safe because entries
    // are immortal and name/handler/ctx are written before linking.
    const CommandEntry* Find(const char* name);

    // Built-in generic commands (everything domain-specific lives in the
    // manager that owns the domain).
    static void Cmd_Ping(void* ctx, const char* json, JsonWriter& resp);
    static void Cmd_Info(void* ctx, const char* json, JsonWriter& resp);
    static void Cmd_Reboot(void* ctx, const char* json, JsonWriter& resp);

    inline static CommandEntry commands_[] = {
        { "ping",   &CommandManager::Cmd_Ping },
        { "info",   &CommandManager::Cmd_Info },
        { "reboot", &CommandManager::Cmd_Reboot },
    };
};
