/*
 * app_manager —— 多应用调度器（纯框架）
 *
 * 职责：维护一张 App 注册表，提供 注册 / 枚举 / 启动 / 回主页 的能力。
 *      它只知道"有个 enter/leave 回调要调"，完全不知道 LVGL 是什么，
 *      也不知道具体有哪些 App —— App 是把自己的描述注册进来的。
 *
 * 约定（重要，别忘）：
 *  1. 第 0 个注册的 App 视为主页（桌面），app_manager_go_home() 会切回它。
 *  2. register / count / get 随时可调（例如 app_main 里、ui_init 之前）。
 *     launch / go_home 必须在 LVGL 初始化之后调用。本组件自身完全不碰 LVGL，
 *     只是回调各 App 的 enter/leave —— **由各 App 自己负责加 LVGL 锁**
 *     （lvgl_port_lock 是递归锁，所以无论从哪个上下文调用都不会死锁）。
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 最多能注册多少个 App */
#define APP_MANAGER_MAX_APPS  16

/**
 * @brief 一个 App 的描述信息（由各 App 自己填充并注册给框架）
 *
 * 生命周期要求：该结构体必须是 static const（通常定义在 App 自己的 .c 文件里），
 *              框架只保存指针，不拷贝。
 */
typedef struct app_desc {
    const char *name;       /*!< App 名称，桌面显示用 */
    const char *icon;       /*!< 图标，暂未使用，先留空 */
    void (*enter)(void);    /*!< 进入本 App：创建并加载自己的 screen */
    void (*leave)(void);    /*!< 离开本 App：释放自己的资源，不需要可为 NULL。
                                 ⚠️ 若在这里删自己的 screen，必须用 lv_obj_delete_async()，
                                    不要直接 lv_obj_delete() —— 该回调可能正在处理
                                    本 screen 内对象的事件，直接删会导致 LVGL 崩溃。 */
} app_desc_t;

/**
 * @brief 注册一个 App
 *
 * @param desc 指向 static const 的描述结构体
 * @return ESP_OK；ESP_ERR_INVALID_ARG 参数为空；ESP_ERR_NO_MEM 表已满
 */
esp_err_t app_manager_register(const app_desc_t *desc);

/** @brief 已注册的 App 数量 */
int app_manager_count(void);

/**
 * @brief 取第 index 个 App 的描述
 *
 * @return 描述指针；index 越界返回 NULL
 */
const app_desc_t *app_manager_get(int index);

/**
 * @brief 启动第 index 个 App
 *
 * 切换顺序为 enter(新) -> leave(旧)，这样新 screen 先就位，切换过程中不会出现
 * "没有有效 screen"的空窗。若 index 就是当前 App，则什么都不做。
 *
 * @return ESP_OK；ESP_ERR_INVALID_ARG index 越界
 */
esp_err_t app_manager_launch(int index);

/**
 * @brief 切回主页（即第 0 个注册的 App）
 */
esp_err_t app_manager_go_home(void);

#ifdef __cplusplus
}
#endif
