#pragma once

// ══════════════════════════════════════════════════════════════
// MINIMAL EXAMPLE — MqttManager registration reworked onto the
// intrusive static-entry pattern from CommandManager. Not built;
// sketches only the plumbing that changes.
//
// Today MqttManager has two std::function hooks:
//   RegisterCommand(name, std::function)   — fixed array of 8
//   RegisterDiscovery(std::function<void()>) — fixed array of 8
// Both go away. Same recipe as CommandEntry: entries are chain
// links, owners hold them as inline static arrays, a trampoline
// erases the owner type at compile time. No heap, no size cap,
// misuse fails at boot.
// ══════════════════════════════════════════════════════════════

#include <type_traits>

// ──────────────────────────────────────────────────────────────
// [MqttManager/MqttEntries.h] — the two entry types
// ──────────────────────────────────────────────────────────────

// Inbound command on {baseTopic}/set/{name}. Payload is raw bytes
// (MQTT data is not zero-terminated), hence data+len instead of
// the WebSocket registry's json string.
struct MqttCommandEntry
{
    const char* name;
    void (*handler)(void* ctx, const char* data, int dataLen);

    // Stamped by MqttManager::Register() — owners never touch these.
    void* ctx = nullptr;
    MqttCommandEntry* next = nullptr;
    bool registered = false;
    // ~MqttCommandEntry: FATAL if a registered entry dies (as CommandEntry)
};

// Discovery hook, fired on every MQTT connect. No name — it's just
// "call me when it's time to publish discovery configs".
struct MqttDiscoveryEntry
{
    void (*handler)(void* ctx);

    void* ctx = nullptr;
    MqttDiscoveryEntry* next = nullptr;
    bool registered = false;
    // ~MqttDiscoveryEntry: FATAL if a registered entry dies
};

// ──────────────────────────────────────────────────────────────
// Trampolines — one per signature, same trick as InvokeCommand:
// class deduced from the method pointer, const methods supported,
// free/static functions fall through the if constexpr.
// ──────────────────────────────────────────────────────────────

template <typename T> struct MqttCommandOwner;
template <typename C> struct MqttCommandOwner<void (C::*)(const char*, int)>       { using type = C; };
template <typename C> struct MqttCommandOwner<void (C::*)(const char*, int) const> { using type = const C; };

template <auto Handler>
void InvokeMqttCommand(void* ctx, const char* data, int dataLen)
{
    if constexpr (std::is_member_function_pointer_v<decltype(Handler)>)
    {
        using C = typename MqttCommandOwner<decltype(Handler)>::type;
        (static_cast<C*>(ctx)->*Handler)(data, dataLen);
    }
    else
    {
        Handler(data, dataLen);
    }
}

template <typename T> struct MqttDiscoveryOwner;
template <typename C> struct MqttDiscoveryOwner<void (C::*)()>       { using type = C; };
template <typename C> struct MqttDiscoveryOwner<void (C::*)() const> { using type = const C; };

template <auto Handler>
void InvokeMqttDiscovery(void* ctx)
{
    if constexpr (std::is_member_function_pointer_v<decltype(Handler)>)
    {
        using C = typename MqttDiscoveryOwner<decltype(Handler)>::type;
        (static_cast<C*>(ctx)->*Handler)();
    }
    else
    {
        Handler();
    }
}

// ──────────────────────────────────────────────────────────────
// [MqttManager.h] — the registry side (sketch)
// ──────────────────────────────────────────────────────────────

class MqttManagerSketch
{
public:
    // Same shape as CommandManager::Register — walks the array,
    // stamps ctx, links into the chain under the mutex. FATAL on
    // re-registration; usable from any Init() regardless of whether
    // MQTT is enabled or connected.
    template <int N> void Register(void* ctx, MqttCommandEntry (&entries)[N]);

    // Ditto — plus: if already connected, fire the new entries
    // immediately so late registrations don't miss the initial
    // discovery window (preserves today's behavior).
    template <int N> void Register(void* ctx, MqttDiscoveryEntry (&entries)[N]);

private:
    MqttCommandEntry*   cmdHead_  = nullptr;
    MqttDiscoveryEntry* discHead_ = nullptr;
    // RecursiveMutex guards the links; handlers run OUTSIDE the lock
    // (entries are immortal), so a discovery handler may Publish()
    // or even Register() more entries without deadlock.

    // Dispatch — replaces today's for-loop over cmdHandlers_[8]:
    //
    //   void HandleCommand(topic, topicLen, data, dataLen)
    //   {
    //       ... extract cmd name after "{base}/set/" as today ...
    //       MqttCommandEntry* e = FindCommand(cmd);   // under lock
    //       if (e) e->handler(e->ctx, data, dataLen); // outside lock
    //       else   ESP_LOGW(TAG, "Unknown command: %s", cmd);
    //   }
    //
    // On MQTT_EVENT_CONNECTED — replaces the callback array loop:
    //
    //   for (e = discHead_; e; e = e->next)   // snapshot under lock
    //       e->handler(e->ctx);               // called outside lock
};

// ──────────────────────────────────────────────────────────────
// [HomeAssistantManager] — the owner side, after the rework
// ──────────────────────────────────────────────────────────────

class HomeAssistantManagerSketch
{
public:
    void Init(/* ServiceProvider& sp */)
    {
        // sp.getMqttManager().Register(this, mqttCommands_);
        // sp.getMqttManager().Register(this, discovery_);
        //
        // That's it — the lambdas from today's Init() become the
        // ordinary members below.
    }

private:
    // was: mqtt.RegisterCommand("led", [this](data, len){ ... });
    void Mqtt_SetLed(const char* data, int dataLen)
    {
        // bool on = (dataLen >= 2 && strncmp(data, "ON", 2) == 0);
        // serviceProvider_.getDeviceManager().getLed().Set(on);
        // PublishLedState();
    }

    // was: mqtt.RegisterDiscovery([this](){ ... });
    void Mqtt_Discovery()
    {
        // auto& mqtt = serviceProvider_.getMqttManager();
        // mqtt.PublishEntityDiscovery("light", "led", ...); // unchanged
        // PublishLedState();
    }

    inline static MqttCommandEntry mqttCommands_[] = {
        { "led", &InvokeMqttCommand<&HomeAssistantManagerSketch::Mqtt_SetLed> },
    };

    inline static MqttDiscoveryEntry discovery_[] = {
        { &InvokeMqttDiscovery<&HomeAssistantManagerSketch::Mqtt_Discovery> },
    };
};

// ──────────────────────────────────────────────────────────────
// Open points (NOT decided by this sketch)
// ──────────────────────────────────────────────────────────────
//
// 1. Dogfooding: MqttManager hard-codes a "reboot" command inside
//    HandleCommand() and diagnostic sensors + reboot button inside
//    PublishDiscovery(). It could register those through its own
//    chains (inline static entries in MqttManager itself), leaving
//    exactly one mechanism — or keep them built-in. Separate call.
//
// 2. PublishEntityDiscovery(component, objectId, std::function
//    <void(JsonWriter&)>) is untouched here. It's a synchronous
//    helper called from inside a discovery handler, so its capture
//    lives on the stack — no heap lifetime issue, but it is the
//    last std::function in the manager. Could later become a plain
//    fn-ptr taking (JsonWriter&, void* user) if we want it gone.
//
// 3. Two entry types + two trampolines is copy-paste of the
//    CommandEntry machinery. If a third registry ever shows up we
//    could fold the chain/Register/FATAL boilerplate into a shared
//    lib template; for now the duplication is small and readable.
