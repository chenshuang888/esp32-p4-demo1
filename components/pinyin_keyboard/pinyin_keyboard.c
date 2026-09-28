/*
 * pinyin_keyboard —— 实现（设计说明见头文件）
 *
 * 结构就四块：**按键表**（纯数据）、**建表**（把表翻译成 buttonmatrix 要的两个数组）、
 * **分页与模式**、**按键分发**（把按键翻译成语义回调）。
 *
 * ============================ 按键表为什么是这个形状 ============================
 *
 * 每个键在一张表里同时写清三件事：**键面文字 / 语义 / 相对宽度**。
 *
 * 语义（kind）是**唯一的真相**：分发时只看 kind，**不看文字**。这一点很重要 ——
 * "中/EN" 这个键的文字会随模式在 "中" 和 "EN" 之间变，靠文字比对就会失灵；
 * 靠 kind 就永远稳。
 *
 * 宽度是**同一行内的相对值**（1..15），LVGL 按比例把整行分完。所以"一个键多宽"
 * 取决于它和同行其它键的比值，而不是绝对值 —— 参照 LVGL 自带键盘的取法：
 * 最上面两行 10 键各占 1，第三行 9 键自然就比它们宽一点（真键盘也是这样）。
 *
 * ============================ buttonmatrix 的两个坑 ============================
 *
 * 1) **"\n" 不是按键**：它在布局表里只表示"换行"，`lv_buttonmatrix_get_selected_button()`
 *    返回的下标**不含**它。所以我们维护两套下标：
 *      s_map[p][mi]  —— 布局表下标（含 "\n" 和结尾哨兵）
 *      s_kind/p][bi] —— **按键**下标（不含 "\n"），分发时用的就是它
 *    两者在建表时一起生成，见 build_page()。
 *
 * 2) **换 map 之后必须重贴 ctrl_map**：`lv_buttonmatrix_set_map()` 会重建按键区域，
 *    宽度和状态会被冲掉。见 set_page() 里的顺序说明。
 */
#include "pinyin_keyboard.h"

#include <string.h>

#include "esp_log.h"

static const char *TAG = "pinyin_keyboard";

/* ===================== 按键表的形状 ===================== */

/** 键的语义。**分发只看它，不看键面文字** —— 所以"中/EN"改文字不会影响识别。 */
typedef enum {
    KT_LETTER = 0,      /* 字母 */
    KT_DIGIT,           /* 数字 */
    KT_PUNCT,           /* 标点/符号（可以多字节，如 ，。） */
    KT_BACKSPACE,       /* 退格 */
    KT_SPACE,           /* 空格 */
    KT_ENTER,           /* 回车 */
    KT_SHIFT,           /* 切大小写 */
    KT_SYMBOL,          /* 切符号页 */
    KT_ABC,             /* 切回字母页 */
    KT_MODE,            /* 中/英 */
    KT_ROW,             /* "\n"：换行，**不是按键** */
    KT_END,             /* 表结束（对应布局表结尾的哨兵 ""） */
} kb_kind_t;

typedef struct {
    const char *txt;    /*!< 键面文字；KT_ROW 必须是 "\n"，KT_END 必须是 "" */
    uint8_t     kind;   /*!< kb_kind_t */
    uint8_t     w;      /*!< 同一行内的相对宽度 1..15 */
} kb_key_t;

#define K(t, kd, wd)   { t, kd, wd }
#define ROW_END        { "\n", KT_ROW, 0 }
#define TBL_END        { "",   KT_END, 0 }

/* "中/EN" 键的占位文字：真正的文字由 apply_mode_text() 按当前模式覆盖 */
#define MODE_TXT_PLACEHOLDER   "中"

/* ===================== 三页的布局 ===================== */

/*
 * 小写字母页（默认页）。
 *
 * 5 行：数字 / qwerty / asdf / shift+zxcv+退格 / 底行。
 * 底行是 `#+= 中/EN ，[空格]。↵` —— 中英切换在左下偏左，和真键盘的位置一致。
 */
