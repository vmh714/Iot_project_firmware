#include "mqtt.h"
#include "network.h"
#include "app_config.h"
#include <PubSubClient.h>

static PubSubClient mqtt;

static mqtt_event_cb_t mqtt_evt_cb = nullptr;

static QueueHandle_t _cmd_queue = NULL; // Nhận lệnh mở (từ FP hoặc MQTT)
static QueueHandle_t _evt_queue = NULL; // Báo cáo tình hình (cho MQTT)

void mqtt_register_event_callback(mqtt_event_cb_t cb)
{
    mqtt_evt_cb = cb;
}

static void door_emit_event(MqttEvent_t evt)
{
    if (mqtt_evt_cb)
        mqtt_evt_cb(evt);
}
static void callback(char *topic, uint8_t *payload, unsigned int length);
bool mqtt_init(void)
{

    mqtt.setClient(*network_get_client());
    mqtt.setServer(MQTT_BROKER, MQTT_PORT);
    mqtt.setCallback(callback);
    return true;
}