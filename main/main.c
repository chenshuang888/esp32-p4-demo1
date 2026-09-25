/*
 * 应用入口 —— 只做"编排"：
 *   硬件层 -> 能力层 -> 注册 App -> UI 层 -> 状态栏 -> 进主页
 *
 * 注意：main 只有"不写 REQUIRES"时才会自动依赖所有组件。本项目 main 显式声明了
 *       REQUIRES（见 main/CMakeLists.txt），所以这里用到的组件都要在那里列出来。
 */
#include <stdio.h>      /* snprintf：下面的"文本来源"回调要用 */
#include <time.h>

#include "esp_log.h"

#include "lcd_screen_display.h"
#include "lcd_screen_touch.h"
#include "ui.h"
#include "ui_status_bar.h"
#include "app_manager.h"
#include "desktop.h"
#include "app_demo.h"
#include "clock.h"
#include "camera.h"
#include "photo.h"
#include "settings.h"
#include "time_service.h"
#include "kv_store.h"
#include "sd_card.h"
#include "wifi_service.h"

static const char *TAG = "app";

/*
 * ===================== 状态栏要用的两个"胶水"函数 =====================
 *
 * 状态栏是通用的，它既不知道"时间"是什么，也不知道"返回"会切去哪。
 * 这两件事由 main 注入进去 —— main 本来就同时依赖 ui 和 time_service，
 * 让它来当这个中间人，ui 组件就能保持零业务耦合（详见 ui_status_bar.h）。
 */

/*
 * 状态栏右侧的文本来源：把 time_service 给的**结构化时间**变成**字符串**。
 * 格式在这里改就行（例如想带秒就改成 %02d:%02d:%02d），ui 一行都不用动。
 *
 * ⚠️ 它是在 LVGL 任务上下文里被调用的，所以里面不要再加 LVGL 锁。
 */
static void status_bar_clock_text(char *out, size_t out_size)
{
    struct tm now;
    if (time_service_get(&now) != ESP_OK) {
        out[0] = '\0';      /* 还没对过时：给空串（状态栏就不显示），而不是显示假时间 */
        return;
    }
    snprintf(out, out_size, "%02d:%02d", now.tm_hour, now.tm_min);
}

/*
 * 返回按钮的动作。
 *
 * 这里必须包一层：app_manager_go_home() 的类型是 esp_err_t(*)(void)，而状态栏
 * 的槽位要求 void(*)(void)，两者**不兼容** —— 直接把函数名传过去，编译器会报
 * incompatible pointer types；就算强转绕过，通过不兼容的函数指针调用也是
 * 未定义行为（C11 6.3.2.3 第 8 段）。包一层把返回值丢掉才是正确做法。
 */
static void status_bar_back(void)
{
    app_manager_go_home();
}

