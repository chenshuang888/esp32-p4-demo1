/*
 * font_cjk —— 中文（CJK）字体资源 + 接入
 *
 * 提供一套《通用规范汉字表》全表 **8105 字**的 LVGL 位图字体，并把它**接成全局默认**：
 * 英文/数字仍走项目原本的默认字体（Montserrat 18），中文靠 LVGL 的字体
 * fallback 落到思源黑体。所以中英文可以混在同一句话里。
 *
 * 用法（必须在 ui_init() 之后 —— 那时 LVGL 的 display 才存在）：
 *     ESP_ERROR_CHECK(font_cjk_install());
 *
 * ⚠️ 依赖 sdkconfig.defaults 里的两个开关（都已开）：
 *      CONFIG_LV_USE_FONT_COMPRESSED=y   字体是 RLE 压缩格式，不开就"中文一片空白"
 *      CONFIG_LV_FONT_FMT_TXT_LARGE=y    位图索引放宽到 32 位，换字号/字表不用再动配置
 *    为什么、以及踩过的两个坑（样式优先级、压缩位图），都写在 font_cjk.c 顶部。
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 把中文字体接成全局默认（英文保持原样，中文走 fallback）
 *
 * 做三件事：
 *   1) 装配"原默认字体 + 中文 fallback"的混排字体
 *   2) 装一条主题链，让**将来**每个新建的根对象（= 每个 App 的 screen）自动带上它
 *   3) 补上**已经存在**的根对象（lv_layer_top / lv_layer_bottom / 当前 screen）
 *
 * 第 3 步不能省：lv_layer_top() 在 lv_init() 阶段就建好了，主题是在那之前应用的，
 * 吃不到第 2 步的新主题 —— 而状态栏正好挂在 lv_layer_top() 上。
 *
 * 前置条件：ui_init() 已完成（display 必须已存在）。
 *
 * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 还没有 display；ESP_ERR_NO_MEM 建主题失败
 */
esp_err_t font_cjk_install(void);

#ifdef __cplusplus
}
#endif
