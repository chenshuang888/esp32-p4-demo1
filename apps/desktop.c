/*
 * 桌面 —— 多应用的主页
 *
 * 做三件事：
 *   1. 遍历 app_manager 里所有已注册的 App，每个生成一个按钮
 *   2. 点击按钮 -> app_manager_launch(index) 切过去
 *   3. 显示 free heap，方便反复进出时观察有没有内存泄漏
 *
 * 本 App 采用"全部回收"策略：enter 时重建界面，leave 时异步释放自己的 screen。
 */
#include "desktop.h"

#include <stdint.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"

#include "app_manager.h"

static const char *TAG = "desktop";

static lv_obj_t *s_scr = NULL;

/*
 * 点击某个 App 的图标。
 * user_data 里存的是 App 的 index —— 注意是"把值转成指针"传进来，
 * 不是传 &i：循环里的局部变量在循环结束后就失效了，传地址会踩内存。
 */
static void on_app_clicked(lv_event_t *e)
{
    int index = (int)(intptr_t)lv_event_get_user_data(e);
    app_manager_launch(index);
}

static void desktop_enter(void)
{
    ESP_LOGI(TAG, "enter");

    /* lvgl_port_lock 是递归锁：即使调用方（点击回调）已持有，这里再锁也不会死锁 */
    lvgl_port_lock(0);

    s_scr = lv_obj_create(NULL);      /* 父传 NULL = 独立 screen */

    lv_obj_t *title = lv_label_create(s_scr);
    lv_label_set_text(title, "Desktop");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 24);

    /* 列出所有已注册的 App（跳过主页自己） */
    const int count = app_manager_count();
    for (int i = 0; i < count; i++) {
        /* 跳过主页（index 0 = 桌面自己）：点它没有任何意义 */
        if (i == 0) {
            continue;
        }

        const app_desc_t *app = app_manager_get(i);
        if (app == NULL) {
            continue;
        }

        lv_obj_t *btn = lv_button_create(s_scr);
        lv_obj_set_size(btn, 260, 64);
        lv_obj_align(btn, LV_ALIGN_TOP_MID, 0, 90 + i * 84);
        lv_obj_add_event_cb(btn, on_app_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t *label = lv_label_create(btn);
        lv_label_set_text(label, app->name);
        lv_obj_center(label);
    }

    /* 可用内存：反复进出时这个数字稳定，就说明没有泄漏 */
    lv_obj_t *mem = lv_label_create(s_scr);
    lv_label_set_text_fmt(mem, "free heap: %u KB",
                          (unsigned)(esp_get_free_heap_size() / 1024));
    lv_obj_align(mem, LV_ALIGN_BOTTOM_LEFT, 16, -16);

    lv_screen_load(s_scr);

    lvgl_port_unlock();
}

static void desktop_leave(void)
{
    ESP_LOGI(TAG, "leave");

    lvgl_port_lock(0);
    /* 本回调可能正跑在 s_scr 里某个对象的事件上，必须异步删除，不能直接 lv_obj_delete */
    lv_obj_delete_async(s_scr);
    s_scr = NULL;
    lvgl_port_unlock();
}

static const app_desc_t s_desc = {
    .name  = "Desktop",
    .icon  = "",
    .enter = desktop_enter,
    .leave = desktop_leave,
};

void desktop_register(void)
{
    app_manager_register(&s_desc);
}
