/*
 * weather_service —— 实现（见头文件的模型说明）
 *
 * 这里就三块：一个常驻任务、一个 HTTP 事件回调（攒 body）、一组 JSON 取字段的函数。
 */
#include "weather_service.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>       /* mktime / struct tm：把日期算成星期几，见 wday_from_iso */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"

static const char *TAG = "weather";

/* ===================== 请求参数：改地点就改这两行 =====================
 * 硬编码一个地点（南京，NUIST 附近），和项目里其它"验证台"的做法一致 ——
 * 要换地方就改这两个常量，别的地方不用动。 */
#define WEATHER_LAT  "32.2"
#define WEATHER_LON  "118.7"

/* 一次拿全"当前 + 24 小时 + 7 日"。三个参数各有原因：
 *
 * - timezone=auto：让服务器按地点换算时区，直接返回**当地时间**字符串。
 *   于是本组件不用认识 time_service —— 小时/星期几全从返回值里解析出来。
 *
 * - forecast_hours=24：**必须有**。不带它，hourly 会按 forecast_days 给满 7 天
 *   （168 条），响应从 ~1.7KB 涨到 ~5.5KB。实测带它 = 1699 字节。
 *
 * - wind_speed_unit=kmh：把单位写成契约，不依赖服务端默认值。
 */
#define WEATHER_URL \
    "https://api.open-meteo.com/v1/forecast" \
    "?latitude=" WEATHER_LAT "&longitude=" WEATHER_LON \
    "&current=temperature_2m,relative_humidity_2m,apparent_temperature,weather_code,wind_speed_10m" \
    "&hourly=temperature_2m,weather_code" \
    "&daily=weather_code,temperature_2m_max,temperature_2m_min" \
    "&timezone=auto&forecast_days=7&forecast_hours=24&wind_speed_unit=kmh"

/* HTTP 总超时（DNS + TCP + TLS 握手 + 传输）。
 * 10s：对"连不上/超时"这种场景够快，又不至于把慢网络误杀。 */
#define WEATHER_HTTP_TIMEOUT_MS 10000

/* body 上限。实测这个查询约 1699 字节，给到 4096（~2.4x 裕量）。
 * ⚠️ 若哪天把 forecast_hours=24 去掉，响应会变 ~5.5KB 直接溢出；
 *    溢出时本次请求判失败，日志会打 overflow=1。 */
#define WEATHER_BODY_MAX 4096

/* ===================== 任务参数 =====================
 * 栈 8192：**和 IDF 官方 HTTPS 例程同值**
 *   examples/protocols/https_request/.../https_request_example_main.c:324  -> 8192
 *   examples/system/ota/advanced_https_ota/.../advanced_https_ota_example.c:279 -> 8192
 * 比项目里其它任务（4096）大一倍，因为 TLS 握手 + 证书链校验的栈开销大得多。
 *
 * 优先级 / 核：与 camera 的 sd_save / record 一致（3 / core 1）——
 * 网络慢一点没关系，别抢 core 0 上 USB 驱动的核。 */
#define WEATHER_TASK_STACK 8192
#define WEATHER_TASK_PRIO  3
#define WEATHER_TASK_CORE  1

static SemaphoreHandle_t s_req_sem  = NULL;
static QueueHandle_t     s_result_q = NULL;
static bool              s_inited   = false;

/* 结果结构体的"工作副本"。约 304 字节，**刻意放静态区**：
 * weather_task 的栈（8192）峰值被 TLS 握手 + 证书链校验占满，把这 304 字节
 * 跨 esp_http_client_perform() 放在栈上，就在最紧的地方吃掉余量 ——
 * 溢出是随机崩溃，极难复现。所以放静态区。
 * 只有 weather_task 一个任务读写它；对外投递时由队列拷贝走（投递后就与它无关）。 */
static weather_now_t s_report;

/* =====================================================================
 * HTTP 响应累积
 * 跑在调用 esp_http_client_perform() 的任务里（也就是 weather_task），
 * 不是事件循环任务 —— 但同样**不要**在这里碰 LVGL。
 * ===================================================================== */

typedef struct {
    char  *buf;
    size_t cap;
    size_t len;
    bool   overflow;    /* 超过 cap：本次判失败 */
} body_ctx_t;

static esp_err_t http_event_cb(esp_http_client_event_t *evt)
{
    body_ctx_t *ctx = (body_ctx_t *)evt->user_data;

    if (evt->event_id == HTTP_EVENT_ON_DATA && ctx != NULL) {
        if (ctx->len + (size_t)evt->data_len > ctx->cap) {
            ctx->overflow = true;       /* 继续收完，但结果作废 */
            return ESP_OK;
        }
        memcpy(ctx->buf + ctx->len, evt->data, (size_t)evt->data_len);
        ctx->len += (size_t)evt->data_len;
    }
    return ESP_OK;
}

