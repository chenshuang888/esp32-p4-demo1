/*
 * pinyin_engine —— 拼音转汉字（**整句转换**，纯 C）
 *
 * ============================ 它做什么 ============================
 *
 * 输入一串拼音字母，输出若干个**完整的**转换结果（按可能性排序）：
 *
 *     "jintiantianqi"  ->  "今天天气" / "今天天其" / "今天天起" / ...
 *     "nihao"          ->  "你好" / "你号" / "尼好" / ...
 *     "bjdx"           ->  "北京大学" / ...        ← 简拼（首字母缩写）
 *     "nh" + "sj"      ->  "你好世界"              ← 简拼可以自由组合、也可以和全拼混着打
 *
 * 一个候选 = 一次完整的转换，调用方选一个整段上屏即可。这是手机输入法的模式，
 * 不是"逐字逐词选"。做法是在字母串上做最短路径（DP），详见 .c 顶部。
 *
 * 三种边：**词边**（子串命中词表）、**字边**（子串是合法音节）、
 * **缩写边**（子串命中缩写表，即"简拼"）。哪个子串该走哪种边、怎么切分，
 * DP 自己找最优解 —— 所以全拼、简拼、两者的混合都不需要调用方区分。
 *
 * ============================ 设计约束 ============================
 *
 * - **零依赖**：只用 <string.h> 和 <stdint.h>。不碰 LVGL、不碰 FreeRTOS、不碰 esp_log，
 *   词典是编译进 flash 的 const 数据（见 dicts/）。所以它可以在任何地方被调用。
 *   （用户学习的**存储**也不在这里 —— 引擎只维护内存里那张表，由调用方喂入/取走。）
 * - **纯整数运算**：代价值在生成词典时就算好并量化成 1 个字节（见生成脚本），
 *   引擎里没有浮点、没有 math.h。又快又不吃栈。
 * - ⚠️ **不可重入**：内部路径表是文件级 static（故意的，理由见 .c 顶部）。
 *   只在 LVGL 任务里调用它，别在多任务里并发调。
 *
 * ============================ 词典从哪来 ============================
 *
 * dicts/ 下是生成物（词表 + 单字表 + 缩写表），由
 * `python components/pinyin_engine/scripts/gen_engine_dict.py` 生成。
 * 数据来源、量化算法、DP 的做法全写在那个脚本顶部的注释里。**别手改生成物。**
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 一次最多给出几个候选（候选栏一屏的数量级） */
#define PE_MAX_CAND      12
/** 单个候选的字节上限（约 18 个汉字）。超出这个长度的转换路径会被丢弃 —— 见 .c 的说明 */
#define PE_MAX_CAND_LEN  56
/** 输入拼音的字母数上限。输入法控件要按它限制用户的输入长度 */
#define PE_INPUT_MAX     28

/* ===================== 用户学习（词级个性化） =====================
 *
 * 引擎**不碰存储**：用户表由调用方（components/pinyin_learn）从 SD 卡读出来喂进来，
 * 引擎只在内存里维护它、并在转换时据此调整词的代价。和 wifi_service 收凭据是同一个模式。
 *
 * ⚠️ 学习是**词级**的，不是"输入串 -> 文本"那种整串记忆。理由是简拼的候选池
 *    按**词**组织：用户打 "nh" 想要「你好」，而「你好」在 nh 组里排第 9，压根不在
 *    候选栏里 —— 他选不到，也就永远教不会系统。只有"用户在别处选了「你好」→
 *    「你好」这个词的代价下降"才能解开这个死锁（下次它在 nh 组里就自动爬上来了）。
 */
/** 用户表最多记多少个词（超出后淘汰命中次数最少的） */
#define PE_LEARN_MAX      128
/** 一个词文本的字节上限（词表里最长 4 个汉字 = 12 字节，这里留足余量） */
#define PE_LEARN_TEXT_MAX 24

/**
 * @brief 一个候选：转换结果 + 代价
 *
 * `cost` 越小越优，只用于观察/调试（比如打日志看排序是否异常），
 * **不要**拿它当业务判断 —— 它的单位是"0.25 个自然对数"，是量化过的。
 */
typedef struct {
    char    text[PE_MAX_CAND_LEN];
    int16_t cost;
} pe_cand_t;

/* ===================== 词典数据的记录类型 =====================
 * 这两个类型是"生成物"和"引擎"之间的契约：dicts/ 下按这个布局写数组，
 * 引擎按这个布局读。改这里 = 两个生成物都要重新生成。
 */

/** @brief 词表的一条记录（音串 -> 一个候选词） */
typedef struct {
    const char *py;      /*!< 音串，如 "jintiantianqi"（NUL 结尾） */
    const char *word;    /*!< 词，UTF-8 且 NUL 结尾 */
    uint8_t     cost;    /*!< 量化代价；越大越不常用 */
} pe_word_t;

