/*
 * 屏幕 —— 显示部分
 * 板子: Waveshare ESP32-P4-WIFI6-Touch-LCD-7B (EK79007, MIPI-DSI, 1024x600)
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "esp_lcd_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 屏幕分辨率（UI 层等外部也需要） */
#define LCD_SCREEN_H_RES    1024
#define LCD_SCREEN_V_RES    600

/**
 * @brief 初始化显示：背光 PWM -> MIPI DSI PHY 供电 -> DSI 总线 -> EK79007 面板
 *
 * 只负责硬件初始化，不做任何绘制。刷屏交给上层（如 LVGL）。
 *
 * @return ESP_OK 成功，其余为 esp_err_t 错误码
 */
esp_err_t lcd_screen_display_init(void);

/**
 * @brief 取面板句柄
 *
 * 必须在 lcd_screen_display_init() 成功后调用。
 */
esp_lcd_panel_handle_t lcd_screen_display_handle(void);

/**
 * @brief 取面板 IO 句柄（DBI 通道）
 *
 * LVGL 的 esp_lvgl_port 在 display 配置里需要它。
 * 必须在 lcd_screen_display_init() 成功后调用。
 */
esp_lcd_panel_io_handle_t lcd_screen_display_io_handle(void);

/**
 * @brief 设置背光亮度
 *
 * @param percent 0 ~ 100（大于 100 按 100 处理）
 */
esp_err_t lcd_screen_display_backlight(uint8_t percent);

#ifdef __cplusplus
}
#endif
