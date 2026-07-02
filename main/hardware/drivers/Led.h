#pragma once

#include "BoardConfig.h"
#include "driver/gpio.h"

// ──────────────────────────────────────────────────────────────
// Simple GPIO LED driver.
// Pin and polarity are configured in BoardConfig.h.
// Set LED_PIN to -1 to compile out all LED code.
// ──────────────────────────────────────────────────────────────

class Led
{
public:
    void Init()
    {
        if constexpr (BoardConfig::LED_PIN < 0) return;

        gpio_config_t cfg = {};
        cfg.pin_bit_mask = 1ULL << BoardConfig::LED_PIN;
        cfg.mode = GPIO_MODE_OUTPUT;
        gpio_config(&cfg);

        Set(false);
    }

    void Set(bool on)
    {
        if constexpr (BoardConfig::LED_PIN < 0) return;
        state_ = on;
        gpio_set_level(static_cast<gpio_num_t>(BoardConfig::LED_PIN),
                       on == BoardConfig::LED_ACTIVE_HIGH ? 1 : 0);
    }

    void On() { Set(true); }
    void Off() { Set(false); }
    void Toggle() { Set(!state_); }
    bool IsOn() const { return state_; }

private:
    bool state_ = false;
};
