#pragma once

#include "ServiceProvider.h"
#include "InitState.h"
#include "TypedSettings.h"
#include "SessionProtocol.h"
#include "Task.h"

#include <cstddef>
#include <cstdint>

// Where a manager sends something it wants recorded over time.
//
// The shape is Influx's, because that is what it becomes: a measurement name, tags
// (what the point is about) and fields (what was measured). This class formats one
// line of Influx line protocol and hands it to the relay, which batches lines and
// POSTs them without reading them. So the *device* owns the format and the server
// owns the transport, which is the same split as everywhere else here.
//
// Usage from any manager, and note nothing needs releasing — a Point is a value that
// writes itself into a fixed buffer and sends on Commit():
//
//     auto p = telemetry.Measure("climate");
//     p.Tag("room", "kitchen");
//     p.Field("temperature", 21.5);
//     p.Field("humidity", 55);
//     p.Commit();
//
// There is NO buffering yet: a point taken while the relay is down is gone. That is a
// deliberate first cut — see docs/backlog for what buffering has to decide first.
class TelemetryManager
{
    static constexpr const char* TAG = "TelemetryManager";

    // One line of line protocol. Generous for a handful of tags and fields, and small
    // enough that a Point can live on the caller's stack.
    static constexpr size_t LINE_CAP = 320;

    // Its own task rather than a Timer: the FreeRTOS timer service task's stack is far
    // too small to hold a Point plus snprintf, and buffering (when it lands) will want
    // something that can block anyway.
    static constexpr int TASK_STACK = 3072;

public:
    // A single measurement under construction.
    //
    // Tags and fields accumulate into SEPARATE buffers and are joined at Commit(),
    // because line protocol requires every tag before every field. Keeping them apart
    // means a caller can interleave Tag() and Field() in any order without producing a
    // line the server would reject — the ordering rule is the format's problem, not
    // the caller's.
    class Point
    {
    public:
        Point(TelemetryManager* owner, const char* measurement);

        Point& Tag(const char* key, const char* value);

        Point& Field(const char* key, double value);
        Point& Field(const char* key, int32_t value);
        Point& Field(const char* key, bool value);
        Point& Field(const char* key, const char* value);

        /// Formats the line and hands it to the relay. A Point with no fields is not a
        /// measurement and is dropped with a warning — line protocol requires one.
        void Commit();

        /// False if anything did not fit; Commit() refuses to send a truncated line
        /// rather than write a half-measurement into the database.
        bool Ok() const { return ok_; }

    private:
        TelemetryManager* owner_;
        char   measurement_[64] = {};
        char   tags_[128]  = {};
        char   fields_[160] = {};
        size_t tagLen_   = 0;
        size_t fieldLen_ = 0;
        bool   ok_ = true;
        bool   committed_ = false;

        void AppendEscaped(char* buf, size_t cap, size_t& len, const char* s,
                           bool escapeEquals);
    };

    explicit TelemetryManager(ServiceProvider& serviceProvider);

    TelemetryManager(const TelemetryManager&) = delete;
    TelemetryManager& operator=(const TelemetryManager&) = delete;
    TelemetryManager(TelemetryManager&&) = delete;
    TelemetryManager& operator=(TelemetryManager&&) = delete;

    void Init();

    /// Start a measurement. Cheap even when telemetry is disabled — the Point is built
    /// and then dropped at Commit(), so callers never have to ask whether it is on.
    Point Measure(const char* measurement) { return Point(this, measurement); }

    bool IsEnabled() const { return enabled_.Get(); }

    /// Points formatted and points that could not be sent, since boot. Cheap to expose
    /// and the first thing worth knowing when a graph is empty.
    uint32_t Sent() const { return sent_; }
    uint32_t Dropped() const { return dropped_; }

private:
    ServiceProvider& serviceProvider_;
    InitState initState_;
    Task task_;

    uint32_t sent_ = 0;
    uint32_t dropped_ = 0;

    /// Appends `device=<id>` so every point says which board it came from without
    /// each caller remembering to, then sends the line on the telemetry session.
    void Send(const char* measurement, const char* tags, const char* fields);

    void TaskLoop();

    /// The one measurement this manager takes itself: free heap and uptime, every
    /// interval. It is the worked example of the API as much as it is useful data —
    /// and it means a fresh device produces something to look at without any feature
    /// having been written yet.
    void SampleVitals();

    // Off by default: a template should not start shipping measurements off the device
    // because someone flashed it.
    //
    // Keys are `telem.` and not `telemetry.` because an NVS key is at most 15 chars,
    // and Register() asserts on that at RUNTIME — `telemetry.enabled` is 17 and boots
    // straight into a reset loop. Nothing catches it at compile time.
    inline static BoolSetting enabled_{ "telem.enabled", "Telemetry Enabled", false };
    inline static UInt32Setting intervalSec_{ "telem.interval",
                                             "Telemetry Interval (s)", 60 };
};
