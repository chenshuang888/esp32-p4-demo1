/*
 * 天气 App —— 手机风格的完整天气页
 *
 * 链路：进 App → weather_service 发一次 HTTPS 请求（Open-Meteo）→ 解析 → 上屏。
 * 界面分两栏（屏幕 1024x600，内容区 552px 高，单栏竖排塞不下）：
 *   左：地点 + hero（大图标 + 大温度）+ 天气文字 + 体感/湿度/风
 *   右：24 小时横滑条 + 7 日预报
 *
 * ⚠️ 本 App **一行网络代码都没有**，那部分全在 weather_service 的常驻任务里
 *    （原因见 weather_service.h：LVGL 任务的栈放不下 TLS）。这里只做两件事：
 *    **发请求**（weather_service_request，非阻塞）和 **用 lv_timer 周期取结果**
 *    （weather_service_poll）—— 和 camera.c 的 on_refresh 收结果是同一个模式。
 *
 * ⚠️ 时区**不依赖 time_service**：请求带了 timezone=auto，服务器返回的就是
 *    地点当地时间。小时数、星期几、「Updated HH:MM」全从返回值里来 ——
 *    所以本 App 比原来**少一条**跨模块依赖（原来 include 了 time_service.h）。
 *
 * ⚠️ 界面文案全用 ASCII：项目没有中文字体，中文会渲染成占位方块。
 *
 * ⚠️ 字体：全局默认是 montserrat_18（不显式设字体就继承它）。这里**首次**显式
 *    给大温度设 montserrat_48 —— 8~48 全档位已在 sdkconfig.defaults 里开满。
 */
#include "weather.h"

#include <stdio.h>      /* snprintf */

#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"

#include "app_manager.h"
#include "picture.h"
#include "ui_status_bar.h"
#include "weather_service.h"
#include "wifi_service.h"

static const char *TAG = "weather_app";

/* 取结果的轮询周期。一次请求实测 1~3 秒，500ms 足够跟手又不浪费。 */
#define POLL_MS 500

/* 内容区几何（见上面对两栏布局的说明） */
#define CONTENT_PAD_TOP  (UI_STATUS_BAR_HEIGHT + 12)
#define LEFT_COL_W       384
#define HOUR_BOX_H       110
#define HOUR_CELL_W       70
#define HERO_ICON_PX      96
#define CELL_ICON_PX      32
#define DAY_WDAY_W       140
#define DAY_TEMP_W        70

static lv_obj_t   *s_scr      = NULL;
static lv_obj_t   *s_icon     = NULL;   /* hero 大图标 */
static lv_obj_t   *s_temp     = NULL;
static lv_obj_t   *s_cond     = NULL;
static lv_obj_t   *s_feels    = NULL;
static lv_obj_t   *s_humid    = NULL;
static lv_obj_t   *s_wind     = NULL;
static lv_obj_t   *s_status   = NULL;
static lv_obj_t   *s_hour_box = NULL;   /* 24 小时条（刷新时整体重建子项） */
static lv_obj_t   *s_day_box  = NULL;   /* 7 日列表（刷新时整体重建子项） */
static lv_timer_t *s_timer    = NULL;

/* WMO 天气码 → 短文字（Open-Meteo 用的就是这套编码）。
 * 码本身是"数据"，映射成文字是表现层的事，所以放在 App 而不是组件里。 */
static const char *wmo_desc(int code)
{
    switch (code) {
    case 0:  return "Clear sky";
    case 1:  return "Mainly clear";
    case 2:  return "Partly cloudy";
    case 3:  return "Overcast";
    case 45: return "Fog";
    case 48: return "Rime fog";
    case 51: return "Light drizzle";
    case 53: return "Drizzle";
    case 55: return "Dense drizzle";
    case 56: return "Light freezing drizzle";
    case 57: return "Freezing drizzle";
    case 61: return "Slight rain";
    case 63: return "Rain";
    case 65: return "Heavy rain";
    case 66: return "Light freezing rain";
    case 67: return "Freezing rain";
    case 71: return "Slight snow";
    case 73: return "Snow";
    case 75: return "Heavy snow";
    case 77: return "Snow grains";
    case 80: return "Light rain showers";
    case 81: return "Rain showers";
    case 82: return "Violent rain showers";
    case 85: return "Snow showers";
    case 86: return "Heavy snow showers";
    case 95: return "Thunderstorm";
    case 96: return "Thunderstorm, slight hail";
    case 99: return "Thunderstorm, heavy hail";
    default: return "Unknown";
    }
}

