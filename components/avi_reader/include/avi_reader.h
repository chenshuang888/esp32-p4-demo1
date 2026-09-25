/*
 * avi_reader —— 从 MJPEG-in-AVI 文件里顺序取出 JPEG 帧
 *
 * 定位：**一个同步的解封装器**，不是播放器。没有任务、没有锁、没有定时器。
 *   - 输入是一个文件路径，输出是"一帧 JPEG 的裸指针 + 长度"；
 *   - 它不知道谁来播、播多快 —— 节奏由调用方决定（相册 App 用 lv_timer 控速）；
 *   - 它也不认识 frame_buf / jpeg_decoder / LVGL。
 *
 * 用法（跟着播放会话走）：
 *     avi_reader_t *r = avi_reader_open(path);        // 打开 + 解析头
 *     avi_reader_get_info(r, &info);                  // 宽高 / fps / 总帧数
 *     while (avi_reader_next(r, &jpg, &len) == ESP_OK) { ... 解 jpg ... }
 *     avi_reader_close(r);                            // 收尾
 *
 * ⚠️ 只在**同一个任务**里用（当前是 LVGL 任务），不保证线程安全。
 *
 * 支持范围（刻意从简，够用就好）：
 *   - 只认 `00dc` 视频块 + MJPG，不认 idx1 索引、不做 seek、不认音频流；
 *   - 帧是**顺序**取出的（没有随机访问）；
 *   - 单帧上限 AVI_READER_FRAME_MAX（和 usb_camera 的槽一致，录出来的帧超不过它）。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 单帧 JPEG 的最大字节数。和 usb_camera 的 UVC_FRAME_BUF_SIZE 一致 ——
 * 录像写入端超不过这个值（超了在帧回调里就被丢了），所以读取端按它开缓冲就够。
 * 超过它的帧会被 avi_reader_next() 判为损坏（返回 ESP_ERR_INVALID_SIZE）。 */
#define AVI_READER_FRAME_MAX  (80 * 1024)

typedef struct avi_reader_s avi_reader_t;

/* 从 AVI 头里解出来的信息（avih 块） */
typedef struct {
    uint16_t width;         /* 视频宽（像素） */
    uint16_t height;        /* 视频高（像素） */
    uint32_t fps;           /* 声明帧率 = 1000000 / dwMicroSecPerFrame；解析不出来为 0 */
    uint32_t frame_count;   /* 声明总帧数（avih.dwTotalFrames）；头没回填时为 0 */
} avi_info_t;

/**
 * @brief 打开 AVI 文件并解析头
 *
 * 会把 RIFF 头扫一遍，定位到 `movi` 才开始。中途失败（不是 RIFF/AVI、没有 movi、
 * 内存不足）一律返回 NULL，并把原因打到日志。
 *
 * 内部会分配两块 **128B 对齐**的 PSRAM 缓冲（帧缓冲 + stdio 读缓冲）——对齐是硬要求，
 * 见项目记忆里的"SD 大块读写的 128B 对齐"与 avi_writer.c 顶部注释。
 *
 * @param path 文件路径（FATFS 挂载点下的完整路径）
 * @return 句柄；NULL = 失败
 */
avi_reader_t *avi_reader_open(const char *path);

/**
 * @brief 取头部信息（画面尺寸 / 帧率 / 总帧数）
 *
 * @param out 输出，不可为 NULL
 * @return true = 解析到了 avih（out 已填）；false = 头里没有 avih 或句柄为空
 */
bool avi_reader_get_info(const avi_reader_t *r, avi_info_t *out);

/**
 * @brief 取下一帧
 *
 * 成功时 *jpg 指向**本组件内部那块帧缓冲**（不要 free），*len 是这一帧的字节数。
 * ⚠️ 这块内存会被下一次 avi_reader_next() 覆盖 —— 和 jpeg_decoder 的输出画布是同一条约定。
 *
 * @return ESP_OK              取到一帧
 *         ESP_ERR_NOT_FOUND   **没有更多帧了**（正常播完，不是错误）
 *         ESP_ERR_INVALID_SIZE 某一帧长度非法（0 或超过 AVI_READER_FRAME_MAX），文件可能损坏
 *         其余               读文件失败
 */
esp_err_t avi_reader_next(avi_reader_t *r, const uint8_t **jpg, size_t *len);

/**
 * @brief 关闭并释放所有资源（文件、两块缓冲、句柄）
 *
 * 调用了 avi_reader_next() 之后必须调用它；句柄为空是安全的空操作。
 */
void avi_reader_close(avi_reader_t *r);

#ifdef __cplusplus
}
#endif
