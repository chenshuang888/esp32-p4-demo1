/*
 * pinyin_learn —— 实现（模型说明见头文件）
 *
 * 就四件事：读文件灌给引擎、记一笔、有改动时落盘、清空。
 * 文件格式与"为什么不先写 tmp 再 rename"都在头文件里。
 */
#include "pinyin_learn.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"

#include "pinyin_engine.h"

static const char *TAG = "pinyin_learn";

static bool s_dirty;        /* 内存表有改动、还没落盘 */
static bool s_warned;       /* "写不了卡"的告警只打一次，免得每次退出都刷屏 */

/* 行缓冲。一行最长 = PE_LEARN_TEXT_MAX 个字节的词 + 制表符 + 次数，留足余量。 */
#define LEARN_LINE_MAX  128

esp_err_t pinyin_learn_init(void)
{
    /* static 而不是栈上：pe_user_t 是 25 字节，128 条就是 3.2KB；
     * 而这个函数在 app_main 里被调用，main 任务的栈并不宽裕。 */
    static pe_user_t tab[PE_LEARN_MAX];

    FILE *f = fopen(PINYIN_LEARN_FILE, "r");
    if (f == NULL) {
        /* 首次运行、或者没插卡 —— 都是正常情况，只提示一句 */
        ESP_LOGI(TAG, "没有 %s（首次运行或没插卡）—— 本次开机学的东西不会保存",
                 PINYIN_LEARN_FILE);
        return ESP_OK;
    }

    int n = 0;
    int bad = 0;
    char line[LEARN_LINE_MAX];

    while (n < PE_LEARN_MAX && fgets(line, sizeof(line), f) != NULL) {
        char *eol = strpbrk(line, "\r\n");
        if (eol != NULL) {
            *eol = '\0';
        }
        char *tab_sep = strchr(line, '\t');
        if (tab_sep == NULL) {
            bad++;
            continue;
        }
        *tab_sep = '\0';

        const int cnt = atoi(tab_sep + 1);
        if (line[0] == '\0' || cnt <= 0) {
            bad++;
            continue;
        }

        strncpy(tab[n].text, line, PE_LEARN_TEXT_MAX - 1);
        tab[n].text[PE_LEARN_TEXT_MAX - 1] = '\0';
        tab[n].count = (uint8_t)(cnt > 255 ? 255 : cnt);
        n++;
    }
    fclose(f);

    pinyin_engine_user_load(tab, n);

    if (bad) {
        /* 写一半掉电会留下残行 —— 跳过就行，不该让整张表作废 */
        ESP_LOGW(TAG, "跳过 %d 行坏数据（不影响其余 %d 条）", bad, n);
    }
    ESP_LOGI(TAG, "载入 %d 条学习记录", n);

    s_dirty  = false;
    s_warned = false;
    return ESP_OK;
}

void pinyin_learn_record(int cand_idx)
{
    pinyin_engine_learn(cand_idx);      /* 只动内存表 */
    s_dirty = true;
}

void pinyin_learn_flush(void)
{
    if (!s_dirty) {
        return;                         /* 没改过就别碰卡 —— 别制造无谓的写 */
    }

    static pe_user_t tab[PE_LEARN_MAX];
    const int n = pinyin_engine_user_save(tab, PE_LEARN_MAX);

    FILE *f = fopen(PINYIN_LEARN_FILE, "w");
    if (f == NULL) {
        if (!s_warned) {
            ESP_LOGW(TAG, "写不了 %s（卡不在或没挂上）—— 学习记录只留在内存里",
                     PINYIN_LEARN_FILE);
            s_warned = true;
        }
        return;                         /* 刻意不清 dirty：下次退出还会再试一次 */
    }

    for (int i = 0; i < n; i++) {
        fprintf(f, "%s\t%d\n", tab[i].text, (int)tab[i].count);
    }
    fclose(f);

    s_dirty = false;
    ESP_LOGI(TAG, "学习记录已落盘（%d 条）", n);
}

void pinyin_learn_clear(void)
{
    pinyin_engine_user_clear();

    /* 文件本来就不存在也算成功 —— 目标是"清空"，不是"删掉一个文件" */
    if (remove(PINYIN_LEARN_FILE) == 0) {
        ESP_LOGI(TAG, "学习记录已清空（文件已删除）");
    } else {
        ESP_LOGI(TAG, "学习记录已清空（本来就没有文件）");
    }

    s_dirty  = false;
    s_warned = false;
}
