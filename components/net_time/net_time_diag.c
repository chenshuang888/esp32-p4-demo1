/*
 * net_time_diag —— 【临时诊断】SNTP 偶发失败的定位探测
 *
 * ⚠️⚠️ 本文件当前**不参与构建**（2026-09-27 起，配套接线已注释掉）。
 *     问题已定位并修掉（服务器换成 ntp.tencent.com），而它每架次会固定刷 300 秒
 *     日志，所以停用、留在仓库里备查。
 *
 *     重新启用要**三处一起放开**（缺一处要么报错要么没探测）：
 *       ① components/net_time/CMakeLists.txt：SRCS 里的 "net_time_diag.c"
 *       ② components/net_time/CMakeLists.txt：REQUIRES 里的 esp_wifi
 *       ③ components/net_time/net_time.c：include + net_time_start() /
 *          on_sntp_sync() 里那三处
 *     （CMakeLists 顶部有一份一模一样的说明。）
 *
 *     ⚠️ 因为不参与构建，它平时**不会被编译检查** —— 以后 IDF/lwip 升级后
 *        重新启用时，可能要顺手修几个 API。
 *
 * ============ 结论（2026-09-27，八架次 + 一台 PC 的对照）============
 *
 * **丢的是"到 NTP 端点的那一次 UDP 123 交换"，不是本机的任何一段。**
 * lwip 的退避（15/30/60 秒翻倍）把"丢一发"放大成 150 秒 ——
 * 这就是"有时 2 秒、有时 150 秒"的全部来源。
 *
 * 已排除（都有硬证据）：SDIO/esp_hosted（pass/out 每次 +1、drop 与 throttling 恒 0）、
 * 下行/C6/AP（失败窗口内 ICMP 与 UDP-53 全通、RSSI 稳定）、"回包到了被 lwip 丢"
 * （rx_in 没涨 = 回包根本没到）。
 *
 * **关键证据是"换服务器就换结果"**（下面名单里的数字），而且**两台独立设备
 * （ESP32 板子 + 一台 PC，同一个校园网）看到同一个模式** → 与本机无关。
 *
 * **修法（2026-09-27 已定，只做了第一步）**：把生产服务器从 ntp.aliyun.com
 * 换成 ntp.tencent.com（见 net_time.c 的 NET_TIME_SERVER 注释）。
 * 另外两个可选项（配多个服务器 / 应用层失败即重问）**决定不做** ——
 * 换掉那个 58% 成功率的端点已经把"连丢 3 次 = 150 秒"的概率压掉约三个数量级。
 * 再发生的话，先回来看这里再考虑。
 *
 * ============ 这一版怎么测 ============
 *
 * 满 300 秒，每 30 秒一轮：
 *   - 每轮打一行轮次头（带 SNTP 此时的同步状态）+ 读一次 RSSI
 *   - 每轮对**每个候选 NTP** 发一个**全新的 UDP 123 请求**（mode-3，48 字节），
 *     看 1 秒内有没有 mode-4 回包
 *   - ICMP（SNTP 服务器 + 校园 DNS）仍每 5 秒一发，**全程不停** ——
 *     它的作用是证明"这 300 秒里下行一直是好的"
 *
 * ⚠️ 故意**不再"对上就提前收工"**：这次要的是每个服务器的成功率统计，
 *    固定窗口才可比。所以不管 SNTP 有没有对上，都跑满 DIAG_MAX_MS。
 *
 * 候选名单是实测选出来的（括号里是"PC 20 次 + 板子 8 次"的合计成功率），
 * 刻意覆盖"好的 / 已停用 / 差的"三类，这样一看就知道是**服务器**的问题
 * 还是这条路上**所有 UDP 123** 都这样：
 *     ntp.tencent.com      26/28   最好 —— **现在生产用的就是它**
 *     ntp.aliyun.com       15/28   已停用（原来用的），留作对照
 *     cn.pool.ntp.org      17/20   第二选择（解析到腾讯的 IP 段）
 *     time.cloudflare.com   7/10   独立运营商、路径远（165~223 ms）
 *     ntp.ntsc.ac.cn        2/10   差的参照
 *
 * 掉过的坑：`ntp.tuna.tsinghua.edu.cn` 名字还能解析但完全不回；
 * `ntp1.nimt.ac.cn` 已失效；`ntp.sjtu.edu.cn` 现在解析到 17.253.68.251
 * （Apple 的网段）—— 名义和实际对不上，所以都不在名单里。
 * 校内/同城也没有公开 NTP（`ntp.nuist.edu.cn` / `nju` / `seu` 都不解析）。
 *
 * ============ 怎么读结果 ============
 *
 * 10 轮跑完，按**每一列（服务器）数成功次数**：
 *   - 某一列 10/10、另一列 3/10，而且**每一轮里 ICMP 都是好的**
 *       -> 就是那个端点的问题，换掉它就好
 *   - 所有列都大面积失败，但 ICMP 全好
 *       -> 这条路上对 UDP 123 有选择性丢弃（换服务器救不了，得换路径/协议）
 *   - 失败时 ICMP 也不好
 *       -> 下行/链路问题（另一类现象，之前抓到过两次约 5 秒的整条空白）
 *
 * ⚠️ 注意（不能从本机排除的混淆项）：
 *   - 自测用的是**新建的 socket**（全新源端口），SNTP 用的是**长命 socket**
 *     （固定源端口）。所以"自测通、SNTP 不通"既可能是流的状态问题，也可能是
 *     **源端口相关**的问题。
 *   - 自测会**预热 lwip 的 DNS 缓存**，所以 SNTP 之后的请求会跳过域名解析：
 *     **DNS 阶段的问题会被掩盖**。
 *   - 自测每 30 秒对每个服务器各发一次，**超过 NTP 的礼貌频率**；如果某些失败
 *     是自己触发限流的，这个测法会放大它。没法避免，只能记着。
 *
 * ============ 怎么删掉它 ============
 *
 *    - 删本文件 + include/net_time_diag.h
 *    - 删 net_time/CMakeLists.txt 的 SRCS 里那行 "net_time_diag.c"（和 esp_wifi 依赖）
 *    - 删 net_time.c 里那两行调用（start / note_synced）
 */
