/*
 * wifi_service —— WiFi 能力组件（由板载 ESP32-C6 提供）
 *
 * 它做四件事：**把 WiFi 起起来**、**扫一遍周边 AP**、**接收凭据**、**按凭据连接**。
 *
 * ⚠️ 本组件**不自带任何凭据**，也**不认识 NVS**。凭据由调用方喂进来：
 *    设置 App 写 kv_store → main 在开机时读出来调 wifi_service_set_credentials()
 *    （键名常量见下面的 WIFI_CFG_*）。这就是 time_service 的模式 ——
 *    组件不认识时间源，值由 main 用 time_service_set() 喂进去。
 *    这样源码里永远没有一个字节的凭据。
 *
 * 生命周期：main 在启动时调一次 wifi_service_init()，之后一直常驻 ——
 * 没有 deinit，也不打算做"按需开关"。这和 usb_camera 是同一个取舍：
 * 网络断了要自己重连，它不是一个能随便拆掉再建的外设。
 *
 * 为什么 init / set_credentials / connect 是三步而不是一步：
 * 让"先扫描看清楚再连接"成为可能（排查时想先知道周边有什么、目标 AP 是什么加密方式）。
 * 设置界面的流程正好也是这个顺序：起流 → 扫描 → 选一个 → 输入密码 → 连接。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ===================== 凭据在 NVS 里的位置 =====================
 * 本组件**不碰 NVS**，但凭据的存储布局由这里声明 —— 因为写入方（设置 App）
 * 和读出方（main）必须是同一份约定，各写各的字面量迟早会写歪。
 * 用的时候直接把它们传给 kv_store 的 kv_read_str / kv_write_str。
 *
 * ⚠️ namespace 和 key 都受 NVS 限制：各不超过 15 个字符。 */
#define WIFI_CFG_NS        "wifi"     /*!< namespace */
#define WIFI_CFG_KEY_SSID  "ssid"     /*!< 值：SSID 字符串 */
#define WIFI_CFG_KEY_PASS  "pass"     /*!< 值：密码字符串；开放网络存空串 */

/* ===================== 扫描结果 ===================== */

/* 内部对扫描结果的硬上限。也是给调用方的"开多大缓冲够用"的参考值：
 * 32 × sizeof(wifi_service_ap_t) ≈ 1.2KB。 */
#define WIFI_SERVICE_SCAN_MAX  32

/**
 * @brief 一个扫到的 AP
 *
 * 刻意**不直接把 esp_wifi 的 wifi_ap_record_t 暴露出去** —— 那样 apps/ 就得
 * include esp_wifi_types.h，界面层就被绑死在 WiFi 驱动上了。这里是"界面够用"的子集。
 */
typedef struct {
    /*! @brief **原样字节**（UTF-8），连接时用它。
     *  ⚠️ 非 ASCII 的 SSID（比如中文）在界面上会显示成一串占位方块 ——
     *     项目没有中文字体，这是已知的、暂时不处理的限制（LVGL 的
     *     LV_USE_FONT_PLACEHOLDER 默认为开，所以至少不是空白行）。 */
    char    ssid[33];
    int8_t  rssi;       /*!< dBm，负值，越接近 0 越强 */
    uint8_t channel;    /*!< 信道 1~13（C6 只支持 2.4GHz） */
    bool    secure;     /*!< true = 需要密码；false = 开放网络 */
} wifi_service_ap_t;

/**
 * @brief 初始化 WiFi（**只把流起来，不设凭据、不连接**）
 *
 * 内部依次做：netif → 默认事件循环 → esp_wifi_init → 设 STA 模式 → start。
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
 * @brief 把凭据交给驱动（**只设配置，不发起连接**）
 *
 * 两种用法：main 开机时把 kv_store 里存的凭据读出来喂进来；
 * 设置 App 在用户输入完密码后喂进来。
 *
 * ⚠️ 本函数**不碰 NVS** —— 持久化是调用方的事（写 kv_store，键名见 WIFI_CFG_*）。
 *    组件不认识 NVS，也刻意不替调用方做持久化：那样"存了"和"生效了"会变成
 *    两件必须自己配对的事，迟早有一边漏掉。
 *
 * ⚠️ 加密方式（threshold.authmode）由**密码是否为空**自动推导，调用方不用管：
 *    空密码 = 开放网络。这个值是"**可接受的最弱加密**"而不是"目标网络的加密方式"，
 *    填错会让开放 AP 被**直接拒绝**，而症状只是"连不上"，极难查。
 *
 * ⚠️ 设完要自己调 wifi_service_connect() 才会真的连。
 *
 * @param ssid      网络名，不可为空
 * @param password  密码；传 NULL 或空串 = 开放网络
 * @return ESP_OK               已设进驱动
 *         ESP_ERR_INVALID_ARG  ssid 为空
 *         ESP_ERR_INVALID_STATE init 还没成功过
 *         其余                 esp_wifi_set_config() 返回的错误
 */
