#include "CommandManager.h"
#include "JsonArgReader.h"
#include "esp_log.h"
#include <cstdio>
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

RequestError CommandManager::Execute(const char* category, const char* name,
                                     Stream& in, Stream& out,
                                     const char** failedArg)
{
    const CommandEntry* e = Find(category, name);
    if (e == nullptr)
        return RequestError::UnknownCommand;

    // Handler runs OUTSIDE the lock: entries are immortal, so the pointer
    // stays valid, and a handler may register commands or dispatch nested
    // commands without deadlocking.
    // Parse the arguments first, so the handler receives them already validated and
    // `in` already positioned at the body. JsonArgs is the only thing in the request
    // path holding a request-sized buffer — swapping in a token implementation here
    // deletes it without touching a single handler.
    JsonArgReader reader(in);
    CommandContext ctx(reader, in, out);
    const RequestError err = e->handler(e->ctx, ctx);
    if (err != RequestError::Ok && failedArg)
        *failedArg = reader.failedArgument();
    return err;
}

const char* DescribeRequestError(RequestError e, const char* arg, char* buf, size_t cap)
{
    switch (e)   // no default: a new RequestError must be handled here
    {
    case RequestError::Ok:              return "ok";
    case RequestError::UnknownCommand:  return "unknown command";
    case RequestError::MissingArgument:
        snprintf(buf, cap, "missing required argument: %s", arg ? arg : "?");
        return buf;
    case RequestError::UnknownArgument:
        snprintf(buf, cap, "unknown argument: %s", arg ? arg : "?");
        return buf;
    case RequestError::MalformedRequest:  return "malformed request";
    case RequestError::MalformedNumber:
        snprintf(buf, cap, "malformed number: %s", arg ? arg : "?");
        return buf;
    case RequestError::ArgumentTooLong:
        snprintf(buf, cap, "argument too long: %s", arg ? arg : "?");
        return buf;
    }
    return "bad request";
}

const CommandEntry* CommandManager::Find(const char* category, const char* name)
{
    LOCK(mutex_);
    for (CommandEntry* e = head_; e != nullptr; e = e->next)
        if (strcmp(name, e->name) == 0 && strcmp(category, e->category) == 0)
            return e;
    return nullptr;
}
