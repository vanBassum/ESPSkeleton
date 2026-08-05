#include "TelemetryManager.h"
#include "SettingsManager.h"
#include "RelayManager.h"
#include "DateTime.h"

#include <esp_log.h>
#include <esp_system.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cstdio>
#include <cstring>

TelemetryManager::TelemetryManager(ServiceProvider& serviceProvider)
    : serviceProvider_(serviceProvider)
{
}

void TelemetryManager::Init()
{
    auto initAttempt = initState_.TryBeginInit();
    if (!initAttempt)
    {
        ESP_LOGW(TAG, "Already initialized or initializing");
        return;
    }

    serviceProvider_.getSettingsManager().Register({ &enabled_, &intervalSec_ });

    if (!enabled_.Get())
    {
        ESP_LOGI(TAG, "Disabled (set telemetry.enabled to record)");
        initAttempt.SetReady();
        return;
    }

    task_.Init("telemetry", 3, TASK_STACK);
    task_.SetHandler([this] { TaskLoop(); });
    if (!task_.Run())
    {
        ESP_LOGE(TAG, "Failed to start telemetry task");
        return;
    }

    ESP_LOGI(TAG, "Enabled, sampling every %us", intervalSec_.Get());
    initAttempt.SetReady();
}

void TelemetryManager::TaskLoop()
{
    for (;;)
    {
        uint32_t period = intervalSec_.Get();
        if (period == 0) period = 60;          // 0 would be a busy loop
        vTaskDelay(pdMS_TO_TICKS(period * 1000));
        SampleVitals();
    }
}

void TelemetryManager::SampleVitals()
{
    auto p = Measure("device");
    p.Field("heapFree", static_cast<int32_t>(esp_get_free_heap_size()));
    p.Field("heapMin", static_cast<int32_t>(esp_get_minimum_free_heap_size()));
    // esp_timer counts microseconds since boot; seconds is the useful unit and keeps
    // the value inside an int32 for over 60 years.
    p.Field("uptime", static_cast<int32_t>(esp_timer_get_time() / 1000000));
    p.Commit();
}

// ──────────────────────────────────────────────────────────────
// Point
// ──────────────────────────────────────────────────────────────

TelemetryManager::Point::Point(TelemetryManager* owner, const char* measurement)
    : owner_(owner)
{
    // A measurement name escapes commas and spaces but NOT equals — the only one of
    // the three positions where that is true.
    size_t len = 0;
    AppendEscaped(measurement_, sizeof(measurement_), len, measurement, false);
}

void TelemetryManager::Point::AppendEscaped(char* buf, size_t cap, size_t& len,
                                            const char* s, bool escapeEquals)
{
    if (!s) return;
    for (; *s; ++s)
    {
        const char c = *s;
        const bool needsEscape = (c == ',' || c == ' ' || (escapeEquals && c == '='));
        const size_t need = needsEscape ? 2u : 1u;
        if (len + need >= cap)      // >= leaves room for the NUL
        {
            ok_ = false;
            break;
        }
        if (needsEscape) buf[len++] = '\\';
        buf[len++] = c;
    }
    buf[len] = '\0';
}

TelemetryManager::Point& TelemetryManager::Point::Tag(const char* key, const char* value)
{
    // Skip empty values rather than emit `key=`, which Influx rejects for a tag.
    if (!key || !*key || !value || !*value) return *this;

    if (tagLen_ + 1 < sizeof(tags_)) tags_[tagLen_++] = ',';
    AppendEscaped(tags_, sizeof(tags_), tagLen_, key, true);
    if (tagLen_ + 1 < sizeof(tags_)) tags_[tagLen_++] = '=';
    AppendEscaped(tags_, sizeof(tags_), tagLen_, value, true);
    tags_[tagLen_] = '\0';
    return *this;
}

TelemetryManager::Point& TelemetryManager::Point::Field(const char* key, double value)
{
    if (!key || !*key) return *this;
    if (fieldLen_ + 1 < sizeof(fields_)) fields_[fieldLen_++] = ',';
    AppendEscaped(fields_, sizeof(fields_), fieldLen_, key, true);

    // %.6g rather than %f: a temperature is 21.5, not 21.500000, and the shorter line
    // is the one that has to fit in a chunk.
    const int n = snprintf(fields_ + fieldLen_, sizeof(fields_) - fieldLen_,
                           "=%.6g", value);
    if (n < 0 || static_cast<size_t>(n) >= sizeof(fields_) - fieldLen_) ok_ = false;
    else fieldLen_ += static_cast<size_t>(n);
    return *this;
}

