/*
 * 拼音输入法验收台 App
 *
 * 链路：屏幕上连打拼音 -> pinyin_engine 给出整句候选 -> 点候选（或按空格）整段上屏
 *       -> 按回车提交到大字区。
 *
 * ⚠️ 这个 App 是**验证台**，不是给用户用的功能（定位同 app_demo.c 之于 app_manager
 *    + kv_store）：它只证明"屏上能打出中文、汉字渲染正确"这一条链路是通的。
 *    联网那条轨道（DeepSeek）以后自带聊天界面，届时把这里的输入区搬过去即可 ——
 *    所以本文件里没有任何一行网络代码，也不需要联网。
 *
 * ⚠️ 中文输入法**只有一套**：`components/pinyin_keyboard`（自研键盘 + 候选栏 +
 *    输入法逻辑，转换交给 pinyin_engine）。一次 `pinyin_keyboard_create()` 全装好。
 *
 *    工程里曾经还有一版"薄接 LVGL 自带 lv_ime_pinyin"的实现（components/pinyin_ime），
 *    已经删掉了 —— 它的候选格只有 4 字节、装不下词组，而 lv_keyboard 的布局表又改不了。
 *    想看那版：`git show 24e039d:components/pinyin_ime/`。踩过的坑留在 README 坑表里。
 *
 * ⚠️ 字号**刻意一处都不设**：中文只有 18px 一档（见 font_cjk.c 的已知限制），
 *    显式设成别的字号会让汉字变成占位方块。不设就继承全局的混排字体，中英混排都对。
 */
#include "ime_test.h"

#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"

#include "app_manager.h"
#include "lcd_screen_display.h"     /* LCD_SCREEN_*_RES：算布局 */
#include "pinyin_keyboard.h"        /* 键盘 + 候选栏 + 输入法（高度常量、创建接口） */
#include "ui_status_bar.h"

static const char *TAG = "ime_test";

/* ---------- 版面 ----------
 * 内容区 552 = 600(屏) - 48(状态栏)，自上而下三块，加法必须对上：
 *
 *     显示区 164   （提交的内容，可纵向滚动）
 *     间距    8
 *     输入框  48   lv_textarea
 *     间距    8
 *     候选栏  44 ┐ 都由 pinyin_keyboard 一起建、一起摆
 *     键盘   280 ┘ （= PINYIN_KEYBOARD_TOTAL_H，324）
 *     --------------------------------
 *     合计   552
 *
 * 改任何一个数字都要重新对一遍这个加法，否则会溢出屏幕或被截掉（同 desktop.c 的纪律）。
 * 高度一律用宏（PINYIN_KEYBOARD_TOTAL_H / PINYIN_KEYBOARD_HEIGHT）而**不是**写死数字
 * —— 键盘和候选栏的尺寸是那个组件自己算的，App 这边写死别的数就会错位。
 *
 * ⚠️ **不要 align 键盘**：它和候选栏是一体的，位置由 pinyin_keyboard 自己摆
 *    （贴 body 底部）。App 再 align 一次，候选栏不会跟着走（它是一次性相对对齐算的）。
 * ⚠️ 这里也**刻意不用 flex 布局**：候选栏的位置是组件内部用 lv_obj_align_to() 算的，
 *    而 flex 容器会接管子对象的位置、把 align 覆盖掉 —— 两者混用是"不崩，但布局全错"。 */
#define PAD        16
#define GAP         8
#define TA_H       48
#define KB_H       PINYIN_KEYBOARD_HEIGHT
#define CONTENT_H (LCD_SCREEN_V_RES - UI_STATUS_BAR_HEIGHT)                  /* = 552 */
#define CONTENT_W (LCD_SCREEN_H_RES - PAD * 2)                               /* = 992 */
#define SHOW_H    (CONTENT_H - PINYIN_KEYBOARD_TOTAL_H - TA_H - GAP * 2)     /* = 164 */

static lv_obj_t *s_scr        = NULL;
static lv_obj_t *s_show_label = NULL;   /* 大字区里那个会自动换行的 label */
static lv_obj_t *s_ta         = NULL;
static lv_obj_t *s_kb         = NULL;

/*
 * 提交 = 按键盘上的回车键。
 *
 * 键盘本身不认识"提交" —— 它按到 ↵ 只报一个语义，是 pinyin_keyboard 往**键盘对象**上
 * 补发一个 LV_EVENT_READY，所以把回调挂在键盘上就能收到。
 *
 * ⚠️ 不要在这里加 LVGL 锁：事件回调本来就跑在 LVGL 任务里、锁已持有。
 */
