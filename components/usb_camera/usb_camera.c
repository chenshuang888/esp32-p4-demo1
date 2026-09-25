/*
 * usb_camera —— USB 摄像头帧源（UVC）
 *
 * 模型：**一次 open，之后只开关推流**。
 *     usb_camera_init()           : 装 USB Host + UVC 驱动，并 open 一条流（不推流）
 *     usb_camera_stream_start/stop(): 相机 App 的 enter/leave 调（**同步**，会阻塞）
 *
 * ⚠️ 本组件**不处理摄像头拔插**。约定是"摄像头是常驻外设，开机必须就位" ——
 *    和触摸屏、MIPI 屏同级：拔了就是坏了，重启。于是：
 *      - 没有管理任务、没有事件组、没有"等连接 → 占用 → 断开 → 重连"状态机；
 *      - open 只在 init 里做一次，失败即终局（init 返回错误，不重试）；
 *      - 拔掉后驱动会自己 pause 流（uvc_host.c 的 DEV_GONE 分支），但**不 free** ——
 *        所以留着的 handle 依然有效，再调 start/stop 只是控制传输失败返回错误，
 *        不会 use-after-free；代价是那条流结构永远留在驱动链表里不回收（泄漏一份）。
 *        在"永不拔"前提下不会发生，接受。
 *      - 唯一的可观测性：stream_callback 里 DEVICE_DISCONNECTED 那一行日志。
 *        拔掉后画面静止且没有任何其它提示，那行是排查时唯一的线索，别删。
 */

#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "usb/usb_host.h"
#include "usb/uvc_host.h"

#include "usb_camera.h"
#include "frame_buf.h"

#define TAG "usb_camera"

/* 单帧 MJPEG 的最大字节数。这一个宏同时喂两处，必须保持一致：
 *   ① s_stream_config.advanced.frame_size —— 驱动拼帧槽的大小（现由驱动自己分配）
 *   ② 我们自己 frame_buf 的槽大小，以及 frame_callback 里的超长检查
 *
 * ⚠️ 这个值必须**实测**，不能照描述符的 dwMaxVideoFrameSize 来定：本机那个值是
 *    614989，而它在相机的出厂默认格式(1280x720@25)和我们请求的格式(640x480@30)
 *    下报的是**同一个数** —— 说明它是设备级的保守常量（大概是照最高支持格式算的），
 *    跟我们实际用的 640x480 没有对应关系。照它分配会过度 8.8 倍。
 *
 * ⚠️ 反过来也成立：frame_size **不能填 0**。驱动在 frame_size==0 时会回退到
 *    dwMaxVideoFrameSize(uvc_host.c:856)，也就是上面那个 614989 —— 2 个槽直接变成
 *    1.2MB。以前用 user_frame_buffers 时驱动有 UVC_CHECK 强制 frame_size>0
 *    (uvc_host.c:796)，改成让驱动分配之后那条检查会被跳过，填错**不会有任何提示**。
 *
 * 80KB 的依据：40 秒 / 627 帧的极端场景扫描里最大单帧 69704 B（68 KiB），裕量 1.18 倍。
 *    超了会怎样：驱动报 UVC_HOST_FRAME_BUFFER_OVERFLOW 并丢掉那一帧 —— 不损坏、有
 *    日志、画面顿一下。所以这个值可以调紧；嫌紧就再跑一次同款扫描看有没有 overflow。 */
#define UVC_FRAME_BUF_SIZE (80 * 1024)
#define UVC_FRAME_BUF_NUM  2

/* =====================================================================
 * 类型定义
 * ===================================================================== */

typedef struct {
    frame_buf_t *uvc_fb;                     /* 帧数据落点(生产者=驱动回调,消费者=解码器) */
    frame_buf_t *jbuf;                       /* 同一份 JPEG 的第二份拷贝(消费者=相机 App 的拍照保存任务) */
    frame_buf_t *rbuf;                       /* 同一份 JPEG 的第三份拷贝(消费者=相机 App 的录像任务) */
} usb_cam_frame_t;

/* =====================================================================
 * 模块级状态
 * ===================================================================== */

/* 那条唯一的流。init 里 open 成功后就不再变；此后所有 start/stop 都作用在它上面。
 * NULL = 相机未就位（init 时 open 失败），此时 start 返回 ESP_ERR_INVALID_STATE。 */
static uvc_host_stream_hdl_t s_stream = NULL;

