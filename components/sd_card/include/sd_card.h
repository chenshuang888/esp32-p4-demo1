/*
 * SD 卡 —— 挂载
 * 板子: Waveshare ESP32-P4-WIFI6-Touch-LCD-7B（SDMMC Slot 0，IOMUX 固定引脚）
 *
 * 这个组件只做一件事：**把卡挂上 FATFS**。
 * 挂载之后 App 直接用标准 C 文件 API：
 *
 *     FILE *f = fopen(SD_CARD_MOUNT_POINT "/log.txt", "w");
 *     DIR  *d = opendir(SD_CARD_MOUNT_POINT);
 *
 * 刻意不提供 read/write/selftest 之类的包装：文件系统的"能力"就是 POSIX
 * 那一套，newlib 已经把最好的 API 给我们了，再包一层只会更差。
 * （对比 kv_store：那边必须包，因为 NVS 的 init 有一段恢复逻辑、"没存过"
 *   的语义也要统一；FATFS 挂载之后没有这类负担。）
 *
 * ⚠️ 本板没有卡检测（Card Detect）引脚 —— 厂家 BSP 同样是 SDMMC_SLOT_NO_CD。
 *    所以卡被拔出、中途插入都无从得知，"现在卡还在不在"没有可靠的查询方式。
 *    因此本组件**不提供** is_mounted() 之类的接口：那个值只能是"启动时挂上了"，
 *    写成 is_xxx 会误导使用者。App 侧遇到文件操作失败按"不可用"降级即可。
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* FATFS 挂载点。挂载后拼上文件名即可访问，如 SD_CARD_MOUNT_POINT "/a.txt" */
#define SD_CARD_MOUNT_POINT  "/sdcard"

/* 照片目录：拍照（camera App）往里写，相册（photo App）从里读，两边共用这一个定义。
 * 放在这里而不是某个 App 里，是因为它既不属于拍照也不属于相册，而两边本来就要
 * 依赖本组件拿挂载点 —— 与其在两边各写一份字面量（demo1 就是三份），不如和挂载点放一起。 */
#define SD_CARD_PHOTO_DIR    SD_CARD_MOUNT_POINT "/DCIM"

/**
 * @brief 初始化 SD 卡并把 FATFS 挂载到 SD_CARD_MOUNT_POINT
 *
 * 一条龙（esp_vfs_fat_sdmmc_mount）：
 *   供电（P4 片上 LDO 通道 4）
 *   → SDMMC Slot 0（IOMUX 固定引脚，4-bit，HIGHSPEED）
 *   → 探测卡 → 挂载 FATFS → 注册 VFS。
 * 成功后即可用标准 C 文件 API 访问 SD_CARD_MOUNT_POINT。
 *
 * 失败是**正常情况**（没插卡）。调用方打条警告继续跑即可，
 * 不要用 ESP_ERROR_CHECK —— 插不插卡不该决定能不能开机。
 * 已经挂载过时重复调用直接返回 ESP_OK。
 *
 * 挂载失败**不会**格式化卡（format_if_mount_failed = false）：
 * 卡上若有 FATFS 不认识的分区（例如 64GB 以上卡出厂默认的 exFAT），
 * 这里只会返回错误。要格式化成 FAT32 请在 PC 上做 —— 那样不会误伤数据。
 *
 * @return ESP_OK  成功（已挂载，可立即 fopen）；或此前已经挂载过
 *         其余为 esp_err_t 错误码（无卡、分区格式不认识等）
 */
esp_err_t sd_card_init(void);

#ifdef __cplusplus
}
#endif
