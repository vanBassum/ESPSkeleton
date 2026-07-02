#include "CommandManager.h"
#include "SettingsManager.h"
#include "JsonWriter.h"
#include "JsonHelpers.h"
#include "DateTime.h"
#include "esp_log.h"
#include "esp_app_desc.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cstring>

CommandManager::CommandManager(ServiceProvider& serviceProvider)
    : serviceProvider_(serviceProvider)
{
}

void CommandManager::Init()
{
    auto initAttempt = initState_.TryBeginInit();
    if (!initAttempt)
    {
        ESP_LOGW(TAG, "Already initialized or initializing");
        return;
    }

    Register(this, commands_);

    initAttempt.SetReady();
    ESP_LOGI(TAG, "Initialized");
}

bool CommandManager::Execute(const char* type, const char* json, JsonWriter& resp)
{
    const CommandEntry* e = Find(type);
    if (e == nullptr)
        return false;

    // Handler runs OUTSIDE the lock: entries are immortal, so the pointer
    // stays valid, and a handler may register commands or dispatch nested
    // commands without deadlocking.
    e->handler(e->ctx, json, resp);
    return true;
}

const CommandEntry* CommandManager::Find(const char* name)
{
    LOCK(mutex_);
    for (CommandEntry* e = head_; e != nullptr; e = e->next)
        if (strcmp(name, e->name) == 0)
            return e;
    return nullptr;
}

bool CommandManager::CheckAuth(const char* json, JsonWriter& resp)
{
    char storedPin[64] = {};
    serviceProvider_.getSettingsManager().getString("device.pin", storedPin, sizeof(storedPin));

    // No PIN configured — auth disabled
    if (storedPin[0] == '\0')
        return true;

    char pin[64] = {};
    ExtractJsonString(json, "pin", pin, sizeof(pin));

    if (strcmp(pin, storedPin) == 0)
        return true;

    ESP_LOGW(TAG, "Auth failed for command");
    resp.field("ok", false);
    resp.field("error", "auth");
    return false;
}

// ──────────────────────────────────────────────────────────────
// Built-in commands
// ──────────────────────────────────────────────────────────────

void CommandManager::Cmd_Ping(void* ctx, const char* json, JsonWriter& resp)
{
    resp.field("pong", true);
}

void CommandManager::Cmd_Info(void* ctx, const char* json, JsonWriter& resp)
{
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

void CommandManager::Cmd_Reboot(void* ctx, const char* json, JsonWriter& resp)
{
    auto* self = static_cast<CommandManager*>(ctx);
    if (!self->CheckAuth(json, resp))
        return;

    resp.field("ok", true);
    // Delay to allow WS response to be sent before restarting
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}
