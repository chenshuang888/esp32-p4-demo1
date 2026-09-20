/*
 * 相机 App —— USB 摄像头预览 + 拍照存 SD 卡
 *
 * 就四块：
 *   1. 常驻（camera_init）：usb_camera + jpeg_decoder + sd_card_save，进出 App 都不动
 *   2. 界面（camera_enter / camera_leave）：一个 lv_image + 底部控制条 + 一个 20ms 定时器
 *   3. 刷新（on_refresh）：把解码输出的最新一帧贴上去；顺手收拍照结果
 *   4. 拍照（on_shutter_clicked）：只发一个信号，写盘在 sd_card_save 的任务里做
 *
 * 裁剪自 demo1 的 app_camera.c。砍掉的是：自画顶栏、自定义字体与配色、圆形按钮。
 * 保留下来的是那套**持帧时序**（见 on_refresh）和"保存中冻结预览"的做法。
 *
 * 没插摄像头时会是什么样：frame_buf 的读槽从没被写过，初值指向它 → **一片黑**。
 * 这是刻意从简（App 是验证台，不是产品）；排查时看日志有没有
 * "jpeg_decode: Decoded N frames" 就知道链路通不通。
 */
#include "camera.h"

#include <stdio.h>

#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"

#include "app_manager.h"
#include "ui_status_bar.h"
#include "picture.h"
#include "usb_camera.h"
#include "jpeg_decode_task.h"
#include "sd_card_save.h"

static const char *TAG = "camera";

/* 刷新周期。画面是 30fps(33ms)，20ms 比它快一点，保证不落帧。
 * 定时器里只是 wait_new(0) 轮询 + 一次 set_src，代价很小。
 * （demo1 用同一个值） */
#define REFRESH_MS 20

/* 底部控制条高度。预览是 640x480、从状态栏下沿开始（48+480=528），屏幕 600 高，
 * 正好剩 72px 放这一条，不会压在画面上。 */
#define CTRL_BAR_H 72

/* ---- 常驻：camera_init 建立，enter/leave 不碰 ---- */
static jpeg_decode_t *s_jd         = NULL;
static bool           s_save_ready = false;   /* 保存任务是否就绪（没就绪时快门不响应） */

/* ---- 界面：enter 建、leave 清 ---- */
static lv_obj_t      *s_scr        = NULL;
static lv_obj_t      *s_image      = NULL;
static lv_obj_t      *s_status     = NULL;    /* 控制条上的状态文字 */
static lv_image_dsc_t s_dsc;
static lv_timer_t    *s_timer      = NULL;
static bool           s_frame_held = false;   /* 是否还压着一个读槽没还 */
static bool           s_saving     = false;   /* 保存中：预览冻结在按快门那一帧 */

/*
 * 定时器回调。跑在 LVGL 任务里（已持锁），所以里面不要再加 LVGL 锁。
 *
 * ⚠️ 这里的"持帧"思路来自 demo1，但**顺序必须是"先还再等"**，demo1 那份写反了：
 *
 *    frame_buf 是"丢旧保新"语义 —— 生产者只有在 read_in_progress == false
 *    （读槽已释放）时才发布新帧、才 give 唤醒信号量。所以如果先 wait_new 再
 *    read_done，就会死锁：生产者发布不了 → 信号量永远是 0 → wait_new 永远超时
 *    → 读槽永远不释放。
 *    症状很典型：**进去显示一帧就冻住**，退出重进才换一张（leave 里会释放一次）。
 *
 *    正确顺序：① 先还上一帧（让生产者有机会发布）→ ② 再等新帧。
 *
 *    还有一点：等待失败时**不要**立刻把旧槽重新抓住 —— 那样生产者能发布的时间
 *    窗口只剩几微秒，几乎永远抓不到新帧（等于换个方式再冻住）。放弃这一轮、
 *    让它空着，生产者就有近一个定时器周期（20ms）去发布，下一轮必得。
 */
