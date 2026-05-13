#include "CommandManager.h"
#include "ConsoleManager.h"
#include "SettingsManager.h"
#include "UpdateManager.h"
#include "JsonWriter.h"
#include "JsonHelpers.h"
#include "DateTime.h"
#include "esp_log.h"
#include "esp_app_desc.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "NetworkManager.h"
#include <cstring>

const CommandManager::CommandEntry CommandManager::commands_[] = {
    { "ping",         &CommandManager::Cmd_Ping,         false },
    { "info",         &CommandManager::Cmd_Info,         false },
    { "updateStatus", &CommandManager::Cmd_UpdateStatus, false },
    { "getSettings",  &CommandManager::Cmd_GetSettings,  false },
    { "setSetting",   &CommandManager::Cmd_SetSetting,   true  },
    { "saveSettings", &CommandManager::Cmd_SaveSettings, true  },
    { "reboot",       &CommandManager::Cmd_Reboot,       true  },
    { "wifiScan",     &CommandManager::Cmd_WifiScan,     false },
    { "getLogs",      &CommandManager::Cmd_GetLogs,      false },
    { "partitions",   &CommandManager::Cmd_Partitions,   false },
    { nullptr, nullptr, false },
};

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

    initAttempt.SetReady();
    ESP_LOGI(TAG, "Initialized");
}

bool CommandManager::Execute(const char* type, const char* json, JsonWriter& resp)
{
    for (int i = 0; commands_[i].type != nullptr; i++)
    {
        if (strcmp(type, commands_[i].type) == 0)
        {
            if (commands_[i].requiresAuth && !CheckAuth(json, resp))
                return true;

            (this->*commands_[i].func)(json, resp);
            return true;
        }
    }

    return false;
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
// Commands
// ──────────────────────────────────────────────────────────────

void CommandManager::Cmd_Ping(const char* json, JsonWriter& resp)
{
    resp.field("pong", true);
}

void CommandManager::Cmd_Info(const char* json, JsonWriter& resp)
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

void CommandManager::Cmd_UpdateStatus(const char* json, JsonWriter& resp)
{
    const esp_app_desc_t* app = esp_app_get_description();
    auto& update = serviceProvider_.getUpdateManager();

    resp.field("firmware", app->version);
    resp.field("running", update.GetRunningPartition());
    resp.field("nextSlot", update.GetNextPartition());
}

void CommandManager::Cmd_GetSettings(const char* json, JsonWriter& resp)
{
    serviceProvider_.getSettingsManager().WriteAllSettings(resp);
}

void CommandManager::Cmd_SetSetting(const char* json, JsonWriter& resp)
{
    char key[64] = {};
    char value[128] = {};
    ExtractJsonString(json, "key", key, sizeof(key));
    ExtractJsonString(json, "value", value, sizeof(value));

    if (key[0] == '\0')
    {
        resp.field("ok", false);
        resp.field("error", "missing key");
        return;
    }

    auto& settings = serviceProvider_.getSettingsManager();
    const auto* defs = settings.GetDefinitions();
    int count = settings.GetDefinitionCount();

    for (int i = 0; i < count; i++)
    {
        if (strcmp(defs[i].key, key) == 0)
        {
            switch (defs[i].type)
            {
            case SettingType::String:
                settings.setString(defs[i].key, value);
                break;
            case SettingType::Int:
                settings.setInt(defs[i].key, atoi(value));
                break;
            case SettingType::Bool:
                settings.setBool(defs[i].key, strcmp(value, "true") == 0 || strcmp(value, "1") == 0);
                break;
            }

            resp.field("ok", true);
            return;
        }
    }

    resp.field("ok", false);
    resp.field("error", "unknown key");
}

void CommandManager::Cmd_SaveSettings(const char* json, JsonWriter& resp)
{
    bool ok = serviceProvider_.getSettingsManager().Save();
    resp.field("ok", ok);
}

void CommandManager::Cmd_Reboot(const char* json, JsonWriter& resp)
{
    resp.field("ok", true);
    // Delay to allow WS response to be sent before restarting
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

void CommandManager::Cmd_WifiScan(const char* json, JsonWriter& resp)
{
    auto& wifi = serviceProvider_.getNetworkManager().wifi();

    WiFiInterface::ScanResult results[20] = {};
    int count = wifi.Scan(results, 20);

    resp.field("ok", true);
    resp.fieldArray("networks");

    for (int i = 0; i < count; i++)
    {
        resp.beginObject();
        resp.field("ssid", results[i].ssid);
        resp.field("rssi", static_cast<int32_t>(results[i].rssi));
        resp.field("channel", static_cast<int32_t>(results[i].channel));
        resp.field("secure", results[i].secure);
        resp.endObject();
    }

    resp.endArray();
}

void CommandManager::Cmd_GetLogs(const char* json, JsonWriter& resp)
{
    serviceProvider_.getConsoleManager().WriteHistory(resp);
}

void CommandManager::Cmd_Partitions(const char* json, JsonWriter& resp)
{
    static constexpr int MAX_PARTITIONS = 16;
    UpdateManager::PartitionInfo parts[MAX_PARTITIONS];
    int count = serviceProvider_.getUpdateManager().GetPartitions(parts, MAX_PARTITIONS);

    resp.fieldArray("partitions");
    for (int i = 0; i < count; i++)
    {
        const auto& p = parts[i];
        resp.beginObject();
        resp.field("label",      p.label);
        resp.field("type",       p.type);
        resp.field("subtype",    p.subtype);
        resp.field("offset",     p.offset);
        resp.field("size",       p.size);
        resp.field("running",    p.running);
        resp.field("nextOta",    p.nextOta);
        resp.field("uploadable", p.uploadable);
        resp.field("version",    p.version);
        resp.endObject();
    }
    resp.endArray();
}
