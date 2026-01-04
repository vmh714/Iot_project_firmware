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