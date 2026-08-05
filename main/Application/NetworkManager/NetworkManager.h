#pragma once

#include <atomic>
#include <stdint.h>

#include "WiFiInterface.h"
#include "ServiceProvider.h"
#include "InitState.h"
#include "CommandEntry.h"
#include "TypedSettings.h"
#include "Timer.h"

class Stream;

class NetworkManager {
    static constexpr const char* TAG = "NetworkManager";
    static constexpr int StaConnectTimeoutMs = 10000;
    static constexpr int MaxStaRetries = 3;

    static constexpr const char* DefaultApSsid = "Strux-AP";
    static constexpr const char* DefaultApPassword = ""; // Open network

public:
    explicit NetworkManager(ServiceProvider& serviceProvider);

    NetworkManager(const NetworkManager&) = delete;
    NetworkManager& operator=(const NetworkManager&) = delete;
    NetworkManager(NetworkManager&&) = delete;
    NetworkManager& operator=(NetworkManager&&) = delete;

    void Init();

    WiFiInterface& wifi();
    const WiFiInterface& wifi() const;

    bool IsAccessPoint() const { return wifi_interface_.IsAP(); }

    /// Is there an address to reach the world with yet? Asked by anything that dials
    /// OUT (the relay), because attempting it before an address exists produces
    /// nothing but a failed connection and the log noise of one.
    bool HasIpv4() const { return wifi_interface_.getStatus().has_ipv4; }

    /// Associated AP's signal strength in dBm. False when there is none to report
    /// (AP mode, or not associated).
    bool GetRssi(int8_t& out) const { return wifi_interface_.GetRssi(out); }

private:
    ServiceProvider& serviceProvider_;

    InitState initState;
    WiFiInterface wifi_interface_;

    // STA connection state
    char staSsid_[33] = {};
    char staPassword_[65] = {};
    std::atomic<int> staRetryCount_{0};
    std::atomic<bool> staConnected_{false};

    Timer connectTimer_;

    void HandleNetworkEvent(const NetworkEvent& event);
    void AttemptStaConnect();
    void FallbackToAP();

    // ── WebSocket commands (registered with CommandManager in Init) ──
    RequestError Cmd_WifiScan(CommandContext& ctx);

    inline static CommandEntry commands_[] = {
        { "wifi", "scan", &InvokeCommand<&NetworkManager::Cmd_WifiScan> },
    };

    // ── Settings (registered with SettingsManager in Init) ──
    inline static StringSetting wifiSsid_    { "wifi.ssid",     "WiFi SSID",     "" };
    inline static StringSetting wifiPassword_{ "wifi.password", "WiFi Password", "" };
};
