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
 *
 * ⚠️ 本组件必须**常驻**（开机初始化一次，之后不销毁）：UVC 的"设备断开"事件
 * 是**流级**的 —— 驱动级只有 DEVICE_CONNECTED，DEV_GONE 只对 uvc_stream_list
 * 里的流发事件。没有活着的流就收不到断开通知，管理任务也就不知道该 close/reopen。
 */
esp_err_t usb_camera_init(void);

/**
 * @brief 获取相机输入帧缓冲（UVC 帧回调填充 JPEG 帧），供解码器消费
 */
frame_buf_t *usb_camera_get_fb(void);

/**
 * @brief 获取拍照用的 JPEG 帧缓冲（帧回调填的**第二份**拷贝），供 相机 App 的保存任务 消费
 *
 * 为什么需要两份：frame_buf 是单读者（read_in_progress 是一个 bool），一份
 * frame_buf 只能有一个消费者。解码器和 相机 App 的保存任务 要的是同一份字节，所以由
 * 本组件在帧回调里多拷一份扇出 —— 换来的是解码器不必认识存储。
 */
frame_buf_t *usb_camera_get_jbuf(void);

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
