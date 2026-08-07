#pragma once

// ──────────────────────────────────────────────────────────────
// BoardContext configuration — ESP32-C3 SuperMini.
//
// The cheap ~18×22 mm C3 board: ESP32-C3FH4/FN4 (RISC-V, 4 MB flash, no
// PSRAM), USB-C wired to the chip's native USB Serial/JTAG, ceramic
// antenna. Pin assignments and constants for this board only. Other
// boards live in sibling folders under hardware/boards/ and are selected
// with -DBOARD=<name>.
// ──────────────────────────────────────────────────────────────

namespace BoardConfig
{
    // LED
    // The SuperMini's blue LED sits between 3V3 and GPIO8, so it lights
    // when the pin is driven LOW — active low, unlike the DevKit.
    // GPIO8 is also a boot strapping pin (it must read high at reset), but
    // it is only driven after the bootloader has run, so using it as an
    // output is safe. Holding the LED on across a reset is not.
    static constexpr int LED_PIN = 8;
    static constexpr bool LED_ACTIVE_HIGH = false;

    // Other fixed pins on this board, for reference when adding peripherals:
    //   GPIO9         BOOT button (strapping; low at reset = download mode)
    //   GPIO18/GPIO19 USB D-/D+ — taken by USB Serial/JTAG, do not reuse
    //   GPIO20/GPIO21 UART0 RX/TX (broken out, free if the console is on USB)
    // Everything else (GPIO0-GPIO7, GPIO10) is free.

    // Add project-specific pin definitions below.
    // Examples:
    //   static constexpr int MODBUS_TX_PIN = 21;
    //   static constexpr int MODBUS_RX_PIN = 20;
    //   static constexpr int SPI_MOSI_PIN  = 6;
}
