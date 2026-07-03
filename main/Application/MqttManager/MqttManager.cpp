#include "MqttManager.h"
#include "SettingsManager/SettingsManager.h"
#include "SystemManager/SystemManager.h"
#include "JsonWriter.h"
#include "BufferStream.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_app_desc.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cstring>
#include <cstdio>

// ──────────────────────────────────────────────────────────────
// Construction & Init
// ──────────────────────────────────────────────────────────────

MqttManager::MqttManager(ServiceProvider &ctx)
    : serviceProvider_(ctx)
{
}

void MqttManager::Init()
{
    auto init = initState_.TryBeginInit();
    if (!init)
    {
        ESP_LOGW(TAG, "Already initialized or initializing");
        return;
    }

    // Registered before the enabled check: the settings must exist in the
    // UI even when MQTT is off, or it could never be enabled again.
    serviceProvider_.getSettingsManager().Register({
        &mqttEnabled_, &mqttBroker_, &mqttPort_, &mqttUser_, &mqttPass_, &mqttPrefix_ });

    enabled_ = mqttEnabled_.Get();

    if (!enabled_)
    {
        ESP_LOGI(TAG, "MQTT disabled");
        init.SetReady();
        return;
    }

    BuildDeviceId();

    char prefix[32] = {};
    mqttPrefix_.Get(prefix, sizeof(prefix));
    if (prefix[0] == '\0')
        snprintf(prefix, sizeof(prefix), "strux");
    snprintf(baseTopic_, sizeof(baseTopic_), "%s/%s", prefix, deviceId_);

    StartClient();

    publishTimer_.Init("mqtt_pub", pdMS_TO_TICKS(30000), true);
    publishTimer_.SetHandler([this]()
    {
        if (connected_)
            PublishState();
    });

    init.SetReady();
    ESP_LOGI(TAG, "Initialized (base topic: %s)", baseTopic_);
}

void MqttManager::BuildDeviceId()
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(deviceId_, sizeof(deviceId_), "%02x%02x%02x%02x",
             mac[2], mac[3], mac[4], mac[5]);
}

// ──────────────────────────────────────────────────────────────
// Registration (for other managers to add entities & commands)
// ──────────────────────────────────────────────────────────────

void MqttManager::RegisterCommand(const char *name, MqttCommandHandler handler)
{
    if (cmdHandlerCount_ >= MAX_COMMAND_HANDLERS)
    {
        ESP_LOGE(TAG, "Command handler table full");
        return;
    }
    auto &entry = cmdHandlers_[cmdHandlerCount_++];
    strncpy(entry.name, name, sizeof(entry.name) - 1);
    entry.handler = handler;
}

void MqttManager::RegisterDiscovery(std::function<void()> callback)
{
    if (discoveryCallbackCount_ >= MAX_DISCOVERY_CALLBACKS)
    {
        ESP_LOGE(TAG, "Discovery callback table full");
        return;
    }
    discoveryCallbacks_[discoveryCallbackCount_++] = callback;

    // If already connected, publish immediately so late registrations
    // don't miss the initial discovery window.
    if (connected_)
        callback();
}

// ──────────────────────────────────────────────────────────────
// MQTT Client
// ──────────────────────────────────────────────────────────────

void MqttManager::StartClient()
{
    char broker[64] = {};
    char user[32] = {};
    char pass[64] = {};
    mqttBroker_.Get(broker, sizeof(broker));
    mqttUser_.Get(user, sizeof(user));
    mqttPass_.Get(pass, sizeof(pass));
    int32_t port = mqttPort_.Get();

    if (broker[0] == '\0')
    {
        ESP_LOGW(TAG, "No broker configured, MQTT will not connect");
        return;
    }

    char uri[128];
    snprintf(uri, sizeof(uri), "mqtt://%s:%ld", broker, static_cast<long>(port));

    char lwtTopic[128];
    snprintf(lwtTopic, sizeof(lwtTopic), "%s/status", baseTopic_);

    esp_mqtt_client_config_t cfg = {};
    cfg.broker.address.uri = uri;
    cfg.credentials.username = user[0] ? user : nullptr;
    cfg.credentials.authentication.password = pass[0] ? pass : nullptr;
    cfg.session.last_will.topic = lwtTopic;
    cfg.session.last_will.msg = "offline";
    cfg.session.last_will.msg_len = 7;
    cfg.session.last_will.qos = 1;
    cfg.session.last_will.retain = 1;

    client_ = esp_mqtt_client_init(&cfg);
    esp_mqtt_client_register_event(client_, static_cast<esp_mqtt_event_id_t>(ESP_EVENT_ANY_ID),
                                   EventHandler, this);
    esp_mqtt_client_start(client_);

    ESP_LOGI(TAG, "Connecting to %s", uri);
}