/* =====================================================================
 * 从 ISO 字符串里取时间
 * ===================================================================== */

/* "2026-09-26T18:00" -> 18；格式不对返回 -1。
 * 只做纯字符串切片 —— 时区已经在服务器侧换算好了（timezone=auto）。 */
static int hour_from_iso(const char *s)
{
    if (s == NULL || strlen(s) < 13 || s[10] != 'T') {
        return -1;
    }
    if (s[11] < '0' || s[11] > '9' || s[12] < '0' || s[12] > '9') {
        return -1;
    }
    return (s[11] - '0') * 10 + (s[12] - '0');
}

/* "2026-09-26" -> 星期几（0=周日..6=周六）；失败返回 -1。
 *
 * 用 libc 的 mktime 算，**不碰 time_service** —— mktime 属于标准库，
 * 不是项目模块，所以不产生跨模块耦合。
 * 日期 -> 星期几与时区无关：我们给的是"该日历日的 00:00:00"，
 * mktime 归一化后 tm_wday 描述的就是这个日期（C 标准保证）。 */
static int wday_from_iso(const char *s)
{
    if (s == NULL || strlen(s) < 10) {
        return -1;
    }

    struct tm t = { 0 };
    t.tm_year  = atoi(s)     - 1900;    /* "2026-..."  atoi 到 '-' 停 */
    t.tm_mon   = atoi(s + 5) - 1;       /* "09-..."    -> 9 -> 8     */
    t.tm_mday  = atoi(s + 8);           /* "26[T]"     -> 26         */
    t.tm_isdst = 0;

    if (mktime(&t) == (time_t)-1) {
        return -1;
    }
    return t.tm_wday;
}

/* =====================================================================
 * JSON 解析
 *
 * 响应形如（字段已省略）：
 *   {"current":{"time":"2026-09-26T18:15","temperature_2m":22.9,
 *       "relative_humidity_2m":96,"apparent_temperature":26.6,
 *       "weather_code":95,"wind_speed_10m":9.3},
 *    "hourly":{"time":["2026-09-26T18:00",...],"temperature_2m":[...],"weather_code":[...]},
 *    "daily":{"time":["2026-09-26",...],"weather_code":[...],
 *       "temperature_2m_max":[...],"temperature_2m_min":[...]}}
 *
 * 失败策略（见头文件 weather_now_t 说明）：
 *   - current 全有或全无 —— 决定 valid。
 *   - hourly / daily 尽力而为 —— 返回实际条数，解析不动就变短，不影响 valid。
 * ===================================================================== */

/* 当前天气：五项全有且为数字才算成功，返回 1；否则 0 且不写入 r 的业务字段。 */
static int parse_current(const cJSON *root, weather_now_t *r)
{
    const cJSON *cur = cJSON_GetObjectItemCaseSensitive(root, "current");
    if (cur == NULL) {
        return 0;
    }

    const cJSON *t  = cJSON_GetObjectItemCaseSensitive(cur, "temperature_2m");
    const cJSON *h  = cJSON_GetObjectItemCaseSensitive(cur, "relative_humidity_2m");
    const cJSON *c  = cJSON_GetObjectItemCaseSensitive(cur, "weather_code");
    const cJSON *a  = cJSON_GetObjectItemCaseSensitive(cur, "apparent_temperature");
    const cJSON *w  = cJSON_GetObjectItemCaseSensitive(cur, "wind_speed_10m");
    const cJSON *ts = cJSON_GetObjectItemCaseSensitive(cur, "time");

    if (!cJSON_IsNumber(t) || !cJSON_IsNumber(h) || !cJSON_IsNumber(c) ||
        !cJSON_IsNumber(a) || !cJSON_IsNumber(w)) {
        return 0;
    }

    r->temp_c       = (float)t->valuedouble;
    r->humidity     = (int)  h->valuedouble;
    r->weather_code = (int)  c->valuedouble;
    r->apparent_c   = (float)a->valuedouble;
    r->wind_kmh     = (float)w->valuedouble;

    /* 观测时刻（地点本地时）。格式固定 "YYYY-MM-DDTHH:MM"，取不到就留 0 ——
     * 界面那边只在 cur_hour 非 0 时才显示 "Updated HH:MM"。 */
    if (cJSON_IsString(ts) && strlen(ts->valuestring) >= 16) {
        r->cur_hour = (uint8_t)((ts->valuestring[11] - '0') * 10 + (ts->valuestring[12] - '0'));
        r->cur_min  = (uint8_t)((ts->valuestring[14] - '0') * 10 + (ts->valuestring[15] - '0'));
    }
    return 1;
}

