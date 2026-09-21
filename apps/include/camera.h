/*
 * 相机 App —— USB 摄像头预览 + 拍照存 SD 卡
 *
 * ⚠️ 这个 App 的定位是**验证台**，不是相机应用：它要证明的是两条链路通
 *    ——"USB 摄像头 → JPEG 解码 → 上屏"和"按快门 → 原样写进 SD 卡"。
 *    所以没有缩放、没有参数调节、没有连拍，也没有"未检测到摄像头"之类的引导
 *    （没插摄像头就是一片黑 —— 见 camera.c 里的说明）。
 *
 * 它用到两个组件，外加一块住在自己文件里的东西：
 *   usb_camera    UVC 流 → 拼帧 → frame_buf（MJPEG，并扇出一份给拍照）
 *   jpeg_decoder  frame_buf → 解码 → frame_buf（RGB565）
 *   拍照保存      原本是独立的 sd_card_save 组件，现已并入 camera.c
 *
 * 生命周期分两类（理由见 camera.c 顶部）：
 *   常驻（camera_init）：usb_camera、拍照保存任务
 *   每次进出（enter/leave）：jpeg_decoder 实例、界面
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 常驻部分：把相机链路搭起来（USB Host + UVC 驱动 + 拍照保存任务）
 *
 * 由 main 在注册 App 之前调一次，位置和 time_service_init() 同层。
 * 这里建的两样**都不跟 enter/leave 拆**：帧源必须常驻（UVC 的断开事件是流级的），
 * 保存任务也必须常驻（它写盘期间持着 jbuf 读槽）—— 详见 camera.c 顶部的说明。
 *
 * **解码器不在这里建**：它跟着 camera_enter/camera_leave 按需生灭。
 * 所以本函数和界面无关，进出 App 也不会重新 open 流（open 只在设备接入时做一次，
 * 本机实测 16ms）。
 *
 * 但**推流是单独管的**：设备接入后这里只 open、不 start；推流由 enter/leave 经
 * usb_camera_stream_request() 按需开关。这样"人在别的 App 里"时，摄像头不占 USB
 * 带宽、也不跑解码（解码 4.9ms/帧 × 实测 15.6fps ≈ 7.6% core1）。
 * 详见 usb_camera_stream_request()。
 *
 * @return ESP_OK 成功；其余为 esp_err_t 错误码
 */
esp_err_t camera_init(void);

/** @brief 把相机 App 注册给 app_manager（由 main 调用） */
void camera_register(void);

#ifdef __cplusplus
}
#endif
