/*
 * 相机 App —— USB 摄像头预览 + 拍照存 SD 卡
 *
 * 本文件里住着三块东西，**生命周期各不相同** —— 这是读它时最该先看清的地方：
 *
 *   1. 常驻（camera_init，main 在注册 App 之前调一次，之后永不拆）
 *        usb_camera  帧源。必须常驻：UVC 的"设备断开"是**流级**事件，
 *                    没有活着的流就收不到断开通知（详见 usb_camera.h）。
 *        sd_save     拍照保存任务。必须常驻：它写盘期间一直持着 jbuf 的读槽，
 *                    中途被杀会让读槽永远释放不了（详见 sd_save_init 的注释）。
 *
 *   2. 每次进出（camera_enter / camera_leave）
 *        jpeg_decoder 实例  它吃的是"丢旧保新"的流，杀掉重启零损失，
 *                           所以按需建拆 —— 不进相机就不占那约 1.4MB PSRAM。
 *        UI（一个 lv_image + 底部控制条 + 一个 20ms 定时器）
 *
 *   3. 拍照（on_shutter_clicked）只发一个信号，写盘在 sd_save_task 里做
 *
 * 第 1 / 2 的分界可以概括成一句话：
 *   **有不可中断的临界区 -> 常驻；可随时打断 -> 动态。**
 * （sd_save 的临界区 = 持着 jbuf 读槽写盘；解码器没有任何临界区。）
 *
 * 裁剪自 demo1 的 app_camera.c。砍掉的是：自画顶栏、自定义字体与配色、圆形按钮。
 * 保留下来的是那套**持帧时序**（见 on_refresh）和"保存中冻结预览"的做法。
 *
 * 没插摄像头时会是什么样：frame_buf 的读槽从没被写过，初值指向它 -> **一片黑**。
 * 这是刻意从简（App 是验证台，不是产品）；排查时看日志有没有
 * "jpeg_decode: Decoded N frames" 就知道链路通不通。
 */
#include "camera.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>     /* fsync() */

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"

#include "app_manager.h"
#include "ui_status_bar.h"
#include "picture.h"
#include "usb_camera.h"
#include "jpeg_decode_task.h"
#include "sd_card.h"

static const char *TAG = "camera";

/* 刷新周期。画面是 30fps(33ms)，20ms 比它快一点，保证不落帧。
 * 定时器里只是 wait_new(0) 轮询 + 一次 set_src，代价很小。
 * （demo1 用同一个值） */
#define REFRESH_MS 20

/* 底部控制条高度。预览是 640x480、从状态栏下沿开始（48+480=528），屏幕 600 高，
 * 正好剩 72px 放这一条，不会压在画面上。 */
#define CTRL_BAR_H 72

/* 保存任务：优先级低于解码器(8)和 USB 驱动，写盘慢一点没关系，别抢它们的核 */
#define SAVE_TASK_STACK 4096
#define SAVE_TASK_PRIO  3
#define SAVE_TASK_CORE  1

/* ---- 界面 + 解码器：enter 建、leave 清 ---- */
static jpeg_decode_t *s_jd         = NULL;
static lv_obj_t      *s_scr        = NULL;
static lv_obj_t      *s_image      = NULL;
static lv_obj_t      *s_status     = NULL;    /* 控制条上的状态文字 */
static lv_image_dsc_t s_dsc;
static lv_timer_t    *s_timer      = NULL;
static bool           s_frame_held = false;   /* 是否还压着一个读槽没还 */
static bool           s_saving     = false;   /* 保存中：预览冻结在按快门那一帧 */

/* ---- 常驻部分的状态（由 camera_init 设置） ---- */
static bool           s_save_ready = false;   /* 保存任务是否就绪（没就绪时快门不响应） */

