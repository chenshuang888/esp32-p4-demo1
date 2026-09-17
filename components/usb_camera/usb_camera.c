#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "usb/usb_host.h"
#include "usb/uvc_host.h"

#include "usb_camera.h"
#include "frame_buf.h"

#define TAG "usb_camera"

#define USB_CAMERA_TASK_STACK 4096
#define USB_CAMERA_TASK_PRIO  6

/* UVC 拼帧槽:每槽容量必须 >= 摄像头 dwMaxVideoFrameSize(本机实测 614989),700KB 留裕量。
 * 同一尺寸也用作拷贝目标 frame_buf 的槽 */
#define UVC_FRAME_BUF_SIZE (700 * 1024)
#define UVC_FRAME_BUF_NUM  2

/* =====================================================================
 * 类型定义
 * ===================================================================== */

typedef struct {
    uint8_t     *user_fb[UVC_FRAME_BUF_NUM]; /* uvc 驱动拼帧槽(驱动 user_frame_buffers) */
    frame_buf_t *uvc_fb;                     /* 帧数据落点(生产者=驱动回调,消费者=解码器) */
} usb_cam_frame_t;

typedef struct {
    uint8_t dev_addr;
    uint8_t uvc_stream_index;
    bool    device_connected;
    uvc_host_stream_hdl_t uvc_stream;        /* 当前打开的流(开/关流用;帧的归还由驱动自己做) */
    SemaphoreHandle_t event_sem;             /* 设备连接通知:uvc_driver_event_cb give,usb_camera_task 阻塞等 */
} usb_cam_connect_t;

typedef struct {
    usb_cam_connect_t connect;
    usb_cam_frame_t   frame;
} usb_camera_ctx_t;

/* usb 相机模块全部状态(连接生命周期 + 帧泵拷贝交接) */
static usb_camera_ctx_t s_usb_cam;

/* =====================================================================
 * 内部函数声明（s_stream_config 前置引用,定义见下方分区）
 * ===================================================================== */

static bool frame_callback(const uvc_host_frame_t *frame, void *user_ctx);
static void stream_callback(const uvc_host_stream_event_data_t *event, void *user_ctx);

/* ---- 流配置:MJPEG 640x480@30fps,2 个拼帧输出槽由上层指定 ---- */
static const uvc_host_stream_config_t s_stream_config = {
    .event_cb = stream_callback,
    .frame_cb = frame_callback,
    .user_ctx = NULL,
    .usb = {
        .vid = UVC_HOST_ANY_VID,
        .pid = UVC_HOST_ANY_PID,
        .uvc_stream_index = 0,
    },
    .vs_format = {
        .h_res = 640,
        .v_res = 480,
        .fps = 30,
        .format = UVC_VS_FORMAT_MJPEG,
    },
    .advanced = {
        .number_of_frame_buffers = UVC_FRAME_BUF_NUM,
        .frame_size = UVC_FRAME_BUF_SIZE,
        .frame_heap_caps = 0,            /* 帧缓冲由 user_frame_buffers 提供,驱动不分配 */
        .number_of_urbs = 3,
        .urb_size = 10 * 1024,
        .user_frame_buffers = s_usb_cam.frame.user_fb, /* 上层指定输出槽数量与位置 */
    },
};

/* =====================================================================
 * 分区 A：USB 连接管理
 * ===================================================================== */

/* ---- USB Host 库级事件处理:负责设备枚举/释放。
 *  UVC 后台任务只处理 client 事件,缺此任务设备无法枚举 ---- */
static void usb_lib_task(void *arg)
{
    while (1) {
        uint32_t event_flags;
        usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            usb_host_device_free_all();
        }
    }
}

/* ---- 设备连接事件回调(运行在驱动后台任务,须轻量):记录地址,唤醒管理任务开流 ---- */
static void uvc_driver_event_cb(const uvc_host_driver_event_data_t *event, void *user_ctx)
{
    (void)user_ctx;
    if (event->type == UVC_HOST_DRIVER_EVENT_DEVICE_CONNECTED) {
        s_usb_cam.connect.dev_addr = event->device_connected.dev_addr;
        s_usb_cam.connect.uvc_stream_index = event->device_connected.uvc_stream_index;
        s_usb_cam.connect.device_connected = true;
        xSemaphoreGive(s_usb_cam.connect.event_sem);
    }
}

