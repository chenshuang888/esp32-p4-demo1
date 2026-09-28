/*
 * pinyin_engine —— 自检
 *
 * 工程里**没有 host 端测试的条件**（这台机器上 gcc/clang/cc/tcc 全无，工程也从来没有
 * 过 host 测试的先例），而 DP 的打分函数又是个需要反复核对的东西 —— 所以改成
 * **运行期自检**：拿固定用例在设备上跑一遍，日志里直接看到命中数。
 * 一次烧录就能验收算法，比手打 30 个词组快得多。
 *
 * 三段：
 *   1) 全拼用例   —— 和生成脚本的 TESTS 同一份。预期 Top-1 28/30、Top-3 30/30。
 *   2) 简拼用例   —— 和生成脚本的 TESTS_ABBR 同一份。预期 12/12。
 *   3) 用户学习   —— 新功能的**闭环**验收：学之前简拼打不出「你好」，学之后能打出来。
 *                    光"能编译"说明不了它有用，所以这一段是必须的。
 *
 * 为什么用例表和生成脚本里那份是"同一份"：那边是参考实现（Python），这边是被测对象
 * （C）。两边对不上，就说明移植出了问题。**改一边要连着改另一边。**
 */
#include "pinyin_engine.h"

#include <string.h>

#include "esp_log.h"

static const char *TAG = "pinyin_engine";

typedef struct {
    const char *in;     /* 输入拼音 */
    const char *want;   /* 期望的 Top-1 */
} case_t;

/*
 * 全拼用例。预期 Top-1 28/30、Top-3 30/30。两个已知不命中的：
 *   zaijian      -> 在建（再见是 Top-2）：jieba 语料里「在建」比「再见」还常用
 *   duoshaoqian  -> 多少前（多少钱是 Top-3）：这个词在 jieba 里频次只有 3，没进词表
 * 两个都是**数据覆盖**问题，不是算法问题。
 */
static const case_t S_CASES[] = {
    { "nihao",           "你好" },
    { "xiexie",          "谢谢" },
    { "zaijian",         "再见" },
    { "beijing",         "北京" },
    { "yinhang",         "银行" },
    { "xianzai",         "现在" },
    { "jintiantianqi",   "今天天气" },
    { "women",           "我们" },
    { "womenshi",        "我们是" },
    { "nizenmeyang",     "你怎么样" },
    { "woaini",          "我爱你" },
    { "qingwen",         "请问" },
    { "zhongguo",        "中国" },
    { "mingtian",        "明天" },
    { "shenme",          "什么" },
    { "zenme",           "怎么" },
    { "keyi",            "可以" },
    { "xuexi",           "学习" },
    { "pengyou",         "朋友" },
    { "shijian",         "时间" },
    { "gongzuo",         "工作" },
    { "shouji",          "手机" },
    { "diannao",         "电脑" },
    { "yinyue",          "音乐" },
    { "tushuguan",       "图书馆" },
    { "duoshaoqian",     "多少钱" },
    { "shangban",        "上班" },
    { "xiaban",          "下班" },
    { "chifan",          "吃饭" },
    { "zhongguoren",     "中国人" },
};

/*
 * 简拼（首字母缩写）用例。这些期望值是"**纯语料词频**、用户学习还没介入"时的结果。
 *
 * ⚠️ 刻意**不**放在这里的例子：「nh -> 你好」「sj -> 手机」「xx -> 谢谢」——
 *    它们在各自的缩写组里排第 9 / 第 11 / 第 26，简拼取不到。这不是 bug，
 *    而是"语料词频 != 用户的使用习惯"；它们正是**用户学习**要解决的（见下面第 3 段）。
 */
static const case_t S_ABBR[] = {
    { "bj",   "北京" }, { "zg",   "中国" }, { "sm",   "什么" },
    { "wm",   "我们" }, { "gz",   "工作" }, { "jt",   "今天" },
    { "py",   "朋友" }, { "xz",   "现在" }, { "ky",   "可以" },
    /* 「世界」和「时间」的量化代价都是 27（打平），按音串序 shijian < shijie 让「时间」在前。
     * 这是合法的平局，不是排序 bug —— 生成脚本里对这个用例有同一份说明。 */
    { "sj",   "时间" },
    /* 4 字母简拼：两个词都在词表里、缩写组里只有它一个 */
    { "bjdx", "北京大学" }, { "qhdx", "清华大学" },
};

/* 自检用的候选缓冲。放 static 不放栈上：PE_MAX_CAND 现在是 12、每个候选 60 字节，
 * 而 main 任务的栈并不宽裕。 */
static pe_cand_t s_cand[PE_MAX_CAND];

/* 用户表的暂存区：第 3 段自检会往用户表里写东西，测完必须还原 ——
 * 否则**每次开机自检都会把用户真实的输入习惯冲掉**。 */
static pe_user_t s_user_saved[PE_LEARN_MAX];

