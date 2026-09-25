#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "frame_buf.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 USB Host + UVC 驱动，并**打开**一条 MJPEG 流（不推流）
 *
 * 推流由 usb_camera_stream_start/stop() 按需开关。解码器由 app 层创建，
 * 通过 usb_camera_get_fb() 注入输入 frame_buf 消费 JPEG 帧。
 *
 * ⚠️ 本函数**同步等待**摄像头就位（最多 5s），open 失败返回错误。本组件不重试、
 *    不重连 —— 一次失败就是终局，相机在这个开机周期里不可用。
 *
 * ⚠️ 本组件按"摄像头是常驻外设"设计（和触摸屏、MIPI 屏同级）：**不处理拔插**。
 *    拔掉后驱动会 pause 流，但本组件不会 close/重开，也不做恢复 —— 只能重启。
 *    所以它也**必须常驻**：一条流从 init 起一直挂着，直到重启。详见 usb_camera.c 顶部。
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
 * @brief 获取录像用的 JPEG 帧缓冲（帧回调填的**第三份**拷贝），供 相机 App 的录像任务 消费
 *
 * 和 get_jbuf() 同一个道理，也是单读者。**为什么不和 get_jbuf() 共用一份**：
 * frame_buf 的单读者已述只是约定、代码不强制（frame_buf_get_read() 不看是否已有读者），
 * 拍照与录像共用一份时，两边会读到同一个槽、先完成的 read_done 放掉后驱动就覆盖了
 * 另一边的内存。各自一份从根上消除这个隐患。语义同样是"丢旧保新"：录像任务写盘
 * 期间提交的新帧会被自动丢弃。
 */
frame_buf_t *usb_camera_get_rbuf(void);

/**
 * @brief 开始推流（由相机 App 的 enter 调用）
 *
 * ⚠️ **阻塞**：内部直接调驱动的 start，约 15~20ms（两次控制传输 + 10ms 设备间隔，
 *    见 uvc_host.c:953-981）。调用点跑在 LVGL 任务里，所以进相机 App 时界面会卡
 *    这一下 —— 已知并接受。**不要再从对实时性敏感的上下文调用。**
 *
 * @return ESP_OK                成功
 *         ESP_ERR_INVALID_STATE 相机未就位（init 时 open 失败）。本组件不重连，
 *                               所以这个状态**不会自己变好** —— 调用方可以据此
 *                               直接给"没有摄像头"的反馈，不必等超时
 *         其余                  驱动返回的错误（有流，但这次 start 失败）
 */
esp_err_t usb_camera_stream_start(void);

/**
 * @brief 停止推流（由相机 App 的 leave 调用）
 *
 * ⚠️ **阻塞**：驱动在 stop 内部有一次 100ms 的固定等待（等未完成的 URB 排空，
 *    见 uvc_host.c:1049），所以出相机 App 时界面会卡这么久 —— 已知并接受。
 *
 * ⚠️ 返回值一般**可以直接忽略**：用户已经不看了，停不掉也不影响什么。
 *
 * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 相机未就位；其余为驱动错误
 */
esp_err_t usb_camera_stream_stop(void);

#ifdef __cplusplus
}
#endif
