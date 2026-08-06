#include "SystemManager.h"
#include "SettingsManager.h"
#include "CommandManager.h"
#include "DateTime.h"
#include "esp_log.h"
#include "esp_app_desc.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cstring>

SystemManager::SystemManager(StruxServices& strux)
    : strux_(strux)
{
}

void SystemManager::Init()
{
    auto initAttempt = initState_.TryBeginInit();
    if (!initAttempt)
    {
        ESP_LOGW(TAG, "Already initialized or initializing");
        return;
    }

    strux_.getSettingsManager().Register({ &name_ });
    strux_.getCommandManager().Register(this, commands_);

    initAttempt.SetReady();
    ESP_LOGI(TAG, "Initialized");
}

void SystemManager::GetDeviceName(char* out, size_t maxLen)
{
    name_.Get(out, maxLen);
    if (out[0] == '\0')
        snprintf(out, maxLen, "%s", esp_app_get_description()->project_name);
}

// ──────────────────────────────────────────────────────────────
// WebSocket commands
// ──────────────────────────────────────────────────────────────

RequestError SystemManager::Cmd_Ping(CommandContext& ctx)
{
    RETURN_IF_ERROR(ctx.readArgs());

    auto resp = ctx.reply.object();
    resp.field("pong", true);
    return RequestError::Ok;
}

RequestError SystemManager::Cmd_Info(CommandContext& ctx)
{
    RETURN_IF_ERROR(ctx.readArgs());

    auto resp = ctx.reply.object();

    const esp_app_desc_t* app = esp_app_get_description();

    char deviceName[32] = {};
    GetDeviceName(deviceName, sizeof(deviceName));
    resp.field("name", deviceName);

    resp.field("project", app->project_name);
    resp.field("firmware", app->version);
    resp.field("idf", app->idf_ver);
    resp.field("date", app->date);
    resp.field("time", app->time);
    resp.field("chip", CONFIG_IDF_TARGET);
    resp.field("heapFree", static_cast<uint32_t>(esp_get_free_heap_size()));
    resp.field("heapMin", static_cast<uint32_t>(esp_get_minimum_free_heap_size()));

    char deviceTimeStr[32] = "Not synced";
    DateTime now = DateTime::Now();
    if (now.YearLocal() >= 2020)
        now.ToStringLocal(deviceTimeStr, sizeof(deviceTimeStr), "%F %T");
    resp.field("deviceTime", deviceTimeStr);
    return RequestError::Ok;
}

RequestError SystemManager::Cmd_Reboot(CommandContext& ctx)
{
    RETURN_IF_ERROR(ctx.readArgs());

    {
        auto resp = ctx.reply.object();
        resp.field("ok", true);
    }   // close the scope BEFORE restarting so the reply is complete

    // Delay to allow WS response to be sent before restarting
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}
