/*
 * 图片资源 —— 编进固件的静态图片
 *
 * 这个组件只干一件事：**把图片数据收在这里，对外给一个"取图"的入口**。
 * 它不认识屏幕、不认识 App、也不管图该怎么摆 —— 那是 ui / apps 的事。
 *
 * 为什么它可以暴露 LVGL 类型（lv_image_dsc_t），而 app_manager 不行？
 * 因为角色不同：app_manager 是"框架"，要求它能不认识任何具体技术；
 * 而这里是"资源仓库"，它的内容本身就是 LVGL 数据。
 */
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 桌面壁纸（1024x600，RGB565）
 *
 * 返回的是编译进固件的静态图片描述符：
 *   - 永远非 NULL，调用方不需要判空；
 *   - 不需要释放，生命周期等于整个程序；
 *   - 尺寸和屏幕完全一致，所以拿它当背景图时不用管平铺和对齐。
 *
 * 数据本身在 images/picture_wallpaper_data.c，由 LVGLImage.py 生成，别手改。
 */
const lv_image_dsc_t *picture_wallpaper(void);

/* ===================== 图标 ===================== */

/*
 * 图标 id。App 在 app_desc_t.icon 里填这些常量，而不是手写字符串 ——
 * 拼错会**直接编译报错**，而不是运行起来静默查不到、图标不显示。
 *
 * 注意这些常量只是"不透明标识符"：app_manager 只负责原样保管那个字符串，
 * 不解释它，也不知道它对应一张图。翻译成图片是 picture_icon() 的事。
 */
#define PICTURE_ICON_CLOCK    "clock"
#define PICTURE_ICON_DEMO     "demo"
#define PICTURE_ICON_CAMERA   "camera"
#define PICTURE_ICON_PHOTO    "photo"
#define PICTURE_ICON_SETTINGS "settings"
#define PICTURE_ICON_WEATHER  "weather"

/* ===================== 天气条件图标（天气 App 用）=====================
 * 把 WMO 天气码归成 12 类，每类两档尺寸：
 *   wmo_xxx    —— 96x96，天气 App 的 hero 大图标
 *   wmo_xxx_s  —— 32x32，小时条 / 7 日列表里的小图标
 *
 * 源图是"中国气象局标准天气图标"（白图标 + 渐变底 + 中文标签的**拼图**，25 个）。
 * 由 原始图片/make_weather_icons.py 切片 + 抠 alpha + 重着色成深色（浅色主题下才看得见），
 * 再用 LVGL 自带的 LVGLImage.py 转成 images/ 下的 .c —— 完整流程见那个脚本的顶部注释。
 *
 * 为什么只用 12 类、而不是源图里全部的 25 个：另外 13 个
 * （暴雨/大暴雨/特大暴雨、中雪/大雪/暴雪、雨夹雪、雷阵雨伴有冰雹、
 *   沙尘暴/强沙尘暴/浮尘/扬沙、霜）**Open-Meteo 的 weather_code 根本不会返回**。
 * 归类依据（哪个码归哪类）见 apps/weather.c 的 wmo_icon_id()。
 *
 * 名称里刻意带 wmo_ 前缀，和桌面 App 图标（clock/weather/...）区分开。 */
#define PICTURE_ICON_WMO_CLEAR         "wmo_clear"
#define PICTURE_ICON_WMO_PARTLY        "wmo_partly_cloudy"
#define PICTURE_ICON_WMO_OVERCAST      "wmo_overcast"
#define PICTURE_ICON_WMO_FOG           "wmo_fog"
#define PICTURE_ICON_WMO_FREEZING      "wmo_freezing"
#define PICTURE_ICON_WMO_LIGHT_RAIN    "wmo_light_rain"
#define PICTURE_ICON_WMO_MODERATE_RAIN "wmo_moderate_rain"
#define PICTURE_ICON_WMO_HEAVY_RAIN    "wmo_heavy_rain"
#define PICTURE_ICON_WMO_SHOWERS       "wmo_showers"
#define PICTURE_ICON_WMO_THUNDER       "wmo_thunder"
#define PICTURE_ICON_WMO_LIGHT_SNOW    "wmo_light_snow"
#define PICTURE_ICON_WMO_SNOW_SHOWERS  "wmo_snow_showers"

#define PICTURE_ICON_WMO_CLEAR_S         "wmo_clear_s"
#define PICTURE_ICON_WMO_PARTLY_S        "wmo_partly_cloudy_s"
#define PICTURE_ICON_WMO_OVERCAST_S      "wmo_overcast_s"
#define PICTURE_ICON_WMO_FOG_S           "wmo_fog_s"
#define PICTURE_ICON_WMO_FREEZING_S      "wmo_freezing_s"
#define PICTURE_ICON_WMO_LIGHT_RAIN_S    "wmo_light_rain_s"
#define PICTURE_ICON_WMO_MODERATE_RAIN_S "wmo_moderate_rain_s"
#define PICTURE_ICON_WMO_HEAVY_RAIN_S    "wmo_heavy_rain_s"
#define PICTURE_ICON_WMO_SHOWERS_S       "wmo_showers_s"
#define PICTURE_ICON_WMO_THUNDER_S       "wmo_thunder_s"
#define PICTURE_ICON_WMO_LIGHT_SNOW_S    "wmo_light_snow_s"
#define PICTURE_ICON_WMO_SNOW_SHOWERS_S  "wmo_snow_showers_s"

/**
 * @brief 按 id 取图标（96x96，RGB565A8，带透明）
 *
 * @param id  PICTURE_ICON_* 之一
 * @return 图片描述符；**查不到返回 NULL**
 *
 * 和 picture_wallpaper() 的区别就在这里：壁纸是固定一张、永远非 NULL，
 * 而这里是查表，**调用方必须处理 NULL**（通常就是"只显示名字，不显示图标"）。
 */
const lv_image_dsc_t *picture_icon(const char *id);

#ifdef __cplusplus
}
#endif
