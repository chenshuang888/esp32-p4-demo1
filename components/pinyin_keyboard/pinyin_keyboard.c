/*
 * pinyin_keyboard —— 实现（设计说明见头文件）
 *
 * 一个文件里两层，接缝只有一个函数 `on_key()`：
 *
 *   第一部分  键盘    布局表 / 分页 / 中/英模式 / 样式。**只把按键翻成语义报出去**，
 *                     不认识输入框、也不认识引擎
 *   第二部分  输入法   决定每一类键要干什么：拼音缓冲、候选栏、上屏、学习
 *   第三部分  对外接口
 *
 * ⚠️ 那条接缝是有纪律的：键盘那半**不判断**"数字键是直选候选还是插字符" ——
 *    它不知道当前有没有候选，也就没资格判断。这条跟"两层在不在一个文件里"无关。
 *
 * ============================ buttonmatrix 的三个坑 ============================
 *
 * 1) **"\n" 不是按键**：`lv_buttonmatrix_get_selected_button()` 返回的下标**不含**它。
 *    所以维护两套下标：`s_map[p][mi]`（布局表下标，含 "\n"）和 `s_kind[p][bi]`
 *    （**按键**下标）。建表时一起生成，见 build_page()。
 *
 * 2) **换 map 之后必须重贴 ctrl_map**：`lv_buttonmatrix_set_map()` 会重建按键区域，
 *    宽度和 `CHECKED`（灰键）一起被冲掉。所以这两件事永远成对做，走 apply_page()。
 *    ⚠️ 上一版在"切中/英"那条路上漏了这一步（只换了 map 刷文字），症状是：**一切
 *       语言，所有键宽就退回默认、灰键变白**。现在两条路都走 apply_page()。
 *
 * 3) **外观不能指望主题**：主题按 `lv_buttonmatrix_class` 给的是"卡片"外观（圆角+
 *    边框），而键盘要的是"铺满一条"；另外主题只给 `lv_keyboard_class` 加"键背景白 +
 *    键专用底色"这两条。所以 apply_style() 里全部显式设一遍（`lv_obj_set_style_*`
 *    是局部样式，优先级高于主题样式）。
 *
 * ============================ 拼音写在输入框里 ============================
 *
 * 拼音字母**就打在输入框里**，代价是"上屏"不再等于"追加"：
 *
 *     上屏 = 把框里末尾那串拼音删掉 + 追加汉字
 *
 * 于是绕不开"末尾有几个字符是拼音"的记账问题，而**四种情况**会让记账失真：
 *   - **App 可能背着我们动输入框**（`ime_test.c` 的提交就是 `lv_textarea_set_text("")`）
 *   - 用户中途插了数字或符号，末尾就不再是拼音了
 *   - 用户点一下把光标挪到中间
 *   - 退格要分两档（退拼音 / 退已上屏的字）
 *
 * 所以**不维护计数器，每次动手前核对一次**：见 `ta_tail_is_pinyin()` ——
 * 对得上才删，对不上就当账已过期（丢掉缓冲、不回删）。这比"猜"稳得多。
 *
 * 配套规则：插数字/标点、切中/英时，都先把当前拼音**定案**（字母原样留在框里当普通
 * 文本、候选栏收起）—— 这样账永远不会因为"末尾被插了别的东西"而错位。
 */
#include "pinyin_keyboard.h"

#include <stdbool.h>
#include <string.h>

#include "esp_log.h"

#include "pinyin_engine.h"
#include "pinyin_learn.h"

static const char *TAG = "pinyin_keyboard";

/* ===================== 两层之间的接缝 ===================== */

/**
 * 键盘报给输入法那半的**语义**。这是两层之间**唯一的接口** —— 键盘只说
 * "按了哪一类键"，不说"该干什么"。
 */
typedef enum {
    EV_LETTER = 0,      /* 字母；txt = "a".."z" / "A".."Z" */
    EV_DIGIT,           /* 数字；txt = "0".."9" */
    EV_PUNCT,           /* 标点/符号；txt 可能多字节（"，" "。"） */
    EV_BACKSPACE,       /* 退格；txt = NULL */
    EV_SPACE,           /* 空格；txt = NULL */
    EV_ENTER,           /* 回车 / 提交；txt = NULL */
    EV_MODE_CHANGED,    /* 中/英 被切了；txt = NULL */
} key_ev_t;

/** 输入法那半的入口（定义在第二部分）。键盘那半只调它，不反过来。 */
static void on_key(key_ev_t ev, const char *txt);

