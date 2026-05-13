#include "SettingsManager.h"
#include "SettingsDefs.h"
#include "JsonWriter.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include <cstring>
#include <cstdlib>

// ──────────────────────────────────────────────────────────────
// Init
// ──────────────────────────────────────────────────────────────

SettingsManager::SettingsManager(ServiceProvider& serviceProvider)
    : serviceProvider_(serviceProvider)
{
}

void SettingsManager::Init()
{
    auto initAttempt = initState_.TryBeginInit();
    if (!initAttempt)
    {
        ESP_LOGW(TAG, "Already initialized or initializing");
        return;
    }

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_LOGW(TAG, "NVS needs erase, reformatting");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    handle_ = nvs::open_nvs_handle(NVS_NAMESPACE, NVS_READWRITE, &err);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to open NVS handle: %s", esp_err_to_name(err));
        initAttempt.SetReady();
        return;
    }

    ApplyDefaults();

    initAttempt.SetReady();
    ESP_LOGI(TAG, "Initialized (%d settings)", GetDefinitionCount());
}

void SettingsManager::ApplyDefaults()
{
    const auto* defs = GetDefinitions();
    int count = GetDefinitionCount();

    for (int i = 0; i < count; i++)
    {
        const auto& def = defs[i];

        switch (def.type)
        {
        case SettingType::String:
        {
            size_t len = 0;
            if (handle_->get_item_size(nvs::ItemType::SZ, def.key, len) != ESP_OK)
            {
                handle_->set_string(def.key, def.strDefault);
            }
            break;
        }
        case SettingType::Int:
        {
            int32_t val;
            if (handle_->get_item(def.key, val) != ESP_OK)
            {
                handle_->set_item(def.key, static_cast<int32_t>(atoi(def.strDefault)));
            }
            break;
        }
        case SettingType::Bool:
        {
            uint8_t val;
            if (handle_->get_item(def.key, val) != ESP_OK)
            {
                handle_->set_item<uint8_t>(def.key, strcmp(def.strDefault, "true") == 0 ? 1 : 0);
            }
            break;
        }
        }
    }

    handle_->commit();
}

// ──────────────────────────────────────────────────────────────
// Typed access
// ──────────────────────────────────────────────────────────────

bool SettingsManager::getString(SettingKey key, char* out, size_t maxLen) const
{
    if (!handle_) return false;
    return handle_->get_string(key, out, maxLen) == ESP_OK;
}

bool SettingsManager::setString(SettingKey key, const char* value)
{
    if (!handle_) return false;
    return handle_->set_string(key, value) == ESP_OK;
}

int32_t SettingsManager::getInt(SettingKey key, int32_t defaultVal) const
{
    if (!handle_) return defaultVal;
    int32_t val = defaultVal;
    handle_->get_item(key, val);
    return val;
}

bool SettingsManager::setInt(SettingKey key, int32_t value)
{
    if (!handle_) return false;
    return handle_->set_item(key, value) == ESP_OK;
}

bool SettingsManager::getBool(SettingKey key, bool defaultVal) const
{
    if (!handle_) return defaultVal;
    uint8_t val = defaultVal ? 1 : 0;
    handle_->get_item(key, val);
    return val != 0;
}

bool SettingsManager::setBool(SettingKey key, bool value)
{
    if (!handle_) return false;
    return handle_->set_item<uint8_t>(key, value ? 1 : 0) == ESP_OK;
}

// ──────────────────────────────────────────────────────────────
// Persistence
// ──────────────────────────────────────────────────────────────

bool SettingsManager::Save()
{
    if (!handle_) return false;
    esp_err_t err = handle_->commit();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Save failed: %s", esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "Settings saved");
    return true;
}

bool SettingsManager::ResetToDefaults()
{
    if (!handle_) return false;

    handle_->erase_all();

    const auto* defs = GetDefinitions();
    int count = GetDefinitionCount();

    for (int i = 0; i < count; i++)
    {
        const auto& def = defs[i];
        switch (def.type)
        {
        case SettingType::String:
            handle_->set_string(def.key, def.strDefault);
            break;
        case SettingType::Int:
            handle_->set_item(def.key, static_cast<int32_t>(atoi(def.strDefault)));
            break;
        case SettingType::Bool:
            handle_->set_item<uint8_t>(def.key, strcmp(def.strDefault, "true") == 0 ? 1 : 0);
            break;
        }
    }

    handle_->commit();
    ESP_LOGI(TAG, "Reset to defaults");
    return true;
}

// ──────────────────────────────────────────────────────────────
// Enumeration
// ──────────────────────────────────────────────────────────────

const SettingDef* SettingsManager::GetDefinitions() const
{
    return SETTINGS_DEFS;
}

int SettingsManager::GetDefinitionCount() const
{
    return SETTINGS_DEFS_COUNT;
}

void SettingsManager::WriteAllSettings(JsonWriter& writer) const
{
    const auto* defs = GetDefinitions();
    int count = GetDefinitionCount();

    writer.fieldArray("settings");

    for (int i = 0; i < count; i++)
    {
        const auto& def = defs[i];

        writer.beginObject();
        writer.field("key", def.key);
        writer.field("label", def.label);

        switch (def.type)
        {
        case SettingType::String:
        {
            writer.field("type", "string");
            char buf[128] = {};
            getString(def.key, buf, sizeof(buf));
            writer.field("value", buf);
            break;
        }
        case SettingType::Int:
        {
            writer.field("type", "int");
            writer.field("value", getInt(def.key));
            break;
        }
        case SettingType::Bool:
        {
            writer.field("type", "bool");
            writer.field("value", getBool(def.key));
            break;
        }
        }

        writer.endObject();
    }

    writer.endArray();
}
