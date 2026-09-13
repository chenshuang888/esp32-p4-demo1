/*
 * 桌面 —— 多应用的主页
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 把桌面注册给 app_manager（必须第一个注册，它是主页） */
void desktop_register(void);

#ifdef __cplusplus
}
#endif
