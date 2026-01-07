#ifndef DOOR_H_
#define DOOR_H_

#include <Arduino.h>

#define OPEN 180
#define CLOSE 0
// ================== ENUM ==================

// Trạng thái CHỐT (cơ cấu khoá)
typedef enum
{
    DOOR_STATE_LOCKED,
    DOOR_STATE_UNLOCKED_WAIT_OPEN, // unlock nhưng chưa mở
    DOOR_STATE_OPEN,               // cửa đang mở
} DoorFSMState_t;
// Trạng thái CỬA (cảm biến cửa)
enum DoorOpenState_t
{

    DOOR_CLOSED = 0, // cửa đóng
    DOOR_OPEN = 1    // cửa đang mở
};
// Handle event
// enum DoorRequest_t
// {
//     DOOR_REQUEST_UNLOCK, // yêu cầu mở khoá
//     DOOR_REQUEST_NONE    // poll trạng thái cửa
// };
typedef enum
{
    DOOR_EVT_NONE,
    DOOR_EVT_UNLOCKED,
    DOOR_EVT_OPENED,
    DOOR_EVT_CLOSED_AND_LOCKED,
    DOOR_EVT_WAIT_TIME_END_AND_LOCKED
} DoorEvent_t;

// ================== TYPE ==================

struct DoorStatus_t
{
    DoorFSMState_t lock_state;
    DoorOpenState_t open_state;
};

// ================== API CÔNG KHAI ==================
typedef void (*door_event_cb_t)(DoorEvent_t evt);

void door_register_event_callback(door_event_cb_t cb);
// Khởi tạo phần cứng cửa
void door_init();
// Chốt cửa
void door_lock();
// Mở chốt
void door_unlock();
// Lấy trạng thái hiện tại của cửa
// DoorStatus_t door_get_status();
// void door_handle_event(DoorRequest_t req = DOOR_REQUEST_NONE);
void door_event_response(DoorEvent_t res);
void door_start_task(QueueHandle_t cmd_queue, QueueHandle_t report_queue);
extern volatile bool s_sensor_event_triggered;
#endif