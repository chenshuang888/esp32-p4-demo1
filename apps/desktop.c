/*
 * 桌面 —— 多应用的主页
 *
 * 做三件事：
 *   1. 遍历 app_manager 里所有已注册的 App，每个生成一个"图标 + 名字"的格子
 *   2. 点击格子 -> app_manager_launch(index) 切过去
 *   3. 显示 free heap，方便反复进出时观察有没有内存泄漏
 *
 * 布局是 6 列 × 3 行的图标栅格，每个格子 = 透明的 button（保留按下反馈）
 * 里面竖排：96px 图标 + 4px 间距 + 名字。
 *
 * 图标从 picture 组件按 id 取（app->icon 里存的就是 id 字符串）。
 * 取不到 NULL 就只显示名字 —— 所以 desktop 完全不需要知道图片的格式和尺寸。
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
#include "ui_status_bar.h"
#include "picture.h"

static const char *TAG = "desktop";

/* ---------- 图标栅格版式 ----------
 * 一格的尺寸 = 图标 96 + 4px 间距 + 名字行高 22，居中在 138 高的格子里。
 * 整页高度的加法（屏幕 600）：
 *   状态栏 48 + 上留白 40 + (3×138 + 2×16 行距 = 446) + 下留白 66 = 600
 * 改任何一个数字都要重新对一遍这个加法，否则会溢出屏幕或被截掉。 */
#define ICON_CELL_H      138
#define ICON_ROW_GAP     16
#define ICON_PAD_TOP     40
#define ICON_PAD_BOTTOM  66

#define ICON_COLS        6
#define ICON_ROWS        3
#define ICON_MAX         (ICON_COLS * ICON_ROWS)      /* 一页最多 18 个，暂时不做翻页 */

static const int32_t S_GRID_COLS[] = {
    LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1),
    LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1),      /* 6 列，要和 ICON_COLS 一致 */
    LV_GRID_TEMPLATE_LAST
};

static const int32_t S_GRID_ROWS[] = {
    ICON_CELL_H, ICON_CELL_H, ICON_CELL_H,            /* 3 行，要和 ICON_ROWS 一致 */
    LV_GRID_TEMPLATE_LAST
};

static lv_obj_t *s_scr = NULL;

/*
 * 点击某个 App 的格子。
 * user_data 里存的是 App 的 index —— 注意是"把值转成指针"传进来，
 * 不是传 &i：循环里的局部变量在循环结束后就失效了，传地址会踩内存。
 */
static void on_app_clicked(lv_event_t *e)
{
    int index = (int)(intptr_t)lv_event_get_user_data(e);
    app_manager_launch(index);
}

/*
 * 把格子做成"没有外观"的 button：
 * 我们只要它的按下反馈（点上去有个视觉变化），不要主题给的浅灰底、边框和阴影 ——
 * 那些会把壁纸盖住，和"图标浮在照片上"的观感冲突。
 */
