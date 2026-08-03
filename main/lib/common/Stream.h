#pragma once

#include <cstddef>
#include <freertos/FreeRTOS.h>

class Stream
{
public:
    virtual ~Stream() = default;

    virtual size_t write(const void* data, size_t size, TickType_t timeout = portMAX_DELAY) = 0;
    virtual size_t read(void* buffer, size_t size, TickType_t timeout = portMAX_DELAY) = 0;

    virtual size_t available() const { return 0; }
    virtual bool flush() { return true; }

    /// True when the stream stopped early because something went wrong, rather
    /// than because it ended. read() returns 0 for both, so a reader that cares
    /// about receiving *all* of the input — a firmware image, say — has to ask;
    /// otherwise a transport failure is indistinguishable from a complete stream.
    virtual bool failed() const { return false; }
};
