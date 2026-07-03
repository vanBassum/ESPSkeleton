#pragma once

#include "ServiceProvider.h"
#include "InitState.h"
#include "CommandEntry.h"
#include "Mutex.h"
#include "Task.h"
#include "freertos/queue.h"
#include <cstdint>

class JsonWriter;

class ConsoleManager {
    static constexpr const char* TAG = "ConsoleManager";
    static constexpr int32_t QUEUE_DEPTH = 16;

public:
    static constexpr int32_t MAX_LINES = 200;
    static constexpr int32_t MAX_LINE_LEN = 200;

public:
    explicit ConsoleManager(ServiceProvider& serviceProvider);

    ConsoleManager(const ConsoleManager&) = delete;
    ConsoleManager& operator=(const ConsoleManager&) = delete;

    void Init();

    using BroadcastFunc = void (*)(const char* json, int32_t len, void* ctx);
    void SetBroadcastCallback(BroadcastFunc func, void* ctx);

    void WriteHistory(JsonWriter& writer) const;

private:
    ServiceProvider& serviceProvider_;
    InitState initState_;

    // Ring buffer for log lines (allocated in PSRAM during Init)
    char (*lines_)[MAX_LINE_LEN] = nullptr;
    int32_t head_ = 0;
    int32_t count_ = 0;
    mutable Mutex mutex_;

    // Async broadcast via queue + task
    QueueHandle_t queue_ = nullptr;
    Task broadcastTask_;
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
    void Cmd_GetLogs(const char* json, JsonWriter& resp);

    inline static CommandEntry commands_[] = {
        { "getLogs", &InvokeCommand<&ConsoleManager::Cmd_GetLogs> },
    };
};
