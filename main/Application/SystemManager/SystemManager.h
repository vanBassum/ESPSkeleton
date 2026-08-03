#pragma once

#include "ServiceProvider.h"
#include "InitState.h"
#include "CommandEntry.h"
#include "TypedSettings.h"

class Stream;

// ──────────────────────────────────────────────────────────────
// SystemManager owns device identity and lifecycle — and nothing
// else (no timers, no watchdogs; those belong elsewhere):
//   - device.name: exposed to other managers via GetDeviceName()
//     (settings are private to their owner — nobody else reads the
//     key)
//   - the generic system commands: ping / info / reboot
// ──────────────────────────────────────────────────────────────
class SystemManager
{
    static constexpr const char* TAG = "SystemManager";

public:
    explicit SystemManager(ServiceProvider& serviceProvider);

    SystemManager(const SystemManager&) = delete;
    SystemManager& operator=(const SystemManager&) = delete;
    SystemManager(SystemManager&&) = delete;
    SystemManager& operator=(SystemManager&&) = delete;

    void Init();

    /// Copies the device name into `out`; falls back to "Strux" if the
    /// stored value is empty.
    void GetDeviceName(char* out, size_t maxLen);

private:
    ServiceProvider& serviceProvider_;
    InitState initState_;

    // ── Settings (registered with SettingsManager in Init) ──
    inline static StringSetting name_{ "device.name", "Device Name", "Strux" };

    // ── WebSocket commands (registered with CommandManager in Init) ──
    RequestError Cmd_Ping(CommandContext& ctx);
    RequestError Cmd_Info(CommandContext& ctx);
    RequestError Cmd_Reboot(CommandContext& ctx);

    inline static CommandEntry commands_[] = {
        { "system", "ping",   &InvokeCommand<&SystemManager::Cmd_Ping> },
        { "system", "info",   &InvokeCommand<&SystemManager::Cmd_Info> },
        { "system", "reboot", &InvokeCommand<&SystemManager::Cmd_Reboot> },
    };
};
