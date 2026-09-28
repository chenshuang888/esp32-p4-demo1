/*
 * pinyin_engine —— 实现
 *
 * ============================ 算法：字母串上的最短路径 ============================
 *
 * 把输入串的位置 0..n 当节点，两两之间连边：
 *
 *     词边   子串 [i,j) 命中词表（如 "jintian" -> 今天）
 *     字边   子串 [i,j) 是一个合法音节（如 "hao" -> 好/号/毫…，按字频取前几个）
 *     缩写边 子串 [i,j) 命中缩写表（如 "bjdx" -> 北京大学）—— 也就是"简拼"
 *
 * 每条边的代价 = 该词/字的量化代价 + PE_GAMMA_U（每多一条边就多付一次固定惩罚）。
 * 从 0 走到 n 的最小代价路径 = 可能性最高的转换结果。每个节点保留最优的
 * PE_PATH_KEEP 条路径（beam search），走到终点后按代价升序给出候选。
 *
 * ⚠️ 为什么"词边"和"字边"的代价能放在一起比：它们在生成期就用**同一个公式**
 *    量化过了（log(总数) - log(频次)）。而"每多一条边多付 GAMMA"这一项让
 *    "一个词"天然优于"两个单字"—— 否则 (log 大 + log 大) 会比 (log 中) 还小，
 *    结果就是单词永远输给逐字，整个词组能力失效。
 *
 * 为什么不需要做分词：**音串精确匹配本身就绕开了切分歧义**。用户打 "xianzai"，
 * 词表里就有 "xianzai" 这个 key，不用先判断是 xian+zai 还是 xi+an+zai。
 * 多音字同理：「银行」在 yinhang 和 yinxing 两个 key 下各有一条记录。
 *
 * **顺带一个重要结果**：加上缩写边之后，"全拼 / 简拼 / 两者混着打"都不需要调用方
 * 做任何区分 —— 三种边在同一个图里，DP 自己找最优的切分方式。
 *
 * ============================ 缩写边（简拼）的两个细节 ============================
 *
 * 1) 键是**打包成 uint32** 的（4 槽 × 5bit，a-z -> 1..26，不足补 0，高位在前），
 *    所以能直接对整数二分，省掉 strcmp、也省掉字符串池。见 abbr_pack()。
 *    打包方式必须和生成脚本的 pack_abbr() 逐位一致，否则简拼全部失灵（且不报错）。
 *
 * 2) 每组还要付一个 **penalty**（生成期算好，= round(log(组内词数) * COST_SCALE)）。
 *    缩写比全拼歧义得多（实测最大的组有 248 个词），不分档的话打 "da" 会先出
 *    「答案」而不是「大」。按组大小罚之后，组小的 "bjdx" 几乎不罚、组大的 "zz" 罚得多。
 *
 * ============================ 用户学习（词级）与回溯指针 ============================
 *
 * 用户选了一个候选之后，要把**路径上的每个词**计数 +1，下次这些词的代价就被压低。
 * 所以需要知道"这条路径由哪些词组成"—— 但候选结构里只有文本（`pe_cand_t` 只有
 * text 和 cost）。于是给每条路径加一个**回溯指针**：
 *
 *     s_bp[pos][slot] = { 上一条边是哪个词, 上一个位置, 上一个 slot }
 *
 * ⚠️ 它必须和 s_paths **在同一个搬迁循环里同步搬运** —— path_push 会重排和淘汰，
 *    漏一处就会静默指到别人的路径上。
 *
 * **为什么这样是安全的**（这条论证不成立的话整个学习就是错的）：
 *    外层是 `for i`，内层只写 `s_paths[j] (j > i)`。所以开始处理位置 i 的时候，
 *    `s_paths[i]` 已经冻结、slot 编号不会再变 —— 指向它的回溯指针恒有效。
 *    （位置 0 的路径没有上一条边，回溯到 pos == 0 就停。）
 *
 * ⚠️ **一条边都学不到的情况**：同一段文本常有多条路径（词边「你好」vs 字边「你+好」），
 *    末尾去重时如果留下的是字边那条，回溯出来全是字边、一个词都记不上 ——
 *    表现为"学习时灵时不灵"。所以去重时**同一条文本要挑"词边最多"的那条路径**
 *    来生成 trace（代价仍取最小的那条）。见 convert() 末尾。
 *
 * ⚠️ **只减代价救不了被截断的词**：词边/缩写边以前是"取组里前 N 条"，一个组里的
 *    第 40 个词即使代价被压到 0，也根本进不了候选池（缩写组最大 248 个词，而
 *    只取前 16）。所以边收集改成"扫完整组"，且 **count > 0 的词无条件建边**
 *    （组内总边数有硬上限 PE_GROUP_HARD_MAX）。这条不做，简拼上的学习就是白搭。
 *
 * ============================ 为什么状态放 static ============================
 *
 * 路径表约 29*16*60 ≈ 28 KB，回溯指针再加 1.9 KB。**不能放栈上** —— 调用方是
 * LVGL 任务，它的栈只有 7168 字节（见 components/ui/ui.c 与 README 的坑表），
 * 放上去直接爆栈。代价是这个函数**不可重入**。目前只有输入法控件在 LVGL 任务里调它。
 *
 * ============================ 两个容易写错的地方 ============================
 *
 * 1) 边跨度的上限是 **PE_MAX_KEY_LEN（最长音串 20 字母）**，不是 PE_MAX_SYL_LEN（最长音节 6）。
 *    第一版就是按 6 写的，结果**所有双字以上的词全部失效**（"beijing" 有 7 个字母，
 *    连边都不会生成），Top-1 从 28/30 掉到 14/30。字边才用 PE_MAX_SYL_LEN 限制。
 *
 * 2) 末尾**必须去重**：不同路径经常产出同一个文本（词边「你好」和字边「你+好」都得到
 *    "你好"），不去重候选栏里会出现两个一模一样的候选。
 *    （去重会消耗候选数，所以每节点保留的路径数要比候选数多几个。）
 */