#include "net_time_diag.h"

#include <stdbool.h>
#include <string.h>

#include "esp_log.h"
#include "esp_wifi.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "lwip/opt.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "lwip/dns.h"
#include "ping/ping_sock.h"

static const char *TAG = "diag";

#define DIAG_ITV_MS     5000        /* ICMP 每 5 秒一发（全程不停，用来证明下行good） */
#define DIAG_TIMEOUT_MS 3000        /* ICMP 单次探测最多等这么久 */
#define DIAG_MAX_MS     300000      /* 固定跑满 5 分钟 */
#define DIAG_ROUND_MS   30000       /* NTP 自测 + RSSI 的轮次间隔 */

#define DIAG_TASK_STACK 4096        /* 本探测任务自己的栈 */
#define DIAG_PING_STACK 4096        /* 每个 ping 会话自带一个任务，它的栈 */

#define DIAG_MAX_TARGETS 2          /* ICMP：SNTP 服务器 + DNS 服务器 */

/* ---- NTP 自测（手写的极简客户端）---- */
#define DIAG_NTP_PORT     123
#define DIAG_NTP_PKT_LEN  48        /* NTP 报文定长 48 字节（不带扩展字段） */
#define DIAG_NTP_RECV_MS  1000      /* 只等 1 秒：正常回复是几十毫秒 */

/* 候选 NTP 服务器。数字见文件头部的说明（实测来的，不是抄的）。 */
static const char *const DIAG_NTP_SERVERS[] = {
    "ntp.tencent.com",          /* 最好 —— 现在生产用的就是它 */
    "ntp.aliyun.com",           /* 已停用的旧服务器，留作对照 */
    "cn.pool.ntp.org",          /* 第二选择 */
    "time.cloudflare.com",      /* 独立运营商、路径远 */
    "ntp.ntsc.ac.cn",           /* 差的参照 */
};
#define DIAG_NTP_MAX ((int)(sizeof(DIAG_NTP_SERVERS) / sizeof(DIAG_NTP_SERVERS[0])))

