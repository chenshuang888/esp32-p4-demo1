/*
 * avi_writer —— 把一串 JPEG 帧写成 MJPEG-in-AVI 文件
 *
 * 它只干一件事：**把独立的一帧帧 JPEG 封装成一个标准 AVI 文件**。
 * 它不知道帧从哪来（USB 相机 / 相册 / SD 文件都行），也不认识 frame_buf、相机、LVGL。
 *
 * 为什么录像要它：相机帧源（usb_camera）给出的本来就是 MJPEG —— 每帧是一张独立的
 * JPEG。所以"录像"= 把这些 JPEG 按时序连续落盘。裸拼（首尾相接）最省事，但多数
 * 播放器不认；包成 AVI 之后 PC/手机都能直接播（代价是头里几个大小字段要等收尾回填）。
 *
 * 生命周期跟着录制会话走：录制开始 open、每帧 add_frame、录制结束 close。
 * 没有任务、没有锁 —— 只在**同一个任务**里用（和 jpeg_decoder 同一条硬约束）。
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct avi_writer_s avi_writer_t;

/**
 * @brief 创建 AVI 文件并写好头（内部会 fopen(path, "wb")）
 *
 * 头里的 RIFF size / movi size / dwTotalFrames 在写完第一帧前无法得知，
 * 这里先写 0，由 avi_writer_close() 回填。所以**中途拔卡/断电得到的文件是坏的**，
 * 必须 close 过才是一个完整可播的 AVI。
 *
 * @param path 文件路径（调用方负责目录已存在，如 "/sdcard/DCIM/VID_0001.avi"）
 * @param w    视频宽（像素），必须 > 0
 * @param h    视频高（像素），必须 > 0
 * @param fps  帧率（写进 AVI 头，播放器据此定时；本写入器不会自己去凑这个帧率）
 * @return 句柄；NULL = 失败（参数非法 / 创建文件失败，原因由 stdio 决定）
 */
avi_writer_t *avi_writer_open(const char *path, uint16_t w, uint16_t h, uint32_t fps);

/**
 * @brief 追加一帧 JPEG
 *
 * 必须按播放顺序调用。帧率由 open 时的 fps 决定，本函数不会插入延时或复制帧 ——
 * 调用方"来一帧写一帧"，文件里的帧数就等于实际收到的帧数。
 *
 * ⚠️ 返回 ESP_FAIL 之后本写入器进入"已损坏"状态，后续 add 都会直接失败；
 *    遇到失败请尽快 close()（close 会尽力回填并返回 ESP_FAIL）。
 *
 * @param jpg 一帧 JPEG 字节（调用方在函数返回后可自由释放，本写入器不留引用）
 * @param len 字节数，必须 > 0
 * @return ESP_OK；ESP_ERR_INVALID_ARG 参数非法；ESP_FAIL 写失败（卡满 / 拔出）
 */
esp_err_t avi_writer_add_frame(avi_writer_t *aw, const uint8_t *jpg, size_t len);

/**
 * @brief 收尾：回填头里的三个大小/帧数字段，fsync，关文件，释放句柄
 *
 * 调用后句柄失效，不可再用。**必须调用**，否则文件不是完整的 AVI。
 *
 * @param[out] frames_out 可选，回填实际写入的帧数（诊断用）
 * @return ESP_OK；ESP_FAIL 收尾过程中发现写错误（文件可能不可播）；ESP_ERR_INVALID_ARG 句柄为空
 */
esp_err_t avi_writer_close(avi_writer_t *aw, uint32_t *frames_out);

#ifdef __cplusplus
}
#endif
