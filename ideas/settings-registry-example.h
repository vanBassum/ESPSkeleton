#pragma once

// ══════════════════════════════════════════════════════════════
// DESIGN SKETCH — not built, not included anywhere.
// Companion to ideas/settings-refactor.md.
//
// Distributed, strongly-typed settings using the registration
// pattern. Revised per Bas's pressure points (2026-07-03):
//
//   • SEPARATION OF CONCERNS: the core knows nothing about JSON or
//     any other presentation. No WriteJson/ApplyFromString in the
//     Setting. Instead: a type tag + iteration + checked downcasts,
//     and converters live at the edge (the getSettings/setSetting
//     command handlers, or whatever wants YAML next).
//   • KEEP IT SIMPLE: Setting = key + label + type + chain link.
//     Typed leaves add a typed default and Get/Set. That's all.
//   • Managed fields are NOT public: next/registered are private
//     (friends: SettingsManager + the iterator); mgr is protected
//     because the typed leaves need it for Get/Set.
//   • Registration validates NVS constraints (key < 15 chars,
//     duplicates) — SettingsManager is the NVS link, so it guards
//     NVS rules, boot-deterministically.
//   • Standard iteration: begin()/end() → for (Setting& s : settings).
//
// NOTE: the "switch with no default" safety net is only a WARNING
// by default (-Wswitch). To make it the compile error we want, add
// to main/CMakeLists.txt:
//   target_compile_options(${COMPONENT_LIB} PRIVATE -Werror=switch)
// ══════════════════════════════════════════════════════════════

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cassert>
#include <initializer_list>
#include "esp_log.h"

class SettingsManager;
class SettingIterator;
struct IntSetting;
struct BoolSetting;
struct StringSetting;

// Grows rarely. Every switch over it has NO default case, so (with
// -Werror=switch) adding a type here breaks the build at every
// converter that hasn't been updated — exactly what we want.
enum class SettingType : uint8_t { String, Int, Bool };

// One place for the names; usable by Die(), the UI converter, logs.
// The trailing return is NOT a default case — -Wswitch still flags a
// missing enum value; the trailing return only satisfies -Wreturn-type.
constexpr const char* SettingTypeToString(SettingType type)
{
    switch (type)
    {
    case SettingType::String: return "string";
    case SettingType::Int:    return "int";
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
    const char* key;         // NVS key — a storage detail, not an API
    const char* label;       // shown in the generated settings UI
    const SettingType type;  // plain member set by the leaf ctor

    // Checked downcasts for generic consumers (converters). Each leaf
    // overrides exactly one. Calling the wrong one is a bug → log +
    // abort. The settings UI iterates on every getSettings, so a
    // wrong-type conversion cannot hide — it dies the first time the
    // settings page is opened.
    virtual IntSetting&    asInt()    { Die(SettingType::Int);    }
    virtual BoolSetting&   asBool()   { Die(SettingType::Bool);   }
    virtual StringSetting& asString() { Die(SettingType::String); }

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
    Setting(const char* key, const char* label, SettingType type)
        : key(key), label(label), type(type) {}

    // Logs what was asked for AND what it actually is:
    //   "setting 'mqtt.port' is int, not string"
    [[noreturn]] void Die(SettingType want) const
    {
        ESP_LOGE("Setting", "setting '%s' is %s, not %s",
                 key, SettingTypeToString(type), SettingTypeToString(want));
        abort();
    }

    // Leaves reach NVS through this (stamped by Register()).
    SettingsManager* mgr = nullptr;

private:
    friend class SettingsManager;   // links the chain
    friend class SettingIterator;   // walks the chain

    Setting* next = nullptr;
    bool registered = false;
};

// ──────────────────────────────────────────────────────────────
// Typed leaves — what owners declare. Typed defaults, typed
// Get/Set; the string key never leaks into calling code.
// ──────────────────────────────────────────────────────────────

struct IntSetting : Setting
{
    int32_t def;

    IntSetting(const char* key, const char* label, int32_t def)
        : Setting(key, label, SettingType::Int), def(def) {}

    int32_t Get() const;   // NVS value via mgr, or `def` when absent
    bool Set(int32_t v);

    IntSetting& asInt() override { return *this; }
};

struct BoolSetting : Setting
{
    bool def;

    BoolSetting(const char* key, const char* label, bool def)
        : Setting(key, label, SettingType::Bool), def(def) {}

    bool Get() const;
    bool Set(bool v);

    BoolSetting& asBool() override { return *this; }
};

struct StringSetting : Setting
{
    const char* def;

    StringSetting(const char* key, const char* label, const char* def)
        : Setting(key, label, SettingType::String), def(def) {}

    bool Get(char* out, size_t maxLen) const;  // copies NVS value or `def`
    bool Set(const char* v);

    StringSetting& asString() override { return *this; }
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
            assert(!s->registered && "setting registered twice");
            assert(strlen(s->key) < 15 && "NVS keys are max 15 chars"); // NVS_KEY_NAME_MAX_SIZE
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

    // NVS primitives used by the typed leaves via the stamped `mgr`
    // pointer (roughly today's getString/getInt/... made key-private):
    // bool ReadInt(const char* key, int32_t& out);
    // bool WriteInt(const char* key, int32_t v);
    // ...
    // bool Save();   // commit, unchanged from today
};

// ──────────────────────────────────────────────────────────────
// A converter — lives at the EDGE (e.g. inside the getSettings
// command handler), not in SettingsManager. Whoever wants YAML or
// an MQTT dump writes their own ten-line walk just like it.
// ──────────────────────────────────────────────────────────────
//
//  for (Setting& s : settings)
//  {
//      json.beginObject();
//      json.field("key", s.key);
//      json.field("label", s.label);
//      json.field("type", SettingTypeToString(s.type));
//
//      switch (s.type)   // NO default → compile error on new types
//      {
//      case SettingType::Int:
//          json.field("value", s.asInt().Get());
//          break;
//      case SettingType::Bool:
//          json.field("value", s.asBool().Get());
//          break;
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

        // Typed reads, no strings, no parsing:
        if (!enabled_.Get())
            return;

        char broker[96];
        broker_.Get(broker, sizeof(broker));
        int32_t port = port_.Get();
        // connect(broker, port) ...
    }

private:
    // Delete this manager's folder and its settings vanish from the
    // UI and NVS schema with it.
    inline static BoolSetting   enabled_{ "mqtt.enabled", "MQTT Enabled", false };
    inline static StringSetting broker_ { "mqtt.broker",  "MQTT Broker",  ""    };
    inline static IntSetting    port_   { "mqtt.port",    "MQTT Port",    1883  };
};

// ──────────────────────────────────────────────────────────────
// Rejected along the way (see ideas/settings-refactor.md for more)
// ──────────────────────────────────────────────────────────────
// - WriteJson/ApplyFromString virtuals on Setting: leaks one
//   presentation format into the schema core; next format explodes it.
// - Descriptor-as-key (settingsManager.GetString(brokerUrl, ...)):
//   reintroduces type mistakes at every call site; typed Get/Set on
//   the leaf makes the wrong call unrepresentable.
// - Bare getInt(key)/getString(key) only: today's system minus the
//   schema — cannot generate the settings UI, which is the point.
// - Public next/registered/mgr fields: "owners never touch these" is
//   now enforced by the compiler, not a comment.
//
// STILL OPEN: SystemManager for device.name/device.pin (and maybe
// CheckAuth + ping/info/reboot); UI grouping (group field vs key-
// prefix sort).