#include <string.h>

#include "pinyin_engine.h"
#include "pe_dict.h"        /* 生成物：数组声明 + 维度常量（见 scripts/gen_engine_dict.py） */

/* ===================== 调参旋钮 =====================
 * GAMMA 由生成脚本里的 GAMMA 换算而来，两边必须一致（脚本里是 GAMMA * COST_SCALE = 4*4）。
 * 好消息：实测 GAMMA 从 2 到 12，Top-1 命中都是 28/30 —— 结果对这个常数不敏感。 */
#define PE_GAMMA_U     16   /* 每条边的固定惩罚（量化单位：0.25 自然对数） */

/* ⚠️ PE_PATH_KEEP / PE_EDGE_KEEP / PE_ABBREV_KEEP 在生成脚本里**各有一份同名的**，
 *    改这里要连着改那边（否则 Python 参考实现和 C 侧对不上，那个自检就失去意义了）。 */
#define PE_PATH_KEEP    16  /* 每个位置保留几条路径。**要比候选数大**：去重会消耗掉一些 */
#define PE_EDGE_KEEP     4  /* 词边/字边每个组取前几个作为边（保持不变，别扰动全拼基线） */
#define PE_ABBREV_KEEP  16  /* 缩写边每个键取前几个词。单独一档：缩写组可以很大（最大 248） */
/* 一个组最多建多少条边。第二遍捞"被学过的词"时用它封顶，免得 248 个词的组全建边。 */
#define PE_GROUP_HARD_MAX 32

/* 学习加成：命中次数越多减得越多，封顶。单位同 cost（0.25 自然对数）。 */
#define PE_LEARN_BONUS_STEP  2
#define PE_LEARN_BONUS_CAP   8      /* bonus 上限 = 8 * 2 = 16 单位 ≈ 一次的 GAMMA 量级 */

/* 回溯出来的分词最多存几个词（一个候选最长 18 个汉字，全是双字词也就 9 个） */
#define PE_TRACE_MAX    16

/* 用户表的哈希槽数（必须是 2 的幂，方便用掩码取模）。
 * PE_LEARN_MAX = 128，所以装载因子 ≤ 0.5 —— 线性探测不会挤成一团。 */
#define PE_USER_HASH_SIZE 256

