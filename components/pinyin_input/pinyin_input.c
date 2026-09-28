/*
 * pinyin_input —— 实现（模型说明见头文件）
 *
 * 结构就三块：**按键分发**、**候选栏刷新**、**上屏**。
 *
 * 键盘本身是另一个组件（components/pinyin_keyboard，自研的）：它负责布局、分页、
 * 中/英模式，然后把"按了哪一类键"当语义事件报过来。所以这里**不再需要**去认
 * 键面文字、也不管换页 —— 只决定"字母/数字/标点/退格/空格/回车 各自要干什么"。
 *
 * 数据流：
 *     按下字母 -> 英文模式直接进框；中文模式进拼音缓冲、同时也进框 -> 刷候选
 *     按下数字 -> **有候选就直选第 N 个**；否则先"定案"拼音再插入这个数字
 *     按下标点 -> 先把拼音"定案"（字母留在框里当普通文本），再插入标点
 *     按空格   -> 有候选就上屏第 1 个；没候选就定案 + 插一个空格
 *     点候选   -> commit(i): 记一笔学习 -> 把框里那串拼音删掉、换成候选文本
 *     按退格   -> 拼音非空就退拼音（框里也退一格）；拼音空了就删输入框最后一个字
 *     按回车   -> 往键盘对象上发 LV_EVENT_READY（App 照旧监听它 = 提交）
 *     切中/英  -> 把还没上屏的拼音定案（字母留在框里，不删）
 *
 * 学习记录**只在内存里攒**，App 退出时（detach）才落盘一次 —— 理由见
 * components/pinyin_learn/include/pinyin_learn.h 的"落盘时机"。
 *
 * ============================ 拼音写在输入框里 ============================
 *
 * 拼音字母**就打在输入框里**（真实输入法就是这样，比另开一个回显 label 好看），
 * 代价是"上屏"不再等于"追加"：
 *
 *     上屏 = 把框里末尾那串拼音删掉 + 追加汉字
 *
 * 于是绕不开"末尾有几个字符是拼音"的记账问题，而且有四种情况会让记账失真：
 *   - 退格要分两档（退拼音 / 退已上屏的字）
 *   - 中途插了数字或符号，末尾就不再是拼音了
 *   - 用户可能点一下把光标挪到中间
 *   - **App 可能背着我们动输入框**（`ime_test.c` 的提交就是 `lv_textarea_set_text("")`）
 *
 * 所以**不维护计数器，每次动手前核对一次**：见 `ta_tail_is_pinyin()` ——
 * 对得上才删，对不上就当账已过期（丢掉缓冲、不回删）。这比"猜"稳得多。
 *
 * 配套规则：输入数字/标点、以及切中/英时，都先把当前拼音**定案**
 * （字母原样留在框里当普通文本、候选栏收起）—— 这样账永远不会因为"末尾被插了
 * 别的东西"而错位。
 */
#include "pinyin_input.h"

#include <stdbool.h>
#include <string.h>

#include "esp_log.h"

#include "pinyin_engine.h"
#include "pinyin_keyboard.h"
#include "pinyin_learn.h"

static const char *TAG = "pinyin_input";

static lv_obj_t *s_kb   = NULL;
static lv_obj_t *s_ta   = NULL;
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

/* ===================== 输入框末尾的"拼音段" ===================== */

/*
 * 输入框末尾那 s_py_len 个字符，是不是就是我们记的那串拼音？
 *
 * ⚠️ 这一条是"拼音写进输入框"这个改动的**安全阀**。s_py_len 只是个记账，
 *    App 一句 `lv_textarea_set_text("")` 就能让它过期（ime_test.c 的提交就是），
 *    用户也可能点一下把光标挪走、或者中途插了数字。与其维护一堆失效条件，
 *    不如动手删之前直接核对一眼 —— 对不上就认账过期。
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

/* ===================== 候选栏刷新 ===================== */

/*
 * 重新转换并刷新候选栏。
 *
 * ⚠️ 只改按钮的文本和显隐，**不删也不建对象** —— 本函数会在"候选按钮被点击"的
 *    事件里被调到（上屏后清空拼音），这时那个按钮的事件正在跑，删它（或它父对象的
 *    孩子）会崩。所以 PE_MAX_CAND 个按钮在 attach 时一次建好，之后只复用。
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

/* ===================== 往输入框写字 ===================== */

/* 一律先跳末尾：本控件按"只在末尾追加"设计，不支持中间插字。
 * （用户点一下把光标挪到中间也无所谓 —— 下一次按键会把它拉回末尾。） */
