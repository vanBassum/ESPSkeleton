#pragma once

#include "StruxProvider.h"
#include "InitState.h"
#include "CommandEntry.h"
#include "Mutex.h"
#include "Task.h"
#include "freertos/queue.h"
#include <atomic>
#include <cstdint>

class Stream;
class ReplyObject;

class ConsoleManager {
    static constexpr const char* TAG = "ConsoleManager";
    static constexpr int32_t QUEUE_DEPTH = 16;

public:
    static constexpr int32_t MAX_LINES = 200;
    static constexpr int32_t MAX_LINE_LEN = 200;

public:
    explicit ConsoleManager(StruxProvider& strux);

    ConsoleManager(const ConsoleManager&) = delete;
    ConsoleManager& operator=(const ConsoleManager&) = delete;

    void Init();

    using BroadcastFunc = void (*)(const char* json, int32_t len, void* ctx);
    void SetBroadcastCallback(BroadcastFunc func, void* ctx);

    void WriteHistory(ReplyObject& resp) const;

private:
    StruxProvider& strux_;
    InitState initState_;

    // Ring buffer for log lines (allocated in PSRAM during Init)
    char (*lines_)[MAX_LINE_LEN] = nullptr;
    int32_t head_ = 0;
    int32_t count_ = 0;
    mutable Mutex mutex_;

    // Async broadcast via queue + task
    QueueHandle_t queue_ = nullptr;
    Task broadcastTask_;

    /// The broadcast task itself, learned when its loop starts. Sending a line to a
    /// client that has gone away makes esp_http_server log the failure, and this
    /// manager captures every log line there is — so that warning became one more line
    /// to send, down the same dead socket, which logged again. One dropped client turned
    /// into hundreds of lines in a few milliseconds, bounded only by this queue filling
    /// up. Lines raised on this task still reach the serial console and the ring buffer;
    /// what they no longer do is go back down the pipe that raised them.
    std::atomic<TaskHandle_t> broadcastTaskHandle_{nullptr};
    BroadcastFunc broadcastFunc_ = nullptr;
    void* broadcastCtx_ = nullptr;

    // Line accumulator (vprintf can be called multiple times per line)
    char lineBuf_[MAX_LINE_LEN] = {};
    int32_t lineLen_ = 0;

    void FlushLine();
    void StoreLine(const char* line, int32_t len);
    void BroadcastTaskLoop();

    static int LogOutput(const char* fmt, va_list args);
    static ConsoleManager* s_instance_;

    // ── WebSocket commands (registered with CommandManager in Init) ──
    RequestError Cmd_GetLogs(CommandContext& ctx);

    inline static CommandEntry commands_[] = {
        { "log", "list", &InvokeCommand<&ConsoleManager::Cmd_GetLogs> },
    };
};
