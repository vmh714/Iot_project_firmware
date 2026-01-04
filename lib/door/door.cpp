#include "door.h"
#include "app_config.h"

#include <Arduino.h>
#include <Servo.h>

static Servo door_servo;

volatile bool s_sensor_event_triggered = false;
static unsigned long last_debounce_time = 0;
static unsigned long last_unlocked_time = 0;
const unsigned long AUTO_LOCK_TIMEOUT = 10000;
const unsigned long DEBOUNCE_DELAY = 200;

static door_event_cb_t door_evt_cb = nullptr;

static QueueHandle_t _cmd_queue = NULL; // Nhận lệnh mở (từ FP hoặc MQTT)
static QueueHandle_t _evt_queue = NULL; // Báo cáo tình hình (cho MQTT)
void door_register_event_callback(door_event_cb_t cb)
{
    door_evt_cb = cb;
}

static void door_emit_event(DoorEvent_t evt)
{
    if (door_evt_cb)
        door_evt_cb(evt);
}

void IRAM_ATTR door_sensor_isr()
{
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    s_sensor_event_triggered = true;
}
void door_init()
{
    pinMode(SENSOR_PIN, INPUT);
    attachInterrupt(digitalPinToInterrupt(SENSOR_PIN), door_sensor_isr, CHANGE);
    door_servo.attach(SERVO_PIN);
    door_servo.write(0);
}
void door_lock()
{
    door_servo.write(0);
}
void door_unlock()
{
    door_servo.write(180);
}
static void taskDoor(void *pvParameters)
{
    DoorFSMState_t state = DOOR_STATE_LOCKED;
    DoorOpenState_t last_open = DOOR_CLOSED;
    DoorRequest_t cmd;

    unsigned long unlocked_time = 0;

    for (;;)
    {
        /* ========= 1. Nhận command ========= */
        if (xQueueReceive(_cmd_queue, &cmd, 0) == pdPASS)
        {
            if (cmd == DOOR_REQUEST_UNLOCK && state == DOOR_STATE_LOCKED)
            {
                door_unlock();
                unlocked_time = millis();
                state = DOOR_STATE_UNLOCKED_WAIT_OPEN;
                door_emit_event(DOOR_EVT_UNLOCKED);
            }
        }

        /* ========= 2. Xử lý sensor ========= */
        if (s_sensor_event_triggered && state != DOOR_STATE_LOCKED)
        {
            s_sensor_event_triggered = false;

            DoorOpenState_t current_open = digitalRead(SENSOR_PIN) ? DOOR_OPEN : DOOR_CLOSED;

            if (current_open != last_open)
            {
                if (current_open == DOOR_OPEN)
                {
                    state = DOOR_STATE_OPEN;
                    door_emit_event(DOOR_EVT_OPENED);
                }
                else // OPEN → CLOSED
                {
                    door_lock();
                    state = DOOR_STATE_LOCKED;
                    door_emit_event(DOOR_EVT_CLOSED_AND_LOCKED);
                }
            }
            last_open = current_open;
        }
        if (state == DOOR_STATE_UNLOCKED_WAIT_OPEN)
        {
            /* ========= 3. Auto-lock timeout ========= */
            if (millis() - unlocked_time >= AUTO_LOCK_TIMEOUT)
            {
                door_lock();
                state = DOOR_STATE_LOCKED;
                door_emit_event(DOOR_EVT_WAIT_TIME_END_AND_LOCKED);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

// --- API KHỞI CHẠY TASK ---
void door_start_task(QueueHandle_t cmd_queue, QueueHandle_t report_queue)
{
    _cmd_queue = cmd_queue;
    _evt_queue = report_queue;

    xTaskCreatePinnedToCore(
        taskDoor,
        "TaskDoor",
        TASK_DOOR_STACK_SIZE, // Nhớ define trong app_config (vd: 2048)
        NULL,
        TASK_DOOR_PRIORITY, // Nhớ define (vd: 2)
        NULL,
        1 // Core 1
    );
}