static const kb_key_t PAGE_LOWER[] = {
    K("1", KT_DIGIT, 1), K("2", KT_DIGIT, 1), K("3", KT_DIGIT, 1), K("4", KT_DIGIT, 1), K("5", KT_DIGIT, 1),
    K("6", KT_DIGIT, 1), K("7", KT_DIGIT, 1), K("8", KT_DIGIT, 1), K("9", KT_DIGIT, 1), K("0", KT_DIGIT, 1),
    ROW_END,
    K("q", KT_LETTER, 1), K("w", KT_LETTER, 1), K("e", KT_LETTER, 1), K("r", KT_LETTER, 1), K("t", KT_LETTER, 1),
    K("y", KT_LETTER, 1), K("u", KT_LETTER, 1), K("i", KT_LETTER, 1), K("o", KT_LETTER, 1), K("p", KT_LETTER, 1),
    ROW_END,
    K("a", KT_LETTER, 1), K("s", KT_LETTER, 1), K("d", KT_LETTER, 1), K("f", KT_LETTER, 1), K("g", KT_LETTER, 1),
    K("h", KT_LETTER, 1), K("j", KT_LETTER, 1), K("k", KT_LETTER, 1), K("l", KT_LETTER, 1),
    ROW_END,
    K(LV_SYMBOL_UP, KT_SHIFT, 3), K("z", KT_LETTER, 2), K("x", KT_LETTER, 2), K("c", KT_LETTER, 2),
    K("v", KT_LETTER, 2), K("b", KT_LETTER, 2), K("n", KT_LETTER, 2), K("m", KT_LETTER, 2),
    K(LV_SYMBOL_BACKSPACE, KT_BACKSPACE, 3),
    ROW_END,
    K("#+=", KT_SYMBOL, 3), K(MODE_TXT_PLACEHOLDER, KT_MODE, 3), K("，", KT_PUNCT, 2), K(" ", KT_SPACE, 8),
    K("。", KT_PUNCT, 2), K(LV_SYMBOL_NEW_LINE, KT_ENTER, 3),
    TBL_END,
};

/*
 * 大写字母页：字母换成大写，⇧ 变成"回到小写"（向下箭头）。
 *
 * 为什么不复用同一张表再在运行时改大小写？因为那要改 26 个指针、还要重贴 ctrl_map，
 * 而多写一张表只是几十行**纯数据**，还更好读。
 */
static const kb_key_t PAGE_UPPER[] = {
    K("1", KT_DIGIT, 1), K("2", KT_DIGIT, 1), K("3", KT_DIGIT, 1), K("4", KT_DIGIT, 1), K("5", KT_DIGIT, 1),
    K("6", KT_DIGIT, 1), K("7", KT_DIGIT, 1), K("8", KT_DIGIT, 1), K("9", KT_DIGIT, 1), K("0", KT_DIGIT, 1),
    ROW_END,
    K("Q", KT_LETTER, 1), K("W", KT_LETTER, 1), K("E", KT_LETTER, 1), K("R", KT_LETTER, 1), K("T", KT_LETTER, 1),
    K("Y", KT_LETTER, 1), K("U", KT_LETTER, 1), K("I", KT_LETTER, 1), K("O", KT_LETTER, 1), K("P", KT_LETTER, 1),
    ROW_END,
    K("A", KT_LETTER, 1), K("S", KT_LETTER, 1), K("D", KT_LETTER, 1), K("F", KT_LETTER, 1), K("G", KT_LETTER, 1),
    K("H", KT_LETTER, 1), K("J", KT_LETTER, 1), K("K", KT_LETTER, 1), K("L", KT_LETTER, 1),
    ROW_END,
    K(LV_SYMBOL_DOWN, KT_SHIFT, 3), K("Z", KT_LETTER, 2), K("X", KT_LETTER, 2), K("C", KT_LETTER, 2),
    K("V", KT_LETTER, 2), K("B", KT_LETTER, 2), K("N", KT_LETTER, 2), K("M", KT_LETTER, 2),
    K(LV_SYMBOL_BACKSPACE, KT_BACKSPACE, 3),
    ROW_END,
    K("#+=", KT_SYMBOL, 3), K(MODE_TXT_PLACEHOLDER, KT_MODE, 3), K("，", KT_PUNCT, 2), K(" ", KT_SPACE, 8),
    K("。", KT_PUNCT, 2), K(LV_SYMBOL_NEW_LINE, KT_ENTER, 3),
    TBL_END,
};

