#pragma once

#include "ServiceProvider.h"
#include "InitState.h"
#include "TypedSettings.h"
#include "Timer.h"
#include "mqtt_client.h"
#include <functional>

class JsonWriter;

class MqttManager
{
    static constexpr const char *TAG = "MqttManager";

public:
    explicit MqttManager(ServiceProvider &serviceProvider);

    MqttManager(const MqttManager &) = delete;
    MqttManager &operator=(const MqttManager &) = delete;
    MqttManager(MqttManager &&) = delete;
    MqttManager &operator=(MqttManager &&) = delete;

    void Init();

    bool IsConnected() const { return connected_; }
    const char *GetBaseTopic() const { return baseTopic_; }
    const char *GetDeviceId() const { return deviceId_; }

    /// Publish a message to {baseTopic}/{subtopic}.
    void Publish(const char *subtopic, const char *payload, bool retain = false);

    /// Register a handler for MQTT commands on {baseTopic}/set/{name}.
    using MqttCommandHandler = std::function<void(const char *data, int dataLen)>;
    void RegisterCommand(const char *name, MqttCommandHandler handler);

    /// Register a callback invoked during HA discovery (on every MQTT connect).
    /// Use PublishEntityDiscovery() inside the callback to publish entity configs.
    void RegisterDiscovery(std::function<void()> callback);

    /// Publish a Home Assistant discovery config for an entity.
    /// The writeFields callback writes entity-specific JSON fields
    /// (name, command_topic, state_topic, etc.). The device block,
    /// unique_id, and availability are handled automatically.
    void PublishEntityDiscovery(const char *component, const char *objectId,
                                std::function<void(JsonWriter &)> writeFields);

private:
    ServiceProvider &serviceProvider_;
    InitState initState_;

    esp_mqtt_client_handle_t client_ = nullptr;
    volatile bool connected_ = false;
    bool enabled_ = false;

    // ── Settings (registered with SettingsManager in Init) ──
    inline static BoolSetting   mqttEnabled_{ "mqtt.enabled", "MQTT Enabled",  false   };
    inline static StringSetting mqttBroker_ { "mqtt.broker",  "MQTT Broker",   ""      };
    inline static Int32Setting  mqttPort_   { "mqtt.port",    "MQTT Port",     1883    };
    inline static StringSetting mqttUser_   { "mqtt.user",    "MQTT User",     ""      };
    inline static StringSetting mqttPass_   { "mqtt.pass",    "MQTT Password", ""      };
    inline static StringSetting mqttPrefix_ { "mqtt.prefix",  "MQTT Prefix",   "strux" };

    char deviceId_[16] = {};   // Last 4 bytes of MAC, hex (e.g. "a1b2c3d4")
    char baseTopic_[96] = {};  // "{prefix}/{deviceId}"

    Timer publishTimer_;

    // Extensibility
    static constexpr int MAX_COMMAND_HANDLERS = 8;
    static constexpr int MAX_DISCOVERY_CALLBACKS = 8;

    struct CommandRegistration
    {
        char name[32];
        MqttCommandHandler handler;
    };

    CommandRegistration cmdHandlers_[MAX_COMMAND_HANDLERS] = {};
    int cmdHandlerCount_ = 0;

    std::function<void()> discoveryCallbacks_[MAX_DISCOVERY_CALLBACKS] = {};
    int discoveryCallbackCount_ = 0;

    void BuildDeviceId();
    void StartClient();
    void PublishDiscovery();
    void PublishState();
    void Subscribe();
    void HandleCommand(const char *topic, int topicLen, const char *data, int dataLen);

    static void EventHandler(void *handlerArgs, esp_event_base_t base,
                             int32_t eventId, void *eventData);
    void OnEvent(esp_mqtt_event_handle_t event);
};
