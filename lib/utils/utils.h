#ifndef UTILS_H_
#define UTILS_H_

#include <Arduino.h>
#include "app_config.h"

// --- Hàm hỗ trợ lấy thời gian (ISO 8601) ---
String get_iso_timestamp();

// --- Ánh xạ sự kiện hệ thống sang MQTT Topic Category ---
const char *event_to_topic(SystemEventType_t type);

#endif // UTILS_H_
