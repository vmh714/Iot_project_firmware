#include "network.h"

#include "app_config.h"
#include <Arduino.h>
#include <WiFi.h>

static WiFiClient net_client;

bool network_init(void)
{
    WiFi.begin(WIFI_SSID, WIFI_PASS);

    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED)
    {
        if (millis() - start > 10000)
            return false;
        delay(200);
    }
    return true;
}

bool network_is_connected(void)
{
    return WiFi.status() == WL_CONNECTED;
}

Client *network_get_client(void)
{
    return &net_client;
}

const char* network_get_ip(void)
{
    static char ip_buf[16];
    strncpy(ip_buf, WiFi.localIP().toString().c_str(), sizeof(ip_buf) - 1);
    ip_buf[sizeof(ip_buf) - 1] = '\0';
    return ip_buf;
}

const char* network_get_mac(void)
{
    static char mac_buf[18];
    strncpy(mac_buf, WiFi.macAddress().c_str(), sizeof(mac_buf) - 1);
    mac_buf[sizeof(mac_buf) - 1] = '\0';
    return mac_buf;
}