#include "fingerprint.h"
#include "app_config.h"

#include <Arduino.h>
#include <Adafruit_Fingerprint.h>
#include <HardwareSerial.h>

static HardwareSerial FPSerial(FP_UART_NUM);
static Adafruit_Fingerprint finger(&FPSerial);

static fingerprint_event_cb_t fp_evt_cb = nullptr;
static FP_InternalState_t fp_state = FP_IDLE;

static QueueHandle_t _door_queue = NULL;   // Để ra lệnh mở
static QueueHandle_t _report_queue = NULL; // Để báo cáo lên MQTT

void fingerprint_register_event_callback(fingerprint_event_cb_t cb)
{
    fp_evt_cb = cb;
}

static void fingerprint_emit_event(FingerprintEvent_t evt, uint16_t id = 0)
{
    if (fp_evt_cb)
        fp_evt_cb(evt, id);
}
// ===== helper: chờ nhấc tay =====
static void wait_finger_removed()
{
    while (finger.getImage() != FINGERPRINT_NOFINGER)
    {
        delay(100);
    }
}

// ===== helper: quét ID =====
static int scan_fingerprint_id()
{
    uint8_t p = finger.getImage();
    if (p != FINGERPRINT_OK)
        return -1; // Không thấy vân tay

    p = finger.image2Tz();
    if (p != FINGERPRINT_OK)
        return -2; // Lỗi chuyển đổi ảnh

    p = finger.fingerSearch();
    if (p != FINGERPRINT_OK)
        return -3; // Không tìm thấy ID khớp

    return finger.fingerID; // Thành công, trả về ID
}
bool fingerprint_init(void)
{
    FPSerial.begin(FP_BAUDRATE, SERIAL_8N1, FP_RX_PIN, FP_TX_PIN);
    finger.begin(FP_BAUDRATE);
    if (finger.verifyPassword())
    {
        fingerprint_emit_event(FP_EVT_INIT_OK);
        return false;
    }
    else
    {
        fingerprint_emit_event(FP_EVT_INIT_FAIL);
    }
    return true;
}
/*
void fingerprint_scan_once(void)
{
    uint8_t p = finger.getImage();
    // chờ có ngón tay
    switch (fp_state)
    {
    case FP_IDLE:
        if (p == FINGERPRINT_OK)
        {
            // phát hiện CẠNH: tay vừa đặt
            fp_state = FP_SCANNING;
        }
        fingerprint_emit_event(FP_EVT_SCAN_IDLE, 0);
        break;

    case FP_SCANNING:
    {
        int result = scan_fingerprint_id();

        if (result >= 0)
        {
            fingerprint_emit_event(FP_EVT_SCAN_SUCCESS, result);
            fp_state = FP_WAIT_REMOVE;
        }
        else if (result == -3)
        {
            fingerprint_emit_event(FP_EVT_SCAN_NOT_MATCH);
            fp_state = FP_WAIT_REMOVE;
        }
        else
        {
            fingerprint_emit_event(FP_EVT_SCAN_ERROR);
            fp_state = FP_WAIT_REMOVE;
        }
        break;
    }

    case FP_WAIT_REMOVE:
        if (p == FINGERPRINT_NOFINGER)
        {
            // phát hiện CẠNH: tay nhấc lên
            fp_state = FP_IDLE;
        }
        break;

    default:
        fp_state = FP_IDLE;
        break;
    }
}
*/
void fingerprint_poll()
{
}
static void taskFingerprint(void *pv)
{
    FP_InternalState_t state = FP_IDLE;
    uint8_t p;
    int id;

    for (;;)
    {
        p = finger.getImage();

        switch (state)
        {
        case FP_IDLE:
            if (p == FINGERPRINT_OK)
                state = FP_SCANNING;
            break;

        case FP_SCANNING:
            id = scan_fingerprint_id();
            if (id >= 0)
            {
                DoorRequest_t cmd = DOOR_REQUEST_UNLOCK;
                xQueueSend(_door_queue, &cmd, 0);

                // MQTT report
                // SystemEvent_t evt = {EVT_FP_MATCH, id};
                // xQueueSend(_report_queue, &evt, 0);

                state = FP_WAIT_REMOVE;
            }
            else if (id == -3)
            {
                // EVT_FP_UNKNOWN
                state = FP_WAIT_REMOVE;
            }
            else
            {
                // EVT_FP_ERROR
                state = FP_WAIT_REMOVE;
            }
            break;

        case FP_WAIT_REMOVE:
            if (p == FINGERPRINT_NOFINGER)
                state = FP_IDLE;
            break;
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void fingerprint_start_task(QueueHandle_t door_queue, QueueHandle_t report_queue)
{
    _door_queue = door_queue;
    _report_queue = report_queue;
    xTaskCreatePinnedToCore(
        taskFingerprint,
        "TaskFingerprint",
        TASK_FP_STACK_SIZE,
        NULL,
        TASK_FP_PRIORITY,
        NULL,
        1 // core 1 (core 0 để WiFi/MQTT)
    );
}