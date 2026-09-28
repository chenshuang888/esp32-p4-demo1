/*
 * pinyin_keyboard —— 自研中文输入法键盘（键盘 + 候选栏 + 输入法逻辑，一次装好）
 *
 * 一次 `pinyin_keyboard_create()` 就把整套装起来：键盘、它正上方的候选栏、
 * 以及"按了键要发生什么"的输入法逻辑。
 *
 * ============================ 为什么键盘是自己搭的 ============================
 *
 * LVGL 自带的 `lv_keyboard` 用不了：它的布局表（`default_kb_map_lc/uc/spec`）和各自的
 * `ctrl_map` 都是 `lv_keyboard.c` 里的**文件内 static**。`lv_keyboard_get_map_array()`
 * 只能读回 map，**ctrl_map 连读都读不到**（只有 set）。所以"想在键面上写个中字"、
 * "想让数字行常驻"这类事一件都做不到 —— 只能自己用 `lv_buttonmatrix` 搭。
 *
 * ============================ 组件内部分两层（这条边界很重要） ============================
 *
 *   keyboard 那半（pinyin_keyboard.c 上半）  布局 / 分页 / 中英模式；把按键翻成
 *                                            **语义**（"按了字母 a"/"按了数字 3"）报出去
 *   ime 那半   （pinyin_keyboard.c 下半）    决定每一类键要干什么：拼音缓冲、候选栏、上屏
 *
 * ⚠️ keyboard 那半**不认识输入框、也不认识引擎** —— 所以"数字键到底是直选候选还是
 *    插字符"这种判断只能由 ime 那半拍板（只有它知道当前有没有候选）。
 *    **这条边界跟"是不是一个组件"无关**，它是防止键盘逻辑变浑的东西，别为了省事跨过去。
 *
 * ============================ 3 页 × 5 行 ============================
 *
 *   abc（默认）/ ABC / 符号
 *
 *   1 2 3 4 5 6 7 8 9 0        ← 数字行常驻，而且有候选时**数字 1~9 = 直选第 N 个**
 *   q w e r t y u i o p
 *   a s d f g h j k l
 *   ⇧ z x c v b n m ⌫
 *   #+=   中/EN   ，  [  空格  ]  。  ↵
 *
 * 与 LVGL 自带键盘的差别：数字行常驻、数字键接到"直选候选"、左下角是中/英切换、
 * 去掉了对中文没用的 `_` `-` `:` `←` `→`。
 *
 * ============================ 拼音写在输入框里 ============================
 *
 * 拼音字母**直接打在输入框里**（真实输入法就是这样），候选栏里只有候选。
 * 于是"上屏"= 把框里末尾那串拼音删掉 + 追加汉字 —— 这带来"末尾有几个字符是拼音"的
 * 记账问题（App 清空过输入框、用户中途插数字、光标被挪走、退格分两档都会让账失真）。
 * 实现上**不维护计数器，动手前核对一次**，详见 pinyin_keyboard.c 顶部。
 * 配套规则：插数字/标点、以及切中/英时，都先把当前拼音"定案"（字母留在框里当普通文本）。
 *
 * ============================ 用法 ============================
 *
 *     lv_obj_t *kb = pinyin_keyboard_create(body, ta, W, PINYIN_KEYBOARD_HEIGHT);
 *     lv_obj_add_event_cb(kb, on_submit, LV_EVENT_READY, NULL);   // 回车 = 提交
 *
 *     // leave 里：
 *     pinyin_keyboard_detach();      // 顺带把学习记录落盘
 *
 * 提交事件仍然走 `LV_EVENT_READY`，**App 的写法不用变** —— 键盘本身不认识"提交"，
 * 是本组件在收到回车键时往键盘对象上补发一个 READY（见 `on_key()` 的 ENTER 分支）。
 *
 * ⚠️ 键盘和候选栏是**一体**的：位置由本组件摆（贴 parent 底部）—— App 别自己再
 *    `lv_obj_align()` 挪键盘，挪了候选栏不会跟着走（它是一次性相对对齐算出来的）。
 * ⚠️ 调用方必须**已持有 LVGL 锁**。本组件不自己加锁。
 * ⚠️ **`ta` 的内容归本组件管**：拼音字母写在里面，光标会被强制拉到末尾；拼音还没
 *    上屏时不要从外面往里写字（对不上账时本组件会当作账已过期、丢掉缓冲）。
 * ⚠️ **只支持一个实例**（状态在文件级 static 里）：`create()` 会重置全部状态
 *    （包括模式回到中文），所以 App 每次 enter 重新建一个是正确用法。
 */
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 键盘本体高度（5 行）。App 排版面时按它 + `PINYIN_KEYBOARD_BAR_H` 预留。 */
#define PINYIN_KEYBOARD_HEIGHT   280

/** 候选栏高度（像素）。它贴在键盘正上方。 */
#define PINYIN_KEYBOARD_BAR_H     44

/** 键盘 + 候选栏一共占多少高 —— App 排显示区时减它就够了。 */
#define PINYIN_KEYBOARD_TOTAL_H  (PINYIN_KEYBOARD_HEIGHT + PINYIN_KEYBOARD_BAR_H)

/**
 * @brief 建一把"能打中文的键盘"：键盘 + 候选栏 + 输入法逻辑一起装好
 *
 * 键盘贴在 `parent` 的底部（左下角对齐），候选栏贴在键盘正上方。
 *
 * @param parent 父对象
 * @param ta     输入框（已上屏的文字 **+ 正在打的拼音**都写在里面）
 * @param w      键盘宽度（候选栏会照它对齐）
 * @param h      键盘高度，建议 `PINYIN_KEYBOARD_HEIGHT`
 * @return 键盘对象（**调用方需要它来监听 `LV_EVENT_READY` = 提交**）；
 *         NULL 表示参数为空或建对象失败
 */
lv_obj_t *pinyin_keyboard_create(lv_obj_t *parent, lv_obj_t *ta, int w, int h);

/**
 * @brief 解挂：清掉所有对象指针与状态，并把学习记录落盘
 *
 * App 的 leave 里调（项目里"enter 申请的游离资源 leave 里一个都不能漏"那条纪律）。
 * 没建过时调用是安全的。
 *
 * ⚠️ 落盘会阻塞几十毫秒 —— 那会儿界面正在切页，看不出来。每选一个词就写卡会有
 *    写放大，所以只在退出时写一次。详见 components/pinyin_learn/。
 */
void pinyin_keyboard_detach(void);

/**
 * @brief 丢掉还没上屏的拼音（**连输入框里那几个字母一起删**）
 *
 * 用于 App 提交之后收尾。没有拼音时调用是安全的。
 * ⚠️ 拼音写在输入框里，所以这个调用**会改动输入框**；若 App 自己已经先清空了
 *    输入框，这里只会清缓冲、不会误删。
 */
void pinyin_keyboard_reset(void);

/** @brief 当前是不是英文模式 */
bool pinyin_keyboard_get_en_mode(void);

/**
 * @brief 切中/英（会同步刷新键面文字，并把没上屏的拼音"定案"）
 *
 * `pinyin_keyboard_create()` 之后默认是**中文**模式。
 * 用户按左下角那个键走的就是这条路径。
 */
void pinyin_keyboard_set_en_mode(bool en);

#ifdef __cplusplus
}
#endif
