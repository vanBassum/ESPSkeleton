#pragma once

// What one application manager may reach for: its peers, the framework beneath it, and
// the board beneath that. Implemented by ApplicationContext and handed to every app
// manager at construction — the same shape as StruxServices one layer down, so a manager
// still takes exactly one reference and finds everything through it.
//
// The two extra accessors are what make this the top layer: getStrux() reaches down to
// the framework (register a command, read a setting, take a telemetry point) and
// getBoard() reaches past it to the hardware. Neither points back up, and nothing in
// Strux or on the Board can see this interface at all.

class Board;
class StruxServices;
class LedManager;

class AppServices
{
public:
    /// The framework layer. Everything Strux offers is behind this one call.
    virtual StruxServices& getStrux() = 0;

    /// The hardware. Only the application layer has this — see StruxServices.h for why.
    virtual Board& getBoard() = 0;

    // ── This application's own managers ──
    virtual LedManager& getLedManager() = 0;
};
