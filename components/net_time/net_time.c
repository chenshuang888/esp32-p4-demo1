/*
 * net_time —— 实现
 *
 * 全部内容就是对 IDF 自带 SNTP 的薄封装：
 *   esp_sntp 在 lwip 里（esp_sntp.h），本项目 sdkconfig 已经把 SNTP 相关项配好了，
 *   所以这里不需要任何托管依赖、也不需要动 sdkconfig。
 *
 * ⚠️ 通知回调 on_sntp_sync() 跑在 **lwip/SNTP 任务** 里，不要在里面碰 LVGL。
 *    这里只做一件事：把 tv_sec 转成 int64 交给注入进来的回调。
 */
#include "net_time.h"

#include <stdbool.h>
#include <sys/time.h>

#include "esp_log.h"
#include "esp_sntp.h"

static const char *TAG = "net_time";

/* NTP 服务器。选国内可达性好的那一个；**运行时设置**，不走 sdkconfig
 * （sdkconfig 里的 CONFIG_LWIP_SNTP_MAX_SERVERS=1，所以只用 idx 0）。
 * 想换服务器改这里一行即可。 */
#define NET_TIME_SERVER  "ntp.aliyun.com"

static net_time_synced_cb_t s_on_synced  = NULL;
static bool                s_configured  = false;   /* net_time_init() 过 */
static bool                s_started     = false;   /* esp_sntp_init() 过 */

/* ---- SNTP 同步通知：跑在 lwip/SNTP 任务里，保持极短 ----
 * 每次从服务器拿到时间都会进来（首次同步、之后的周期更新、以及 sntp_restart()
 * 触发的强制重问）。重复通知是**对的**：能顺便校正走时漂移。 */
static void on_sntp_sync(struct timeval *tv)
{
    if (s_on_synced != NULL) {
        s_on_synced((int64_t)tv->tv_sec);
    }
}

esp_err_t net_time_init(net_time_synced_cb_t on_synced)
{
    s_on_synced = on_synced;

    /* ⚠️ operatingmode 必须在 esp_sntp_init() **之前**设（IDF 的要求） */
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, NET_TIME_SERVER);

    /* IMMED = 一收到就把系统时间改掉；另一种 SMOOTH 是慢慢调（我们不需要，
     * 因为开机时的时间本来就是错的，没有"平滑"的意义）。 */
    sntp_set_sync_mode(SNTP_SYNC_MODE_IMMED);
    sntp_set_time_sync_notification_cb(on_sntp_sync);

    s_configured = true;
    ESP_LOGI(TAG, "SNTP 已配置: server=%s", NET_TIME_SERVER);
    return ESP_OK;
}

esp_err_t net_time_start(void)
{
    if (!s_configured) {
        ESP_LOGE(TAG, "还没 net_time_init()，拒绝启动");
        return ESP_ERR_INVALID_STATE;
    }

    if (!s_started) {
        esp_sntp_init();
        s_started = true;
        ESP_LOGI(TAG, "SNTP 已启动，等待首次对时");
    } else {
        /* 又拿到一次 IP（换 AP / 重连）：强制立刻重问一次，别等更新周期
         * （CONFIG_LWIP_SNTP_UPDATE_DELAY 默认 1 小时）。 */
        if (sntp_restart()) {
            ESP_LOGI(TAG, "SNTP 已重启，立刻重新对时");
        }
    }
    return ESP_OK;
}