TelemetryManager::Point& TelemetryManager::Point::Field(const char* key, int32_t value)
{
    if (!key || !*key) return *this;
    if (fieldLen_ + 1 < sizeof(fields_)) fields_[fieldLen_++] = ',';
    AppendEscaped(fields_, sizeof(fields_), fieldLen_, key, true);

    // The trailing `i` is what makes Influx store this as an integer instead of a
    // float. Without it the field's type silently becomes float, and a later integer
    // write to the same field is then rejected as a type conflict.
    const int n = snprintf(fields_ + fieldLen_, sizeof(fields_) - fieldLen_,
                           "=%ldi", static_cast<long>(value));
    if (n < 0 || static_cast<size_t>(n) >= sizeof(fields_) - fieldLen_) ok_ = false;
    else fieldLen_ += static_cast<size_t>(n);
    return *this;
}

TelemetryManager::Point& TelemetryManager::Point::Field(const char* key, bool value)
{
    if (!key || !*key) return *this;
    if (fieldLen_ + 1 < sizeof(fields_)) fields_[fieldLen_++] = ',';
    AppendEscaped(fields_, sizeof(fields_), fieldLen_, key, true);

    const int n = snprintf(fields_ + fieldLen_, sizeof(fields_) - fieldLen_,
                           "=%c", value ? 't' : 'f');
    if (n < 0 || static_cast<size_t>(n) >= sizeof(fields_) - fieldLen_) ok_ = false;
    else fieldLen_ += static_cast<size_t>(n);
    return *this;
}

TelemetryManager::Point& TelemetryManager::Point::Field(const char* key, const char* value)
{
    if (!key || !*key || !value) return *this;
    if (fieldLen_ + 1 < sizeof(fields_)) fields_[fieldLen_++] = ',';
    AppendEscaped(fields_, sizeof(fields_), fieldLen_, key, true);

    // A string field is quoted, and inside the quotes only `"` and `\` are escaped —
    // commas and spaces are fine there, unlike everywhere else on the line.
    if (fieldLen_ + 2 < sizeof(fields_))
    {
        fields_[fieldLen_++] = '=';
        fields_[fieldLen_++] = '"';
    }
    for (const char* s = value; *s; ++s)
    {
        const bool q = (*s == '"' || *s == '\\');
        const size_t need = q ? 3u : 2u;   // +1 to keep room for the closing quote
        if (fieldLen_ + need >= sizeof(fields_)) { ok_ = false; break; }
        if (q) fields_[fieldLen_++] = '\\';
        fields_[fieldLen_++] = *s;
    }
    if (fieldLen_ + 1 < sizeof(fields_)) fields_[fieldLen_++] = '"';
    fields_[fieldLen_] = '\0';
    return *this;
}

void TelemetryManager::Point::Commit()
{
    if (committed_) return;      // a Point sends once
    committed_ = true;

    if (fieldLen_ == 0)
    {
        ESP_LOGW(TAG, "point '%s' has no fields — not sent", measurement_);
        return;
    }
    if (!ok_)
    {
        // Refused rather than truncated: half a line is either a parse error at the
        // server or, worse, a valid line missing a field.
        ESP_LOGW(TAG, "point '%s' did not fit — not sent", measurement_);
        owner_->dropped_++;
        return;
    }
    owner_->Send(measurement_, tags_, fields_);
}

// ──────────────────────────────────────────────────────────────
// Sending
// ──────────────────────────────────────────────────────────────

void TelemetryManager::Send(const char* measurement, const char* tags,
                            const char* fields)
{
    if (!enabled_.Get()) return;

    auto& relay = serviceProvider_.getRelayManager();
    if (!relay.IsConnected())
    {
        // No buffer yet, so this point is gone. Counted rather than logged: a
        // disconnected relay would otherwise produce a log line per measurement.
        dropped_++;
        return;
    }

    char line[LINE_CAP] = {};

    // `device=<id>` first, so every point is attributable without each caller
    // remembering to tag it. The relay does not verify this, which is fine while every
    // device is one we installed — noted in the backlog.
    const char* deviceId = relay.GetDeviceId();

    // A timestamp only when the clock is actually set. Unsynced, the device would
    // stamp everything 1970 and the points would land outside the retention window;
    // omitting the field lets Influx use arrival time, which without buffering is
    // within a second of the truth anyway.
    DateTime now = DateTime::Now();
    int n;
    if (now.YearLocal() >= 2020)
    {
        n = snprintf(line, sizeof(line), "%s,device=%s%s %s %lld000000000",
                     measurement, deviceId, tags, fields,
                     static_cast<long long>(now.UtcSeconds()));
    }
    else
    {
        n = snprintf(line, sizeof(line), "%s,device=%s%s %s",
                     measurement, deviceId, tags, fields);
    }

    if (n < 0 || static_cast<size_t>(n) >= sizeof(line))
    {
        ESP_LOGW(TAG, "line for '%s' did not fit — not sent", measurement);
        dropped_++;
        return;
    }

    if (relay.BroadcastTelemetry(line, n))
        sent_++;
    else
        dropped_++;
}
