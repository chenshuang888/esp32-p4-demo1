/*
 * 屏幕 —— 触摸部分实现（GT911, I2C）
 * 板子: Waveshare ESP32-P4-WIFI6-Touch-LCD-7B
 *
 * 注意: GT911 的 I2C 地址由复位时序决定，可能是 0x5D 或 0x14，
 *       所以要两个地址都探测一遍（厂家 BSP 也是这么做的）。
 */
#include "lcd_screen_touch.h"

#include "esp_check.h"
#include "esp_log.h"
#include "driver/i2c_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch_gt911.h"

static const char *TAG = "lcd_screen";

/* ================= 板级参数 ================= */
#define TOUCH_I2C_PORT      1
#define TOUCH_PIN_SCL       8
#define TOUCH_PIN_SDA       7
#define TOUCH_I2C_SPEED_HZ  400000

/* 复位/中断脚本板未引出（复位在硬件上与 LCD 复位共用） */
#define TOUCH_PIN_RST       (-1)
#define TOUCH_PIN_INT       (-1)

#define TOUCH_X_MAX         1024
#define TOUCH_Y_MAX         600

static esp_lcd_touch_handle_t s_touch = NULL;

esp_err_t lcd_screen_touch_init(void)
{
    /* 1) I2C 总线 */
    const i2c_master_bus_config_t bus_cfg = {
        .clk_source                   = I2C_CLK_SRC_DEFAULT,
        .sda_io_num                   = TOUCH_PIN_SDA,
        .scl_io_num                   = TOUCH_PIN_SCL,
        .i2c_port                     = TOUCH_I2C_PORT,
        .glitch_ignore_cnt            = 7,
        .flags                        = { .enable_internal_pullup = true },
    };
    i2c_master_bus_handle_t i2c_bus = NULL;
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &i2c_bus), TAG, "new I2C master bus");

    /* 2) 探测 GT911：0x5D 优先，其次 0x14 */
    esp_lcd_panel_io_i2c_config_t io_cfg = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
    if (i2c_master_probe(i2c_bus, ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS, 100) == ESP_OK) {
        ESP_LOGI(TAG, "GT911 found at 0x%02X", ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS);
    } else if (i2c_master_probe(i2c_bus, ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS_BACKUP, 100) == ESP_OK) {
        ESP_LOGI(TAG, "GT911 found at 0x%02X", ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS_BACKUP);
        io_cfg.dev_addr = ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS_BACKUP;
    } else {
        ESP_LOGE(TAG, "GT911 not found on I2C (SDA=%d SCL=%d)", TOUCH_PIN_SDA, TOUCH_PIN_SCL);
        return ESP_ERR_NOT_FOUND;
    }
    io_cfg.scl_speed_hz = TOUCH_I2C_SPEED_HZ;

    /* 3) 触摸 IO + GT911 驱动 */
    esp_lcd_panel_io_handle_t tp_io = NULL;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(i2c_bus, &io_cfg, &tp_io), TAG, "new touch io");

    const esp_lcd_touch_config_t touch_cfg = {
        .x_max        = TOUCH_X_MAX,
        .y_max        = TOUCH_Y_MAX,
        .rst_gpio_num = TOUCH_PIN_RST,
        .int_gpio_num = TOUCH_PIN_INT,
        .levels       = { .reset = 0, .interrupt = 0 },
        .flags        = {
            .swap_xy  = 0,
            .mirror_x = 1,      /* 方向按本板调整 */
            .mirror_y = 1,
        },
    };
    ESP_RETURN_ON_ERROR(esp_lcd_touch_new_i2c_gt911(tp_io, &touch_cfg, &s_touch), TAG, "new GT911");

    ESP_LOGI(TAG, "touch ready");
    return ESP_OK;
}

esp_lcd_touch_handle_t lcd_screen_touch_handle(void)
{
    return s_touch;
}