static usb_cam_frame_t s_frame;

/* =====================================================================
 * 内部函数声明（s_stream_config 前置引用,定义见下方分区）
 * ===================================================================== */

static bool frame_callback(const uvc_host_frame_t *frame, void *user_ctx);
static void stream_callback(const uvc_host_stream_event_data_t *event, void *user_ctx);

/* ---- 流配置:MJPEG 640x480@30fps,2 个拼帧输出槽由驱动分配 ---- */
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
        .frame_heap_caps = MALLOC_CAP_SPIRAM, /* ⚠️ 拼帧槽改由驱动分配,所以必须指明放 PSRAM。
                                               * 填 0 不是"不限" —— 驱动会当成 MALLOC_CAP_DEFAULT
                                               * (= 内部 RAM),2×80KB 静默挤占 URB 的 DMA 空间,
                                               * 不报错、只是启动日志里内存数字难看。 */
        .number_of_urbs = 3,
        .urb_size = 10 * 1024,           /* 只是提示:驱动会向上取整到 ISOC 包的整数倍
                                          * (本机实际 4×3072 = 12288,见 uvc_host_install 那段) */
        /* user_frame_buffers 留空(NULL):槽交给驱动分配并管理,我们不再持有 */
    },
};

/* =====================================================================
 * 分区 A：USB Host 与流
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

/* ---- 流事件回调(运行在驱动后台任务,须轻量):错误与缓冲区告警 ----
 *
 * 这里**不做任何状态流转** —— 本组件不重连,收到什么都没有要恢复的动作。 */
