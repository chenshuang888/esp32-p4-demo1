/*
 * wifi_service —— WiFi 能力组件（由板载 ESP32-C6 提供）
 *
 * ESP32-P4 本身**没有射频**，WiFi 由板载的 ESP32-C6 提供。软件栈是固定的两件套：
 *
 *     App 调 esp_wifi_xxx()
 *         ↓  esp_wifi_remote 提供标准 esp_wifi.h 声明 + 弱符号空实现
 *     esp_hosted（真实实现）—— 把调用序列化成 protobuf RPC
 *         ↓  SDIO（P4 侧 GPIO14~19 + 复位 54，配置在 sdkconfig.defaults）
 *     C6 上的 ESP-Hosted slave 固件 —— 真正跑 WiFi 协议栈
 *
 * 所以本文件里看不到任何 "hosted" 字眼：用的全是标准 esp_wifi API。
 * 把这一套收在组件里而不是塞进 main，是为了让 main 继续"只做编排"，
 * 也让两个托管依赖有个明确的主人。
 *
 * ⚠️ /!\ 最重要的一条 /!\ ：**C6 只支持 2.4GHz**（单频芯片，官方规格）。
 *    所以本组件永远看不到 5GHz 的 AP —— 如果目标 SSID 只跑在 5G，
 *    这里怎么配都连不上，那是物理限制。这是一个常见且容易误判的坑，
 *    wifi_service_scan() 的日志里会再强调一次。
 *
 * ⚠️ 和 C6 的 SDIO 链路是**第一次调 esp_wifi_* 时自动建立**的，不需要显式初始化。
 *    成功与否的锚点是启动日志里这一行：
 *        host_init: ESP Hosted : Host chip_ip[18]
 *    它出现 = P4↔C6 的传输层通了；**没出现 = 传输层就没起来，跟 SSID 对不对无关**。
 *
 * ⚠️ 本组件**不阻塞**（除了 scan）：连接结果全靠事件回调打日志。
 *    例程是在 init 里死等连上的，这里不行 —— main 还要继续去建 UI。
 */
#include "wifi_service.h"

#include <stdio.h>
#include <stdlib.h>     /* calloc / free */

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"

static const char *TAG = "wifi";

/* ===================== 凭据：本组件不带 =====================
 * 本组件**不自带任何 SSID / 密码**，也不认识 NVS。
 * 凭据将来由**设置 App** 输入 → 存进 kv_store(NVS) → 由 main（编排层）读出来喂进来。
 * 这就是 time_service 的模式：组件不认识时间源，值由 main 用 time_service_set() 喂。
 *
 * ⚠️ 在那条路走通之前，这里是**空壳**：init() 不设凭据，connect() 一律拒绝。
 *    也就是说**板子现在连不上 WiFi —— 这是刻意的，不是缺陷**。
 *    换来的是：源码里再也没有一个字节的凭据。
 */

/* 连不上时重试几次就放弃。只为压制日志噪音 —— 放弃后**不重启也不阻塞谁**，
 * 只是不再自动重连（想再来一次就重启板子）。 */
#define WIFI_MAX_RETRY  5

/* 一次最多列多少个扫描结果。只影响日志长度，不影响扫描本身。 */
#define WIFI_SCAN_MAX   32

static EventGroupHandle_t s_events;         /* NULL = init 没成功过 */
#define WIFI_CONNECTED_BIT  BIT0            /* 已拿到 IP */

static int  s_retry  = 0;
static bool s_inited = false;
static esp_netif_t *s_netif = NULL;         /* init 时建好的 STA netif，wifi_service_get_ip() 要用 */

/* auth mode → 字符串。只列我们认得的，其余交给 default。
 *
 * ⚠️ 不要在这里同时写 WIFI_AUTH_ENTERPRISE 和 WIFI_AUTH_WPA2_ENTERPRISE ——
 *    它们在 esp_wifi_types_generic.h 里是**同一个值**
 *    （`WIFI_AUTH_WPA2_ENTERPRISE = WIFI_AUTH_ENTERPRISE`），写成两个 case
 *    会直接报 duplicated case value。WPA3 的两个 Enterprise 变体同理，交给 default。 */
static const char *auth_mode_str(wifi_auth_mode_t m)
{
    switch (m) {
    case WIFI_AUTH_OPEN:            return "OPEN";
    case WIFI_AUTH_WEP:             return "WEP";
    case WIFI_AUTH_WPA_PSK:         return "WPA-PSK";
    case WIFI_AUTH_WPA2_PSK:        return "WPA2-PSK";
    case WIFI_AUTH_WPA_WPA2_PSK:    return "WPA/WPA2-PSK";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2-ENTERPRISE";
    case WIFI_AUTH_WPA3_PSK:        return "WPA3-PSK";
    case WIFI_AUTH_WPA2_WPA3_PSK:   return "WPA2/WPA3-PSK";
    case WIFI_AUTH_WAPI_PSK:        return "WAPI-PSK";
    case WIFI_AUTH_OWE:             return "OWE";
    default:                        return "other";
    }
}

