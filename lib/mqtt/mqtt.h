#ifndef MQTT_H
#define MQTT_H
#include <Arduino.h>

enum MqttEvent_t
{
    MQTT_NET_CONNECT_FAIL,
    MQTT_NET_CONNECTED,
    MQTT_NET_DISCONECTED
};

bool mqtt_init();

typedef void (*mqtt_event_cb_t)(MqttEvent_t evt);
void mqtt_register_event_callback(mqtt_event_cb_t cb);

#endif