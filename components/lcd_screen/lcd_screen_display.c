/*
 * 屏幕 —— 显示部分实现
 * 板子: Waveshare ESP32-P4-WIFI6-Touch-LCD-7B (EK79007, MIPI-DSI, 1024x600)
 *
 * 初始化顺序（和厂家做法一致）:
 *   背光 PWM -> 打开 MIPI DSI PHY 供电(LDO) -> 建立 DSI 总线
 *   -> DBI 通道(发命令) -> DPI 面板 + EK79007 驱动 -> reset -> init
 *
 * 本组件只做硬件初始化，刷屏由上层（LVGL）负责。
 */
#include "lcd_screen_display.h"

#include "esp_check.h"
#include "esp_log.h"
#include "driver/ledc.h"
#include "esp_ldo_regulator.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_ek79007.h"

static const char *TAG = "lcd_screen";

/* ================= 板级参数 ================= */
#define LCD_COLOR_BITS      16      /* RGB565 */
#define LCD_PIXEL_FORMAT    LCD_COLOR_PIXEL_FORMAT_RGB565
#define LCD_PIN_RST         33      /* 面板复位 */

/* 帧缓冲个数。>=2 才能配合 LVGL 的 avoid_tearing 做"帧缓冲交换"防撕裂 */
#define LCD_DPI_BUFFER_NUM  2

/* 背光: LEDC PWM（本板为低电平点亮，故 output_invert） */
#define LCD_PIN_BACKLIGHT   32
#define LCD_LEDC_TIMER      LEDC_TIMER_1
#define LCD_LEDC_CHANNEL    LEDC_CHANNEL_1
#define LCD_LEDC_RES        LEDC_TIMER_10_BIT
#define LCD_LEDC_FREQ_HZ    5000
#define LCD_LEDC_DUTY_MAX   1023    /* 10bit -> 1023 = 100% */

/* MIPI DSI PHY 供电: LDO_VO3 -> VDD_MIPI_DPHY */
#define LCD_LDO_CHAN        3
#define LCD_LDO_VOLTAGE_MV  2500

static esp_lcd_panel_handle_t s_panel = NULL;
static esp_lcd_panel_io_handle_t s_io = NULL;

static esp_err_t backlight_init(void)
{
    const ledc_timer_config_t timer_cfg = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LCD_LEDC_RES,
        .timer_num       = LCD_LEDC_TIMER,
        .freq_hz         = LCD_LEDC_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer_cfg), TAG, "backlight timer config");

    const ledc_channel_config_t channel_cfg = {
        .gpio_num   = LCD_PIN_BACKLIGHT,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = LCD_LEDC_CHANNEL,
        .intr_type  = LEDC_INTR_DISABLE,
        .timer_sel  = LCD_LEDC_TIMER,
        .duty       = 0,
        .hpoint     = 0,
        .flags      = { .output_invert = 1 },
    };
    return ledc_channel_config(&channel_cfg);
}

esp_err_t lcd_screen_display_backlight(uint8_t percent)
{
    if (percent > 100) {
        percent = 100;
    }
    uint32_t duty = (uint32_t)LCD_LEDC_DUTY_MAX * percent / 100;
    ESP_RETURN_ON_ERROR(ledc_set_duty(LEDC_LOW_SPEED_MODE, LCD_LEDC_CHANNEL, duty), TAG, "set duty");
    return ledc_update_duty(LEDC_LOW_SPEED_MODE, LCD_LEDC_CHANNEL);
}

esp_err_t lcd_screen_display_init(void)
{
    /* 1) 背光 PWM（先建好，亮度由 lcd_screen_display_backlight() 控制） */
    ESP_RETURN_ON_ERROR(backlight_init(), TAG, "backlight init");

    /* 2) 打开 MIPI DSI PHY 供电 */
    esp_ldo_channel_handle_t ldo_chan = NULL;
    const esp_ldo_channel_config_t ldo_cfg = {
        .chan_id    = LCD_LDO_CHAN,
        .voltage_mv = LCD_LDO_VOLTAGE_MV,
    };
    ESP_RETURN_ON_ERROR(esp_ldo_acquire_channel(&ldo_cfg, &ldo_chan), TAG, "acquire DSI PHY LDO");

    /* 3) MIPI DSI 总线 */
    esp_lcd_dsi_bus_handle_t dsi_bus = NULL;
    const esp_lcd_dsi_bus_config_t bus_cfg = EK79007_PANEL_BUS_DSI_2CH_CONFIG();
    ESP_RETURN_ON_ERROR(esp_lcd_new_dsi_bus(&bus_cfg, &dsi_bus), TAG, "new DSI bus");

    /* 4) DBI 通道（用于发送面板命令），句柄保留给上层 */
    const esp_lcd_dbi_io_config_t dbi_cfg = EK79007_PANEL_IO_DBI_CONFIG();
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_dbi(dsi_bus, &dbi_cfg, &s_io), TAG, "new DBI io");

    /* 5) DPI 面板配置 + EK79007 驱动 */
    esp_lcd_dpi_panel_config_t dpi_cfg = EK79007_1024_600_PANEL_60HZ_CONFIG(LCD_PIXEL_FORMAT);
    dpi_cfg.num_fbs = LCD_DPI_BUFFER_NUM;

    ek79007_vendor_config_t vendor_cfg = {
        .mipi_config = {
            .dsi_bus    = dsi_bus,
            .dpi_config = &dpi_cfg,
        },
    };
    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = LCD_PIN_RST,
        .rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = LCD_COLOR_BITS,
        .vendor_config  = &vendor_cfg,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_ek79007(s_io, &panel_cfg, &s_panel), TAG, "new panel EK79007");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "panel reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "panel init");

    /*
     * 刻意不注册 on_color_trans_done 回调。
     * esp_lcd_dpi_panel_register_event_callbacks() 是"独占覆盖"式的，
     * 后续 LVGL 的 esp_lvgl_port 会注册它自己的；谁后注册谁生效。
     * 我们不抢这个回调，避免被覆盖后失去同步信号。
     */

    ESP_LOGI(TAG, "display ready: %dx%d RGB565", LCD_SCREEN_H_RES, LCD_SCREEN_V_RES);
    return ESP_OK;
}

esp_lcd_panel_handle_t lcd_screen_display_handle(void)
{
    return s_panel;
}

esp_lcd_panel_io_handle_t lcd_screen_display_io_handle(void)
{
    return s_io;
}