// ──────────────────────────────────────────────────────────────
// Event handling
// ──────────────────────────────────────────────────────────────

void MqttManager::EventHandler(void *args, esp_event_base_t base,
                                int32_t eventId, void *eventData)
{
    auto *self = static_cast<MqttManager *>(args);
    self->OnEvent(static_cast<esp_mqtt_event_handle_t>(eventData));
}

void MqttManager::OnEvent(esp_mqtt_event_handle_t event)
{
    switch (event->event_id)
    {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "Connected to broker");
        connected_ = true;
        PublishDiscovery();
        {
            char topic[128];
            snprintf(topic, sizeof(topic), "%s/status", baseTopic_);
            esp_mqtt_client_publish(client_, topic, "online", 6, 1, 1);
        }
        Subscribe();
        PublishState();
        publishTimer_.Start();
        break;

    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "Disconnected from broker");
        connected_ = false;
        publishTimer_.Stop();
        break;

    case MQTT_EVENT_DATA:
        HandleCommand(event->topic, event->topic_len, event->data, event->data_len);
        break;

    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "MQTT error");
        break;

    default:
        break;
    }
}

void MqttManager::Subscribe()
{
    char topic[128];
    snprintf(topic, sizeof(topic), "%s/set/#", baseTopic_);
    esp_mqtt_client_subscribe(client_, topic, 1);
    ESP_LOGI(TAG, "Subscribed to %s", topic);
}

void MqttManager::HandleCommand(const char *topic, int topicLen,
                                 const char *data, int dataLen)
{
    char setPrefix[128];
    int prefixLen = snprintf(setPrefix, sizeof(setPrefix), "%s/set/", baseTopic_);

    if (topicLen <= prefixLen)
        return;

    // Extract the command name after "{baseTopic}/set/"
    char cmd[32] = {};
    int cmdLen = topicLen - prefixLen;
    if (cmdLen >= static_cast<int>(sizeof(cmd)))
        cmdLen = sizeof(cmd) - 1;
    memcpy(cmd, topic + prefixLen, cmdLen);

    ESP_LOGI(TAG, "Command: %s", cmd);

    // Built-in commands
    if (strcmp(cmd, "reboot") == 0)
    {
        ESP_LOGI(TAG, "Reboot requested via MQTT");
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
        return;
    }

    // Registered command handlers
    for (int i = 0; i < cmdHandlerCount_; i++)
    {
        if (strcmp(cmd, cmdHandlers_[i].name) == 0)
        {
            cmdHandlers_[i].handler(data, dataLen);
            return;
        }
    }

    ESP_LOGW(TAG, "Unknown command: %s", cmd);
}

// ──────────────────────────────────────────────────────────────
// Publishing
// ──────────────────────────────────────────────────────────────

void MqttManager::Publish(const char *subtopic, const char *payload, bool retain)
{
    if (!connected_ || !client_)
        return;

    char topic[128];
    snprintf(topic, sizeof(topic), "%s/%s", baseTopic_, subtopic);
    esp_mqtt_client_publish(client_, topic, payload, 0, 0, retain ? 1 : 0);
}

void MqttManager::PublishState()
{
    // IP address
    char ipStr[16] = "0.0.0.0";
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif)
    {
        esp_netif_ip_info_t ipInfo;
        if (esp_netif_get_ip_info(netif, &ipInfo) == ESP_OK)
            esp_ip4addr_ntoa(&ipInfo.ip, ipStr, sizeof(ipStr));
    }

    // WiFi RSSI
    int32_t rssi = 0;
    wifi_ap_record_t apInfo;
    if (esp_wifi_sta_get_ap_info(&apInfo) == ESP_OK)
        rssi = apInfo.rssi;

    // Uptime
    uint32_t uptimeSec = static_cast<uint32_t>(esp_timer_get_time() / 1000000);

    // Free heap
    uint32_t freeHeap = static_cast<uint32_t>(esp_get_free_heap_size());

    char buf[256];
    BufferStream stream(buf, sizeof(buf));
    JsonWriter json(stream);
    json.beginObject();
    json.field("ip", ipStr);
    json.field("rssi", rssi);
    json.field("uptime", uptimeSec);
    json.field("heap", freeHeap);
    json.endObject();

    Publish("state", buf);
}

// ──────────────────────────────────────────────────────────────
// Home Assistant MQTT Discovery
// ──────────────────────────────────────────────────────────────

static void WriteDeviceBlock(JsonWriter &json, const char *deviceId,
                             const char *deviceName, const char *version)
{
    json.fieldObject("dev");

    json.fieldArray("ids");
    char id[32];
    snprintf(id, sizeof(id), "strux_%s", deviceId);
    json.value(id);
    json.endArray();

    json.field("name", deviceName);
    json.field("mf", "Strux");
    json.field("mdl", CONFIG_IDF_TARGET);
    json.field("sw", version);

    json.endObject();
}

