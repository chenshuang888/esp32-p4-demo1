/*
 * 屏幕 —— 触摸部分（GT911, I2C）
 * 板子: Waveshare ESP32-P4-WIFI6-Touch-LCD-7B
 */
#pragma once

#include "esp_err.h"
#include "esp_lcd_touch.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化触摸：建立 I2C 总线 -> 探测 GT911 地址 -> 创建触摸驱动
 *
 * @return ESP_OK 成功；ESP_ERR_NOT_FOUND 表示未找到 GT911
 */
esp_err_t lcd_screen_touch_init(void);

/**
 * @brief 取触摸句柄
 *
 * 用于读取坐标，以后也交给 LVGL 使用。
 * 必须在 lcd_screen_touch_init() 成功后调用。
 */
esp_lcd_touch_handle_t lcd_screen_touch_handle(void);

#ifdef __cplusplus
}
#endif
