/*
 * UI 层 —— 顶部状态栏
 *
 * 它是一份**全局唯一**的状态栏，挂在 LVGL 的顶层（lv_layer_top()）上，
 * 不属于任何一个 App 的 screen。所以：
 *   - lv_screen_load() 切 App 时它不会被销毁 —— 切换时零重建、零闪烁；
 *   - 刷新用的定时器只有一份、生命周期等于整个程序 —— 不存在"漏删"的问题。
 *
 * ============================ 这个组件不认识业务 ============================
 * 状态栏要显示什么文本（时间？WiFi？电量？）、点返回要干什么，它通通不知道：
 * 这两件事都通过**回调注入**由外部提供。所以 ui 组件：
 *   - 不需要 include time_service.h（不知道"时间"是什么）
 *   - 不需要 include app_manager.h（不知道"返回"会切去哪）
 * 负责注入的是 main —— 它本来就是组装者，同时依赖 ui 和 time_service。
 *
 * 往回调里传什么也有讲究：槽位的参数类型是"字符 + 长度"，而不是 struct tm
 * 之类的具体类型。否则即使不 include time_service.h，"ui 的槽位是给时间用的"
 * 也会写进 ui 的接口里 —— 那叫概念耦合，编译能过但已经绑死了。
 * 字符串是"最低公分母"，所以这个槽位以后接 WiFi 状态、电量都不用改 ui。
 * ==========================================================================
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 状态栏高度（像素）。App 自己的内容要从这个高度往下排，否则会被挡住 */
#define UI_STATUS_BAR_HEIGHT  48

/**
 * @brief "文本来源"回调：把要显示的文字写进 out
 *
 * 调用时机与上下文（重要）：
 *   - 由状态栏内部的定时器驱动，**运行在 LVGL 任务上下文里**；
 *   - 所以里面不要再调 lvgl_port_lock()（已经持有锁了）；
 *   - 只做"纯读"操作，不要在里面无保护地写共享状态。
 *     （例如 time_service_get() 内部是 localtime_r，是线程安全的）
 *
 * 为什么是"写进调用方给的 buffer"而不是"返回一个字符串指针"：
 * 返回指针会牵扯到缓冲区归谁管、下次调用是否失效、多任务下是否被覆盖，
 * 直接写进 out 这些坑全都没有。
 *
 * 写入空字符串（out[0] = '\0'）表示"这次没有内容可显示"。
 *
 * @param out       输出缓冲，由状态栏提供，保证非 NULL
 * @param out_size  缓冲大小，保证 > 0
 */
typedef void (*ui_status_bar_text_cb_t)(char *out, size_t out_size);

/**
 * @brief "返回"动作回调：返回按钮被点击时调用
 *
 * 同样运行在 LVGL 任务上下文里。
 * 注意它被定义成 void(*)(void)：点击返回这个动作在语义上就是"做一件事、
 * 不关心结果"。如果直接把 esp_err_t 返回类型的函数传进来，类型不兼容
 * （通过不兼容的函数指针调用是未定义行为），需要在调用侧包一层。
 */
typedef void (*ui_status_bar_action_cb_t)(void);

/**
 * @brief 状态栏要展示的内容 —— 由各 App 在 enter() 里填写
 */
typedef struct {
    const char *title;      /*!< App 名。NULL 或空串表示不显示 */
    bool        show_back;  /*!< 是否显示返回按钮（主页没有"回"的地方，传 false） */
} ui_status_bar_cfg_t;

/**
 * @brief 创建状态栏，并注入两个外部动作
 *
 * 由 main 在 ui_init() 之后调用一次（重复调用是安全的，直接返回 OK）。
 * 创建后状态栏是空的：无标题、无返回按钮、文本槽为空，
 * 随后由各 App 通过 ui_status_bar_apply() 填内容。
 *
 * @param text_source  文本来源，可传 NULL（则文本槽一直为空）
 * @param back_action  返回动作，可传 NULL（则返回按钮点了没反应）
 */
esp_err_t ui_status_bar_init(ui_status_bar_text_cb_t  text_source,
                             ui_status_bar_action_cb_t back_action);

/**
 * @brief 按当前 App 的配置刷新状态栏
 *
 * 各 App 在 enter() 里调用。必须在 ui_status_bar_init() 成功之后。
 *
 * @param cfg 配置，不可为 NULL
 * @return ESP_OK               成功
 *         ESP_ERR_INVALID_ARG  cfg 为 NULL
 *         ESP_ERR_INVALID_STATE 状态栏还没创建（忘了调 ui_status_bar_init）
 */
esp_err_t ui_status_bar_apply(const ui_status_bar_cfg_t *cfg);

#ifdef __cplusplus
}
#endif
