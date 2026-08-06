#include "UpdateManager.h"
#include "PartitionWriter.h"
#include "CommandManager.h"
#include "esp_log.h"
#include "esp_app_desc.h"
#include <cstring>
#include <cstdio>

UpdateManager::UpdateManager(StruxProvider& strux)
    : strux_(strux)
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

    strux_.getCommandManager().Register(this, commands_);

    initAttempt.SetReady();
    ESP_LOGI(TAG, "Initialized");
}

// The 4 KB I/O buffer these two commands used to carry is gone rather than moved.
// It could not live on the stack — handlers run on whichever transport task
// dispatched them, and 4 KB of a few-KB stack scribbles on whatever follows (in the
// KC1245 fork that surfaced as a NULL semaphore inside lwIP's select teardown, with
// nothing in the backtrace pointing at the culprit). The heap fixed that overrun and
// bought a fragmentation problem plus an allocation that can fail mid-write.
//
// Neither is needed: the bytes are already in a buffer at both ends. An upload sits
// in the transport's inbound buffer, a download is assembled in its framing buffer,
// and Session lends both out (Stream::canLend), so these handlers move bytes between
// flash and a buffer they do not own.

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

RequestError UpdateManager::Cmd_UpdateStatus(CommandContext& ctx)
{
    RETURN_IF_ERROR(ctx.readArgs());

    auto resp = ctx.reply.object();

    const esp_app_desc_t* app = esp_app_get_description();

    resp.field("firmware", app->version);
    resp.field("running", GetRunningPartition());
    resp.field("nextSlot", GetNextPartition());
    return RequestError::Ok;
}

