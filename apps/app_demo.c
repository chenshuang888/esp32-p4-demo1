/*
 * 测试 App —— 用来验证 app_manager 的调度逻辑
 *
 * 页面上只剩两样东西，每样都对应一个验证点：
 *   1. 标题            —— 确认"进到这个 App 了"
 *   2. "+1" 计数        —— 点几下再返回、再进来，计数应回到 0，
 *                          说明页面真的被重建了（验证 leave 的释放生效）
 *
 * "返回"不再由本 App 自己画按钮了 —— 交给顶部的公共状态栏，所以这里少了一样东西。
 */
#include "app_demo.h"

#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"

#include "app_manager.h"
#include "ui_status_bar.h"
#include "picture.h"

static const char *TAG = "app_demo";

static lv_obj_t *s_scr = NULL;
static lv_obj_t *s_count_label = NULL;
static int s_count = 0;

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

    /* 状态栏：返回按钮由它提供，本 App 只管填标题 */
    const ui_status_bar_cfg_t bar = {
        .title     = "Demo App",
        .show_back = true,
    };
    ui_status_bar_apply(&bar);

    lv_obj_t *title = lv_label_create(s_scr);
    lv_label_set_text(title, "Demo App");
    /* 顶部要给状态栏让出 UI_STATUS_BAR_HEIGHT，否则会被它盖住 */
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, UI_STATUS_BAR_HEIGHT + 40);

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

    lv_screen_load(s_scr);

    lvgl_port_unlock();
}

static void app_demo_leave(void)
{
    ESP_LOGI(TAG, "leave");

    lvgl_port_lock(0);
    /* 点状态栏的 Back 时本回调正跑在 s_scr 内的按钮事件上，必须异步删除 */
    lv_obj_delete_async(s_scr);
    s_scr = NULL;
    s_count_label = NULL;
    lvgl_port_unlock();
}

static const app_desc_t s_desc = {
    .name  = "Demo",
    .icon  = PICTURE_ICON_DEMO,
    .enter = app_demo_enter,
    .leave = app_demo_leave,
};

void app_demo_register(void)
{
    app_manager_register(&s_desc);
}
