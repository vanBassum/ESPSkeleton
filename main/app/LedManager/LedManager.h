#pragma once

#include "AppProvider.h"
#include "InitState.h"
#include "CommandEntry.h"
#include "TypedSettings.h"
#include "Timer.h"

// ──────────────────────────────────────────────────────────────
// The worked example of an application manager. It blinks the LED, and in doing so
// touches every edge the layering allows and none that it forbids:
//
//   • the board, for the hardware        — app_.getBoard().GetLed()
//   • the framework, for settings        — led.blink / led.period, in the settings UI
//                                          with no UI code written
//   • the framework, for commands        — `led get` / `led set`, in `help list` and
//                                          reachable over WebSocket and the relay alike
//   • the framework, for telemetry       — a point when the LED is switched
//
// Note what it does NOT do: nothing in Strux knows this class exists. It is not named in
// StruxContext, not in StruxProvider, and not in main.cpp. It announces itself by
// registering, from its own Init(), into managers it found through AppProvider. Copy this
// file to start a feature; delete it when the product has real ones.
// ──────────────────────────────────────────────────────────────
class LedManager
{
    static constexpr const char* TAG = "LedManager";

public:
    explicit LedManager(AppProvider& app);

    LedManager(const LedManager&) = delete;
    LedManager& operator=(const LedManager&) = delete;
    LedManager(LedManager&&) = delete;
    LedManager& operator=(LedManager&&) = delete;

    void Init();

    /// Blink, or hold the LED at a fixed state. Public because it is this manager's
    /// domain API — another app manager would call this, not the command.
    void SetBlinking(bool blinking);
    void SetOn(bool on);

    bool IsBlinking() const { return blinking_; }

private:
    AppProvider& app_;
    InitState initState_;
    Timer timer_;
    bool blinking_ = false;

    /// Runs on the FreeRTOS timer service task, whose stack is small — so this toggles a
    /// GPIO and re-arms, and nothing more. A telemetry Point does not fit here (see
    /// TelemetryManager), which is why the points are taken in the command handlers.
    void OnTick();

    void ApplyPeriod();

    // ── Settings. Keys are at most 15 characters — NVS's limit, asserted at RUNTIME in
    // Register(), so an over-long key compiles fine and then boot-loops the device.
    inline static BoolSetting   blinkOnBoot_{ "led.blink",  "Blink LED on boot", true };
    inline static UInt32Setting periodMs_{ "led.period", "Blink Period (ms)", 1000 };

    // ── Commands ──
    RequestError Cmd_Get(CommandContext& ctx);
    RequestError Cmd_Set(CommandContext& ctx);

    inline static CommandEntry commands_[] = {
        { "led", "get", &InvokeCommand<&LedManager::Cmd_Get> },
        { "led", "set", &InvokeCommand<&LedManager::Cmd_Set> },
    };
};