/* ===================== 事件回调 =====================
 * 跑在**默认事件循环任务**里，不是 LVGL 任务 ——
 * 所以这里千万不要碰 lv_xxx / lvgl_port_lock。
 */
static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        /* 流起来了。**这里不自动连接** —— 连接由 wifi_service_connect() 显式发起，
         * 这样调用方可以先扫描再决定连谁（见 wifi_service.h 的说明）。 */
        ESP_LOGI(TAG, "WiFi 已启动（等待显式 connect）");

    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *ev = (const wifi_event_sta_disconnected_t *)data;
        xEventGroupClearBits(s_events, WIFI_CONNECTED_BIT);
        /* reason 是排查的关键：201 = NO_AP_FOUND（多半是压根没扫到，比如目标只在 5G），
         * 15 = 4WAY_HANDSHAKE_TIMEOUT（多半是密码错），202 = AUTH_FAIL。 */
        if (s_retry < WIFI_MAX_RETRY) {
            s_retry++;
            ESP_LOGW(TAG, "连接断开 (reason=%d)，重试 %d/%d",
                     (int)ev->reason, s_retry, WIFI_MAX_RETRY);
            esp_wifi_connect();
        } else {
            ESP_LOGE(TAG, "重试 %d 次仍连不上 (reason=%d)，放弃（不再自动重连）",
                     WIFI_MAX_RETRY, (int)ev->reason);
        }

    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *ev = (const ip_event_got_ip_t *)data;
        s_retry = 0;
        xEventGroupSetBits(s_events, WIFI_CONNECTED_BIT);
        /* 这一行 = 链路验证成功的最终标志 */
        ESP_LOGI(TAG, "拿到 IP: " IPSTR, IP2STR(&ev->ip_info.ip));
    }
}

esp_err_t wifi_service_init(void)
{
    if (s_inited) {
        return ESP_OK;                  /* 幂等 */
    }

    s_events = xEventGroupCreate();
    if (s_events == NULL) {
        return ESP_ERR_NO_MEM;
    }

    /* netif 与默认事件循环。这两个 API 在"别人已经建过"时会返回
     * ESP_ERR_INVALID_STATE —— 对我们来说那是正常情况，要当成成功。 */
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_netif_init 失败: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "创建默认事件循环失败: %s", esp_err_to_name(err));
        return err;
    }

    /* 句柄存下来给 wifi_service_get_ip() 用。
     * 不存的话就得靠 esp_netif_get_handle_from_ifkey("WIFI_STA_DEF") 去找 ——
     * 那个字符串是 IDF 的内部实现细节，能不依赖就不依赖。 */
    s_netif = esp_netif_create_default_wifi_sta();
    if (s_netif == NULL) {
        ESP_LOGE(TAG, "创建 STA netif 失败");
        return ESP_FAIL;
    }

    const wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        /* 这一步就会去拉 C6 的 SDIO 链路。失败时先看日志里有没有
         * "ESP Hosted : Host chip_ip[18]" —— 没有就是传输层的问题。 */
        ESP_LOGE(TAG, "esp_wifi_init 失败: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                              wifi_event_handler, NULL, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "注册 WIFI_EVENT 失败: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                              wifi_event_handler, NULL, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "注册 IP_EVENT 失败: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_mode 失败: %s", esp_err_to_name(err));
        return err;
    }

    /* ⚠️ 这里刻意**不调 esp_wifi_set_config()** —— 本组件已经不带凭据（见文件上方）。
     *    设置 App 做好后，凭据由 main 读出来喂进来，那一刻在这里补上 set_config。
     *    到那时有两个坑必须记住（原来是代码，现在只是注释）：
     *      1. `threshold.authmode` 是**"可接受的最弱加密"**，不是"目标网络的加密方式"。
     *         开放 AP 若被要求 WPA2 会被**直接拒绝**，症状是"连不上但看不出原因"。
     *         所以必须由"密码是否为空"推出，而不是写死：
     *             authmode = password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
     *      2. ssid / password 是以 0 结尾的定长数组，要用 snprintf 且带 "%.31s" / "%.63s"
     *         精度。不带精度、源串又更长时 GCC 会以 -Werror=format-truncation 报错
     *         （photo.c 里踩过同一个坑）。 */

    /* 只 start，不 connect —— 连接是 wifi_service_connect() 的事。
     * ⚠️ esp_wifi 默认用 WIFI_STORAGE_FLASH，会把配置写进 NVS —— 这也是本组件
     *    要求"调用前 NVS 已初始化"的原因（即使目前没有凭据可存）。 */
    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start 失败: %s", esp_err_to_name(err));
        return err;
    }

    s_inited = true;
    ESP_LOGI(TAG, "WiFi 已就绪（STA 模式；尚无凭据，等设置 App）");
    return ESP_OK;
}

