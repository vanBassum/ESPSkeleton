#pragma once

#include "InitState.h"
#include "BoardConfig.h"
#include "interfaces/BoardProvider.h"
#include "drivers/GpioLed.h"

// ──────────────────────────────────────────────────────────────
// The board layer's context for the ESP32-C3 SuperMini: owns every
// hardware driver instance (and bus host) and answers BoardProvider.
//
// The bottom layer, and it depends on nothing above it — not the
// framework, not the application. Drivers take their pins and buses as
// constructor arguments, so nothing here needs the provider to find a
// peer; BoardProvider exists for the layer above.
//
// Every board folder provides a class named BoardContext; #include
// "BoardContext.h" resolves to the board selected with -DBOARD=<name>.
//
// Surface rules:
//   • role interfaces (Led&, ...) for devices the application
//     addresses by meaning — declared on BoardProvider, so every board
//     owes every role and binds a Mock* driver when not fitted;
//   • concrete driver accessors are the escape hatch for when the
//     application needs a driver's full API. Those stay OFF
//     BoardProvider and are checked at compile time, which is what
//     stops the role list becoming the union of every board's
//     peripherals.
// ──────────────────────────────────────────────────────────────

class BoardContext : public BoardProvider
{
    static constexpr const char *TAG = "Board";

public:
    BoardContext() = default;

    BoardContext(const BoardContext &) = delete;
    BoardContext &operator=(const BoardContext &) = delete;
    BoardContext(BoardContext &&) = delete;
    BoardContext &operator=(BoardContext &&) = delete;

    void Init();

    Led &GetLed() override { return led_; }

private:
    InitState initState_;

    // Hardware instances — buses first, then the drivers that use them.
    GpioLed led_{ BoardConfig::LED_PIN, BoardConfig::LED_ACTIVE_HIGH };
};