/*
 * WMO 码 → 图标 id。把 28 个码归成 12 类（每类在 picture.h 里有 96px 和 32px 两个 id）。
 *
 * 归类取舍（依据"Open-Meteo 实际会返回什么" + 用户最在意雨的细分度）：
 *   - 雨按强度**分三档**：小雨(61) / 中雨(63) / 大雨(65) —— 这是刻意要的细分。
 *   - 毛毛雨(51/53/55) 并入"小雨"：源图那套图标里没有"毛毛雨"。
 *   - 阵雨(80/81/82) 只给一张：源图没有阵雨的强度分级。
 *   - 雪不分强度（71/73/75/77 合成一张"小雪"）—— 用户说其它类型不必细分。
 * 查不到的码返回 NULL —— 调用方按"没图也占位"处理，不崩、不塌布局。
 * 这里的"码 → 类"是表现层的归类，所以放在 App 而不是 picture 组件里。
 */
static const char *wmo_icon_id(int code, bool small)
{
    switch (code) {
    case 0:  case 1:
        return small ? PICTURE_ICON_WMO_CLEAR_S         : PICTURE_ICON_WMO_CLEAR;
    case 2:
        return small ? PICTURE_ICON_WMO_PARTLY_S        : PICTURE_ICON_WMO_PARTLY;
    case 3:
        return small ? PICTURE_ICON_WMO_OVERCAST_S      : PICTURE_ICON_WMO_OVERCAST;
    case 45: case 48:
        return small ? PICTURE_ICON_WMO_FOG_S           : PICTURE_ICON_WMO_FOG;
    case 51: case 53: case 55: case 61:
        return small ? PICTURE_ICON_WMO_LIGHT_RAIN_S    : PICTURE_ICON_WMO_LIGHT_RAIN;
    case 63:
        return small ? PICTURE_ICON_WMO_MODERATE_RAIN_S : PICTURE_ICON_WMO_MODERATE_RAIN;
    case 65:
        return small ? PICTURE_ICON_WMO_HEAVY_RAIN_S    : PICTURE_ICON_WMO_HEAVY_RAIN;
    case 56: case 57: case 66: case 67:
        return small ? PICTURE_ICON_WMO_FREEZING_S      : PICTURE_ICON_WMO_FREEZING;
    case 71: case 73: case 75: case 77:
        return small ? PICTURE_ICON_WMO_LIGHT_SNOW_S    : PICTURE_ICON_WMO_LIGHT_SNOW;
    case 80: case 81: case 82:
        return small ? PICTURE_ICON_WMO_SHOWERS_S       : PICTURE_ICON_WMO_SHOWERS;
    case 85: case 86:
        return small ? PICTURE_ICON_WMO_SNOW_SHOWERS_S  : PICTURE_ICON_WMO_SNOW_SHOWERS;
    case 95: case 96: case 99:
        return small ? PICTURE_ICON_WMO_THUNDER_S       : PICTURE_ICON_WMO_THUNDER;
    default:
        return NULL;
    }
}

/* 给 image 挂图标。查不到（模组未接入 / 未知码）就按 px 占位、不画图 ——
 * 布局不塌陷也不会崩（lv_image 没有 src 时不绘制，是安全的）。
 * 注意 source 图标是 96px / 32px 两套原生资源，这里不做缩放。 */
static void icon_apply(lv_obj_t *img, int code, int px, bool small)
{
    const char *id = wmo_icon_id(code, small);
    const lv_image_dsc_t *dsc = (id != NULL) ? picture_icon(id) : NULL;

    if (dsc == NULL) {
        lv_obj_set_size(img, px, px);
        return;
    }
    lv_image_set_src(img, dsc);
}

/* 浮点 → 标签。**不能**用 lv_label_set_text_fmt 的 %f：
 * CONFIG_LV_USE_FLOAT 没开，LVGL 自己的格式化会输出乱码。
 * 所以浮点一律先走标准 snprintf 转成字符串。 */
static void set_float_label(lv_obj_t *lbl, float v, int decimals)
{
    char buf[16];
    snprintf(buf, sizeof(buf), "%.*f", decimals, (double)v);
    lv_label_set_text(lbl, buf);
}

