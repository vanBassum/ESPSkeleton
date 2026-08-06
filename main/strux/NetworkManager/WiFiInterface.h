#pragma once

#include "NetworkInterface.h"
#include "esp_wifi.h"

class WiFiInterface final : public NetworkInterface {
    static constexpr const char* TAG = "WiFiInterface";

public:
    void Init();
    void SetHostname(const char* hostname);

    void ConnectSta(const char* ssid, const char* password);
    void StartAP(const char* ssid, const char* password, uint8_t channel = 1, uint8_t maxConnections = 4);
    void Stop();

    bool IsAP() const { return isAP_; }

    /// Signal strength of the AP this station is associated with, in dBm (negative;
    /// around -50 is strong, -80 is marginal). False in AP mode or while not
    /// associated — there is no number then, and reporting 0 would read as a perfect
    /// signal rather than as "unknown".
    bool GetRssi(int8_t& out) const;

    struct ScanResult {
        char ssid[33];
        int8_t rssi;
        uint8_t channel;
        bool secure;
    };

    /// Scan for WiFi networks. Returns the number of results written.
    int Scan(ScanResult* out, int maxResults);

    const char* getName() const override { return "wifi"; }
    void SetEventHandler(NetworkEventHandler handler) override;

private:
    NetworkEventHandler eventHandler_;
    esp_netif_t* staNetif_ = nullptr;
    esp_netif_t* apNetif_ = nullptr;
    bool isAP_ = false;

    static void WifiEventHandler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data);
    void OnWifiEvent(esp_event_base_t event_base, int32_t event_id, void* event_data);
    void RaiseEvent(NetworkEventType type);
};