/*
 * 符号页：数字行照旧（它同时也是"直选候选"），底下三行符号，最后一行回到字母页。
 * 这里每行都是 10 个键、宽度全 1 —— 一行里键数一样，自然就对齐了。
 */
static const kb_key_t PAGE_SYMBOL[] = {
    K("1", KT_DIGIT, 1), K("2", KT_DIGIT, 1), K("3", KT_DIGIT, 1), K("4", KT_DIGIT, 1), K("5", KT_DIGIT, 1),
    K("6", KT_DIGIT, 1), K("7", KT_DIGIT, 1), K("8", KT_DIGIT, 1), K("9", KT_DIGIT, 1), K("0", KT_DIGIT, 1),
    ROW_END,
    K("-", KT_PUNCT, 1), K("_", KT_PUNCT, 1), K("=", KT_PUNCT, 1), K("+", KT_PUNCT, 1), K("/", KT_PUNCT, 1),
    K("\\", KT_PUNCT, 1), K("|", KT_PUNCT, 1), K("~", KT_PUNCT, 1), K("`", KT_PUNCT, 1), K("^", KT_PUNCT, 1),
    ROW_END,
    K("!", KT_PUNCT, 1), K("@", KT_PUNCT, 1), K("#", KT_PUNCT, 1), K("$", KT_PUNCT, 1), K("%", KT_PUNCT, 1),
    K("&", KT_PUNCT, 1), K("*", KT_PUNCT, 1), K("(", KT_PUNCT, 1), K(")", KT_PUNCT, 1), K("?", KT_PUNCT, 1),
    ROW_END,
    K("[", KT_PUNCT, 1), K("]", KT_PUNCT, 1), K("{", KT_PUNCT, 1), K("}", KT_PUNCT, 1), K("<", KT_PUNCT, 1),
    K(">", KT_PUNCT, 1), K(":", KT_PUNCT, 1), K(";", KT_PUNCT, 1), K("\"", KT_PUNCT, 1), K("'", KT_PUNCT, 1),
    ROW_END,
    K("ABC", KT_ABC, 3), K(MODE_TXT_PLACEHOLDER, KT_MODE, 3), K("，", KT_PUNCT, 2), K(" ", KT_SPACE, 8),
    K("。", KT_PUNCT, 2), K(LV_SYMBOL_BACKSPACE, KT_BACKSPACE, 3),
    TBL_END,
};

/* ===================== 状态 ===================== */

#define KB_PAGE_CNT     3
/* 单页最多多少个布局表项（含 "\n" 与结尾哨兵）。当前最大的一页是 45 键 + 4 换行 + 1 哨兵。 */
#define KB_MAX_ENTRIES  64
/* 单页最多多少个真按键（不含 "\n"）。当前最大是 41。 */
#define KB_MAX_BTNS     56

enum { PG_LOWER = 0, PG_UPPER, PG_SYMBOL };

static lv_obj_t *s_kb;
static pn_kb_key_cb_t s_cb;
static void          *s_cb_user;
static int            s_page = PG_LOWER;
static bool           s_en   = false;