static void insert_raw(const char *txt)
{
    lv_textarea_set_cursor_pos(s_ta, LV_TEXTAREA_CURSOR_LAST);
    lv_textarea_add_text(s_ta, txt);
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

/* ===================== 按键分发 ===================== */

/*
 * 键盘（components/pinyin_keyboard）把按键翻成语义报过来，**这里决定每一类要干什么**。
 *
 * 分工就是这么切的：键盘只管"按了哪一类键"，它不认识输入框也不认识引擎 ——
 * 所以"数字键到底是直选候选还是插入字符"这种判断必须在这边做，
 * 因为只有这边知道当前有没有候选。
 */
static void on_kb_key(pn_kb_key_t key, const char *txt, void *user)
{
    (void)user;

    switch (key) {
    case PN_KB_KEY_LETTER:
        /* 英文模式：字母直接进框，不碰引擎、不出候选 —— "英文模式"的全部就是这一条 */
        if (pinyin_kb_get_en_mode()) {
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

    case PN_KB_KEY_DIGIT:
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

    case PN_KB_KEY_PUNCT:
        /* 标点同理：先把拼音定案，再插标点（"中文里夹英文数字"走的就是这条路） */
        clear_pinyin();
        insert_raw(txt);
        return;

    case PN_KB_KEY_SPACE:
        if (s_ncand > 0) {
            commit(0);              /* 有候选：上屏首选（手不用离开键盘区） */
        } else {
            clear_pinyin();
            insert_raw(" ");
        }
        return;

    case PN_KB_KEY_BACKSPACE:
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

    case PN_KB_KEY_ENTER:
        /* 键盘不认识"提交"这个概念，所以由这里转一层：往**键盘对象**上发一个
         * LV_EVENT_READY，App 照旧监听它的 READY —— 和以前用 LVGL 键盘时一样。 */
        lv_obj_send_event(s_kb, LV_EVENT_READY, NULL);
        return;

    case PN_KB_KEY_MODE_CHANGED:
        /* 切中/英时把"还没上屏的拼音"定案：字母留在输入框里当普通文本，只清缓冲与
         * 候选栏 —— 和插数字/标点那条规则一致，不删用户看得见的字符。 */
        clear_pinyin();
        return;

    default:
        return;
    }
}

/* ===================== 对外接口 ===================== */

esp_err_t pinyin_input_attach(lv_obj_t *kb, lv_obj_t *ta)
{
    if (kb == NULL || ta == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    /* 刻意**不**在已挂过时报错：App 每次 enter 都会重新建 screen 并重新挂一次，
     * 旧的 keyboard / 候选栏随着旧 screen 一起没了。这里直接覆盖那套指针即可
     * （若报错，第二次进 App 输入法就再也起不来了）。 */
    s_kb     = kb;
    s_ta     = ta;
    s_py_len = 0;
    s_py[0]  = '\0';
    s_ncand  = 0;
    s_full_warned = false;

    /* 1) 挂按键回调。键盘（components/pinyin_keyboard）会把它认出来的按键当**语义事件**
     *    报过来；换页和中/英由键盘自己管，不经过这里。
     *    ⚠️ 这里不再往键盘上挂 LVGL 的事件回调了 —— 现在键盘是我们自己的组件，
     *       只用它这一条回调通道，两边职责才清楚。 */
    pinyin_kb_set_key_cb(on_kb_key, NULL);

    lv_obj_t *parent = lv_obj_get_parent(kb);

    /* 2) 候选栏：和键盘做兄弟，贴在键盘正上方（这个位置关系是本控件的内在约定，
     *    所以自己摆；App 只需要按 PINYIN_INPUT_BAR_H 给它留出高度）。
     *    句柄不用存 —— 它归 screen 所有，跟着 screen 一起释放。 */
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
     * lv_obj_get_width() 读的是**已缓存的坐标**（obj->coords），不是"你设进去的那个值"。
     * lv_obj_pos.h 里这两个函数的 @note 原话是：
     *     "The position of the object is recalculated only on the next redraw."
     * 而 App 是在 enter() 里刚建好键盘就调进来的 —— 那时还没经历过任何一次布局，
     * 键盘的 coords 全是 0。直接取宽度会拿到 0，后果是**连锁**的：
     *   ① 候选栏被设成 0 宽 —— 容器默认裁剪子对象，里面的候选**整条看不见**；
     *   ② 紧接着的 lv_obj_align_to 又用"候选栏自己的 0 宽"去算水平居中，
     *      位置也一起算错（实测 x 偏到了半个屏幕宽）。
     * 实测症状极具迷惑性：**敲拼音什么都不出来，但 ASCII 直插照常能用**
     * —— 因为 ASCII 那条路根本不碰候选栏。
     *
     * （旧的 pinyin_ime 没这个问题：它的候选栏尺寸是 App 传进来的显式常量，没去问键盘。）
     */
    lv_obj_update_layout(kb);
    lv_obj_set_size(bar, lv_obj_get_width(kb), PINYIN_INPUT_BAR_H);
    lv_obj_align_to(bar, kb, LV_ALIGN_OUT_TOP_MID, 0, 0);

    /* 候选区占满整条。
     * 拼音回显原先占左边 180px，现在拼音写在输入框里了，那一块省下来给候选 ——
     * 这是个白捡的好处：同样宽度能看到更多候选。
     *
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

    /* 3) 候选按钮一次建好，之后只改文本/显隐（理由见 refresh_bar 的说明）。
     *    宽度用 LV_SIZE_CONTENT：候选长度从 1 个字到十几个字都有，等分宽度放不下长的。 */
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

    refresh_bar();

    ESP_LOGI(TAG, "整句拼音输入法已挂上（候选每页最多 %d 个，输入上限 %d 字母）",
             PE_MAX_CAND, PE_INPUT_MAX);
    return ESP_OK;
}

void pinyin_input_detach(void)
{
    /* 把这次会话学到的东西落盘（没改动的话内部直接返回，不会碰卡）。
     * 放在清指针之前只是语义上更顺 —— flush 只读引擎的内存表，和这些指针无关。 */
    pinyin_learn_flush();

    /* 只清指针与状态，**不去删那些对象** —— 它们是 screen 的子孙，由 App 删 screen 时
     * 一起释放。这里删反而会重复释放。 */
    s_kb   = NULL;
    s_ta   = NULL;
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

void pinyin_input_reset(void)
{
    if (s_kb == NULL) {
        return;
    }
    /* ⚠️ 拼音现在写在输入框里，所以"丢掉还没上屏的拼音"必须**连框里那几个字母一起删**
     *    —— 否则用户会看到一串谁也解释不了的裸字母留在输入框里。
     *    （App 若先自己 set_text("") 再调这里也安全：核对不过就只清缓冲。） */
    ta_drop_pinyin();
    clear_pinyin();
}
