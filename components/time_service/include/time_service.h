/*
 * time_service —— 时间能力组件
 *
 * 给 App 提供"现在几点"。设计要点（也是这个组件的全部复杂度）：
 *
 * 1) 走时和对时是两件事
 *    - 走时："从那一刻起过了多久"，**永远**由单调时间(esp_timer)完成，本组件内部负责。
 *      （IDF 自己的 settimeofday/gettimeofday 也是这个模型：内部就是 "偏移 + 单调时间"）
 *    - 对时：外部把"此刻的绝对时间"交给它**一次**。
 *
 *    所以本组件**不认识任何"时间源"** —— 手动设置 / SNTP / 以后的 RTC 芯片，
 *    都只是"调用 time_service_set() 的那个东西"。谁去拿到这个时间、由谁负责组装，
 *    是上层(main)的事，组件之间因此零耦合。
 *
 * 2) "未对时过"用返回码表达，不单独提供 is_valid()
 *    get() 返回 ESP_ERR_INVALID_STATE 就代表时间不可信。
 *    一个接口同时给出"值"和"可用性"，App 应当处理这个情况（例如显示"时间未校准"），
 *    而不是把一个假时间（1970 或开机秒数）当真显示出去。
 */
#pragma once

#include <stdint.h>
#include <time.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化：设置时区等项目级决策
 *
 * 无参数、幂等。必须在其它接口之前调用。
 */
esp_err_t time_service_init(void);

/**
 * @brief 对时：把"此刻的绝对时间"交进来
 *
 * 这是外部时间源（手动设置 / SNTP / RTC 芯片）唯一的入口。
 *
 * @param utc_sec UTC 秒数（注意不是本地时间）
 */
esp_err_t time_service_set(int64_t utc_sec);

/**
 * @brief 取当前本地时间
 *
 * @param out 输出本地时间（已按 init() 设置的时区换算）
 * @return ESP_OK                 成功
 *         ESP_ERR_INVALID_ARG     out 为空
 *         ESP_ERR_INVALID_STATE   还没对过时，时间不可信
 */
esp_err_t time_service_get(struct tm *out);

#ifdef __cplusplus
}
#endif
