/*
 * pinyin_keyboard —— 自研中文输入法软键盘
 *
 * ============================ 它是什么 ============================
 *
 * 用 `lv_buttonmatrix` **从零搭**的一个键盘：布局、分页、中/英模式、按键语义
 * **全部是我们自己的**。
 *
 * 为什么不用 LVGL 自带的 `lv_keyboard`：它的布局表是 `lv_keyboard.c` 里的**文件内
 * static 数组**，外部既改不了键上写什么字，也拿不到 ctrl_map（只有 set、没有 get）。
 * 而我们要的恰好就是"按中文输入的习惯自由设计一个键盘" —— 所以只能自己搭。
 * （参考它的做法：`lv_buttonmatrix` 是通用控件，键盘这一层本来就很薄。）
 *
 * ============================ 它不认识谁 ============================
 *
 * **不认识拼音引擎、也不认识输入框。** 它只做一件事：把"用户按了哪一类键"翻译成
 * `pn_kb_key_t` 回调出去。
 *
 * 所以"数字键到底是直选候选还是插入字符"这种判断**不在这里** —— 由调用方
 * （`pinyin_input`）决定，因为只有它知道当前有没有候选。
 *
 * ============================ 3 页 × 5 行 ============================
 *
 *   abc（默认）        ABC               符号
 *   ---------------    ---------------   ---------------
 *   1 2 3 4 5 6 7 8 9 0                 ← 数字行常驻，且接到"直选候选"
 *   q w e r t y u i o p                 ← 10 键
 *   a s d f g h j k l                   ← 9 键（和真键盘一样略宽）
 *   ⇧ z x c v b n m ⌫                   ← ⇧/⌫ 加宽
 *   #+=  中/EN  ，  [  空格  ]  。  ↵      ← 中/英 在左下角（真键盘也在这儿）
 *
 * 与 LVGL 自带键盘的区别（也是自己做它的理由）：
 *   - **数字行常驻**，并且接到"直选候选"（按 3 = 选第 3 个候选）
 *   - 左下角是**中/英**，键面直接写当前模式
 *   - 去掉了对中文没用的 `_` `-` `:` `←` `→`
 *   - 逗号句号紧挨空格（中文里最常用的两个标点）
 *
 * ============================ 单实例 ============================
 *
 * 和项目里其它输入组件一样：状态放文件级 static，**同一时刻只支持一个键盘**。
 * `pinyin_kb_create()` 会重置全部状态（包括模式回到中文），所以 App 每次
 * enter 重新建一个设备是正确的用法。
 */
#pragma once

#include <stdbool.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 键盘高度：5 行。App 排版面时按它预留 —— **别写死数字**。 */
#define PINYIN_KB_HEIGHT   280

/**
 * 按键的**语义类型**（不是"哪个物理键"）。
 *
 * 键盘只报到这一类，具体怎么处理由调用方决定 —— 它才认识输入框和拼音引擎。
 * 比如 `PN_KB_KEY_DIGIT`：有候选时拿来直选、没候选时插入字符，是调用方的事。
 */
typedef enum {
    PN_KB_KEY_LETTER = 0,   /*!< 字母（带 txt） */
    PN_KB_KEY_DIGIT,        /*!< 数字（带 txt） */
    PN_KB_KEY_PUNCT,        /*!< 标点/符号（带 txt；**可能多字节**，如 "，" "。"） */
    PN_KB_KEY_BACKSPACE,    /*!< 退格（长按连发；txt 为 NULL） */
    PN_KB_KEY_SPACE,        /*!< 空格（txt 为 NULL，插入什么由调用方决定） */
    PN_KB_KEY_ENTER,        /*!< 回车/提交（txt 为 NULL） */
    PN_KB_KEY_MODE_CHANGED, /*!< 中/英 被切了（txt 为 NULL）；新状态用
                                  `pinyin_kb_get_en_mode()` 查 */
} pn_kb_key_t;

/**
 * 按键回调。
 *
 * @param key  语义类型
 * @param txt  **要插入的文本**（UTF-8，NUL 结尾），只对
 *             LETTER / DIGIT / PUNCT 有效；其余类型传 NULL ——
 *             免得调用方误把 `⌫` 的图标字符插进输入框。
 * @param user `pinyin_kb_set_key_cb()` 传进来的透传指针
 *
 * ⚠️ 跑在 LVGL 任务里（事件回调），里面**不要再加 LVGL 锁**。
 */
typedef void (*pn_kb_key_cb_t)(pn_kb_key_t key, const char *txt, void *user);

/**
 * @brief 建一个键盘
 *
 * ⚠️ 只支持一个实例（状态在文件级 static 里），重复调用会丢弃上一个。
 *
 * @param parent 父对象
 * @param w      宽度（键盘按这个宽度铺满）
 * @param h      高度（建议 `PINYIN_KB_HEIGHT`）
 * @return 键盘对象；调用方负责 align（本组件不摆位置）
 */
lv_obj_t *pinyin_kb_create(lv_obj_t *parent, int w, int h);

/**
 * @brief 挂按键回调
 *
 * 由 `pinyin_input` 在 attach 时调用。重复调用是覆盖（同项目里其它组件的做法）。
 */
void pinyin_kb_set_key_cb(pn_kb_key_cb_t cb, void *user);

/** @brief 当前是不是英文模式 */
bool pinyin_kb_get_en_mode(void);

/**
 * @brief 切中/英
 *
 * 会更新键面文字，并触发一次 `PN_KB_KEY_MODE_CHANGED` 回调。
 * `pinyin_kb_create()` 之后默认是**中文**模式。
 */
void pinyin_kb_set_en_mode(bool en);

#ifdef __cplusplus
}
#endif
