#include "display.h"

LiquidCrystal_I2C lcd(LCD_ADDR, LCD_COLS, LCD_ROWS);
QueueHandle_t lcd_queue;

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
  if (lcd_queue != NULL) {
      xQueueSend(lcd_queue, &evt, 0);
  }
}

static void TaskLCD(void *pvParameters)
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

void display_start_task(QueueHandle_t lcd_q)
{
    lcd_queue = lcd_q;
    xTaskCreatePinnedToCore(TaskLCD, "TaskLCD", 3072, NULL, 1, NULL, 1);
}
