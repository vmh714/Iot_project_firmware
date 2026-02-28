#ifndef DISPLAY_H_
#define DISPLAY_H_

#include <Arduino.h>
#include <LiquidCrystal_I2C.h>
#include "app_config.h"

// Biến ngoại lai để các nơi khác có thể truy cập (nếu cần)
extern LiquidCrystal_I2C lcd;
extern QueueHandle_t lcd_queue;

// Hàm khởi tạo và chạy Task quản lý LCD
void display_init();
void display_start_task(QueueHandle_t lcd_q);

// Hàm tiện ích gửi thông báo xuất ra màn hình
void send_lcd_message(LcdMessageType_t type, const char *l1, const char *l2, uint32_t duration);

#endif // DISPLAY_H_
