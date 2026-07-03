#pragma once

// ══════════════════════════════════════════════════════════════
// DESIGN SKETCH — not built, not included anywhere.
// Companion to ideas/settings-refactor.md.
//
// Distributed, strongly-typed settings using the registration
// pattern. Third revision (2026-07-03), decisions locked in:
//
//   • Core knows nothing about JSON/UI. Type tag + iteration +
//     checked downcasts; converters live at the edge.
//   • Explicit types: Int32/UInt32/Float/Bool/String. No bare
//     `int`. Float is bit-cast through u32 (NVS has no float).
//     Double deliberately skipped (ESP32 FPU is single-precision);
//     the no-default switches make adding it later loud.
//   • Defaults are const. Managed fields are private/protected.
//   • Get/Set before Register() CRASHES — a forgotten registration
//     is found on the first test run, not shipped as a silent
//     default.
//   • Guards that would corrupt the chain use FATAL() — the
//     planned lib helper (message + file:line + panic backtrace,
//     never compiled out). Sloppiness checks may stay assert().
//     Until lib/Fatal.h exists, read FATAL as ESP_LOGE + abort().
//   • Flash-residency of key/label/string-default is checked at
//     Register() via esp_ptr_in_drom — runtime, boot-deterministic
//     (compile-time is impossible: pointer values don't exist
//     until link time). Yes, it ties us to ESP. We are tied to ESP.
//   • ResetToDefaults() becomes: erase NVS namespace, commit.
//     Defaults resolve at read, so nothing needs writing back.
//
// NOTE: the "switch with no default" safety net needs one line in
// main/CMakeLists.txt to be an error instead of a warning:
//   target_compile_options(${COMPONENT_LIB} PRIVATE -Werror=switch)
// ══════════════════════════════════════════════════════════════

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cassert>
#include <initializer_list>
#include "esp_log.h"
#include "esp_memory_utils.h"   // esp_ptr_in_drom()

class SettingsManager;
class SettingIterator;
struct Int32Setting;
struct UInt32Setting;
struct FloatSetting;
struct BoolSetting;
struct StringSetting;

// Grows rarely. Every switch over it has NO default case, so (with
// -Werror=switch) adding a type here breaks the build at every
// converter that hasn't been updated — exactly what we want.
enum class SettingType : uint8_t { String, Int32, UInt32, Float, Bool };

// One place for the names; usable by Die(), the UI converter, logs.
// The trailing return is NOT a default case — -Wswitch still flags a
// missing enum value; the trailing return only satisfies -Wreturn-type.
constexpr const char* SettingTypeToString(SettingType type)
{
    switch (type)
    {
    case SettingType::String: return "string";
    case SettingType::Int32:  return "int32";
    case SettingType::UInt32: return "uint32";
    case SettingType::Float:  return "float";
    case SettingType::Bool:   return "bool";
    }
    return "?";
}

// ──────────────────────────────────────────────────────────────
// Base entry — the intrusive chain link + schema facts.
// Owners declare the typed leaves below, never this directly.
// ──────────────────────────────────────────────────────────────

struct Setting
{
    const char* const key;    // NVS key — a storage detail, not an API
    const char* const label;  // shown in the generated settings UI
    const SettingType type;

    // Checked downcasts for generic consumers (converters). Each leaf
    // overrides exactly its own pair. Calling the wrong one is a bug →
    // Die(). The settings UI iterates on every getSettings, so a
    // wrong-type conversion cannot hide.
    virtual Int32Setting&        asInt32()        { Die(SettingType::Int32);  }
    virtual const Int32Setting&  asInt32()  const { Die(SettingType::Int32);  }
    virtual UInt32Setting&       asUInt32()       { Die(SettingType::UInt32); }
    virtual const UInt32Setting& asUInt32() const { Die(SettingType::UInt32); }
    virtual FloatSetting&        asFloat()        { Die(SettingType::Float);  }
    virtual const FloatSetting&  asFloat()  const { Die(SettingType::Float);  }
    virtual BoolSetting&         asBool()         { Die(SettingType::Bool);   }
    virtual const BoolSetting&   asBool()   const { Die(SettingType::Bool);   }
    virtual StringSetting&       asString()       { Die(SettingType::String); }
    virtual const StringSetting& asString() const { Die(SettingType::String); }

    // A registered entry is a live chain link; destroying it would leave
    // a dangling pointer in the chain → FATAL (chain-corruption class).
    virtual ~Setting()
    {
        if (registered)
        {
            ESP_LOGE("Setting", "registered setting '%s' destroyed — "
                     "setting tables must live for the whole application", key);
            abort();   // FATAL("...") once lib/Fatal.h exists
        }
    }

protected:
    Setting(const char* key, const char* label, SettingType type)
        : key(key), label(label), type(type) {}

    // Logs what was asked for AND what it actually is:
    //   "setting 'mqtt.port' is uint32, not string"
    [[noreturn]] void Die(SettingType want) const
    {
        ESP_LOGE("Setting", "setting '%s' is %s, not %s",
                 key, SettingTypeToString(type), SettingTypeToString(want));
        abort();   // FATAL("...") once lib/Fatal.h exists
    }