/* =====================================================================
 * 拍照保存
 *
 * 原本是独立的 components/sd_card_save 组件，现并入本文件 —— 它全项目只有本 App
 * 一个使用者，而且里面的东西（IMG_%04u.jpg 这个名字格式、"扫目录取最大编号 +1"
 * 防覆盖、保存期间冻结预览）本来就是相机的业务，不是通用存储能力。
 * 真正的"能力"是 sd_card（挂载）加 POSIX 文件 API。
 *
 * 拆成"快门发信号 + 独立任务写盘"的原因：
 *   一次保存 = 扫目录 + fopen + fwrite(20~80KB) + fsync + fclose，在这块板上是
 *   几十毫秒量级。快门回调跑在 LVGL 任务里，直接写会把整个界面卡住（预览定时器、
 *   触摸响应全部顺延）。所以拆成两半：
 *     LVGL 任务：   sd_save_trigger()     非阻塞，只给一个信号量
 *     sd_save_task：取最新一帧 JPEG -> 落盘 -> 结果塞进结果队列
 *     LVGL 任务：   sd_save_get_result()  在预览刷新定时器里顺手收结果
 *
 * 被保存的数据来自 usb_camera_get_jbuf()，语义是"丢旧保新"：本任务从取到读槽
 * 到 read_done 之间，生产者发布不了新帧（新帧自动丢弃），这正好保证"写文件时
 * 那块内存不会被覆盖"——不需要额外加锁。
 * ⚠️ 也正因为这条，本任务**必须常驻**：被杀在这个区间里，读槽就永远释放不了。
 * ===================================================================== */

/* 保存结果：保存任务写完之后回传给 LVGL（界面弹提示用） */
typedef struct {
    bool ok;
    char fname[32];     /* 文件名（不含目录），如 "IMG_0001.jpg" */
} sd_save_result_t;

static frame_buf_t      *s_jbuf     = NULL;  /* usb_camera 扇出的 JPEG 双缓冲 */
static SemaphoreHandle_t s_save_sem = NULL;  /* LVGL 侧 give -> 保存任务 take */
static QueueHandle_t     s_result_q = NULL;  /* 保存任务回传结果（容量 1） */

/*
 * 扫照片目录，找现有 IMG_XXXX.jpg 的最大编号，生成下一个不冲突的路径。
 *
 * 必须是"扫目录"而不是"静态递增序号"：后者重启就归零，第二次开机拍的第一张
 * 会直接覆盖掉上次的照片（demo1 踩过这个坑）。
 *
 * 目录不存在就先建（已存在时 mkdir 返回 -1/EEXIST，忽略即可）。
 */
static int sd_next_photo_path(char *path, size_t cap)
{
    mkdir(SD_CARD_PHOTO_DIR, 0777);

    uint32_t max_idx = 0;
    DIR *dir = opendir(SD_CARD_PHOTO_DIR);
    if (dir == NULL) {
        ESP_LOGE(TAG, "opendir(%s) failed", SD_CARD_PHOTO_DIR);
        return -1;
    }

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        unsigned long idx = 0;
        if (sscanf(ent->d_name, "IMG_%lu", &idx) != 1) {
            continue;
        }
        /* FAT 的 8.3 短文件名一律存大写（.JPG），所以这里必须忽略大小写 */
        const char *dot = strrchr(ent->d_name, '.');
        if (dot == NULL || strcasecmp(dot, ".jpg") != 0) {
            continue;
        }
        if (idx > max_idx) {
            max_idx = (uint32_t)idx;
        }
    }
    closedir(dir);

    snprintf(path, cap, SD_CARD_PHOTO_DIR "/IMG_%04u.jpg", (unsigned)(max_idx + 1));
    return 0;
}

static void sd_post_result(bool ok, const char *fname)
{
    sd_save_result_t res;
    res.ok = ok;
    if (fname != NULL) {
        snprintf(res.fname, sizeof(res.fname), "%s", fname);
    } else {
        res.fname[0] = '\0';
    }
    xQueueSend(s_result_q, &res, 0);    /* 队列满（上次结果未取）就丢新结果，无妨 */
}

