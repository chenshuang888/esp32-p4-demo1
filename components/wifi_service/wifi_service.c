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

/* ===================== 凭据：本组件不带，也不存 =====================
 * 本组件**不自带任何 SSID / 密码**，也**不认识 NVS**。
 * 凭据由调用方喂进来（wifi_service_set_credentials()）：
 *   设置 App 输入 → 写 kv_store → 之后每次开机由 main 读出来喂进来。
 * 这就是 time_service 的模式：组件不认识时间源，值由 main 用 time_service_set() 喂。
 *
 * 结果是**源码里永远没有一个字节的凭据**；而且"凭据存哪"只由头文件里
 * WIFI_CFG_* 那一份常量约定，写入方和读出方不会各写各的。
 */

/* 连不上时重试几次就放弃。只为压制日志噪音 —— 放弃后**不重启也不阻塞谁**，
 * 只是不再自动重连（想再来一次就重启板子）。 */
#define WIFI_MAX_RETRY  5

static EventGroupHandle_t s_events;         /* NULL = init 没成功过 */
#define WIFI_CONNECTED_BIT  BIT0            /* 已拿到 IP */

static int  s_retry  = 0;
static bool s_inited = false;
static esp_netif_t *s_netif = NULL;         /* init 时建好的 STA netif，wifi_service_get_ip() 要用 */

/* 驱动里已经设过凭据了吗。没有就拒绝 connect() —— 刻意不去赌驱动 flash 里
 * 残留的旧配置，那样"能不能连上"取决于上次烧过什么，是最难查的一类问题。 */
static bool s_has_creds = false;

/* 上一次断开的原因，给界面翻译成人话用。
 * 每次 connect() 清零、每次断开更新 —— 所以读到的一定是本次尝试的原因。 */
static uint8_t s_last_disc_reason = 0;

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

/*
 * 把一份凭据填进驱动配置并交给 esp_wifi。
 *
 * 这里是两个坑的唯一落点（别把它们散到调用方去）：
 *
 *   1. `threshold.authmode` 是**"可接受的最弱加密"**，不是"目标网络的加密方式"。
 *      开放 AP 若被要求 WPA2 会被**直接拒绝** —— 症状只是"连不上但看不出任何原因"。
 *      所以它必须由"密码是否为空"推出，而不是由调用方配一个独立的值。
 *
 *   2. ssid / password 是以 0 结尾的**定长数组**，要用带精度的 snprintf
 *      （%.31s / %.63s = 数组长度 - 1）。不带精度、源串又更长时，
 *      GCC 会以 -Werror=format-truncation 直接报错（photo.c 里踩过同一个坑）。
 */
static esp_err_t apply_config(const char *ssid, const char *password)
{
    wifi_config_t cfg = { 0 };
    snprintf((char *)cfg.sta.ssid, sizeof(cfg.sta.ssid), "%.31s", ssid);
    snprintf((char *)cfg.sta.password, sizeof(cfg.sta.password), "%.63s", password);
    cfg.sta.threshold.authmode = (password[0] == '\0') ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;

    const esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_config 失败: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "已设置凭据：SSID=\"%s\"，%s",
             ssid, (password[0] == '\0') ? "开放网络（无密码）" : "需要密码");
    return ESP_OK;
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
        s_last_disc_reason = (uint8_t)ev->reason;
        /* reason 是排查的关键：201 = NO_AP_FOUND（多半是压根没扫到，比如目标只在 5G），
         * 15 = 4WAY_HANDSHAKE_TIMEOUT（多半是密码错），202 = AUTH_FAIL。
         * 同一个值也会被设置 App 读走，翻译成人话显示在界面上。 */
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

    /* ⚠️ init 刻意**不设凭据** —— 凭据由调用方调 wifi_service_set_credentials()
     *    喂进来（开机时 main 从 kv_store 读，用户改配置时由设置 App 写）。
     *    本组件不认识 NVS，也不带任何默认凭据（见文件上方）。 */

    /* 只 start，不 connect —— 连接是 wifi_service_connect() 的事。
     * ⚠️ esp_wifi 默认用 WIFI_STORAGE_FLASH，会把配置写进 NVS —— 这也是本组件
     *    要求"调用前 NVS 已初始化"的原因（即使此刻还没有凭据可存）。 */
    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start 失败: %s", esp_err_to_name(err));
        return err;
    }

    s_inited = true;
    ESP_LOGI(TAG, "WiFi 已就绪（STA 模式，等待凭据）");
    return ESP_OK;
}

