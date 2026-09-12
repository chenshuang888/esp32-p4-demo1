/*
 * UI 层 —— demo 界面
 * 用于验证整条链路：显示（LVGL 绘制）+ 触摸（按钮点击）
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 创建 demo 界面
 *
 * 必须在 ui_init() 成功之后调用。
 */
void ui_demo_start(void);

#ifdef __cplusplus
}
#endif
