/*
 * AI Chat —— 实现（定位与"砍掉了什么"见头文件）
 *
 * ============================ 版面（键盘是按需弹出的） ============================
 *
 * 内容区 552 = 600(屏) - 48(状态栏)，三块自顶向下：
 *
 *     ┌───────────────────────┐
 *     │  聊天区（可纵向滚动）  │  ← 高度随键盘显隐在 488 / 172 之间变
 *     ├───────────────────────┤  GAP 8
 *     │  输入框   [新对话]     │  同一行，都是 48 高
 *     ├───────────────────────┤  GAP 8（只有在键盘显示时这两块才相邻）
 *     │  候选栏 44 ┐           │
 *     │  键盘   280 ┘ 324     │  ← 平时整块隐藏
 *     └───────────────────────┘
 *
 *   - 键盘收起：488 + 8 + 48 = 544，底部留 8 的边距
 *   - 键盘展开：172 + 8 + 48 = 228，正好接在 324 高的键盘上方（228 + 324 = 552）
 *   改任何一个数字都要重新对一遍这个加法（同 desktop.c / ime_test.c 的纪律）。
 *
 * ⚠️ **键盘和候选栏是一体的、而且候选栏的句柄没对外暴露**，所以"按需弹出"不能靠
 *    单独隐藏键盘对象 —— 那样候选栏会孤零零留在屏幕上。做法是给它们套一个
 *    **可整块隐藏的 holder**（见 ai_chat_enter 里建 s_kb_holder 那段）：键盘和候选栏
 *    都是 holder 的子对象，隐藏 holder 两个一起消失。
 * ⚠️ holder 必须有**确定的高度**（= PINYIN_KEYBOARD_TOTAL_H），因为 pinyin_keyboard
 *    是按"贴 parent 底部"给自己定位、再把候选栏贴到键盘上方的（pinyin_keyboard.c:721
 *    与 :652）。高度不对，候选栏就会跑到 holder 外面去。
 */
#include "ai_chat.h"

#include <stdbool.h>
#include <stdio.h>                  /* snprintf：给中断说明拼一句短话 */
#include <stdlib.h>                 /* strdup / free：对话历史 */
#include <string.h>

#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"

#include "app_manager.h"
#include "lcd_screen_display.h"     /* LCD_SCREEN_*_RES：算布局 */
#include "llm_client.h"
#include "pinyin_keyboard.h"        /* 键盘 + 候选栏 + 输入法（高度常量、创建接口） */
#include "ui_status_bar.h"

static const char *TAG = "ai_chat";

/* ---------- 版面常量 ---------- */
#define PAD        16
#define GAP         8
#define INPUT_H    48
#define NEW_BTN_W  110                                                        /* 「新对话」按钮宽 */
#define CONTENT_H  (LCD_SCREEN_V_RES - UI_STATUS_BAR_HEIGHT)                  /* = 552 */
#define CONTENT_W  (LCD_SCREEN_H_RES - PAD * 2)                               /* = 992 */
#define MSG_W      (CONTENT_W - 16)                                           /* 聊天区 pad 8*2 */
#define INPUT_W    (CONTENT_W - NEW_BTN_W - GAP)                              /* = 874 */

#define CHAT_H_FULL    (CONTENT_H - INPUT_H - GAP * 2)                        /* = 488 */
#define CHAT_H_TYPING  (CONTENT_H - PINYIN_KEYBOARD_TOTAL_H - INPUT_H - GAP)  /* = 172 */

/* 轮询结果用的定时器周期。⚠️ 它同时也是流式刷新的节奏（见 on_timer）——
 * 100ms 一次 = 把这段时间攒下的字合并成一次重排，这就是"批量刷新"。 */
#define POLL_MS 100

/* 进 App 时聊天区里的那句提示。它是**界面文案，不是对话消息** ——
 * 所以只画出来，不记进上下文（否则模型会以为它自己说过这句话）。 */
#define GREETING "点下面的输入框开始对话，按键盘上的回车发送。"

