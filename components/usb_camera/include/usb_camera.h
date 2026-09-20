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
 * 设备接入后由管理任务**打开**（open）MJPEG 流，但**不推流** —— 推流由
 * usb_camera_stream_request() 按需开关。解码器由 app 层创建，
 * 通过 usb_camera_get_fb() 注入输入 frame_buf 消费 JPEG 帧。
 */
esp_err_t usb_camera_init(void);

/**
 * @brief 获取相机输入帧缓冲（UVC 拷贝任务填充 JPEG 帧），供解码器消费
 */
frame_buf_t *usb_camera_get_fb(void);

/**
 * @brief 按需开关推流（由相机 App 的 enter/leave 调用）
 *
 * 只置"期望状态"并唤醒管理任务，**不阻塞**，所以可以从 LVGL 任务里调用。
 * 真正的 start/stop 在管理任务里执行 —— 必须如此：驱动的 start/stop 不受
 * open_close_mutex 保护，跨任务调用会和 close 抢同一个 handle。
 *
 * 设备不在场时传 on=true 也没关系：管理任务会记住这个期望，等设备接上并
 * open 完成后自动开流。
 *
 * @param on true = 开始推流；false = 停止推流
 */
void usb_camera_stream_request(bool on);

#ifdef __cplusplus
}
#endif