static void on_submit(lv_event_t *e)
{
    (void)e;

    const char *txt = lv_textarea_get_text(s_ta);
    if (txt == NULL || txt[0] == '\0') {
        return;                 /* 空输入不覆盖上一次结果，方便对比 */
    }

    lv_label_set_text(s_show_label, txt);
    /* ⚠️ 先 reset 再清空：reset 会把框里**还没上屏的那串拼音字母**删掉
     * （拼音是写在输入框里的）。顺序反了也不会出错 —— reset 会自己核对账，
     * 但先 reset 语义更清楚。 */
    pinyin_keyboard_reset();
    lv_textarea_set_text(s_ta, "");     /* 清空，接着测下一次 */
}

/* 显示区：可纵向滚动的容器 + 一个自动换行的 label。内容超出时能上下拖。 */
static void build_show_area(lv_obj_t *parent)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, CONTENT_W, SHOW_H);
    lv_obj_align(box, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_scroll_dir(box, LV_DIR_VER);
    lv_obj_set_style_pad_all(box, 8, 0);

    s_show_label = lv_label_create(box);
    lv_obj_set_width(s_show_label, lv_pct(100));
    lv_label_set_long_mode(s_show_label, LV_LABEL_LONG_WRAP);
    lv_label_set_text(s_show_label,
                      "连打拼音（如 jintiantianqi）：点候选 / 按空格 / 按数字 1-9 都能上屏，回车提交。\n"
                      "左下角那个键切中/英；键盘最上面一行数字常驻。");
}

static void ime_test_enter(void)
{
    ESP_LOGI(TAG, "enter");

    /* lvgl_port_lock 是递归锁：从点击回调进来也不会死锁 */
    lvgl_port_lock(0);

    s_scr = lv_obj_create(NULL);

    const ui_status_bar_cfg_t bar = { .title = "IME Test", .show_back = true };
    ui_status_bar_apply(&bar);

    /* 坐标基准容器。**不设 pad**：lv_obj_align 是相对父对象的"内容区"定位的，
     * 设了 pad 会让每个子对象的原点整体偏移，还要到处补偏移量。让 body 自己内缩
     * PAD 更省事：宽度直接取 CONTENT_W (= 1024 - 2*PAD)。 */
    lv_obj_t *body = lv_obj_create(s_scr);
    lv_obj_remove_style_all(body);
    lv_obj_set_size(body, CONTENT_W, CONTENT_H);
    lv_obj_align(body, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);

    build_show_area(body);

    /* 输入框。max_length 默认 0 = 不限长，不用设。 */
    s_ta = lv_textarea_create(body);
    lv_textarea_set_one_line(s_ta, true);
    lv_obj_set_size(s_ta, CONTENT_W, TA_H);
    lv_obj_align(s_ta, LV_ALIGN_TOP_LEFT, 0, SHOW_H + GAP);

    /* 键盘 + 候选栏 + 输入法：**一次装好**（components/pinyin_keyboard）。
     * 和 LVGL 自带的键盘相比：数字行常驻（有候选时数字键 = 直选第 N 个候选）、
     * 左下角是中/英切换键（键面直接写当前模式）、去掉了对中文没用的 _ - : ← →。
     * ⚠️ 键盘和候选栏是一体的，位置由它自己摆（贴 body 底部）—— 这里不要再 align。 */
    s_kb = pinyin_keyboard_create(body, s_ta, CONTENT_W, KB_H);
    /* 回车 = 提交。键盘本身不认识"提交"，是本组件收到回车键后往键盘对象上补发一个
     * LV_EVENT_READY —— 所以这里监听的写法一直没变。 */
    lv_obj_add_event_cb(s_kb, on_submit, LV_EVENT_READY, NULL);

    lv_screen_load(s_scr);
    lvgl_port_unlock();
}

static void ime_test_leave(void)
{
    ESP_LOGI(TAG, "leave");

    lvgl_port_lock(0);

    /* 先解挂：它把键盘/输入框/候选栏的指针存在 static 里，不告诉它就把 screen 删了，
     * 下次 enter 再建时会指着一堆已释放的对象。顺带把学习记录落盘。
     * （那些对象本身不用在这里删 —— 它们是 screen 的子孙，跟着一起释放。）
     * 本 App 没有别的游离资源（没有 lv_timer），所以到这里就干净了。 */
    pinyin_keyboard_detach();

    lv_obj_delete_async(s_scr);
    s_scr = NULL;
    s_show_label = NULL;
    s_ta = NULL;
    s_kb = NULL;

    lvgl_port_unlock();
}

static const app_desc_t s_desc = {
    .name  = "IME",
    .icon  = "",            /* 没有图标：picture_icon() 查不到会返回 NULL，桌面只显示名字 */
    .enter = ime_test_enter,
    .leave = ime_test_leave,
};

void ime_test_register(void)
{
    app_manager_register(&s_desc);
}
