/*
 * 应用入口 —— 只做"编排"：
 *   硬件层 -> 能力层 -> 注册 App -> UI 层 -> 进主页
 *
 * 注意：main 只有"不写 REQUIRES"时才会自动依赖所有组件。本项目 main 显式声明了
 *       REQUIRES（见 main/CMakeLists.txt），所以这里用到的组件都要在那里列出来。
 */
#include <time.h>

#include "esp_log.h"

#include "lcd_screen_display.h"
#include "lcd_screen_touch.h"
#include "ui.h"
#include "app_manager.h"
#include "desktop.h"
#include "app_demo.h"
#include "clock.h"
#include "time_service.h"

static const char *TAG = "app";

void app_main(void)
{
    /* ---- 1. 硬件层：点亮屏 + 起触摸 ---- */
    ESP_ERROR_CHECK(lcd_screen_display_init());
    lcd_screen_display_backlight(80);

    if (lcd_screen_touch_init() != ESP_OK) {
        ESP_LOGW(TAG, "触摸初始化失败，仅运行显示");
    }

    /* ---- 2. 能力层：时间 ---- */
    time_service_init();

    /* 对时：由 main 负责把"外部时间源"接进来（组件本身不认识任何时间源）
     *
     * ⚠️ 现在是硬编码一个固定时间用于验证链路（约 2026-01，UTC 秒）。
     *    以后接 SNTP / RTC 芯片时，只改这一处 —— time_service 和 App 都不用动。 */
    time_service_set(1768000000);

    /* 临时验证：确认"对时 + 时区"都生效了（以后可删） */
    struct tm now;
    if (time_service_get(&now) == ESP_OK) {
        ESP_LOGI(TAG, "local time: %04d-%02d-%02d %02d:%02d:%02d",
                 now.tm_year + 1900, now.tm_mon + 1, now.tm_mday,
                 now.tm_hour, now.tm_min, now.tm_sec);
    }

    /* ---- 3. 注册 App ----
     * 顺序就是"已装 App 清单"。第 0 个是主页，所以桌面必须第一个。 */
    desktop_register();
    app_demo_register();
    clock_register();

    /* ---- 4. UI 层：接入 LVGL ---- */
    ESP_ERROR_CHECK(ui_init());

    /* ---- 5. 进主页 ---- */
    ESP_ERROR_CHECK(app_manager_go_home());
}
