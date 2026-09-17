#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "frame_buf.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 USB Host + UVC 驱动，并启动摄像头管理任务
 *
 * 设备接入后由管理任务打开 MJPEG 流。解码器由 app 层创建，
 * 通过 usb_camera_get_fb() 注入输入 frame_buf 消费 JPEG 帧。
 */
esp_err_t usb_camera_init(void);

/**
 * @brief 获取相机输入帧缓冲（UVC 拷贝任务填充 JPEG 帧），供解码器消费
 */
frame_buf_t *usb_camera_get_fb(void);

#ifdef __cplusplus
}
#endif
