#pragma once

#include "SessionMux.h"

class CommandManager;

// Routes an opened session to CommandManager: read the command type off the
// header line, run the handler with the session as both `in` and `out`, close the
// reply. Transport-agnostic — the local WebSocket and the relay pipe each own one,
// so this dispatch logic exists exactly once regardless of how many transports
// feed the mux.
class CommandSink : public SessionMux::Sink
{
    static constexpr const char* TAG = "CommandSink";

public:
    void SetCommandManager(CommandManager& commands) { commands_ = &commands; }

    void OnSessionOpened(Session& session) override;

private:
    CommandManager* commands_ = nullptr;
};
