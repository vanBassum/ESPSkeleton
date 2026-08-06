#pragma once

#include <stdint.h>

#include "esp_netif.h"
#include "esp_netif_ip_addr.h"
#include "esp_netif_types.h"

struct DnsServers {
    bool has_main = false;
    bool has_backup = false;
    bool has_fallback = false;

    esp_netif_dns_info_t main = {};
    esp_netif_dns_info_t backup = {};
    esp_netif_dns_info_t fallback = {};
};

struct NetworkStatus {
    static constexpr uint8_t MacLength = 6;

    bool link_up = false;
    bool has_ipv4 = false;

    esp_netif_ip_info_t ipv4 = {};
    uint8_t mac[MacLength] = {};
};

enum class NetworkEventType
{
    LinkUp,
    LinkDown,
    Ipv4Acquired,
    Ipv4Lost
};

struct NetworkEvent
{
    NetworkEventType type;
    NetworkStatus status;
};