esp_err_t wifi_service_scan(void)
{
    if (!s_inited) {
        ESP_LOGE(TAG, "还没 init，不能扫描");
        return ESP_ERR_INVALID_STATE;
    }

    /* 全部信道、全部 SSID（含隐藏 SSID）。block=true 表示扫完才返回。 */
    const wifi_scan_config_t scan_cfg = { .show_hidden = true };
    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_scan_start 失败: %s", esp_err_to_name(err));
        return err;
    }

    uint16_t num = 0;
    err = esp_wifi_scan_get_ap_num(&num);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_scan_get_ap_num 失败: %s", esp_err_to_name(err));
        esp_wifi_clear_ap_list();
        return err;
    }
    if (num == 0) {
        ESP_LOGW(TAG, "扫描完成：一个 AP 都没扫到（天线？或者周边只有 5G 的 SSID？）");
        esp_wifi_clear_ap_list();
        return ESP_OK;
    }

    uint16_t n = (num > WIFI_SCAN_MAX) ? (uint16_t)WIFI_SCAN_MAX : num;
    /* 用堆而不是栈：一条 record 约 100 字节，32 条就 3KB 多，放栈上太冒险 */
    wifi_ap_record_t *recs = calloc(n, sizeof(*recs));
    if (recs == NULL) {
        ESP_LOGE(TAG, "分配扫描结果缓冲失败");
        esp_wifi_clear_ap_list();
        return ESP_ERR_NO_MEM;
    }

    err = esp_wifi_scan_get_ap_records(&n, recs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_scan_get_ap_records 失败: %s", esp_err_to_name(err));
        free(recs);
        esp_wifi_clear_ap_list();
        return err;
    }

    /* ⚠️ 这张表里**不可能**出现 5GHz 的 AP —— C6 是单频 2.4G 芯片。
     *    所以"目标 SSID 没出现在下面" = "它不在 2.4GHz 广播"，是物理限制。 */
    ESP_LOGI(TAG, "扫描完成：共 %u 个 AP（⚠️ 只能看到 2.4GHz，5G 的一个都看不到）",
             (unsigned)num);
    for (uint16_t i = 0; i < n; i++) {
        ESP_LOGI(TAG, "  [%2u] ch%-3u %4d dBm  %-16s  \"%s\"",
                 (unsigned)i,
                 (unsigned)recs[i].primary,
                 (int)recs[i].rssi,
                 auth_mode_str(recs[i].authmode),
                 (const char *)recs[i].ssid);
    }

    free(recs);
    esp_wifi_clear_ap_list();
    return ESP_OK;
}

esp_err_t wifi_service_connect(void)
{
    if (!s_inited) {
        ESP_LOGE(TAG, "还没 init，不能连接");
        return ESP_ERR_INVALID_STATE;
    }

    /* 手动连接 = 重试预算重置。此刻下面还没真正连上，但这一行必须留在"连接入口"上：
     * 等凭据接通后，若漏了它，一次失败到达上限就再也不会自动重连了。 */
    s_retry = 0;

    /* 空壳阶段：没有凭据可连。
     * 刻意**不**调 esp_wifi_connect() 去赌驱动里残留的旧配置（flash 里可能还留着
     * 上一次写进去的 SSID）—— 那样"能不能连上"取决于上次烧过什么，是最难查的一类
     * 问题。等设置 App 把凭据接进来，再放开这里。 */
    ESP_LOGW(TAG, "跳过连接：本组件目前不带凭据（等设置 App 输入）");
    return ESP_ERR_INVALID_STATE;
}

bool wifi_service_is_connected(void)
{
    /* init 从没成功过（含根本没调用）时 s_events 是 NULL，
     * 而 xEventGroupGetBits(NULL) 会直接崩，所以要挡住。 */
    if (s_events == NULL) {
        return false;
    }
    return (xEventGroupGetBits(s_events) & WIFI_CONNECTED_BIT) != 0;
}

esp_err_t wifi_service_get_ip(char *out, size_t out_size)
{
    if (out == NULL || out_size < 16) {     /* 最短的 IPv4 也要 "0.0.0.0" + '\0' */
        return ESP_ERR_INVALID_ARG;
    }
    if (s_netif == NULL || !wifi_service_is_connected()) {
        return ESP_ERR_INVALID_STATE;       /* 还没连上（或 init 都没成功） */
    }

    esp_netif_ip_info_t info;
    const esp_err_t err = esp_netif_get_ip_info(s_netif, &info);
    if (err != ESP_OK) {
        return err;
    }
    snprintf(out, out_size, IPSTR, IP2STR(&info.ip));
    return ESP_OK;
}