/* 逐小时：三个并行数组按 time[] 的长度对齐取。返回实际解析到的条数（0..24）。 */
static int parse_hours(const cJSON *hourly, weather_now_t *r)
{
    const cJSON *ta = cJSON_GetObjectItemCaseSensitive(hourly, "time");
    const cJSON *tt = cJSON_GetObjectItemCaseSensitive(hourly, "temperature_2m");
    const cJSON *tc = cJSON_GetObjectItemCaseSensitive(hourly, "weather_code");

    if (!cJSON_IsArray(ta) || !cJSON_IsArray(tt) || !cJSON_IsArray(tc)) {
        return 0;
    }

    int n = cJSON_GetArraySize(ta);
    if (n > WEATHER_HOURS_MAX) {
        n = WEATHER_HOURS_MAX;          /* forecast_hours=24 保证 n==24 */
    }

    int out = 0;
    for (int i = 0; i < n; i++) {
        const cJSON *e1 = cJSON_GetArrayItem(ta, i);
        const cJSON *e2 = cJSON_GetArrayItem(tt, i);
        const cJSON *e3 = cJSON_GetArrayItem(tc, i);
        if (!cJSON_IsString(e1) || !cJSON_IsNumber(e2) || !cJSON_IsNumber(e3)) {
            break;
        }
        const int hr = hour_from_iso(e1->valuestring);
        if (hr < 0) {
            break;
        }
        r->hours[out].hour   = (uint8_t)hr;
        r->hours[out].temp_c = (float)e2->valuedouble;
        r->hours[out].code   = (int8_t)e3->valuedouble;     /* WMO 码 0..99，int8 装得下 */
        out++;
    }
    return out;
}

/* 逐日：四个并行数组按 time[] 的长度对齐取。返回实际解析到的条数（0..7）。 */
static int parse_days(const cJSON *daily, weather_now_t *r)
{
    const cJSON *ta = cJSON_GetObjectItemCaseSensitive(daily, "time");
    const cJSON *tc = cJSON_GetObjectItemCaseSensitive(daily, "weather_code");
    const cJSON *tm = cJSON_GetObjectItemCaseSensitive(daily, "temperature_2m_max");
    const cJSON *tn = cJSON_GetObjectItemCaseSensitive(daily, "temperature_2m_min");

    if (!cJSON_IsArray(ta) || !cJSON_IsArray(tc) ||
        !cJSON_IsArray(tm) || !cJSON_IsArray(tn)) {
        return 0;
    }

    int n = cJSON_GetArraySize(ta);
    if (n > WEATHER_DAYS_MAX) {
        n = WEATHER_DAYS_MAX;
    }

    int out = 0;
    for (int i = 0; i < n; i++) {
        const cJSON *e1 = cJSON_GetArrayItem(ta, i);
        const cJSON *e2 = cJSON_GetArrayItem(tc, i);
        const cJSON *e3 = cJSON_GetArrayItem(tm, i);
        const cJSON *e4 = cJSON_GetArrayItem(tn, i);
        if (!cJSON_IsString(e1) || !cJSON_IsNumber(e2) ||
            !cJSON_IsNumber(e3) || !cJSON_IsNumber(e4)) {
            break;
        }
        const int wd = wday_from_iso(e1->valuestring);
        if (wd < 0) {
            break;
        }
        r->days[out].code   = (int8_t)e2->valuedouble;
        r->days[out].tmax_c = (float)e3->valuedouble;
        r->days[out].tmin_c = (float)e4->valuedouble;
        r->days[out].wday   = (uint8_t)wd;
        out++;
    }
    return out;
}

/* 编排三个解析器，并打一条可作为验收依据的汇总日志。
 * 返回 true = 当前天气可用（写入 r->valid 由调用方负责）。 */
static bool parse_report(const char *body, weather_now_t *r)
{
    cJSON *root = cJSON_Parse(body);
    if (root == NULL) {
        ESP_LOGW(TAG, "JSON parse failed (len=%u)", (unsigned)strlen(body));
        return false;
    }

    const bool ok = parse_current(root, r) != 0;
    if (ok) {
        const cJSON *hourly = cJSON_GetObjectItemCaseSensitive(root, "hourly");
        const cJSON *daily  = cJSON_GetObjectItemCaseSensitive(root, "daily");

        r->hour_count = (hourly != NULL) ? (uint8_t)parse_hours(hourly, r) : 0;
        r->day_count  = (daily  != NULL) ? (uint8_t)parse_days (daily,  r) : 0;

        ESP_LOGI(TAG, "parsed cur=%.1f app=%.1f wind=%.1f hum=%d wmo=%d | hrs=%d days=%d | len=%u",
                 r->temp_c, r->apparent_c, r->wind_kmh, r->humidity, r->weather_code,
                 r->hour_count, r->day_count, (unsigned)strlen(body));
    } else {
        ESP_LOGW(TAG, "current 缺失或非数字 (len=%u)", (unsigned)strlen(body));
    }

    cJSON_Delete(root);
    return ok;
}

