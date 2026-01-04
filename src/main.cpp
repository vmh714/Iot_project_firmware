// INCLUDE LIB
#include <Arduino.h>
// library for hardware

// library for MQTT
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

#include "app_config.h"
#include "door.h"
#include "fingerprint.h"

// TaskHandle_t task_fp_handle = NULL;
// TaskHandle_t task_door_handle = NULL;
#define LEDPIN 2

QueueHandle_t door_cmd_queue;   // Queue lệnh
QueueHandle_t fp_request_queue; // Queue lệnh cho fp
QueueHandle_t system_evt_queue; // Queue báo cáo (cho MQTT)
QueueHandle_t mqtt_payload_queue;
SemaphoreHandle_t mqtt_client_mutex;

static SystemState_t sys_state = SYS_IDLE;
static bool is_scanning = true;

// WiFi
const char *ssid = "IT Hoc Bach Khoa";        // Enter your Wi-Fi name
const char *password = "chungtalamotgiadinh"; // Enter Wi-Fi password

// MQTT Broker
const char *mqtt_broker = "broker.emqx.io";
const char *topic_base = "esp32/vmh-test";
const char *mqtt_username = "emqx-vmh-test";
const char *mqtt_password = "public";
const int mqtt_port = 1883;

String client_id = "esp32-client-";
WiFiClient espClient;
PubSubClient client(espClient);

struct MqttMsg
{
  char topic[50];
  char payload[150];
};

const char *event_to_topic(SystemEventType_t type)
{
  switch (type)
  {
  case EVT_FP_MATCH:
  case EVT_FP_UNKNOWN:
  case EVT_FP_ERROR:
  case EVT_FP_ENROLL_DONE:
  case EVT_FP_DELETE_DONE:
  case EVT_FP_SHOW_ALL_DONE:
    return "fingerprint";
  case EVT_DOOR_LOCKED:
  case EVT_DOOR_UNLOCKED_WAIT_OPEN:
  case EVT_DOOR_OPEN:
    return "door";

  default:
    return nullptr;
  }
}
void callback(char *topic, byte *payload, unsigned int length)
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

  xQueueSend(mqtt_payload_queue, &msg, 0);

  Serial.print("[MQTT] Command received: ");
  Serial.println(msg.payload);
}

