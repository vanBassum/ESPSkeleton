#pragma once
#include "SessionTable.h"
#include "TypedSettings.h"
#include "Mutex.h"
#include <cstddef>

class SettingsManager;

// The credential authority: owns web.password + the RAM session-key table +
// change detection. Transport-neutral; no connection state. Owned by
// WebServerManager (a plain class, not a ServiceProvider manager).
class Authenticator {
public:
    void Register(SettingsManager& settings);   // register web.password

    bool AuthRequired();                 // web.password non-empty
    bool CheckPassword(const char* pw);  // epoch-check, then compare
    void MintKey(char* out);             // SessionTable::Create (out >= SessionTable::TOKEN_LEN)
    bool ValidateKey(const char* key);   // epoch-check, then SessionTable::Touch
    void TouchKey(const char* key);      // SessionTable::Touch (refresh only)

private:
    static constexpr const char* TAG = "Authenticator";

    inline static StringSetting webPassword_{ "web.password", "Web Password", "" };
    SessionTable sessions_;
    char passwordSnapshot_[64] = {};
    Mutex authMutex_;
    void CheckPasswordEpoch();           // clears sessions_ when web.password changed
};