static void stream_callback(const uvc_host_stream_event_data_t *event, void *user_ctx)
{
    (void)user_ctx;
    switch (event->type) {
    case UVC_HOST_TRANSFER_ERROR:
        ESP_LOGE(TAG, "USB transfer error: %d", (int)event->transfer_error.error);
        break;
    case UVC_HOST_DEVICE_DISCONNECTED:
        /* 本组件不做重连。拔掉后画面会静止、且不会有任何其它提示 ——
         * 这行日志是"画面为什么不动"的唯一线索，**不要删**。 */
        ESP_LOGW(TAG, "camera gone, restart required");
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

/* ---- 开关推流:由相机 App 的 enter/leave 调用 ----
 *
 * 直接调驱动的 start/stop，不再经过管理任务 —— 因为"断开时 close 会和 start/stop
 * 抢同一个 handle"这个前提已经不存在了（本组件永不 close）。
 *
 * 两者都**阻塞**（start 约 15~20ms、stop 约 100ms），耗时来源见 usb_camera.h。
 *
 * 失败时**只记日志、不做任何补救**（本组件不重连）。返回值是给调用方判断用的：
 * "相机未就位"用 ESP_ERR_INVALID_STATE 单独标出，与"有流但这次 start 失败"区分开，
 * 应用层据此可以给不同的提示，而不是干等一片黑。 */
esp_err_t usb_camera_stream_start(void)
{
    if (s_stream == NULL) {
        /* 相机未就位（init 时 open 失败）。这不是"这次运气不好"，而是一个确定、
         * 且不会自己变好的状态（本组件不重连）—— 所以单独一个错误码，别混进
         * 驱动的错误里。 */
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = uvc_host_stream_start(s_stream);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "stream started");
    } else {
        ESP_LOGE(TAG, "uvc_host_stream_start failed: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t usb_camera_stream_stop(void)
{
    if (s_stream == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = uvc_host_stream_stop(s_stream);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "stream stopped");
    } else {
        ESP_LOGE(TAG, "uvc_host_stream_stop failed: %s", esp_err_to_name(err));
    }
    return err;
}

/* =====================================================================
 * 分区 B：uvc 帧 -> frame_buf（生产者就是驱动回调本身）
 * ===================================================================== */

/* ---- 生产者:驱动拼好一帧,直接 memcpy 进 frame_buf 写槽并发布。
 *
 * 一帧要**扇出成三份**,因为 frame_buf 是单读者(read_in_progress 是一个 bool,
 * 见 frame_buf.c):一份 frame_buf 只能有一个消费者。三个消费者是:
 *     uvc_fb -> 解码器    (JPEG -> RGB565 -> 上屏)
 *     jbuf   -> 拍照保存任务 (原样写进 SD 卡,拍照用)
 *     rbuf   -> 录像任务     (原样按序写进 AVI 文件,录像用)
 * 所以"一帧给三个人看"必然有两次拷贝 —— 这些拷贝放在这里,换来的是解码器/拍照/
 * 录像三者从此互不认识,各自只管自己那份输入。
 *
 * ⚠️ 为什么拍照和录像不共用一份(非要付第三次拷贝)? 因为 frame_buf 的"单读者"
 *    是约定,代码不强制:frame_buf_get_read() 不检查是否已有读者。若两者共用一份,
 *    一个读盘未读完时另一个也 get_read 到同一个槽,先完成的那个 read_done 一放,
 *    驱动立刻覆盖该槽,另一个还在读 -> 内存被踩。各自一份从根上消除这个隐患。
 *
 * 跑在驱动后台任务("USB-UVC")里,所以必须够快:
 *     三次 memcpy(实测 20~68KiB/帧)      约 0.6~1.5ms
 *   + 被唤醒的消费者(解码器)可能插进来    几十~几百 μs
 *   = 最坏约 3ms,而 URB 链路的安全余量约 23ms
 *     (单个 URB 约 11.6ms 填满,回调期间还有 2 个在排队;推导见 uvc_host_install
 *      那段的注释)。余量近十倍,而且每帧只发生一次(实测 15.6fps,约 64ms 一次)。
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

    /* 三个槽一样大(UVC_FRAME_BUF_SIZE),所以这一处长度检查同时管住三份拷贝 */
    if (frame->data_len > UVC_FRAME_BUF_SIZE) {
        ESP_LOGW(TAG, "frame %u exceeds slot %u, dropped",
                 (unsigned)frame->data_len, (unsigned)UVC_FRAME_BUF_SIZE);
        return true;
    }

    /* ① 给解码器 */
    uint8_t *slot = frame_buf_get_write(s_frame.uvc_fb);
    memcpy(slot, frame->data, frame->data_len);
    frame_buf_commit(s_frame.uvc_fb, frame->data_len);

    /* ② 给 相机 App 的拍照保存任务。消费者忙(正在写盘)时 commit 返回 false 自动丢本帧 */
    uint8_t *jslot = frame_buf_get_write(s_frame.jbuf);
    memcpy(jslot, frame->data, frame->data_len);
    frame_buf_commit(s_frame.jbuf, frame->data_len);

    /* ③ 给 相机 App 的录像任务。同上,录像任务写 SD 期间 commit 返回 false 自动丢本帧 */
    uint8_t *rslot = frame_buf_get_write(s_frame.rbuf);
    memcpy(rslot, frame->data, frame->data_len);
    frame_buf_commit(s_frame.rbuf, frame->data_len);

    return true;    /* 处理完了,所有权立刻还给驱动 */
}

/* =====================================================================
 * 分区 C：帧缓冲暴露
 *   uvc_fb -> 解码器（消费 JPEG 帧）
 *   jbuf   -> 相机 App 的拍照保存任务（拍照时原样写卡）
 *   rbuf   -> 相机 App 的录像任务（录像时按序写 AVI 文件）
 * ===================================================================== */

frame_buf_t *usb_camera_get_fb(void)
{
    return s_frame.uvc_fb;
}

frame_buf_t *usb_camera_get_jbuf(void)
{
    return s_frame.jbuf;
}

frame_buf_t *usb_camera_get_rbuf(void)
{
    return s_frame.rbuf;
}

/* =====================================================================
 * 初始化
 * ===================================================================== */

esp_err_t usb_camera_init(void)
{
    /* 帧数据落点:驱动回调直接 memcpy 进这里的槽,解码器从这消费。
     * (uvc 那边用来拼帧的 2 个槽**不在这里**分配 —— 交给驱动自己管,
     *  见 s_stream_config 的 frame_heap_caps) */
    const frame_buf_cfg_t fb_cfg = {
        .slot_size = UVC_FRAME_BUF_SIZE,
        .has_len = true,
    };
    s_frame.uvc_fb = frame_buf_create(&fb_cfg);
    if (s_frame.uvc_fb == NULL) {
        ESP_LOGE(TAG, "frame_buf_create failed");
        return ESP_ERR_NO_MEM;
    }

    /* 同一份 JPEG 的第二份拷贝落点:消费者是 相机 App 的拍照保存任务(拍照写 SD 卡)。
     * 槽大小与 uvc_fb 相同即可 —— 超过 UVC_FRAME_BUF_SIZE 的帧在 frame_callback
     * 入口就已经丢了,所以这里不需要更大的槽,也不需要再判长度。 */
    const frame_buf_cfg_t jbuf_cfg = {
        .slot_size = UVC_FRAME_BUF_SIZE,
        .has_len = true,
    };
    s_frame.jbuf = frame_buf_create(&jbuf_cfg);
    if (s_frame.jbuf == NULL) {
        ESP_LOGE(TAG, "jbuf frame_buf_create failed");
        return ESP_ERR_NO_MEM;
    }

    /* 同一份 JPEG 的第三份拷贝落点:消费者是 相机 App 的录像任务(录像写 AVI)。
     * 同样大小。多出一个 2×80KB 的常驻缓冲,换来拍照/录像各自独占一份输入、
     * 不会互踩对方正在读的槽(见 frame_callback 顶部那段)。 */
    const frame_buf_cfg_t rbuf_cfg = {
        .slot_size = UVC_FRAME_BUF_SIZE,
        .has_len = true,
    };
    s_frame.rbuf = frame_buf_create(&rbuf_cfg);
    if (s_frame.rbuf == NULL) {
        ESP_LOGE(TAG, "rbuf frame_buf_create failed");
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
     * ⚠️ event_cb 传 NULL:本组件不再需要"设备接入"的通知 —— 那条通知以前是用来
     *    唤醒管理任务的，现在没有管理任务了。驱动对它有判空，传 NULL 合法
     *    (uvc_host.c:81)。
     *
     * ⚠️ xCoreID = 0 把驱动任务绑在 core 0 —— frame_callback 就跑在这个任务里,
     *    所以这条链路的安全余量 = (URB 数 - 1) × 单个 URB 的填满时长:
     *        URB 实际 12288 B —— 配置里写的 urb_size = 10KiB 只是提示,驱动按
     *          ISOC 包大小(3072 MPS)向上取整成 4 个包(实测日志
     *          "Each: 12288 bytes, 4 ISOC packets, 3072 MPS")
     *        码率上界 ≈ 15.6fps × 68KiB(单帧实测上限) ≈ 1.06MB/s
     *        → 单个 URB 填满 ≈ 11.6ms;3 个 URB 里还有 2 个在排队 → 余量 ≈ 23ms
     *    回调里那次 memcpy(实测 0.2~0.5ms)相对它很安全。等时传输的 URB 是
     *    **按时钟调度**填的(不是"数据攒够一个 URB 才满"),所以这个时长跟
     *    URB 大小 / 码率成正比。
     *
     *    **如果以后出现"长时间连续不阻塞"的高优先级任务**,它会在核心上把这个
     *    任务饿死、吃光这 23ms。首选做法是把它绑到 core 1
     *    (xTaskCreatePinnedToCore(..., 1)),而不是加 URB —— 零成本且根治;
     *    加 URB 只是多买缓冲时间,还占 DMA 可用的内部 RAM。 */
    const uvc_host_driver_config_t uvc_config = {
        .driver_task_stack_size = 4096,
        .driver_task_priority = 5,
        .xCoreID = 0,
        .create_background_task = true,
        .event_cb = NULL,
        .user_ctx = NULL,
    };
    err = uvc_host_install(&uvc_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uvc_host_install failed: %s", esp_err_to_name(err));
        return err;
    }

    /* 3. 打开那唯一一条流(只 open、不 start:推流由 stream_start/stop 按需开关)
     *
     * 同步阻塞,最多 5s —— 那个 5000 是"等设备出现"的**超时上限**,设备已枚举时
     * 第一轮就返回(本机实测 16ms)。本函数在 app_main 里、LVGL 之前调用,
     * 所以这 5s 阻塞没有 UI 影响。
     *
     * ⚠️ 这是本组件**唯一**一次 open。失败即终局:不重试、不重连 —— 相机在这个
     *    开机周期里就是不可用(帧缓冲区仍会给出去,只是永远收不到新帧)。 */
    err = uvc_host_stream_open(&s_stream_config, pdMS_TO_TICKS(5000), &s_stream);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uvc_host_stream_open failed: %s (摄像头未就位?)", esp_err_to_name(err));
        s_stream = NULL;
        return err;
    }

    ESP_LOGI(TAG, "UVC stream opened (idle, not streaming)");
    return ESP_OK;
}
