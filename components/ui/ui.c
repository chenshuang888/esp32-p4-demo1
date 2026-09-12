/*
 * UI 层 —— LVGL 接入
 *
 * 职责：把 LVGL 接到硬件层暴露的句柄上
 *   1) 启动 LVGL（任务 / 锁 / tick）
 *   2) 接入显示（MIPI-DSI，开 avoid_tearing 防撕裂）
 *   3) 接入触摸
 *
 * 不负责：面板/触摸的硬件初始化（那是 lcd_screen 的事）、具体界面内容（那是 ui_demo 的事）
 */
#include "ui.h"

#include "esp_check.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"

#include "lcd_screen_display.h"
#include "lcd_screen_touch.h"

static const char *TAG = "ui";

esp_err_t ui_init(void)
{
    /* 1) 启动 LVGL：创建 LVGL 任务、递归锁、tick 定时器 */
    const lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_RETURN_ON_ERROR(lvgl_port_init(&port_cfg), TAG, "lvgl_port_init failed");

    /* 2) 接入显示：句柄来自硬件层 */
    esp_lcd_panel_handle_t panel = lcd_screen_display_handle();
    esp_lcd_panel_io_handle_t io = lcd_screen_display_io_handle();
    ESP_RETURN_ON_FALSE(panel != NULL && io != NULL, ESP_ERR_INVALID_STATE, TAG,
                        "display not initialized");

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle    = io,
        .panel_handle = panel,
        /* 必须以像素为单位给出（驱动一开始就 assert buffer_size > 0）。
         * 开 avoid_tearing 后它会被覆盖成全屏尺寸，而 direct_mode 也要求是全屏，故写全屏最合理 */
        .buffer_size  = LCD_SCREEN_H_RES * LCD_SCREEN_V_RES,
        .hres         = LCD_SCREEN_H_RES,
        .vres         = LCD_SCREEN_V_RES,
        .monochrome   = false,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .rotation     = {
            .swap_xy  = false,
            .mirror_x = false,
            .mirror_y = false,
        },
        .flags = {
            /* direct_mode: 直接渲染到面板帧缓冲，配合 avoid_tearing 才能防撕裂 */
            .direct_mode = 1,
            /* sw_rotate: 不让 lvgl_port 去调面板的 swap_xy/mirror。
             * EK79007(DSI) 未实现 swap_xy，硬调会打一条 ESP_ERR_NOT_SUPPORTED 的 E 日志；
             * 我们恒为 0° 不需要旋转，旋转交给软件/PPA 处理。 */
            .sw_rotate = 1,
        },
    };
    const lvgl_port_display_dsi_cfg_t dsi_cfg = {
        /* 用面板的内部帧缓冲做 LVGL 绘制缓冲，在帧边界交换 -> 不撕裂 */
        .flags = { .avoid_tearing = 1 },
    };
    lv_display_t *disp = lvgl_port_add_disp_dsi(&disp_cfg, &dsi_cfg);
    ESP_RETURN_ON_FALSE(disp != NULL, ESP_FAIL, TAG, "lvgl_port_add_disp_dsi failed");

    /* 3) 接入触摸（未初始化则跳过，只跑显示） */
    esp_lcd_touch_handle_t touch = lcd_screen_touch_handle();
    if (touch != NULL) {
        const lvgl_port_touch_cfg_t touch_cfg = {
            .disp   = disp,
            .handle = touch,
        };
        ESP_RETURN_ON_FALSE(lvgl_port_add_touch(&touch_cfg) != NULL, ESP_FAIL, TAG,
                            "lvgl_port_add_touch failed");
    } else {
        ESP_LOGW(TAG, "no touch handle, running display only");
    }

    ESP_LOGI(TAG, "LVGL ready (%dx%d)", LCD_SCREEN_H_RES, LCD_SCREEN_V_RES);
    return ESP_OK;
}
