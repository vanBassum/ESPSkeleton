#include "ConsoleManager.h"
#include "CommandManager.h"
#include "JsonWriter.h"
#include "JsonScope.h"
#include "BufferStream.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <cstdio>
#include <cstring>
#include <cassert>

ConsoleManager* ConsoleManager::s_instance_ = nullptr;

struct LogQueueItem {
    char text[ConsoleManager::MAX_LINE_LEN];
};

ConsoleManager::ConsoleManager(ServiceProvider& serviceProvider)
    : serviceProvider_(serviceProvider)
{
}

void ConsoleManager::Init()
{
    auto initAttempt = initState_.TryBeginInit();
    if (!initAttempt)
    {
        return;
    }

    s_instance_ = this;

    // Allocate log ring buffer in PSRAM (40KB)
    lines_ = static_cast<char (*)[MAX_LINE_LEN]>(
        heap_caps_calloc(MAX_LINES, MAX_LINE_LEN, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!lines_)
        lines_ = static_cast<char (*)[MAX_LINE_LEN]>(calloc(MAX_LINES, MAX_LINE_LEN));
    assert(lines_ && "Failed to allocate log buffer");

    queue_ = xQueueCreate(QUEUE_DEPTH, sizeof(LogQueueItem));

    broadcastTask_.Init("ConsoleBroadcast", 5, 4096);
    broadcastTask_.SetHandler([this]() { BroadcastTaskLoop(); });
    broadcastTask_.Run();

    esp_log_set_vprintf(&LogOutput);

    // ConsoleManager initializes before CommandManager::Init() — fine by
    // design: the registry is usable from construction.
    serviceProvider_.getCommandManager().Register(this, commands_);

    initAttempt.SetReady();
    ESP_LOGI(TAG, "Initialized (capturing stdout)");
}

void ConsoleManager::SetBroadcastCallback(BroadcastFunc func, void* ctx)
{
    broadcastFunc_ = func;
    broadcastCtx_ = ctx;
}

int ConsoleManager::LogOutput(const char* fmt, va_list args)
{
    int ret = vprintf(fmt, args);

    if (!s_instance_) return ret;

    // Format into a temp buffer
    char buf[MAX_LINE_LEN];
    va_list args_copy;
    va_copy(args_copy, args);
    int len = vsnprintf(buf, sizeof(buf), fmt, args_copy);
    va_end(args_copy);

    if (len <= 0) return ret;
    if (len >= static_cast<int>(sizeof(buf))) len = sizeof(buf) - 1;

    // Accumulate into lineBuf_, flush on newline
    auto* self = s_instance_;
    for (int i = 0; i < len; i++)
    {
        char c = buf[i];
        if (c == '\n')
        {
            self->FlushLine();
        }
        else if (c != '\r')
        {
            if (self->lineLen_ < MAX_LINE_LEN - 1)
            {
                self->lineBuf_[self->lineLen_++] = c;
            }
        }
    }

    return ret;
}

void ConsoleManager::FlushLine()
{
    if (lineLen_ == 0) return;

    lineBuf_[lineLen_] = '\0';

    StoreLine(lineBuf_, lineLen_);

    if (queue_)
    {
        LogQueueItem item = {};
        strncpy(item.text, lineBuf_, sizeof(item.text) - 1);
        xQueueSend(queue_, &item, 0);
    }

    lineLen_ = 0;
}

void ConsoleManager::StoreLine(const char* line, int32_t len)
{
    LOCK(mutex_);
    if (len >= MAX_LINE_LEN) len = MAX_LINE_LEN - 1;
    memcpy(lines_[head_], line, len);
    lines_[head_][len] = '\0';
    head_ = (head_ + 1) % MAX_LINES;
    if (count_ < MAX_LINES) count_++;
}

void ConsoleManager::BroadcastTaskLoop()
{
    LogQueueItem item;

    while (true)
    {
        if (xQueueReceive(queue_, &item, portMAX_DELAY) != pdTRUE)
            continue;

        if (!broadcastFunc_)
            continue;

        char jsonBuf[MAX_LINE_LEN + 32];
        BufferStream stream(jsonBuf, sizeof(jsonBuf));
        JsonWriter json(stream);
        json.beginObject();
        json.field("log", item.text);
        json.endObject();

        broadcastFunc_(stream.data(), stream.length(), broadcastCtx_);
    }
}

void ConsoleManager::WriteHistory(JsonObject& resp) const
{
    LOCK(mutex_);

    JsonArray lines = resp.array("lines");

    int32_t start = (count_ < MAX_LINES) ? 0 : head_;
    for (int32_t i = 0; i < count_; i++)
    {
        int32_t idx = (start + i) % MAX_LINES;
        lines.value(lines_[idx]);
    }
}   // `lines` closes here; caller's `resp` stays usable (auto-detached)

// ──────────────────────────────────────────────────────────────
// WebSocket commands
// ──────────────────────────────────────────────────────────────

void ConsoleManager::Cmd_GetLogs(Stream& in, Stream& out)
{
    JsonObject resp(out);
    WriteHistory(resp);
}
