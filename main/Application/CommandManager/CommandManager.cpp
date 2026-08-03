#include "CommandManager.h"
#include "JsonHelpers.h"
#include "esp_log.h"
#include <algorithm>
#include <cstring>

CommandManager::CommandManager(ServiceProvider& serviceProvider)
    : serviceProvider_(serviceProvider)
{
}

void CommandManager::Init()
{
    auto initAttempt = initState_.TryBeginInit();
    if (!initAttempt)
    {
        ESP_LOGW(TAG, "Already initialized or initializing");
        return;
    }

    initAttempt.SetReady();
    ESP_LOGI(TAG, "Initialized");
}

bool CommandManager::Execute(const char* type, Stream& in, Stream& out)
{
    const CommandEntry* e = Find(type);
    if (e == nullptr)
        return false;

    // Handler runs OUTSIDE the lock: entries are immortal, so the pointer
    // stays valid, and a handler may register commands or dispatch nested
    // commands without deadlocking.
    e->handler(e->ctx, in, out);
    return true;
}

void CommandManager::Execute(Session& session)
{
    // The request's first chunk carries the header line — {"type":"...",...args}
    // terminated by '\n' — followed (for a streamed command) by the body. Peek
    // it (without consuming) to route on "type"; the handler then reads the same
    // line for its own args and the body from the same session (in == out).
    const uint8_t* head = nullptr;
    size_t headLen = 0;
    session.peekRequest(head, headLen);

    char line[128];
    size_t n = std::min(headLen, sizeof(line) - 1);
    memcpy(line, head, n);
    line[n] = '\0';
    if (char* nl = strchr(line, '\n')) *nl = '\0';

    char type[32] = {};
    ExtractJsonString(line, "type", type, sizeof(type));

    if (type[0] == '\0')
    {
        session.reject("missing type");
        return;
    }

    if (!Execute(type, session, session))
    {
        session.reject(type);   // unknown command
        return;
    }
    session.finish();   // FINAL — end of reply
}

const CommandEntry* CommandManager::Find(const char* name)
{
    LOCK(mutex_);
    for (CommandEntry* e = head_; e != nullptr; e = e->next)
        if (strcmp(name, e->name) == 0)
            return e;
    return nullptr;
}
