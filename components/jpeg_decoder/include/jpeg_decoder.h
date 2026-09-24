/*
 * jpeg_decoder —— JPEG → RGB565（同步、一次性）
 *
 * 定位：**一个同步解码器**，不是一条管线。
 *   - 没有任务、没有锁、没有信号量、没有任何等待
 *   - 输入是裸指针 + 长度：组件不知道帧从哪来（USB 相机 / SD 相册 / 别的）
 *   - 输出是组件自己持有的一块画布，交给调用方上屏
 *
 * 用法（跟着 App 的 enter/leave 走）：
 *     jd = jpeg_decode_init();                  // 进 App
 *     ...
 *     jpeg_decode_frame(jd, jpg, len, &out);    // 有帧就解，out 拿来上屏
 *     ...
 *     jpeg_decode_deinit(jd);                   // 退 App
 *
 * ⚠️ 只在**同一个任务**里用。引擎是句柄里的独占资源，两个任务同时调会互相踩。
 *    （当前两个使用者都跑在 LVGL 任务里，天然满足 —— 但这条是硬约束。）
 *
 * 为什么是同步的：两个使用者都是**一次性解码** —— 相册是"点一下解一张"，相机是
 * "在一个 20ms 的 LVGL 定时器里解最新一帧"。原来的"任务 + 双缓冲管线"对它们都是
 * 纯开销：相册解完还得自己阻塞等结果，相机则多背一条任务和一层缓冲。
 * 持续流的解耦由**帧源侧**的 frame_buf 负责（usb_camera 的 uvc_fb），不在这里。
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 输出的帧规格。**这是本组件与调用方之间唯一的约定**：
 *   组件拿它分配画布，调用方拿它填 LVGL 的 image descriptor。
 * 两边读同一个宏，所以不可能不一致 —— 这也是它不做成 init 参数的原因。
 *
 * ⚠️ 这条链路假定输入 JPEG 就是 640×480（相机取的就是这个，照片也是它拍的）。
 *    喂进来别的尺寸时组件**不会报错**（引擎按 JPEG 实际尺寸输出），但调用方按
 *    640×480 描述出来的画面会是错的。 */
#define JPEG_DEC_FRAME_W     640
#define JPEG_DEC_FRAME_H     480
#define JPEG_DEC_FRAME_PIXEL 2
#define JPEG_DEC_FRAME_BUF_SIZE (JPEG_DEC_FRAME_W * JPEG_DEC_FRAME_H * JPEG_DEC_FRAME_PIXEL)

typedef struct jpeg_decoder_s jpeg_decoder_t;

/**
 * @brief 创建解码器：建解码引擎 + 分配输出画布
 *
 * 引擎和画布的生命周期都跟着这个句柄，所以它应该在 App 的 enter 里建、
 * leave 里 deinit（画布 600KB，不进 App 就不占）。
 *
 * @return 句柄；NULL = 失败（原因已打到日志）
 */
jpeg_decoder_t *jpeg_decode_init(void);

/**
 * @brief 同步解一帧 JPEG
 *
 * @param[in]  jpg 输入 JPEG 字节。函数返回后调用方即可释放（组件不留引用）
 * @param[in]  len 输入字节数，必须 > 0
 * @param[out] out 解码结果的画布。**成功时写入，失败时不修改** —— 调用方可以据此
 *                 保留上一帧画面继续跑（坏帧是预期内的：源头截断 / 拼帧溢出）。
 *                 ⚠️ 这块内存归组件所有：**不要 free**；而且**下一次
 *                 jpeg_decode_frame() 会把它的内容原地覆盖**。
 *
 * @return ESP_OK；ESP_ERR_INVALID_ARG 参数非法；
 *         其余为解码失败（这张图解不出来，不代表组件坏了 —— 见上面对 out 的说明）
 */
esp_err_t jpeg_decode_frame(jpeg_decoder_t *jd, const uint8_t *jpg, size_t len, uint8_t **out);

/**
 * @brief 销毁解码器：释放引擎和画布
 *
 * ⚠️ 调用前请确认屏幕上已经不再引用那块画布。当前 app_manager 的切换顺序是
 *    enter(新) → leave(旧)，所以 leave 里直接 deinit 是安全的：旧 screen 已经
 *    不在台面上了。
 */
void jpeg_decode_deinit(jpeg_decoder_t *jd);

#ifdef __cplusplus
}
#endif
