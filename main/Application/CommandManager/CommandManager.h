#pragma once

#include "ServiceProvider.h"
#include "InitState.h"
#include "CommandEntry.h"
#include "RecursiveMutex.h"
#include "ContextLock.h"
#include <cstring>
#include <cassert>
#include <cstddef>

class Stream;

// Pure dispatcher — knows no commands and no other managers. Every
// command lives in the manager that owns its domain and is registered
// from that manager's Init().
//
// Deliberately knows nothing about sessions, transports or the wire format: give
// it a command name and two streams and it runs the handler. That is what keeps it
// the one piece of the request path that can be reasoned about on its own.
//
// Naming a request from its envelope is the protocol layer's job — see
// protocol::RunCommandSession, which every transport calls.
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
            // and cycle it → Execute() would hang. Chain-corruption class,
            // so FATAL (survives NDEBUG), not assert.
            if (commands[i].registered)
                FATAL("command '%s %s' registered twice",
                      commands[i].category, commands[i].name);
            assert(Find(commands[i].category, commands[i].name) == nullptr &&
                   "duplicate command name");

            commands[i].ctx = ctx;
            commands[i].registered = true;
            commands[i].next = head_;
            head_ = &commands[i];
        }
    }

    /// Execute a command by type name. `in` carries the request payload;
    /// the handler writes its complete reply (e.g. one JSON object) to
    /// `out`. The caller owns any transport envelope around it.
    ///
    /// Returns Ok, UnknownCommand, or whatever a failed argument pull reported.
    /// `failedArg` (when non-null) receives the argument name a pull was looking
    /// for, so the caller can compose the refusal text.
    ///
    /// Whether the request has already been read depends on the handler's shape:
    /// an argument-pulling handler gets its envelope consumed by the framework
    /// first, an unconverted one still parses `in` itself. See CommandEntry.
    RequestError Execute(const char* category, const char* name,
                         Stream& in, Stream& out,
                         const char** failedArg = nullptr);

private:
    ServiceProvider& serviceProvider_;
    InitState initState_;

    RecursiveMutex mutex_;
    CommandEntry* head_ = nullptr;

    // Locks internally (recursive, so Register may call it under its own
    // lock). Handing the pointer out after unlock is safe because entries
    // are immortal and name/handler/ctx are written before linking.
    const CommandEntry* Find(const char* category, const char* name);
};
