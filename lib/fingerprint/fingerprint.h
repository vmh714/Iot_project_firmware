#ifndef FINGERPRINT_H_
#define FINGERPRINT_H_

#include <stdint.h>
#include <Arduino.h>
typedef enum
{
    /* ===== INIT EVENTS ===== */
    FP_EVT_INIT_OK,   // verify password OK
    FP_EVT_INIT_FAIL, // verify password FAIL / sensor not respond

    // /* ===== RUNTIME EVENTS ===== */
    // FP_EVT_IDLE,          // không có tay
    // FP_EVT_FINGER_DETECT, // phát hiện có tay
    // FP_EVT_MATCH,         // khớp vân tay
    // FP_EVT_NOT_MATCH,     // không khớp
    // FP_EVT_ERROR,          // lỗi khi chạy (comm, image fail...)
    FP_EVT_SCAN_IDLE,      // không có tay
    FP_EVT_SCAN_SUCCESS,   // tìm thấy ID
    FP_EVT_SCAN_NOT_MATCH, // vân tay không khớp
    FP_EVT_SCAN_ERROR      // lỗi kỹ thuật
} FingerprintEvent_t;

typedef enum
{
    FP_IDLE,        // không có tay
    FP_FINGER_ON,   // tay vừa đặt xuống
    FP_SCANNING,    // đang xử lý ảnh
    FP_WAIT_REMOVE, // chờ nhấc tay
} FP_InternalState_t;

typedef void (*fingerprint_event_cb_t)(
    FingerprintEvent_t evt,
    uint16_t finger_id // chỉ dùng cho FP_EVT_MATCH
);
void fingerprint_register_event_callback(fingerprint_event_cb_t cb);

bool fingerprint_init(void);
void fingerprint_scan_once(void);
void fingerprint_poll(void);
// Thay đổi hàm start để nhận 2 queue
void fingerprint_start_task(QueueHandle_t door_queue, QueueHandle_t report_queue);
#endif