/*
 * sd_card_save —— 拍照保存
 *
 * 把"按快门"和"写 SD 卡"这两件事解耦：
 *   快门回调只 give 一个信号量就返回，真正的 mkdir / 扫目录 / 写文件 / fsync
 *   全在一个独立任务里做。
 *
 * 为什么不让快门直接写：一次保存 = 扫目录 + fopen + fwrite(20~80KB) + fsync + fclose，
 * 在这块板上是几十毫秒量级。快门回调跑在 LVGL 任务里，直接写会把整个界面卡住
 * （预览定时器、触摸响应全部顺延）。所以拆成两半：
 *   LVGL 任务：sd_card_save_trigger()      非阻塞，只发信号
 *   保存任务： 取最新一帧 JPEG -> 落盘 -> 结果塞进结果队列
 *   LVGL 任务：sd_card_save_get_result()   在刷新定时器里顺手收结果
 *
 * 被保存的数据来自 frame_buf（jpeg_decode_get_jbuf），语义是"丢旧保新"：
 * 保存任务从取到读槽到 read_done 之间，生产者发布不了新帧（新帧自动丢弃），
 * 这正好保证"写文件时那块内存不会被覆盖"——不需要额外加锁。
 *
 * 文件操作走 POSIX（fatfs 注册的 VFS），本组件不包装 read/write/selftest
 * 这类通用能力，只管"把最新一帧存成一个文件"这一件事。
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "frame_buf.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 保存结果：保存任务写完之后回传给 LVGL（界面弹提示用） */
typedef struct {
    bool ok;
    char fname[32];     /*!< 保存的文件名（不含目录），如 "IMG_0001.jpg" */
} sd_save_result_t;

/**
 * @brief 初始化保存任务（常驻，被 trigger 唤醒）
 *
 * 由相机 App 在初始化阶段调用一次，注入解码器产出的 JPEG 双缓冲。
 * 只创建信号量/队列/任务，**不访问 SD 卡** —— 卡在不在、目录存不存在，
 * 都是 trigger 之后由保存任务去面对的事。
 *
 * @param jbuf JPEG 原数据双缓冲（jpeg_decode_get_jbuf 的返回值），不可为 NULL
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG jbuf 为 NULL；ESP_ERR_NO_MEM 内存不足
 */
esp_err_t sd_card_save_init(frame_buf_t *jbuf);

/**
 * @brief 存一帧最新 JPEG（非阻塞，可在 LVGL 回调里直接调）
 *
 * ⚠️ 只在 sd_card_save_init() 成功之后才有效。没初始化时本函数直接返回（不做事），
 *    所以调用方拿不到"到底存没存"—— 需要在界面上区分的话，请自行记住 init 的结果。
 */
void sd_card_save_trigger(void);

/**
 * @brief 非阻塞收保存结果（LVGL 定时器里调用）
 *
 * 队列容量为 1：上一次结果没被取走时，新的结果会被丢弃（界面上少弹一条提示而已）。
 *
 * @param out 收到结果时填充
 * @return true 收到结果；false 暂无结果
 */
bool sd_card_save_get_result(sd_save_result_t *out);

#ifdef __cplusplus
}
#endif
