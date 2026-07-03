#pragma once

// ══════════════════════════════════════════════════════════════
// DESIGN SKETCH — not built, not included anywhere.
// Companion to ideas/settings-refactor.md.
//
// Distributed, strongly-typed settings using the registration
// pattern (see ideas/registration-pattern.h and the CommandManager
// registry). Each manager owns its settings as typed inline static
// members and registers them in Init(). SettingsManager becomes a
// pure schema registry + NVS storage; SettingsDefs.h dies.
//
// Key properties (beyond the pattern's usual ones):
//   • Typed defaults: 1883 as an int, false as a bool — not "1883"
//     strings parsed at boot. ApplyDefaults() disappears: Get()
//     falls back to the entry's default when NVS has no value, so
//     "never changed" stays distinguishable from "set to default".
//   • Settings are PRIVATE to their owner. Cross-cutting reads go
//     through the owner's typed API (getSystemManager().
//     GetDeviceName(...)), never by string key. Each key string
//     exists in exactly ONE place: here, in the owner's definition.
//   • Registration in Init(), unconditionally — never register-on-
//     first-use (unused settings must still show in the frontend).
//     Stamping mgr/linking needs no NVS; owners read their own
//     settings in their own Init(), which runs after
//     SettingsManager::Init().
// ══════════════════════════════════════════════════════════════

#include <cstdint>
#include <cstdlib>
#include <cassert>
#include <initializer_list>
#include "esp_log.h"
#include "JsonWriter.h"

class SettingsManager;

// ──────────────────────────────────────────────────────────────
// Base entry — the intrusive chain link. Owners declare the typed
// leaves below, never this directly.
// ──────────────────────────────────────────────────────────────

struct Setting
{
    const char* key;    // NVS key — a storage detail, not an API
    const char* label;  // shown in the generated settings UI

    // Managed by SettingsManager::Register() — owners never touch these.
    SettingsManager* mgr = nullptr;
    Setting* next = nullptr;
    bool registered = false;

    // Writes {key, label, type, value} for the settings UI, and applies
    // a value arriving from the frontend (setSetting path). Virtual so
    // the generic UI/dispatch code never switches on a type tag; costs
    // one vtable pointer per entry.
    virtual void WriteJson(JsonWriter& json) const = 0;
    virtual bool ApplyFromString(const char* value) = 0;

    // Same guard as CommandEntry: a registered entry is a live chain
    // link; destroying it aborts on the first run of the offending code.
    virtual ~Setting()
    {
        if (registered)
        {
            ESP_LOGE("Setting", "registered setting '%s' destroyed — "
                     "setting tables must live for the whole application", key);
            abort();
        }
    }

protected:
    Setting(const char* key, const char* label) : key(key), label(label) {}
};

// ──────────────────────────────────────────────────────────────
// Typed leaves — what owners actually declare. Typed defaults,
// typed Get/Set; the string key never leaks into calling code.
// ──────────────────────────────────────────────────────────────

struct IntSetting : Setting
{
    int32_t def;

    IntSetting(const char* key, const char* label, int32_t def)
        : Setting(key, label), def(def) {}

    int32_t Get() const;         // NVS value, or `def` when absent/unregistered
    bool Set(int32_t v);         // writes NVS (commit via Save(), as today)

    void WriteJson(JsonWriter& json) const override;   // type:"int", value:Get()
    bool ApplyFromString(const char* value) override;  // Set(atoi(value))
};

struct BoolSetting : Setting
{
    bool def;

    BoolSetting(const char* key, const char* label, bool def)
        : Setting(key, label), def(def) {}

    bool Get() const;
    bool Set(bool v);

    void WriteJson(JsonWriter& json) const override;
    bool ApplyFromString(const char* value) override;
};

struct StringSetting : Setting
{
    const char* def;

    StringSetting(const char* key, const char* label, const char* def)
        : Setting(key, label), def(def) {}

    bool Get(char* out, size_t maxLen) const;  // copies NVS value or `def`
    bool Set(const char* v);

    void WriteJson(JsonWriter& json) const override;
    bool ApplyFromString(const char* value) override;
};

// ──────────────────────────────────────────────────────────────
// SettingsManager side
// ──────────────────────────────────────────────────────────────

class SettingsManager
{
    static constexpr const char* TAG = "SettingsManager";

    // Same locking story as the command registry: a mutex guards the
    // chain; NVS access goes through the existing handle_ (which the
    // typed Get/Set reach via the stamped `mgr` pointer).
    Setting* head_ = nullptr;

public:
    // Heterogeneous types register through base pointers; an
    // initializer_list lives on the caller's stack — still no heap.
    void Register(std::initializer_list<Setting*> settings)
    {
        // LOCK(mutex_);
        for (Setting* s : settings)
        {
            assert(!s->registered && "setting registered twice");
            // assert(FindLocked(s->key) == nullptr && "duplicate key");

            s->mgr = this;
            s->registered = true;
            s->next = head_;
            head_ = s;
        }
    }

    // The generated settings UI: walk the chain, each entry writes
    // itself. Replaces the SETTINGS_DEFS walk in WriteAllSettings().
    void WriteAllSettings(JsonWriter& json) const
    {
        json.fieldArray("settings");
        for (const Setting* s = head_; s != nullptr; s = s->next)
        {
            json.beginObject();
            s->WriteJson(json);
            json.endObject();
        }
        json.endArray();
    }

    // The frontend setSetting path: find by key, apply.
    bool ApplySetting(const char* key, const char* value);  // walks chain

    // NVS primitives used by the typed leaves (roughly today's
    // getString/setString/getInt/... made key-private):
    // bool ReadInt(const char* key, int32_t& out);
    // bool WriteInt(const char* key, int32_t v);
    // ...
};

// ──────────────────────────────────────────────────────────────
// Owner side — what a manager writes
// ──────────────────────────────────────────────────────────────

class MqttManager
{
public:
    void Init(SettingsManager& settings)
    {
        settings.Register({ &enabled_, &broker_, &port_ });

        // Typed reads, no strings, no parsing:
        if (!enabled_.Get())
            return;

        char broker[96];
        broker_.Get(broker, sizeof(broker));
        int32_t port = port_.Get();
        // connect(broker, port) ...
    }

private:
    // ~40 bytes RAM each (key/label/def pointers + chain + vtable).
    // Delete this manager's folder and its settings vanish from the
    // UI and NVS schema with it.
    inline static BoolSetting   enabled_{ "mqtt.enabled", "MQTT Enabled", false };
    inline static StringSetting broker_ { "mqtt.broker",  "MQTT Broker",  ""    };
    inline static IntSetting    port_   { "mqtt.port",    "MQTT Port",    1883  };
};

// ──────────────────────────────────────────────────────────────
// Cross-cutting settings — the ownership rule
// ──────────────────────────────────────────────────────────────
//
// A manager that doesn't own a setting has no key to read it with —
// that's the point. It asks the owner:
//
//   class SystemManager {           // ← candidate owner, see the .md
//   public:
//       void GetDeviceName(char* out, size_t maxLen) { name_.Get(out, maxLen); }
//   private:
//       inline static StringSetting name_{ "device.name", "Device Name", "Strux" };
//   };
//
//   // NetworkManager: sp.getSystemManager().GetDeviceName(host, sizeof(host));
//
// OPEN (per ideas/settings-refactor.md): does SystemManager also take
// device.pin + CheckAuth + ping/info/reboot? UI grouping: `group`
// field vs key-prefix sort.