static void strip_button_style(lv_obj_t *item)
{
    lv_obj_set_style_bg_opa(item, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(item, 0, 0);
    lv_obj_set_style_shadow_width(item, 0, 0);
    lv_obj_set_style_pad_all(item, 0, 0);
    lv_obj_remove_flag(item, LV_OBJ_FLAG_SCROLLABLE);
}

/* 建一个"图标 + 名字"的格子，放进栅格的第 (col, row) 格 */
static void icon_item_create(lv_obj_t *grid, const app_desc_t *app,
                             int index, int col, int row)
{
    lv_obj_t *item = lv_button_create(grid);
    strip_button_style(item);

    /* 撑满整格 —— 这样点击区是整个格子，而不是只有图标那一小块 */
    lv_obj_set_grid_cell(item, LV_GRID_ALIGN_STRETCH, col, 1,
                               LV_GRID_ALIGN_STRETCH, row, 1);
    lv_obj_add_event_cb(item, on_app_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)index);

    /* 内部竖排：图标在上，名字在下 */
    lv_obj_set_flex_flow(item, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(item, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(item, 4, 0);

    /* 图标：按 id 查表取。查不到（NULL）就只显示名字，静默降级 */
    const lv_image_dsc_t *icon = picture_icon(app->icon);
    if (icon != NULL) {
        lv_obj_t *img = lv_image_create(item);
        lv_image_set_src(img, icon);      /* 尺寸由图片自己决定（96x96） */
    }

    /* 名字：直接压在照片上，加一圈同色描边保证任何背景下都看得清 */
    lv_obj_t *name = lv_label_create(item);
    lv_label_set_text(name, app->name);
    lv_obj_set_style_text_color(name, lv_color_white(), 0);
    lv_obj_set_style_text_outline_stroke_color(name, lv_color_black(), 0);
    lv_obj_set_style_text_outline_stroke_width(name, 3, 0);
    lv_obj_set_style_text_outline_stroke_opa(name, LV_OPA_COVER, 0);
}

static void desktop_enter(void)
{
    ESP_LOGI(TAG, "enter");

    /* lvgl_port_lock 是递归锁：即使调用方（点击回调）已持有，这里再锁也不会死锁 */
    lvgl_port_lock(0);

    s_scr = lv_obj_create(NULL);      /* 父传 NULL = 独立 screen */

    /* 壁纸：直接当 screen 的背景图。
     * 图是 1024x600、和屏幕一致，所以平铺/对齐都不用管；
     * 也不需要判空（编进固件的静态资源永远在）。 */
    lv_obj_set_style_bg_image_src(s_scr, picture_wallpaper(), 0);

    /* 状态栏：主页没有"回"的地方，所以不显示返回按钮。
     * 注意标题交给状态栏显示，这里不再单独画一个，免得重复。 */
    const ui_status_bar_cfg_t bar = {
        .title     = "Desktop",
        .show_back = false,
    };
    ui_status_bar_apply(&bar);

    /* 图标栅格：只是个布局容器，本身不要任何外观 */
    lv_obj_t *grid = lv_obj_create(s_scr);
    lv_obj_remove_style_all(grid);
    /* 宽度铺满；高度由内容撑（= 上留白 + 3 行 + 行距 + 下留白 = 552），
     * 贴着状态栏下沿放，正好到屏幕底部 */
    lv_obj_set_size(grid, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_align(grid, LV_ALIGN_TOP_MID, 0, UI_STATUS_BAR_HEIGHT);
    lv_obj_set_style_pad_top(grid, ICON_PAD_TOP, 0);
    lv_obj_set_style_pad_bottom(grid, ICON_PAD_BOTTOM, 0);
    lv_obj_set_style_pad_row(grid, ICON_ROW_GAP, 0);
    lv_obj_set_grid_dsc_array(grid, S_GRID_COLS, S_GRID_ROWS);

    /* 列出所有已注册的 App（跳过主页自己），依次填进格子 */
    const int count = app_manager_count();
    int slot = 0;
    for (int i = 1; i < count && slot < ICON_MAX; i++) {
        const app_desc_t *app = app_manager_get(i);
        if (app == NULL) {
            continue;
        }
        icon_item_create(grid, app, i, slot % ICON_COLS, slot / ICON_COLS);
        slot++;
    }

    /* 可用内存：反复进出时这个数字稳定，就说明没有泄漏 */
    lv_obj_t *mem = lv_label_create(s_scr);
    lv_label_set_text_fmt(mem, "free heap: %u KB",
                          (unsigned)(esp_get_free_heap_size() / 1024));
    lv_obj_align(mem, LV_ALIGN_BOTTOM_LEFT, 16, -16);
    lv_obj_set_style_text_color(mem, lv_color_white(), 0);
    lv_obj_set_style_text_outline_stroke_color(mem, lv_color_black(), 0);
    lv_obj_set_style_text_outline_stroke_width(mem, 3, 0);
    lv_obj_set_style_text_outline_stroke_opa(mem, LV_OPA_COVER, 0);

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
