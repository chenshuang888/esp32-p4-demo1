/*
 * net_time_diag —— 【临时诊断】SNTP 偶发失败的定位探测
 *
 * 用途和判据写在 net_time_diag.c 顶部（结论也在那里）。
 *
 * ⚠️ 本文件当前**不参与构建**：问题已定位并修掉（服务器换成 ntp.tencent.com），
 *    而探测每架次会固定刷 300 秒日志，所以停用、留在仓库里备查。
 *    重新启用要三处一起放开 —— 见 net_time/CMakeLists.txt 顶部，
 *    或 net_time_diag.c 顶部同一份说明。
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 起一个探测任务（固定跑满 5 分钟，每 30 秒一轮）
 *
 * 每轮：打一行轮次头 + 读一次 RSSI + 对**每个候选 NTP 服务器**发一个全新的
 * UDP 123 请求（mode-3，1 秒超时），看有没有 mode-4 回包。
 * 另外 ICMP 探两个目标（SNTP 服务器 + 本机 DNS 服务器）全程每 5 秒一发 ——
 * 它的作用是证明"这 5 分钟里下行一直是好的"。
 *
 * 故意不提前收工：要的是**每个服务器的成功率统计**，固定窗口才可比。
 *
 * 只在首次对时时真正起任务，重复调用无效（本组件挂的钩子每次重连都会进来）。
 *
 * @param server_host SNTP 服务器的主机名。由 net_time.c 把它传进来，
 *                    免得两处各维护一份、日后改了一处忘了另一处。
 *                    （DNS 服务器不用传：探测自己用 lwip 的 dns_getserver(0) 取；
 *                      候选 NTP 名单写在 net_time_diag.c 里。）
 */
void net_time_diag_start(const char *server_host);

/**
 * @brief 告诉探测"这一架次已经对时成功了" —— 由 net_time.c 的 SNTP 通知回调调用
 *
 * ⚠️ 必须有这一个：探测要靠它判断"什么时候可以收工 / 日志里那一行该标已对时"，
 *    而**墙上时钟不能用来判断**。本工程 CONFIG_NEWLIB_TIME_SYSCALL_USE_RTC_HRT=y，
 *    系统时间存在 RTC_STORE 寄存器里（newlib/Kconfig:102-103），**上一次开机同步到的
 *    时间会跨软复位留下来** —— 实测见过：开机第 1 秒墙上时钟就已经 >2020 年，
 *    而 SNTP 到 2.8 秒后才真正同步。拿它当判据会让探测在开机 31 秒就误收工，
 *    把整个失败窗口漏掉。
 *
 * 这个标志是"本架次真的收到过服务器时间"的唯一可靠来源（回调只在成功时触发）。
 */
void net_time_diag_note_synced(void);

#ifdef __cplusplus
}
#endif
