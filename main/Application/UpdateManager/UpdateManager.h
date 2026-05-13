#pragma once

#include "ServiceProvider.h"
#include "InitState.h"
#include "Mutex.h"
#include <esp_ota_ops.h>
#include <esp_vfs_fat.h>

class UpdateManager {
    static constexpr const char* TAG = "UpdateManager";

public:
    explicit UpdateManager(ServiceProvider& serviceProvider);

    UpdateManager(const UpdateManager&) = delete;
    UpdateManager& operator=(const UpdateManager&) = delete;

    void Init();

    // ── App firmware OTA ──────────────────────────────────────

    bool BeginAppUpdate();
    bool WriteAppChunk(const void* data, size_t size);
    const char* FinalizeAppUpdate();

    const char* GetRunningPartition() const;
    const char* GetNextPartition() const;

    // ── WWW partition update ─────────────────────────────────

    bool BeginWwwUpdate();
    bool WriteWwwChunk(const void* data, size_t size);
    const char* FinalizeWwwUpdate();

    // ── Partition inspection (for Firmware page) ─────────────

    struct PartitionInfo
    {
        char     label[16];
        char     type[8];       // "app" or "data"
        char     subtype[16];   // "ota_0" / "fat" / "nvs" / "0xNN" …
        uint32_t offset;
        uint32_t size;
        bool     running;
        bool     nextOta;
        bool     uploadable;    // safe to overwrite via HTTP
        char     version[32];   // app partitions only; empty otherwise
    };

    /// Enumerate all partitions into `out`. Returns count written.
    int GetPartitions(PartitionInfo* out, int maxCount) const;

private:
    ServiceProvider& serviceProvider_;
    InitState initState_;

    // OTA state
    esp_ota_handle_t otaHandle_ = 0;
    const esp_partition_t* otaPartition_ = nullptr;
    bool otaActive_ = false;

    // WWW state
    const esp_partition_t* wwwPartition_ = nullptr;
    size_t wwwOffset_ = 0;
    bool wwwActive_ = false;

    void AbortOta();

    Mutex mutex_;
};