/* 布局表要长期存在（LVGL 只存指针），所以放 static 而不是栈上 */
static const char *s_map[KB_PAGE_CNT][KB_MAX_ENTRIES];
static lv_buttonmatrix_ctrl_t s_ctrl[KB_PAGE_CNT][KB_MAX_BTNS];
static uint8_t s_kind[KB_PAGE_CNT][KB_MAX_BTNS];
static uint8_t s_mi[KB_PAGE_CNT][KB_MAX_BTNS];   /* 每个按键在布局表里的下标（改文字要用） */
static int     s_btn_cnt[KB_PAGE_CNT];
static int     s_mode_bi[KB_PAGE_CNT];           /* 中/EN 键的按键下标；-1 = 本页没有 */

/* ===================== 建表 ===================== */

/*
 * 按键的控制位。三档（照 LVGL 自带键盘的做法）：
 *   - 输入键：白底、**按下即响应**（不加 CLICK_TRIG，手感更跟手）
 *   - 功能键（退格/空格/回车）：**灰底**（CHECKED），但不加 NO_REPEAT —— 退格要能长按连删
 *   - 切换键（⇧/#+=/ABC/中英）：灰底 + 长按不重复 + 松开才响应
 */
static lv_buttonmatrix_ctrl_t ctrl_for(const kb_key_t *k)
{
    switch (k->kind) {
    case KT_SHIFT:
    case KT_SYMBOL:
    case KT_ABC:
    case KT_MODE:
        return (lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_CHECKED
                                       | LV_BUTTONMATRIX_CTRL_NO_REPEAT
                                       | LV_BUTTONMATRIX_CTRL_CLICK_TRIG
                                       | k->w);
    case KT_BACKSPACE:
    case KT_SPACE:
    case KT_ENTER:
        return (lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_CHECKED | k->w);
    default:
        return (lv_buttonmatrix_ctrl_t)k->w;
    }
}

/*
 * 把一张按键表翻译成 buttonmatrix 要的两个数组。
 *
 * ⚠️ 这里同时维护两套下标（见文件头"两个坑"）：mi 数的是布局表表项（含 "\n"），
 *    bi 数的是**真按键**（不含 "\n"）。分发时用的是 bi。
 */
static void build_page(const kb_key_t *tbl, int p)
{
    int mi = 0;
    int bi = 0;
    s_mode_bi[p] = -1;

    for (const kb_key_t *k = tbl; k->kind != KT_END; k++) {
        /* 留一格给结尾哨兵 ""，所以是 -1 */
        if (mi >= KB_MAX_ENTRIES - 1 || bi >= KB_MAX_BTNS) {
            ESP_LOGE(TAG, "页 %d 的按键超过预留的 %d 个，表被截断 —— 调大 KB_MAX_*", p, KB_MAX_BTNS);
            break;
        }
        s_map[p][mi++] = k->txt;

        if (k->kind == KT_ROW) {
            continue;               /* "\n" 只是换行，**不是按键** */
        }
        s_kind[p][bi] = (uint8_t)k->kind;
        s_mi[p][bi]   = (uint8_t)(mi - 1);      /* 这个按键在布局表里的位置 */
        s_ctrl[p][bi] = ctrl_for(k);
        if (k->kind == KT_MODE) {
            s_mode_bi[p] = bi;
        }
        bi++;
    }

    s_map[p][mi] = "";              /* buttonmatrix 的终止哨兵 */
    s_btn_cnt[p] = bi;

    /* 布局表写错（比如漏了 TBL_END、控制键下标错位）时，在这里立刻报出来，
     * 而不是等用户按下去才发现没反应。 */
    if (s_mode_bi[p] < 0) {
        ESP_LOGE(TAG, "页 %d 里没有 KT_MODE 键 —— 中/英 就切不了了", p);
    }
}

/* ===================== 模式与分页 ===================== */

