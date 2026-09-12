/*
 * UI 层 —— 把 LVGL 接到屏幕上
 * 依赖硬件层 lcd_screen（通过它暴露的句柄），但不反过来依赖
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 LVGL：启动 LVGL 任务/锁，接入显示与触摸
 *
 * 前置条件：必须先完成
 *   - lcd_screen_display_init()
 *   - lcd_screen_touch_init()（可选；未初始化则只接显示）
 *
 * @return ESP_OK 成功，其余为 esp_err_t 错误码
 */
esp_err_t ui_init(void);

#ifdef __cplusplus
}
#endif
