#include "SystemManager.h"
#include "SettingsManager.h"
#include "CommandManager.h"
#include "JsonScope.h"
#include "DateTime.h"
#include "esp_log.h"
#include "esp_app_desc.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cstring>

SystemManager::SystemManager(ServiceProvider& serviceProvider)
    : serviceProvider_(serviceProvider)
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

    serviceProvider_.getSettingsManager().Register({ &name_ });
    serviceProvider_.getCommandManager().Register(this, commands_);

    initAttempt.SetReady();
    ESP_LOGI(TAG, "Initialized");
}

void SystemManager::GetDeviceName(char* out, size_t maxLen)
{
    name_.Get(out, maxLen);
    if (out[0] == '\0')
        snprintf(out, maxLen, "Strux");
}

// ──────────────────────────────────────────────────────────────
// WebSocket commands
// ──────────────────────────────────────────────────────────────

void SystemManager::Cmd_Ping(Stream& in, Stream& out)
{
    JsonObject resp(out);
    resp.field("pong", true);
}

void SystemManager::Cmd_Info(Stream& in, Stream& out)
{
    JsonObject resp(out);

    const esp_app_desc_t* app = esp_app_get_description();

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
}

void SystemManager::Cmd_Reboot(Stream& in, Stream& out)
{
    {
        JsonObject resp(out);
        resp.field("ok", true);
    }   // close the scope BEFORE restarting so the reply is complete

    // Delay to allow WS response to be sent before restarting
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}
