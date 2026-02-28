#include "mqtt.h"
#include "network.h"
#include "app_config.h"
#include "utils.h"
#include "display.h"      // For send_lcd_message
#include <PubSubClient.h>
#include <ArduinoJson.h>

static PubSubClient mqtt;
static mqtt_event_cb_t mqtt_evt_cb = nullptr;

static QueueHandle_t mqtt_payload_queue = NULL; 
static QueueHandle_t system_evt_queue = NULL;
static QueueHandle_t door_cmd_queue = NULL;
static QueueHandle_t fp_request_queue = NULL;

static SemaphoreHandle_t mqtt_client_mutex;
static String client_id;

const char *topic_base = "esp32/vmh-test";
const char *mqtt_username = "emqx-vmh-test";
const char *mqtt_password = "public";

void mqtt_register_event_callback(mqtt_event_cb_t cb)
{
    mqtt_evt_cb = cb;
}

static void callback(char *topic, byte *payload, unsigned int length)
{
  char expect_topic[128];
  snprintf(
      expect_topic,
      sizeof(expect_topic),
      "%s/%s/command",
      topic_base,
      client_id.c_str());

  if (strcmp(topic, expect_topic) != 0)
  {
    return; // không phải command của device này
  }

  MqttMsg msg = {0};

  strncpy(msg.topic, topic, sizeof(msg.topic) - 1);

  int copyLen = min((int)sizeof(msg.payload) - 1, (int)length);
  memcpy(msg.payload, payload, copyLen);
  msg.payload[copyLen] = '\0';

  if (mqtt_payload_queue != NULL) {
      xQueueSend(mqtt_payload_queue, &msg, 0);
  }

  Serial.print("[MQTT] Command received: ");
  Serial.println(msg.payload);
}

static void MqttControlTask(void *pvParameter)
{
  MqttMsg msg;
  Serial.println("[MQTT] Control task started");
  while (1)
  {
    if (xQueueReceive(mqtt_payload_queue, &msg, portMAX_DELAY))
    {
      Serial.print("[MQTT CTRL] Topic: ");
      Serial.println(msg.topic);
      Serial.print("[MQTT CTRL] Payload: ");
      Serial.println(msg.payload);

      StaticJsonDocument<256> doc;
      auto err = deserializeJson(doc, msg.payload);
      if (err)
      {
        Serial.println("[MQTT CTRL] JSON parse failed");
        continue;
      }

      const char *cmd = doc["cmd"];
      if (!cmd)
      {
        Serial.println("[MQTT CTRL] Missing cmd");
        continue;
      }

      /* ========= DOOR ========= */
      if (strcasecmp(cmd, "door_unlock") == 0)
      {
        DoorRequest_t door_cmd = DOOR_REQUEST_UNLOCK;
        xQueueSend(door_cmd_queue, &door_cmd, 0);

        Serial.println("[MQTT CTRL] Door unlock request");
      }
      /* ========= FINGERPRINT ENROLL ========= */
      else if (strcasecmp(cmd, "fp_enroll") == 0)
      {
        int id = doc["id"] | -1;
        if (id < 0)
          if (id < 0)
          {
            Serial.println("[MQTT CTRL] fp_delete missing id");
            continue;
          }
        FingerprintRequestMsg_t req;
        req.type = FP_REQUEST_ENROLL;
        req.id = id;
        xQueueSend(fp_request_queue, &req, 0);
        Serial.printf("[MQTT CTRL] FP enroll request");
      }
      /* ========= FINGERPRINT DELETE ========= */
      else if (strcasecmp(cmd, "fp_delete") == 0)
      {
        int id = doc["id"] | -1;
        if (id < 0)
        {
          Serial.println("[MQTT CTRL] fp_delete missing id");
          continue;
        }
        FingerprintRequestMsg_t req;
        req.type = FP_REQUEST_DELETE_ID;
        req.id = id;
        xQueueSend(fp_request_queue, &req, 0);
        Serial.printf("[MQTT CTRL] FP delete request, id=%d\n", id);
      }
      /* ========= FINGERPRINT SHOW ALL ========= */
      else if (strcasecmp(cmd, "fp_show_all") == 0)
      {
        FingerprintRequestMsg_t req;
        req.type = FP_REQUEST_SHOW_ALL_ID;
        req.id = 0;
        xQueueSend(fp_request_queue, &req, 0);
        Serial.println("[MQTT CTRL] FP show all IDs request");
      }
      else if (strcasecmp(cmd, "device_get_status") == 0)
      {

        SystemEvent_t req;
        req.type = EVT_STATUS_ONLINE;
        xQueueSend(system_evt_queue, &req, 0);
        Serial.println("[MQTT CTRL] get device's status request");
      }
      else
      {
        Serial.print("[MQTT CTRL] Unknown cmd: ");
        Serial.println(cmd);
      }
    }
  }
}