static esp_ping_handle_t s_hdl[DIAG_MAX_TARGETS];
static int               s_cnt;         /* 已经建成的 ICMP 会话数 */
static bool              s_started;     /* 只探一次 */

/* 本架次有没有真的对时成功。
 * ⚠️ 由 net_time.c 的 SNTP 通知回调置位（net_time_diag_note_synced），
 *    **不能**用"墙上时钟是否已设定"来判断 —— 本工程
 *    CONFIG_NEWLIB_TIME_SYSCALL_USE_RTC_HRT=y，墙钟存在 RTC_STORE 里，
 *    **跨软复位会保留**（实测：开机第 1 秒墙钟就已 >2020 年，而 SNTP 2.8 秒后才同步）。 */
static volatile bool s_synced;

static const char *sync_str(void)
{
    return s_synced ? "已对时" : "未对时";
}

static void on_ping_ok(esp_ping_handle_t hdl, void *args)
{
    uint32_t ms = 0;
    if (esp_ping_get_profile(hdl, ESP_PING_PROF_TIMEGAP, &ms, sizeof(ms)) != ESP_OK) {
        ms = 0;
    }
    ESP_LOGI(TAG, "ICMP %s 回复 %ums   [%s]", (const char *)args, (unsigned)ms, sync_str());
}

static void on_ping_timeout(esp_ping_handle_t hdl, void *args)
{
    (void)hdl;
    ESP_LOGW(TAG, "ICMP %s 超时(>%dms)   [%s]", (const char *)args, DIAG_TIMEOUT_MS, sync_str());
}

/* 建一个"不停"的 ICMP 会话并启动它（count=0 = 不限次数，最后由我们 stop 掉）。
 * ⚠️ 这两个回调跑在 **ping 任务**上下文里（每个会话自带一个任务），不是我们的任务。 */
static void start_session(const char *name, const ip_addr_t *addr)
{
    if (s_cnt >= DIAG_MAX_TARGETS) {
        return;
    }

    /* 故意不用 ESP_PING_DEFAULT_CONFIG()：那个宏里引用了 ESP_TASK_PING_STACK
     * （在 esp_task.h，属于 esp_system，本组件没依赖那个组件），用了编不过。
     * 逐字段写反而更清楚 —— 这里就是 IDF 那份默认值，栈改大一点更稳。 */
    esp_ping_config_t cfg = {
        .count           = ESP_PING_COUNT_INFINITE,
        .interval_ms     = DIAG_ITV_MS,
        .timeout_ms      = DIAG_TIMEOUT_MS,
        .data_size       = 64,
        .tos             = 0,
        .ttl             = IP_DEFAULT_TTL,
        .target_addr     = *addr,
        .task_stack_size = DIAG_PING_STACK,
        .task_prio       = 2,
        .interface       = 0,
    };
    esp_ping_callbacks_t cbs = {
        .cb_args         = (void *)name,
        .on_ping_success = on_ping_ok,
        .on_ping_timeout = on_ping_timeout,
        .on_ping_end     = NULL,
    };

    esp_ping_handle_t hdl = NULL;
    esp_err_t err = esp_ping_new_session(&cfg, &cbs, &hdl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "建 ping 会话失败(%s): %s", name, esp_err_to_name(err));
        return;
    }
    s_hdl[s_cnt++] = hdl;       /* 建成了就记账 —— 最后一定要删掉它 */

    err = esp_ping_start(hdl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "启动 ping 失败(%s): %s", name, esp_err_to_name(err));
    }
}

/* RSSI：走 esp_wifi 的 RPC 到 C6（不是网络动作）。
 * 它返回的是**驱动缓存里最近一次的信标测量值**，所以即使此刻收不到东西也读得到 ——
 * 读到的值正常 = 最后一次听到的信标质量是好的，可以据此排除"信号变差"这条。 */
