#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
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

/* ---- 事件位：表达"刚刚发生了什么"，和"当前状态"分开 ----
 *
 * 为什么不用"标志 + 轮询"等事件：标志会被后续的相反事件改回原值，轮询就会漏掉
 * 中间那次边沿 —— 设备拔掉后快速插回时 s_device_connected 又会变回 true，于是任务
 * 永远退不出占用期（旧流不关、新设备也不处理）。事件位在被取走之前一直保留，不会丢。
 *
 * 注意下面两处 xEventGroupWaitBits 的掩码是**分开**的，原因见「管理任务」的不变量 1。 */
#define EVT_CONN     BIT0    /* 设备连上（uvc_driver_event_cb 置） */
#define EVT_DISCONN  BIT1    /* 设备断开（stream_callback 置） */
#define EVT_CMD      BIT2    /* 流的开关请求（usb_camera_stream_request 置） */

static EventGroupHandle_t s_evt = NULL;

/* ---- 流的"期望"与"实际" ----
 *
 * 这是两个互相独立的事实：
 *   - "设备在场"由 USB 事件驱动（插拔）
 *   - "用户想要" 由 App 的 enter/leave 驱动
 * 它们可以任意组合（比如设备还没插、人已经在相机 App 里等着），所以必须分开记，
 * 由管理任务在占用期里把两者对齐。 */
static volatile bool s_want_stream  = false;   /* 期望：相机 App 是否想看画面 */
static volatile bool s_is_streaming = false;   /* 实际：驱动是否正在推流 */

/* =====================================================================
 * 类型定义
 * ===================================================================== */

typedef struct {
    frame_buf_t *uvc_fb;                     /* 帧数据落点(生产者=驱动回调,消费者=解码器) */
    frame_buf_t *jbuf;                       /* 同一份 JPEG 的第二份拷贝(消费者=相机 App 的保存任务) */
} usb_cam_frame_t;

/* ---- 模块级状态 ----
 *
 * "设备在场"是一个**跨任务的状态**（两个驱动回调写 / 管理任务读），它和 EVT_CONN
 * 那个**事件**不是一回事（见顶部事件位的注释）—— 所以它只能是独立标志，不可能
 * 收成管理任务的局部变量：事件到了的时候设备可能已经走了，那条配对的 EVT_DISCONN
 * 边沿必须靠这个标志才认得出来。
 * 给它 volatile 是与 s_want_stream / s_is_streaming 保持一致（严格说不是必需：
 * 读点在 xEventGroupWaitBits 之后，事件组操作自带屏障）。
 *
 * 流句柄**不在这里** —— open/start/stop/close 全在管理任务里做，用那个任务自己的
 * 局部变量就够（"当前打开的流"没有第三个使用者，存成全局是纯死状态）。 */
static volatile bool   s_device_connected = false;
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

/* ---- 设备连接事件回调(运行在驱动后台任务,须轻量):置状态,唤醒管理任务 ----
 *
 * 地址/流索引**就地打印** —— 信息本来就在 event 里,没必要先存进全局再让管理任务
 * 取出来打（本文件其它回调也是就地打日志）。这里必然早于管理任务被唤醒，所以
 * 日志顺序仍是 "device connected" 在前、"stream opened" 在后。 */
