/*
 * 相机 App —— USB 摄像头预览
 *
 * ⚠️ 这个 App 的定位是**验证台**，不是相机应用：它只证明
 *    "USB 摄像头 → JPEG 解码 → 上屏"这条链路是通的。
 *    所以没有拍照、没有缩放、没有参数调节，也没有"未检测到摄像头"之类的提示
 *    （没插摄像头就是一片黑 —— 见 camera.c 里的说明）。
 *
 * 真正的活都在两个组件里：
 *   usb_camera    UVC 流 → 拼帧 → frame_buf（MJPEG）
 *   jpeg_decoder  frame_buf → 解码 → frame_buf（RGB565）
 * 本 App 只做一件事：用一个定时器把解码输出的**最新一帧**贴到屏幕上。
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 常驻部分：把相机链路搭起来（USB Host + UVC 驱动 + JPEG 解码器）
 *
 * 由 main 在注册 App 之前调一次，位置和 time_service_init() 同层。
 * **它和界面无关** —— enter/leave 只管界面，进出 App 不会销毁解码器、也不会重新
 * 枚举 USB 设备（那可能要好几秒）。
 *
 * 但**推流是单独管的**：设备接入后这里只 open、不 start；推流由 enter/leave 经
 * usb_camera_stream_request() 按需开关。这样"人在别的 App 里"时，摄像头不占 USB
 * 带宽、也不跑解码（解码约 15% core1）。详见 usb_camera_stream_request()。
 *
 * @return ESP_OK 成功；其余为 esp_err_t 错误码
 */
esp_err_t camera_init(void);

/** @brief 把相机 App 注册给 app_manager（由 main 调用） */
void camera_register(void);

#ifdef __cplusplus
}
#endif
