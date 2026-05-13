#include "UpdateManager.h"
#include "ContextLock.h"
#include "esp_log.h"
#include "esp_system.h"
#include <cstring>

UpdateManager::UpdateManager(ServiceProvider& serviceProvider)
    : serviceProvider_(serviceProvider)
{
}

void UpdateManager::Init()
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

// ──────────────────────────────────────────────────────────────
// App firmware OTA
// ──────────────────────────────────────────────────────────────

bool UpdateManager::BeginAppUpdate()
{
    LOCK(mutex_);
    if (otaActive_)
    {
        ESP_LOGW(TAG, "OTA already in progress");
        return false;
    }

    otaPartition_ = esp_ota_get_next_update_partition(nullptr);
    if (!otaPartition_)
    {
        ESP_LOGE(TAG, "No OTA partition available");
        return false;
    }

    esp_err_t err = esp_ota_begin(otaPartition_, OTA_WITH_SEQUENTIAL_WRITES, &otaHandle_);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        return false;
    }

    otaActive_ = true;
    ESP_LOGI(TAG, "App OTA started on partition '%s'", otaPartition_->label);
    return true;
}

bool UpdateManager::WriteAppChunk(const void* data, size_t size)
{
    LOCK(mutex_);
    if (!otaActive_) return false;

    esp_err_t err = esp_ota_write(otaHandle_, data, size);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
        AbortOta();
        return false;
    }
    return true;
}

const char* UpdateManager::FinalizeAppUpdate()
{
    LOCK(mutex_);
    if (!otaActive_) return "No OTA in progress";

    otaActive_ = false;

    esp_err_t err = esp_ota_end(otaHandle_);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
        return "Image validation failed";
    }

    err = esp_ota_set_boot_partition(otaPartition_);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
        return "Failed to set boot partition";
    }

    ESP_LOGI(TAG, "App OTA finalized, next boot from '%s'", otaPartition_->label);
    return nullptr; // success
}

void UpdateManager::AbortOta()
{
    if (otaActive_)
    {
        esp_ota_abort(otaHandle_);
        otaActive_ = false;
        ESP_LOGW(TAG, "OTA aborted");
    }
}

const char* UpdateManager::GetRunningPartition() const
{
    const esp_partition_t* p = esp_ota_get_running_partition();
    return p ? p->label : "unknown";
}

const char* UpdateManager::GetNextPartition() const
{
    const esp_partition_t* p = esp_ota_get_next_update_partition(nullptr);
    return p ? p->label : "none";
}

// ──────────────────────────────────────────────────────────────
// WWW partition update
// ──────────────────────────────────────────────────────────────

bool UpdateManager::BeginWwwUpdate()
{
    LOCK(mutex_);
    if (wwwActive_)
    {
        ESP_LOGW(TAG, "WWW update already in progress");
        return false;
    }

    wwwPartition_ = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, "www");
    if (!wwwPartition_)
    {
        ESP_LOGE(TAG, "WWW partition not found");
        return false;
    }

    esp_err_t err = esp_partition_erase_range(wwwPartition_, 0, wwwPartition_->size);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to erase WWW partition: %s", esp_err_to_name(err));
        return false;
    }

    wwwOffset_ = 0;
    wwwActive_ = true;
    ESP_LOGI(TAG, "WWW update started (partition size: %lu)", (unsigned long)wwwPartition_->size);
    return true;
}

bool UpdateManager::WriteWwwChunk(const void* data, size_t size)
{
    LOCK(mutex_);
    if (!wwwActive_) return false;

    if (wwwOffset_ + size > wwwPartition_->size)
    {
        ESP_LOGE(TAG, "WWW data exceeds partition size");
        wwwActive_ = false;
        return false;
    }

    esp_err_t err = esp_partition_write(wwwPartition_, wwwOffset_, data, size);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_partition_write failed: %s", esp_err_to_name(err));
        wwwActive_ = false;
        return false;
    }

    wwwOffset_ += size;
    return true;
}

const char* UpdateManager::FinalizeWwwUpdate()
{
    LOCK(mutex_);
    if (!wwwActive_) return "No WWW update in progress";

    wwwActive_ = false;
    ESP_LOGI(TAG, "WWW update finalized (%lu bytes written)", (unsigned long)wwwOffset_);
    return nullptr; // success
}

// ──────────────────────────────────────────────────────────────
// Partition enumeration
// ──────────────────────────────────────────────────────────────

int UpdateManager::GetPartitions(PartitionInfo* out, int maxCount) const
{
    const esp_partition_t* running = esp_ota_get_running_partition();
    const esp_partition_t* next    = esp_ota_get_next_update_partition(nullptr);
    const esp_partition_t* wwwP    = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, "www");

    int count = 0;
    esp_partition_iterator_t it = esp_partition_find(
        ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, nullptr);

    while (it != nullptr && count < maxCount)
    {
        const esp_partition_t* p = esp_partition_get(it);
        PartitionInfo& info = out[count++];

        strncpy(info.label, p->label, sizeof(info.label) - 1);
        info.label[sizeof(info.label) - 1] = '\0';

        // Subtype values collide across types (e.g. APP_FACTORY and DATA_OTA are both 0x00),
        // so we branch by type first.
        if (p->type == ESP_PARTITION_TYPE_APP)
        {
            strcpy(info.type, "app");
            switch (p->subtype)
            {
                case ESP_PARTITION_SUBTYPE_APP_FACTORY: strcpy(info.subtype, "factory"); break;
                case ESP_PARTITION_SUBTYPE_APP_OTA_0:   strcpy(info.subtype, "ota_0"); break;
                case ESP_PARTITION_SUBTYPE_APP_OTA_1:   strcpy(info.subtype, "ota_1"); break;
                default: snprintf(info.subtype, sizeof(info.subtype), "0x%02x", (int)p->subtype);
            }
        }
        else
        {
            strcpy(info.type, "data");
            switch (p->subtype)
            {
                case ESP_PARTITION_SUBTYPE_DATA_OTA: strcpy(info.subtype, "ota"); break;
                case ESP_PARTITION_SUBTYPE_DATA_PHY: strcpy(info.subtype, "phy"); break;
                case ESP_PARTITION_SUBTYPE_DATA_NVS: strcpy(info.subtype, "nvs"); break;
                case ESP_PARTITION_SUBTYPE_DATA_FAT: strcpy(info.subtype, "fat"); break;
                default: snprintf(info.subtype, sizeof(info.subtype), "0x%02x", (int)p->subtype);
            }
        }

        info.offset = p->address;
        info.size   = p->size;
        info.running = (running && p == running);
        info.nextOta = (next && p == next);

        // Uploadable: any non-running OTA app slot, or the www FAT partition.
        info.uploadable =
            (p->type == ESP_PARTITION_TYPE_APP && !info.running) ||
            (wwwP && p == wwwP);

        info.version[0] = '\0';
        if (p->type == ESP_PARTITION_TYPE_APP)
        {
            esp_app_desc_t desc;
            if (esp_ota_get_partition_description(p, &desc) == ESP_OK)
            {
                strncpy(info.version, desc.version, sizeof(info.version) - 1);
                info.version[sizeof(info.version) - 1] = '\0';
            }
        }

        it = esp_partition_next(it);
    }

    if (it != nullptr)
        esp_partition_iterator_release(it);

    return count;
}