RequestError UpdateManager::Cmd_Partitions(CommandContext& ctx)
{
    static constexpr int MAX_PARTITIONS = 16;
    PartitionInfo parts[MAX_PARTITIONS];
    RETURN_IF_ERROR(ctx.readArgs());

    int count = GetPartitions(parts, MAX_PARTITIONS);

    auto root = ctx.reply.object();
    auto arr  = root.array("partitions");

    for (int i = 0; i < count; i++)
    {
        const auto& p = parts[i];
        auto o = arr.object();
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
    return RequestError::Ok;
}

// ──────────────────────────────────────────────────────────────
// Streamed upload — one command carries the whole image.
// Envelope: {"type":"writePartition","partition":"<label>"}\n<bytes…>
// ──────────────────────────────────────────────────────────────

RequestError UpdateManager::Cmd_WritePartition(CommandContext& ctx)
{
    // Reply is a stream of newline-free JSON messages, one per chunk: zero or more
    // progress reports {"p":<bytesWritten>} flushed as they happen, then a final
    // result. Progress is device-authoritative (bytes actually written to flash),
    // so the client's bar tracks the real write, not bytes queued into the socket.
    static constexpr size_t REPORT_EVERY = 32 * 1024;

    char label[17] = {};
    uint32_t offset = 0;

    // Absence and a legitimate zero mean different things here, hence has():
    // no `offset` is the one-shot upload — start at zero, erase as we go, activate
    // at the end, the whole image in one command, which is what the web UI sends.
    // An explicit offset (including 0) means the sender is driving the upload in
    // pieces and owns the clearPartition and activatePartition steps itself.
    RETURN_IF_ERROR(ctx.readArgs(
        Required("partition", label),
        Optional("offset",    offset)
    ));

    // `in` is now positioned at the body, past the envelope.
    const char* err = nullptr;
    PartitionWriter w(label, offset, &err);
    if (!w.ok())
    {
        auto resp = ctx.reply.object();
        resp.field("ok", false);
        resp.field("error", err);
        return RequestError::Ok;
    }

    // Asked before the loop, not inside it: past this point 0 means end of image,
    // and a stream that lends nothing would look exactly like an empty one — an
    // upload that "succeeded" having written nothing.
    if (!ctx.in.canLend())
    {
        auto resp = ctx.reply.object();
        resp.field("ok", false);
        resp.field("error", "transport cannot stream");
        return RequestError::Ok;
    }

    const uint8_t* chunk = nullptr;
    size_t n;
    size_t reported = 0;
    while ((n = ctx.in.lendInput(chunk)) > 0)   // 0 == end of stream == full image
    {
        if (!w.write(chunk, n))
        {
            auto resp = ctx.reply.object();
            resp.field("ok", false);
            resp.field("error", "write failed");
            return RequestError::Ok;
        }
        if (w.written() - reported >= REPORT_EVERY)
        {
            {
                auto progress = ctx.reply.object();
                progress.field("p", static_cast<uint32_t>(w.written()));
            }   // closed before the flush, or the chunk carries half a record
            ctx.out.flush();                       // push this progress chunk now
            reported = w.written();
        }
    }

    // A stream that broke is not a complete write, even though it ended the same way
    // a complete one does — read() returns 0 for both. Returning without activating
    // leaves the boot pointer where it was, so a truncated image is inert and the
    // sender is told the real reason instead of "image validation failed" later.
    if (ctx.in.failed())
    {
        ESP_LOGE(TAG, "request stream failed after %u bytes, not activating",
                 (unsigned)w.written());
        // The byte count belongs in the message, so it is composed as a value. A field
        // value is still just a value; nothing here names the wire format.
        char why[48];
        snprintf(why, sizeof(why), "stream failed at %lu bytes", (unsigned long)w.written());

        auto resp = ctx.reply.object();
        resp.field("ok", false);
        resp.field("error", why);
        return RequestError::Ok;
    }

    // This piece landed, and that is all this command claims. Erasing is `clear`'s
    // job and switching the boot slot is `activate`'s; a write writes.
    // The dispatcher's finish() emits this as the FINAL chunk.
    auto resp = ctx.reply.object();
    resp.field("ok", true);
    resp.field("offset", offset);
    resp.field("size", static_cast<uint32_t>(w.written()));
    return RequestError::Ok;
}

// ──────────────────────────────────────────────────────────────
// Chunked-upload steps — the two halves the one-shot path does implicitly, so a
// sender can drive an upload as many short sessions instead of one long one.
// ──────────────────────────────────────────────────────────────

// These two are the first handlers written against the console request format
// rather than the JSON envelope:
//
//     clearPartition -p ota_1
//     activatePartition -p ota_1
//
// Note what is absent: no JsonReader, so no buffer holding the request. `label` is
// seventeen bytes because a partition label is seventeen bytes — the request's length
// does not enter into it. The reply stays JSON, which costs nothing because writing
// is single-pass already.
//
// Converted first because they are new and nothing in the web UI calls them yet, so
// the format can be proven on hardware without touching the frontend.

RequestError UpdateManager::Cmd_ClearPartition(CommandContext& ctx)
{
    char label[17] = {};
    RETURN_IF_ERROR(ctx.readArgs(Required("partition", label)));

    auto resp = ctx.reply.object();
    if (const char* err = PartitionWriter::Clear(label))
    {
        resp.field("ok", false);
        resp.field("error", err);
        return RequestError::Ok;
    }
    resp.field("ok", true);
    return RequestError::Ok;
}

RequestError UpdateManager::Cmd_ActivatePartition(CommandContext& ctx)
{
    char label[17] = {};
    RETURN_IF_ERROR(ctx.readArgs(Required("partition", label)));

    auto resp = ctx.reply.object();
    if (const char* err = PartitionWriter::Activate(label))
    {
        resp.field("ok", false);
        resp.field("error", err);
        return RequestError::Ok;
    }
    resp.field("ok", true);
    return RequestError::Ok;
}

// ──────────────────────────────────────────────────────────────
// Partition download — tiny JSON request in, raw bytes out
// ──────────────────────────────────────────────────────────────

RequestError UpdateManager::Cmd_DownloadPartition(CommandContext& ctx)
{
    char label[17] = {};
    RETURN_IF_ERROR(ctx.readArgs(Required("partition", label)));

    const esp_partition_t* p = esp_partition_find_first(
        ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, label);
    if (!p)
    {
        auto resp = ctx.reply.object();
        resp.field("ok", false);
        resp.field("error", "unknown partition");
        return RequestError::Ok;
    }

    ESP_LOGI(TAG, "Download partition '%s' (%lu bytes)", label, (unsigned long)p->size);

    if (!ctx.out.canLend())
    {
        auto resp = ctx.reply.object();
        resp.field("ok", false);
        resp.field("error", "transport cannot stream");
        return RequestError::Ok;
    }

    // Read flash straight into the reply frame the transport is about to send. The
    // run is one chunk's worth of payload, not a size of ours — which is why there
    // is no buffer here to pick a size for.
    size_t offset = 0;
    while (offset < p->size)
    {
        size_t avail = 0;
        uint8_t* dst = ctx.out.lendOutput(avail);
        if (dst == nullptr)
        {
            ESP_LOGW(TAG, "Client disconnected during download");
            return RequestError::Ok;
        }

        size_t n = (p->size - offset < avail) ? (p->size - offset) : avail;
        if (esp_partition_read(p, offset, dst, n) != ESP_OK)
        {
            ESP_LOGE(TAG, "esp_partition_read failed at offset %lu", (unsigned long)offset);
            return RequestError::Ok;
        }
        ctx.out.commitOutput(n);
        offset += n;
    }
    return RequestError::Ok;
}