static void log_rssi(void)
{
    int rssi = 0;
    esp_err_t err = esp_wifi_sta_get_rssi(&rssi);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "链路 rssi=%d dBm   [%s]", rssi, sync_str());
    } else {
        /* 连这个都失败 = 连 RPC 到 C6 都不通了，是个强信号，别只当噪音 */
        ESP_LOGW(TAG, "链路 rssi 读取失败: %s   [%s]", esp_err_to_name(err), sync_str());
    }
}

/* ---- 解析域名到 IPv4。用 lwip_getaddrinfo 这个**原名**：本工程
 *      LWIP_COMPAT_SOCKETS=0（port/include/lwipopts.h:969），netdb.h 里
 *      "#define getaddrinfo ..." 那段不生效，直接写 getaddrinfo 会解析到 newlib
 *      的同名声明上去。 */
static bool resolve_v4(const char *host, ip_addr_t *out)
{
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_RAW;

    struct addrinfo *res = NULL;
    if (lwip_getaddrinfo(host, NULL, &hints, &res) != 0 || res == NULL) {
        return false;
    }
    /* ⚠️ 用 _val 版本：ip_addr_set_ip4_u32() 展开成 do{if(ipaddr){...}}，传局部变量
     *    的地址进去会触发 -Werror=address。type 字段必须设对 ——
     *    esp_ping_new_session() 靠 IP_IS_V4() 决定建 IPv4 还是 IPv6 的 raw socket。 */
    ip_addr_set_ip4_u32_val(*out, ((struct sockaddr_in *)res->ai_addr)->sin_addr.s_addr);
    lwip_freeaddrinfo(res);
    return true;
}

/* ==================== 极简 NTP 客户端（诊断用） ====================
 *
 * 只做最少的事：发一个 48 字节 mode-3 请求，看 1 秒内有没有回包；回包按
 * mode==4 认。不解析时间（那是 SNTP 的活）。
 *
 * ⚠️ 每次调用都**新建 socket**：源端口是全新的。这点是有意为之 ——
 *    要分清"是这条路不通"还是"SNTP 那条流卡住"。但也因此，"自测通、SNTP 不通"
 *    时无法排除"问题在源端口上"（见文件头部的注意）。 */

static uint32_t s_ntp_tag;      /* 只用来把回包和请求对上，内容无所谓 */

