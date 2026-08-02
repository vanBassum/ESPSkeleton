#include "CommandSink.h"
#include "CommandManager.h"
#include "JsonHelpers.h"

#include <algorithm>
#include <cstring>

void CommandSink::OnSessionOpened(Session& session)
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

    if (!commands_ || !commands_->Execute(type, session, session))
    {
        session.reject(type);   // unknown command
        return;
    }
    session.finish();   // FINAL — end of reply
}