/** @brief 单字表的一条记录（音节 -> 一批候选字） */
typedef struct {
    const char    *syl;      /*!< 音节，如 "hao"（NUL 结尾） */
    const char    *chars;    /*!< 候选汉字 UTF-8 拼接（3/4 字节一字），按 主读音优先、字频降序 */
    const uint8_t *costs;    /*!< 与 chars **逐字对应**的量化代价 */
    uint8_t        count;    /*!< 候选字个数（也等于 costs 的长度） */
} pe_syllable_t;

/** @brief 缩写表的一条记录（声母序列 -> 一批候选词），即"简拼"用的索引 */
typedef struct {
    /**
     * 缩写键，**打包成 uint32**：4 个槽 × 5 bit，a-z -> 1..26，不足 4 位补 0，高位在前。
     * 这样打包出来的整数大小关系与字符串字典序一致，所以能直接对 uint32 二分 ——
     * 省掉 strcmp，也省掉"字符串池 + 偏移数组"那一整套。见生成脚本的 pack_abbr()。
     */
    uint32_t key;
    /** 本组在 pe_dict_abbrev_entries[] 里的起始下标 */
    uint16_t first;
    /** 本组词数 */
    uint8_t  count;
    /**
     * 该组的信息量罚项 = round(log(count) * COST_SCALE)，越大越歧义。
     * 缩写比全拼歧义得多（实测最大的组有 248 个词），不分档的话打 "da" 会先出「答案」
     * 而不是「大」。按组大小罚之后，组小的 "bjdx" 几乎不罚、组大的 "zz" 罚得多。
     */
    uint8_t  penalty;
} pe_abbr_t;

/* ===================== 用户学习：记录类型与接口 ===================== */

/** @brief 用户表的一条：一个词 + 它被用户选中过的次数 */
typedef struct {
    char    text[PE_LEARN_TEXT_MAX];   /*!< 词的 UTF-8 文本（NUL 结尾） */
    uint8_t count;                     /*!< 命中次数，引擎内部饱和在 255 */
} pe_user_t;

/**
 * @brief 灌入用户表（开机由持久化层从 SD 卡读出来后调用）
 *
 * 覆盖式的：调一次就替换掉之前的内容。n 超过 PE_LEARN_MAX 时只取前 PE_LEARN_MAX 条。
 * 词长超过 PE_LEARN_TEXT_MAX 的条目会被跳过。
 */
void pinyin_engine_user_load(const pe_user_t *tab, int n);

/**
 * @brief 导出用户表（供持久化层写回存储）
 *
 * @param[out] out  输出数组
 * @param[in]  max  out 的容量
 * @return 实际写出的条数（0 表示当前没有学习记录）
 */
int pinyin_engine_user_save(pe_user_t *out, int max);

/** @brief 清空用户表（设置里的"清除输入习惯"） */
void pinyin_engine_user_clear(void);

/**
 * @brief 把"用户刚选中了第 idx 个候选"记一笔
 *
 * 从该候选的路径**回溯出分词**，把路径上的每个词计数 +1（字边不算 —— 单字学习价值低，
 * 而且会污染词的统计）。
 *
 * ⚠️ **必须在同一次 `pinyin_engine_convert()` 之后立刻调用，期间不能再调 convert()。**
 *    引擎内部在转换时就把分词拷贝到了 s_trace 里，learn() 只读那份拷贝；但下次 convert()
 *    会覆盖它。调用方（pinyin_input 的 commit()）满足这个条件：它拿到候选后马上 learn，
 *    之后才清空拼音。
 *
 * @param[in] idx 候选下标（`pinyin_engine_convert()` 返回列表里的位置）
 */
void pinyin_engine_learn(int cand_idx);

/**
 * @brief 把一串拼音转成候选（整句转换）
 *
 * 输入只允许小写 a-z（一个都不许有别的字符）；长度超过 PE_INPUT_MAX 直接返回 0。
 * 拼不出任何东西时也返回 0（例如打成 "nihap"）—— 调用方自己降级（保留拼音回显）。
 *
 * @param[in]  py   输入拼音，NUL 结尾
 * @param[out] out  输出数组；返回 n 时只写了 out[0..n-1]
 * @param[in]  max  输出数组容量
 * @return 实际候选个数（0 表示这个输入转不出来）
 */
int pinyin_engine_convert(const char *py, pe_cand_t *out, int max);

/**
 * @brief 跑一遍自检用例并打日志（验收用）
 *
 * 工程里没有 host 端测试的条件（没有 host 编译器），所以用这个代替：
 * 由 main 在启动时调一次，日志里直接看到命中数。三段：
 *   1) 全拼用例（预期 Top-1 28/30）
 *   2) 简拼用例（预期 12/12）
 *   3) 用户学习的闭环验收（学之前简拼打不出「你好」、学之后能打出来）
 * ⚠️ 它会调 esp_log，所以这个函数不"零依赖"（但 convert 是）。
 * ⚠️ 第 3 段会往用户表里写东西，内部**自己存档/还原** —— 但还原的是"调用那一刻"的
 *    内容，所以**开机时要在 pinyin_learn_init() 之后调它**才谈得上还原用户数据。
 *
 * @return 全拼 Top-1 命中的用例数
 */
int pinyin_engine_selftest(void);

#ifdef __cplusplus
}
#endif
