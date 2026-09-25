/*
 * wifi_service —— WiFi 能力组件（由板载 ESP32-C6 提供）
 *
 * 它做三件事：**把 WiFi 起起来**、**扫一遍周边 AP**、**按凭据连接**。
 *
 * ⚠️ 本组件**不自带任何凭据**，也不认识 NVS。凭据将来由**设置 App** 输入 →
 *    存进 kv_store(NVS) → 由 main（编排层）读出来喂进来。这就是 time_service 的模式
 *    （组件不认识时间源，值由 main 用 time_service_set() 喂进去）。
 *
 * ⚠️ **当前是空壳阶段**：还没有"喂凭据"的入口，所以 init() 不设凭据、
 *    connect() 一律拒绝 —— **板子现在连不上 WiFi，这是刻意的，不是缺陷**。
 *    换来的是源码里再没有一个字节的凭据。等设置 App 做出来这条链路才通。
 *
 * 生命周期：main 在启动时调一次 wifi_service_init()，之后一直常驻 ——
 * 没有 deinit，也不打算做"按需开关"。这和 usb_camera 是同一个取舍：
 * 网络断了要自己重连，它不是一个能随便拆掉再建的外设。
 *
 * 为什么 init 和 connect 是分开的：让"先扫描看清楚再连接"成为可能
 * （排查时想先知道周边有什么、目标 AP 是什么加密方式）。以后要做网络设置界面时，
 * 这个顺序（起流 → 扫描 → 选一个 → 连接）也正好是界面的流程。
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 WiFi（**只把流起来，不连接**）
 *
 * 内部依次做：netif → 默认事件循环 → esp_wifi_init → 设 STA 模式 → start。
 *
 * ⚠️ **不设凭据**（本组件不带凭据，见文件顶部），也不会开始连接 ——
 *    连接由调用方随后显式调 wifi_service_connect()（目前它也会拒绝）。
 *
 * ⚠️ 必须在 **NVS 初始化之后**调用（本项目由 main 的 kv_init() 完成）：
 *    esp_wifi 要在 NVS 里存配置。本组件刻意不自己调 nvs_flash_init()，
 *    免得和 kv_store 抢"谁负责初始化 NVS"这件事。
 *
 * ⚠️ 幂等：重复调用直接返回 ESP_OK。
 *
 * @return ESP_OK           流已起来
 *         ESP_ERR_NO_MEM   事件组分配失败
 *         其余             netif / wifi 初始化失败（原因已打到日志）
 */
esp_err_t wifi_service_init(void);

/**
 * @brief 扫描一遍周边 AP，把结果打到日志（**阻塞**，约 2~4 秒）
 *
 * ⚠️ **只能扫到 2.4GHz 的 AP** —— 板上的 ESP32-C6 是单频 2.4G 芯片。
 *    所以"扫不到某个 SSID"就等于"那个 SSID 不在 2.4GHz 广播"，是物理限制，
 *    再怎么改配置也没用。这一条是排查校园网/办公网连不上时的第一个判断点。
 *
 * ⚠️ 会打断已建立的连接（扫完由事件回调自动重连）。请在 init 之后、
 *    connect 之前调用，或明确接受这次闪断。
 *
 * @return ESP_OK               扫描完成（哪怕一个 AP 都没有）
 *         ESP_ERR_INVALID_STATE init 还没成功过
 *         其余                 扫描 / 取结果失败
 */
esp_err_t wifi_service_scan(void);

/**
 * @brief 开始连接（**非阻塞**）
 *
 * 连上 / 断开都通过事件回调打日志。要查"现在连上没有"用 wifi_service_is_connected()。
 * 失败重试由事件回调负责（上限见 .c 里的 WIFI_MAX_RETRY）。
 *
 * ⚠️ **现在必然返回 ESP_ERR_INVALID_STATE** —— 本组件还没有凭据来源（空壳阶段，
 *    见文件顶部）。刻意不去赌驱动里残留的旧配置，那样"能不能连上"取决于上次
 *    烧过什么，是最难查的一类问题。
 *
 *    保留这个接口是为了让 main 的编排形状一次到位：等设置 App 把凭据接进来，
 *    这里不用再改。
 *
 * @return ESP_OK               已开始连接（凭据接进来之后才会出现）
 *         ESP_ERR_INVALID_STATE init 没成功过 / 还没有凭据
 *         其余                 esp_wifi_connect() 返回的错误
 */
esp_err_t wifi_service_connect(void);

/**
 * @brief 现在拿到 IP 了吗
 *
 * 由事件回调写、可以从任何任务读（内部走 FreeRTOS 事件组，是线程安全的）。
 *
 * @return true = 已连上且已获取 IP
 */
bool wifi_service_is_connected(void);

/**
 * @brief 取当前的 IPv4 地址（点分十进制字符串）
 *
 * ⚠️ 这里拿到的是**本机自己认为的**地址。门户网络里它和"服务器看到你的地址"是
 *    一致的（没 NAT），所以可以直接拿去填门户的字段 —— 但它不是"问服务器要来的"。
 *
 * @param out      输出缓冲
 * @param out_size 缓冲大小，至少 16
 * @return ESP_OK                成功（out 里是 "a.b.c.d"）
 *         ESP_ERR_INVALID_ARG  参数不合法
 *         ESP_ERR_INVALID_STATE 还没连上（或 init 没成功过）
 */
esp_err_t wifi_service_get_ip(char *out, size_t out_size);

#ifdef __cplusplus
}
#endif