/* ---- 流事件回调(运行在驱动后台任务,须轻量):断开/溢出通知 ---- */
static void stream_callback(const uvc_host_stream_event_data_t *event, void *user_ctx)
{
    (void)user_ctx;
    switch (event->type) {
    case UVC_HOST_TRANSFER_ERROR:
        ESP_LOGE(TAG, "USB transfer error: %d", (int)event->transfer_error.error);
        break;
    case UVC_HOST_DEVICE_DISCONNECTED:
        ESP_LOGI(TAG, "Device disconnected");
        s_usb_cam.connect.device_connected = false;
        break;
    case UVC_HOST_FRAME_BUFFER_OVERFLOW:
        ESP_LOGW(TAG, "Frame buffer overflow");
        break;
    case UVC_HOST_FRAME_BUFFER_UNDERFLOW:
        ESP_LOGW(TAG, "Frame buffer underflow");
        break;
    default:
        ESP_LOGW(TAG, "Unsupported UVC stream event: %d", (int)event->type);
        break;
    }
}

/* ---- 管理任务:设备连接后打开 MJPEG 流并等待断开 ---- */
static void usb_camera_task(void *arg)
{
    while (1) {
        if (xSemaphoreTake(s_usb_cam.connect.event_sem, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (!s_usb_cam.connect.device_connected) {
            continue;
        }

        ESP_LOGI(TAG, "UVC device connected: dev_addr=%u stream_index=%u",
                 s_usb_cam.connect.dev_addr, s_usb_cam.connect.uvc_stream_index);

        esp_err_t err;
        uvc_host_stream_hdl_t uvc_stream = NULL;
        err = uvc_host_stream_open(&s_stream_config, pdMS_TO_TICKS(5000), &uvc_stream);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "uvc_host_stream_open failed: %s", esp_err_to_name(err));
            continue;
        }
        s_usb_cam.connect.uvc_stream = uvc_stream;
        ESP_LOGI(TAG, "UVC stream opened");
        err = uvc_host_stream_start(uvc_stream);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "uvc_host_stream_start failed: %s", esp_err_to_name(err));
        }

        /* 等待设备断开(stream_callback 清 device_connected);断开后关流,回外层等待重连。
         * 关流不会卡在"帧未归还"上:帧在 frame_callback 里就地处理完并立刻还给驱动,
         * 我们从不持有 UVC 帧(详见分区 B 的注释) */
        while (s_usb_cam.connect.device_connected) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        uvc_host_stream_close(uvc_stream);
        s_usb_cam.connect.uvc_stream = NULL;
        ESP_LOGI(TAG, "Stream closed, waiting for reconnect");
    }
}

/* =====================================================================
 * 分区 B：uvc 帧 -> frame_buf（生产者就是驱动回调本身）
 * ===================================================================== */

/* ---- 生产者:驱动拼好一帧,直接 memcpy 进 frame_buf 写槽并发布。
 *
 * 跑在驱动后台任务("USB-UVC")里,所以必须够快:
 *     memcpy 一帧(实测 20~80KB)          约 0.2~0.5ms
 *   + 被唤醒的消费者(解码器)可能插进来    几十~几百 μs
 *   = 最坏约 1.5ms,而 URB 链路的安全余量约 9.6ms
 *     (3 个 URB 各约 4.8ms 填满,回调期间还有 2 个在排队;见 uvc_host_install
 *      那段的注释)。余量 6 倍以上,而且每帧只发生一次(33ms 一次)。
 *
 * 相比"另起拷贝任务 + 队列"的写法,这里省掉一个常驻任务、一个队列和每帧
 * 一次任务切换;更重要的是**我们从头到尾不持有 UVC 帧** —— 返回 true 之后
 * 驱动会立刻调 uvc_host_frame_return()(uvc_isoc.c:189-191),所以
 * "关流前必须把所有帧还完"这个问题从根上不存在了。
 *
 * 丢帧不在这里处理:frame_buf_commit() 失败即本帧自动丢弃(丢旧保新语义)。 */
static bool frame_callback(const uvc_host_frame_t *frame, void *user_ctx)
{
    (void)user_ctx;

    if (frame->data_len <= UVC_FRAME_BUF_SIZE) {
        uint8_t *slot = frame_buf_get_write(s_usb_cam.frame.uvc_fb);
        memcpy(slot, frame->data, frame->data_len);
        frame_buf_commit(s_usb_cam.frame.uvc_fb, frame->data_len);
    } else {
        ESP_LOGW(TAG, "frame %u exceeds slot %u, dropped",
                 (unsigned)frame->data_len, (unsigned)UVC_FRAME_BUF_SIZE);
    }

    return true;    /* 处理完了,所有权立刻还给驱动 */
}

