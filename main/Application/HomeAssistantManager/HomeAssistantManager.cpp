#include "HomeAssistantManager.h"
#include "MqttManager/MqttManager.h"
#include "Board.h"
#include "JsonWriter.h"
#include "esp_log.h"
#include <cstring>
#include <cstdio>

HomeAssistantManager::HomeAssistantManager(ServiceProvider &ctx)
    : serviceProvider_(ctx)
{
}

void HomeAssistantManager::Init()
{
    auto init = initState_.TryBeginInit();
    if (!init)
    {
        ESP_LOGW(TAG, "Already initialized or initializing");
        return;
    }

    auto &mqtt = serviceProvider_.getMqttManager();

    // ── LED light entity ─────────────────────────────────────

    mqtt.RegisterCommand("led", [this](const char *data, int len)
    {
        bool on = (len >= 2 && strncmp(data, "ON", 2) == 0);
        serviceProvider_.getBoard().GetLed().Set(on);
        PublishLedState();
    });

    mqtt.RegisterDiscovery([this]()
    {
        auto &mqtt = serviceProvider_.getMqttManager();

        mqtt.PublishEntityDiscovery("light", "led", [&mqtt](JsonWriter &json)
        {
            json.field("name", "LED");

            char topic[128];
            snprintf(topic, sizeof(topic), "%s/set/led", mqtt.GetBaseTopic());
            json.field("cmd_t", topic);

            snprintf(topic, sizeof(topic), "%s/led/state", mqtt.GetBaseTopic());
            json.field("stat_t", topic);

            json.field("pl_on", "ON");
            json.field("pl_off", "OFF");
        });

        PublishLedState();
    });

    // ── Add more HA entities here ────────────────────────────
    // Projects wire the board's hardware (getBoard()) to HA entities
    // using the same RegisterCommand / RegisterDiscovery pattern.

    init.SetReady();
    ESP_LOGI(TAG, "Initialized");
}

void HomeAssistantManager::PublishLedState()
{
    bool on = serviceProvider_.getBoard().GetLed().IsOn();
    serviceProvider_.getMqttManager().Publish("led/state", on ? "ON" : "OFF", true);
}