/* 把"中/EN"的文字按当前模式刷新到布局表里，并让 buttonmatrix 重新读一遍。 */
static void apply_mode_text(void)
{
    const char *txt = s_en ? "EN" : "中";

    for (int p = 0; p < KB_PAGE_CNT; p++) {
        if (s_mode_bi[p] >= 0) {
            s_map[p][s_mi[p][s_mode_bi[p]]] = txt;
        }
    }
    if (s_kb != NULL) {
        lv_buttonmatrix_set_map(s_kb, s_map[s_page]);
    }
}

static void set_page(int p)
{
    if (p < 0 || p >= KB_PAGE_CNT || p == s_page) {
        return;
    }
    s_page = p;
    /* ⚠️ 顺序不能反：换 map 会重建按键区域、把宽度和状态冲掉，
     *    所以必须**紧跟**一次 set_ctrl_map 把宽度/灰底贴回去。
     *    （LVGL 自带的键盘也是这么做的，见它默认回调里的 set_map + update_ctrl_map。） */
    lv_buttonmatrix_set_map(s_kb, s_map[p]);
    lv_buttonmatrix_set_ctrl_map(s_kb, s_ctrl[p]);
}

/* ===================== 按键分发 ===================== */

/*
 * 只做"翻译"：把按下的键翻成 `pn_kb_key_t` 报给调用方。
 *
 * 换页和中/英是键盘**自己的**事，不往外报（除了中/英变化会额外报一个
 * MODE_CHANGED，让调用方有机会收拾自己那摊 —— 比如丢掉没上屏的拼音）。
 */
static void on_value_changed(lv_event_t *e)
{
    lv_obj_t *kb = lv_event_get_target_obj(e);

    const uint32_t id = lv_buttonmatrix_get_selected_button(kb);
    if (id == LV_BUTTONMATRIX_BUTTON_NONE || (int)id >= s_btn_cnt[s_page]) {
        return;
    }

    const kb_kind_t kind = (kb_kind_t)s_kind[s_page][id];
    const char *txt = s_map[s_page][s_mi[s_page][id]];

    pn_kb_key_t ev;
    switch (kind) {
    case KT_LETTER:     ev = PN_KB_KEY_LETTER;    break;
    case KT_DIGIT:      ev = PN_KB_KEY_DIGIT;     break;
    case KT_PUNCT:      ev = PN_KB_KEY_PUNCT;     break;
    case KT_BACKSPACE:  ev = PN_KB_KEY_BACKSPACE; txt = NULL; break;
    case KT_SPACE:      ev = PN_KB_KEY_SPACE;     txt = NULL; break;
    case KT_ENTER:      ev = PN_KB_KEY_ENTER;     txt = NULL; break;

    /* ↓ 这几个是键盘自己的事，不往外报 */
    case KT_SHIFT:  set_page(s_page == PG_LOWER ? PG_UPPER : PG_LOWER); return;
    case KT_SYMBOL: set_page(PG_SYMBOL); return;
    case KT_ABC:    set_page(PG_LOWER);  return;
    case KT_MODE:   pinyin_kb_set_en_mode(!s_en); return;   /* 里面会发 MODE_CHANGED */

    default:        return;                                 /* KT_ROW / KT_END 不该出现 */
    }

    if (s_cb != NULL) {
        s_cb(ev, txt, s_cb_user);
    }
}

/* ===================== 外观 ===================== */

/*
 * 样式：**全部显式设置**。
 *
 * 为什么不靠主题默认值：主题是按 `lv_buttonmatrix_class` 给样式的，而它给的是
 * "卡片"外观（圆角 + 边框），键盘要的是"铺满一条"。另外主题只给 `lv_keyboard_class`
 * 加"键背景白 + 键专用底色"这两条，纯 buttonmatrix 拿不到。
 *
 * `lv_obj_set_style_*` 是**局部样式**，优先级高于主题样式（同 components/font_cjk 里
 * 记的那条），所以这里设了就能盖住，不需要先把主题样式摘掉。
 */
