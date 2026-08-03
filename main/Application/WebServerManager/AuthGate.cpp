#include "AuthGate.h"
#include "Authenticator.h"
#include "WsConnection.h"
#include "SessionProtocol.h"
#include "JsonHelpers.h"
#include <algorithm>
#include <cstring>
#include <cstdio>

void AuthGate::SendReply(SessionLink& link, uint16_t sid, const char* json)
{
    uint8_t buf[session::HEADER_LEN + 160];
    size_t n = strlen(json);
    if (n > sizeof(buf) - session::HEADER_LEN) n = sizeof(buf) - session::HEADER_LEN;
    session::writeHeader(buf, sid, session::FLAG_FINAL);
    memcpy(buf + session::HEADER_LEN, json, n);
    link.SendRaw(buf, session::HEADER_LEN + n);
}

void AuthGate::SendReject(SessionLink& link, uint16_t sid, const char* reason)
{
    uint8_t buf[session::HEADER_LEN + 32];
    size_t n = strlen(reason);
    if (n > sizeof(buf) - session::HEADER_LEN) n = sizeof(buf) - session::HEADER_LEN;
    session::writeHeader(buf, sid, session::FLAG_REJECT);
    memcpy(buf + session::HEADER_LEN, reason, n);
    link.SendRaw(buf, session::HEADER_LEN + n);
}

AuthGate::Disposition AuthGate::Handle(WsConnection& conn, SessionLink& link,
                                       uint16_t sid, const uint8_t* payload, size_t len)
{
    char line[160];
    size_t n = std::min(len, sizeof(line) - 1);
    memcpy(line, payload, n);
    line[n] = '\0';
    if (char* nl = strchr(line, '\n')) *nl = '\0';

    char type[16] = {};
    ExtractJsonString(line, "type", type, sizeof(type));

    if (strcmp(type, "hello") == 0)
    {
        SendReply(link, sid, auth_.AuthRequired() ? "{\"authRequired\":true}"
                                                  : "{\"authRequired\":false}");
        return Disposition::Handled;
    }
    if (strcmp(type, "login") == 0)
    {
        char pw[64] = {};
        ExtractJsonString(line, "password", pw, sizeof(pw));
        if (auth_.CheckPassword(pw))
        {
            char key[SessionTable::TOKEN_LEN] = {};
            auth_.MintKey(key);
            conn.authenticate(key);
            char reply[64];
            snprintf(reply, sizeof(reply), "{\"ok\":true,\"key\":\"%s\"}", key);
            SendReply(link, sid, reply);
        }
        else SendReply(link, sid, "{\"ok\":false}");
        return Disposition::Handled;
    }
    if (strcmp(type, "auth") == 0)
    {
        char key[SessionTable::TOKEN_LEN] = {};
        ExtractJsonString(line, "key", key, sizeof(key));
        if (auth_.ValidateKey(key)) { conn.authenticate(key); SendReply(link, sid, "{\"ok\":true}"); }
        else SendReply(link, sid, "{\"ok\":false}");
        return Disposition::Handled;
    }

    if (conn.authed) return Disposition::Dispatch;
    SendReject(link, sid, "unauthorized");
    return Disposition::Rejected;
}
