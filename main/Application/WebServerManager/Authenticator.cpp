#include "Authenticator.h"
#include "SettingsManager.h"
#include <cstring>
#include <esp_log.h>

// ──────────────────────────────────────────────────────────────
// Auth — the credential authority. Nothing above the transport
// edge (commands, streams) ever sees a token or password.
// ──────────────────────────────────────────────────────────────

void Authenticator::Register(SettingsManager& settings)
{
    settings.Register({ &webPassword_ });
    webPassword_.Get(passwordSnapshot_, sizeof(passwordSnapshot_));
}

void Authenticator::CheckPasswordEpoch()
{
    LOCK(authMutex_);
    char current[64] = {};
    webPassword_.Get(current, sizeof(current));
    if (strcmp(current, passwordSnapshot_) != 0)
    {
        ESP_LOGI(TAG, "web.password changed — clearing all sessions");
        sessions_.Clear();
        strlcpy(passwordSnapshot_, current, sizeof(passwordSnapshot_));
    }
}

bool Authenticator::ValidateKey(const char* key)
{
    CheckPasswordEpoch();
    return sessions_.Touch(key);
}

void Authenticator::TouchKey(const char* key)
{
    sessions_.Touch(key);
}

bool Authenticator::AuthRequired()
{
    char pw[64] = {};
    webPassword_.Get(pw, sizeof(pw));
    return pw[0] != '\0';
}

bool Authenticator::CheckPassword(const char* pw)
{
    CheckPasswordEpoch();
    char expected[64] = {};
    webPassword_.Get(expected, sizeof(expected));
    return strcmp(pw ? pw : "", expected) == 0;
}

void Authenticator::MintKey(char* out)
{
    sessions_.Create(out);
}
