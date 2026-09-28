/*
 * pinyin_ime —— 中文输入（拼音输入法）
 *
 * ============================ 这个组件是什么 ============================
 *
 * 两部分：
 *   1) **词典资源** —— dicts/lv_pinyin_dict.c：415 个音节 -> 按字频排好序的候选汉字，
 *      由 scripts/gen_pinyin_dict.py 生成（数据出处、格式、四条铁律都写在脚本顶部）。
 *      这是本组件真正的"内容"，约 45 KB rodata。
 *   2) **一层薄接入** —— pinyin_ime_attach()：把词典装进 LVGL 的输入法，并绕开两个坑。
 *
 * ====================== 用的是 LVGL 自带的输入法 ======================
 *
 * 输入法本身**不是我们写的**，是 LVGL 9.5 自带的 lv_ime_pinyin
 * （managed_components/lvgl__lvgl/src/widgets/ime/lv_ime_pinyin.c）。
 * 它自带：拼音缓冲、退格、候选栏、候选栏首尾的 `<` `>` 翻页、"点候选则把 textarea 里
 * 那串拼音替换成汉字"（靠内部的 ta_count 记账）。这些都不用我们实现。
 *
 * 我们只做两件 LVGL 没替我们做的事：**提供简体词典**、**绕开两个坑**（见下）。
 *
 * 前置条件：sdkconfig.defaults 里的三项目必须已生效
 *   CONFIG_LV_USE_IME_PINYIN=y                不开则 lv_ime_pinyin_* 全套 API 都不存在
 *   CONFIG_LV_IME_PINYIN_USE_DEFAULT_DICT=n   关掉内置繁体词典（否则白占 ~12KB 固件）
 *   CONFIG_LV_IME_PINYIN_CAND_TEXT_NUM=9      候选栏每页 9 个（默认 6，翻页太久）
 * ⚠️ 这三项在 sdkconfig 里若已是 "# ... is not set"，defaults 不会覆盖 —— 必须删掉
 *    sdkconfig 重新生成。详见 sdkconfig.defaults 的"拼音输入法"一节。
 *
 * ============================ 怎么用 ============================
 *
 *     lv_obj_t *kb  = lv_keyboard_create(page);
 *     lv_obj_t *ime = pinyin_ime_attach(kb);          // 必须在 set_textarea 之前/之后都行
 *     lv_keyboard_set_textarea(kb, ta);               // 键盘仍然要绑 textarea
 *
 *     // 候选栏由 IME 创建，是**键盘的兄弟对象** —— 大小/位置由调用方摆：
 *     lv_obj_t *cand = lv_ime_pinyin_get_cand_panel(ime);
 *     lv_obj_set_size(cand, lv_pct(100), 44);
 *     lv_obj_align_to(cand, kb, LV_ALIGN_OUT_TOP_MID, 0, 0);
 *
 * ⚠️ 调用方必须**已持有 LVGL 锁**（App 的 enter 里天然持有）。本组件不自己加锁，
 *    所以也不依赖 esp_lvgl_port。
 *
 * ⚠️ **不要**给 WiFi 密码那种字段挂它（apps/settings.c）—— 那个要的是纯 ASCII，
 *    挂上输入法反而会让输入的字母被当成拼音吃掉。
 *
 * ============================ 已知限制 ============================
 *
 * 1) **只能单字候选**：LVGL 的候选缓冲每格只有 4 字节（源码里的
 *    lv_pinyin_cand_str[][4]），装不下词组。所以 "nihao" 要分两次选字。
 * 2) 全局同时只有一个 IME 可用：候选字符串数组是 lv_ime_pinyin.c 的文件级 static
 *    （lv_pinyin_cand_str / lv_btnm_def_pinyin_sel_map），多个实例会互相踩。
 * 3) 大小写模式下输入法不工作：IME 只接小写 a-z，按 `ABC` 后输入的是纯 ASCII。
 *    这是**需要的特性**（输 API key / URL 时就是要这样），不是缺陷。
 */
#pragma once

#include "lvgl.h"       /* 对外给的是 lv_obj_t*，所以这里必须引 LVGL（同 picture.h 的做法）*/

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 把一个拼音输入法挂到已经建好的键盘上
 *
 * 依次做四件事：
 *   1) 建 IME 对象、绑定键盘（同时把候选栏的父对象也挂到键盘的父对象上）
 *   2) 装本组件的简体词典（按字频排序；运行时替换掉 LVGL 内置的繁体那份）
 *   3) 给候选栏设字体 —— 它是键盘的兄弟、**不会**从 IME 对象继承字体，
 *      不设的话汉字会渲染成占位方块
 *   4) 在键盘上追加一个回调，把左下角那个键（LV_SYMBOL_KEYBOARD）造成的
 *      "模式错配"按回去（理由见 pinyin_ime.c 顶部"坑 2"）
 *
 * 不做布局：候选栏的大小与位置由调用方自己摆（用 lv_ime_pinyin_get_cand_panel() 拿）。
 *
 * @param kb 已经建好的 lv_keyboard（非 NULL）
 * @return IME 对象；NULL 表示入参为空或建对象失败（内存不足）
 */
lv_obj_t *pinyin_ime_attach(lv_obj_t *kb);

#ifdef __cplusplus
}
#endif