/* ===================== 状态 ===================== */

/* ---- 两层共用 ---- */
static lv_obj_t *s_kb = NULL;       /* 键盘本体（buttonmatrix） */
static bool      s_en = false;      /* 中/英 */

/* ---- 键盘那半：布局表与分页 ---- */

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

#define KB_PAGE_CNT     3
/* 单页最多多少个布局表项（含 "\n" 与结尾哨兵）。当前最大的一页是 45 键 + 4 换行 + 1 哨兵。 */
#define KB_MAX_ENTRIES  64
/* 单页最多多少个真按键（不含 "\n"）。当前最大是 41。 */
#define KB_MAX_BTNS     56

enum { PG_LOWER = 0, PG_UPPER, PG_SYMBOL };

/* 布局表要长期存在（LVGL 只存指针），所以放 static 而不是栈上 */
static const char *s_map[KB_PAGE_CNT][KB_MAX_ENTRIES];
static lv_buttonmatrix_ctrl_t s_ctrl[KB_PAGE_CNT][KB_MAX_BTNS];
static uint8_t s_kind[KB_PAGE_CNT][KB_MAX_BTNS];
static uint8_t s_mi[KB_PAGE_CNT][KB_MAX_BTNS];   /* 每个按键在布局表里的下标（改文字要用） */
static int     s_btn_cnt[KB_PAGE_CNT];
static int     s_mode_bi[KB_PAGE_CNT];           /* 中/EN 键的按键下标；-1 = 本页没有 */
static int     s_page = PG_LOWER;

/* ---- 输入法那半：输入框、拼音缓冲、候选栏 ---- */

static lv_obj_t *s_ta = NULL;
static lv_obj_t *s_cand_box = NULL;
static lv_obj_t *s_cand_btn[PE_MAX_CAND];
static lv_obj_t *s_cand_lb[PE_MAX_CAND];

static char s_py[PE_INPUT_MAX + 1];
static int  s_py_len = 0;

/* 候选文本的副本。引擎的转换结果本来是局部变量，而候选按钮的 label 要长期引用它，
 * 所以拷到控件自己的静态缓冲里 —— 生命周期跟控件走，不跟某次调用走。 */
static char s_cand_text[PE_MAX_CAND][PE_MAX_CAND_LEN];
static int  s_ncand = 0;

static bool s_full_warned = false;      /* 拼音写满的告警只打一次 */

/* =========================================================================
 * 第一部分：键盘
 *   —— 布局 / 分页 / 中英模式 / 样式。只把按键翻成"语义"报出去。
 * ========================================================================= */

/*
 * 小写字母页（默认页）。5 行：数字 / qwerty / asdf / shift+zxcv+退格 / 底行。
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
 * ⚠️ 这里同时维护两套下标（见文件头坑 1）：mi 数的是布局表表项（含 "\n"），
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

/*
 * 把"当前页"的布局表和控制位**成对**贴上去。
 *
 * ⚠️ 这两步必须一起做：`lv_buttonmatrix_set_map()` 会重建按键区域，把宽度和
 *    `CHECKED`（灰键）一起冲掉。任何"换了 map"的地方都要走这个函数。
 */
static void apply_page(void)
{
    if (s_kb == NULL) {
        return;
    }
    lv_buttonmatrix_set_map(s_kb, s_map[s_page]);
    lv_buttonmatrix_set_ctrl_map(s_kb, s_ctrl[s_page]);
}

/* 把"中/EN"的文字按当前模式刷新到布局表里，再重贴当前页（会一起补上 ctrl_map）。 */
static void apply_mode_text(void)
{
    const char *txt = s_en ? "EN" : "中";

    for (int p = 0; p < KB_PAGE_CNT; p++) {
        if (s_mode_bi[p] >= 0) {
            s_map[p][s_mi[p][s_mode_bi[p]]] = txt;
        }
    }
    apply_page();
}

static void set_page(int p)
{
    if (p < 0 || p >= KB_PAGE_CNT || p == s_page) {
        return;
    }
    s_page = p;
    apply_page();
}