/* =====================================================================
 * 分区 C：帧缓冲暴露（解码器经此拿到输入 frame_buf 消费 JPEG 帧）
 * ===================================================================== */

frame_buf_t *usb_camera_get_fb(void)
{
    return s_usb_cam.frame.uvc_fb;
}

/* =====================================================================
 * 初始化
 * ===================================================================== */

esp_err_t usb_camera_init(void)
{
    memset(&s_usb_cam, 0, sizeof(s_usb_cam));

    /* 帧源原语必须就绪于设备接入之前:uvc 拼帧槽 + frame_buf 落点 */
    for (int i = 0; i < UVC_FRAME_BUF_NUM; i++) {
        s_usb_cam.frame.user_fb[i] = heap_caps_aligned_alloc(64, UVC_FRAME_BUF_SIZE, MALLOC_CAP_SPIRAM);
        if (s_usb_cam.frame.user_fb[i] == NULL) {
            ESP_LOGE(TAG, "frame buf %d alloc failed", i);
            return ESP_ERR_NO_MEM;
        }
    }

    /* 帧数据落点:驱动回调直接 memcpy 进这里的槽,解码器从这消费 */
    const frame_buf_cfg_t fb_cfg = {
        .slot_size = UVC_FRAME_BUF_SIZE,
        .has_len = true,
    };
    s_usb_cam.frame.uvc_fb = frame_buf_create(&fb_cfg);
    if (s_usb_cam.frame.uvc_fb == NULL) {
        ESP_LOGE(TAG, "frame_buf_create failed");
        return ESP_ERR_NO_MEM;
    }

    /* 信号量必须先创建:uvc_host_install 后设备可能立即触发事件回调 */
    s_usb_cam.connect.event_sem = xSemaphoreCreateBinary();
    if (s_usb_cam.connect.event_sem == NULL) {
        return ESP_ERR_NO_MEM;
    }

    /* 1. USB Host 库(uvc_host_install 要求先安装) */
    const usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LOWMED,
    };
    esp_err_t err = usb_host_install(&host_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "usb_host_install failed: %s", esp_err_to_name(err));
        return err;
    }

    /* USB Host 库级事件任务:处理设备枚举、无 client 时释放设备 */
    if (xTaskCreate(usb_lib_task, "usb_lib", 4096, NULL, 15, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    /* 2. UVC 驱动(后台任务模式,自动处理 USB Host 事件)
     *
     * ⚠️ xCoreID = 0 把驱动任务绑在 core 0 —— frame_callback 就跑在这个任务里,
     *    所以这条链路的安全余量 = (URB 数 - 1) × 单个 URB 的填满时长:
     *        URB 填满时长 = urb_size / 码率 = 10KB / 2.1MB/s ≈ 4.8ms
     *        3 个 URB,回调期间还有 2 个在排队  →  余量 ≈ 9.6ms
     *    回调里那次 memcpy(~1ms)相对它很安全。等时传输的 URB 是**按时钟调度**
     *    填的(不是"数据攒够 10KB 才满"),所以这个时长跟 urb_size/码率成正比。
     *
     *    **如果以后出现"长时间连续不阻塞"的高优先级任务**,它会在核心上把这个
     *    任务饿死、吃光这 9.6ms。首选做法是把它绑到 core 1
     *    (xTaskCreatePinnedToCore(..., 1)),而不是加 URB —— 零成本且根治;
     *    加 URB 只是多买缓冲时间,还占 DMA 可用的内部 RAM。 */
    const uvc_host_driver_config_t uvc_config = {
        .driver_task_stack_size = 4096,
        .driver_task_priority = 5,
        .xCoreID = 0,
        .create_background_task = true,
        .event_cb = uvc_driver_event_cb,
        .user_ctx = &s_usb_cam,
    };
    err = uvc_host_install(&uvc_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uvc_host_install failed: %s", esp_err_to_name(err));
        return err;
    }

    /* 3. 管理任务(解码器由 app 层创建并通过 frame_buf 注入) */
    if (xTaskCreate(usb_camera_task, "usb_cam", USB_CAMERA_TASK_STACK, NULL,
                    USB_CAMERA_TASK_PRIO, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "USB camera initialized. Plug in a UVC camera.");
    return ESP_OK;
}
