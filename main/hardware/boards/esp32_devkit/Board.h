#pragma once

#include "ServiceProvider.h"
#include "InitState.h"
#include "BoardConfig.h"
#include "drivers/GpioLed.h"

// ──────────────────────────────────────────────────────────────
// Board for the generic ESP32 DevKit. Owns every hardware driver
// instance (and bus host) and exposes the capability surface the
// application compiles against.
//
// Every board folder provides a class named Board with the same
// surface; #include "Board.h" resolves to the board selected with
// -DBOARD=<name>. There is deliberately no IBoard base class: a
// board that misses a method the application uses fails to compile
// for that board.
//
// Surface rules:
//   • role interfaces (Led&, ...) for devices the application
//     addresses by meaning — bind a Mock* driver when not fitted;
//   • concrete driver accessors are allowed as an escape hatch
//     when the application needs a driver's full API.
// ──────────────────────────────────────────────────────────────

class Board
{
    static constexpr const char *TAG = "Board";

public:
    explicit Board(ServiceProvider &serviceProvider);

    Board(const Board &) = delete;
    Board &operator=(const Board &) = delete;
    Board(Board &&) = delete;
    Board &operator=(Board &&) = delete;

    void Init();

    Led &GetLed() { return led_; }

private:
    ServiceProvider &serviceProvider_;
    InitState initState_;

    // Hardware instances — buses first, then the drivers that use them.
    GpioLed led_{ BoardConfig::LED_PIN, BoardConfig::LED_ACTIVE_HIGH };
};