void app_main(void)
{
    /* ---- 1. 硬件层：点亮屏 + 起触摸 ---- */
    ESP_ERROR_CHECK(lcd_screen_display_init());
    lcd_screen_display_backlight(80);

    if (lcd_screen_touch_init() != ESP_OK) {
        ESP_LOGW(TAG, "触摸初始化失败，仅运行显示");
    }

    /* ---- 2. 能力层：存储 + 网络 + 时间 ---- */

    /* 存储要先于任何 App 读写之前建好（各 App 的 enter 里就会用到） */
    ESP_ERROR_CHECK(kv_init());

    /* SD 卡：失败是**正常情况**（没插卡），所以只告警不 panic ——
     * 插不插卡不该决定能不能开机。卡不在时由 App 侧自行降级。 */
    if (sd_card_init() != ESP_OK) {
        ESP_LOGW(TAG, "SD 卡未挂载，文件功能不可用");
    }

    /* WiFi：由板载 ESP32-C6 协处理器提供（P4 通过 SDIO 与它通信）。
     * 同样**非阻塞 + 失败不 panic** —— 网络是外设，不该决定能不能开机。
     * 连上与否看 wifi_service 打的日志（"拿到 IP: ..."）。
     * ⚠️ 必须在 kv_init() 之后：esp_wifi 要用 NVS 存配置，而且下面就要读 kv_store。
     * 放在 camera_init 之前，是为了不被相机那 5s 的等待拖住（见下）。 */
    if (wifi_service_init() != ESP_OK) {
        ESP_LOGW(TAG, "WiFi 未启动，网络功能不可用");
    } else {
        /* 凭据由 main 从 kv_store 读出来喂进组件 —— wifi_service **不认识 NVS**，
         * 这和 time_service_set() 是同一个模式：谁组装谁负责把外部输入接进来。
         * 键名常量（WIFI_CFG_*）定义在 wifi_service.h，和设置 App 写入时用的是同一份。
         *
         * 读不到 = 还没配过：保持"没有凭据"的状态，wifi_service_connect() 会拒绝。
         * 用户在设置 App 里配一次就会写进这里，下次开机自动生效。 */
        char ssid[33] = { 0 };
        char pass[64] = { 0 };

        if (kv_read_str(WIFI_CFG_NS, WIFI_CFG_KEY_SSID, ssid, sizeof(ssid)) == ESP_OK &&
            ssid[0] != '\0') {
            /* 密码读不到就当空串（开放网络），所以不把它的返回值当错 */
            kv_read_str(WIFI_CFG_NS, WIFI_CFG_KEY_PASS, pass, sizeof(pass));
            if (wifi_service_set_credentials(ssid, pass) == ESP_OK) {
                ESP_LOGI(TAG, "用已保存的凭据连接 WiFi: %s", ssid);
                /* 只发起连接，**不在这里等 IP** —— 等待会拖住开机（UI 还没建起来）。
                 * 想"先扫描看清楚再连"，调 wifi_service_scan()（阻塞 2~4 秒）。 */
                wifi_service_connect();
            }
        } else {
            ESP_LOGI(TAG, "还没有保存的 WiFi 凭据（去设置 App 里配）");
        }
    }

    /* ---- 相机链路：USB Host + UVC 驱动 + 拍照保存任务 ----
     * 由 camera App 提供，但属于"能力就位"阶段：必须在注册 App 之前完成。
     * 这里建的两样都**常驻**（进出 App 不重建）：
     *   usb_camera —— UVC 的"设备断开"是流级事件，没有活着的流就收不到通知；
     *   sd_save    —— 写盘期间持着 jbuf 读槽，中途被杀会让读槽永远释放不了。
     * 解码器**不在这里**建 —— 它跟着相机 App 的 enter/leave 按需生灭
     * （它没有不可中断的临界区，输入又是丢旧保新的流，杀掉重启零损失）。
     * 失败也继续启动 —— 摄像头是外设，不该决定能不能开机。 */
    if (camera_init() != ESP_OK) {
        ESP_LOGE(TAG, "相机初始化失败（App 仍会注册，进去只显示提示）");
    }

    time_service_init();

    /* 对时：由 main 负责把"外部时间源"接进来（组件本身不认识任何时间源）
     *
     * ⚠️ 现在是硬编码一个固定时间用于验证链路（约 2026-01，UTC 秒）。
     *    以后接 SNTP / RTC 芯片时，只改这一处 —— time_service 和 App 都不用动。 */
    time_service_set(1768000000);

    /* ---- 3. 注册 App ----
     * 顺序就是"已装 App 清单"。第 0 个是主页，所以桌面必须第一个。 */
    desktop_register();
    app_demo_register();
    clock_register();
    camera_register();
    photo_register();
    settings_register();

    /* ---- 4. UI 层：接入 LVGL ---- */
    ESP_ERROR_CHECK(ui_init());

    /* ---- 5. 状态栏：建一份全局的，并把两个外部动作注入进去 ----
     * 必须在 ui_init() 之后（状态栏要画在 LVGL 上），
     * 在 go_home() 之前（各 App 进 enter 时就要用状态栏）。 */
    ESP_ERROR_CHECK(ui_status_bar_init(status_bar_clock_text, status_bar_back));

    /* ---- 6. 进主页 ---- */
    ESP_ERROR_CHECK(app_manager_go_home());
}
