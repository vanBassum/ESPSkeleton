#pragma once

#include "ServiceProvider.h"
#include "InitState.h"
#include "CommandEntry.h"
#include "Mutex.h"
#include <esp_ota_ops.h>
#include <esp_vfs_fat.h>

class Stream;

class UpdateManager {
    static constexpr const char* TAG = "UpdateManager";

public:
    explicit UpdateManager(ServiceProvider& serviceProvider);

    UpdateManager(const UpdateManager&) = delete;
    UpdateManager& operator=(const UpdateManager&) = delete;

    void Init();

    // Everything else is commands: the manager's entire external
    // surface is its command table below.

private:
    ServiceProvider& serviceProvider_;
    InitState initState_;

    // ── Update session ────────────────────────────────────────
    // One mechanism for ANY partition, addressed by label. App
    // partitions go through the esp_ota_* API (image validation +
    // set-boot-partition, running slot refused); data partitions are
    // raw erase+write. Driven by updateBegin/updateWrite/updateEnd.

    bool BeginUpdate(const char* label, const char** err);
    bool WriteChunk(const void* data, size_t size);
    const char* FinalizeUpdate();
    void AbortUpdate();   // expects mutex_ held
    bool HasSession() const { return target_ != nullptr; }

    const char* GetRunningPartition() const;
    const char* GetNextPartition() const;

    // ── Partition inspection ──────────────────────────────────

    struct PartitionInfo
    {
        char     label[16];
        char     type[8];       // "app" or "data"
        char     subtype[16];   // "ota_0" / "fat" / "nvs" / "0xNN" …
        uint32_t offset;
        uint32_t size;
        bool     running;
        bool     nextOta;
        bool     uploadable;    // safe to overwrite via upload
        char     version[32];   // app partitions only; empty otherwise
    };

    /// Enumerate all partitions into `out`. Returns count written.
    int GetPartitions(PartitionInfo* out, int maxCount) const;

    // Session state (guarded by mutex_)
    const esp_partition_t* target_ = nullptr;   // nullptr = no session
    esp_ota_handle_t otaHandle_ = 0;            // valid while target_ is an app partition
    size_t writeOffset_ = 0;                    // used for data partitions

    Mutex mutex_;

    // ── WebSocket commands (registered with CommandManager in Init) ──
    void Cmd_UpdateStatus(Stream& in, Stream& out);
    void Cmd_Partitions(Stream& in, Stream& out);
    void Cmd_UpdateBegin(Stream& in, Stream& out);
    void Cmd_UpdateWrite(Stream& in, Stream& out);
    void Cmd_UpdateEnd(Stream& in, Stream& out);
    void Cmd_UpdateFromUrl(Stream& in, Stream& out);
    void Cmd_DownloadPartition(Stream& in, Stream& out);

    inline static CommandEntry commands_[] = {
        { "updateStatus",      &InvokeCommand<&UpdateManager::Cmd_UpdateStatus> },
        { "partitions",        &InvokeCommand<&UpdateManager::Cmd_Partitions> },
        { "updateBegin",       &InvokeCommand<&UpdateManager::Cmd_UpdateBegin> },
        { "updateWrite",       &InvokeCommand<&UpdateManager::Cmd_UpdateWrite> },
        { "updateEnd",         &InvokeCommand<&UpdateManager::Cmd_UpdateEnd> },
        { "updateFromUrl",     &InvokeCommand<&UpdateManager::Cmd_UpdateFromUrl> },
        { "downloadPartition", &InvokeCommand<&UpdateManager::Cmd_DownloadPartition> },
    };
};
