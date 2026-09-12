/*
 * UI 层 —— demo 界面
 * 一个标题 + 一个计数标签 + 一个按钮，用来验证显示和触摸都通了。
 */
#include "ui_demo.h"

#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"

static const char *TAG = "ui_demo";

static lv_obj_t *s_count_label = NULL;
static int s_click_count = 0;

/*
 * 按钮回调。它运行在 LVGL 任务上下文里，此时 LVGL 锁已被 lvgl_port 持有，
 * 所以回调里不要再调用 lvgl_port_lock()，否则会死锁。
 */
static void on_button_clicked(lv_event_t *e)
{
    (void)e;
    s_click_count++;
    lv_label_set_text_fmt(s_count_label, "Clicked: %d", s_click_count);
}

void ui_demo_start(void)
{
    /* 操作 LVGL 对象前必须持有 LVGL 锁 */
    lvgl_port_lock(0);

    lv_obj_t *scr = lv_screen_active();

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "Hello LVGL 9 on ESP32-P4");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 40);

    s_count_label = lv_label_create(scr);
    lv_label_set_text(s_count_label, "Clicked: 0");
    lv_obj_align(s_count_label, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t *btn = lv_button_create(scr);
    lv_obj_set_size(btn, 200, 70);
    lv_obj_align(btn, LV_ALIGN_BOTTOM_MID, 0, -80);
    lv_obj_add_event_cb(btn, on_button_clicked, LV_EVENT_CLICKED, NULL);

    lv_obj_t *btn_label = lv_label_create(btn);
    lv_label_set_text(btn_label, "Click me");
    lv_obj_center(btn_label);

    lvgl_port_unlock();

    ESP_LOGI(TAG, "demo UI created");
}
