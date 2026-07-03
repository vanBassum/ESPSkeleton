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
//   if (!CheckCommandAuth(self->serviceProvider_, json, resp))
//       return;
//
// This lives with the command layer on purpose: JSON in/out is edge
// business. SystemManager only answers "does this PIN match?" and
// the stored PIN never leaves it.
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
