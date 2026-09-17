#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "frame_buf.h"

/* 解码后的 RGB565 帧参数：与取流分辨率一致 */
#define JPEG_DEC_FRAME_W 640
#define JPEG_DEC_FRAME_H 480
#define JPEG_DEC_FRAME_PIXEL 2
#define JPEG_DEC_FRAME_BUF_SIZE (JPEG_DEC_FRAME_W * JPEG_DEC_FRAME_H * JPEG_DEC_FRAME_PIXEL)

/* JPEG 原数据输出缓冲槽大小：实测帧 20~80KB，120KB 覆盖波动尖峰与质量档上调 */
#define JPEG_BUF_SLOT_SIZE 120000

typedef struct jpeg_decode_s jpeg_decode_t;

/**
 * @brief 创建 JPEG 解码器
 *
 * @param in 输入 JPEG 帧缓冲（由帧源侧——USB 相机 / SD 相册——创建并注入）。
 *           解码器只通过 frame_buf 接口拿帧与还帧，不感知帧来源细节。
 */
jpeg_decode_t *jpeg_decode_create(frame_buf_t *in);
void jpeg_decode_destroy(jpeg_decode_t *jd);
esp_err_t jpeg_decode_start(jpeg_decode_t *jd);

/* 消费者（app 上屏）接口：解码后的 RGB565 输出缓冲 */
frame_buf_t *jpeg_decode_get_fb(jpeg_decode_t *jd);

/* 消费者（SD 卡等存储侧）接口：JPEG 原数据输出缓冲（双缓冲，丢旧保新） */
frame_buf_t *jpeg_decode_get_jbuf(jpeg_decode_t *jd);
