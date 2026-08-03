#pragma once

#include "ServiceProvider.h"
#include "InitState.h"
#include "CommandEntry.h"
#include "RecursiveMutex.h"
#include "ContextLock.h"
#include "SessionMux.h"
#include <cstring>
#include <cassert>
#include <cstddef>

// Pure dispatcher — knows no commands and no other managers. Every
// command lives in the manager that owns its domain and is registered
// from that manager's Init().
//
// It is also the SessionMux::Sink: routing an opened session means reading the
// routing key off it, which is the router's own job. Every transport hands its
// sessions here, so dispatch exists once no matter how many transports there
// are, with no adapter class in between.
class CommandManager : public SessionMux::Sink {
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
                FATAL("command '%s' registered twice", commands[i].name);
            assert(Find(commands[i].name) == nullptr && "duplicate command name");

            commands[i].ctx = ctx;
            commands[i].registered = true;
            commands[i].next = head_;
            head_ = &commands[i];
        }
    }

    /// Execute a command by type name. `in` carries the request payload;
    /// the handler writes its complete reply (e.g. one JSON object) to
    /// `out`. The caller owns any transport envelope around it.
    /// Returns true if the command was recognized.
    bool Execute(const char* type, Stream& in, Stream& out);

    /// Dispatch a session opened by any transport: peek its header line for
    /// `type`, run the handler with the session as both `in` and `out`, close the
    /// reply. Rejects an unparseable or unknown type.
    ///
    /// The envelope convention this implies, and which handlers rely on: a
    /// request starts with a single '\n'-terminated line of JSON, and the body —
    /// if any — is whatever bytes follow it in the same session.
    ///
    ///     {"type":"updateWrite","partition":"ota_1"}\n<firmware bytes…>
    ///
    /// Dispatch only *peeks* that line, so a handler with a body consumes its own
    /// envelope on line one (StringReader::readLine) and then reads the body. A
    /// handler without a body just parses the line as its JSON request.
    void OnSessionOpened(Session& session) override;

private:
    ServiceProvider& serviceProvider_;
    InitState initState_;

    RecursiveMutex mutex_;
    CommandEntry* head_ = nullptr;

    // Locks internally (recursive, so Register may call it under its own
    // lock). Handing the pointer out after unlock is safe because entries
    // are immortal and name/handler/ctx are written before linking.
    const CommandEntry* Find(const char* name);
};