static void on_refresh(lv_timer_t *timer)
{
    (void)timer;

    /* ⓪ 先收拍照结果。保存期间预览是冻结的，收到结果才能解冻。
     *    不等结果就继续刷新的话，"Saved xxx" 这行字会被状态栏立即覆盖掉，白弹。 */
    sd_save_result_t res;
    if (sd_card_save_get_result(&res)) {
        s_saving = false;
        if (res.ok) {
            char msg[48];
            snprintf(msg, sizeof(msg), "Saved %s", res.fname);
            lv_label_set_text(s_status, msg);
        } else {
            lv_label_set_text(s_status, "Save failed");
        }
    }
    if (s_saving) {
        return;                     /* 保存中：画面停在按快门那一帧 */
    }

    frame_buf_t *fb = jpeg_decode_get_fb(s_jd);
    if (fb == NULL) {
        return;
    }

    /* ① 先还掉上一帧的读槽。它在上一次 set_src 之后又过了 20ms，
     *    LVGL 早就在这一轮的绘制里把它画完了，现在释放是安全的 */
    if (s_frame_held) {
        frame_buf_read_done(fb);
        s_frame_held = false;
    }

    /* ② 再等新帧。到这一步读槽是空的，生产者随时能发布。
     *    注意这里没有立即重新抓住旧槽 —— 见上面那段说明 */
    if (!frame_buf_wait_new(fb, 0)) {
        return;                     /* 这一轮还没有新帧：保持当前画面 */
    }
    s_dsc.data = frame_buf_get_read(fb, NULL);
    s_frame_held = true;
    lv_image_set_src(s_image, &s_dsc);
    lv_obj_invalidate(s_image);
}

/*
 * 按快门。这里**只发信号**就返回，扫目录/写文件/fsync 都在 sd_card_save 的任务里。
 * 直接在这里写的话要占住 LVGL 任务几十毫秒（预览和触摸全停），没必要。
 *
 * 保存期间把预览冻住（s_saving），是为了让屏幕上停着的那一帧就是正在存的那一帧 ——
 * 否则"存下来的和看到的不是同一张"，回头对不上会以为是 bug。
 */
static void on_shutter_clicked(lv_event_t *e)
{
    (void)e;

    if (s_saving) {
        return;                     /* 上一张还在写盘 */
    }
    if (!s_save_ready) {
        lv_label_set_text(s_status, "SD save unavailable");
        return;
    }

    s_saving = true;
    lv_label_set_text(s_status, "Saving...");
    sd_card_save_trigger();
}