static lv_obj_t   *s_scr       = NULL;
static lv_obj_t   *s_chat      = NULL;   /* 聊天记录：可纵向滚动的 flex 列 */
static lv_obj_t   *s_input     = NULL;   /* 单行输入框 */
static lv_obj_t   *s_new_btn   = NULL;   /* 「新对话」 */
static lv_obj_t   *s_kb_holder = NULL;   /* 键盘 + 候选栏的容器（整块显隐它） */
static lv_obj_t   *s_kb        = NULL;
static lv_obj_t   *s_pending   = NULL;   /* 正在流式输出的那条 label（也当"正在生成…"占位） */
static lv_timer_t *s_timer     = NULL;

/* 接流式正文的暂存缓冲：llm_client_copy_text() 每次给**全量**，拷到这里再上屏。
 * ⚠️ 必须 ≥ LLM_TEXT_MAX，否则流到一半会缺字（结束时 poll 给的那份完整、能补上，
 *    但中途看着会少）。 */
static char s_stream[LLM_TEXT_MAX];

/* ---------- 对话历史（多轮上下文） ----------
 *
 * 服务端**无状态**：每次请求都得把整段历史重发一遍（协议要求，见 llm_client.h）。
 * 所以历史由本 App 持有 —— 它本来就要把每条消息画到屏幕上、本来就有这些文本。
 * 组件那边只是把这份数组拷走再拼 JSON，它自己不记会话。
 *
 * ⚠️ 文本是 strdup 出来的堆内存：**leave 里必须 hist_clear()**，否则就是
 *    "游离资源 leave 里漏了"。也就是说**退出 App 就等于结束这次对话**
 *    （回归时历史是空的，和界面一致 —— 这正是把历史交给 App 持有的好处）。
 * ⚠️ 请求失败要**回滚**（hist_pop）：否则下一轮的上下文里会留一条"没人回答的问题"，
 *    角色也就不交替了。
 * ⚠️ 不做裁剪：纯文本对话几十条也就几 KB，离任何上限都远（详见 llm_client.h 里
 *    "请求体大小 ≠ 上下文窗口"那段）。真需要了再按字符预算裁。
 */
static llm_msg_t s_hist[LLM_MAX_MSGS];
static int       s_hist_n = 0;

/* 追加一条历史。失败（数组满/内存不足）返回 false，由调用方决定怎么办。 */
static bool hist_push(bool is_ai, const char *text)
{
    if (s_hist_n >= LLM_MAX_MSGS) {
        ESP_LOGE(TAG, "历史已达 %d 条上限，本条不记入上下文", LLM_MAX_MSGS);
        return false;
    }
    char *copy = strdup(text);
    if (copy == NULL) {
        ESP_LOGE(TAG, "历史拷贝失败（内存不足）");
        return false;
    }
    s_hist[s_hist_n].text  = copy;
    s_hist[s_hist_n].is_ai = is_ai;
    s_hist_n++;
    return true;
}

/* 撤掉最后一条（失败回滚用）。空表时调用是安全的。 */
static void hist_pop(void)
{
    if (s_hist_n <= 0) {
        return;
    }
    s_hist_n--;
    free((void *)s_hist[s_hist_n].text);    /* text 是 const char*，free 要转一下 */
    s_hist[s_hist_n].text = NULL;
}

static void hist_clear(void)
{
    while (s_hist_n > 0) {
        hist_pop();
    }
}


/* ---------- 聊天区 ---------- */

/* 滚到最底（新消息 / 流式刷新后调）。 */
static void scroll_to_bottom(void)
{
    if (s_chat == NULL) {
        return;
    }
    /* ⚠️ 先强制刷一次布局：label 刚被 set_text，新高度还没算出来，此时算出的
     *    "最大滚动量"还是旧的，滚过去会差一跳。
     * ⚠️ **不能用 `lv_obj_scroll_to_view(最后一个孩子)`**（原来就是这么写的）：
     *    流式那条 label 会长得比容器还高，而 scroll_to_view 在"对象比容器高"时
     *    会先把**顶部**对齐 —— 于是每 100ms 就把视图拽回这条消息的开头，
     *    看着像根本没在滚。直接滚到容器最底才对。 */
    lv_obj_update_layout(s_chat);
    lv_obj_scroll_to_y(s_chat, LV_COORD_MAX, LV_ANIM_OFF);
}

