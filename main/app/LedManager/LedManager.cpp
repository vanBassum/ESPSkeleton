#include "LedManager.h"
#include "BoardContext.h"
#include "StruxProvider.h"
#include "SettingsManager.h"
#include "CommandManager.h"
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
    strux.getSettingsManager().Register({ &blinkOnBoot_, &periodMs_ });
    strux.getCommandManager().Register(this, commands_);

    timer_.SetHandler([this] { OnTick(); });
    timer_.Init("led", pdMS_TO_TICKS(periodMs_.Get()));

    // Either way the LED leaves Init() in a state this manager chose, rather than in
    // whatever the driver happened to leave behind.
    if (blinkOnBoot_.Get())
        SetBlinking(true);
    else
        SetOn(false);

    initAttempt.SetReady();
    ESP_LOGI(TAG, "Initialized");
}

void LedManager::SetBlinking(bool blinking)
{
    blinking_ = blinking;

    if (blinking)
    {
        ApplyPeriod();
        timer_.Start();
        return;
    }

    timer_.Stop();
    app_.getBoard().GetLed().Off();
}

void LedManager::SetOn(bool on)
{
    if (blinking_)
        SetBlinking(false);

    app_.getBoard().GetLed().Set(on);
}

void LedManager::OnTick()
{
    app_.getBoard().GetLed().Toggle();

    // Re-read the setting every tick so a period changed in the settings UI takes effect
    // on the next one. Get() is a field read, not an NVS read, so this is free.
    ApplyPeriod();
}

void LedManager::ApplyPeriod()
{
    const TickType_t want = pdMS_TO_TICKS(periodMs_.Get());

    TickType_t have = 0;
    if (!timer_.GetPeriod(have) || have == want)
        return;

    // Timeout of 0, never the default portMAX_DELAY: this also runs from OnTick(), and
    // blocking inside the timer service task is how that task deadlocks against its own
    // command queue. A period change that cannot be queued right now lands on the tick
    // after, which for a blink is no change at all.
    timer_.SetPeriod(want, 0);
}

// ──────────────────────────────────────────────────────────────
// Commands. These appear in `help list` and work over the local WebSocket and the relay
// alike, because a handler serves neither — it serves a CommandContext.
// ──────────────────────────────────────────────────────────────

RequestError LedManager::Cmd_Get(CommandContext& ctx)
{
    RETURN_IF_ERROR(ctx.readArgs());

    auto resp = ctx.reply.object();
    resp.field("on", app_.getBoard().GetLed().IsOn());
    resp.field("blinking", blinking_);
    resp.field("periodMs", periodMs_.Get());
    return RequestError::Ok;
}

RequestError LedManager::Cmd_Set(CommandContext& ctx)
{
    // "Absent means leave it alone" needs no has() here: the destinations start at the
    // current state, so an Optional the caller omitted simply writes itself back.
    bool blink = blinking_;
    bool on    = app_.getBoard().GetLed().IsOn();

    RETURN_IF_ERROR(ctx.readArgs(
        Optional("blink", blink),
        Optional("on",    on)
    ));

    if (blink)
        SetBlinking(true);
    else
        SetOn(on);

    // Telemetry from a command handler rather than from OnTick(): a Point is a few
    // hundred bytes of stack, which the timer service task has not got.
    auto point = app_.getStrux().getTelemetryManager().Measure("led");
    point.Tag("mode", blinking_ ? "blink" : "static");
    point.Field("on", app_.getBoard().GetLed().IsOn());
    point.Commit();

    auto resp = ctx.reply.object();
    resp.field("ok", true);
    resp.field("on", app_.getBoard().GetLed().IsOn());
    resp.field("blinking", blinking_);
    return RequestError::Ok;
}
