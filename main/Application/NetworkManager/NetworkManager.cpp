#include "NetworkManager.h"
#include "SettingsManager.h"
#include "SystemManager.h"
#include "CommandManager.h"
#include "JsonWriter.h"
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "mdns.h"

NetworkManager::NetworkManager(ServiceProvider& serviceProvider)
    : serviceProvider_(serviceProvider)
{
}

void NetworkManager::Init()
{
    auto initAttempt = initState.TryBeginInit();
    if (!initAttempt)
    {
        ESP_LOGW(TAG, "Already initialized or initializing");
        return;
    }

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(esp_netif_init());

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
    {
        ESP_ERROR_CHECK(err);
    }

    // Reduce noisy WiFi/LWIP init logs
    esp_log_level_set("wifi", ESP_LOG_WARN);
    esp_log_level_set("wifi_init", ESP_LOG_WARN);
    esp_log_level_set("phy_init", ESP_LOG_WARN);
    esp_log_level_set("esp_netif_handlers", ESP_LOG_WARN);

    wifi_interface_.SetEventHandler([this](const NetworkEvent& e) { HandleNetworkEvent(e); });
    wifi_interface_.Init();

    // Set hostname from the device name so it shows in the router
    char deviceName[33] = {};
    serviceProvider_.getSystemManager().GetDeviceName(deviceName, sizeof(deviceName));
    wifi_interface_.SetHostname(deviceName);

    // mDNS — <deviceName>.local
    ESP_ERROR_CHECK(mdns_init());
    mdns_hostname_set(deviceName);
    mdns_instance_name_set(deviceName);
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);

    // Setup connect timeout timer
    connectTimer_.Init("sta_timeout", pdMS_TO_TICKS(StaConnectTimeoutMs), false);
    connectTimer_.SetHandler([this]() {
        if (!staConnected_)
        {
            staRetryCount_++;
            ESP_LOGW(TAG, "STA connect timeout (attempt %d/%d)", staRetryCount_.load(), MaxStaRetries);

            if (staRetryCount_ >= MaxStaRetries)
            {
                FallbackToAP();
            }
            else
            {
                AttemptStaConnect();
            }
        }
    });

    serviceProvider_.getCommandManager().Register(this, commands_);
    serviceProvider_.getSettingsManager().Register({ &wifiSsid_, &wifiPassword_ });

    initAttempt.SetReady();
    ESP_LOGI(TAG, "Initialized");

    // Load WiFi credentials from settings and try to connect
    wifiSsid_.Get(staSsid_, sizeof(staSsid_));
    wifiPassword_.Get(staPassword_, sizeof(staPassword_));

    if (staSsid_[0] != '\0')
    {
        AttemptStaConnect();
    }
    else
    {
        ESP_LOGI(TAG, "No WiFi SSID configured, starting AP");
        FallbackToAP();
    }
}

WiFiInterface& NetworkManager::wifi()
{
    WAIT_FOR_READY(initState);
    return wifi_interface_;
}

const WiFiInterface& NetworkManager::wifi() const
{
    WAIT_FOR_READY(initState);
    return wifi_interface_;
}

void NetworkManager::AttemptStaConnect()
{
    if (staSsid_[0] == '\0')
    {
        ESP_LOGW(TAG, "No STA credentials configured, starting AP");
        FallbackToAP();
        return;
    }

    ESP_LOGI(TAG, "Attempting STA connection to '%s' (attempt %d/%d)",
             staSsid_, staRetryCount_.load() + 1, MaxStaRetries);

    wifi_interface_.Stop();
    staConnected_ = false;
    wifi_interface_.ConnectSta(staSsid_, staPassword_);
    connectTimer_.Start();
}

void NetworkManager::FallbackToAP()
{
    connectTimer_.Stop();
    wifi_interface_.Stop();

    ESP_LOGW(TAG, "Falling back to AP mode: '%s'", DefaultApSsid);
    wifi_interface_.StartAP(DefaultApSsid, DefaultApPassword);
}

void NetworkManager::HandleNetworkEvent(const NetworkEvent& event)
{
    switch (event.type)
    {
    case NetworkEventType::LinkUp:
        if (!wifi_interface_.IsAP())
        {
            ESP_LOGI(TAG, "STA connected to AP");
        }
        else
        {
            ESP_LOGI(TAG, "AP started");
        }
        break;

    case NetworkEventType::LinkDown:
        if (!wifi_interface_.IsAP())
        {
            ESP_LOGW(TAG, "STA disconnected");

            if (staConnected_)
            {
                // Was connected, lost connection — try to reconnect
                staConnected_ = false;
                staRetryCount_ = 0;
                ESP_LOGI(TAG, "Lost connection, attempting reconnect");
                esp_wifi_connect();
                connectTimer_.Start();
            }
            // If not yet connected, the timeout timer handles retries
        }
        break;

    case NetworkEventType::Ipv4Acquired:
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event.status.ipv4.ip));
        connectTimer_.Stop();
        staConnected_ = true;
        staRetryCount_ = 0;
        break;

    case NetworkEventType::Ipv4Lost:
        ESP_LOGW(TAG, "Lost IP");
        staConnected_ = false;
        break;
    }
}

// ──────────────────────────────────────────────────────────────
// WebSocket commands
// ──────────────────────────────────────────────────────────────

void NetworkManager::Cmd_WifiScan(void* ctx, const char* json, JsonWriter& resp)
{
    auto* self = static_cast<NetworkManager*>(ctx);

    WiFiInterface::ScanResult results[20] = {};
    int count = self->wifi().Scan(results, 20);

    resp.field("ok", true);
    resp.fieldArray("networks");

    for (int i = 0; i < count; i++)
    {
        resp.beginObject();
        resp.field("ssid", results[i].ssid);
        resp.field("rssi", static_cast<int32_t>(results[i].rssi));
        resp.field("channel", static_cast<int32_t>(results[i].channel));
        resp.field("secure", results[i].secure);
        resp.endObject();
    }

    resp.endArray();
}
