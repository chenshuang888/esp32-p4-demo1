/*
 * pinyin_ime —— 实现（见头文件里的模型说明）
 *
 * 这里就三块：一个按键回调（绕坑 2）、一段字体传递（绕坑 1）、一个 attach 函数。
 */
#include "pinyin_ime.h"

#include <string.h>     /* strcmp */

#include "esp_log.h"

/* 护栏：lv_pinyin_dict_t 这个类型**只在 LV_USE_IME_PINYIN 打开时才存在**
 * （lv_ime_pinyin.h 里被 #if LV_USE_IME_PINYIN 包着）。漏开配置时，正常会得到一串
 * "unknown type name 'lv_pinyin_dict_t'"，很难联想到是配置问题 —— 这里换成一句能直接照做的提示。 */
#if !LV_USE_IME_PINYIN
#error "拼音输入法需要一个 LVGL 配置项：CONFIG_LV_USE_IME_PINYIN=y。它已经写在 sdkconfig.defaults 的『拼音输入法』一节里，但 defaults 只对 sdkconfig 里『尚未出现』的符号生效 —— 该符号在当前 sdkconfig 中已存在（是 not set），所以必须删掉 sdkconfig 后重新 build。详见 README『中文输入（拼音输入法）』。"
#endif

static const char *TAG = "pinyin_ime";

/* 生成物（dicts/lv_pinyin_dict.c）里的词典数组，约 45 KB rodata。
 * 末尾有 { NULL, NULL } 终止哨兵 —— lv_ime_pinyin 的 init_pinyin_dict() 靠它结束。 */
extern const lv_pinyin_dict_t pinyin_dict_data[];

/* =====================================================================
 * 坑 2：键盘左下角那个键会把输入法弄坏
 *
 * lv_ime_pinyin 把键盘上的 LV_SYMBOL_KEYBOARD 键当成"切 26 键 / 9 键模式"：
 *     else if (txt == LV_SYMBOL_KEYBOARD) {
 *         lv_ime_pinyin_set_mode(obj, LV_IME_PINYIN_MODE_K9);   // 或切回 K26
 *         ...
 *     }
 * 但 lv_ime_pinyin_set_mode() 里只有改模式那行是**无条件**执行的：
 *     pinyin_ime->mode = mode;
 *     #if LV_IME_PINYIN_USE_K9_MODE          // ← 换键盘布局这段被条件编译包着
 *         ...lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_USER_1, ...)...
 *     #endif
 * 我们不开 K9 模式（LV_IME_PINYIN_USE_K9_MODE=n，九宫格不是本项目要的），
 * 于是按下这个键会得到"内部模式 = K9、键盘布局还是 K26"的错配。而 IME 的按键处理是：
 *     else if ((mode == K26) && (txt[0] >= 'a' && txt[0] <= 'z')) { ...出候选... }
 * 模式已经不是 K26 了 ⇒ **之后所有字母都不再产生候选**，只是被键盘默认回调
 * 原样写进 textarea。不崩、不报错、日志也没有 —— 表现就是"输入法突然失灵"。
 *
 * 绕法：在键盘上**追加**一个 VALUE_CHANGED 回调，把模式按回 K26。
 * 之所以能生效：LVGL 对同一事件按**注册顺序**依次调用回调，而
 *     ① lv_keyboard_def_event_cb    —— 键盘构造函数里注册
 *     ② lv_ime_pinyin_kb_event      —— lv_ime_pinyin_set_keyboard() 里注册
 *     ③ 本回调                       —— 我们在 attach 里最后注册
 * 所以本回调必然在 IME 把模式改成 K9 **之后**才执行。
 *
 * 顺带一个白捡的收获：①（键盘默认回调）本来就把这个键当"收起键盘"用，
 * 会发 LV_EVENT_CANCEL —— 所以 App 只要监听 LV_EVENT_CANCEL 就能实现收键盘，
 * 不用自己再画一个按钮。
 * ===================================================================== */
static void on_kb_value_changed(lv_event_t *e)
{
    lv_obj_t *kb  = lv_event_get_target_obj(e);
    lv_obj_t *ime = lv_event_get_user_data(e);

    const uint32_t id = lv_keyboard_get_selected_button(kb);
    if (id == LV_BUTTONMATRIX_BUTTON_NONE) {
        return;
    }

    const char *txt = lv_keyboard_get_button_text(kb, id);
    if (txt != NULL && strcmp(txt, LV_SYMBOL_KEYBOARD) == 0) {
        /* 撤销 IME 刚才切成 K9 的动作。K9 关闭时这个调用只改回那个字段，没有副作用。 */
        lv_ime_pinyin_set_mode(ime, LV_IME_PINYIN_MODE_K26);
    }
}

