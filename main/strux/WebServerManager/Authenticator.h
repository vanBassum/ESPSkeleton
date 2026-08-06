#pragma once
#include "SessionTable.h"
#include "TypedSettings.h"
#include "Mutex.h"
#include <cstddef>

// The credential authority: checks the configured password, owns the RAM
// session-key table, and detects a password change. Transport-neutral; no
// connection state. Owned by WebServerManager (a plain class, not a
// StruxProvider manager).
//
// It does not DECLARE the password setting — settings belong to the manager that
// owns them, and this is not a manager. It holds a reference to WebServerManager's
// setting and reads it live on every check: a copy taken at Init would make
// change detection blind, since nothing notifies on a setting write.
class Authenticator {
public:
    explicit Authenticator(StringSetting& password) : password_(password) {}

    /// Snapshot the stored password. Must run AFTER the owning manager has
    /// registered the setting, or the first check would see NVS-value-vs-default
    /// and report a spurious password change.
    void Init();

    bool AuthRequired();                 // password non-empty
    bool CheckPassword(const char* pw);  // epoch-check, then compare
    void MintKey(char* out);             // SessionTable::Create (out >= SessionTable::TOKEN_LEN)
    bool ValidateKey(const char* key);   // epoch-check, then SessionTable::Touch
    void TouchKey(const char* key);      // SessionTable::Touch (refresh only)

private:
    static constexpr const char* TAG = "Authenticator";

    StringSetting& password_;            // declared and registered by WebServerManager
    SessionTable sessions_;
    char passwordSnapshot_[64] = {};
    Mutex authMutex_;
    void CheckPasswordEpoch();           // clears sessions_ when the password changed
};
