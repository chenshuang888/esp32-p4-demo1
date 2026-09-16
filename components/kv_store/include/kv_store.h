/*
 * kv_store —— 键值存储
 *
 * 给 App 一个"存/取小数据"的入口，**当前后端是 NVS**。
 * 组件名刻意不叫 nvs_store：NVS 是内部实现细节，以后若换成文件或其他实现，
 * 只要这几个函数的行为不变，调用方一行都不用改。
 *
 * ======================= 为什么正好是这 5 个函数 =======================
 *
 * 只支持 int32 和字符串两种值，且各自独立成函数，而不是一个带 type 参数的
 * 通用接口。这样类型在**编译期**就能对上：
 *   - kv_read_i32 的 out 是 int32_t*，传错类型编译不过，也不用手写 sizeof；
 *   - kv_write_str 直接收 const char*，长度和结尾符都由组件内部负责
 *     （nvs_set_str 自己算 strlen + 1），调用方不用记任何约定。
 *
 * 代价是：除了类型，其余约束编译器管不了 —— "约定的正确性靠注释承担"，
 * 也就是下面这几条。这是选"函数少"时**主动接受**的代价，不是疏漏。
 * =====================================================================
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化存储
 *
 * 必须在任何读写之前调用一次，位置在 main 的"能力层"那一批里
 * （和 time_service_init() 并列）。
 *
 * 幂等：内部 nvs_flash_init() 重复调用返回 ESP_OK，不会破坏已有数据。
 * 只有分区写满 / 分区数据格式与当前 IDF 不匹配这两种情况会触发擦除重建 ——
 * 这个恢复逻辑就是本组件存在的主要理由（不然每个 App 都要抄一遍）。
 *
 * @return ESP_OK 成功；其余为 esp_err_t 错误码
 */
esp_err_t kv_init(void);

/**
 * @brief 读一个 int32
 *
 * @param[in]  ns   namespace，"数据归属"的声明。单个 App 私有的数据直接用
 *                  App 名即可（如 "demo"），不需要额外抽常量。
 * @param[in]  key  键名
 * @param[out] out  输出
 *
 * @return ESP_OK                    成功
 *         ESP_ERR_NVS_NOT_FOUND     **从没存过** —— 不是错误，按默认值处理即可。
 *                                   （namespace 不存在和 key 不存在返回同一个码，
 *                                     所以调用方不需要分开判断。）
 *         ESP_ERR_NVS_INVALID_NAME  ns 或 key 超过 15 字符
 *         ESP_ERR_INVALID_ARG       参数为 NULL
 *
 * 用法：
 *     int32_t cnt = 0;
 *     if (kv_read_i32("demo", "count", &cnt) != ESP_OK) {
 *         cnt = 0;        // 首次启动
 *     }
 */
esp_err_t kv_read_i32(const char *ns, const char *key, int32_t *out);

/**
 * @brief 写一个 int32
 *
 * ⚠️ **立即落盘**（内部 nvs_commit）。所以不要用它做高频写（例如每秒一次、
 *    或拖动滑块时连续写）—— NVS 所在的 flash 有擦写寿命。真需要那种场景，
 *    要先在外面做"写合并 + 变化阈值"，而不是靠这个组件。
 *
 * @param[in] ns    参见 kv_read_i32()
 * @param[in] key   键名
 * @param[in] value 要写入的值
 *
 * @return ESP_OK 成功（已落盘）；其余为 esp_err_t 错误码
 */
esp_err_t kv_write_i32(const char *ns, const char *key, int32_t value);

/**
 * @brief 读一个字符串
 *
 * @param[in]  ns   参见 kv_read_i32()
 * @param[in]  key  键名
 * @param[out] out  输出缓冲。**无论成功失败，返回时 out 都是一条合法的
 *                  C 字符串**（失败时为空串），不会是未初始化的残留数据。
 * @param[in]  size 缓冲大小（传 sizeof out）。比存进去的串短就返回
 *                  ESP_ERR_NVS_INVALID_LENGTH，此时 out 被置为空串。
 *
 * @return ESP_OK                     成功
 *         ESP_ERR_NVS_NOT_FOUND     从没存过 —— 不是错误
 *         ESP_ERR_NVS_INVALID_LENGTH 缓冲不够，需要更大的 size
 *         ESP_ERR_NVS_INVALID_NAME   ns 或 key 超过 15 字符
 *         ESP_ERR_INVALID_ARG        参数为 NULL 或 size 为 0
 */
esp_err_t kv_read_str(const char *ns, const char *key, char *out, size_t size);

/**
 * @brief 写一个字符串
 *
 * ⚠️ 同 kv_write_i32()：立即落盘，不要高频写。
 *
 * @param[in] ns    参见 kv_read_i32()
 * @param[in] key   键名
 * @param[in] value 要写入的字符串（长度不限，只受 NVS 单个值的大小上限约束）
 *
 * @return ESP_OK 成功（已落盘）；其余为 esp_err_t 错误码
 */
esp_err_t kv_write_str(const char *ns, const char *key, const char *value);

#ifdef __cplusplus
}
#endif