static void ntp_probe_once(const char *name, const ip_addr_t *addr)
{
    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family      = AF_INET;
    dst.sin_port        = PP_HTONS(DIAG_NTP_PORT);
    dst.sin_addr.s_addr = ip4_addr_get_u32(ip_2_ip4(addr));

    int sock = lwip_socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        ESP_LOGW(TAG, "NTP %s 建 socket 失败: %d   [%s]", name, sock, sync_str());
        return;
    }
    /* ⚠️ 必须确认真的设上了：LWIP_SO_RCVTIMEO=1（port/include/lwipopts.h:1005）所以
     *    这里会成功；万一哪天被关掉，recvfrom 会**永久阻塞**，整个探测任务就卡死 ——
     *    那比失败更难查，所以这里显式挡一下。 */
    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    if (lwip_setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0) {
        ESP_LOGE(TAG, "NTP %s 设不了接收超时，跳过（否则会永久阻塞）", name);
        lwip_close(sock);
        return;
    }

    uint8_t req[DIAG_NTP_PKT_LEN];
    memset(req, 0, sizeof(req));
    req[0] = (4 << 3) | 3;      /* LI=0 VN=4 Mode=3(client) */
    req[2] = 4;                 /* poll interval 2^4；服务端不看这个 */
    s_ntp_tag++;
    memcpy(&req[40], &s_ntp_tag, sizeof(s_ntp_tag));    /* transmit timestamp 的位置。
                                                        * 填个序号：服务端会把它抄进
                                                        * originate 字段（字节 24..31），
                                                        * 用来确认回包是配我们这一发的 */

    if (lwip_sendto(sock, req, sizeof(req), 0, (struct sockaddr *)&dst, sizeof(dst)) < 0) {
        ESP_LOGW(TAG, "NTP %s 发送失败   [%s]", name, sync_str());
        lwip_close(sock);
        return;
    }

    uint8_t rep[DIAG_NTP_PKT_LEN + 16];
    struct sockaddr_in from;
    socklen_t fromlen = sizeof(from);
    TickType_t t0 = xTaskGetTickCount();
    int n = lwip_recvfrom(sock, rep, sizeof(rep), 0, (struct sockaddr *)&from, &fromlen);
    uint32_t ms = (uint32_t)((xTaskGetTickCount() - t0) * portTICK_PERIOD_MS);
    lwip_close(sock);

    if (n < 0) {
        ESP_LOGW(TAG, "NTP %-20s 超时(>%dms)   [%s]", name, DIAG_NTP_RECV_MS, sync_str());
        return;
    }
    if (n < DIAG_NTP_PKT_LEN) {
        ESP_LOGW(TAG, "NTP %-20s 回包太短 n=%d   [%s]", name, n, sync_str());
        return;
    }

    /* 回包必须来自我们问的那个地址。不是的话是异常（NAT/中间设备作怪），单独报。
     * ⚠️ 这里两个地址各用各的缓冲区：ip4addr_ntoa 用的是静态缓冲，同一条语句里
     *    调两次会互相覆盖。 */
    if (from.sin_addr.s_addr != dst.sin_addr.s_addr) {
        ip4_addr_t f4;
        char a[16], b[16];
        f4.addr = from.sin_addr.s_addr;
        ip4addr_ntoa_r(&f4, a, sizeof(a));
        ip4addr_ntoa_r(ip_2_ip4(addr), b, sizeof(b));
        ESP_LOGW(TAG, "NTP %-20s 回包来自别的地址 %s（目标是 %s）   [%s]", name, a, b, sync_str());
        return;
    }

    int mode     = rep[0] & 0x07;               /* 模式在字节 0 的低 3 位 */
    int li       = (rep[0] >> 6) & 0x03;        /* 3 = 服务器自己还没同步 */
    bool echo_ok = (memcmp(&rep[24], &req[40], sizeof(s_ntp_tag)) == 0);

    /* 判定只认 mode==4（服务器回复）。序号对不对只当**附注**报出来：
     * 万一某家服务端不抄 originate 字段，报成"异常"会把"有回复"这个关键事实盖掉。 */
    if (mode != 4) {
        ESP_LOGW(TAG, "NTP %-20s 回包异常 mode=%d li=%d n=%d   [%s]",
                 name, mode, li, n, sync_str());
        return;
    }
    ESP_LOGI(TAG, "NTP %-20s 回复 %4ums li=%d echo=%s   [%s]",
             name, (unsigned)ms, li, echo_ok ? "ok" : "no", sync_str());
}

/* NTP 自测的目标表（解析成功才入表） */
typedef struct {
    const char *name;
    ip_addr_t   addr;
} ntp_tgt_t;

static ntp_tgt_t s_ntp[DIAG_NTP_MAX];
static int       s_ntp_cnt;

static void ntp_add(const char *name, const ip_addr_t *addr)
{
    if (s_ntp_cnt >= DIAG_NTP_MAX) {
        return;
    }
    s_ntp[s_ntp_cnt].name = name;
    s_ntp[s_ntp_cnt].addr = *addr;
    s_ntp_cnt++;
}