void MqttManager::PublishEntityDiscovery(const char *component, const char *objectId,
                                          std::function<void(JsonWriter &)> writeFields)
{
    if (!connected_ || !client_)
        return;

    const esp_app_desc_t *app = esp_app_get_description();

    char deviceName[32] = {};
    serviceProvider_.getSystemManager().GetDeviceName(deviceName, sizeof(deviceName));

    char availTopic[128];
    snprintf(availTopic, sizeof(availTopic), "%s/status", baseTopic_);

    char configTopic[192];
    snprintf(configTopic, sizeof(configTopic),
             "homeassistant/%s/%s/%s/config", component, deviceId_, objectId);

    char uid[64];
    snprintf(uid, sizeof(uid), "strux_%s_%s", deviceId_, objectId);

    char buf[512];
    BufferStream stream(buf, sizeof(buf));
    JsonWriter json(stream);

    json.beginObject();
    json.field("uniq_id", uid);
    json.field("avty_t", availTopic);
    WriteDeviceBlock(json, deviceId_, deviceName, app->version);
    writeFields(json);
    json.endObject();

    esp_mqtt_client_publish(client_, configTopic, buf, 0, 1, 1);
}

void MqttManager::PublishDiscovery()
{
    const esp_app_desc_t *app = esp_app_get_description();

    char deviceName[32] = {};
    serviceProvider_.getSystemManager().GetDeviceName(deviceName, sizeof(deviceName));

    char stateTopic[128];
    snprintf(stateTopic, sizeof(stateTopic), "%s/state", baseTopic_);

    char availTopic[128];
    snprintf(availTopic, sizeof(availTopic), "%s/status", baseTopic_);

    // ── Diagnostic sensors ───────────────────────────────────

    struct SensorDef
    {
        const char *objectId;
        const char *name;
        const char *valueTemplate;
        const char *deviceClass;
        const char *unit;
        const char *icon;
    };

    static const SensorDef sensors[] = {
        {"ip",     "IP Address",  "{{ value_json.ip }}",     nullptr,           nullptr, "mdi:ip-network"},
        {"rssi",   "WiFi Signal", "{{ value_json.rssi }}",   "signal_strength", "dBm",   nullptr},
        {"uptime", "Uptime",      "{{ value_json.uptime }}", "duration",        "s",     nullptr},
        {"heap",   "Free Heap",   "{{ value_json.heap }}",   nullptr,           "B",     "mdi:memory"},
    };

    for (const auto &s : sensors)
    {
        char configTopic[192];
        snprintf(configTopic, sizeof(configTopic),
                 "homeassistant/sensor/%s/%s/config", deviceId_, s.objectId);

        char buf[512];
        BufferStream stream(buf, sizeof(buf));
        JsonWriter json(stream);

        json.beginObject();

        json.field("name", s.name);

        char uid[64];
        snprintf(uid, sizeof(uid), "strux_%s_%s", deviceId_, s.objectId);
        json.field("uniq_id", uid);

        json.field("stat_t", stateTopic);
        json.field("val_tpl", s.valueTemplate);

        if (s.deviceClass)
            json.field("dev_cla", s.deviceClass);
        if (s.unit)
            json.field("unit_of_meas", s.unit);
        if (s.icon)
            json.field("ic", s.icon);

        json.field("ent_cat", "diagnostic");
        json.field("avty_t", availTopic);

        WriteDeviceBlock(json, deviceId_, deviceName, app->version);

        json.endObject();

        esp_mqtt_client_publish(client_, configTopic, buf, 0, 1, 1);
    }

    // ── Reboot button ────────────────────────────────────────

    {
        char configTopic[192];
        snprintf(configTopic, sizeof(configTopic),
                 "homeassistant/button/%s/reboot/config", deviceId_);

        char cmdTopic[128];
        snprintf(cmdTopic, sizeof(cmdTopic), "%s/set/reboot", baseTopic_);

        char buf[512];
        BufferStream stream(buf, sizeof(buf));
        JsonWriter json(stream);

        json.beginObject();

        json.field("name", "Reboot");

        char uid[64];
        snprintf(uid, sizeof(uid), "strux_%s_reboot", deviceId_);
        json.field("uniq_id", uid);

        json.field("cmd_t", cmdTopic);
        json.field("dev_cla", "restart");
        json.field("ent_cat", "config");
        json.field("avty_t", availTopic);

        WriteDeviceBlock(json, deviceId_, deviceName, app->version);

        json.endObject();

        esp_mqtt_client_publish(client_, configTopic, buf, 0, 1, 1);
    }

    // ── Registered discovery callbacks ───────────────────────

    for (int i = 0; i < discoveryCallbackCount_; i++)
        discoveryCallbacks_[i]();

    ESP_LOGI(TAG, "Home Assistant discovery published");
}