esp_err_t wifi_service_scan(wifi_service_ap_t *out, size_t cap, size_t *count)
{
    if (out == NULL || count == NULL || cap == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    *count = 0;

    if (!s_inited) {
        ESP_LOGE(TAG, "还没 init，不能扫描");
        return ESP_ERR_INVALID_STATE;
    }

    /* 全部信道。show_hidden = false：隐藏 SSID 的名字是空的，返回给界面就是一行空白，
     * 没有意义（而且"空白行"和"字体缺字形"看起来一样，会互相误导）。
     * block=true = 扫完才返回，所以本函数是阻塞的（2~4 秒）。 */
    const wifi_scan_config_t scan_cfg = { .show_hidden = false };
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

    /* 用堆而不是栈：一条 record 约 100 字节，32 条就 3KB 多，放栈上太冒险 */
    wifi_ap_record_t *recs = calloc(WIFI_SERVICE_SCAN_MAX, sizeof(*recs));
    if (recs == NULL) {
        ESP_LOGE(TAG, "分配扫描结果缓冲失败");
        esp_wifi_clear_ap_list();
        return ESP_ERR_NO_MEM;
    }

    uint16_t n = (num > WIFI_SERVICE_SCAN_MAX) ? (uint16_t)WIFI_SERVICE_SCAN_MAX : num;
    err = esp_wifi_scan_get_ap_records(&n, recs);   /* ⚠️ 可能把 n 改小（实际取到的条数）*/
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_scan_get_ap_records 失败: %s", esp_err_to_name(err));
        free(recs);
        esp_wifi_clear_ap_list();
        return err;
    }

    /* ⚠️ 这张表里**不可能**出现 5GHz 的 AP —— C6 是单频 2.4G 芯片。
     *    所以"目标 SSID 没出现" = "它不在 2.4GHz 广播"，是物理限制。 */
    ESP_LOGI(TAG, "扫描完成：共 %u 个 AP（⚠️ 只能看到 2.4GHz，5G 的一个都看不到）",
             (unsigned)num);

    /* 转成项目自己的结构，同时按信号强度做**插入排序**（从强到弱），
     * 这样界面拿到就能直接按顺序显示，不用再排。
     * 数据量最多 32 条，插入排序的 O(n²) 完全无所谓，胜在不用额外缓冲、代码短。 */
    size_t out_n = 0;
    for (uint16_t i = 0; i < n; i++) {
        if (recs[i].ssid[0] == '\0' || out_n >= cap) {
            continue;               /* 空名字（再保险一次）/ 调用方缓冲满了 */
        }

        size_t pos = out_n;
        while (pos > 0 && out[pos - 1].rssi < recs[i].rssi) {
            out[pos] = out[pos - 1];    /* 把更弱的往后挪，给这一条腾位置 */
            pos--;
        }

        /* ⚠️ 用 snprintf("%.32s") 而不是 strcpy：ssid 是 uint8_t[33] 的**裸字节**，
         *    名字正好 32 字节时可能没有结尾符。%.32s 最多读 32 字节，既不会越界
         *    也会补上结尾符。 */
        snprintf(out[pos].ssid, sizeof(out[pos].ssid), "%.32s", (const char *)recs[i].ssid);
        out[pos].rssi    = recs[i].rssi;
        out[pos].channel = recs[i].primary;
        /* OWE 是"加密但不需要密码"，所以不能算 secure —— 否则界面会白问一次密码，
         * 而用户在密码页留空后推出的是 OPEN，反而连不上 OWE 的 AP。 */
        out[pos].secure  = (recs[i].authmode != WIFI_AUTH_OPEN &&
                            recs[i].authmode != WIFI_AUTH_OWE);
        out_n++;
    }

    /* 明细降到 DEBUG：界面已经有列表了，INFO 级别再刷屏只是噪音。
     * 想排查时把 wifi 这个 tag 的日志级别调到 DEBUG 即可。 */
    for (uint16_t i = 0; i < n; i++) {
        ESP_LOGD(TAG, "  [%2u] ch%-3u %4d dBm  %-16s  \"%s\"",
                 (unsigned)i,
                 (unsigned)recs[i].primary,
                 (int)recs[i].rssi,
                 auth_mode_str(recs[i].authmode),
                 (const char *)recs[i].ssid);
    }

    free(recs);
    esp_wifi_clear_ap_list();
    *count = out_n;
    return ESP_OK;
}

esp_err_t wifi_service_set_credentials(const char *ssid, const char *password)
{
    if (!s_inited) {
        ESP_LOGE(TAG, "还没 init，不能设凭据");
        return ESP_ERR_INVALID_STATE;
    }
    if (ssid == NULL || ssid[0] == '\0') {
        ESP_LOGE(TAG, "SSID 不能为空");
        return ESP_ERR_INVALID_ARG;
    }
    if (password == NULL) {
        password = "";              /* 调用方传 NULL 也表示开放网络 */
    }

    const esp_err_t err = apply_config(ssid, password);
    if (err == ESP_OK) {
        s_has_creds = true;
    }
    return err;
}

esp_err_t wifi_service_connect(void)
{
    if (!s_inited) {
        ESP_LOGE(TAG, "还没 init，不能连接");
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_has_creds) {
        /* 刻意不去赌驱动 flash 里残留的旧配置（可能还留着上一次写进去的 SSID）——
         * 那样"能不能连上"取决于上次烧过什么，是最难查的一类问题。 */
        ESP_LOGW(TAG, "跳过连接：还没有凭据（先调 wifi_service_set_credentials()）");
        return ESP_ERR_INVALID_STATE;
    }

    /* 手动连接 = 重试预算重置 + 断开原因清零。
     * 后者是为了让界面读到的一定是**本次尝试**的原因，而不是上一次的残留。 */
    s_retry = 0;
    s_last_disc_reason = 0;

    const esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_connect 失败: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "开始连接（断开会自动重试，上限 %d 次）", WIFI_MAX_RETRY);
    return ESP_OK;
}

uint8_t wifi_service_last_disconnect_reason(void)
{
    return s_last_disc_reason;
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