/* 回溯指针里 word 取这个值表示"这条边是字边"，学习时跳过。
 * （单字学习价值低，而且会污染词的统计；词表里也没有单字。） */
#define PE_BP_CHAR  0xFFFFu

/* 缩写键打包成几个 5 bit 槽。**必须和生成脚本的 ABBR_SLOTS 一致**（= 词表最大词长 4）。
 * ⚠️ 它**不是** PE_MAX_ABBR_LEN —— 后者是"数据里实际出现的最长键"，可能小于 4。
 *    拿它当槽数会让 C 和 Python 的打包结果错位：简拼全部失灵，而且不报任何错。 */
#define PE_ABBR_SLOTS   4
_Static_assert(PE_MAX_ABBR_LEN <= PE_ABBR_SLOTS,
               "缩写键比打包槽数还长 —— 要同时调大 PE_ABBR_SLOTS 和脚本的 ABBR_SLOTS");

/* 一个"到达位置 j 的转换方案" */
typedef struct {
    int16_t cost;
    uint8_t len;                        /* text 的字节数（不含 NUL） */
    char    text[PE_MAX_CAND_LEN];
} pe_path_t;

/* 一条路径的"上一条边"：供 learn() 回溯出分词 */
typedef struct {
    uint16_t word;                      /* 词表下标；PE_BP_CHAR 表示字边 */
    uint8_t  from_pos;                  /* 上一条边从哪个位置来（严格小于本位置） */
    uint8_t  from_slot;
} pe_bp_t;

/* 路径表：位置 0..n，每个位置 PE_PATH_KEEP 条，按 cost 升序。
 * static 是必须的（栈放不下），代价是不可重入 —— 见文件头。 */
static pe_path_t s_paths[PE_INPUT_MAX + 1][PE_PATH_KEEP];
static pe_bp_t   s_bp[PE_INPUT_MAX + 1][PE_PATH_KEEP];
static uint8_t   s_npaths[PE_INPUT_MAX + 1];

/* 每次 convert() 结束时，把每个输出候选的**分词**物化到这里，供 learn() 使用。
 * 刻意做成拷贝而不是"让 learn 去读 DP 现场"：后者依赖"DP 状态还活着"这个
 * 没有注释保护的隐式约定，改一下 refresh_bar 就会静默失效。 */
static uint16_t  s_trace[PE_MAX_CAND][PE_TRACE_MAX];
static int       s_trace_n[PE_MAX_CAND];

/* 用户表（词 -> 命中次数）。引擎**不认识存储**，内容由 pinyin_learn 喂入/取走。 */
static pe_user_t s_user[PE_LEARN_MAX];
static int       s_user_n;
static int16_t   s_uhash[PE_USER_HASH_SIZE];    /* 槽 -> s_user 下标；-1 为空 */

/* ===================== 小工具 ===================== */

/* UTF-8 首字节 -> 该字符占几个字节。非法首字节按 1 字节处理，免得原地死循环。 */
static int utf8_len(unsigned char b)
{
    if (b < 0x80)              return 1;
    if ((b & 0xE0) == 0xC0)    return 2;
    if ((b & 0xF0) == 0xE0)    return 3;
    if ((b & 0xF8) == 0xF0)    return 4;
    return 1;
}

/*
 * 往位置 j 插一条路径（保持该位置按 cost 升序、最多 PE_PATH_KEEP 条）。
 * 满了且新路径不比最差的更好，就直接丢 —— 标准的 beam pruning。
 *
 * ⚠️ 代价相等时**新的排在后面**（下面的 while 用严格 `>`）：这样"没学过的词"
 *    那条路径的取舍和以前完全一致，不会因为加了学习就抖动。
 *
 * ⚠️ 回溯指针 s_bp 必须和 s_paths **在同一个搬迁循环里**一起挪。漏一处不会崩，
 *    只是 learn() 会回溯到别人的分词上，表现为"学习记错词"。
 */
