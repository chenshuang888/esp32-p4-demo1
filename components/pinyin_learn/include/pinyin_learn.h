/*
 * pinyin_learn —— 输入法的「用户学习」持久化（能力组件）
 *
 * 引擎（pinyin_engine）自己**不认识任何存储** —— 它只维护内存里那张
 * "词 -> 命中次数"的表。本组件负责把那张表存到 SD 卡、开机读回来、在合适的时候落盘。
 * 两边由 main 组装 —— 和 wifi_service 收凭据、net_time 挂 WiFi 回调是同一个模式。
 *
 * ============================ 落盘时机 ============================
 *
 * 用户每选一次候选都会改内存表，但**不能每次都写卡**：
 *   - 连着选词就会连着写，这种写放大对 SD 卡不友好；
 *   - 写卡是同步阻塞的，而调用方（输入法控件）跑在 LVGL 任务里。
 *
 * 所以：记的时候只置脏标记，**App 退出时（pinyin_keyboard_detach）落盘一次**。
 * 那会儿界面正在切页，阻塞几十毫秒看不出来。代价是"选完词立刻拔电"会丢最后一批。
 *
 * ============================ 没插卡是正常情况 ============================
 *
 * 读不到就空表起步，本次开机内的学习照常生效（只是不持久化），这也是**设计预期**：
 * 卡插不插不该决定输入法能不能用。（本板没有卡检测引脚，"卡还在不在"根本没法查 ——
 * 见 sd_card.h，所以文件操作失败就按不可用降级。）
 *
 * ============================ 文件格式 ============================
 *
 *     SD_CARD_MOUNT_POINT "/pinyin_learn.tsv"，一行一条：
 *
 *         词<TAB>次数
 *
 * 刻意用可读文本：方便在 PC 上直接看/改/删，调试时不用先写个解析器。
 * 读取时**坏行直接跳过** —— 写一半掉电留下的残行不该让整张表作废。
 *
 * ⚠️ 落盘是**直接覆写**这个文件，不玩"先写 .tmp 再 rename"：
 *    那个套路要先 remove 旧文件，中间有一段"新旧都不存在"的窗口，掉电反而全丢；
 *    而我们的读入端本来就容忍坏行，截断只会丢掉尾部几条，比全丢好。
 */
#pragma once

#include "esp_err.h"

#include "sd_card.h"        /* SD_CARD_MOUNT_POINT */

#ifdef __cplusplus
extern "C" {
#endif

/* 学习记录文件（SD 卡根目录）。放这里而不是某个 App 里，理由同 SD_CARD_PHOTO_DIR：
 * 它不属于任何 App，而读写的两边本来就要依赖 sd_card 拿挂载点。 */
#define PINYIN_LEARN_FILE  SD_CARD_MOUNT_POINT "/pinyin_learn.tsv"

/**
 * @brief 从 SD 卡载入学习记录并灌给引擎
 *
 * 在 main 的"能力层"里、**sd_card_init() 之后**调用一次。
 * 文件不存在（首次运行 / 没插卡）不算错误：空表起步，只打一条日志。
 *
 * @return ESP_OK（几乎总是；读不到文件不是错误）
 */
esp_err_t pinyin_learn_init(void);

/**
 * @brief 记一次用户选择（只在内存里，不落盘）
 *
 * 由输入法控件在用户点候选时调用。真正的写卡推迟到 pinyin_learn_flush()。
 *
 * @param[in] cand_idx 候选下标 —— 直接转给 pinyin_engine_learn()，
 *                     所以同样要求"紧跟在上一次 pinyin_engine_convert() 之后"。
 */
void pinyin_learn_record(int cand_idx);

/**
 * @brief 若有改动就落盘（没有改动则什么都不做）
 *
 * 由输入法控件在 App 退出（detach）时调用。写不了（没卡）只告警一次，
 * 并且**不清脏标记** —— 下次退出还会再试。
 */
void pinyin_learn_flush(void);

/**
 * @brief 清空内存表 + 删掉 SD 卡上的文件（设置里的"清除输入习惯"）
 *
 * 误选一次不至于救不回来 —— 它是这套学习机制的必要出口。
 */
void pinyin_learn_clear(void);

#ifdef __cplusplus
}
#endif