/* =====================================================================
 * 坑 1：候选栏的中文不会自己继承字体
 *
 * 候选栏是 IME 构造函数里建的 lv_buttonmatrix，而 lv_ime_pinyin_set_keyboard()
 * 把它的父对象设成了**键盘的父对象** —— 也就是说它是 IME 对象的**兄弟**，不是子对象，
 * 所以不会从 IME 对象继承字体（候选栏是唯一显示汉字的地方，字体不对就是满屏方块）。
 *
 * LVGL 官方示例走的是另一条路：lv_obj_set_style_text_font(ime, font, 0)，
 * 借 IME 自己的 LV_EVENT_STYLE_CHANGED 回调把字体转给候选栏
 * （lv_ime_pinyin_style_change_event）。这里直接设到候选栏上 —— 目的相同，
 * 少依赖一层"事件有没有被触发"。
 *
 * 字体取哪儿：从**这个 App 的 screen** 上读，而不是写死 &lv_font_cjk_18。
 * 因为 components/font_cjk 装的是一个"Montserrat + 中文 fallback"的**混排字体**，
 * 它挂在根对象上、子对象靠继承。读 screen 当前的字体，拿到的就是这个混排字体，
 * 而且将来全局字体策略变了（换字号、换 fallback）这里自动跟着变，
 * 不需要给 font_cjk 再加一个暴露内部字体对象的 API。
 * ===================================================================== */
static void apply_cand_font(lv_obj_t *ime, lv_obj_t *kb)
{
    lv_obj_t *cand = lv_ime_pinyin_get_cand_panel(ime);
    if (cand == NULL) {
        return;
    }

    lv_obj_t *scr = lv_obj_get_screen(kb);
    if (scr == NULL) {
        return;
    }

    const lv_font_t *font = lv_obj_get_style_text_font(scr, LV_PART_MAIN);
    if (font != NULL) {
        lv_obj_set_style_text_font(cand, font, 0);
    } else {
        ESP_LOGW(TAG, "screen 上没有字体，候选栏的中文会显示成方块");
    }
}

/* =====================================================================
 * 对外接口
 * ===================================================================== */

lv_obj_t *pinyin_ime_attach(lv_obj_t *kb)
{
    if (kb == NULL) {
        ESP_LOGE(TAG, "kb 为空");
        return NULL;
    }

    /* 1) 建 IME 并绑定键盘。
     *    父对象先给"键盘的父对象"只是为了少一次重挂 —— set_keyboard() 内部本来就会
     *    把 IME 和候选栏都 lv_obj_set_parent() 到键盘的父对象上。 */
    lv_obj_t *ime = lv_ime_pinyin_create(lv_obj_get_parent(kb));
    if (ime == NULL) {
        ESP_LOGE(TAG, "lv_ime_pinyin_create 失败（内存不足？）");
        return NULL;
    }
    lv_ime_pinyin_set_keyboard(ime, kb);

    /* 2) 装自己的词典。
     *    ⚠️ 必须紧跟 create 之后：LV_IME_PINYIN_USE_DEFAULT_DICT=n 时构造函数不会
     *       初始化词典，任何按键都必须在 set_dict 之后。
     *    ⚠️ 这里要显式转型：生成的数组声明成 const（内容本来就只读），而 LVGL 这个
     *       setter 的形参漏写了 const。看它的内部实现可以确认不会改内容
     *       （lv_ime_pinyin_private.h 里存的字段就是 const lv_pinyin_dict_t *），
     *       所以转掉 const 是安全的。 */
    lv_ime_pinyin_set_dict(ime, (lv_pinyin_dict_t *)pinyin_dict_data);

    /* 3) 候选栏字体（坑 1） */
    apply_cand_font(ime, kb);

    /* 4) 追加回调，守住左下角那个键（坑 2）。
     *    注册顺序很重要：必须晚于 set_keyboard（那里注册了 IME 自己的回调），
     *    否则我们的"按回 K26"会先执行、再被 IME 改成 K9。 */
    lv_obj_add_event_cb(kb, on_kb_value_changed, LV_EVENT_VALUE_CHANGED, ime);

    /* 不在这里打词典的统计数字（音节数/候选对数）：那是生成物的事实，
     * 重新生成就会变，写死在日志里迟早变成过期信息。要看就重跑生成脚本。 */
    ESP_LOGI(TAG, "拼音输入法已挂上（简体词典来自 dicts/lv_pinyin_dict.c）");
    return ime;
}
