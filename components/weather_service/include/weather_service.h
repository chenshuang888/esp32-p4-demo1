/*
 * weather_service —— 天气能力组件（HTTPS 拉 Open-Meteo）
 *
 * 定位：**"拉一次天气"这件事的封装**（当前 + 24 小时 + 7 日，一次请求拿全）。
 * 它不认识 LVGL、不认识 wifi_service、也不认识 time_service ——
 * 只对外给三个动作：初始化 / 请求一次 / 取结果。
 * （时区由服务器按 `timezone=auto` 换算，所以不需要 time_service；见 weather_now_t 的说明。）
 *
 * ⚠️ 为什么必须有自己的任务（这不是设计偏好，是硬约束）：
 *    渲染界面的 LVGL 任务栈只有 **7168 字节**（esp_lvgl_port 的
 *    ESP_LVGL_PORT_INIT_CONFIG() 默认值，见 components/ui/ui.c）。
 *    TLS 握手 + 证书链校验的栈开销远超它，把 esp_http_client 放进 LVGL 任务里
 *    轻则冻界面 1~5 秒，重则直接爆栈。
 *    所以本组件自带一个 8192 栈的常驻任务（与 IDF 官方 HTTPS 例程同值），
 *    LVGL 侧只发信号 + 收结果，全程不碰网络。
 *
 * 任务模型（镜像 camera.c 的 sd_save）：
 *      App: weather_service_request() ──give──> [二进制信号量]
 *                                                    ↓
 *                          weather_task: 等信号 -> HTTPS GET -> 解析 -> 投结果
 *                                                    ↓
 *      App: weather_service_poll()    <──take──  [结果队列，深度 1]
 *
 *      - 队列深度 1 + xQueueOverwrite：天然"丢旧保新"，App 来不及取也不会堆积
 *      - 常驻不销毁：它的生命周期 = weather_service_init()，与 App 的进出无关
 *      - 失败也会投一条结果（valid=false），否则 App 会永远停在 "Loading..."
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 预报条数上限（请求里用 forecast_hours=24 / forecast_days=7 固定住） */
#define WEATHER_HOURS_MAX  24
#define WEATHER_DAYS_MAX    7

/** @brief 逐小时预报的一条 */
typedef struct {
    float   temp_c;      /*!< 气温 °C */
    int8_t  code;        /*!< WMO 天气码 */
    uint8_t hour;        /*!< 0..23，**地点本地时**（请求带了 timezone=auto，服务器已换算） */
} weather_hour_t;

/** @brief 逐日预报的一条 */
typedef struct {
    float   tmax_c;      /*!< 当日最高 °C */
    float   tmin_c;      /*!< 当日最低 °C */
    int8_t  code;        /*!< WMO 天气码 */
    uint8_t wday;        /*!< 0=周日..6=周六（由日期经 libc mktime 算出，非项目 time_service） */
} weather_day_t;

/**
 * @brief 一次拉取的完整结果（当前 + 逐小时 + 逐日）
 *
 * 刻意**只给"界面够用"的原始数值**：天气码到文字/图标的映射属于表现层，由 App 负责 ——
 * 这样本组件换数据源时，App 的显示逻辑不受影响。
 *
 * ⚠️ 时间/时区**不是**本组件从 time_service 拿的：请求里带了 `timezone=auto`，
 *    服务器直接返回**地点当地时间**的字符串，组件只把小时/星期几解析成整数。
 *    所以本组件依然不认识 time_service（保持能力组件互不相识）。
 *
 * ⚠️ 这个结构体约 304 字节，**必须放在静态区**（weather_service.c 里的 s_report），
 *    不要放在工作任务的栈上 —— 那个栈的峰值被 TLS 握手占满（见 .c 顶部说明）。
 *
 * 字段语义：
 *  - `valid` 只由"当前天气"决定：任一项缺失/为 null → false（宁可整块不显示，
 *    也不显示半真半假的当前天气）。
 *  - `hour_count` / `day_count` 各自独立，是遍历 `hours[]` / `days[]` 的**唯一依据**；
 *    预报解析失败只是变短，不影响 `valid`。
 */
typedef struct {
    /* ---- 当前天气 ---- */
    float   temp_c;        /*!< 气温 °C */
    float   apparent_c;    /*!< 体感温度 °C */
    float   wind_kmh;      /*!< 风速 km/h */
    int     humidity;      /*!< 相对湿度 % */
    int     weather_code;  /*!< WMO 天气码（0=晴、3=阴、61=小雨……）；文字/图标由 App 映射 */
    uint8_t cur_hour;      /*!< 观测时刻的时（地点本地时），用于界面的 "Updated HH:MM" */
    uint8_t cur_min;       /*!< 观测时刻的分 */

    /* ---- 预报 ---- */
    uint8_t hour_count;    /*!< hours[] 里实际有效的条数（0..24） */
    uint8_t day_count;     /*!< days[]  里实际有效的条数（0..7）  */
    bool    valid;         /*!< false = 本次拉取失败（网络/HTTP/解析），其余字段无意义 */

    weather_hour_t hours[WEATHER_HOURS_MAX];
    weather_day_t  days [WEATHER_DAYS_MAX];
} weather_now_t;

/* 变大就是有意的了 —— 顺便提醒复查"放静态区、不放栈"这条约束 */
_Static_assert(sizeof(weather_now_t) <= 320, "weather_now_t 变大了，检查栈/队列假设");

/**
 * @brief 初始化：建信号量、结果队列和常驻工作任务
 *
 * **非阻塞**（只建任务，不发请求），幂等。由 main 在启动时调一次。
 *
 * @return ESP_OK         就绪（或已经就绪过）
 *         ESP_ERR_NO_MEM 建同步对象 / 建任务失败
 */
esp_err_t weather_service_init(void);

/**
 * @brief 请求拉取一次天气（**非阻塞**）
 *
 * 只是给工作任务发个信号，立刻返回。结果稍后由 weather_service_poll() 取。
 *
 * ⚠️ 会**先清空结果队列**再发请求 —— 否则"上次退出 App 时留下的旧结果"会被
 *    本次当成新数据先显示出来（App 离开后没人取结果，队列里的值会一直留着）。
 *
 * ⚠️ 本函数**不检查网络是否可用**（本组件不认识 wifi_service）。没联网时
 *    请求照样发，只是结果会是 valid=false。想给更准的提示由 App 先自查。
 *
 * @return ESP_OK              已发起
 *         ESP_ERR_INVALID_STATE init 没成功过
 */
esp_err_t weather_service_request(void);

/**
 * @brief 取一次结果（**非阻塞**，取走即清）
 *
 * 供 App 的 lv_timer 周期调用。
 *
 * @param[out] out 结果。**只在返回 true 时被写入**
 * @return true  这次拿到了一条新结果（可能是 valid=false 的失败结果）
 *         false 还没有结果，继续等
 */
bool weather_service_poll(weather_now_t *out);

#ifdef __cplusplus
}
#endif
