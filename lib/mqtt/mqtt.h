#ifndef MQTT_H
#define MQTT_H
#include <Arduino.h>

enum MqttEvent_t
{
    MQTT_NET_CONNECT_FAIL,
    MQTT_NET_CONNECTED,
    MQTT_NET_DISCONECTED
};

typedef void (*mqtt_event_cb_t)(MqttEvent_t evt);

// Khởi tạo MQTT client và bắt đầu các tác vụ
bool mqtt_init();
void mqtt_start_tasks(
    QueueHandle_t _mqtt_payload_queue, 
    QueueHandle_t _system_evt_queue, 
    QueueHandle_t _door_cmd_queue, 
    QueueHandle_t _fp_request_queue
);

void mqtt_register_event_callback(mqtt_event_cb_t cb);

#endif