static void diag_task(void *arg)
{
    const char *server_host = (const char *)arg;

    /* ---- ICMP 目标①：SNTP 服务器（就是 SNTP 实际用的那个 IP）---- */
    ip_addr_t srv;
    if (resolve_v4(server_host, &srv)) {
        ESP_LOGI(TAG, "ICMP① SNTP 服务器 %s -> %s", server_host, ip4addr_ntoa(ip_2_ip4(&srv)));
        start_session("服务器", &srv);
    } else {
        ESP_LOGW(TAG, "  解析 %s 失败", server_host);
    }

    /* ---- ICMP 目标②：本机配置的 DNS 服务器（另一条路，而且是校内主机）---- */
    const ip_addr_t *dns = dns_getserver(0);
    if (dns == NULL || ip_addr_isany(dns)) {
        ESP_LOGW(TAG, "DNS 服务器地址未知");
    } else if (!IP_IS_V4(dns)) {
        ESP_LOGW(TAG, "DNS 服务器 %s 不是 IPv4，跳过", ipaddr_ntoa(dns));
    } else {
        ESP_LOGI(TAG, "ICMP② DNS 服务器 %s", ipaddr_ntoa(dns));
        start_session("DNS", dns);
    }

    /* ---- NTP 自测：把候选名单解析出来 ---- */
    for (int i = 0; i < DIAG_NTP_MAX; i++) {
        const char *host = DIAG_NTP_SERVERS[i];
        ip_addr_t ip;
        if (resolve_v4(host, &ip)) {
            ESP_LOGI(TAG, "NTP候选 %-20s -> %s", host, ip4addr_ntoa(ip_2_ip4(&ip)));
            ntp_add(host, &ip);
        } else {
            ESP_LOGW(TAG, "NTP候选 %-20s 解析失败，本轮起跳过", host);
        }
    }

    /* ---- 主循环：满 300 秒，每 30 秒一轮 ----
     * ⚠️ 时间基准用 tick、不用"循环次数"：一轮里 5 个 NTP 最多各等 1 秒，
     *    按次数算会把节拍和 300 秒窗口一起拖长（最多 350 秒）。
     *    tick 粒度是 portTICK_PERIOD_MS（默认 10 ms），够用。 */
    TickType_t t0     = xTaskGetTickCount();
    int elapsed_ms    = 0;
    int last_round_ms = -DIAG_ROUND_MS;     /* 让第一轮立刻开始 */
    int round         = 0;
    while (1) {
        elapsed_ms = (int)((xTaskGetTickCount() - t0) * portTICK_PERIOD_MS);
        if (elapsed_ms >= DIAG_MAX_MS) {
            break;
        }

        if ((elapsed_ms - last_round_ms) >= DIAG_ROUND_MS) {
            last_round_ms = elapsed_ms;
            round++;
            ESP_LOGI(TAG, "----- 第 %d 轮（t≈%ds，SNTP %s）-----",
                     round, elapsed_ms / 1000, sync_str());
            log_rssi();
            for (int i = 0; i < s_ntp_cnt; i++) {
                ntp_probe_once(s_ntp[i].name, &s_ntp[i].addr);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(200));
    }

    /* ---- 收工 ----
     * ⚠️ 顺序必须是 stop → delete，两个位管的是不同的事：
     *    esp_ping_stop()           清 START 位 -> ping 任务内层循环的条件不成立，退出
     *    esp_ping_delete_session() 清 INIT  位 -> 它的外层循环下一次超时后 break、
     *                                            释放 socket/内存并自删任务
     *    count=0（不限次数）的会话**只**清 INIT 是停不下来的 —— 内层循环条件是
     *    "START 还在"，光清 INIT 它照跑不误。
     *    删完就不要再碰 hdl：ping 任务会把它 free 掉（ping_sock.c 的 esp_ping_thread 末尾）。 */
    for (int i = 0; i < s_cnt; i++) {
        esp_ping_stop(s_hdl[i]);
        esp_ping_delete_session(s_hdl[i]);
    }
    ESP_LOGI(TAG, "探测结束（跑满 %d 轮 / t≈%ds）", round, elapsed_ms / 1000);

    vTaskDelete(NULL);
}

void net_time_diag_start(const char *server_host)
{
    if (s_started) {
        return;         /* 只在首次对时时探一次 */
    }
    s_started = true;

    if (xTaskCreate(diag_task, "diag_downlink", DIAG_TASK_STACK,
                    (void *)server_host, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "创建探测任务失败");
        s_started = false;
    }
}

void net_time_diag_note_synced(void)
{
    if (!s_synced) {
        ESP_LOGI(TAG, "SNTP 已对时（后续每轮会有标记）");
    }
    s_synced = true;
}