static const char *wday_name(int wd)
{
    static const char *S_WDAY[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    return (wd >= 0 && wd < 7) ? S_WDAY[wd] : "-";
}

/* 指标列：上面是数值，下面是标签（体感/湿度/风）。 */
static void metric_create(lv_obj_t *parent, const char *label, lv_obj_t **out_value)
{
    lv_obj_t *col = lv_obj_create(parent);
    lv_obj_remove_style_all(col);
    lv_obj_set_size(col, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(col, 1);
    lv_obj_remove_flag(col, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(col, 2, 0);

    lv_obj_t *v = lv_label_create(col);
    lv_obj_set_style_text_font(v, &lv_font_montserrat_20, 0);
    *out_value = v;

    lv_obj_t *l = lv_label_create(col);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
    lv_label_set_text(l, label);
}

/* 小时条的一格：小时 / 小图标 / 温度。宽度固定，纵向排列。 */
static void hour_cell_create(lv_obj_t *parent, const weather_hour_t *h, bool is_now)
{
    lv_obj_t *cell = lv_obj_create(parent);
    lv_obj_remove_style_all(cell);
    lv_obj_set_size(cell, HOUR_CELL_W, lv_pct(100));
    lv_obj_remove_flag(cell, LV_OBJ_FLAG_SCROLLABLE);   /* 别把横滑的拖拽吃掉 */
    lv_obj_set_flex_flow(cell, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(cell, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(cell, 4, 0);

    lv_obj_t *t = lv_label_create(cell);
    if (is_now) {
        lv_label_set_text(t, "Now");        /* 第一格就是当前小时 */
    } else {
        lv_label_set_text_fmt(t, "%02u", (unsigned)h->hour);
    }

    lv_obj_t *img = lv_image_create(cell);
    icon_apply(img, h->code, CELL_ICON_PX, true);

    lv_obj_t *tp = lv_label_create(cell);
    set_float_label(tp, h->temp_c, 0);      /* 列表里用整数，和手机一致 */
}

/* 7 日列表的一行：星期 / 小图标 / 最低 / 最高（最低最高推到右侧）。 */
static void day_row_create(lv_obj_t *parent, const weather_day_t *d, bool is_today)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_flex_grow(row, 1);           /* 7 行等分可用高度 */
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 12, 0);

    lv_obj_t *wd = lv_label_create(row);
    lv_obj_set_width(wd, DAY_WDAY_W);
    lv_label_set_text(wd, is_today ? "Today" : wday_name(d->wday));

    lv_obj_t *img = lv_image_create(row);
    icon_apply(img, d->code, CELL_ICON_PX, true);

    /* 撑开的空对象，把后面两个温度推到右边（remove_style_all 后不可见） */
    lv_obj_t *spacer = lv_obj_create(row);
    lv_obj_remove_style_all(spacer);
    lv_obj_set_height(spacer, 1);
    lv_obj_set_flex_grow(spacer, 1);
    lv_obj_remove_flag(spacer, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *mn = lv_label_create(row);
    lv_obj_set_width(mn, DAY_TEMP_W);
    lv_obj_set_style_text_align(mn, LV_TEXT_ALIGN_RIGHT, 0);
    set_float_label(mn, d->tmin_c, 0);

    lv_obj_t *mx = lv_label_create(row);
    lv_obj_set_width(mx, DAY_TEMP_W);
    lv_obj_set_style_text_align(mx, LV_TEXT_ALIGN_RIGHT, 0);
    set_float_label(mx, d->tmax_c, 0);
}

/* 只重建两个预报容器：hero 与指标是原地更新，大温度不会闪。
 * 刷新是用户触发的（进 App / 点 Refresh），不是周期性的，重建 ~124 个对象只要几 ms。 */
static void rebuild_forecasts(const weather_now_t *w)
{
    lv_obj_clean(s_hour_box);           /* 只删子对象，容器自身与样式保留 */
    for (int i = 0; i < w->hour_count; i++) {
        hour_cell_create(s_hour_box, &w->hours[i], i == 0);
    }

    lv_obj_clean(s_day_box);
    for (int i = 0; i < w->day_count; i++) {
        day_row_create(s_day_box, &w->days[i], i == 0);
    }
}

/*
 * 发起一次拉取。
 *
 * ⚠️ 网络是否可用**由本 App 判断**，不是 weather_service —— 组件刻意不认识
 *    wifi_service（能力组件之间不互相认识）。在这里判断能给出更准的提示：
 *    "没配过 WiFi" 和 "连上了但请求失败" 是两回事，分开说对排查更有用。
 */
static void request(void)
{
    if (!wifi_service_is_connected()) {
        lv_label_set_text(s_status, "No WiFi (configure in Settings)");
        return;
    }
    if (weather_service_request() != ESP_OK) {
        /* 只可能是 init 失败（内存不足），此时永远不会有结果回来 */
        lv_label_set_text(s_status, "Weather service unavailable");
        return;
    }
    lv_label_set_text(s_status, "Loading...");
}

/* Refresh 按钮。LVGL 9 的事件回调签名是 void(*)(lv_event_t *)，不是 (lv_obj_t *) */
static void on_refresh(lv_event_t *e)
{
    (void)e;
    request();
}

/* 每 POLL_MS 看一眼有没有结果 —— 跑在 LVGL 任务里，锁已持有，不用再加锁 */
static void on_poll(lv_timer_t *timer)
{
    (void)timer;

    weather_now_t w;
    if (!weather_service_poll(&w)) {
        return;                     /* 还没结果，继续等 */
    }

    if (!w.valid) {
        /* 失败时**保留上一次的好数据**，只改状态行 —— 免得整页突然变空白 */
        lv_label_set_text(s_status, "Unavailable (network / HTTP)");
        return;
    }

    set_float_label(s_temp, w.temp_c, 1);
    lv_label_set_text(s_cond, wmo_desc(w.weather_code));
    icon_apply(s_icon, w.weather_code, HERO_ICON_PX, false);

    set_float_label(s_feels, w.apparent_c, 1);
    lv_label_set_text_fmt(s_humid, "%d%%", w.humidity);
    set_float_label(s_wind, w.wind_kmh, 1);

    rebuild_forecasts(&w);

    /* 观测时刻用**服务器给的当地时间**（timezone=auto），不是本机时钟 */
    lv_label_set_text_fmt(s_status, "Updated %02u:%02u",
                          (unsigned)w.cur_hour, (unsigned)w.cur_min);
}

static void weather_enter(void)
{
    ESP_LOGI(TAG, "enter");

    lvgl_port_lock(0);

    s_scr = lv_obj_create(NULL);

    /* 标题与"回桌面"按钮都由公共状态栏提供，本 App 不自己画 */
    const ui_status_bar_cfg_t bar = {
        .title     = "Weather",
        .show_back = true,
    };
    ui_status_bar_apply(&bar);

    /* body：横向两栏。⚠️ remove_style_all 会清掉 layout，所以 flex 必须在它之后设。 */
    lv_obj_t *body = lv_obj_create(s_scr);
    lv_obj_remove_style_all(body);
    lv_obj_set_size(body, lv_pct(100), lv_pct(100));
    lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_top(body, CONTENT_PAD_TOP, 0);
    lv_obj_set_style_pad_bottom(body, 12, 0);
    lv_obj_set_style_pad_hor(body, 16, 0);
    lv_obj_set_style_pad_column(body, 16, 0);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_ROW);

    /* ---------------- 左栏：当前天气 ---------------- */
    lv_obj_t *left = lv_obj_create(body);
    lv_obj_remove_style_all(left);
    lv_obj_set_size(left, LEFT_COL_W, lv_pct(100));
    lv_obj_remove_flag(left, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(left, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(left, 10, 0);

    lv_obj_t *loc = lv_label_create(left);
    lv_obj_set_style_text_font(loc, &lv_font_montserrat_22, 0);
    lv_label_set_text(loc, "Nanjing");

    /* hero：大图标 + 大温度（温度单位用稍小的字号，和手机一致） */
    lv_obj_t *hero = lv_obj_create(left);
    lv_obj_remove_style_all(hero);
    lv_obj_set_size(hero, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_remove_flag(hero, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(hero, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hero, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(hero, 12, 0);

    s_icon = lv_image_create(hero);
    lv_obj_set_size(s_icon, HERO_ICON_PX, HERO_ICON_PX);

    lv_obj_t *tempgrp = lv_obj_create(hero);
    lv_obj_remove_style_all(tempgrp);
    lv_obj_set_size(tempgrp, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_remove_flag(tempgrp, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(tempgrp, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(tempgrp, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_column(tempgrp, 4, 0);

    s_temp = lv_label_create(tempgrp);
    lv_obj_set_style_text_font(s_temp, &lv_font_montserrat_48, 0);

    lv_obj_t *unit = lv_label_create(tempgrp);
    lv_obj_set_style_text_font(unit, &lv_font_montserrat_24, 0);
    lv_label_set_text(unit, "C");

    s_cond = lv_label_create(left);
    lv_obj_set_style_text_font(s_cond, &lv_font_montserrat_20, 0);

    /* 指标行：体感 / 湿度 / 风 */
    lv_obj_t *metrics = lv_obj_create(left);
    lv_obj_remove_style_all(metrics);
    lv_obj_set_size(metrics, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_remove_flag(metrics, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(metrics, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(metrics, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    metric_create(metrics, "Feels like", &s_feels);
    metric_create(metrics, "Humidity",   &s_humid);
    metric_create(metrics, "Wind km/h",  &s_wind);

    s_status = lv_label_create(left);
    lv_obj_set_style_text_font(s_status, &lv_font_montserrat_14, 0);

    lv_obj_t *btn = lv_button_create(left);
    lv_obj_set_size(btn, 120, 40);
    lv_obj_add_event_cb(btn, on_refresh, LV_EVENT_CLICKED, NULL);
    lv_obj_t *btn_lb = lv_label_create(btn);
    lv_label_set_text(btn_lb, "Refresh");
    lv_obj_center(btn_lb);

    /* ---------------- 右栏：预报 ---------------- */
    lv_obj_t *right = lv_obj_create(body);
    lv_obj_remove_style_all(right);
    lv_obj_set_height(right, lv_pct(100));
    lv_obj_set_flex_grow(right, 1);
    lv_obj_remove_flag(right, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(right, 6, 0);

    lv_obj_t *h1 = lv_label_create(right);
    lv_obj_set_style_text_font(h1, &lv_font_montserrat_16, 0);
    lv_label_set_text(h1, "Next 24 hours");

    /* ⚠️ 24 小时横滑条。三处都是"静默失败"型陷阱：
     *   1) 宽度**必须**是确定的（这里 lv_pct(100)）。用 LV_SIZE_CONTENT 时容器随
     *      子项长大，就没有可滚区，横滑会静默失效。
     *   2) flex 保持默认的 NOWRAP —— 子项溢出才形成滚动区，别设成 WRAP。
     *   3) flex 必须在 remove_style_all 之后设（它会清掉 layout）。 */
    s_hour_box = lv_obj_create(right);
    lv_obj_remove_style_all(s_hour_box);
    lv_obj_set_size(s_hour_box, lv_pct(100), HOUR_BOX_H);
    lv_obj_set_flex_flow(s_hour_box, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_hour_box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_hour_box, 4, 0);
    lv_obj_set_scroll_dir(s_hour_box, LV_DIR_HOR);      /* 只允许横向（禁纵向） */
    lv_obj_set_scrollbar_mode(s_hour_box, LV_SCROLLBAR_MODE_OFF);

    lv_obj_t *h2 = lv_label_create(right);
    lv_obj_set_style_text_font(h2, &lv_font_montserrat_16, 0);
    lv_label_set_text(h2, "7-day forecast");

    s_day_box = lv_obj_create(right);
    lv_obj_remove_style_all(s_day_box);
    lv_obj_set_width(s_day_box, lv_pct(100));
    lv_obj_set_flex_grow(s_day_box, 1);         /* 占满右栏剩余高度，7 行等分 */
    lv_obj_set_flex_flow(s_day_box, LV_FLEX_FLOW_COLUMN);

    /* 先放占位文字，避免第一个结果回来之前是空白 */
    lv_label_set_text(s_temp,  "--.-");
    lv_label_set_text(s_cond,  "-");
    lv_label_set_text(s_feels, "--");
    lv_label_set_text(s_humid, "--%");
    lv_label_set_text(s_wind,  "--");
    lv_label_set_text(s_status, "");

    /* ⚠️ 定时器不属于 s_scr，leave 里必须显式删（clock.c 那条纪律） */
    s_timer = lv_timer_create(on_poll, POLL_MS, NULL);

    /* 进 App 就拉一次（非阻塞） */
    request();

    lv_screen_load(s_scr);
    lvgl_port_unlock();
}

static void weather_leave(void)
{
    ESP_LOGI(TAG, "leave");

    lvgl_port_lock(0);

    /* ⚠️ 必须先删定时器：它不属于 s_scr，删 screen 不会带走它。
     *    lv_timer_create 可能返回 NULL，而 lv_timer_delete 不判空，所以要判一下。 */
    if (s_timer != NULL) {
        lv_timer_delete(s_timer);
        s_timer = NULL;
    }

    /* 常驻的 weather_task **不在这里销毁** —— 它的生命周期跟
     * weather_service_init()，与 App 进出无关（同 camera 的 sd_save/record）。 */

    lv_obj_delete_async(s_scr);
    s_scr = NULL;
    s_icon = s_temp = s_cond = NULL;
    s_feels = s_humid = s_wind = NULL;
    s_status = NULL;
    s_hour_box = s_day_box = NULL;

    lvgl_port_unlock();
}

static const app_desc_t s_desc = {
    .name  = "Weather",
    .icon  = PICTURE_ICON_WEATHER,
    .enter = weather_enter,
    .leave = weather_leave,
};

void weather_register(void)
{
    app_manager_register(&s_desc);
}