static void apply_style(lv_obj_t *kb)
{
    /* 键盘本体：浅灰底、不要圆角和边框、留一点键间距 */
    lv_obj_set_style_bg_color(kb, lv_color_hex(0xD9D9D9), 0);
    lv_obj_set_style_bg_opa(kb, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(kb, 0, 0);
    lv_obj_set_style_radius(kb, 0, 0);
    lv_obj_set_style_shadow_width(kb, 0, 0);
    lv_obj_set_style_pad_all(kb, 4, 0);
    lv_obj_set_style_pad_row(kb, 5, 0);
    lv_obj_set_style_pad_column(kb, 5, 0);

    /* 普通键：白底、圆角、无反光 */
    lv_obj_set_style_bg_color(kb, lv_color_white(), LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(kb, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_text_color(kb, lv_color_hex(0x1A1A1A), LV_PART_ITEMS);
    lv_obj_set_style_border_width(kb, 0, LV_PART_ITEMS);
    lv_obj_set_style_radius(kb, 6, LV_PART_ITEMS);
    lv_obj_set_style_shadow_width(kb, 0, LV_PART_ITEMS);

    /* 按下：给一点明显的反馈（触摸屏上这是唯一的"手感"） */
    lv_obj_set_style_bg_color(kb, lv_color_hex(0x9EC5F0), LV_PART_ITEMS | LV_STATE_PRESSED);

    /* 灰键：功能键与切换键（它们带 CHECKED 位）。和 LVGL 键盘的观感一致。 */
    lv_obj_set_style_bg_color(kb, lv_color_hex(0xB8B8B8), LV_PART_ITEMS | LV_STATE_CHECKED);
}

/* ===================== 对外接口 ===================== */

lv_obj_t *pinyin_kb_create(lv_obj_t *parent, int w, int h)
{
    if (parent == NULL) {
        ESP_LOGE(TAG, "parent 为空");
        return NULL;
    }

    /* 单实例：重复调用就把旧的丢掉（和 pinyin_input 的做法一致） */
    s_kb        = NULL;
    s_cb        = NULL;
    s_cb_user   = NULL;
    s_page      = PG_LOWER;
    s_en        = false;

    build_page(PAGE_LOWER,  PG_LOWER);
    build_page(PAGE_UPPER,  PG_UPPER);
    build_page(PAGE_SYMBOL, PG_SYMBOL);

    s_kb = lv_buttonmatrix_create(parent);
    if (s_kb == NULL) {
        ESP_LOGE(TAG, "建 buttonmatrix 失败（内存不足？）");
        return NULL;
    }

    lv_obj_set_size(s_kb, w, h);
    lv_obj_remove_flag(s_kb, LV_OBJ_FLAG_SCROLLABLE);
    apply_style(s_kb);

    apply_mode_text();                                  /* 先把 "中" 写上去 */
    lv_buttonmatrix_set_map(s_kb, s_map[PG_LOWER]);
    lv_buttonmatrix_set_ctrl_map(s_kb, s_ctrl[PG_LOWER]);
    lv_obj_add_event_cb(s_kb, on_value_changed, LV_EVENT_VALUE_CHANGED, NULL);

    ESP_LOGI(TAG, "自研键盘已建：%d 页，默认页 %d 个键，尺寸 %dx%d",
             KB_PAGE_CNT, s_btn_cnt[PG_LOWER], w, h);
    return s_kb;
}

void pinyin_kb_set_key_cb(pn_kb_key_cb_t cb, void *user)
{
    s_cb      = cb;
    s_cb_user = user;
}

bool pinyin_kb_get_en_mode(void)
{
    return s_en;
}

void pinyin_kb_set_en_mode(bool en)
{
    if (s_en == en) {
        return;                     /* 没变就别刷（避免无谓的换 map） */
    }
    s_en = en;
    apply_mode_text();

    if (s_cb != NULL) {
        /* 报给调用方去收拾自己那摊 —— 比如把"还没上屏的拼音"定案 */
        s_cb(PN_KB_KEY_MODE_CHANGED, NULL, s_cb_user);
    }
}
