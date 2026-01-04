#ifndef APP_CONFIG_H_
#define APP_CONFIG_H_

// UART For AS608
#define FP_UART_NUM 2
#define FP_TX_PIN 17
#define FP_RX_PIN 16
#define FP_BAUDRATE 57600

// Servo and door sensor
#define SERVO_PIN 4
#define SENSOR_PIN 15

// Wifi
#define WIFI_SSID "IT Hoc Bach Khoa"
#define WIFI_PASS "chungtalamotgiadinh"

// MQTT
#define MQTT_BROKER "broker.emqx.io"
#define MQTT_PORT 1883;
// SYSTEM
#define DEVICE_ID "esp32_door_001"
// FREERTOS
#define TASK_FP_STACK_SIZE 4096
#define TASK_FP_PRIORITY 3

#define TASK_DOOR_STACK_SIZE 2048
#define TASK_DOOR_PRIORITY 2

typedef enum
{
    EVT_FP_MATCH,   // Quét đúng vân tay
    EVT_FP_UNKNOWN, // Quét sai/không có trong db
    EVT_FP_ERROR,
    EVT_FP_ENROLL_DONE,
    EVT_FP_DELETE_DONE,
    EVT_FP_SHOW_ALL_DONE,
    EVT_DOOR_LOCKED,
    EVT_DOOR_UNLOCKED_WAIT_OPEN, // unlock nhưng chưa mở
    EVT_DOOR_OPEN
} SystemEventType_t;

typedef struct
{
    SystemEventType_t type;
    int value; // Ví dụ: ID vân tay, hoặc mã lỗi
} SystemEvent_t;

enum DoorRequest_t
{
    DOOR_REQUEST_UNLOCK, // yêu cầu mở khoá
    DOOR_REQUEST_NONE    // poll trạng thái cửa
};
enum FingerprintRequest_t
{
    FP_REQUEST_ENROLL, // yêu cầu mở khoá
    FP_REQUEST_DELETE_ID,
    FP_REQUEST_SHOW_ALL_ID,
    FP_REQUEST_NONE // poll trạng thái cửa
};
typedef struct
{
    FingerprintRequest_t type;
    int id; // dùng cho ENROLL / DELETE, còn SHOW_ALL thì bỏ qua
} FingerprintRequestMsg_t;
typedef enum
{
    SYS_IDLE,      // chờ quét vân tay
    SYS_UNLOCKED,  // đã unlock, chờ mở cửa
    SYS_DOOR_OPEN, // cửa đang mở
} SystemState_t;

#endif