#pragma once

// ──────────────────────────────────────────────────────────────
// SKETCH — board/device layering idea (2026-07-06). Not compiled.
//
// Idiom: the application defines role interfaces + a semantic enum;
// each board folder provides a concrete `Board` class that owns the
// driver instances and maps roles to them. `Board` replaces
// DeviceManager. Same include-path trick as BoardConfig.h: the app
// does `#include "Board.h"` and gets whichever board is selected
// with -DBOARD=<name>.
//
// Contract enforcement is duck-typed: no IBoard base class. If a
// board forgets a method the application uses, that board simply
// fails to compile. Drift between boards surfaces when each board
// is built (which -DBOARD= forces anyway).
//
// In real code each section below is its own file, marked with
// `// FILE:` headers.
// ──────────────────────────────────────────────────────────────


// ══════════════════════════════════════════════════════════════
// 1. Role interfaces — application vocabulary, board-independent.
//    A role earns an interface only when the application speaks in
//    that role. Small: 1–3 methods. Never chip vocabulary, never
//    GPIO/SPI plumbing.
// ══════════════════════════════════════════════════════════════

// FILE: main/hardware/interfaces/TemperatureSensor.h
class TemperatureSensor
{
public:
    virtual float GetTemperature() = 0;   // °C
    virtual ~TemperatureSensor() = default;
};

// FILE: main/hardware/interfaces/Roles.h
// Semantic roles the application expects. Boards map each role to
// an instance; a board that lacks the hardware binds a mock.
// Names mean something (`Water`), never positions (`Sensor_2`).
enum class Sensor
{
    Ambient,
    Water,
};


// ══════════════════════════════════════════════════════════════
// 2. Drivers — concrete chips, board-independent, implement roles.
//    Unchanged from today's driver story: buses injected by ctor
//    reference, extra chip features simply present on the concrete
//    type.
// ══════════════════════════════════════════════════════════════

// FILE: main/hardware/drivers/Aht12.h
class Aht12 : public TemperatureSensor
{
public:
    explicit Aht12(i2c_master_bus_handle_t bus);
    void  Init();
    float GetTemperature() override;
    float GetHumidity();              // extra feature: just there
};

// FILE: main/hardware/drivers/Ds18b20.h
class Ds18b20 : public TemperatureSensor
{
public:
    explicit Ds18b20(OneWireBus &bus);
    void  Init();
    float GetTemperature() override;
};

// FILE: main/hardware/drivers/MockTemperatureSensor.h
// A mock is a driver like any other.
class MockTemperatureSensor : public TemperatureSensor
{
public:
    explicit MockTemperatureSensor(float fixed) : value_(fixed) {}
    float GetTemperature() override { return value_; }

private:
    float value_;
};


// ══════════════════════════════════════════════════════════════
// 3. The board class — owns driver instances, maps roles.
//    Replaces DeviceManager. Lives in the board folder; Board.cpp
//    is added via BOARD_SOURCES in board.cmake. Follows the manager
//    pattern (ServiceProvider& ctor, InitState in Init).
// ══════════════════════════════════════════════════════════════

// FILE: main/hardware/boards/kc1245_rev_a/Board.h
// Rev A: AHT12 ambient on I2C, DS18B20 water probe on OneWire.
class Board
{
public:
    explicit Board(ServiceProvider &ctx);
    void Init();   // bus first, then drivers — init order lives here

    // ── Contract the application compiles against ──
    Led               &GetLed() { return led_; }
    TemperatureSensor &GetTemperatureSensor(Sensor role);

    // ── Escape hatch: full driver API, fork-specific code only ──
    OneWireBus        &GetOneWire() { return oneWire_; }

private:
    ServiceProvider &serviceProvider_;
    InitState        initState_;

    i2c_master_bus_handle_t i2cBus_{};   // owned host, handed to drivers
    OneWireBus oneWire_;
    Led        led_;
    Aht12      ambient_{ /* i2cBus_ wired in Init */ };
    Ds18b20    water_{ oneWire_ };
};

// FILE: main/hardware/boards/kc1245_rev_a/Board.cpp
TemperatureSensor &Board::GetTemperatureSensor(Sensor role)
{
    switch (role)
    {
    case Sensor::Ambient: return ambient_;
    case Sensor::Water:   return water_;
    }
    abort();   // unreachable while enum and switch stay in sync
}

// FILE: main/hardware/boards/kc1245_rev_b/Board.cpp
// Rev B has no water probe fitted — only its own folder changes.
//
//   TemperatureSensor &Board::GetTemperatureSensor(Sensor role)
//   {
//       switch (role)
//       {
//       case Sensor::Ambient: return ambient_;
//       case Sensor::Water:   return waterMock_;   // MockTemperatureSensor{ 20.0f }
//       }
//       abort();
//   }


// ══════════════════════════════════════════════════════════════
// 4. Application side — never changes between boards.
// ══════════════════════════════════════════════════════════════

// FILE: main/Application/ServiceProvider.h
//   Board& getBoard() = 0;              // replaces getDeviceManager()

// FILE: main/Application/ApplicationContext.h
//   #include "Board.h"                  // resolves per selected board
//   Board board_{ *this };
//   Board& getBoard() override { return board_; }

// FILE: main/main.cpp
//   g_appContext.getBoard().Init();     // same slot DeviceManager has today

// FILE: main/Application/HomeAssistantManager/HomeAssistantManager.cpp
//   float t = ctx.getBoard().GetTemperatureSensor(Sensor::Water).GetTemperature();
//   ctx.getBoard().GetLed().Set(on);