    // Leaves reach NVS through this (stamped by Register()). Leaves'
    // Get/Set call RequireRegistered() first: using a setting that was
    // never registered is a code bug → crash on the first test run.
    SettingsManager& Manager() const
    {
        if (mgr == nullptr)
        {
            ESP_LOGE("Setting", "setting '%s' used before registration", key);
            abort();   // FATAL("...") once lib/Fatal.h exists
        }
        return *mgr;
    }

private:
    friend class SettingsManager;   // links the chain
    friend class SettingIterator;   // walks the chain

    SettingsManager* mgr = nullptr;
    Setting* next = nullptr;
    bool registered = false;
};

// ──────────────────────────────────────────────────────────────
// Typed leaves — what owners declare. Typed const defaults, typed
// Get/Set; the string key never leaks into calling code.
// ──────────────────────────────────────────────────────────────

struct Int32Setting : Setting
{
    const int32_t def;

    Int32Setting(const char* key, const char* label, int32_t def)
        : Setting(key, label, SettingType::Int32), def(def) {}

    int32_t Get() const;   // NVS value via Manager(), or `def` when absent
    bool Set(int32_t v);

    Int32Setting&       asInt32()       override { return *this; }
    const Int32Setting& asInt32() const override { return *this; }
};

struct UInt32Setting : Setting
{
    const uint32_t def;

    UInt32Setting(const char* key, const char* label, uint32_t def)
        : Setting(key, label, SettingType::UInt32), def(def) {}

    uint32_t Get() const;
    bool Set(uint32_t v);

    UInt32Setting&       asUInt32()       override { return *this; }
    const UInt32Setting& asUInt32() const override { return *this; }
};

struct FloatSetting : Setting
{
    const float def;

    FloatSetting(const char* key, const char* label, float def)
        : Setting(key, label, SettingType::Float), def(def) {}

    // NVS has no float type — stored bit-cast through u32 (memcpy).
    float Get() const;
    bool Set(float v);

    FloatSetting&       asFloat()       override { return *this; }
    const FloatSetting& asFloat() const override { return *this; }
};

struct BoolSetting : Setting
{
    const bool def;

    BoolSetting(const char* key, const char* label, bool def)
        : Setting(key, label, SettingType::Bool), def(def) {}

    bool Get() const;
    bool Set(bool v);

    BoolSetting&       asBool()       override { return *this; }
    const BoolSetting& asBool() const override { return *this; }
};

struct StringSetting : Setting
{
    const char* const def;   // must be a string literal (checked at Register)

    StringSetting(const char* key, const char* label, const char* def)
        : Setting(key, label, SettingType::String), def(def) {}

    bool Get(char* out, size_t maxLen) const;  // copies NVS value or `def`
    bool Set(const char* v);

    StringSetting&       asString()       override { return *this; }
    const StringSetting& asString() const override { return *this; }
};

// ──────────────────────────────────────────────────────────────
// Iteration — the ONLY public way to walk the chain, which is what
// lets `next` be private. Minimal forward iterator, enough for
// range-for; no heap, holds one pointer.
// ──────────────────────────────────────────────────────────────

class SettingIterator
{
    Setting* cur_;

public:
    explicit SettingIterator(Setting* s) : cur_(s) {}

    Setting& operator*() const { return *cur_; }
    SettingIterator& operator++() { cur_ = cur_->next; return *this; }
    bool operator!=(const SettingIterator& o) const { return cur_ != o.cur_; }
};

// ──────────────────────────────────────────────────────────────
// SettingsManager side — schema registry + NVS storage. NOTHING
// about JSON, UI, or any presentation format lives here.
// ──────────────────────────────────────────────────────────────

class SettingsManager
{
    static constexpr const char* TAG = "SettingsManager";

    // Mutex mutex_;            // guards the chain, same as the command registry
    Setting* head_ = nullptr;

public:
    // Heterogeneous leaves register through base pointers; an
    // initializer_list lives on the caller's stack — still no heap.
    //
    // SettingsManager is the NVS link, so registration enforces NVS
    // rules — boot-deterministically, like every guard in this pattern.
    void Register(std::initializer_list<Setting*> settings)
    {
        // LOCK(mutex_);
        for (Setting* s : settings)
        {
            // Chain-corruption class → unconditional (FATAL once it exists):
            if (s->registered)
            {
                ESP_LOGE(TAG, "setting '%s' registered twice", s->key);
                abort();   // re-linking would cycle the chain → UI hangs
            }

            // Sloppiness class → assert is fine:
            assert(strlen(s->key) < 15 && "NVS keys are max 15 chars"); // NVS_KEY_NAME_MAX_SIZE
            assert(esp_ptr_in_drom(s->key)   && "key must be a string literal");
            assert(esp_ptr_in_drom(s->label) && "label must be a string literal");
            if (s->type == SettingType::String)
                assert(esp_ptr_in_drom(s->asString().def) && "default must be a string literal");
            // assert(FindLocked(s->key) == nullptr && "duplicate key");

            s->mgr = this;
            s->registered = true;
            s->next = head_;
            head_ = s;
        }
    }