static void path_push(int j, int cost, const char *text, int len,
                      uint16_t bp_word, int bp_pos, int bp_slot)
{
    if (s_npaths[j] == PE_PATH_KEEP && cost >= s_paths[j][PE_PATH_KEEP - 1].cost) {
        return;
    }

    int pos;
    if (s_npaths[j] < PE_PATH_KEEP) {
        pos = s_npaths[j]++;
    } else {
        pos = PE_PATH_KEEP - 1;         /* 队列已满：顶掉最后一名 */
    }
    /* 向前挪，给新路径腾位置 */
    while (pos > 0 && s_paths[j][pos - 1].cost > cost) {
        s_paths[j][pos] = s_paths[j][pos - 1];
        s_bp[j][pos]    = s_bp[j][pos - 1];     /* ← 必须同步 */
        pos--;
    }

    s_paths[j][pos].cost = (int16_t)cost;
    s_paths[j][pos].len  = (uint8_t)len;
    memcpy(s_paths[j][pos].text, text, (size_t)len);
    s_paths[j][pos].text[len] = '\0';
    s_bp[j][pos].word      = bp_word;
    s_bp[j][pos].from_pos  = (uint8_t)bp_pos;
    s_bp[j][pos].from_slot = (uint8_t)bp_slot;
}

/*
 * 把 etxt 接到"位置 i 的每一条路径"后面，形成终点为 j 的候选路径。
 * wid 是这条边对应的词表下标（字边传 PE_BP_CHAR），只是给回溯用。
 */
static void extend(int i, int j, int ecost, const char *etxt, uint16_t wid)
{
    const int elen = (int)strlen(etxt);
    const int n    = s_npaths[i];

    for (int p = 0; p < n; p++) {
        const pe_path_t *src = &s_paths[i][p];
        const int tlen = src->len + elen;
        if (tlen >= PE_MAX_CAND_LEN) {
            continue;       /* 太长：丢弃这条路径（见头文件对 PE_MAX_CAND_LEN 的说明） */
        }
        char buf[PE_MAX_CAND_LEN];
        memcpy(buf, src->text, (size_t)src->len);
        memcpy(buf + src->len, etxt, (size_t)elen);
        path_push(j, src->cost + ecost, buf, tlen, wid, i, p);
    }
}

/*
 * 从 (pos, slot) 沿回溯指针往回走，数出这条路径由几个词组成；
 * words 非 NULL 时顺便把词下标写进去（最多 cap 个）。
 *
 * 终止条件：pos == 0（每条边都严格 i < j，所以 pos 严格递减，不会绕圈）；
 * 位置 0 的路径没有"上一条边"，循环不读它。
 */
static int trace_back(int pos, int slot, uint16_t *words, int cap)
{
    int nw = 0;
    while (pos > 0) {
        const pe_bp_t *b = &s_bp[pos][slot];
        if (b->word != PE_BP_CHAR) {
            if (words != NULL && nw < cap) {
                words[nw] = b->word;
            }
            nw++;
        }
        const int np = b->from_pos;
        const int ns = b->from_slot;
        pos  = np;
        slot = ns;
    }
    return nw;
}

/* ===================== 用户表（学习） ===================== */

/* FNV-1a。词很短，不用在乎哈希质量，只要稳定。 */
static uint32_t user_hash(const char *s)
{
    uint32_t h = 2166136261u;
    for (const unsigned char *p = (const unsigned char *)s; *p != '\0'; p++) {
        h ^= (uint32_t)*p;
        h *= 16777619u;
    }
    return h;
}

/*
 * 在 convert() 开头整体重建一次索引。
 *
 * 刻意**不做增量维护**：每次重建 ≤128 次插入，代价可以忽略，而且永远不会出现
 * "表改了、索引没跟上"那类不一致。learn() 之后不需要任何处理 —— 下次 convert()
 * 重建时自然就带上了。
 */
static void user_hash_build(void)
{
    for (int i = 0; i < PE_USER_HASH_SIZE; i++) {
        s_uhash[i] = -1;
    }
    for (int i = 0; i < s_user_n; i++) {
        uint32_t h = user_hash(s_user[i].text) & (PE_USER_HASH_SIZE - 1);
        while (s_uhash[h] >= 0) {
            h = (h + 1) & (PE_USER_HASH_SIZE - 1);
        }
        s_uhash[h] = (int16_t)i;
    }
}