esp_err_t wifi_service_set_credentials(const char *ssid, const char *password);

/**
 * @brief 扫描一遍周边 AP（**阻塞**，约 2~4 秒）
 *
 * 结果按**信号强度从强到弱**排序写进 out，界面直接按顺序显示即可。
 *
 * ⚠️ **是阻塞调用**：调用它的任务会被挡住 2~4 秒。不要从对时间敏感的地方调
 *    （目前唯一的使用者是设置 App，它调之前会先显示 "Scanning..."）。
 *
 * ⚠️ **只能扫到 2.4GHz 的 AP** —— 板上的 ESP32-C6 是单频 2.4G 芯片。
 *    所以"扫不到某个 SSID"就等于"那个 SSID 不在 2.4GHz 广播"，是物理限制，
 *    再怎么改配置也没用。这是排查"某个网络连不上"时的第一个判断点。
 *
 * ⚠️ **隐藏 SSID 的 AP 不返回**：它们的名字是空的，在列表里就是一行空白，没有意义
 *    （而且空白行和"字体缺字形"看起来一样，容易互相误导）。
 *
 * ⚠️ 会打断已建立的连接（扫完由事件回调自动重连）。请在 init 之后、connect 之前
 *    调用，或明确接受这次闪断。
 *
 * @param[out] out    AP 数组
 * @param[in]  cap    out 能装几条；WIFI_SERVICE_SCAN_MAX 是够用的参考值
 * @param[out] count  实际写进去的条数。扫到 0 个也是 ESP_OK，此时 *count = 0
 * @return ESP_OK               扫描完成
 *         ESP_ERR_INVALID_ARG  任一参数为空 / cap 为 0
 *         ESP_ERR_INVALID_STATE init 还没成功过
 *         其余                 扫描 / 取结果失败
 */
esp_err_t wifi_service_scan(wifi_service_ap_t *out, size_t cap, size_t *count);

/**
 * @brief 连接上一步 set_credentials() 设好的那个 AP（**非阻塞**）
 *
 * 连上 / 断开都通过事件回调打日志。要查"现在连上没有"用 wifi_service_is_connected()；
 * 失败了想知道原因用 wifi_service_last_disconnect_reason()。
 * 失败重试由事件回调负责（上限见 .c 里的 WIFI_MAX_RETRY）。
 *
 * ⚠️ **还没有凭据时返回 ESP_ERR_INVALID_STATE**（并且只打一条日志）。
 *    刻意不去赌驱动 flash 里残留的旧配置 —— 那样"能不能连上"取决于上次烧过什么，
 *    是最难查的一类问题。
 *
 * @return ESP_OK               已开始连接
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
 * @brief 上一次 STA 断开的原因（`wifi_err_reason_t` 的数值；0 = 本次尝试还没断开过）
 *
 * 给界面把"连接失败"翻译成人话用的：15 = 密码错、201 = 周围找不到这个 AP、
 * 202 = 认证失败…… 完整清单见 `esp_wifi_types_generic.h` 里的 `wifi_err_reason_t`。
 *
 * ⚠️ 返回 `uint8_t` 而不是那个 enum，同样是为了不让 apps/ 依赖 esp_wifi 的头。
 * ⚠️ **每次 wifi_service_connect() 会清零**，之后每次断开都会更新，
 *    所以拿到的一定是"本次连接尝试"的原因，不是上一次的残留。
 *
 * @return >=0 的原因码；0 表示本次尝试还没发生过断开
 */
uint8_t wifi_service_last_disconnect_reason(void);

/**
 * @brief 取当前的 IPv4 地址（点分十进制字符串）
 *
 * ⚠️ 这里拿到的是**本机自己认为的**地址，不是"问服务器要来的"。
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
