#pragma once

#include "ServiceProvider.h"
#include "InitState.h"
#include "drivers/Led.h"

class DeviceManager
{
    static constexpr const char *TAG = "DeviceManager";

public:
    explicit DeviceManager(ServiceProvider &serviceProvider);

    DeviceManager(const DeviceManager &) = delete;
    DeviceManager &operator=(const DeviceManager &) = delete;
    DeviceManager(DeviceManager &&) = delete;
    DeviceManager &operator=(DeviceManager &&) = delete;

    void Init();

    Led &getLed() { return led_; }

    // Projects add more hardware accessors here.
    // Example: DPS5020& getDPS5020() { return dps5020_; }

private:
    ServiceProvider &serviceProvider_;
    InitState initState_;

    // Hardware instances
    Led led_;
};
