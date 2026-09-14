/*
 * 时钟 App
 *
 * 和前面两个 App 最大的不同：它需要一个"每秒触发的 LVGL 定时器"。
 *
 * ⚠️ 定时器**不属于任何 screen** —— 它挂在 LVGL 全局的定时器链表上。
 *    所以 leave 里必须显式删掉它，否则 screen 释放之后定时器还在跑，
 *    回调里去操作已释放的对象 -> 崩（而且是"进去几秒后才崩"，很难查）。
 *    记住这个模式：enter 申请的、游离于 screen 之外的资源，leave 里一个都不能漏。
 */
#include "clock.h"

#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"

#include "app_manager.h"
#include "time_service.h"

static const char *TAG = "clock";

static lv_obj_t   *s_scr        = NULL;
static lv_obj_t   *s_time_label = NULL;
static lv_timer_t *s_timer      = NULL;

/* 把当前时间刷到 label 上。未对过时显示占位符，而不是把假时间当真显示出去。 */
static void update_time(void)
{
    struct tm now;
    if (time_service_get(&now) == ESP_OK) {
        lv_label_set_text_fmt(s_time_label, "%02d:%02d:%02d",
                              now.tm_hour, now.tm_min, now.tm_sec);
    } else {
        lv_label_set_text(s_time_label, "--:--:--");
    }
}

/*
 * 每秒被调用一次。它运行在 LVGL 任务里（由 lv_timer_handler 驱动），
 * 和点击回调是同一个上下文，所以不需要再加 LVGL 锁。
 */
static void on_tick(lv_timer_t *timer)
{
    (void)timer;
    update_time();
}

static void on_back_clicked(lv_event_t *e)
{
    (void)e;
    app_manager_go_home();
}

static void clock_enter(void)
{
    ESP_LOGI(TAG, "enter");

    lvgl_port_lock(0);

    s_scr = lv_obj_create(NULL);

    lv_obj_t *title = lv_label_create(s_scr);
    lv_label_set_text(title, "Clock");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 24);

    s_time_label = lv_label_create(s_scr);
    /* 不显式设字体，用 LVGL 默认字体（montserrat_14）——
     * 这样就不用为了这个 App 去开 CONFIG_LV_FONT_MONTSERRAT_48 了。 */
    lv_obj_align(s_time_label, LV_ALIGN_CENTER, 0, -30);
    update_time();      /* 先立刻显示一次，不然会空白 1 秒 */

    lv_obj_t *btn_back = lv_button_create(s_scr);
    lv_obj_set_size(btn_back, 200, 64);
    lv_obj_align(btn_back, LV_ALIGN_BOTTOM_MID, 0, -40);
    lv_obj_add_event_cb(btn_back, on_back_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lb = lv_label_create(btn_back);
    lv_label_set_text(lb, "Back");
    lv_obj_center(lb);

    s_timer = lv_timer_create(on_tick, 1000, NULL);   /* 每秒刷新 */

    lv_screen_load(s_scr);

    lvgl_port_unlock();
}

static void clock_leave(void)
{
    ESP_LOGI(TAG, "leave");

    lvgl_port_lock(0);

    /* ⚠️ 必须先删定时器：
     *    它不属于 s_scr，删 screen 不会带走它。
     *    lv_timer_create 可能因内存不足返回 NULL，而 lv_timer_delete 不判空，所以要判一下。 */
    if (s_timer != NULL) {
        lv_timer_delete(s_timer);
        s_timer = NULL;
    }

    lv_obj_delete_async(s_scr);
    s_scr = NULL;
    s_time_label = NULL;

    lvgl_port_unlock();
}

static const app_desc_t s_desc = {
    .name  = "Clock",
    .icon  = "",
    .enter = clock_enter,
    .leave = clock_leave,
};

void clock_register(void)
{
    app_manager_register(&s_desc);
}