/*
 * 键盘的按键分发：只做"翻译"，把按下的键翻成 key_ev_t 交给输入法那半。
 *
 * 换页和中/英是键盘**自己的**事（不经过输入法）；中/英变化会**额外**报一个
 * EV_MODE_CHANGED，让输入法有机会收拾自己那摊（丢掉没上屏的拼音）。
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

    key_ev_t ev;
    switch (kind) {
    case KT_LETTER:     ev = EV_LETTER;    break;
    case KT_DIGIT:      ev = EV_DIGIT;     break;
    case KT_PUNCT:      ev = EV_PUNCT;     break;
    case KT_BACKSPACE:  ev = EV_BACKSPACE; txt = NULL; break;
    case KT_SPACE:      ev = EV_SPACE;     txt = NULL; break;
    case KT_ENTER:      ev = EV_ENTER;     txt = NULL; break;

    /* ↓ 这几个是键盘自己的事，不交给输入法 */
    case KT_SHIFT:  set_page(s_page == PG_LOWER ? PG_UPPER : PG_LOWER); return;
    case KT_SYMBOL: set_page(PG_SYMBOL); return;
    case KT_ABC:    set_page(PG_LOWER);  return;
    case KT_MODE:   pinyin_keyboard_set_en_mode(!s_en); return;

    default:        return;                                 /* KT_ROW / KT_END 不该出现 */
    }

    on_key(ev, txt);
}

/*
 * 样式：**全部显式设置**（理由见文件头坑 3）。`lv_obj_set_style_*` 是局部样式，
 * 优先级高于主题样式，所以设了就能盖住主题给 buttonmatrix 的那套"卡片"外观。
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

/* =========================================================================
 * 第二部分：输入法
 *   —— 消费键盘报上来的语义，决定每一类键怎么处理。
 * ========================================================================= */

/* ===================== 输入框末尾的"拼音段" ===================== */

/*
 * 输入框末尾那 s_py_len 个字符，是不是就是我们记的那串拼音？
 *
 * ⚠️ 这一条是"拼音写进输入框"这个设计的**安全阀**。s_py_len 只是个记账，App 一句
 *    `lv_textarea_set_text("")` 就能让它过期，用户也可能把光标挪走、或者中途插了数字。
 *    与其维护一堆失效条件，不如动手删之前直接核对一眼 —— 对不上就认账过期。
 *
 * 比字节是安全的：拼音只含 a-z，一字一字节；而 strlen 给的是总字节数，
 * 所以"末尾 s_py_len 个字节"就是"末尾 s_py_len 个字符"。
 */
static bool ta_tail_is_pinyin(void)
{
    if (s_py_len <= 0) {
        return false;
    }
    const char *t  = lv_textarea_get_text(s_ta);
    const size_t tl = strlen(t);
    if (tl < (size_t)s_py_len) {
        return false;
    }
    return memcmp(t + (tl - (size_t)s_py_len), s_py, (size_t)s_py_len) == 0;
}

/*
 * 把输入框末尾那串拼音删掉（**核对不过就什么都不做**）。
 *
 * lv_textarea_delete_char 删的是"光标前的那一个字符"，所以先把光标顶到末尾，
 * 再删 s_py_len 次 —— 它自己会按 UTF-8 边界退，ASCII 就是一次一字节。
 */
static void ta_drop_pinyin(void)
{
    if (!ta_tail_is_pinyin()) {
        return;
    }
    lv_textarea_set_cursor_pos(s_ta, LV_TEXTAREA_CURSOR_LAST);
    for (int i = 0; i < s_py_len; i++) {
        lv_textarea_delete_char(s_ta);
    }
}

/* ===================== 往输入框写字 ===================== */

/* 一律先跳末尾：本组件按"只在末尾追加"设计，不支持中间插字。
 * （用户点一下把光标挪到中间也无所谓 —— 下一次按键会把它拉回末尾。） */
static void insert_raw(const char *txt)
{
    lv_textarea_set_cursor_pos(s_ta, LV_TEXTAREA_CURSOR_LAST);
    lv_textarea_add_text(s_ta, txt);
}

/* ===================== 候选栏 ===================== */

/*
 * 重新转换并刷新候选栏。
 *
 * ⚠️ 只改按钮的文本和显隐，**不删也不建对象** —— 本函数会在"候选按钮被点击"的
 *    事件里被调到（上屏后清空拼音），这时那个按钮的事件正在跑，删它（或它父对象的
 *    孩子）会崩。所以 PE_MAX_CAND 个按钮在建的时候一次做好，之后只复用。
 *    （同一条纪律见 README 里"leave 里删 screen 必须用 lv_obj_delete_async"。）
 */
