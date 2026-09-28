/*
 * pinyin_engine 词典的**维度与声明** —— 生成物，永不手改。
 *
 * 重新生成：python components/pinyin_engine/scripts/gen_engine_dict.py
 *
 * 为什么要单独一个头：数组在别的 .c 里定义，而引擎需要"元素个数"来二分查找。
 * 在这里把长度写进数组类型（`pe_dict_words[N]`），引擎就能用
 * `sizeof(a)/sizeof(a[0])` 拿到个数，而且**改数据后忘了改这里的长度会直接编译报错**
 * （数组声明与定义尺寸不一致）—— 比运行时才发现好得多。
 */
#pragma once

#include "pinyin_engine.h"

#define PE_MAX_KEY_LEN       20   /* 最长音串的字母数：DP 里 j-i 的上限 */
#define PE_MAX_SYL_LEN       6   /* 最长音节的字母数：只有不超过它才试"字边" */
#define PE_WORD_COUNT        30808
#define PE_SYLLABLE_COUNT    415
#define PE_MAX_CHARS_PER_SYL 20
#define PE_ABBREV_COUNT        5553      /* 缩写键（简拼组）个数 */
#define PE_ABBREV_ENTRY_COUNT  30808   /* 缩写表里词记录总条数 */
#define PE_MIN_ABBR_LEN      2   /* 缩写键长度下限：DP 里只有 >= 它才查 */
#define PE_MAX_ABBR_LEN      4   /* 缩写键长度上限：DP 里只有 <= 它才查 */

extern const pe_word_t     pe_dict_words[PE_WORD_COUNT];
extern const pe_syllable_t pe_dict_syllables[PE_SYLLABLE_COUNT];
extern const pe_abbr_t     pe_dict_abbrev[PE_ABBREV_COUNT];
/* 缩写表只存"词在 pe_dict_words 里的下标"，不重复存词串 —— 这是它只有 ~100KB
 * 而不是 ~700KB 的原因。代价是下标用 uint16，所以词表不能超过 65535 条： */
#if defined(__cplusplus)
static_assert(PE_WORD_COUNT <= 65535, "缩写表用 uint16 存词下标，词表超过 65535 条会静默截断");
#else
_Static_assert(PE_WORD_COUNT <= 65535, "缩写表用 uint16 存词下标，词表超过 65535 条会静默截断");
#endif
extern const uint16_t      pe_dict_abbrev_entries[PE_ABBREV_ENTRY_COUNT];
