/*
 * 测试 App —— 用来验证 app_manager 的调度逻辑 + kv_store 的持久化
 *
 * 页面上每样东西都对应一个验证点：
 *   1. 标题        —— 确认"进到这个 App 了"
 *   2. "count: N"  —— 本次进入点了多少次。点几下再返回、再进来，它应回到 0，
 *                     说明页面真的被重建了（验证 leave 的释放生效）
 *   3. "total: M"  —— 累计点击数，存在 NVS 里。**重启之后应接着累加**，
 *                     说明 kv_store 真的落盘了（验证持久化生效）
 *
 * 2 和 3 是两个独立的验证点，故意不合并：如果把 count 也持久化，就再也看不出
 * 页面有没有被重建了。
 *
 * "返回"不再由本 App 自己画按钮了 —— 交给顶部的公共状态栏，所以这里少了一样东西。
 */
#include "app_demo.h"

#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"

#include "app_manager.h"
#include "kv_store.h"
#include "ui_status_bar.h"
#include "picture.h"

static const char *TAG = "app_demo";

static lv_obj_t *s_scr         = NULL;
static lv_obj_t *s_count_label = NULL;
static lv_obj_t *s_total_label = NULL;
static int       s_count       = 0;     /* 本次进入点了多少次（只在内存里） */
static int32_t   s_total       = 0;     /* 累计点击数（落在 NVS 的 "demo"/"total"） */

static void on_count_clicked(lv_event_t *e)
{
    (void)e;

    s_count++;
    s_total++;
    lv_label_set_text_fmt(s_count_label, "count: %d", s_count);
    lv_label_set_text_fmt(s_total_label, "total: %d", (int)s_total);

    /* 每次点击就落盘：所见即所存，验证起来最直接。
     * 人类点击的频率远够不上 kv_store 头文件里警告的"高频写"，不用担心 flash 寿命。
     * 失败只告警不中断 —— 界面上的计数还得继续。
     *
     * ⚠️ 这一行是在 LVGL 锁里做 flash 写。单次小数据只要几毫秒，测试 App 无所谓；
     *    真要在正式 App 里频繁落盘，应该把写操作挪到独立任务，不要占着 LVGL 锁。 */
    const esp_err_t err = kv_write_i32("demo", "total", s_total);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "persist total failed: %s", esp_err_to_name(err));
    }
}

static void app_demo_enter(void)
{
    ESP_LOGI(TAG, "enter");

    /* ---- 先把要显示的数据准备好，再动 LVGL ---- */

    s_count = 0;                        /* 本次计数每次进入都归零（这就是验证点 2） */

    /* 累计计数从存储里读。读不到（ESP_ERR_NVS_NOT_FOUND = 从没存过）就是首次运行，
     * 保持 0 即可 —— 这不是错误。
     * namespace 直接用 App 名：这是本 App 私有的数据，不需要抽成常量。 */
    s_total = 0;
    const esp_err_t err = kv_read_i32("demo", "total", &s_total);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "no stored total (%s), starting from 0", esp_err_to_name(err));
    }

    /* ---- 界面 ---- */
    lvgl_port_lock(0);

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
    lv_label_set_text_fmt(s_count_label, "count: %d", s_count);
    lv_obj_align(s_count_label, LV_ALIGN_CENTER, 0, -60);

    s_total_label = lv_label_create(s_scr);
    lv_label_set_text_fmt(s_total_label, "total: %d", (int)s_total);
    lv_obj_align(s_total_label, LV_ALIGN_CENTER, 0, -20);

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
    s_total_label = NULL;
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
