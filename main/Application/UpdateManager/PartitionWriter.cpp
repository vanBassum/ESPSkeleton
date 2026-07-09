#include "PartitionWriter.h"
#include "esp_log.h"
#include "spi_flash_mmap.h"   // SPI_FLASH_SEC_SIZE (flash erase granularity)

PartitionWriter::PartitionWriter(const char* label, const char** err)
{
    const esp_partition_t* p = esp_partition_find_first(
        ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, label);
    if (!p) { *err = "unknown partition"; return; }

    isApp_ = (p->type == ESP_PARTITION_TYPE_APP);

    if (isApp_)
    {
        if (p == esp_ota_get_running_partition())
        {
            *err = "partition is running";   // never overwrite the slot we booted from
            return;
        }
        esp_err_t e = esp_ota_begin(p, OTA_WITH_SEQUENTIAL_WRITES, &otaHandle_);
        if (e != ESP_OK)
        {
            ESP_LOGE(TAG, "esp_ota_begin: %s", esp_err_to_name(e));
            *err = "ota begin failed";
            return;
        }
        began_ = true;
    }
    // Data partition: nothing to erase up front — sectors are erased lazily in
    // write(). written_ / erasedTo_ start at 0.

    p_ = p;   // ok() now true
}

PartitionWriter::~PartitionWriter()
{
    if (isApp_ && began_)
    {
        esp_ota_abort(otaHandle_);
        ESP_LOGW(TAG, "aborted half-written app image");
    }
}

bool PartitionWriter::write(const void* data, size_t size)
{
    if (!p_) return false;

    if (isApp_)
    {
        esp_err_t e = esp_ota_write(otaHandle_, data, size);
        if (e != ESP_OK)
        {
            ESP_LOGE(TAG, "esp_ota_write: %s", esp_err_to_name(e));
            return false;
        }
    }
    else
    {
        const size_t end = written_ + size;
        if (end > p_->size)
        {
            ESP_LOGE(TAG, "write exceeds partition size");
            return false;
        }
        // Erase every sector this write touches, exactly once, just in time.
        // Writes are sequential from 0, so erasedTo_ only advances.
        while (erasedTo_ < end)
        {
            esp_err_t e = esp_partition_erase_range(p_, erasedTo_, SPI_FLASH_SEC_SIZE);
            if (e != ESP_OK)
            {
                ESP_LOGE(TAG, "erase @0x%lx: %s",
                         (unsigned long)erasedTo_, esp_err_to_name(e));
                return false;
            }
            erasedTo_ += SPI_FLASH_SEC_SIZE;
        }
        esp_err_t e = esp_partition_write(p_, written_, data, size);
        if (e != ESP_OK)
        {
            ESP_LOGE(TAG, "esp_partition_write: %s", esp_err_to_name(e));
            return false;
        }
    }
    written_ += size;
    return true;
}

const char* PartitionWriter::finish()
{
    if (!p_) return "no target";

    if (isApp_)
    {
        began_ = false;                       // consumed: dtor must not abort
        esp_err_t e = esp_ota_end(otaHandle_);
        if (e != ESP_OK)
        {
            ESP_LOGE(TAG, "esp_ota_end: %s", esp_err_to_name(e));
            return "image validation failed";
        }
        e = esp_ota_set_boot_partition(p_);
        if (e != ESP_OK)
        {
            ESP_LOGE(TAG, "set_boot_partition: %s", esp_err_to_name(e));
            return "failed to set boot partition";
        }
        ESP_LOGI(TAG, "app image finalized, next boot '%s'", p_->label);
    }
    else
    {
        ESP_LOGI(TAG, "partition '%s' written (%lu bytes)",
                 p_->label, (unsigned long)written_);
    }
    return nullptr;
}
