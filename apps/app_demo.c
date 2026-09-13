/*
 * 测试 App —— 用来验证 app_manager 的调度逻辑
 *
 * 页面上只有三样东西，每样都对应一个验证点：
 *   1. 标题            —— 确认"进到这个 App 了"
 *   2. "+1" 计数        —— 点几下再返回、再进来，计数应回到 0，
 *                          说明页面真的被重建了（验证 leave 的释放生效）
 *   3. "Back" 按钮      —— 走 app_manager_go_home()，验证回主页这条路
 */
#include "app_demo.h"

#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"

#include "app_manager.h"

static const char *TAG = "app_demo";

static lv_obj_t *s_scr = NULL;
static lv_obj_t *s_count_label = NULL;
static int s_count = 0;

static void on_back_clicked(lv_event_t *e)
{
    (void)e;
    app_manager_go_home();
}

static void on_count_clicked(lv_event_t *e)
{
    (void)e;
    s_count++;
    lv_label_set_text_fmt(s_count_label, "count: %d", s_count);
}

static void app_demo_enter(void)
{
    ESP_LOGI(TAG, "enter");

    lvgl_port_lock(0);

    s_count = 0;                          /* 每次进入都是全新页面 */

    s_scr = lv_obj_create(NULL);

    lv_obj_t *title = lv_label_create(s_scr);
    lv_label_set_text(title, "Demo App");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 40);

    s_count_label = lv_label_create(s_scr);
    lv_label_set_text(s_count_label, "count: 0");
    lv_obj_align(s_count_label, LV_ALIGN_CENTER, 0, -50);

    lv_obj_t *btn_count = lv_button_create(s_scr);
    lv_obj_set_size(btn_count, 200, 64);
    lv_obj_align(btn_count, LV_ALIGN_CENTER, 0, 40);
    lv_obj_add_event_cb(btn_count, on_count_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lc = lv_label_create(btn_count);
    lv_label_set_text(lc, "+1");
    lv_obj_center(lc);

    lv_obj_t *btn_back = lv_button_create(s_scr);
    lv_obj_set_size(btn_back, 200, 64);
    lv_obj_align(btn_back, LV_ALIGN_BOTTOM_MID, 0, -40);
    lv_obj_add_event_cb(btn_back, on_back_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lb = lv_label_create(btn_back);
    lv_label_set_text(lb, "Back");
    lv_obj_center(lb);

    lv_screen_load(s_scr);

    lvgl_port_unlock();
}

static void app_demo_leave(void)
{
    ESP_LOGI(TAG, "leave");

    lvgl_port_lock(0);
    /* 点 Back 时本回调正跑在 s_scr 内的按钮事件上，必须异步删除 */
    lv_obj_delete_async(s_scr);
    s_scr = NULL;
    s_count_label = NULL;
    lvgl_port_unlock();
}

static const app_desc_t s_desc = {
    .name  = "Demo",
    .icon  = "",
    .enter = app_demo_enter,
    .leave = app_demo_leave,
};

void app_demo_register(void)
{
    app_manager_register(&s_desc);
}
