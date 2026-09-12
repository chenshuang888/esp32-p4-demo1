#include "esp_log.h"

#include "lcd_screen_display.h"
#include "lcd_screen_touch.h"
#include "ui.h"
#include "ui_demo.h"

static const char *TAG = "app";

void app_main(void)
{
    /* ---- 1. 硬件层：点亮屏 + 起触摸 ---- */
    ESP_ERROR_CHECK(lcd_screen_display_init());
    lcd_screen_display_backlight(80);

    if (lcd_screen_touch_init() != ESP_OK) {
        ESP_LOGW(TAG, "触摸初始化失败，仅运行显示");
    }

    /* ---- 2. UI 层：接入 LVGL + 建界面 ---- */
    ESP_ERROR_CHECK(ui_init());
    ui_demo_start();
}
