#include "UpdateManager.h"
#include "CommandManager.h"
#include "ContextLock.h"
#include "JsonScope.h"
#include "JsonReader.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_app_desc.h"
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

    serviceProvider_.getCommandManager().Register(this, commands_);

    initAttempt.SetReady();
    ESP_LOGI(TAG, "Initialized");
}

// ──────────────────────────────────────────────────────────────
// Update session — one mechanism for any partition, by label.
// App partitions: esp_ota_* API (validation, set-boot-partition).
// Data partitions: raw erase + sequential write.
// ──────────────────────────────────────────────────────────────

bool UpdateManager::BeginUpdate(const char* label, const char** err)
{
    LOCK(mutex_);
    if (target_) { *err = "busy"; return false; }

    const esp_partition_t* p = esp_partition_find_first(
        ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, label);
    if (!p) { *err = "unknown partition"; return false; }

    if (p->type == ESP_PARTITION_TYPE_APP)
    {
        if (p == esp_ota_get_running_partition())
        {
            *err = "partition is running";
            return false;
        }
        esp_err_t e = esp_ota_begin(p, OTA_WITH_SEQUENTIAL_WRITES, &otaHandle_);
        if (e != ESP_OK)
        {
            ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(e));
            *err = "ota begin failed";
            return false;
        }
    }
    else
    {
        esp_err_t e = esp_partition_erase_range(p, 0, p->size);
        if (e != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to erase '%s': %s", label, esp_err_to_name(e));
            *err = "erase failed";
            return false;
        }
        writeOffset_ = 0;
    }

    target_ = p;
    ESP_LOGI(TAG, "Update session started on '%s'", p->label);
    return true;
}

bool UpdateManager::WriteChunk(const void* data, size_t size)
{
    LOCK(mutex_);
    if (!target_) return false;

    if (target_->type == ESP_PARTITION_TYPE_APP)
    {
        esp_err_t e = esp_ota_write(otaHandle_, data, size);
        if (e != ESP_OK)
        {
            ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(e));
            AbortUpdate();
            return false;
        }
    }
    else
    {
        if (writeOffset_ + size > target_->size)
        {
            ESP_LOGE(TAG, "Data exceeds partition size");
            AbortUpdate();
            return false;
        }
        esp_err_t e = esp_partition_write(target_, writeOffset_, data, size);
        if (e != ESP_OK)
        {
            ESP_LOGE(TAG, "esp_partition_write failed: %s", esp_err_to_name(e));
            AbortUpdate();
            return false;
        }
        writeOffset_ += size;
    }
    return true;
}

const char* UpdateManager::FinalizeUpdate()
{
    LOCK(mutex_);
    if (!target_) return "no session";

    const esp_partition_t* p = target_;
    target_ = nullptr;

    if (p->type == ESP_PARTITION_TYPE_APP)
    {
        esp_err_t e = esp_ota_end(otaHandle_);
        if (e != ESP_OK)
        {
            ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(e));
            return "Image validation failed";
        }
        e = esp_ota_set_boot_partition(p);
        if (e != ESP_OK)
        {
            ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(e));
            return "Failed to set boot partition";
        }
        ESP_LOGI(TAG, "App update finalized, next boot from '%s'", p->label);
    }
    else
    {
        ESP_LOGI(TAG, "Partition '%s' updated (%lu bytes)", p->label, (unsigned long)writeOffset_);
    }
    return nullptr; // success
}