static void camera_enter(void)
{
    if (s_jd == NULL) {
        /* camera_init 失败过。注册照做（桌面图标还在），进来只是个空页面 */
        ESP_LOGE(TAG, "解码器没起来，什么都不显示");
        return;
    }

    /* 按需开流：这里只"表达意图"，真正的 start 在 usb_camera 的管理任务里做
     * （那边和 open/close 同一任务、天然串行；而且 start 里含控制传输和 vTaskDelay，
     * 不该让 UI 线程去等）。放在 LVGL 锁外，和 app_demo 的"先备数据再动 LVGL"一致。 */
    usb_camera_stream_request(true);

    ESP_LOGI(TAG, "enter");
    lvgl_port_lock(0);

    s_scr = lv_obj_create(NULL);

    /* 标题和返回按钮都由公共状态栏提供，本 App 不自己画 */
    const ui_status_bar_cfg_t bar = {
        .title     = "Camera",
        .show_back = true,
    };
    ui_status_bar_apply(&bar);

    /* LVGL 不认 frame_buf，得手工把"一块 RGB565 像素"描述出来。
     * 尺寸就是解码器的输出尺寸，之后每帧只换 data 指针。 */
    s_dsc.header.magic  = LV_IMAGE_HEADER_MAGIC;
    s_dsc.header.cf     = LV_COLOR_FORMAT_RGB565;
    s_dsc.header.w      = JPEG_DEC_FRAME_W;
    s_dsc.header.h      = JPEG_DEC_FRAME_H;
    s_dsc.header.stride = JPEG_DEC_FRAME_W * JPEG_DEC_FRAME_PIXEL;
    s_dsc.data_size     = JPEG_DEC_FRAME_BUF_SIZE;

    /* ⚠️ data 初值必须指向**有效内存**（哪怕内容是黑的），否则 LVGL 绘制空 data
     *    会崩。做法照抄 demo1：先借一次读槽当初始画面，立刻还回去。
     *    没插摄像头时借到的就是那块没被写过的槽 → 显示为一片黑。 */
    frame_buf_t *fb = jpeg_decode_get_fb(s_jd);
    s_dsc.data = frame_buf_get_read(fb, NULL);
    frame_buf_read_done(fb);
    s_frame_held = false;

    s_image = lv_image_create(s_scr);
    lv_image_set_src(s_image, &s_dsc);
    /* 画面 640×480 **原样、不缩放**（缩放要么软件要么 PPA，是另一个量级）。
     * LV_ALIGN_TOP_MID = 水平居中，纵向从状态栏下沿开始（用 lv_obj_align 而不是
     * 手算 (1024-640)/2，这样不用让 apps 依赖 lcd_screen 拿屏幕宽度）。 */
    lv_obj_align(s_image, LV_ALIGN_TOP_MID, 0, UI_STATUS_BAR_HEIGHT);

    /* 底部控制条：快门 + 状态文字。宽度用 lv_pct(100) 而不是屏幕宽度常量，
     * 理由同上 —— apps 不需要知道屏幕有多宽。 */
    lv_obj_t *ctrl = lv_obj_create(s_scr);
    lv_obj_set_size(ctrl, lv_pct(100), CTRL_BAR_H);
    lv_obj_align(ctrl, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_radius(ctrl, 0, 0);        /* card 样式默认圆角，贴底用直角 */

    lv_obj_t *shutter = lv_button_create(ctrl);
    lv_obj_set_size(shutter, 120, 48);
    lv_obj_align(shutter, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_event_cb(shutter, on_shutter_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *shutter_lb = lv_label_create(shutter);
    lv_label_set_text(shutter_lb, "Shot");
    lv_obj_center(shutter_lb);

    s_status = lv_label_create(ctrl);
    lv_obj_align(s_status, LV_ALIGN_LEFT_MID, 16, 0);
    lv_label_set_text(s_status, s_save_ready ? "" : "SD save unavailable");

    s_saving = false;               /* 上次离开时可能正在保存，重新进来要复位 */
    s_timer = lv_timer_create(on_refresh, REFRESH_MS, NULL);

    lv_screen_load(s_scr);
    lvgl_port_unlock();
}

static void camera_leave(void)
{
    ESP_LOGI(TAG, "leave");

    /* 关流：同样只表达意图。用户已经不看了，就不该继续让摄像头推流、让解码器空跑 */
    usb_camera_stream_request(false);

    lvgl_port_lock(0);

    /* ① 定时器挂在 LVGL 全局的定时器链上，**不属于 s_scr** —— 不删的话它会在
     *    screen 释放之后继续跑，回调里去操作已释放的对象。
     *    和 clock.c 里那个每秒定时器是同一个坑。 */
    if (s_timer != NULL) {
        lv_timer_delete(s_timer);
        s_timer = NULL;
    }

    /* ② 还掉可能还压着的读槽。不还的话，离开之后解码器的 commit 会一直失败
     *    （丢旧保新 → 帧被连续丢弃），再回到这个 App 就一直没有画面了。 */
    if (s_frame_held) {
        frame_buf_read_done(jpeg_decode_get_fb(s_jd));
        s_frame_held = false;
    }

    lv_obj_delete_async(s_scr);
    s_scr = NULL;
    s_image = NULL;
    s_status = NULL;
    lvgl_port_unlock();
}

/* ===================== 常驻部分 ===================== */

esp_err_t camera_init(void)
{
    esp_err_t err = usb_camera_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "usb_camera_init failed: %s", esp_err_to_name(err));
        return err;
    }

    /* 解码器消费 usb_camera 的帧缓冲 —— 它只认 frame_buf，不知道帧来自 UVC */
    s_jd = jpeg_decode_create(usb_camera_get_fb());
    if (s_jd == NULL) {
        ESP_LOGE(TAG, "jpeg_decode_create failed");
        return ESP_ERR_NO_MEM;
    }
    err = jpeg_decode_start(s_jd);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "jpeg_decode_start failed: %s", esp_err_to_name(err));
        return err;
    }

    /* 保存任务消费解码器的 JPEG 原数据输出缓冲（jbuf），与解码器同寿命。
     * 失败**不算** camera_init 失败：预览和拍照是两件独立的事，少了拍照不该
     * 让整个 App 打不开 —— 界面会把快门标成不可用（见 s_save_ready）。 */
    if (sd_card_save_init(jpeg_decode_get_jbuf(s_jd)) != ESP_OK) {
        ESP_LOGE(TAG, "sd_card_save_init failed, 拍照保存不可用");
    } else {
        s_save_ready = true;
    }
    return ESP_OK;
}

static const app_desc_t s_desc = {
    .name  = "Camera",
    .icon  = PICTURE_ICON_CAMERA,
    .enter = camera_enter,
    .leave = camera_leave,
};

void camera_register(void)
{
    app_manager_register(&s_desc);
}
