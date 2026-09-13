/*
 * time_service —— 时间能力组件实现
 *
 * 全部内容就是：一个"是否已对时"的标志 + 对 IDF 标准接口的薄封装。
 * 走时交给 esp_timer（IDF 内部完成），我们只负责"对时"和"读出来"。
 */
#include "time_service.h"

#include <stdbool.h>
#include <stdlib.h>
#include <sys/time.h>

#include "esp_log.h"

static const char *TAG = "time_service";

/* 中国标准时间 UTC+8。POSIX TZ 格式里，数字是"加到本地时间上得到 UTC"的小时数，
   所以 UTC+8 对应 -8。 */
#define TIME_SERVICE_TZ  "CST-8"

static bool s_synced = false;

esp_err_t time_service_init(void)
{
    setenv("TZ", TIME_SERVICE_TZ, 1);
    tzset();
    ESP_LOGI(TAG, "time zone: %s", TIME_SERVICE_TZ);
    return ESP_OK;
}

esp_err_t time_service_set(int64_t utc_sec)
{
    const struct timeval tv = {
        .tv_sec  = (time_t)utc_sec,
        .tv_usec = 0,
    };
    settimeofday(&tv, NULL);
    s_synced = true;
    ESP_LOGI(TAG, "time synced: %lld (utc sec)", (long long)utc_sec);
    return ESP_OK;
}

esp_err_t time_service_get(struct tm *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_synced) {
        return ESP_ERR_INVALID_STATE;
    }

    const time_t now = time(NULL);
    localtime_r(&now, out);
    return ESP_OK;
}
