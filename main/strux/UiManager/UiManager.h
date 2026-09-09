#pragma once

#include "StruxProvider.h"
#include "InitState.h"
#include "CommandEntry.h"
#include "UiModule.h"
#include "Mutex.h"
#include <initializer_list>

// ──────────────────────────────────────────────────────────────
// The registry of UI modules this firmware ships, and the one command that reports it.
//
// `ui modules` is how a shell — the device's own, or the relay's — learns which pages a
// device offers. It is a COMMAND and not a file in `www`, for the same reason `help
// list` and `settings list` are commands: the command surface is how this device
// describes itself, and its HTTP server hands static files to a browser on the LAN and
// does nothing else. Two things fall out of that and both are worth having:
//
//   • one mechanism for both shells — a shell reached through the relay asks the same
//     question the same way, over the pipe it already has,
//   • the answer cannot go stale, because it never passes through the relay's file
//     cache the way a `modules.json` would have.
//
// Nothing here knows what a module *is* beyond its declaration. The bundle is bytes in
// `www` that a shell imports; this manager says which ones exist and what they claim to
// contribute, and the claim is matched against what the module registers on activation.
// ──────────────────────────────────────────────────────────────
class UiManager
{
    static constexpr const char* TAG = "UiManager";

    // The shell/module contract this firmware's bundles were built against. A range
    // rather than a single number so a shell can be newer than the device and still
    // load its modules; a plain integer rather than a list of capability strings
    // because one party owns the firmware and both shells (see docs/reasoning). It
    // versions the whole host contract, the frontend runtime included, so a React major
    // in the shells moves it.
    static constexpr uint32_t HOST_API_MIN = 1;
    static constexpr uint32_t HOST_API_MAX = 1;

public:
    explicit UiManager(StruxProvider& strux);

    UiManager(const UiManager&) = delete;
    UiManager& operator=(const UiManager&) = delete;
    UiManager(UiManager&&) = delete;
    UiManager& operator=(UiManager&&) = delete;

    void Init();

    /// Called from an application manager's Init(). Entries MUST have static storage
    /// duration — the registry keeps the pointer and reads it on every `ui modules`.
    void Register(std::initializer_list<UiModule*> modules);

private:
    StruxProvider& strux_;
    InitState initState_;
    Mutex mutex_;

    /// Intrusive chain, head-inserted — so modules report in reverse registration
    /// order, exactly as settings do. Nothing depends on the order; a shell keys on id.
    UiModule* head_ = nullptr;

    RequestError Cmd_Modules(CommandContext& ctx);

    inline static CommandEntry commands_[] = {
        { "ui", "modules", &InvokeCommand<&UiManager::Cmd_Modules> },
    };
};