static void TaskMQTTClientLoop(void *pvParameters)
{
  (void)pvParameters;

  String cmd_topic;
  String status_topic;
  SystemEvent_t req;

  cmd_topic = String(topic_base) + "/" + client_id + "/command";
  status_topic = String(topic_base) + "/" + client_id + "/status";
  static unsigned long last_heartbeat_time = 0;

  Serial.println("[MQTT] Client Loop task started");
  while (1)
  {
    if (xSemaphoreTake(mqtt_client_mutex, pdMS_TO_TICKS(2000)))
    {
      if (!mqtt.connected())
      {
        Serial.println("[MQTT] Not connected, reconnecting...");

        if (mqtt.connect(client_id.c_str(), mqtt_username, mqtt_password))
        {
          Serial.println("[MQTT] Reconnected");
          mqtt.subscribe(cmd_topic.c_str());

          req.type = EVT_STATUS_ONLINE;
          xQueueSend(system_evt_queue, &req, 0);

          Serial.print("[MQTT] Subscribed: ");
          Serial.println(cmd_topic);
        }
        else
        {
          Serial.printf("[MQTT] Reconnect failed, state=%d\n", mqtt.state());
        }
      }
      else
      {
        mqtt.loop();
      }
      xSemaphoreGive(mqtt_client_mutex);
    }
    if (millis() - last_heartbeat_time > 60000)
    {
      last_heartbeat_time = millis();

      SystemEvent_t hb_req;
      hb_req.type = EVT_STATUS_ONLINE;
      xQueueSend(system_evt_queue, &hb_req, 0);

      Serial.println("[MQTT LOOP] Triggered 60s Heartbeat");
    }
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

// Bổ sung extern nếu cần gộp handler thông báo lỗi vân tay từ main.cpp
extern const char *fingerprint_enroll_fault_handler(int16_t err);

static void TaskMqttPublish(void *pvParameter)
{
  SystemEvent_t evt;
  char payload[256]; 

  Serial.println("[MQTT] Publish task started");

  for (;;)
  {
    if (xQueueReceive(system_evt_queue, &evt, portMAX_DELAY) == pdTRUE)
    {
      StaticJsonDocument<256> doc;
      doc["device"] = client_id;
      doc["ts"] = get_iso_timestamp();

      switch (evt.type)
      {
      case EVT_FP_MATCH:
        doc["event"] = "fp_match";
        doc["finger_id"] = evt.value;
        break;
      case EVT_FP_UNKNOWN:
        doc["event"] = "fp_unknown";
        break;
      case EVT_FP_ERROR:
        doc["event"] = "fp_error";
        break;
      case EVT_FP_ENROLL_SUCCESS:
        doc["event"] = "fp_enroll_success";
        doc["finger_id"] = evt.value;
        break;
      case EVT_FP_ENROLL_FAIL:
        doc["event"] = "fp_enroll_fail";
        doc["payload"] = fingerprint_enroll_fault_handler(evt.value);
        break;
      case EVT_FP_DELETE_DONE:
        doc["event"] = "fp_delete_done";
        doc["finger_id"] = evt.value;
        break;
      case EVT_FP_SHOW_ALL_DONE:
        doc["event"] = "fp_show_all_done";
        doc["count_of_IDs"] = evt.value;
        break;
      case EVT_DOOR_LOCKED:
        doc["event"] = "door_state";
        doc["state"] = "locked";
        break;
      case EVT_DOOR_UNLOCKED_WAIT_OPEN:
        doc["event"] = "door_state";
        doc["state"] = "unlocked_wait_open";
        break;
      case EVT_DOOR_OPEN:
        doc["event"] = "door_state";
        doc["state"] = "open";
        break;
      case EVT_STATUS_ONLINE:
        doc["event"] = "device_status";
        doc["status"] = "online";
        break;
      default:
        continue;
      }

      serializeJson(doc, payload, sizeof(payload));

      if (xSemaphoreTake(mqtt_client_mutex, pdMS_TO_TICKS(2000)))
      {
        if (mqtt.connected())
        {
          const char *category = event_to_topic(evt.type);
          if (category)
          {
            String full_topic = String(topic_base) + "/" + client_id + "/" + category;
            mqtt.publish(full_topic.c_str(), payload);

            Serial.print("[MQTT] Published to ");
            Serial.print(full_topic);
            Serial.print(": ");
            Serial.println(payload);
          }
        }
        else
        {
          Serial.println("[MQTT] Skip publish, mqtt not connected");
        }
        xSemaphoreGive(mqtt_client_mutex);
      }
    }
  }
}

bool mqtt_init(void)
{
    mqtt.setClient(*network_get_client());
    mqtt.setServer(MQTT_BROKER, MQTT_PORT);
    mqtt.setCallback(callback);

    mqtt_client_mutex = xSemaphoreCreateMutex();
    if (mqtt_client_mutex == NULL)
    {
        Serial.println("Failed to create mqtt mutex");
        return false;
    }

    client_id = "esp32-" + String(network_get_mac());

    while (!mqtt.connected())
    {
        Serial.printf("MQTT connecting as %s...\n", client_id.c_str());
        if (mqtt.connect(client_id.c_str(), mqtt_username, mqtt_password))
        {
            Serial.println("MQTT broker connected");
        }
        else
        {
            Serial.print("failed, rc=");
            Serial.print(mqtt.state());
            delay(2000);
        }
    }

    String cmd_topic = String(topic_base) + "/" + client_id + "/command";
    mqtt.subscribe(cmd_topic.c_str());
    Serial.print("[MQTT] Subscribed: ");
    Serial.println(cmd_topic);

    return true;
}

void mqtt_start_tasks(
    QueueHandle_t _mqtt_payload_queue, 
    QueueHandle_t _system_evt_queue, 
    QueueHandle_t _door_cmd_queue, 
    QueueHandle_t _fp_request_queue)
{
    // Lưu các queue vào biến static
    mqtt_payload_queue = _mqtt_payload_queue;
    system_evt_queue = _system_evt_queue;
    door_cmd_queue = _door_cmd_queue;
    fp_request_queue = _fp_request_queue;

    xTaskCreatePinnedToCore(MqttControlTask, "MQTT Cmd", 8198, NULL, 1, NULL, 1);
    xTaskCreatePinnedToCore(TaskMQTTClientLoop, "MQTT Loop", 4096, NULL, 2, NULL, 1);
    xTaskCreatePinnedToCore(TaskMqttPublish, "MQTT Pub", 4096, NULL, 1, NULL, 1);
}