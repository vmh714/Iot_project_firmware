#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <LiquidCrystal_I2C.h>
#include "time.h" // Thư viện native để xử lý RTC nội và NTP

#include "app_config.h"
#include "door.h"
#include "fingerprint.h"

QueueHandle_t door_cmd_queue;   // Queue lệnh
QueueHandle_t fp_request_queue; // Queue lệnh cho fp
QueueHandle_t system_evt_queue; // Queue báo cáo (cho MQTT)
QueueHandle_t mqtt_payload_queue;
SemaphoreHandle_t mqtt_client_mutex;

static SystemState_t sys_state = SYS_IDLE;
static bool is_scanning = true;

// NTP Server Configuration
const char *ntpServer = "in.pool.ntp.org";
const long gmtOffset_sec = 25200; // GMT+7 cho Vietnam (7 * 3600)
const int daylightOffset_sec = 0;

// MQTT Broker
const char *mqtt_broker = "broker.emqx.io";
const char *topic_base = "esp32/vmh-test";
const char *mqtt_username = "emqx-vmh-test";
const char *mqtt_password = "public";
const int mqtt_port = 1883;

LiquidCrystal_I2C lcd(LCD_ADDR, LCD_COLS, LCD_ROWS);
QueueHandle_t lcd_queue;

String client_id = "esp32-client-";
WiFiClient espClient;
PubSubClient client(espClient);

struct MqttMsg
{
  char topic[50];
  char payload[150];
};

// --- HÀM HỖ TRỢ LẤY THỜI GIAN TỪ RTC NỘI ---
String get_iso_timestamp()
{
  struct tm timeinfo;
  // getLocalTime sẽ lấy thời gian từ RTC nội (đã được sync bởi configTime)
  if (!getLocalTime(&timeinfo))
  {
    return "1970-01-01T00:00:00Z"; // Trả về mặc định nếu chưa sync được giờ
  }

  char timeStringBuff[30];
  // Định dạng chuỗi theo ISO 8601: YYYY-MM-DDTHH:MM:SSZ
  strftime(timeStringBuff, sizeof(timeStringBuff), "%Y-%m-%dT%H:%M:%SZ", &timeinfo);

  return String(timeStringBuff);
}