/* =====================================================================
 * 工作任务
 * ===================================================================== */

/* 投一条结果。队列深度 1 + overwrite：App 没来得及取也不会堆积，永远是最后一次的。 */
static void post_result(const weather_now_t *r)
{
    if (s_result_q != NULL) {
        xQueueOverwrite(s_result_q, r);
    }
}

static void weather_task(void *arg)
{
    (void)arg;

    for (;;) {
        if (xSemaphoreTake(s_req_sem, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        /* 每轮先清零：valid=false、count=0，失败时也就投这个 */
        memset(&s_report, 0, sizeof(s_report));

        /* body 放堆（4KB）而不是栈：任务栈要留给 TLS 握手。 */
        char *body = malloc(WEATHER_BODY_MAX);
        if (body == NULL) {
            ESP_LOGE(TAG, "no mem for body");
            post_result(&s_report);
            continue;
        }

        body_ctx_t ctx = { .buf = body, .cap = WEATHER_BODY_MAX - 1 };

        const esp_http_client_config_t cfg = {
            .url               = WEATHER_URL,
            .method            = HTTP_METHOD_GET,
            .timeout_ms        = WEATHER_HTTP_TIMEOUT_MS,
            /* 用 IDF 自带的 Mozilla 根证书包（CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y）。
             * 证书校验需要正确的系统时间 —— 这就是为什么要先有 net_time(SNTP)。 */
            .crt_bundle_attach = esp_crt_bundle_attach,
            .event_handler     = http_event_cb,
            .user_data         = &ctx,
        };

        esp_http_client_handle_t cli = esp_http_client_init(&cfg);
        if (cli == NULL) {
            ESP_LOGE(TAG, "esp_http_client_init failed");
            free(body);
            post_result(&s_report);
            continue;
        }

        const esp_err_t err = esp_http_client_perform(cli);
        const int status    = esp_http_client_get_status_code(cli);
        esp_http_client_cleanup(cli);

        if (err == ESP_OK && status == 200 && !ctx.overflow) {
            body[ctx.len] = '\0';
            s_report.valid = parse_report(body, &s_report);
        } else {
            ESP_LOGW(TAG, "fetch failed: %s, http=%d, overflow=%d",
                     esp_err_to_name(err), status, (int)ctx.overflow);
        }

        free(body);
        post_result(&s_report);     /* 失败也投：否则 App 永远停在 Loading... */
    }
}

/* =====================================================================
 * 对外接口
 * ===================================================================== */

esp_err_t weather_service_init(void)
{
    if (s_inited) {
        return ESP_OK;                  /* 幂等 */
    }

    s_req_sem  = xSemaphoreCreateBinary();
    s_result_q = xQueueCreate(1, sizeof(weather_now_t));
    if (s_req_sem == NULL || s_result_q == NULL) {
        ESP_LOGE(TAG, "create sem/queue failed");
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreatePinnedToCore(weather_task, "weather", WEATHER_TASK_STACK, NULL,
                               WEATHER_TASK_PRIO, NULL, WEATHER_TASK_CORE) != pdPASS) {
        ESP_LOGE(TAG, "task create failed");
        return ESP_ERR_NO_MEM;
    }

    s_inited = true;
    ESP_LOGI(TAG, "ready (worker: %d bytes, core %d, report: %u bytes)",
             WEATHER_TASK_STACK, WEATHER_TASK_CORE, (unsigned)sizeof(weather_now_t));
    return ESP_OK;
}

esp_err_t weather_service_request(void)
{
    if (s_req_sem == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    /* 丢掉可能残留的旧结果（App 离开后没人取，队列里的值会一直留着）——
     * 不丢的话本次会先把上次的旧数据显示出来。 */
    if (s_result_q != NULL) {
        weather_now_t drop;
        while (xQueueReceive(s_result_q, &drop, 0) == pdTRUE) {
        }
    }

    xSemaphoreGive(s_req_sem);
    return ESP_OK;
}

bool weather_service_poll(weather_now_t *out)
{
    /* 无条件被 App 的定时器调用，所以 s_result_q 可能是 NULL（init 失败）——
     * xQueueReceive(NULL, ...) 会直接崩，必须挡住。 */
    if (out == NULL || s_result_q == NULL) {
        return false;
    }
    return xQueueReceive(s_result_q, out, 0) == pdTRUE;
}