/*
 * 追加一条消息，返回它的 label（"思考中"那条稍后要替换文本）。
 *
 * 区分角色**只用文字颜色**：气泡、头像、时间戳都砍了。
 * ⚠️ lv_label_set_text 会把字符串**拷进** label 自己的缓冲，所以调用方传的临时串
 *    随后失效也没关系（on_send 里清空输入框就不会影响已加进去的消息）。
 */
static lv_obj_t *add_message(const char *text, bool is_ai)
{
    lv_obj_t *lb = lv_label_create(s_chat);
    lv_obj_set_width(lb, MSG_W);
    lv_label_set_long_mode(lb, LV_LABEL_LONG_WRAP);
    lv_label_set_text(lb, text);
    if (is_ai) {
        /* 深蓝：和用户的黑字区分开。不是"美化"，是为了能一眼看出谁说的。 */
        lv_obj_set_style_text_color(lb, lv_color_hex(0x1A4E8A), 0);
    }
    scroll_to_bottom();
    return lb;
}

/* ---------- 版面 ---------- */

/*
 * 按"键盘是否展开"重排。**所有尺寸变化都收在这一个函数里** —— 散在事件回调里
 * 早晚会算错加法。
 */
static void apply_layout(bool typing)
{
    if (s_chat == NULL || s_input == NULL || s_kb_holder == NULL) {
        return;
    }
    const int chat_h = typing ? CHAT_H_TYPING : CHAT_H_FULL;

    lv_obj_set_size(s_chat, CONTENT_W, chat_h);
    lv_obj_align(s_chat, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_align(s_input, LV_ALIGN_TOP_LEFT, 0, chat_h + GAP);
    if (s_new_btn != NULL) {
        lv_obj_align(s_new_btn, LV_ALIGN_TOP_RIGHT, 0, chat_h + GAP);
    }

    if (typing) {
        lv_obj_remove_flag(s_kb_holder, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_kb_holder, LV_OBJ_FLAG_HIDDEN);
    }
    scroll_to_bottom();
}

/* ---------- 事件 ---------- */

/* 点输入框 = 要打字：展开键盘。已经展开就不重复排（省一次无谓的重排）。 */
static void on_input_clicked(lv_event_t *e)
{
    (void)e;
    if (s_kb_holder != NULL && lv_obj_has_flag(s_kb_holder, LV_OBJ_FLAG_HIDDEN)) {
        apply_layout(true);
    }
}

/* 点聊天区 = 想看清楚：收起键盘，把屏幕让给消息。
 * （LVGL 里"拖动滚动"不会触发 CLICKED，所以上下翻消息不会误收键盘。） */
static void on_chat_clicked(lv_event_t *e)
{
    (void)e;
    if (s_kb_holder != NULL && !lv_obj_has_flag(s_kb_holder, LV_OBJ_FLAG_HIDDEN)) {
        apply_layout(false);
    }
}

/*
 * 「新对话」：把上下文和聊天区一起清掉，回到刚进 App 的样子。
 *
 * ⚠️ 生成中直接忽略：那条回复回来时会往一段空历史里追加，上下文就错位了
 *    （会变成"模型凭空说了一句、前面没有提问"）。与发送时的处理一致。
 */
static void on_new_chat(lv_event_t *e)
{
    (void)e;

    if (llm_client_busy()) {
        ESP_LOGW(TAG, "还在生成，忽略「新对话」");
        return;
    }

    hist_clear();

    /* ⚠️ 顺序要紧：先摘掉"思考中"的指针，再清容器 —— lv_obj_clean 会把它的子对象
     *    全删掉，s_pending 立刻就成了野指针（on_timer 还会用它）。
     *    （这里删的都是 s_chat 的子对象，点按钮的事件挂在按钮上，不在其中，安全。） */
    s_pending = NULL;
    lv_obj_clean(s_chat);

    add_message(GREETING, true);
    ESP_LOGI(TAG, "新对话：上下文与聊天区已清空");
}

/*
 * 发送 = 按键盘上的回车（pinyin_keyboard 收到回车键会往键盘对象补发 LV_EVENT_READY）。
 * ⚠️ 不要在这里加 LVGL 锁：事件回调本来就跑在 LVGL 任务里、锁已持有。
 */
static void on_send(lv_event_t *e)
{
    (void)e;

    const char *txt = lv_textarea_get_text(s_input);
    if (txt == NULL || txt[0] == '\0') {
        return;                     /* 空输入不发 */
    }

    /* 上一条还没生成完：**直接忽略**，不清输入框、也不往聊天区加东西 ——
     * 用户那句话原样留着，等回复到了再按一次回车即可。
     * （把"被忽略"也加一条消息进聊天区会很吵，而且容易让人以为发出去了。） */
    if (llm_client_busy()) {
        ESP_LOGW(TAG, "上一条还在生成，本次发送忽略");
        return;
    }

    /* ⚠️ 顺序：先把这句记进上下文（请求要带上它），再发。
     *    记不进去就别发了 —— 上下文不完整，模型答的会不对。 */
    if (!hist_push(false, txt)) {
        add_message("（内存不足，这句没能记入上下文，未发送）", true);
        return;
    }

    /* 用户这句上屏。lv_label_set_text 会拷贝，所以下面清空输入框不影响它。 */
    add_message(txt, false);

    /* 整段历史发出去（服务端无状态，每次都全量重发 —— 见 llm_client.h）。
     * ⚠️ 必须在清空输入框/动拼音缓冲**之前**：llm_client_request 会拷贝，
     *    但它读的 s_hist 里最后一条已经是上面的 strdup 副本了，所以其实安全；
     *    这里保持"先发再清"的顺序是为了不依赖那个细节。 */
    const esp_err_t rc = llm_client_request(s_hist, s_hist_n);

    /* 提交后收尾（顺序同 ime_test.c:86-90：先 reset 丢掉还没上屏的拼音，再清空） */
    pinyin_keyboard_reset();
    lv_textarea_set_text(s_input, "");

    if (rc != ESP_OK) {
        /* 这条压根没发出去，回滚掉，别留在上下文里。
         * 屏幕上的那条保留 —— 用户确实说过，抹掉他会以为是 bug。 */
        hist_pop();
        add_message(rc == ESP_ERR_NOT_FINISHED ? "（上一条还在生成）"
                                               : "（发送失败：对话服务未就绪）", true);
        return;
    }

    /* 占位：结果到了由 on_timer 替换它的文本 */
    s_pending = add_message("（正在生成…）", true);

    /* 收起键盘：把屏幕让给回复区（也更接近手机上的行为） */
    apply_layout(false);
}

/*
 * 轮询：流式过程中刷屏 + 结束时收尾。跑在 lv_timer 里 —— 也就是 **LVGL 任务上下文**，
 * 所以能安全地动控件。
 *
 * ⚠️ 定时器的周期（100ms）本身**就是**批量：这段时间攒下的正文一次性刷上去，
 *    绝不会每来一个字就重排一遍 label（长回复那样是 O(n²) 的开销）。所以这里
 *    不需要再做额外的节流。
 */
static void on_timer(lv_timer_t *t)
{
    (void)t;

    llm_reply_t r;
    if (llm_client_poll(&r)) {
        /* ---------- 结束了（成功或失败）---------- */

        /* ⚠️ 先更新上下文，再动界面 —— 两件事都要做，别塞进同一个 if 里，
         *    否则哪天 s_pending 为空就会把上下文漏更新。 */
        if (r.ok) {
            /* 成功：把模型这句追加进历史（hist_push 内部会 strdup，
             * 所以在下面 llm_client_reply_free 之后也依然是活的）。
             * 万一记不进去（表满/内存不足），就把这一轮整个撤掉 —— 只留用户那句会让
             * 角色不交替，和失败回滚是同一个理由。 */
            if (!hist_push(true, r.text)) {
                hist_pop();
                ESP_LOGW(TAG, "回复没能记入上下文，这一轮已从上下文里撤掉");
            }
        } else {
            /* 失败：回滚掉本次的用户提问（发送时 push、这里 pop，正好配平）。
             * 不滚的话上下文里会留一条"没人回答的问题"，角色也不交替了。
             * ⚠️ 代价：**失败时界面与历史会不一致**（屏幕上留着那条提问，历史里没有）。
             *    这是有意的取舍 —— 见 on_send 里的说明。 */
            hist_pop();
        }

        if (s_pending != NULL) {
            if (r.ok) {
                /* 用 poll 给的正文定稿（它是完整的一份独立拷贝）。 */
                lv_label_set_text(s_pending, r.text);
                s_pending = NULL;
            } else if (llm_client_copy_text(s_stream, sizeof(s_stream)) > 0) {
                /* **已经流出一部分了**：把它留在屏幕上定稿，中断原因另起一条 ——
                 * 把用户正在读的文字抹掉换成一句错误提示，体验更差。 */
                lv_label_set_text(s_pending, s_stream);
                s_pending = NULL;
                char note[LLM_ERR_MAX + 32];
                snprintf(note, sizeof(note), "（已中断：%s）", r.err);
                add_message(note, true);
            } else {
                /* 一个字都还没出来：直接就在占位那条上写原因 */
                lv_label_set_text_fmt(s_pending, "（失败：%s）", r.err);
                s_pending = NULL;
            }
            scroll_to_bottom();
        }

        /* ⚠️ 正文是堆内存、所有权已经在 poll 时交给我们了，必须还回去。
         *    漏了这句就是每次对话漏一块。 */
        llm_client_reply_free(&r);
        return;
    }

    /* ---------- 还没结束：把当前收到的正文刷到屏幕上 ---------- */
    /* copy_text 每次给的是**全量**，所以直接 set 就行，App 不用记账。 */
    if (s_pending != NULL && llm_client_copy_text(s_stream, sizeof(s_stream)) > 0) {
        lv_label_set_text(s_pending, s_stream);
        scroll_to_bottom();
    }
}

/* ---------- App 生命周期 ---------- */

static void ai_chat_enter(void)
{
    ESP_LOGI(TAG, "enter");

    /* lvgl_port_lock 是递归锁：从点击回调进来也不会死锁 */
    lvgl_port_lock(0);

    s_scr = lv_obj_create(NULL);

    const ui_status_bar_cfg_t bar = { .title = "AI Chat", .show_back = true };
    ui_status_bar_apply(&bar);

    /* 坐标基准容器。**不设 pad**，理由同 ime_test.c:123-125。 */
    lv_obj_t *body = lv_obj_create(s_scr);
    lv_obj_remove_style_all(body);
    lv_obj_set_size(body, CONTENT_W, CONTENT_H);
    lv_obj_align(body, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);

    /* 聊天记录：flex 列从上往下排，超出高度就能滚。
     * ⚠️ flex 必须在 remove_style_all **之后**设（它会连 layout 一起清掉）。 */
    s_chat = lv_obj_create(body);
    lv_obj_remove_style_all(s_chat);
    lv_obj_set_size(s_chat, CONTENT_W, CHAT_H_FULL);
    lv_obj_set_flex_flow(s_chat, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_chat, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(s_chat, 8, 0);
    lv_obj_set_style_pad_row(s_chat, 8, 0);
    lv_obj_set_scroll_dir(s_chat, LV_DIR_VER);
    lv_obj_add_event_cb(s_chat, on_chat_clicked, LV_EVENT_CLICKED, NULL);

    /* 输入框（单行）。点它展开键盘。max_length 默认 0 = 不限长。 */
    s_input = lv_textarea_create(body);
    lv_textarea_set_one_line(s_input, true);
    lv_textarea_set_placeholder_text(s_input, "点这里输入，按回车发送");
    lv_obj_set_size(s_input, INPUT_W, INPUT_H);
    lv_obj_add_event_cb(s_input, on_input_clicked, LV_EVENT_CLICKED, NULL);

    /* 「新对话」按钮，占输入框右边那一小块（具体位置由 apply_layout 摆）。
     * ⚠️ **不要给里面的 label 设字号**：中文只有 18px 一档，设了别的字号会让汉字
     *    变成占位方块（同 ime_test.c 顶部那条）。不设就继承全局的混排字体。 */
    s_new_btn = lv_button_create(body);
    lv_obj_set_size(s_new_btn, NEW_BTN_W, INPUT_H);
    lv_obj_t *new_lb = lv_label_create(s_new_btn);
    lv_label_set_text(new_lb, "新对话");
    lv_obj_center(new_lb);
    lv_obj_add_event_cb(s_new_btn, on_new_chat, LV_EVENT_CLICKED, NULL);

    /* 键盘的"可隐藏容器"：见文件头那两条 ⚠️（高度必须正好是键盘 + 候选栏）。 */
    s_kb_holder = lv_obj_create(body);
    lv_obj_remove_style_all(s_kb_holder);
    lv_obj_set_size(s_kb_holder, CONTENT_W, PINYIN_KEYBOARD_TOTAL_H);
    lv_obj_align(s_kb_holder, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_remove_flag(s_kb_holder, LV_OBJ_FLAG_SCROLLABLE);

    /* 键盘 + 候选栏 + 输入法一次装好（父对象是 holder，不是 body）。
     * ⚠️ 键盘和候选栏的位置由本组件自己摆 —— 这里不要再 align 键盘。 */
    s_kb = pinyin_keyboard_create(s_kb_holder, s_input, CONTENT_W, PINYIN_KEYBOARD_HEIGHT);
    if (s_kb == NULL) {
        ESP_LOGE(TAG, "键盘创建失败，本 App 只能看不能打字");
    } else {
        lv_obj_add_event_cb(s_kb, on_send, LV_EVENT_READY, NULL);
    }

    /* 首屏那句提示是**界面文案，不是对话消息** —— 只画出来，不记进上下文。 */
    add_message(GREETING, true);

    /* 一次性把所有对象摆到位：位置/尺寸/显隐统统由 apply_layout 负责（单一出处，
     * 散在别处早晚会算错加法）。传 false = 键盘收起。 */
    apply_layout(false);

    /* 结果轮询 —— 同时也是**流式刷新的节奏**（流式期间 on_timer 每跳都会把新收到的
     * 正文刷上去，见 on_timer）。 */
    s_timer = lv_timer_create(on_timer, POLL_MS, NULL);
    if (s_timer == NULL) {
        ESP_LOGE(TAG, "定时器创建失败，本 App 收不到回复");
    }

    lv_screen_load(s_scr);
    lvgl_port_unlock();
}

static void ai_chat_leave(void)
{
    ESP_LOGI(TAG, "leave");

    lvgl_port_lock(0);

    /* 游离资源一律在这里清干净（项目纪律：enter 申请的，leave 一个都不能漏）。 */
    if (s_timer != NULL) {
        lv_timer_delete(s_timer);
        s_timer = NULL;
    }

    /* 取走残留的结果并释放正文 —— 结果队列深度 1，不收就漏一块堆内存。
     * 如果此刻正有一条请求在飞：它以后投进来的那条没人取，由下一次
     * llm_client_request() 顺带清掉，不会累积，所以这里不必等它。 */
    llm_reply_t r;
    while (llm_client_poll(&r)) {
        llm_client_reply_free(&r);
    }

    /* 对话历史是 strdup 出来的堆内存 —— "enter 申请的、游离于 screen 之外的资源，
     * leave 里一个都不能漏"。清掉它也就意味着**退出 App 即结束这次对话**：
     * 下次进来历史是空的，和界面（只有一句提示）保持一致。 */
    hist_clear();

    /* 先解挂键盘：它把输入框/候选栏的指针存在 static 里，不告诉它就把 screen 删了，
     * 下次 enter 再建时会指着已释放的对象。顺带把输入习惯落盘。 */
    pinyin_keyboard_detach();

    lv_obj_delete_async(s_scr);
    s_scr       = NULL;
    s_chat      = NULL;
    s_input     = NULL;
    s_new_btn   = NULL;
    s_kb_holder = NULL;
    s_kb        = NULL;
    s_pending   = NULL;

    lvgl_port_unlock();
}

static const app_desc_t s_desc = {
    .name  = "AI Chat",
    .icon  = "",            /* 没有图标：picture_icon() 查不到返回 NULL，桌面只显示名字 */
    .enter = ai_chat_enter,
    .leave = ai_chat_leave,
};

void ai_chat_register(void)
{
    app_manager_register(&s_desc);
}
