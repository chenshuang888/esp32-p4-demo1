/*
 * net_time —— 网络对时能力组件（SNTP 客户端）
 *
 * 定位：**只负责"从网络上问到此刻的绝对时间"，然后把它交出去。**
 *   - 不认识 wifi（不知道网络怎么来的）、不认识 time_service（不知道时间存哪）
 *   - 不认识 LVGL、不碰 NVS、不持有任何凭据
 *   - 它只是"时间源"的一种，和"手动设置 / 以后的 RTC 芯片"并列。
 *     谁去组装、把值接到哪，是上层(main)的事 —— 和 time_service 同一个模式：
 *     time_service 不认识时间源，本组件不认识时间的去处。
 *
 * ⚠️ 本组件**不认识"网络就绪"这件事**，也不自己等待。start() 之前必须已经有
 *    netif（本项目的顺序是：wifi_service_init() 起 netif → 拿到 IP → start()）。
 *    若在没网络时启动，也不会崩 —— 只是 lwip 会按自己的退避重试
 *    （15s 起、逐次加倍、上限 150s），要等很久才可能对上。
 *
 * ⚠️ 为什么"配置"和"启动"是两个函数（而不是合成一个 init）：
 *    配置必须赶在"网络可能就绪"之前完成。本项目的注册顺序是
 *        net_time_init(...) → wifi_service_set_on_connected(net_time_start)
 *    因为 main 里在发起 WiFi 连接之后、注册钩子之前还有一段耗时（相机初始化
 *    约 5s），IP 可能在这段窗口内到达；若那时才配服务器名，SNTP 会先拿默认
 *    服务器发一次请求。拆成两步就没有这个窗口。
 */
#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 对时成功的回调：把拿到的绝对时间交出去
 *
 * ⚠️ 它跑在 **lwip/SNTP 任务**上下文，**不是** LVGL 任务也不是调用者的任务 ——
 *    所以里面**不要碰任何 lv_xxx / lvgl_port_lock**。
 *    本项目由 main 注入一个只调 time_service_set() 的薄包装，天然满足。
 *
 * @param utc_sec UTC 秒数（注意不是本地时间）
 */
typedef void (*net_time_synced_cb_t)(int64_t utc_sec);

/**
 * @brief 配置 SNTP（**只配置，不启动**）
 *
 * 做的事：设服务器、设轮询模式、设"立即"同步模式、挂上通知回调。
 * 不含任何网络动作，所以可以在 netif 之前调用。
 *
 * ⚠️ 必须在 net_time_start() 之前调用，否则 start() 返回 ESP_ERR_INVALID_STATE。
 *
 * @param on_synced 对时成功时的回调；传 NULL 表示只对时、不通知任何人
 * @return ESP_OK（当前不会有别的返回值，保留 esp_err_t 是为了以后能报服务器设置失败）
 */
esp_err_t net_time_init(net_time_synced_cb_t on_synced);

/**
 * @brief 启动（或重启）对时
 *
 * 首次调用 = 启动 SNTP；之后每次调用 = **强制立刻重新对时一次**
 * （而不是等 SNTP 自己的更新周期，那个默认是 1 小时）。
 *
 * ⚠️ **幂等**：可以重复调用。它被设计成挂在"拿到 IP"事件上的钩子 ——
 *    换 AP、掉线重连都会再次触发，这里必须能安全地重复进来。
 *
 * @return ESP_OK               已启动 / 已重启
 *         ESP_ERR_INVALID_STATE 还没 net_time_init()
 */
esp_err_t net_time_start(void);

#ifdef __cplusplus
}
#endif