void UpdateManager::AbortUpdate()
{
    if (!target_) return;
    if (target_->type == ESP_PARTITION_TYPE_APP)
        esp_ota_abort(otaHandle_);
    target_ = nullptr;
    ESP_LOGW(TAG, "Update session aborted");
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

// ──────────────────────────────────────────────────────────────
// WebSocket commands
// ──────────────────────────────────────────────────────────────

void UpdateManager::Cmd_UpdateStatus(Stream& in, Stream& out)
{
    JsonObject resp(out);

    const esp_app_desc_t* app = esp_app_get_description();

    resp.field("firmware", app->version);
    resp.field("running", GetRunningPartition());
    resp.field("nextSlot", GetNextPartition());
}

void UpdateManager::Cmd_Partitions(Stream& in, Stream& out)
{
    static constexpr int MAX_PARTITIONS = 16;
    PartitionInfo parts[MAX_PARTITIONS];
    int count = GetPartitions(parts, MAX_PARTITIONS);

    JsonObject root(out);
    JsonArray arr = root.array("partitions");

    for (int i = 0; i < count; i++)
    {
        const auto& p = parts[i];
        JsonObject o = arr.object();
        o.field("label",      p.label);
        o.field("type",       p.type);
        o.field("subtype",    p.subtype);
        o.field("offset",     p.offset);
        o.field("size",       p.size);
        o.field("running",    p.running);
        o.field("nextOta",    p.nextOta);
        o.field("uploadable", p.uploadable);
        o.field("version",    p.version);
    }
}

// ──────────────────────────────────────────────────────────────
// Session-based update commands (updateBegin / updateWrite / updateEnd)
// ──────────────────────────────────────────────────────────────

void UpdateManager::Cmd_UpdateBegin(Stream& in, Stream& out)
{
    JsonReader<256> req(in);
    JsonObject resp(out);

    char label[17] = {};
    req.GetString("partition", label, sizeof(label));

    const char* err = nullptr;
    if (!BeginUpdate(label, &err))
    {
        resp.field("ok", false);
        resp.field("error", err);
        return;
    }
    resp.field("ok", true);
}

void UpdateManager::Cmd_UpdateWrite(Stream& in, Stream& out)
{
    char buf[1024];

    if (!HasSession())
    {
        while (in.read(buf, sizeof(buf)) > 0) {}   // drain so the transport isn't left mid-body
        JsonObject resp(out);
        resp.field("ok", false);
        resp.field("error", "no session");
        return;
    }

    uint32_t total = 0;
    size_t n;
    while ((n = in.read(buf, sizeof(buf))) > 0)
    {
        if (!WriteChunk(buf, n))   // aborts the session on failure
        {
            while (in.read(buf, sizeof(buf)) > 0) {}
            JsonObject resp(out);
            resp.field("ok", false);
            resp.field("error", "write failed");
            return;
        }
        total += n;
    }

    JsonObject resp(out);
    resp.field("ok", true);
    resp.field("size", total);
}

void UpdateManager::Cmd_UpdateEnd(Stream& in, Stream& out)
{
    JsonObject resp(out);

    const char* err = FinalizeUpdate();
    if (err) { resp.field("ok", false); resp.field("error", err); return; }
    resp.field("ok", true);
}

// ──────────────────────────────────────────────────────────────
// Pull OTA — the device fetches the image itself
// ──────────────────────────────────────────────────────────────

void UpdateManager::Cmd_UpdateFromUrl(Stream& in, Stream& out)
{
    JsonReader<512> req(in);
    JsonObject resp(out);

    char url[256] = {};
    if (!req.GetString("url", url, sizeof(url)))
    {
        resp.field("ok", false);
        resp.field("error", "missing url");
        return;
    }

    // Partition by label; defaults to the next OTA slot (the normal
    // "update my firmware from here" case).
    char label[17] = {};
    if (!req.GetString("partition", label, sizeof(label)))
    {
        const esp_partition_t* next = esp_ota_get_next_update_partition(nullptr);
        if (!next) { resp.field("ok", false); resp.field("error", "no ota slot"); return; }
        snprintf(label, sizeof(label), "%s", next->label);
    }

    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.timeout_ms = 15000;
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) { resp.field("ok", false); resp.field("error", "client init failed"); return; }

    bool began = false;
    const char* err = nullptr;
    uint32_t total = 0;

    do
    {
        if (esp_http_client_open(client, 0) != ESP_OK) { err = "connect failed"; break; }
        esp_http_client_fetch_headers(client);
        int status = esp_http_client_get_status_code(client);
        if (status != 200) { err = "http status"; break; }

        began = BeginUpdate(label, &err);
        if (!began) break;

        char buf[1024];
        int n;
        while ((n = esp_http_client_read(client, buf, sizeof(buf))) > 0)
        {
            if (!WriteChunk(buf, n)) { err = "write failed"; began = false; break; }   // aborted
            total += n;
        }
        if (err) break;
        if (n < 0) { err = "read failed"; break; }

        err = FinalizeUpdate();
        began = false;   // finalize consumed the session, success or not
    } while (false);

    if (began)   // opened a session but bailed before finalize consumed it
    {
        LOCK(mutex_);
        AbortUpdate();
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (err) { resp.field("ok", false); resp.field("error", err); return; }
    resp.field("ok", true);
    resp.field("size", total);
    ESP_LOGI(TAG, "Pull update from %s complete (%lu bytes)", url, (unsigned long)total);
}

// ──────────────────────────────────────────────────────────────
// Partition download — tiny JSON request in, raw bytes out
// ──────────────────────────────────────────────────────────────

void UpdateManager::Cmd_DownloadPartition(Stream& in, Stream& out)
{
    JsonReader<256> req(in);

    char label[17] = {};
    req.GetString("partition", label, sizeof(label));

    const esp_partition_t* p = esp_partition_find_first(
        ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, label);
    if (!p)
    {
        JsonObject resp(out);
        resp.field("ok", false);
        resp.field("error", "unknown partition");
        return;
    }

    ESP_LOGI(TAG, "Download partition '%s' (%lu bytes)", label, (unsigned long)p->size);

    uint8_t buf[4096];
    size_t offset = 0;
    while (offset < p->size)
    {
        size_t n = (p->size - offset < sizeof(buf)) ? (p->size - offset) : sizeof(buf);
        if (esp_partition_read(p, offset, buf, n) != ESP_OK)
        {
            ESP_LOGE(TAG, "esp_partition_read failed at offset %lu", (unsigned long)offset);
            return;
        }
        if (out.write(buf, n) != n)
        {
            ESP_LOGW(TAG, "Client disconnected during download");
            return;
        }
        offset += n;
    }
}