static void refresh_bar(void)
{
    s_ncand = 0;
    if (s_py_len > 0) {
        pe_cand_t c[PE_MAX_CAND];
        const int n = pinyin_engine_convert(s_py, c, PE_MAX_CAND);
        for (int i = 0; i < n && i < PE_MAX_CAND; i++) {
            strncpy(s_cand_text[s_ncand], c[i].text, PE_MAX_CAND_LEN - 1);
            s_cand_text[s_ncand][PE_MAX_CAND_LEN - 1] = '\0';
            s_ncand++;
        }
    }

    for (int i = 0; i < PE_MAX_CAND; i++) {
        if (i < s_ncand) {
            lv_label_set_text(s_cand_lb[i], s_cand_text[i]);
            lv_obj_remove_flag(s_cand_btn[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_cand_btn[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void clear_pinyin(void)
{
    s_py_len = 0;
    s_py[0]  = '\0';
    s_full_warned = false;
    refresh_bar();              /* 拼音为空 -> 候选全隐藏 */
}

/* 上屏一个候选：先记一笔学习，再把框里那串拼音换成候选文本 */
static void commit(int idx)
{
    if (idx < 0 || idx >= s_ncand) {
        return;
    }
    /* ⚠️ 顺序：**先学、再替换、最后清拼音缓冲**。
     *    pinyin_engine_learn(idx) 依赖"上一次 convert 留下的分词拷贝"，而清拼音
     *    会走 refresh_bar() —— 它在拼音为空时不会重跑 convert，所以理论上放哪都行，
     *    但那个细节不值得依赖。先学是唯一稳妥的写法。
     *    ta_drop_pinyin() 自己会核对，账过期了就只删缓冲不碰输入框。 */
    pinyin_learn_record(idx);
    ta_drop_pinyin();
    insert_raw(s_cand_text[idx]);
    clear_pinyin();
}

static void on_cand_clicked(lv_event_t *e)
{
    commit((int)(intptr_t)lv_event_get_user_data(e));
}

/* ===================== 按键语义 -> 具体动作 ===================== */

/*
 * 这是两层之间的接缝的另一头：键盘说"按了哪一类键"，这里决定要干什么。
 *
 * "数字键到底是直选候选还是插字符"这类判断必须在**这里**做 —— 键盘那半不知道
 * 当前有没有候选，它没资格判断。
 */
static void on_key(key_ev_t ev, const char *txt)
{
    switch (ev) {
    case EV_LETTER:
        /* 英文模式：字母直接进框，不碰引擎、不出候选 —— "英文模式"的全部就是这一条 */
        if (s_en) {
            insert_raw(txt);
            return;
        }
        /* 中文模式：进拼音缓冲，**同时也进输入框**（拼音就打在框里） */
        if (s_py_len >= PE_INPUT_MAX) {
            if (!s_full_warned) {
                ESP_LOGW(TAG, "拼音已到 %d 字母上限，多出的按键忽略（先选字再继续打）",
                         PE_INPUT_MAX);
                s_full_warned = true;
            }
            return;
        }
        /* 账过期（App 清空过输入框、或用户插过别的东西）就先丢掉旧账 ——
         * 不然接下来追加的字母会和缓冲对不上，删的时候就删错了。 */
        if (s_py_len > 0 && !ta_tail_is_pinyin()) {
            s_py_len = 0;
            s_py[0]  = '\0';
        }
        s_py[s_py_len++] = txt[0];
        s_py[s_py_len]   = '\0';
        insert_raw(txt);
        refresh_bar();
        return;

    case EV_DIGIT:
        /* 数字键：**有候选就拿它直选**（按 3 = 选第 3 个候选，这是中文输入法效率的
         * 主要来源）。没候选、或按的是 0、或超出候选数，就当普通数字插进去。
         * （候选最多 12 个，而数字只有 1~9 能直选 —— 第 10 个以后只能点。） */
        if (s_ncand > 0 && txt[0] >= '1' && txt[0] <= '9') {
            const int idx = txt[0] - '1';
            if (idx < s_ncand) {
                commit(idx);
                return;
            }
        }
        clear_pinyin();             /* 定案：字母留在框里当普通文本 */
        insert_raw(txt);
        return;

    case EV_PUNCT:
        /* 标点同理：先把拼音定案，再插标点（"中文里夹英文数字"走的就是这条路） */
        clear_pinyin();
        insert_raw(txt);
        return;

    case EV_SPACE:
        if (s_ncand > 0) {
            commit(0);              /* 有候选：上屏首选（手不用离开键盘区） */
        } else {
            clear_pinyin();
            insert_raw(" ");
        }
        return;

    case EV_BACKSPACE:
        if (s_py_len > 0) {
            /* 账没过期就把框里最后那个字母也删掉；过期了说明那串拼音已经不在框里，
             * 这时只退缓冲、不动输入框（否则会误删用户已上屏的字）。 */
            if (ta_tail_is_pinyin()) {
                lv_textarea_set_cursor_pos(s_ta, LV_TEXTAREA_CURSOR_LAST);
                lv_textarea_delete_char(s_ta);
            }
            s_py[--s_py_len] = '\0';
            refresh_bar();
        } else {
            lv_textarea_set_cursor_pos(s_ta, LV_TEXTAREA_CURSOR_LAST);
            lv_textarea_delete_char(s_ta);
        }
        return;

    case EV_ENTER:
        /* 键盘不认识"提交"这个概念，所以由这里转一层：往**键盘对象**上发一个
         * LV_EVENT_READY，App 照旧监听它的 READY —— 和以前用 LVGL 键盘时一样。 */
        lv_obj_send_event(s_kb, LV_EVENT_READY, NULL);
        return;

    case EV_MODE_CHANGED:
        /* 切中/英时把"还没上屏的拼音"定案：字母留在输入框里当普通文本，只清缓冲与
         * 候选栏 —— 和插数字/标点那条规则一致，不删用户看得见的字符。 */
        clear_pinyin();
        return;

    default:
        return;
    }
}

/* ===================== 建候选栏 ===================== */

/*
 * 候选栏：和键盘做兄弟，贴在键盘正上方。位置关系是这个组件的内在约定，所以自己摆。
 * 句柄存起来是因为刷新时要改按钮文本；栏本身归 screen 所有，跟着 screen 一起释放。
 */
static void build_cand_bar(lv_obj_t *parent)
{
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_remove_style_all(bar);
    /* ⚠️ flex 必须在 remove_style_all 之后设 —— 它会把 layout 一起清掉 */
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(bar, 8, 0);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    /*
     * ⚠️⚠️ 必须先强制刷一次布局，**再**取键盘宽度。
     *
     * lv_obj_get_width() 读的是**已缓存的坐标**（obj->coords），不是"你设进去的值"。
     * lv_obj_pos.h 里那两个函数的 @note 原话是：
     *     "The position of the object is recalculated only on the next redraw."
     * 而键盘是刚建好、还没经历过任何一次布局 —— coords 全是 0。直接取宽度会拿到 0，
     * 后果是**连锁**的：
     *   ① 候选栏被设成 0 宽 —— 容器默认裁剪子对象，里面的候选**整条看不见**；
     *   ② 紧接着的 lv_obj_align_to 又用"候选栏自己的 0 宽"去算水平居中，位置也错。
     * 实测症状极具迷惑性：**敲拼音什么都不出来，但数字/标点照常能插进输入框**
     * —— 因为那条路根本不碰候选栏。
     */
    lv_obj_update_layout(s_kb);
    lv_obj_set_size(bar, lv_obj_get_width(s_kb), PINYIN_KEYBOARD_BAR_H);
    lv_obj_align_to(bar, s_kb, LV_ALIGN_OUT_TOP_MID, 0, 0);

    /* 候选区占满整条。
     * ⚠️ 横滑三原则（同 apps/weather.c 的 s_hour_box）：
     *   ① 宽度必须是确定值（这里靠 flex_grow 从候选栏分到确定宽度）
     *   ② flex 保持默认 NOWRAP —— 子项溢出才形成滚动区
     *   ③ flex 在 remove_style_all 之后设 */
    s_cand_box = lv_obj_create(bar);
    lv_obj_remove_style_all(s_cand_box);
    lv_obj_set_flex_flow(s_cand_box, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_cand_box, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_cand_box, 4, 0);
    lv_obj_set_height(s_cand_box, lv_pct(100));
    lv_obj_set_flex_grow(s_cand_box, 1);
    lv_obj_set_scroll_dir(s_cand_box, LV_DIR_HOR);      /* 只允许横滑 */
    lv_obj_set_scrollbar_mode(s_cand_box, LV_SCROLLBAR_MODE_OFF);

    /* 候选按钮一次建好，之后只改文本/显隐（理由见 refresh_bar 的说明）。
     * 宽度用 LV_SIZE_CONTENT：候选长度从 1 个字到十几个字都有，等分宽度放不下长的。 */
    for (int i = 0; i < PE_MAX_CAND; i++) {
        lv_obj_t *b = lv_button_create(s_cand_box);
        lv_obj_set_size(b, LV_SIZE_CONTENT, lv_pct(100));
        lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);      /* 别把横滑的拖拽吃掉 */
        lv_obj_add_flag(b, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_event_cb(b, on_cand_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *lb = lv_label_create(b);
        lv_obj_center(lb);
        s_cand_btn[i] = b;
        s_cand_lb[i]  = lb;
    }
}

/* =========================================================================
 * 第三部分：对外接口
 * ========================================================================= */

lv_obj_t *pinyin_keyboard_create(lv_obj_t *parent, lv_obj_t *ta, int w, int h)
{
    if (parent == NULL || ta == NULL) {
        ESP_LOGE(TAG, "parent / ta 为空");
        return NULL;
    }
    /* 单实例：重复调用就覆盖（App 每次 enter 重新建一个是正确用法）。 */
    s_kb        = NULL;
    s_ta        = ta;
    s_page      = PG_LOWER;
    s_en        = false;
    s_py_len    = 0;
    s_py[0]     = '\0';
    s_ncand     = 0;
    s_cand_box  = NULL;
    s_full_warned = false;
    for (int i = 0; i < PE_MAX_CAND; i++) {
        s_cand_btn[i] = NULL;
        s_cand_lb[i]  = NULL;
    }

    build_page(PAGE_LOWER,  PG_LOWER);
    build_page(PAGE_UPPER,  PG_UPPER);
    build_page(PAGE_SYMBOL, PG_SYMBOL);

    /* 1) 键盘本体。位置也由这里摆（键盘和候选栏是一体的，见头文件那条约定）。 */
    s_kb = lv_buttonmatrix_create(parent);
    if (s_kb == NULL) {
        ESP_LOGE(TAG, "建键盘失败（内存不足？）");
        return NULL;
    }
    lv_obj_set_size(s_kb, w, h);
    lv_obj_align(s_kb, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_remove_flag(s_kb, LV_OBJ_FLAG_SCROLLABLE);
    apply_style(s_kb);
    apply_mode_text();              /* 先把 "中" 写上，并贴好当前页 */
    lv_obj_add_event_cb(s_kb, on_value_changed, LV_EVENT_VALUE_CHANGED, NULL);

    /* 2) 候选栏 + 候选按钮 */
    build_cand_bar(parent);
    refresh_bar();

    ESP_LOGI(TAG, "输入法已就绪：键盘 %d 页（默认页 %d 键）+ 候选栏（最多 %d 个候选）",
             KB_PAGE_CNT, s_btn_cnt[PG_LOWER], PE_MAX_CAND);
    return s_kb;
}

void pinyin_keyboard_detach(void)
{
    /* 把这次会话学到的东西落盘（没改动的话内部直接返回，不会碰卡）。
     * 放在清指针之前只是语义上更顺 —— flush 只读引擎的内存表，和这些指针无关。 */
    pinyin_learn_flush();

    /* 只清指针与状态，**不去删那些对象** —— 它们是 screen 的子孙，由 App 删 screen
     * 时一起释放。这里删反而会重复释放。 */
    s_kb  = NULL;
    s_ta  = NULL;
    s_cand_box = NULL;
    for (int i = 0; i < PE_MAX_CAND; i++) {
        s_cand_btn[i] = NULL;
        s_cand_lb[i]  = NULL;
    }
    s_py_len = 0;
    s_py[0]  = '\0';
    s_ncand  = 0;
    s_full_warned = false;
}

void pinyin_keyboard_reset(void)
{
    if (s_ta == NULL) {
        return;
    }
    /* ⚠️ 拼音写在输入框里，所以"丢掉还没上屏的拼音"必须**连框里那几个字母一起删**
     *    —— 否则用户会看到一串谁也解释不了的裸字母留在输入框里。
     *    （App 若先自己 set_text("") 再调这里也安全：核对不过就只清缓冲。） */
    ta_drop_pinyin();
    clear_pinyin();
}

bool pinyin_keyboard_get_en_mode(void)
{
    return s_en;
}

void pinyin_keyboard_set_en_mode(bool en)
{
    if (s_en == en) {
        return;                     /* 没变就别刷（避免无谓的换 map） */
    }
    s_en = en;
    apply_mode_text();              /* 刷新键面 + 重贴 ctrl_map（见 apply_page 的说明） */
    on_key(EV_MODE_CHANGED, NULL);  /* 让输入法把没上屏的拼音定案 */
}
