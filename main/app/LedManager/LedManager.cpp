#include "LedManager.h"
#include "BoardContext.h"
#include "StruxProvider.h"
#include "SettingsManager.h"
#include "CommandManager.h"
#include "RelayManager.h"
#include "TelemetryManager.h"
#include "esp_log.h"

LedManager::LedManager(AppProvider& app)
    : app_(app)
{
}

void LedManager::Init()
{
    auto initAttempt = initState_.TryBeginInit();
    if (!initAttempt)
    {
        ESP_LOGW(TAG, "Already initialized or initializing");
        return;
    }

    // Reaching DOWN into the framework, which is the only direction allowed. Nothing in
    // Strux was edited to make these two lines work.
    StruxProvider& strux = app_.getStrux();
    strux.getSettingsManager().Register({ &enabled_ });
    strux.getCommandManager().Register(this, commands_);

    timer_.SetHandler([this] { OnTick(); });
    timer_.Init("led", pdMS_TO_TICKS(POLL_MS));

    // The LED leaves Init() saying what is true right now — dark, because the relay
    // cannot be up this early — rather than in whatever state the driver left behind.
    Apply();
    timer_.Start();

    initAttempt.SetReady();
    ESP_LOGI(TAG, "Initialized");
}

void LedManager::SetEnabled(bool enabled)
{
    enabled_.Set(enabled);
    app_.getStrux().getSettingsManager().Save();

    // Don't wait for the next tick: a switch in the dashboard that takes a visible
    // moment to reach the board reads as a switch that did not work.
    Apply();
}

void LedManager::Apply()
{
    const bool want = enabled_.Get() && app_.getStrux().getRelayManager().IsConnected();

    Led& led = app_.getBoard().GetLed();
    if (want != led.IsOn())
        led.Set(want);
}

// ──────────────────────────────────────────────────────────────
// Commands. These appear in `help list` and work over the local WebSocket and the relay
// alike, because a handler serves neither — it serves a CommandContext.
// ──────────────────────────────────────────────────────────────

RequestError LedManager::Cmd_Get(CommandContext& ctx)
{
    RETURN_IF_ERROR(ctx.readArgs());

    auto resp = ctx.reply.object();
    resp.field("enabled", enabled_.Get());
    resp.field("connected", app_.getStrux().getRelayManager().IsConnected());
    resp.field("on", app_.getBoard().GetLed().IsOn());
    return RequestError::Ok;
}

RequestError LedManager::Cmd_Set(CommandContext& ctx)
{
    // "Absent means leave it alone" needs no has() here: the destination starts at the
    // current state, so an Optional the caller omitted simply writes itself back.
    bool enabled = enabled_.Get();

    RETURN_IF_ERROR(ctx.readArgs(
        Optional("enabled", enabled)
    ));

    SetEnabled(enabled);

    // Telemetry from a command handler rather than from OnTick(): a Point is a few
    // hundred bytes of stack, which the timer service task has not got.
    auto point = app_.getStrux().getTelemetryManager().Measure("led");
    point.Tag("mode", enabled ? "link" : "off");
    point.Field("on", app_.getBoard().GetLed().IsOn());
    point.Commit();

    auto resp = ctx.reply.object();
    resp.field("ok", true);
    resp.field("enabled", enabled_.Get());
    resp.field("connected", app_.getStrux().getRelayManager().IsConnected());
    resp.field("on", app_.getBoard().GetLed().IsOn());
    return RequestError::Ok;
}
