/*
 * SD 卡 —— 挂载实现
 * 板子: Waveshare ESP32-P4-WIFI6-Touch-LCD-7B
 *
 * 参数取自实测 + 厂家 BSP
 * （Waveshare-ESP32-components/bsp/esp32_p4_wifi6_touch_lcd_7b/esp32_p4_wifi6_touch_lcd_7b.c），
 * 唯一和厂家不同的一处是簇大小，理由写在 mount 配置那里。
 */
#include "sd_card.h"

#include "esp_check.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"

static const char *TAG = "sd_card";

/* 挂上的卡句柄。本板无 CD 引脚，它同时兼作"启动时挂载成功过"的标志 */
static sdmmc_card_t *s_card = NULL;

esp_err_t sd_card_init(void)
{
    if (s_card != NULL) {
        return ESP_OK;              /* 幂等：已经挂过了 */
    }

    /* 1) 卡供电：P4 片上 LDO 通道 4（微雪官方 BSP 做法，不去碰 GPIO45）
     *
     * ⚠️ 这里必然会打一条
     *      W ldo: The voltage value 0 is out of the recommended range [500, 2700]
     *    **可以忽略，且无法通过配置消除**：
     *    - sd_pwr_ctrl_ldo_config_t 只有 ldo_chan_id 一个字段（见 sd_pwr_ctrl_by_on_chip_ldo.h），
     *      公开 API 里没有地方传电压；
     *    - IDF 在 sd_pwr_ctrl_by_on_chip_ldo.c 里刻意不填 voltage_mv，把通道以 adjustable
     *      模式获取，电压稍后由 SDMMC 驱动按速度模式自行调整（源码注释原话：
     *      "adjust the voltage later according to different speed mode"）；
     *    - esp_ldo_regulator.c 里那句只是 range 检查的 ESP_LOGW，**不会中断**通道获取。
     *    厂家 BSP 走同一条路，同样会打这条警告。卡能跑到 HIGHSPEED 就说明电压是对的。 */
    const sd_pwr_ctrl_ldo_config_t ldo_cfg = {
        .ldo_chan_id = 4,
    };
    sd_pwr_ctrl_handle_t pwr_ctrl = NULL;
    ESP_RETURN_ON_ERROR(sd_pwr_ctrl_new_on_chip_ldo(&ldo_cfg, &pwr_ctrl), TAG,
                        "on-chip LDO4 init failed");

    /* 2) SDMMC host。Slot 0 走 IOMUX 固定引脚，所以不需要指定 clk/cmd/d0-d3 */
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_0;
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;
    host.pwr_ctrl_handle = pwr_ctrl;

    /* 3) 本板既没有卡检测也没有写保护引脚（和厂家 BSP 一致） */
    const sdmmc_slot_config_t slot_cfg = {
        .cd    = SDMMC_SLOT_NO_CD,
        .wp    = SDMMC_SLOT_NO_WP,
        .width = 4,
        .flags = 0,
    };

    /* 4) 挂载
     *
     * format_if_mount_failed = false 是**刻意**的：卡上若是 FATFS 不认识的分区
     * （64GB 以上的卡出厂默认是 exFAT），打开这个开关会直接整卡格式化，PC 上的
     * 数据全没。宁可挂载失败，让调用方去 PC 上处理。
     *
     * 32KB 簇：allocation_unit_size = 0 会退化成 512B/簇，8GB 卡会产生 1490 万簇、
     * 两个 FAT 表共 120MB，f_mkfs 要写 4 分钟；实测裸写 16KB→64KB 块带宽
     * 2.7→5.5MB/s，32KB 落在中段。选 32KB 还因为它正好是标准 FAT32 的簇上限，
     * Windows 原生支持 —— 厂家 BSP 用的 64KB 属于非标准簇，Windows 不一定认。 */
    const esp_vfs_fat_mount_config_t mount_cfg = {
        .format_if_mount_failed    = false,
        .max_files                 = 5,
        .allocation_unit_size      = 32768,
        .disk_status_check_enable  = false,
        .use_one_fat               = false,
    };

    const esp_err_t err = esp_vfs_fat_sdmmc_mount(SD_CARD_MOUNT_POINT, &host, &slot_cfg,
                                                  &mount_cfg, &s_card);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mount failed: %s (卡插了吗？卡上是 FAT 分区吗？)",
                 esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "mounted at %s (max=%lukHz real=%lukHz)",
             SD_CARD_MOUNT_POINT,
             (unsigned long)s_card->max_freq_khz,
             (unsigned long)s_card->real_freq_khz);
    return ESP_OK;
}