/* 跑一组用例，返回 Top-1 命中数。没进 Top-3 的打日志（命中的静默，日志才看得清）。 */
static int run_cases(const case_t *cases, int total, const char *label)
{
    int hit1 = 0;
    int hit3 = 0;

    for (int i = 0; i < total; i++) {
        const case_t *c = &cases[i];
        const int n = pinyin_engine_convert(c->in, s_cand, PE_MAX_CAND);

        int ok1 = 0, ok3 = 0;
        for (int k = 0; k < n && k < 3; k++) {
            if (strcmp(s_cand[k].text, c->want) == 0) {
                ok3 = 1;
                if (k == 0) {
                    ok1 = 1;
                }
                break;
            }
        }
        hit1 += ok1;
        hit3 += ok3;

        if (!ok3) {
            char got[PE_MAX_CAND_LEN * 3 + 8] = { 0 };
            for (int k = 0; k < n && k < 3; k++) {
                if (k > 0) {
                    strcat(got, "/");
                }
                strcat(got, s_cand[k].text);
            }
            ESP_LOGW(TAG, "  ✗ [%s] %-16s 期望 %s，实际 %s",
                     label, c->in, c->want, (n > 0) ? got : "(无候选)");
        }
    }

    ESP_LOGI(TAG, "自检[%s]：Top-1 %d/%d，Top-3 %d/%d", label, hit1, total, hit3, total);
    return hit1;
}

/*
 * 用户学习验收 —— 新功能的闭环验证。
 *
 * 场景（就是真机上的用法）：
 *   用户一直用全拼打「你好」（nihao -> 你好 本来就是 Top-1），选中几次之后，
 *   学习把「你好」这个词记下来。此后他打简拼 nh —— 原本「你好」在组里排第 9、
 *   根本不在候选栏里，现在应该爬进 Top-3。
 *
 * 为什么必须"先打全拼再打简拼"：简拼 nh 的候选池里**没有**「你好」，
 * 用户在那里**根本选不到它** —— 只有全拼这条路能"教"会系统。这就是词级学习的意义：
 * 学习必须作用在**词**上，才能反过来改善简拼的排序。
 */
static int selftest_learn(void)
{
    /* 先把用户真实的表存下来，测完还原（见 s_user_saved 的说明） */
    const int saved_n = pinyin_engine_user_save(s_user_saved, PE_LEARN_MAX);
    pinyin_engine_user_clear();

    /* 学之前：简拼 nh 里应当**没有**「你好」。（如果有，这个验收就失去意义了 ——
     * 那说明 PE_ABBREV_KEEP 已经够大，学习的效果看不出来。） */
    int n = pinyin_engine_convert("nh", s_cand, PE_MAX_CAND);
    int before = 0;
    for (int k = 0; k < n; k++) {
        if (strcmp(s_cand[k].text, "你好") == 0) {
            before = k + 1;
            break;
        }
    }

    /* 模拟用户打全拼并选中「你好」。次数取 8 次是为了吃到加成上限
     * （PE_LEARN_BONUS_CAP = 8、每步 2 单位，8 次之后 bonus 就封顶在 16 了）。 */
    int learned = 0;
    for (int round = 0; round < 8; round++) {
        n = pinyin_engine_convert("nihao", s_cand, PE_MAX_CAND);
        if (n <= 0 || strcmp(s_cand[0].text, "你好") != 0) {
            break;      /* 全拼都不出「你好」的话，问题在别处，不在这里 */
        }
        pinyin_engine_learn(0);
        learned++;
    }

    /* 学之后：简拼 nh 的 Top-3 里应当有「你好」 */
    n = pinyin_engine_convert("nh", s_cand, PE_MAX_CAND);
    int after = 0;
    for (int k = 0; k < 3 && k < n; k++) {
        if (strcmp(s_cand[k].text, "你好") == 0) {
            after = k + 1;
            break;
        }
    }

    ESP_LOGI(TAG, "自检[用户学习]：全拼选中「你好」%d 次；简拼 nh 里「你好」"
                  "学之前%s、学之后%s",
             learned,
             before ? "已在候选" : "不在候选",
             after ? "进 Top-3" : "仍不在 Top-3");

    /* 还原用户真实的表 */
    pinyin_engine_user_clear();
    pinyin_engine_user_load(s_user_saved, saved_n);

    if (learned == 0) {
        ESP_LOGE(TAG, "全拼 nihao 都没给出「你好」—— 先查全拼那条链路");
        return 0;
    }
    if (after == 0) {
        ESP_LOGE(TAG, "用户学习没生效：学过之后「你好」仍进不了简拼 nh 的 Top-3。"
                      "查三处：user_hash_build() / user_bonus() /"
                      "缩写边那第二遍『被学过的词无条件建边』");
        return 0;
    }
    return 1;
}

int pinyin_engine_selftest(void)
{
    const int hit1 = run_cases(S_CASES, (int)(sizeof(S_CASES) / sizeof(S_CASES[0])), "全拼");
    run_cases(S_ABBR, (int)(sizeof(S_ABBR) / sizeof(S_ABBR[0])), "简拼");

    /* 顺带验两个边界：拼不出来的输入要返回 0 而不是崩 */
    const int bad1 = pinyin_engine_convert("nihap", s_cand, PE_MAX_CAND);
    const int bad2 = pinyin_engine_convert("", s_cand, PE_MAX_CAND);
    const int bad3 = pinyin_engine_convert("NiHao", s_cand, PE_MAX_CAND);

    ESP_LOGI(TAG, "自检[边界]：无解=%d 空=%d 大写=%d（都应为 0）", bad1, bad2, bad3);
    if (bad1 != 0 || bad2 != 0 || bad3 != 0) {
        ESP_LOGE(TAG, "边界用例没返回 0 —— 输入校验有问题");
    }

    selftest_learn();

    return hit1;
}