/* 这个词被学过几次 -> 该减多少代价。没学过返回 0。 */
static int user_bonus(const char *word)
{
    if (s_user_n == 0) {
        return 0;                       /* fast path：绝大多数 convert 走这条 */
    }
    uint32_t h = user_hash(word) & (PE_USER_HASH_SIZE - 1);
    for (int probe = 0; probe < PE_USER_HASH_SIZE; probe++) {
        const int16_t idx = s_uhash[h];
        if (idx < 0) {
            return 0;                   /* 空槽 = 线性探测链到头了，没这个词 */
        }
        if (strcmp(s_user[idx].text, word) == 0) {
            int c = s_user[idx].count;
            if (c > PE_LEARN_BONUS_CAP) {
                c = PE_LEARN_BONUS_CAP;
            }
            return c * PE_LEARN_BONUS_STEP;
        }
        h = (h + 1) & (PE_USER_HASH_SIZE - 1);
    }
    return 0;
}

/*
 * 一条词边的**调整后代价**。
 *
 * ⚠️ 顺序很重要：**先减到 0 为止、再在调用处加 GAMMA**。反过来的话（先加 GAMMA
 *    再减）GAMMA 会把负值抬回来，学得越多代价反而越大。
 */
static int adj_word_cost(uint8_t cost, const char *word)
{
    const int c = (int)cost - user_bonus(word);
    return c > 0 ? c : 0;
}

/* 记一次：找到就 +1（饱和），没有就插入；表满时顶掉命中次数最少的那条。 */
static void user_bump(const char *word)
{
    for (int i = 0; i < s_user_n; i++) {
        if (strcmp(s_user[i].text, word) == 0) {
            if (s_user[i].count < 255) {
                s_user[i].count++;
            }
            return;
        }
    }

    int slot;
    if (s_user_n < PE_LEARN_MAX) {
        slot = s_user_n++;
    } else {
        slot = 0;                       /* 满了：淘汰命中次数最少的 */
        for (int i = 1; i < s_user_n; i++) {
            if (s_user[i].count < s_user[slot].count) {
                slot = i;
            }
        }
    }
    strncpy(s_user[slot].text, word, PE_LEARN_TEXT_MAX - 1);
    s_user[slot].text[PE_LEARN_TEXT_MAX - 1] = '\0';
    s_user[slot].count = 1;
}

void pinyin_engine_user_load(const pe_user_t *tab, int n)
{
    s_user_n = 0;
    if (tab == NULL) {
        return;
    }
    for (int i = 0; i < n && s_user_n < PE_LEARN_MAX; i++) {
        if (tab[i].text[0] == '\0') {
            continue;                   /* 空条目：跳过（存储层可能写坏过一行） */
        }
        s_user[s_user_n] = tab[i];
        s_user[s_user_n].text[PE_LEARN_TEXT_MAX - 1] = '\0';
        if (s_user[s_user_n].count == 0) {
            s_user[s_user_n].count = 1; /* 计数为 0 的条目没有意义，兜一下底 */
        }
        s_user_n++;
    }
}

int pinyin_engine_user_save(pe_user_t *out, int max)
{
    if (out == NULL || max <= 0) {
        return 0;
    }
    int n = 0;
    for (int i = 0; i < s_user_n && n < max; i++) {
        out[n++] = s_user[i];
    }
    return n;
}

void pinyin_engine_user_clear(void)
{
    s_user_n = 0;
}

void pinyin_engine_learn(int cand_idx)
{
    if (cand_idx < 0 || cand_idx >= PE_MAX_CAND) {
        return;
    }
    const int nw = s_trace_n[cand_idx];
    for (int k = 0; k < nw && k < PE_TRACE_MAX; k++) {
        user_bump(pe_dict_words[s_trace[cand_idx][k]].word);
    }
}