static void uvc_driver_event_cb(const uvc_host_driver_event_data_t *event, void *user_ctx)
{
    (void)user_ctx;
    if (event->type == UVC_HOST_DRIVER_EVENT_DEVICE_CONNECTED) {
        ESP_LOGI(TAG, "UVC device connected: dev_addr=%u stream_index=%u",
                 event->device_connected.dev_addr, event->device_connected.uvc_stream_index);
        s_device_connected = true;
        xEventGroupSetBits(s_evt, EVT_CONN);
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
        s_device_connected = false;                   /* 状态：给"设备是否就绪"这类判断用 */
        xEventGroupSetBits(s_evt, EVT_DISCONN);       /* 事件：唤醒占用期，立刻退出 */
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

/* =====================================================================
 * 管理任务
 *
 * 一个 USB 设备的一生，四个阶段，一阶段一个函数：
 *
 *     等连接 ──► 开流 ──► 会话(对齐意愿 / 等断开) ──► 关流 ──┐
 *        ▲                                                    │
 *        └───────────────────── 重连 ─────────────────────┘
 *
 * 主任务就是这四行；每个阶段的"为什么"写在各自的函数头上。
 *
 * ------------------------------ 不变量 ------------------------------
 * 动这一块的任何代码之前，先把下面四条读一遍：
 *
 * 1. 两个等待点的掩码必须**分开**：等连接只等 EVT_CONN；占用期只等 DISCONN|CMD。
 *    若合成一个掩码，"断开又快速插入"会让两个位同时挂起、被一起消费 —— 结果处理了
 *    断开却把连接丢了，任务再也等不到下一次连接。
 *
 * 2. **流句柄只在管理任务里操作**（open/start/stop/close）。驱动只给 open/close 加了
 *    锁(open_close_mutex)，start/stop **没有** —— 跨任务调用会和 close 抢同一个
 *    handle（拔线那一刻就可能 use-after-free）。
 *
 * 3. **"想不想看"和"正在推不推"必须分开记**（s_want_stream / s_is_streaming）：
 *    "设备没插、人已经在相机 App 里等着"是常态，一个变量表达不了这两个维度。
 *
 * 4. **"事件"和"状态"不是一回事**：EVT_* 是"刚发生了什么"（取走就没了），
 *    s_device_connected 是"此刻在不在"（一直可读）。区别见文件顶部"事件位"那段。
 * ===================================================================== */

static bool                  wait_for_connection(void);
static uvc_host_stream_hdl_t open_stream(void);
static void                  run_session(uvc_host_stream_hdl_t stream);
static void                  close_stream(uvc_host_stream_hdl_t stream);

static void usb_camera_task(void *arg)
{
    (void)arg;

    for (;;) {
        if (!wait_for_connection()) {
            continue;                   /* 这次连接事件已经作废，重来 */
        }
        uvc_host_stream_hdl_t stream = open_stream();
        if (stream == NULL) {
            continue;                   /* 开流失败，重来 */
        }
        run_session(stream);            /* 只在设备断开时返回 */
        close_stream(stream);
    }
}

/* ---- 阶段 1：等下一次连接，并回答"设备真的还在吗" ----
 *
 * 这里同时是**唯一**清 EVT_DISCONN 的地方：本轮还没开流，就不会有占用期去消费那条
 * 配对边沿，只能在这里清。不清的话它会留到下一轮，把新一轮刚开的流立刻关掉。
 *
 * 返回 false = 连接事件到了、但设备其实已经走了（连上后马上又拔）—— 这靠
 * s_device_connected 这个"状态"才认得出来（见不变量 4）。 */
static bool wait_for_connection(void)
{
    xEventGroupWaitBits(s_evt, EVT_CONN, pdTRUE, pdFALSE, portMAX_DELAY);
    xEventGroupClearBits(s_evt, EVT_DISCONN);
    return s_device_connected;
}

/* ---- 阶段 2：打开流（只 open、不 start），失败返回 NULL ----
 *
 * 只 open 不 start：推流由会话期按 App 的意愿开关，这里不管。
 *
 * ⚠️ 为什么 open 要在这里做、而不是等 App 进了相机再做？**不是因为耗时** —— 实测
 *    只要 16ms（那个 5000ms 是"等设备出现"的**超时上限**，设备已枚举时第一轮就返回；
 *    本机时序 initialized(1616) → device connected(2094) → opened(2110)）。
 *    真正的原因是：**UVC 的"设备断开"事件是流级的** —— 驱动级只有 DEVICE_CONNECTED，
 *    DEV_GONE 只发给 uvc_stream_list 里的流。**没有流就收不到断开通知**，管理任务
 *    也就不知道该 close/reopen。所以只要设备在，就让它一直挂着一条流 ——
 *    代价只是 open 本身，不推流、不占带宽。 */
static uvc_host_stream_hdl_t open_stream(void)
{
    uvc_host_stream_hdl_t stream = NULL;
    esp_err_t err = uvc_host_stream_open(&s_stream_config, pdMS_TO_TICKS(5000), &stream);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uvc_host_stream_open failed: %s", esp_err_to_name(err));
        return NULL;
    }
    s_is_streaming = false;
    ESP_LOGI(TAG, "UVC stream opened (idle, not streaming)");
    return stream;
}

/* ---- 阶段 3：会话期 —— 把"意愿"落到"实际"，直到设备断开 ----
 *
 * 三条规矩：
 *  ① 对齐必须在**等待之前**做 —— 所以"设备刚插上、人已经在 App 里等着"能立刻开流，
 *     不用多等一轮。
 *  ② 退出只认 EVT_DISCONN。**不能**拿状态（比如"设备在不在"）当循环条件：那个标志
 *     会被"快速插回"改回 true，任务就永远退不出占用期（旧流不关、新设备也不处理）。
 *  ③ start 失败**故意不重试**：start 里含控制传输，失败后死循环重试会白烧总线 +
 *     刷屏。下一次 enter/leave 会带来新的 EVT_CMD，那时再试。 */
static void run_session(uvc_host_stream_hdl_t stream)
{
    for (;;) {
        if (s_want_stream && !s_is_streaming) {
            esp_err_t err = uvc_host_stream_start(stream);
            if (err == ESP_OK) {
                s_is_streaming = true;
                ESP_LOGI(TAG, "stream started");
            } else {
                ESP_LOGE(TAG, "uvc_host_stream_start failed: %s", esp_err_to_name(err));
            }
        } else if (!s_want_stream && s_is_streaming) {
            esp_err_t err = uvc_host_stream_stop(stream);
            s_is_streaming = false;
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "stream stopped");
            } else {
                ESP_LOGE(TAG, "uvc_host_stream_stop failed: %s", esp_err_to_name(err));
            }
        }

        /* 只等"断开"或"新的开关请求" —— 没有轮询（掩码为什么不能合并，见不变量 1）。 */
        EventBits_t bits = xEventGroupWaitBits(s_evt, EVT_DISCONN | EVT_CMD,
                                               pdTRUE, pdFALSE, portMAX_DELAY);
        if (bits & EVT_DISCONN) {
            ESP_LOGI(TAG, "Device disconnected");
            break;
        }
    }
}

