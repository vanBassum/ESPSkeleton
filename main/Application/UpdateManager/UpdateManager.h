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
        char     label[17];     // esp_partition_t::label is char[17] — 16 + NUL
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
    RequestError Cmd_UpdateStatus(CommandContext& ctx);
    RequestError Cmd_Partitions(CommandContext& ctx);
    /// Streamed upload: header line + body. `offset` is optional and decides which
    /// of two modes this is:
    ///
    ///   absent  — one shot. Erase as we go from zero and activate at the end; the
    ///             whole image in a single command, which is what the web UI sends.
    ///   present — one piece of a caller-driven upload. Writes exactly where told,
    ///             erases nothing, activates nothing. The sender calls
    ///             clearPartition first and activatePartition after the last piece,
    ///             and may leave gaps between pieces for other traffic.
    ///
    /// The second mode exists because a single command that runs for tens of seconds
    /// monopolises the transport, which is what the relay's in-flight timeout trips
    /// over. Many short commands need no concurrency support to coexist with others.
    RequestError Cmd_WritePartition(CommandContext& ctx);
    RequestError Cmd_UpdateFromUrl(CommandContext& ctx);
    RequestError Cmd_DownloadPartition(CommandContext& ctx);

    /// Erase a partition whole, so a chunked upload starts from a known state.
    RequestError Cmd_ClearPartition(CommandContext& ctx);

    /// Validate an app image and make it the next boot slot. No-op for data.
    RequestError Cmd_ActivatePartition(CommandContext& ctx);

    inline static CommandEntry commands_[] = {
        { "updateStatus",      &InvokeCommand<&UpdateManager::Cmd_UpdateStatus> },
        { "partitions",        &InvokeCommand<&UpdateManager::Cmd_Partitions> },
        { "writePartition",    &InvokeCommand<&UpdateManager::Cmd_WritePartition> },
        { "clearPartition",    &InvokeCommand<&UpdateManager::Cmd_ClearPartition> },
        { "activatePartition", &InvokeCommand<&UpdateManager::Cmd_ActivatePartition> },
        { "updateFromUrl",     &InvokeCommand<&UpdateManager::Cmd_UpdateFromUrl> },
        { "downloadPartition", &InvokeCommand<&UpdateManager::Cmd_DownloadPartition> },
    };
};
