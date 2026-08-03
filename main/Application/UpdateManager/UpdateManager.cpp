#include "UpdateManager.h"
#include "PartitionWriter.h"
#include "CommandManager.h"
#include "StringReader.h"
#include <cstdlib>
#include "JsonScope.h"
#include "JsonReader.h"
#include "JsonHelpers.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_app_desc.h"
#include <cstring>
#include <cstdio>

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

namespace {

// Command handlers run on whichever transport task dispatched them, and those
// stacks are a few KB — so a multi-KB *local* buffer eats most of one and can
// scribble on whatever memory follows. In the KC1245 fork that surfaced as a NULL
// semaphore inside lwIP's select teardown, with no clue in the backtrace pointing
// at the real culprit. Anything of that size belongs on the heap, freed on every
// exit path.
struct HeapBuf
{
    uint8_t* p;
    explicit HeapBuf(size_t n) : p(static_cast<uint8_t*>(malloc(n))) {}
    ~HeapBuf() { free(p); }
    HeapBuf(const HeapBuf&) = delete;
    HeapBuf& operator=(const HeapBuf&) = delete;
};

constexpr size_t kIoBufSize = 4096;

} // namespace

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

        // snprintf, not strncpy: a partition label can fill our buffer exactly,
        // and strncpy would then leave it unterminated (-Wstringop-truncation).
        snprintf(info.label, sizeof(info.label), "%s", p->label);

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
                snprintf(info.version, sizeof(info.version), "%s", desc.version);
            }
        }

        it = esp_partition_next(it);
    }

    if (it != nullptr)
        esp_partition_iterator_release(it);

    return count;
}

// ──────────────────────────────────────────────────────────────
// Status / enumeration commands
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
// Streamed upload — one command carries the whole image.
// Envelope: {"type":"writePartition","partition":"<label>"}\n<bytes…>
// ──────────────────────────────────────────────────────────────

void UpdateManager::Cmd_WritePartition(Stream& in, Stream& out)
{
    // Reply is a stream of newline-free JSON messages, one per chunk: zero or more
    // progress reports {"p":<bytesWritten>} flushed as they happen, then a final
    // result. Progress is device-authoritative (bytes actually written to flash),
    // so the client's bar tracks the real write, not bytes queued into the socket.
    static constexpr size_t REPORT_EVERY = 32 * 1024;

    char line[128];
    StringReader(in).readLine(line, sizeof(line));   // consume the envelope; body follows

    char label[17] = {};
    ExtractJsonString(line, "partition", label, sizeof(label));

    char msg[96];

    const char* err = nullptr;
    PartitionWriter w(label, &err);
    if (!w.ok())
    {
        int len = snprintf(msg, sizeof(msg), "{\"ok\":false,\"error\":\"%s\"}", err);
        out.write(msg, len);
        return;
    }

    HeapBuf io(kIoBufSize);
    if (!io.p)
    {
        int len = snprintf(msg, sizeof(msg), "{\"ok\":false,\"error\":\"out of memory\"}");
        out.write(msg, len);
        return;
    }

    size_t n;
    size_t reported = 0;
    while ((n = in.read(io.p, kIoBufSize)) > 0)   // 0 == end of stream == full image
    {
        if (!w.write(io.p, n))                     // dtor aborts a half-written image
        {
            int len = snprintf(msg, sizeof(msg), "{\"ok\":false,\"error\":\"write failed\"}");
            out.write(msg, len);
            return;
        }
        if (w.written() - reported >= REPORT_EVERY)
        {
            int len = snprintf(msg, sizeof(msg), "{\"p\":%lu}", (unsigned long)w.written());
            out.write(msg, len);
            out.flush();                           // push this progress chunk now
            reported = w.written();
        }
    }

    // A stream that broke is not a complete image, even though it ended the same
    // way a complete one does — read() returns 0 for both. Returning here without
    // finish() leaves the destructor to abort the write, so a truncated image is
    // never validated, never activated, and the sender is told the real reason
    // instead of "image validation failed" a megabyte later.
    if (in.failed())
    {
        ESP_LOGE(TAG, "request stream failed after %u bytes, discarding the image",
                 (unsigned)w.written());
        int len = snprintf(msg, sizeof(msg),
                           "{\"ok\":false,\"error\":\"stream failed at %lu bytes\"}",
                           (unsigned long)w.written());
        out.write(msg, len);
        return;
    }

    err = w.finish();
    int len = err
        ? snprintf(msg, sizeof(msg), "{\"ok\":false,\"error\":\"%s\"}", err)
        : snprintf(msg, sizeof(msg), "{\"ok\":true,\"size\":%lu}", (unsigned long)w.written());
    out.write(msg, len);   // OnSessionOpened's finish() emits this as the FINAL chunk
}

// ──────────────────────────────────────────────────────────────
// Pull OTA — the device fetches the image itself, into the same writer.
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

    const char* err = nullptr;
    uint32_t total = 0;

    do
    {
        if (esp_http_client_open(client, 0) != ESP_OK) { err = "connect failed"; break; }
        esp_http_client_fetch_headers(client);
        int status = esp_http_client_get_status_code(client);
        if (status != 200) { err = "http status"; break; }

        PartitionWriter w(label, &err);   // dtor aborts if we break before finish()
        if (!w.ok()) break;

        char buf[1024];
        int n;
        while ((n = esp_http_client_read(client, buf, sizeof(buf))) > 0)
        {
            if (!w.write(buf, n)) { err = "write failed"; break; }
            total += n;
        }
        if (err) break;
        if (n < 0) { err = "read failed"; break; }

        err = w.finish();
    } while (false);

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

    HeapBuf io(kIoBufSize);
    if (!io.p)
    {
        JsonObject resp(out);
        resp.field("ok", false);
        resp.field("error", "out of memory");
        return;
    }

    size_t offset = 0;
    while (offset < p->size)
    {
        size_t n = (p->size - offset < kIoBufSize) ? (p->size - offset) : kIoBufSize;
        if (esp_partition_read(p, offset, io.p, n) != ESP_OK)
        {
            ESP_LOGE(TAG, "esp_partition_read failed at offset %lu", (unsigned long)offset);
            return;
        }
        if (out.write(io.p, n) != n)
        {
            ESP_LOGW(TAG, "Client disconnected during download");
            return;
        }
        offset += n;
    }
}
