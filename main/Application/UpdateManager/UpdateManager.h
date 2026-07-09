#pragma once

#include "ServiceProvider.h"
#include "InitState.h"
#include "CommandEntry.h"
#include <esp_ota_ops.h>

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

    // One mechanism for ANY partition, addressed by label — see PartitionWriter.
    // An upload is one streamed command (writePartition): the handler drains its
    // input stream straight into a PartitionWriter, finalize runs at end-of-stream.
    // There is no cross-request session state; the transport carries the whole
    // image within one command.

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

    // ── Commands (registered with CommandManager in Init) ──
    void Cmd_UpdateStatus(Stream& in, Stream& out);
    void Cmd_Partitions(Stream& in, Stream& out);
    void Cmd_WritePartition(Stream& in, Stream& out);   // streamed upload: header line + body
    void Cmd_UpdateFromUrl(Stream& in, Stream& out);
    void Cmd_DownloadPartition(Stream& in, Stream& out);

    inline static CommandEntry commands_[] = {
        { "updateStatus",      &InvokeCommand<&UpdateManager::Cmd_UpdateStatus> },
        { "partitions",        &InvokeCommand<&UpdateManager::Cmd_Partitions> },
        { "writePartition",    &InvokeCommand<&UpdateManager::Cmd_WritePartition> },
        { "updateFromUrl",     &InvokeCommand<&UpdateManager::Cmd_UpdateFromUrl> },
        { "downloadPartition", &InvokeCommand<&UpdateManager::Cmd_DownloadPartition> },
    };
};
