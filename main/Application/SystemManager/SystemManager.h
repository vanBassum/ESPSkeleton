#pragma once

#include "ServiceProvider.h"
#include "InitState.h"
#include "CommandEntry.h"
#include "TypedSettings.h"

class JsonWriter;

// ──────────────────────────────────────────────────────────────
// SystemManager owns device identity and lifecycle — and nothing
// else (no timers, no watchdogs; those belong elsewhere):
//   - device.name: exposed to other managers via GetDeviceName()
//     (settings are private to their owner — nobody else reads the
//     key)
//   - device.pin + CheckAuth(): the auth helper lives with the
//     credential it checks
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

    /// Compares a candidate PIN against the stored one. True when they
    /// match, or when no PIN is configured (auth disabled). The stored PIN
    /// never leaves this manager. Transport concerns (extracting the
    /// candidate from a JSON payload, writing an error response) live at
    /// the edge — see CommandManager/CommandAuth.h.
    bool CheckPin(const char* candidate);

private:
    ServiceProvider& serviceProvider_;
    InitState initState_;

    // ── Settings (registered with SettingsManager in Init) ──
    inline static StringSetting name_{ "device.name", "Device Name", "Strux" };
    inline static StringSetting pin_ { "device.pin",  "Device PIN",  ""      };

    // ── WebSocket commands (registered with CommandManager in Init) ──
    void Cmd_Ping(const char* json, JsonWriter& resp);
    void Cmd_Info(const char* json, JsonWriter& resp);
    void Cmd_Reboot(const char* json, JsonWriter& resp);

    inline static CommandEntry commands_[] = {
        { "ping",   &InvokeCommand<&SystemManager::Cmd_Ping> },
        { "info",   &InvokeCommand<&SystemManager::Cmd_Info> },
        { "reboot", &InvokeCommand<&SystemManager::Cmd_Reboot> },
    };
};