void MqttControlTask(void *pvParameter)
{
  MqttMsg msg;

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

        FingerprintRequestMsg_t req;
        req.type = FP_REQUEST_ENROLL;

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

      else
      {
        Serial.print("[MQTT CTRL] Unknown cmd: ");
        Serial.println(cmd);
      }
    }
  }
}
void TaskMQTTClientLoop(void *pvParameters)
{
  (void)pvParameters;

  String cmd_topic;
  String status_topic;

  cmd_topic =
      String(topic_base) + "/" + client_id + "/command";

  status_topic =
      String(topic_base) + "/" + client_id + "/status";

  while (1)
  {
    if (xSemaphoreTake(mqtt_client_mutex, pdMS_TO_TICKS(2000)))
    {
      if (!client.connected())
      {
        Serial.println("[MQTT] Not connected, reconnecting...");

        if (client.connect(
                client_id.c_str(),
                mqtt_username,
                mqtt_password))
        {
          Serial.println("[MQTT] Reconnected");

          client.subscribe(cmd_topic.c_str());

          client.publish(
              status_topic.c_str(),
              "online");

          Serial.print("[MQTT] Subscribed: ");
          Serial.println(cmd_topic);

          Serial.print("[MQTT] Published: ");
          Serial.println(status_topic);
        }
        else
        {
          Serial.printf(
              "[MQTT] Reconnect failed, state=%d\n",
              client.state());
        }
      }
      else
      {
        client.loop();
      }

      xSemaphoreGive(mqtt_client_mutex);
    }

    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

void TaskMqttPublish(void *pvParameter)
{
  SystemEvent_t evt;
  char payload[128];

  Serial.println("[MQTT] Publish task started (fingerprint mode)");

  for (;;)
  {
    // Chờ event từ fingerprint/system
    if (xQueueReceive(system_evt_queue, &evt, portMAX_DELAY) == pdTRUE)
    {
      StaticJsonDocument<128> doc;

      doc["device"] = client_id;

      switch (evt.type)
      {
      /* ========= Fingerprint ========= */
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
      case EVT_FP_ENROLL_DONE:
        doc["event"] = "fp_enroll_done";
        doc["finger_id"] = evt.value;
        break;

      case EVT_FP_DELETE_DONE:
        doc["event"] = "fp_delete_done";
        doc["finger_id"] = evt.value;
        break;

      case EVT_FP_SHOW_ALL_DONE:
        doc["event"] = "fp_show_all_done";
        // payload có thể là string list, hiện mock thì bỏ
        break;
      /* ========= Door state ========= */
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

      default:
        continue;
      }

      serializeJson(doc, payload, sizeof(payload));

      if (xSemaphoreTake(mqtt_client_mutex, pdMS_TO_TICKS(2000)))
      {
        if (client.connected())
        {
          // client.publish((String(topic) + "/event").c_str(), payload);
          const char *category = event_to_topic(evt.type);
          if (category)
          {
            String full_topic = String(topic_base) + "/" + client_id + "/" + category;
            client.publish(full_topic.c_str(), payload);

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

void taskFingerprintMock(void *pv)
{
  FingerprintRequestMsg_t req;
  String input;

  static bool enrolling = false;
  static int next_mock_id = 1; // giả lập sensor cấp ID tăng dần

  Serial.println("[FP MOCK] Fingerprint mock task started");
  Serial.println("[FP MOCK] Commands:");
  Serial.println("  <number>   -> MATCH (scan)");
  Serial.println("  unknown    -> NOT MATCH");
  Serial.println("  show       -> show all IDs");
  Serial.println("  (ENROLL is triggered from MQTT)");

  for (;;)
  {
    /* ========= HANDLE REQUEST FROM SYSTEM ========= */
    if (xQueueReceive(fp_request_queue, &req, 0) == pdTRUE)
    {
      switch (req.type)
      {
      case FP_REQUEST_ENROLL:
      {
        enrolling = true;

        /* giả lập delay sensor */
        vTaskDelay(pdMS_TO_TICKS(500));

        int new_id = next_mock_id++;

        SystemEvent_t evt;
        evt.type = EVT_FP_ENROLL_DONE;
        evt.value = new_id;

        xQueueSend(system_evt_queue, &evt, 0);

        enrolling = false;

        Serial.printf(
            "[FP MOCK] ENROLL DONE → new ID = %d\n",
            new_id);
        break;
      }

      case FP_REQUEST_DELETE_ID:
      {
        SystemEvent_t evt;
        evt.type = EVT_FP_DELETE_DONE;
        evt.value = req.id;

        xQueueSend(system_evt_queue, &evt, 0);

        Serial.printf(
            "[FP MOCK] DELETE id=%d -> OK (mock)\n",
            req.id);
        break;
      }

      case FP_REQUEST_SHOW_ALL_ID:
      {
        SystemEvent_t evt;
        evt.type = EVT_FP_SHOW_ALL_DONE;
        evt.value = 0;

        xQueueSend(system_evt_queue, &evt, 0);

        Serial.println("[FP MOCK] SHOW ALL DONE (mock)");
        break;
      }

      default:
        break;
      }
    }

    /* ========= HANDLE SERIAL SCAN ========= */
    while (Serial.available())
    {
      char c = Serial.read();

      if (c == '\n' || c == '\r')
      {
        input.trim();

        if (input.equalsIgnoreCase("unknown"))
        {
          SystemEvent_t evt;
          evt.type = EVT_FP_UNKNOWN;
          evt.value = -1;

          xQueueSend(system_evt_queue, &evt, 0);
          Serial.println("[FP MOCK] NOT MATCH");
        }
        else
        {
          char *endptr;
          int id = strtol(input.c_str(), &endptr, 10);

          if (*endptr == '\0')
          {
            SystemEvent_t evt;
            evt.type = EVT_FP_MATCH;
            evt.value = id;

            xQueueSend(system_evt_queue, &evt, 0);

            DoorRequest_t door_cmd = DOOR_REQUEST_UNLOCK;
            xQueueSend(door_cmd_queue, &door_cmd, 0);

            Serial.printf(
                "[FP MOCK] MATCH id=%d → unlock door\n",
                id);
          }
          else
          {
            Serial.println("[FP MOCK] Invalid input");
          }
        }

        input = "";
      }
      else
      {
        input += c;
      }
    }

    vTaskDelay(pdMS_TO_TICKS(20));
  }
}
void door_event_handler(DoorEvent_t res)
{
  SystemEvent_t evt;
  evt.value = 0; // door event không cần value

  switch (res)
  {
  case DOOR_EVT_UNLOCKED:
    Serial.println("Door has been unlocked!");

    sys_state = SYS_IDLE; // vẫn đang chờ mở
    evt.type = EVT_DOOR_UNLOCKED_WAIT_OPEN;
    xQueueSend(system_evt_queue, &evt, 0);
    break;

  case DOOR_EVT_OPENED:
    Serial.println("Door opened");

    sys_state = SYS_DOOR_OPEN;
    evt.type = EVT_DOOR_OPEN;
    xQueueSend(system_evt_queue, &evt, 0);
    break;

  case DOOR_EVT_CLOSED_AND_LOCKED:
    Serial.println("Door closed and locked");

    sys_state = SYS_IDLE;
    evt.type = EVT_DOOR_LOCKED;
    xQueueSend(system_evt_queue, &evt, 0);
    break;

  case DOOR_EVT_WAIT_TIME_END_AND_LOCKED:
    Serial.println("Door auto-locked after timeout");

    sys_state = SYS_IDLE;
    evt.type = EVT_DOOR_LOCKED;
    xQueueSend(system_evt_queue, &evt, 0);
    break;

  default:
    break;
  }
}
void fingerprint_event_handler(FingerprintEvent_t evt, uint16_t id)
{
  // DoorRequest_t cmd;
  switch (evt)
  {
  case FP_EVT_INIT_OK:
    Serial.println("Fingerprint sensor ready");
    break;

  case FP_EVT_INIT_FAIL:
    Serial.println("Fingerprint sensor init failed");
    break;

  case FP_EVT_SCAN_IDLE:
    if (is_scanning)
    {
      Serial.println("Please scan your fingerprint");
      is_scanning = false;
    }
    break;

  case FP_EVT_SCAN_SUCCESS:
    if (sys_state == SYS_IDLE)
    {
      Serial.printf("Access granted, id=%d\n", id);
      sys_state = SYS_UNLOCKED;
      is_scanning = true;

      // cmd = DOOR_REQUEST_UNLOCK;
      // xQueueSend(door_cmd_queue, &cmd, 0);
    }
    break;

  case FP_EVT_SCAN_NOT_MATCH:
    if (sys_state == SYS_IDLE)
    {
      Serial.println("Access denied, try again");
    }
    break;

  case FP_EVT_SCAN_ERROR:
    Serial.println("Fingerprint runtime error");
    break;

  default:
    break;
  }
}
void mqtt_event_handler()
{
}
void setup()
{
  Serial.begin(115200);
  door_cmd_queue = xQueueCreate(5, sizeof(DoorRequest_t));
  fp_request_queue = xQueueCreate(5, sizeof(FingerprintRequestMsg_t));
  system_evt_queue = xQueueCreate(10, sizeof(SystemEvent_t)); // Queue này nên dài hơn chút
  mqtt_payload_queue = xQueueCreate(5, sizeof(MqttMsg));

  door_register_event_callback(door_event_handler);
  door_init();
  door_start_task(door_cmd_queue, system_evt_queue);

  // fingerprint_register_event_callback(fingerprint_event_handler);
  // if (fingerprint_init())
  // {
  //   fingerprint_start_task(door_cmd_queue, system_evt_queue);
  // }
  WiFi.begin(ssid, password);

  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED)
  {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi connected!");
  Serial.print("Local IP: ");
  Serial.println(WiFi.localIP());

  mqtt_client_mutex = xSemaphoreCreateMutex();
  if (mqtt_client_mutex == NULL)
  {
    Serial.println("Failed to create mqtt mutex");
    while (1)
      vTaskDelay(pdMS_TO_TICKS(1000));
  }

  /* ========= MQTT CONFIG ========= */
  client.setServer(mqtt_broker, mqtt_port);
  client.setCallback(callback);

  /* ========= BUILD CLIENT ID (ONCE) ========= */
  client_id = "esp32-" + WiFi.macAddress();

  /* ========= CONNECT ========= */
  while (!client.connected())
  {
    Serial.printf(
        "MQTT connecting as %s...\n",
        client_id.c_str());

    if (client.connect(
            client_id.c_str(),
            mqtt_username,
            mqtt_password))
    {
      Serial.println("MQTT broker connected");
    }
    else
    {
      Serial.print("failed, rc=");
      Serial.print(client.state());
      delay(2000);
    }
  }

  /* ========= SUBSCRIBE COMMAND TOPIC ========= */
  String cmd_topic =
      String(topic_base) + "/" + client_id + "/command";

  client.subscribe(cmd_topic.c_str());

  Serial.print("[MQTT] Subscribed: ");
  Serial.println(cmd_topic);

  /* ========= OPTIONAL: ONLINE EVENT ========= */
  String online_topic =
      String(topic_base) + "/" + client_id + "/status";

  client.publish(online_topic.c_str(), "online");

  xTaskCreate(taskFingerprintMock, "fp_mock", 4096, NULL, 3, NULL);
  xTaskCreatePinnedToCore(MqttControlTask, "MQTT's Command Control", 8198, NULL, 1, NULL, 1);
  xTaskCreatePinnedToCore(TaskMQTTClientLoop, "MQTT Loop", 4096, NULL, 2, NULL, 1);
  xTaskCreatePinnedToCore(TaskMqttPublish, "MQTT Publish", 4096, NULL, 1, NULL, 1);
}

void loop()
{
}
