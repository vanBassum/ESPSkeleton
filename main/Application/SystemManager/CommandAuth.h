#pragma once

#include "ServiceProvider.h"
#include "SystemManager.h"
#include "JsonWriter.h"
#include "JsonHelpers.h"

// ──────────────────────────────────────────────────────────────
// Transport-side auth glue for command handlers.
//
// Extracts the candidate PIN from the command's JSON payload, asks
// the credential owner (SystemManager) whether it matches, and
// writes the error response on failure. Call as the FIRST line of
// any handler that guards a state-changing command:
//
//   if (!CheckCommandAuth(serviceProvider_, json, resp))
//       return;
//
// This lives next to SystemManager on purpose: the PIN is its
// credential, so the glue that checks it belongs to it too — and
// the CommandManager folder stays free of manager dependencies.
// SystemManager itself only answers "does this PIN match?"; the
// stored PIN never leaves it, and JSON stays out of its domain API.
// ──────────────────────────────────────────────────────────────
inline bool CheckCommandAuth(ServiceProvider& sp, const char* json, JsonWriter& resp)
{
    char pin[64] = {};
    ExtractJsonString(json, "pin", pin, sizeof(pin));

    if (sp.getSystemManager().CheckPin(pin))
        return true;

    resp.field("ok", false);
    resp.field("error", "auth");
    return false;
}
