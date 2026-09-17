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
#define PICTURE_ICON_CLOCK  "clock"
#define PICTURE_ICON_DEMO   "demo"
#define PICTURE_ICON_CAMERA "camera"

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