/* ---- 阶段 4：回收这一轮的流 ----
 *
 * 断开时驱动自己已经把流 pause 了（uvc_host.c 的 DEV_GONE 分支），这里要做的是"拆"：
 * 释放 3 个 URB、释放接口、归还 USB 设备引用、free 掉 stream 结构体。**不做的话每次
 * 拔插漏一份，而且漏的是内部 RAM + DMA 能力。** */
static void close_stream(uvc_host_stream_hdl_t stream)
{
    s_is_streaming = false;      /* 驱动已 pause，同步我们的记录 */
    uvc_host_stream_close(stream);
    ESP_LOGI(TAG, "Stream closed, waiting for reconnect");
}

/* ---- 按需开关推流:由相机 App 的 enter/leave 调用 ----
 *
 * 只置位、不阻塞,所以从 LVGL 任务里调用是安全的;真正的 start/stop 由管理任务执行
 * (那边和 open/close 同一任务、天然串行;而且 start/stop 里各有 10ms / 100ms 的
 * vTaskDelay,不该让 UI 线程去等)。 */
void usb_camera_stream_request(bool on)
{
    s_want_stream = on;
    if (s_evt != NULL) {
        xEventGroupSetBits(s_evt, EVT_CMD);   /* 立刻唤醒,不用等下一个周期 */
    }
}

/* =====================================================================
 * 分区 B：uvc 帧 -> frame_buf（生产者就是驱动回调本身）
 * ===================================================================== */

/* ---- 生产者:驱动拼好一帧,直接 memcpy 进 frame_buf 写槽并发布。
 *
 * 一帧要**扇出成两份**,因为 frame_buf 是单读者(read_in_progress 是一个 bool,
 * 见 frame_buf.c):一份 frame_buf 只能有一个消费者。两个消费者是:
 *     uvc_fb -> 解码器 (JPEG -> RGB565 -> 上屏)
 *     jbuf   -> 相机 App 的保存任务 (原样写进 SD 卡,拍照用)
 * 所以"一帧给两个人看"必然有一次拷贝 —— 这次拷贝放在这里,换来的是解码器
 * 从此只管"JPEG 进、RGB565 出",不认识存储。
 *
 * 跑在驱动后台任务("USB-UVC")里,所以必须够快:
 *     两次 memcpy(实测 20~68KiB/帧)      约 0.4~1.0ms
 *   + 被唤醒的消费者(解码器)可能插进来    几十~几百 μs
 *   = 最坏约 2.5ms,而 URB 链路的安全余量约 23ms
 *     (单个 URB 约 11.6ms 填满,回调期间还有 2 个在排队;推导见 uvc_host_install
 *      那段的注释)。余量十几倍,而且每帧只发生一次(实测 15.6fps,约 64ms 一次)。
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

    /* 两个槽一样大(UVC_FRAME_BUF_SIZE),所以这一处长度检查同时管住两份拷贝 */
    if (frame->data_len > UVC_FRAME_BUF_SIZE) {
        ESP_LOGW(TAG, "frame %u exceeds slot %u, dropped",
                 (unsigned)frame->data_len, (unsigned)UVC_FRAME_BUF_SIZE);
        return true;
    }

    /* ① 给解码器 */
    uint8_t *slot = frame_buf_get_write(s_frame.uvc_fb);
    memcpy(slot, frame->data, frame->data_len);
    frame_buf_commit(s_frame.uvc_fb, frame->data_len);

    /* ② 给 相机 App 的保存任务(拍照)。消费者忙(正在写盘)时 commit 返回 false 自动丢本帧 */
    uint8_t *jslot = frame_buf_get_write(s_frame.jbuf);
    memcpy(jslot, frame->data, frame->data_len);
    frame_buf_commit(s_frame.jbuf, frame->data_len);

    return true;    /* 处理完了,所有权立刻还给驱动 */
}

/* =====================================================================
 * 分区 C：帧缓冲暴露
 *   uvc_fb -> 解码器（消费 JPEG 帧）
 *   jbuf   -> 相机 App 的保存任务（拍照时原样写卡）
 * ===================================================================== */

frame_buf_t *usb_camera_get_fb(void)
{
    return s_frame.uvc_fb;
}

frame_buf_t *usb_camera_get_jbuf(void)
{
    return s_frame.jbuf;
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

    /* 同一份 JPEG 的第二份拷贝落点:消费者是 相机 App 的保存任务(拍照写 SD 卡)。
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

    /* 事件组必须先创建:uvc_host_install 后设备可能立即触发事件回调(回调里要置位) */
    s_evt = xEventGroupCreate();
    if (s_evt == NULL) {
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
        .event_cb = uvc_driver_event_cb,
        .user_ctx = NULL,       /* 回调不需要上下文:设备相关的状态都在本文件的 static 里 */
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