    // begin() takes the lock only to read head_; the links behind it
    // are write-once and the entries immortal, so the walk itself needs
    // no lock (same reasoning as dispatching command handlers outside
    // the lock).
    //
    //   for (Setting& s : settingsManager) { ... }
    SettingIterator begin() { /* LOCK(mutex_); */ return SettingIterator(head_); }
    SettingIterator end()   { return SettingIterator(nullptr); }

    // Reset: erase the NVS namespace and commit. That's ALL — defaults
    // resolve at read, so nothing needs to be written back. (Today's
    // ResetToDefaults rewrites every default; that code dies.)
    // bool ResetToDefaults();

    // NVS primitives used by the typed leaves via Manager():
    // bool ReadU32(const char* key, uint32_t& out);   // also backs Float (bitcast) and Bool
    // bool WriteU32(const char* key, uint32_t v);
    // bool ReadI32(const char* key, int32_t& out);
    // ...
    // bool Save();   // commit, unchanged from today
};

// ──────────────────────────────────────────────────────────────
// A converter — lives at the EDGE (e.g. inside the getSettings
// command handler), not in SettingsManager. Whoever wants YAML or
// an MQTT dump writes their own ten-line walk just like it.
// ──────────────────────────────────────────────────────────────
//
//  for (const Setting& s : settings)
//  {
//      json.beginObject();
//      json.field("key", s.key);
//      json.field("label", s.label);
//      json.field("type", SettingTypeToString(s.type));
//
//      switch (s.type)   // NO default → compile error on new types
//      {
//      case SettingType::Int32:  json.field("value", s.asInt32().Get());  break;
//      case SettingType::UInt32: json.field("value", s.asUInt32().Get()); break;
//      case SettingType::Float:  json.field("value", s.asFloat().Get());  break;
//      case SettingType::Bool:   json.field("value", s.asBool().Get());   break;
//      case SettingType::String:
//      {
//          char buf[128] = {};
//          s.asString().Get(buf, sizeof(buf));
//          json.field("value", buf);
//          break;
//      }
//      }
//      json.endObject();
//  }
//
// The setSetting path is the same shape: find the entry by key while
// walking, switch on type, parse the string, call the typed Set().

// ──────────────────────────────────────────────────────────────
// Owner side — what a manager writes (unchanged, still elegant)
// ──────────────────────────────────────────────────────────────

class MqttManager
{
public:
    void Init(SettingsManager& settings)
    {
        settings.Register({ &enabled_, &broker_, &port_ });

        // Typed reads, no strings, no parsing. Get() on a setting that
        // was forgotten in the Register list above CRASHES with the key
        // name — found on the first test run.
        if (!enabled_.Get())
            return;

        char broker[96];
        broker_.Get(broker, sizeof(broker));
        uint32_t port = port_.Get();
        // connect(broker, port) ...
    }

private:
    // Delete this manager's folder and its settings vanish from the
    // UI and NVS schema with it.
    inline static BoolSetting   enabled_{ "mqtt.enabled", "MQTT Enabled", false };
    inline static StringSetting broker_ { "mqtt.broker",  "MQTT Broker",  ""    };
    inline static UInt32Setting port_   { "mqtt.port",    "MQTT Port",    1883  };
};

// ──────────────────────────────────────────────────────────────
// Rejected / deferred (see ideas/settings-refactor.md for more)
// ──────────────────────────────────────────────────────────────
// - WriteJson/ApplyFromString virtuals on Setting: leaks one
//   presentation format into the schema core.
// - Descriptor-as-key (settingsManager.GetString(brokerUrl, ...)):
//   reintroduces type mistakes at every call site.
// - Bare getInt(key)/getString(key) only: can't generate the UI.
// - Public next/registered/mgr fields: encapsulation is now the
//   compiler's job, not a comment's.
// - Silent default when unregistered: would ship misconfigured
//   devices; crashing finds the forgotten Register() in testing.
// - DoubleSetting: ESP32 FPU is single-precision; add when needed —
//   the no-default switches make the addition loud.
// - Validation (min/max, URL shape): a valid-but-wrong value passes
//   any validator anyway; this is "bad config", not the schema's job.
// - onChange hooks: the one real use case is hot-applying UI changes
//   without reboot (owner read the value in Init). Today's save→reboot
//   UX is fine; nothing in this design blocks adding hooks later.
//
// NEXT UP (separate feature): lib FATAL(fmt, ...) helper —
// unconditional, logs message + __FILE__:__LINE__, then aborts into
// the panic backtrace. The abort() calls above become FATAL(...).
//
// STILL OPEN: SystemManager for device.name/device.pin (and maybe
// CheckAuth + ping/info/reboot); UI grouping (group field vs key-
// prefix sort).