/* ===================== 查表（二分） =====================
 *
 * 词表和单字表都是**按 key 的 ASCII 升序**排的（生成脚本里有断言守着），所以能二分。
 *
 * 比较用 strncmp(表中的键, 输入, len)：
 *   - 前 len 个字节不等    -> 按 strncmp 的结果决定往哪边收
 *   - 前 len 个字节相等    -> 还要看表里那条是否"恰好到此为止"（表键[len] == '\0'）；
 *                            不是的话说明表里的键更长，精确匹配只可能在左边
 *
 * 两张表的记录类型不同（pe_word_t / pe_syllable_t），所以下面的循环抄了两份 ——
 * 比为了复用去抽一个带函数指针的通用版本更好读。
 *
 * ⚠️ 三者有个**关键差别**：词表里同一个音串有**多条**记录（多个同音词），缩写表里同一个
 *    缩写键也有多条（同一个"简拼"对应多个词），而单字表里每个音节**只有一条**
 *    （生成脚本保证唯一）。所以只有前两者需要"退到组头"，见 convert() 里那段长注释。
 */

/* 单字表版本 */
static int find_syllable(const char *py, int len)
{
    int lo = 0, hi = PE_SYLLABLE_COUNT - 1;
    while (lo <= hi) {
        const int mid = lo + (hi - lo) / 2;
        int c = strncmp(pe_dict_syllables[mid].syl, py, (size_t)len);
        if (c == 0) {
            if (pe_dict_syllables[mid].syl[len] == '\0') {
                return mid;
            }
            c = 1;
        }
        if (c < 0) {
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return -1;
}

/* 词表版本 */
static int find_word_key(const char *py, int len)
{
    int lo = 0, hi = PE_WORD_COUNT - 1;
    while (lo <= hi) {
        const int mid = lo + (hi - lo) / 2;
        int c = strncmp(pe_dict_words[mid].py, py, (size_t)len);
        if (c == 0) {
            if (pe_dict_words[mid].py[len] == '\0') {
                return mid;
            }
            c = 1;
        }
        if (c < 0) {
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return -1;
}

/*
 * 把缩写串打包成 uint32：4 个 5 bit 槽，a-z -> 1..26，不足 4 位补 0，高位在前。
 *
 * 这样的大小关系与字符串字典序一致（都是从最高位那个槽开始比），所以缩写表能直接
 * 按整数二分。0 留给"这个槽没有字符"。
 *
 * ⚠️ 必须和生成脚本的 pack_abbr() 逐位一致 —— 不一致不会报错，只是简拼全部失灵。
 *    （selftest 里有简拼用例守着这条。）
 */
static uint32_t abbr_pack(const char *p, int len)
{
    uint32_t v = 0;
    for (int i = 0; i < PE_ABBR_SLOTS; i++) {
        v <<= 5;
        if (i < len) {
            v |= (uint32_t)(p[i] - 'a' + 1);
        }
    }
    return v;
}

/* 缩写表版本（键已经是整数，直接比大小） */
static int find_abbrev(uint32_t key)
{
    int lo = 0, hi = PE_ABBREV_COUNT - 1;
    while (lo <= hi) {
        const int mid = lo + (hi - lo) / 2;
        const uint32_t k = pe_dict_abbrev[mid].key;
        if (k == key) {
            return mid;
        }
        if (k < key) {
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return -1;
}

/* ===================== 对外接口 ===================== */

int pinyin_engine_convert(const char *py, pe_cand_t *out, int max)
{
    if (py == NULL || out == NULL || max <= 0) {
        return 0;
    }

    const int n = (int)strlen(py);
    if (n == 0 || n > PE_INPUT_MAX) {
        return 0;
    }
    /* 只接受小写字母。调用方（输入法控件）保证这一点，这里防一手 */
    for (int i = 0; i < n; i++) {
        if (py[i] < 'a' || py[i] > 'z') {
            return 0;
        }
    }

    /* 用户表索引：整体重建（理由见 user_hash_build）。没学过任何词就整段跳过 ——
     * 绝大多数调用走这条 fast path。 */
    if (s_user_n > 0) {
        user_hash_build();
    }
    /* 分词拷贝也要清：万一这次没产出候选（或候选变少），learn() 不会读到上一次的残留 */
    for (int i = 0; i < PE_MAX_CAND; i++) {
        s_trace_n[i] = 0;
    }

    for (int i = 0; i <= n; i++) {
        s_npaths[i] = 0;
    }
    path_push(0, 0, "", 0, PE_BP_CHAR, 0, 0);

    for (int i = 0; i < n; i++) {
        if (s_npaths[i] == 0) {
            continue;               /* 这个位置不可达（前缀就拼不出来） */
        }

        /* ⚠️ 上限用 PE_MAX_KEY_LEN（最长音串），不是 PE_MAX_SYL_LEN。
         *    写错这个，所有多字词都不会被考虑 —— 见文件头"两个容易写错的地方"。 */
        int jmax = i + PE_MAX_KEY_LEN;
        if (jmax > n) {
            jmax = n;
        }

        for (int j = i + 1; j <= jmax; j++) {
            const int len = j - i;

            /* ---- 词边 ----
             *
             * ⚠️ 二分只保证"找到了这个音串的某一条记录"，**不保证是第一条**。
             *
             * 同一个音串有多个候选词（beijing -> 北京/背景/北经…），它们按词频降序
             * 连续存放。若二分落在中间某一条上，后面取 EDGE_KEEP 条边就会从中间开始数，
             * 把最高频的几个词整个跳过去 —— 症状是**整句候选里最常见的那个词反而不出现**
             * （实测 beijing 出「背景」不出「北京」、keyi 出「可疑」不出「可以」）。
             *
             * 所以必须先退到这一组的开头。组内最多十几条，这个循环代价可忽略。
             * （生成脚本里有断言保证音串升序、同音串连续，所以不会退过头。）
             */
            const int k = find_word_key(py + i, len);
            if (k >= 0) {
                int g = k;
                while (g > 0 && strcmp(pe_dict_words[g - 1].py, pe_dict_words[g].py) == 0) {
                    g--;
                }
                const char *key = pe_dict_words[g].py;
                int gend = g;
                while (gend < PE_WORD_COUNT && strcmp(pe_dict_words[gend].py, key) == 0) {
                    gend++;
                }
                const int gcount = gend - g;

                /* 组本身按代价升序，所以"前 PE_EDGE_KEEP 条"就是"代价最小的几条"
                 * —— 没学过任何词时，这与旧行为**逐字一致**（不会扰动全拼基线）。
                 * 剩下的一遍只捞**被学过的**词：见文件头"只减代价救不了被截断的词"。 */
                uint16_t ewid[PE_GROUP_HARD_MAX];
                int32_t  ecost[PE_GROUP_HARD_MAX];
                int      nedge = 0;

                for (int e = 0; e < gcount && nedge < PE_EDGE_KEEP; e++) {
                    const pe_word_t *w = &pe_dict_words[g + e];
                    ewid[nedge]  = (uint16_t)(g + e);
                    ecost[nedge] = adj_word_cost(w->cost, w->word);
                    nedge++;
                }
                for (int e = PE_EDGE_KEEP; e < gcount && nedge < PE_GROUP_HARD_MAX; e++) {
                    const pe_word_t *w = &pe_dict_words[g + e];
                    const int b = user_bonus(w->word);
                    if (b > 0) {
                        const int c = (int)w->cost - b;
                        ewid[nedge]  = (uint16_t)(g + e);
                        ecost[nedge] = c > 0 ? c : 0;
                        nedge++;
                    }
                }
                for (int e = 0; e < nedge; e++) {
                    extend(i, j, ecost[e] + PE_GAMMA_U,
                           pe_dict_words[ewid[e]].word, ewid[e]);
                }
            }

            /* ---- 字边（只有长度不超过最长音节才可能是音节） ----
             * 这里刻意**保持"取存储顺序的前 N 个"**，不按代价重排：那个顺序是生成期
             * 定好的"主读音优先、再按字频降序"，本身是有意义的；而且单字不参与学习
             * （字边的 bp 是 PE_BP_CHAR），没有重排的理由。动了它会扰动全拼基线。 */
            if (len <= PE_MAX_SYL_LEN) {
                const int s = find_syllable(py + i, len);
                if (s >= 0) {
                    const pe_syllable_t *t = &pe_dict_syllables[s];
                    int cnt = t->count;
                    if (cnt > PE_EDGE_KEEP) {
                        cnt = PE_EDGE_KEEP;
                    }
                    const char *p = t->chars;
                    for (int e = 0; e < cnt; e++) {
                        const int clen = utf8_len((unsigned char)*p);
                        /* chars 是拼接串，不是 NUL 分隔的，所以要自己截出一个字 */
                        char cbuf[8];
                        memcpy(cbuf, p, (size_t)clen);
                        cbuf[clen] = '\0';
                        extend(i, j, t->costs[e] + PE_GAMMA_U, cbuf, PE_BP_CHAR);
                        p += clen;
                    }
                }
            }

            /* ---- 缩写边（简拼） ----
             * 键长只在 [PE_MIN_ABBR_LEN, PE_MAX_ABBR_LEN] 之间才可能是缩写 ——
             * 词表只收 2~4 字词，所以缩写键恒为 2~4 字母，短输入不会误触发。 */
            if (len >= PE_MIN_ABBR_LEN && len <= PE_MAX_ABBR_LEN) {
                const int a = find_abbrev(abbr_pack(py + i, len));
                if (a >= 0) {
                    const pe_abbr_t *t = &pe_dict_abbrev[a];
                    uint16_t ewid[PE_GROUP_HARD_MAX];
                    int32_t  ecost[PE_GROUP_HARD_MAX];
                    int      nedge = 0;

                    for (int e = 0; e < t->count && nedge < PE_ABBREV_KEEP; e++) {
                        const uint16_t w = pe_dict_abbrev_entries[t->first + e];
                        const pe_word_t *pw = &pe_dict_words[w];
                        ewid[nedge]  = w;
                        ecost[nedge] = adj_word_cost(pw->cost, pw->word);
                        nedge++;
                    }
                    /* 同词边那条：被学过的词**无条件**建边。缩写组最大 248 个词，
                     * 只取前 16 的话，第 17 名以后的词把代价压到 0 也出不来。 */
                    for (int e = PE_ABBREV_KEEP; e < t->count && nedge < PE_GROUP_HARD_MAX; e++) {
                        const uint16_t w = pe_dict_abbrev_entries[t->first + e];
                        const pe_word_t *pw = &pe_dict_words[w];
                        const int b = user_bonus(pw->word);
                        if (b > 0) {
                            const int c = (int)pw->cost - b;
                            ewid[nedge]  = w;
                            ecost[nedge] = c > 0 ? c : 0;
                            nedge++;
                        }
                    }
                    for (int e = 0; e < nedge; e++) {
                        extend(i, j, ecost[e] + (int)t->penalty + PE_GAMMA_U,
                               pe_dict_words[ewid[e]].word, ewid[e]);
                    }
                }
            }
        }
    }

    /*
     * 末端去重 + 给每条输出挑一条**适合学习**的路径。
     *
     * s_paths[n] 已按 cost 升序，但同一段文本常有多条路径（词边「你好」和字边
     * 「你+好」都得到 "你好"）。这里做两件事：
     *   1) 同一条文本只输出一次（否则候选栏里会出现两个一模一样的候选）；
     *   2) trace 取**词边最多**的那条 —— 取到字边那条的话回溯不出任何词，
     *      表现为"学习时灵时不灵"。代价仍用代价最小的那条（第一个命中的）。
     */
    int cnt = 0;
    for (int p = 0; p < s_npaths[n] && cnt < max; p++) {
        const char *txt = s_paths[n][p].text;
        int dup = -1;
        for (int q = 0; q < cnt; q++) {
            if (strcmp(out[q].text, txt) == 0) {
                dup = q;
                break;
            }
        }

        if (dup < 0) {
            memcpy(out[cnt].text, txt, strlen(txt) + 1);
            out[cnt].cost = s_paths[n][p].cost;
            s_trace_n[cnt] = trace_back(n, p, s_trace[cnt], PE_TRACE_MAX);
            cnt++;
        } else {
            uint16_t tmp[PE_TRACE_MAX];
            const int nw = trace_back(n, p, tmp, PE_TRACE_MAX);
            if (nw > s_trace_n[dup]) {
                memcpy(s_trace[dup], tmp, sizeof(tmp));
                s_trace_n[dup] = nw;
            }
        }
    }
    return cnt;
}
