#include "CommandManager.h"
#include "JsonArgReader.h"
#include "DescribeArgReader.h"
#include "JsonScope.h"
#include "Stream.h"
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

    Register(this, commands_);

    initAttempt.SetReady();
    ESP_LOGI(TAG, "Initialized");
}

RequestError CommandManager::Execute(const char* category, const char* name,
                                     Stream& in, Stream& out,
                                     ConnectionAuth* connection,
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
    CommandContext ctx(reader, in, out, connection);
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
    case RequestError::MalformedRequest:  return "malformed request";
    case RequestError::MalformedNumber:
        snprintf(buf, cap, "malformed number: %s", arg ? arg : "?");
        return buf;
    case RequestError::ArgumentTooLong:
        snprintf(buf, cap, "argument too long: %s", arg ? arg : "?");
        return buf;
    case RequestError::Described:  return "described";   // help swallows this
    }
    return "bad request";
}

// ──────────────────────────────────────────────────────────────
// help
// ──────────────────────────────────────────────────────────────

namespace {

// The streams the described handler gets. It is stopped at its readArgs call, so
// these exist to be unused — and to mean that a handler which somehow reaches its
// body writes to nobody instead of to the client.
class NullStream final : public Stream
{
public:
    size_t write(const void*, size_t size, TickType_t = portMAX_DELAY) override { return size; }
    size_t read(void*, size_t, TickType_t = portMAX_DELAY) override { return 0; }
};

} // namespace

RequestError CommandManager::Cmd_Help(CommandContext& ctx)
{
    char category[MAX_ROUTE] = {};
    char command[MAX_ROUTE]  = {};

    RETURN_IF_ERROR(ctx.readArgs(
        Optional("category", category),
        Optional("command",  command)
    ));

    if (command[0] != '\0')
        return DescribeCommand(category, command, ctx.out);

    if (category[0] != '\0')
        ListCategory(category, ctx.out);
    else
        ListCategories(ctx.out);

    return RequestError::Ok;
}

void CommandManager::ListCategories(Stream& out)
{
    // Held across the JSON, so a manager registering from another task cannot relink
    // the chain half way through the answer. Safe to write to `out` from under it:
    // nothing acquires this mutex from inside a transport's send path, so there is no
    // pair of locks to take in two orders.
    LOCK(mutex_);

    // The chain has no notion of a category, so collect the distinct ones first.
    // Fixed array, and it says so when it fills up rather than quietly answering with
    // part of the registry.
    const char* seen[MAX_CATEGORIES];
    size_t count = 0;
    bool truncated = false;

    for (const CommandEntry* e = head_; e != nullptr; e = e->next)
    {
        bool known = false;
        for (size_t i = 0; i < count && !known; ++i)
            known = strcmp(seen[i], e->category) == 0;
        if (known) continue;

        if (count == MAX_CATEGORIES) { truncated = true; break; }
        seen[count++] = e->category;
    }

    JsonObject resp(out);
    resp.field("ok", true);
    {
        JsonArray cats = resp.array("categories");
        for (size_t i = 0; i < count; ++i)
        {
            JsonObject cat = cats.object();
            cat.field("category", seen[i]);
            JsonArray names = cat.array("commands");
            for (const CommandEntry* e = head_; e != nullptr; e = e->next)
                if (strcmp(seen[i], e->category) == 0)
                    names.value(e->name);
        }
    }
    if (truncated)
        resp.field("truncated", true);
}

void CommandManager::ListCategory(const char* category, Stream& out)
{
    LOCK(mutex_);   // see ListCategories

    JsonObject resp(out);

    bool found = false;
    for (const CommandEntry* e = head_; e != nullptr && !found; e = e->next)
        found = strcmp(category, e->category) == 0;

    if (!found)
    {
        resp.field("ok", false);
        resp.field("error", "unknown category");
        return;
    }

    resp.field("ok", true);
    resp.field("category", category);
    JsonArray names = resp.array("commands");
    for (const CommandEntry* e = head_; e != nullptr; e = e->next)
        if (strcmp(category, e->category) == 0)
            names.value(e->name);
}

RequestError CommandManager::DescribeCommand(const char* category, const char* command,
                                             Stream& out)
{
    JsonObject resp(out);

    if (category[0] == '\0')
    {
        // Routes are two words all the way down, so a command without its category
        // is not a route. Meaning rather than form, so it goes in the reply.
        resp.field("ok", false);
        resp.field("error", "a command needs its category");
        return RequestError::Ok;
    }

    const CommandEntry* e = Find(category, command);
    if (e == nullptr)
    {
        resp.field("ok", false);
        resp.field("error", "unknown command");
        return RequestError::Ok;
    }

    resp.field("ok", true);
    resp.field("category", e->category);
    resp.field("command", e->name);

    // Not through Execute(): that one is the wire path — it builds the reader for
    // today's format and reports which argument a parse tripped over. Here the reader
    // IS the point, and there is no request to parse.
    JsonArray args = resp.array("arguments");
    DescribeArgReader reader(args);
    NullStream sink;
    CommandContext described(reader, sink, sink, nullptr);
    const RequestError r = e->handler(e->ctx, described);

    if (r != RequestError::Described)
    {
        // The handler returned without ever asking for its arguments, which means it
        // ran its body — under help, against streams that go nowhere. Nothing here
        // can undo that; the fix is a readArgs call in the handler.
        ESP_LOGE(TAG, "'%s %s' declares no arguments — its body ran under help",
                 e->category, e->name);
        resp.field("declared", false);   // closes `args`
    }
    return RequestError::Ok;
}

const CommandEntry* CommandManager::Find(const char* category, const char* name)
{
    LOCK(mutex_);
    for (CommandEntry* e = head_; e != nullptr; e = e->next)
        if (strcmp(name, e->name) == 0 && strcmp(category, e->category) == 0)
            return e;
    return nullptr;
}
