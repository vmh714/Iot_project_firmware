#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <LiquidCrystal_I2C.h>
#include "time.h" // Thư viện native để xử lý RTC nội và NTP

#include "app_config.h"
#include "door.h"
#include "fingerprint.h"
#include "utils.h"
#include "display.h"
#include "network.h"
#include "mqtt.h"

QueueHandle_t door_cmd_queue;   // Queue lệnh
QueueHandle_t fp_request_queue; // Queue lệnh cho fp
QueueHandle_t system_evt_queue; // Queue báo cáo (cho MQTT)
QueueHandle_t mqtt_payload_queue;

static SystemState_t sys_state = SYS_IDLE;
static bool is_scanning = true;

// NTP Server Configuration
const char *ntpServer = "in.pool.ntp.org";
const long gmtOffset_sec = 25200; // GMT+7 cho Vietnam (7 * 3600)
const int daylightOffset_sec = 0;

// HÀM HỖ TRỢ được gọi từ lib/utils/utils.h


// Hàm send_lcd_message được chuyển tới lib/display/display.cpp

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
  display_start_task(lcd_queue);

  // WiFi Connection qua lib/network
  Serial.print("Connecting to WiFi");
  send_lcd_message(LCD_MSG_INFO, "Connecting...", "WiFi Network", 0);
  
  if (network_init()) {
    Serial.println("\nWiFi connected!");
    Serial.print("Local IP: ");
    Serial.println(network_get_ip());
    send_lcd_message(LCD_MSG_INFO, "WiFi Connected", network_get_ip(), 2000);

    // Sync NTP Time
    Serial.println("Syncing time with NTP...");
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
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

    // Khởi tạo MQTT qua lib/mqtt
    if (mqtt_init()) {
      // Truyền vào các queue để task MQTT xử lý
      mqtt_start_tasks(mqtt_payload_queue, system_evt_queue, door_cmd_queue, fp_request_queue);
    } else {
      Serial.println("MQTT Failed");
    }
  } else {
    Serial.println("\nWiFi Failed!");
    send_lcd_message(LCD_MSG_ERROR, "WiFi Failed", "Check Network", 2000);
  }
}

void loop()
{
}