static void sd_save_task(void *arg)
{
    (void)arg;

    for (;;) {
        if (xSemaphoreTake(s_save_sem, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        /* 取最新一帧。注意：**所有分支都必须 read_done** ——
         * 漏了的话生产者再也发布不了新帧（丢旧保新 -> 帧被连续丢弃），画面就冻住了。 */
        uint32_t len = 0;
        uint8_t *data = frame_buf_get_read(s_jbuf, &len);
        if (len == 0) {             /* 从没收到过帧（没插摄像头）：读槽还没被写过 */
            frame_buf_read_done(s_jbuf);
            ESP_LOGW(TAG, "no frame yet, nothing to save");
            sd_post_result(false, NULL);
            continue;
        }

        char path[64];
        if (sd_next_photo_path(path, sizeof(path)) != 0) {
            frame_buf_read_done(s_jbuf);
            sd_post_result(false, NULL);
            continue;
        }

        FILE *f = fopen(path, "wb");
        if (f == NULL) {
            ESP_LOGE(TAG, "fopen %s failed", path);
            frame_buf_read_done(s_jbuf);
            sd_post_result(false, NULL);
            continue;
        }
        const size_t n = fwrite(data, 1, len, f);
        /* fsync 把数据 + FAT 表都刷下去。不刷的话拔卡/断电可能留下一个长度对、
         * 内容却是半截的文件 —— 照片存坏比存失败更讨厌。 */
        fsync(fileno(f));
        fclose(f);
        frame_buf_read_done(s_jbuf);

        ESP_LOGI(TAG, "saved %s (%u/%u bytes)", path, (unsigned)n, (unsigned)len);
        /* 回传只带文件名（不含目录），界面直接显示 */
        sd_post_result(n == len, path + strlen(SD_CARD_PHOTO_DIR) + 1);
    }
}

/* ---- 常驻：由 camera_init 调一次 ---- */
static esp_err_t sd_save_init(frame_buf_t *jbuf)
{
    if (jbuf == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    s_jbuf = jbuf;

    s_save_sem = xSemaphoreCreateBinary();
    s_result_q = xQueueCreate(1, sizeof(sd_save_result_t));
    if (s_save_sem == NULL || s_result_q == NULL) {
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreatePinnedToCore(sd_save_task, "sd_save", SAVE_TASK_STACK, NULL,
                                SAVE_TASK_PRIO, NULL, SAVE_TASK_CORE) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "save ready, photo dir: %s", SD_CARD_PHOTO_DIR);
    return ESP_OK;
}

static void sd_save_trigger(void)
{
    if (s_save_sem == NULL) {
        /* init 没成功过（只可能是内存不足）。没有这个哨兵的话下面会解引用 NULL 崩掉，
         * 而"拍了照没反应"比"按一下快门就重启"好处理得多。 */
        return;
    }
    xSemaphoreGive(s_save_sem);
}

static bool sd_save_get_result(sd_save_result_t *out)
{
    /* s_result_q 为空 = 保存任务从没起来过（sd_save_init 失败）。
     * 本函数是**无条件**被预览定时器调用的，所以这里必须挡住 ——
     * 否则 xQueueReceive(NULL, ...) 会直接崩。 */
    if (out == NULL || s_result_q == NULL) {
        return false;
    }
    return xQueueReceive(s_result_q, out, 0) == pdTRUE;
}

/* =====================================================================
 * 预览刷新
 * ===================================================================== */

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
    if (sd_save_get_result(&res)) {
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
 * 按快门。这里**只发信号**就返回，扫目录/写文件/fsync 都在 sd_save_task 里。
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
    sd_save_trigger();
}

/* =====================================================================
 * 界面：enter / leave
 * ===================================================================== */

static void camera_enter(void)
{
    ESP_LOGI(TAG, "enter");

    /* ---- 1. 解码器按需建（每次进入约 1.4MB PSRAM + 一条任务）----
     * 放在动 LVGL 之前，和下面 usb_camera_stream_request 同一种做法：
     * "先把数据源备好，再画界面"。 */
    s_jd = jpeg_decode_create(usb_camera_get_fb());
    if (s_jd != NULL && jpeg_decode_start(s_jd) != ESP_OK) {
        jpeg_decode_destroy(s_jd);
        s_jd = NULL;
    }
    if (s_jd == NULL) {
        /* ⚠️ 失败也**不能**直接 return：本函数必须保证最后有一张 screen 被 load。
         *    否则 app_manager 接着会调 desktop_leave() 把桌面那张拆掉，
         *    而这边什么都没建 —— LVGL 就没有有效 screen 了。
         *    所以下面照样建 screen，只是显示一行提示。 */
        ESP_LOGE(TAG, "解码器没起来，只显示提示");
    }

    /* ---- 2. 按需开流：这里只"表达意图"，真正的 start 在 usb_camera 的管理任务里做
     * （那边和 open/close 同一任务、天然串行；而且 start 里含控制传输和 vTaskDelay，
     * 不该让 UI 线程去等）。 ---- */
    usb_camera_stream_request(true);

    lvgl_port_lock(0);

    s_scr = lv_obj_create(NULL);

    /* 标题和返回按钮都由公共状态栏提供，本 App 不自己画 */
    const ui_status_bar_cfg_t bar = {
        .title     = "Camera",
        .show_back = true,
    };
    ui_status_bar_apply(&bar);

    if (s_jd == NULL) {
        lv_obj_t *msg = lv_label_create(s_scr);
        lv_label_set_text(msg, "Decoder unavailable");
        lv_obj_center(msg);
    } else {
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

        s_timer = lv_timer_create(on_refresh, REFRESH_MS, NULL);
    }

    s_saving = false;               /* 上次离开时可能正在保存，重新进来要复位 */
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

    /* ② 还掉可能还压着的读槽。不还的话，解码器的 commit 会一直失败
     *    （丢旧保新 -> 帧被连续丢弃），再回到这个 App 就一直没有画面了。 */
    if (s_frame_held) {
        frame_buf_read_done(jpeg_decode_get_fb(s_jd));
        s_frame_held = false;
    }

    /* ③ 拆解码器。顺序不能颠倒：s_dsc.data 指着 jd->fb 里那块内存，
     *    所以必须"先还读槽 -> 拆解码器 -> 最后异步删 screen"（和 photo.c 一致）。
     * ⚠️ jpeg_decode_destroy 内部要等解码任务自己退出（100ms 轮询一轮），实测约
     *    100ms；这一步是在**持着 LVGL 锁**的情况下等的 —— 退相机会有一瞬间的停顿。 */
    if (s_jd != NULL) {
        jpeg_decode_destroy(s_jd);
        s_jd = NULL;
    }

    lv_obj_delete_async(s_scr);
    s_scr = NULL;
    s_image = NULL;
    s_status = NULL;
    lvgl_port_unlock();
}

/* =====================================================================
 * 常驻部分（camera_init）
 * ===================================================================== */

esp_err_t camera_init(void)
{
    /* 1. USB Host + UVC 驱动 + 帧源。必须常驻 ——
     *    理由见 usb_camera.h 里 usb_camera_init() 的说明（UVC 的断开事件是流级的）。 */
    esp_err_t err = usb_camera_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "usb_camera_init failed: %s", esp_err_to_name(err));
        return err;
    }

    /* 2. 拍照保存任务。必须常驻，**不能**跟着 enter/leave 建拆：
     *    它有一条不可中断的临界区 —— 从 frame_buf_get_read(jbuf) 到 read_done
     *    之间一直持着 jbuf 的读槽（写文件期间不能让生产者覆盖那块内存）。
     *    如果它随 App 退出被杀在这个区间里，read_in_progress 会永远是 true，
     *    之后 usb_camera 往 jbuf 的每一次 commit 都失败 —— jbuf 再也不会更新，
     *    也就是"以后每次拍照存下的都是同一张旧图"，要重启才恢复。
     *
     *    （对比：解码器没有这个问题 —— 它的输入是丢旧保新的流，中途被杀零损失，
     *      所以解码器可以每次 enter 重建。这就是本文件里"谁常驻、谁动态"的分界。）
     *
     *    失败**不算** camera_init 失败：预览和拍照是两件独立的事，少了拍照不该
     *    让整个 App 打不开 —— 界面会把快门标成不可用（见 s_save_ready）。 */
    if (sd_save_init(usb_camera_get_jbuf()) != ESP_OK) {
        ESP_LOGE(TAG, "sd_save_init failed, 拍照保存不可用");
        return ESP_OK;
    }
    s_save_ready = true;
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
