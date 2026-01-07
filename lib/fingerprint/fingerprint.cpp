#include "fingerprint.h"
#include "app_config.h"

#include <Arduino.h>
#include <Adafruit_Fingerprint.h>
#include <HardwareSerial.h>

static HardwareSerial FPSerial(FP_UART_NUM);
static Adafruit_Fingerprint finger(&FPSerial);
static bool scan_enabled = true;
static fingerprint_event_cb_t fp_evt_cb = nullptr;
static FP_InternalState_t fp_state = FP_IDLE;

static QueueHandle_t _fp_req_queue = NULL; // Để ra lệnh mở
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
static int enroll_fingerprint(uint16_t id)
{
    uint8_t p;

    fingerprint_emit_event(FP_EVT_ENROLL_START, id);

    /* ===== STEP 1: lấy ảnh lần 1 ===== */
    while ((p = finger.getImage()) != FINGERPRINT_OK)
    {
        if (p == FINGERPRINT_NOFINGER)
            vTaskDelay(pdMS_TO_TICKS(100));
        else
            return -1;
    }

    p = finger.image2Tz(1);
    if (p != FINGERPRINT_OK)
        return -2;

    fingerprint_emit_event(FP_EVT_ENROLL_STEP1_OK, id);

    /* ===== yêu cầu nhấc tay ===== */
    while (finger.getImage() != FINGERPRINT_NOFINGER)
        vTaskDelay(pdMS_TO_TICKS(100));

    vTaskDelay(pdMS_TO_TICKS(500));

    /* ===== STEP 2: lấy ảnh lần 2 ===== */
    while ((p = finger.getImage()) != FINGERPRINT_OK)
    {
        if (p == FINGERPRINT_NOFINGER)
            vTaskDelay(pdMS_TO_TICKS(100));
        else
            return -3;
    }

    p = finger.image2Tz(2);
    if (p != FINGERPRINT_OK)
        return -4;

    fingerprint_emit_event(FP_EVT_ENROLL_STEP2_OK, id);

    /* ===== tạo model ===== */
    p = finger.createModel();
    if (p != FINGERPRINT_OK)
        return -5;

    /* ===== lưu model ===== */
    p = finger.storeModel(id);
    if (p != FINGERPRINT_OK)
        return -6;

    fingerprint_emit_event(FP_EVT_ENROLL_DONE, id);
    return id;
}

bool fingerprint_init(void)
{
    FPSerial.begin(FP_BAUDRATE, SERIAL_8N1, FP_RX_PIN, FP_TX_PIN);
    finger.begin(FP_BAUDRATE);

    if (finger.verifyPassword())
    {
        fingerprint_emit_event(FP_EVT_INIT_OK);
        return true;
    }
    else
    {
        fingerprint_emit_event(FP_EVT_INIT_FAIL);
        return false;
    }
}

void fingerprint_poll()
{
}
static void taskFingerprint(void *pv)
{
    FingerprintRequestMsg_t req;
    uint8_t p;
    int id;

    for (;;)
    {
        /* ===== 1. Handle REQUEST ===== */
        if (xQueueReceive(_fp_req_queue, &req, 0) == pdTRUE)
        {
            switch (req.type)
            {
            case FP_REQUEST_ENROLL:
                id = enroll_fingerprint(req.id);
                fingerprint_emit_event(FP_EVT_ENROLL_DONE, id);
                break;

            case FP_REQUEST_DELETE_ID:
                finger.deleteModel(req.id);
                fingerprint_emit_event(FP_EVT_DELETE_DONE, req.id);
                break;

            case FP_REQUEST_SHOW_ALL_ID:
                finger.getTemplateCount();
                fingerprint_emit_event(FP_EVT_SHOW_ALL_DONE, finger.templateCount);
                break;

            case FP_REQ_SCAN_ENABLE:
                scan_enabled = true;
                break;

            case FP_REQ_SCAN_DISABLE:
                scan_enabled = false;
                break;
            }
        }

        /* ===== 2. Runtime SCAN ===== */
        if (!scan_enabled)
        {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        p = finger.getImage();
        if (p == FINGERPRINT_OK)
        {
            id = scan_fingerprint_id();
            if (id >= 0)
                fingerprint_emit_event(FP_EVT_SCAN_SUCCESS, id);
            else
                fingerprint_emit_event(FP_EVT_SCAN_NOT_MATCH);

            wait_finger_removed();
        }

        vTaskDelay(pdMS_TO_TICKS(40));
    }
}

void fingerprint_start_task(QueueHandle_t fp_request_queue)
{
    _fp_req_queue = fp_request_queue;
    xTaskCreatePinnedToCore(
        taskFingerprint,
        "TaskFingerprint",
        TASK_FP_STACK_SIZE,
        NULL,
        TASK_FP_PRIORITY,
        NULL,
        1);
}