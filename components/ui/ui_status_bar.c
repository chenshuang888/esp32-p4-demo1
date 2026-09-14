/*
 * UI 层 —— 顶部状态栏实现
 *
 * 结构：三个控件都是状态栏的**直接子对象**，靠"标题撑开"来分开左右：
 *
 *   ┌──────────────────────────────────────────────────────┐
 *   │ [Back] Clock                          07:45          │
 *   └──────────────────────────────────────────────────────┘
 *     │      └ 标题 flex_grow=1：吃掉剩余宽度，把文本槽顶到最右
 *     └ 主页时隐藏它（flex 会自动跳过隐藏的子项，标题随之左移）
 *
 * 外观全部用 LVGL 默认主题（普通 lv_obj = card 样式：浅色底 + 边框），
 * 只把圆角改成 0，让它贴着屏幕顶部像一条状态栏，而不是一张浮起来的卡片。
 */
#include "ui_status_bar.h"

#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"

#include "lcd_screen_display.h"

static const char *TAG = "ui_status_bar";

/* 文本槽的刷新周期。只显示 HH:MM 的话 1 秒一次属于过量刷新，
 * 但 lv_label 的内容没变时 LVGL 会自己跳过重绘，代价可忽略，先不做优化。 */
#define TEXT_REFRESH_MS  1000

static lv_obj_t *s_bar      = NULL;     /* 状态栏本体（挂在 lv_layer_top 上） */
static lv_obj_t *s_back_btn = NULL;
static lv_obj_t *s_title    = NULL;
static lv_obj_t *s_text     = NULL;     /* 文本槽 */

/* 两个注入进来的外部动作 —— 本组件只知道"要调它们"，不知道它们是什么 */
static ui_status_bar_text_cb_t   s_text_cb     = NULL;
static ui_status_bar_action_cb_t s_back_action = NULL;

static void on_back_clicked(lv_event_t *e)
{
    (void)e;
    if (s_back_action != NULL) {
        s_back_action();
    }
}

/*
 * 定时器回调：跑在 LVGL 任务里（由 lv_timer_handler 驱动），
 * 所以下面不需要再加 LVGL 锁。
 *
 * 这里不用判 s_text_cb 是否为空 —— 定时器只在注入过 text_source 时才创建
 * （见 ui_status_bar_init），存在即非空。
 */
static void on_text_tick(lv_timer_t *timer)
{
    (void)timer;

    char buf[32] = { 0 };
    s_text_cb(buf, sizeof(buf));        /* 外部把要显示的字写进来 */
    buf[sizeof(buf) - 1] = '\0';        /* 防一手：外部没保证结尾时兜住 */
    lv_label_set_text(s_text, buf);     /* 内容没变时 LVGL 会跳过重绘 */
}

esp_err_t ui_status_bar_init(ui_status_bar_text_cb_t  text_source,
                             ui_status_bar_action_cb_t back_action)
{
    if (s_bar != NULL) {
        return ESP_OK;      /* 幂等：已经建过了 */
    }

    s_text_cb     = text_source;
    s_back_action = back_action;

    /* lvgl_port_lock 是递归锁，从任何上下文调用都不会死锁 */
    lvgl_port_lock(0);

    /* 建在顶层：它比所有 screen 都高，lv_screen_load() 切 App 影响不到它 */
    s_bar = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_bar, LCD_SCREEN_H_RES, UI_STATUS_BAR_HEIGHT);
    lv_obj_set_pos(s_bar, 0, 0);
    lv_obj_remove_flag(s_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(s_bar, 0, 0);       /* card 样式默认是圆角，贴顶用直角 */
    lv_obj_set_style_pad_all(s_bar, 0, 0);      /* 清掉 card 的 padding，自己控制内缩 */
    lv_obj_set_style_pad_hor(s_bar, 12, 0);
    lv_obj_set_flex_flow(s_bar, LV_FLEX_FLOW_ROW);
    /* 主轴上靠 START 依次排，再由标题 flex_grow 撑开剩余宽度，
     * 末尾的文本槽就被顶到最右边了 —— 所以不需要额外的容器。 */
    lv_obj_set_flex_align(s_bar, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_bar, 12, 0);

    /* 返回按钮：主页会把它隐藏掉。flex 会自动跳过隐藏的子项，
     * 所以隐藏后标题会自己左移，这里不需要任何额外处理。 */
    s_back_btn = lv_button_create(s_bar);
    lv_obj_set_size(s_back_btn, 80, 32);        /* 48 高的条里，上下各留 8px */
    lv_obj_add_event_cb(s_back_btn, on_back_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *back_lb = lv_label_create(s_back_btn);
    lv_label_set_text(back_lb, "Back");
    lv_obj_center(back_lb);

    s_title = lv_label_create(s_bar);
    lv_label_set_text(s_title, "");
    lv_obj_set_flex_grow(s_title, 1);           /* 撑开：把文本槽顶到最右 */

    s_text = lv_label_create(s_bar);
    lv_label_set_text(s_text, "");

    /* 没注入文本来源就不建定时器（没东西可显示，不用白跑一秒一次） */
    if (s_text_cb != NULL) {
        lv_timer_create(on_text_tick, TEXT_REFRESH_MS, NULL);
    }

    lvgl_port_unlock();

    ESP_LOGI(TAG, "ready (%dx%d)", LCD_SCREEN_H_RES, UI_STATUS_BAR_HEIGHT);
    return ESP_OK;
}

esp_err_t ui_status_bar_apply(const ui_status_bar_cfg_t *cfg)
{
    if (cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_bar == NULL) {
        ESP_LOGE(TAG, "not initialized, call ui_status_bar_init() first");
        return ESP_ERR_INVALID_STATE;
    }

    lvgl_port_lock(0);

    lv_label_set_text(s_title, (cfg->title != NULL) ? cfg->title : "");

    if (cfg->show_back) {
        lv_obj_remove_flag(s_back_btn, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_back_btn, LV_OBJ_FLAG_HIDDEN);
    }

    lvgl_port_unlock();
    return ESP_OK;
}