const char *event_to_topic(SystemEventType_t type)
{
  switch (type)
  {
  case EVT_FP_MATCH:
  case EVT_FP_UNKNOWN:
  case EVT_FP_ERROR:
  case EVT_FP_ENROLL_SUCCESS:
  case EVT_FP_ENROLL_FAIL:
  case EVT_FP_DELETE_DONE:
  case EVT_FP_SHOW_ALL_DONE:
    return "fingerprint";
  case EVT_DOOR_LOCKED:
  case EVT_DOOR_UNLOCKED_WAIT_OPEN:
  case EVT_DOOR_OPEN:
    return "door";
  case EVT_STATUS_ONLINE:
    return "status";
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
void send_lcd_message(LcdMessageType_t type, const char *l1, const char *l2, uint32_t duration)
{
  LcdEvent_t evt;
  evt.type = type;
  evt.duration = duration;

  // Copy chuỗi an toàn, tránh tràn bộ nhớ
  strncpy(evt.line1, l1, sizeof(evt.line1) - 1);
  evt.line1[sizeof(evt.line1) - 1] = '\0';

  strncpy(evt.line2, l2, sizeof(evt.line2) - 1);
  evt.line2[sizeof(evt.line2) - 1] = '\0';

  // Gửi vào queue (wait 0 để không treo callback nếu queue đầy)
  xQueueSend(lcd_queue, &evt, 0);
}

void door_event_handler(DoorEvent_t res)
{
  SystemEvent_t evt;
  evt.value = 0;
  Serial.print("DOOR ");
  switch (res)
  {
  case DOOR_EVT_UNLOCKED:
    Serial.println("Door has been unlocked!");
    sys_state = SYS_IDLE;
    evt.type = EVT_DOOR_UNLOCKED_WAIT_OPEN;
    xQueueSend(system_evt_queue, &evt, 0);
    break;
  case DOOR_EVT_OPENED:
    Serial.println("Door opened");
    send_lcd_message(LCD_MSG_DOOR_OPEN, "DOOR OPENED", "Be Careful", 3000);
    sys_state = SYS_DOOR_OPEN;
    evt.type = EVT_DOOR_OPEN;
    xQueueSend(system_evt_queue, &evt, 0);
    break;
  case DOOR_EVT_CLOSED_AND_LOCKED:
    send_lcd_message(LCD_MSG_IDLE, "Door Locked", "\0", 2000);
    Serial.println("Door closed and locked");
    sys_state = SYS_IDLE;
    evt.type = EVT_DOOR_LOCKED;
    xQueueSend(system_evt_queue, &evt, 0);
    break;
  case DOOR_EVT_WAIT_TIME_END_AND_LOCKED:
    send_lcd_message(LCD_MSG_IDLE, "Door auto-locked", "after timeout", 2000);
    Serial.println("Door auto-locked after timeout");
    sys_state = SYS_IDLE;
    evt.type = EVT_DOOR_LOCKED;
    xQueueSend(system_evt_queue, &evt, 0);
    break;
  default:
    break;
  }
}
const char *fingerprint_enroll_fault_handler(int16_t err)
{
  switch (err)
  {
  case -1:
    Serial.println("[FP][ENROLL] Step 1: Failed to get image");
    return "ENROLL_FAIL_STEP1_GET_IMAGE";

  case -2:
    Serial.println("[FP][ENROLL] Step 1: image2Tz(1) failed");
    return "ENROLL_FAIL_STEP1_IMAGE_CONVERT";

  case -3:
    Serial.println("[FP][ENROLL] Step 2: Failed to get image");
    return "ENROLL_FAIL_STEP2_GET_IMAGE";

  case -4:
    Serial.println("[FP][ENROLL] Step 2: image2Tz(2) failed");
    return "ENROLL_FAIL_STEP2_IMAGE_CONVERT";

  case -5:
    Serial.println("[FP][ENROLL] createModel() failed");
    return "ENROLL_FAIL_CREATE_MODEL";

  case -6:
    Serial.println("[FP][ENROLL] storeModel() failed");
    return "ENROLL_FAIL_STORE_MODEL";
  case -100:
    Serial.println("[FP][ENROLL] Duplicate found");
    return "ENROLL_FAIL_DUPLICATE_FOUND";
  default:
    Serial.print("[FP][ENROLL] Unknown error code: ");
    Serial.println(err);
    return "ENROLL_FAIL_UNKNOWN";
  }
}

void fingerprint_event_handler(FingerprintEvent_t res, int16_t id)
{
  DoorRequest_t cmd;
  SystemEvent_t evt;
  char buff[16];
  switch (res)
  {
  case FP_EVT_INIT_OK:
    Serial.println("[FP] Sensor ready");
    break;

  case FP_EVT_INIT_FAIL:
    Serial.println("[FP] Sensor init failed");
    break;

  case FP_EVT_ENROLL_START:
    snprintf(buff, sizeof(buff), "Enroll ID: %d", id);
    Serial.printf("[FP] Start enrolling finger id=%d\n", id);
    send_lcd_message(LCD_MSG_INFO, "Place Finger", buff, 0);
    break;

  case FP_EVT_ENROLL_STEP1_OK:
    Serial.printf("[FP] Step 1 OK for finger id=%d. Please lift your finger.\n", id);
    send_lcd_message(LCD_MSG_INFO, "Step 1 OK", "Lift Finger Now", 2000);
    break;

  case FP_EVT_ENROLL_STEP2_OK:
    Serial.printf("[FP] Step 2 OK for finger id=%d. Creating model...\n", id);
    send_lcd_message(LCD_MSG_INFO, "Step 2 OK", "Processing...", 1000);
    break;

  case FP_EVT_ENROLL_DONE:
    if (id >= 0)
    {
      Serial.printf("[FP] Enroll done for finger id=%d\n", id);
      send_lcd_message(LCD_MSG_SUCCESS, "Enroll Done!", "Success", 2000);
      evt.type = EVT_FP_ENROLL_SUCCESS;
      evt.value = id;
      xQueueSend(system_evt_queue, &evt, 0);
    }
    else
    {
      Serial.printf("[FP] Enroll fail! ERROR Code: %d\n", id);
      send_lcd_message(LCD_MSG_ERROR, "Enroll Failed", "Error", 2000);
      evt.type = EVT_FP_ENROLL_FAIL;
      xQueueSend(system_evt_queue, &evt, 0);
    }
    // Gửi event lên MQTT nếu cần
    break;

  case FP_EVT_DELETE_DONE:
    Serial.printf("[FP] Delete done for finger id=%d\n", id);
    // Gửi event lên MQTT nếu cần
    break;

  case FP_EVT_SHOW_ALL_DONE:
    Serial.println("[FP] Show all IDs done");
    // Gửi event lên MQTT nếu cần
    evt.type = EVT_FP_SHOW_ALL_DONE;
    evt.value = id;
    xQueueSend(system_evt_queue, &evt, 0);
    break;

  case FP_EVT_SCAN_IDLE:
    if (is_scanning)
    {
      Serial.println("Please scan your fingerprint");
      is_scanning = false;
    }
    break;

  case FP_EVT_SCAN_SUCCESS:
    snprintf(buff, sizeof(buff), "ID: %d", id);
    Serial.printf("Access granted, id=%d\n", id);
    send_lcd_message(LCD_MSG_SUCCESS, "Access Granted", buff, 3000);
    cmd = DOOR_REQUEST_UNLOCK;
    xQueueSend(door_cmd_queue, &cmd, 0);
    evt.type = EVT_FP_MATCH;
    evt.value = id;
    xQueueSend(system_evt_queue, &evt, 0);
    break;

  case FP_EVT_SCAN_NOT_MATCH:
    Serial.println("Access denied");
    send_lcd_message(LCD_MSG_ERROR, "Access Denied", "Try Again", 2000);
    break;

  case FP_EVT_SCAN_ERROR:
    Serial.println("Fingerprint error");
    // Gửi event lên MQTT nếu cần
    break;

  default:
    break;
  }
}
void TaskLCD(void *pvParameters)
{
  LcdEvent_t msg;

  // Khởi tạo LCD
  lcd.init();
  lcd.backlight();

  // Hiển thị màn hình khởi động
  lcd.setCursor(0, 0);
  lcd.print("System Booting..");
  vTaskDelay(pdMS_TO_TICKS(1000));

  // Trạng thái nội bộ để quản lý việc tự động quay về IDLE
  unsigned long last_temp_msg_time = 0;
  bool is_showing_temp_msg = false;
  uint32_t current_msg_duration = 0;

  // Gửi lệnh IDLE ban đầu
  LcdEvent_t idleMsg;
  idleMsg.type = LCD_MSG_IDLE;
  strcpy(idleMsg.line1, "IoT Smart Door");
  strcpy(idleMsg.line2, "Ready to scan...");
  idleMsg.duration = 0;

  // Vẽ màn hình IDLE ngay lập tức
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(idleMsg.line1);
  lcd.setCursor(0, 1);
  lcd.print(idleMsg.line2);

  for (;;)
  {
    // 1. Kiểm tra xem có tin nhắn mới trong Queue không (Non-blocking hoặc wait ngắn)
    if (xQueueReceive(lcd_queue, &msg, pdMS_TO_TICKS(100)))
    {

      // Xóa màn hình và in nội dung mới
      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print(msg.line1);
      lcd.setCursor(0, 1);
      lcd.print(msg.line2);

      // Nếu đây là tin nhắn tạm thời (có duration > 0)
      if (msg.duration > 0)
      {
        is_showing_temp_msg = true;
        current_msg_duration = msg.duration;
        last_temp_msg_time = millis();
      }
      else
      {
        // Nếu là tin nhắn vĩnh viễn (ví dụ IDLE update), tắt cờ tạm
        is_showing_temp_msg = false;
      }
    }

    // 2. Logic tự động quay về màn hình chờ (IDLE) sau khi hiển thị thông báo xong
    if (is_showing_temp_msg)
    {
      if (millis() - last_temp_msg_time > current_msg_duration)
      {
        is_showing_temp_msg = false;

        // Quay về màn hình chờ mặc định
        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print("IoT Smart Door");

        // Dòng 2 hiển thị giờ (nếu có thể lấy từ hàm get_iso_timestamp hoặc đơn giản là text)
        lcd.setCursor(0, 1);
        lcd.print("Please Scan...");
      }
    }

    // Delay nhẹ để nhường CPU
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}
void MqttControlTask(void *pvParameter)
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

void TaskMQTTClientLoop(void *pvParameters)
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
      if (!client.connected())
      {
        Serial.println("[MQTT] Not connected, reconnecting...");

        if (client.connect(client_id.c_str(), mqtt_username, mqtt_password))
        {
          Serial.println("[MQTT] Reconnected");
          client.subscribe(cmd_topic.c_str());

          req.type = EVT_STATUS_ONLINE;
          xQueueSend(system_evt_queue, &req, 0);

          Serial.print("[MQTT] Subscribed: ");
          Serial.println(cmd_topic);
        }
        else
        {
          Serial.printf("[MQTT] Reconnect failed, state=%d\n", client.state());
        }
      }
      else
      {
        client.loop();
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

void TaskMqttPublish(void *pvParameter)
{
  SystemEvent_t evt;
  char payload[256]; // buffer để chứa chuỗi thời gian

  Serial.println("[MQTT] Publish task started");

  for (;;)
  {
    // Chờ event từ fingerprint/system
    if (xQueueReceive(system_evt_queue, &evt, portMAX_DELAY) == pdTRUE)
    {
      // Tăng size JSON Document để chứa timestamp
      StaticJsonDocument<256> doc;

      doc["device"] = client_id;

      // === THÊM TIMESTAMP VÀO JSON ===
      doc["ts"] = get_iso_timestamp();

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
        if (client.connected())
        {
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

// void taskFingerprintMock(void *pv)
// {
//   FingerprintRequestMsg_t req;
//   String input;
//   static bool enrolling = false;
//   static int next_mock_id = 1;

//   Serial.println("[FP MOCK] Fingerprint mock task started");
//   for (;;)
//   {
//     if (xQueueReceive(fp_request_queue, &req, 0) == pdTRUE)
//     {
//       switch (req.type)
//       {
//       case FP_REQUEST_ENROLL:
//       {
//         enrolling = true;
//         vTaskDelay(pdMS_TO_TICKS(500));
//         int new_id = next_mock_id++;
//         SystemEvent_t evt;
//         evt.type = EVT_FP_ENROLL_DONE;
//         evt.value = new_id;
//         xQueueSend(system_evt_queue, &evt, 0);
//         enrolling = false;
//         Serial.printf("[FP MOCK] ENROLL DONE → new ID = %d\n", new_id);
//         break;
//       }
//       case FP_REQUEST_DELETE_ID:
//       {
//         SystemEvent_t evt;
//         evt.type = EVT_FP_DELETE_DONE;
//         evt.value = req.id;
//         xQueueSend(system_evt_queue, &evt, 0);
//         Serial.printf("[FP MOCK] DELETE id=%d -> OK (mock)\n", req.id);
//         break;
//       }
//       case FP_REQUEST_SHOW_ALL_ID:
//       {
//         SystemEvent_t evt;
//         evt.type = EVT_FP_SHOW_ALL_DONE;
//         evt.value = 0;
//         xQueueSend(system_evt_queue, &evt, 0);
//         Serial.println("[FP MOCK] SHOW ALL DONE (mock)");
//         break;
//       }
//       default:
//         break;
//       }
//     }
//     while (Serial.available())
//     {
//       char c = Serial.read();
//       if (c == '\n' || c == '\r')
//       {
//         input.trim();
//         if (input.equalsIgnoreCase("unknown"))
//         {
//           SystemEvent_t evt;
//           evt.type = EVT_FP_UNKNOWN;
//           evt.value = -1;
//           xQueueSend(system_evt_queue, &evt, 0);
//           Serial.println("[FP MOCK] NOT MATCH");
//         }
//         else
//         {
//           char *endptr;
//           int id = strtol(input.c_str(), &endptr, 10);
//           if (*endptr == '\0')
//           {
//             SystemEvent_t evt;
//             evt.type = EVT_FP_MATCH;
//             evt.value = id;
//             xQueueSend(system_evt_queue, &evt, 0);
//             DoorRequest_t door_cmd = DOOR_REQUEST_UNLOCK;
//             xQueueSend(door_cmd_queue, &door_cmd, 0);
//             Serial.printf("[FP MOCK] MATCH id=%d → unlock door\n", id);
//           }
//         }
//         input = "";
//       }
//       else
//       {
//         input += c;
//       }
//     }
//     vTaskDelay(pdMS_TO_TICKS(20));
//   }
// }

void setup()
{
  Serial.begin(115200);

  // Create Queues
  door_cmd_queue = xQueueCreate(5, sizeof(DoorRequest_t));
  fp_request_queue = xQueueCreate(5, sizeof(FingerprintRequestMsg_t));
  system_evt_queue = xQueueCreate(10, sizeof(SystemEvent_t));
  mqtt_payload_queue = xQueueCreate(5, sizeof(MqttMsg));
  lcd_queue = xQueueCreate(5, sizeof(LcdEvent_t));

  // Init Door
  door_register_event_callback(door_event_handler);
  door_init();
  door_start_task(door_cmd_queue, system_evt_queue);

  fingerprint_register_event_callback(fingerprint_event_handler);

  if (fingerprint_init())
  {
    fingerprint_start_task(fp_request_queue);
  }
  xTaskCreatePinnedToCore(TaskLCD, "TaskLCD", 3072, NULL, 1, NULL, 1);

  // WiFi Connection
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("Connecting to WiFi");
  send_lcd_message(LCD_MSG_INFO, "Connecting...", "WiFi Network", 0);
  while (WiFi.status() != WL_CONNECTED)
  {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi connected!");
  Serial.print("Local IP: ");
  Serial.println(WiFi.localIP());
  send_lcd_message(LCD_MSG_INFO, "WiFi Connected", WiFi.localIP().toString().c_str(), 2000);

  // === INIT TIME SYNC (NTP) ===
  // Bước này sẽ cấu hình RTC nội tự động sync với server
  Serial.println("Syncing time with NTP...");
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);

  // Chờ sync thời gian lần đầu (optional nhưng khuyến khích)
  struct tm timeinfo;
  if (getLocalTime(&timeinfo))
  {
    Serial.println("Time synced successfully!");
    Serial.println(&timeinfo, "Current time: %A, %B %d %Y %H:%M:%S");
  }
  else
  {
    Serial.println("Time sync failed, will retry in background.");
  }

  // Init MQTT Mutex
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
    Serial.printf("MQTT connecting as %s...\n", client_id.c_str());
    if (client.connect(client_id.c_str(), mqtt_username, mqtt_password))
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

  String cmd_topic = String(topic_base) + "/" + client_id + "/command";
  client.subscribe(cmd_topic.c_str());
  Serial.print("[MQTT] Subscribed: ");
  Serial.println(cmd_topic);

  String online_topic = String(topic_base) + "/" + client_id + "/status";
  SystemEvent_t evt;
  evt.type = EVT_STATUS_ONLINE;
  evt.value = 0;
  xQueueSend(system_evt_queue, &evt, 0);

  // Create Tasks
  // xTaskCreate(taskFingerprintMock, "fp_mock", 4096, NULL, 3, NULL);
  xTaskCreatePinnedToCore(MqttControlTask, "MQTT Cmd", 8198, NULL, 1, NULL, 1);
  xTaskCreatePinnedToCore(TaskMQTTClientLoop, "MQTT Loop", 4096, NULL, 2, NULL, 1);
  xTaskCreatePinnedToCore(TaskMqttPublish, "MQTT Pub", 4096, NULL, 1, NULL, 1);
}

void loop()
{
}