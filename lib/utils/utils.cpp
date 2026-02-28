#include "utils.h"
#include "time.h" // Thư viện native để xử lý RTC nội và NTP

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
