/*
 * 相机 App —— USB 摄像头预览 + 拍照存 SD 卡
 *
 * 本文件里住着三块东西，**生命周期各不相同** —— 这是读它时最该先看清的地方：
 *
 *   1. 常驻（camera_init，main 在注册 App 之前调一次，之后永不拆）
 *        usb_camera  帧源。必须常驻：它按"摄像头是常驻外设"设计 —— init 时 open
 *                    一条流，之后一直挂着，直到重启（不处理拔插，详见 usb_camera.h）。
 *        sd_save     拍照保存任务。必须常驻：它写盘期间一直持着 jbuf 的读槽，
 *                    中途被杀会让读槽永远释放不了（详见 sd_save_init 的注释）。
 *
 *   2. 每次进出（camera_enter / camera_leave）
 *        jpeg_decoder 引擎 + 一块 600KB 画布。不进相机就不占；没有任务，
 *                     所以"拆"是平凡的（不像以前要等任务自己退出）。
 *        UI（一个 lv_image + 底部控制条 + 一个 20ms 定时器）
 *
 *   3. 拍照（on_shutter_clicked）只发一个信号，写盘在 sd_save_task 里做
 *
 * 第 1 / 2 的分界可以概括成一句话：
 *   **有不可中断的临界区 -> 常驻；可随时打断 -> 动态。**
 * （sd_save 的临界区 = 持着 jbuf 读槽写盘；解码器没有任何临界区。）
 *
 * 解码是**在 LVGL 定时器里同步做的**（见 on_refresh）：从 uvc_fb 取最新一帧 ->
 * 解到解码器自己那块画布 -> 上屏。输出**只需要一个槽** —— 因为生产者和消费者
 * 都是这个 LVGL 任务，而 LVGL 的重绘发生在定时器回调返回之后。
 * 真正需要双缓冲的是帧源那侧（uvc_fb，生产者是异步的驱动回调）。
 *
 * 裁剪自 demo1 的 app_camera.c。砍掉的是：自画顶栏、自定义字体与配色、圆形按钮。
 * 保留下来的是"保存中冻结预览"的做法。
 *
 * 没插摄像头时会是什么样：usb_camera_init 里那次 open 会失败（等 5s 超时），
 * camera_init 直接返回错误 -> 解码器和保存任务都不会建 -> **一片黑**。
 * 这是刻意从简（App 是验证台，不是产品）；排查时看 "UVC stream opened" 有没有出现，
 * 以及进 App 时 "stream started" 有没有跟着出现。
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
#include "jpeg_decoder.h"
#include "sd_card.h"

static const char *TAG = "camera";

/* 刷新周期。画面是 30fps(33ms)，20ms 比它快一点，保证不落帧。
 *
 * ⚠️ 定时器里现在做的是"查有没有新帧 -> 同步解码 -> 上屏"，一次要占约 4.9ms
 *    （解码本身）—— 这段时间 LVGL 任务是**被挡住**的。20ms 的周期对 15.6fps 的
 *    输入够用（平均每三次才真解一帧）。
 * （demo1 用同一个值） */
#define REFRESH_MS 20

/* 底部控制条高度。预览是 640x480、从状态栏下沿开始（48+480=528），屏幕 600 高，
 * 正好剩 72px 放这一条，不会压在画面上。 */
#define CTRL_BAR_H 72

/* 保存任务：优先级只高于 LVGL 任务，写盘慢一点没关系，别抢 USB 驱动的核 */
#define SAVE_TASK_STACK 4096
#define SAVE_TASK_PRIO  3
#define SAVE_TASK_CORE  1

/* ---- 界面 + 解码器：enter 建、leave 清 ---- */
static jpeg_decoder_t *s_jd         = NULL;
static lv_obj_t       *s_scr        = NULL;
static lv_obj_t       *s_image      = NULL;
static lv_obj_t       *s_status     = NULL;    /* 控制条上的状态文字 */
static lv_image_dsc_t  s_dsc;
static lv_timer_t     *s_timer      = NULL;
static bool            s_saving     = false;   /* 保存中：预览冻结在按快门那一帧 */

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
 * 每 20ms 一次：看看帧源有没有新帧，有就同步解一帧、上屏。
 *
 * 流程很短，但有三条约束要记住：
 *
 *  ① **解码器没起来就只空转**（s_jd == NULL）：进 App 时建失败会走这条路，
 *     界面已经显示了一行提示，定时器照跑但什么都不用做。
 *  ② **读槽只在解码期间持有，解完立刻还。** uvc_fb 是"丢旧保新"语义 ——
 *     生产者（驱动回调）只有在读槽没被占用时才能发布新帧，所以持有时间越短越好。
 *     这里**不存在**以前那个"先还槽再等帧、反了就冻住"的死锁：等帧用的是非阻塞的
 *     wait_new(0)，而且持槽期间不做任何等待。
 *  ③ **解码失败不能动上一帧**：坏帧是预期内的（源头截断 / 拼帧溢出），这时接口
 *     不会改写 out，画布还是上一帧的内容，直接让它留在屏幕上就行。
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
    if (s_jd == NULL) {
        return;                     /* 解码器没起来：界面已有提示，这里只空转 */
    }

    /* ① 帧源有没新帧？没有就保持当前画面（wait_new 是非阻塞的） */
    frame_buf_t *uvc_fb = usb_camera_get_fb();
    if (uvc_fb == NULL || !frame_buf_wait_new(uvc_fb, 0)) {
        return;
    }

    /* ② 取最新一帧，同步解码。这一下约 4.9ms（坏帧会走到引擎 15ms 的上限），
     *    期间 LVGL 任务是被挡住的 —— 已知并接受，契约见 jpeg_decoder.h。 */
    uint32_t len = 0;
    const uint8_t *jpg = frame_buf_get_read(uvc_fb, &len);
    uint8_t *out = NULL;
    const esp_err_t err = jpeg_decode_frame(s_jd, jpg, len, &out);

    /* ③ 立刻还读槽：解完就不需要这块输入了，让生产者尽早能发布下一帧 */
    frame_buf_read_done(uvc_fb);

    if (err != ESP_OK) {
        return;                     /* 坏帧：画布内容没动，继续显示上一帧 */
    }

    /* ④ 上屏。画布指针始终是同一块（内容在原地被覆盖），set_src 照样每次都调 ——
     *    它表达的就是"这一帧变了、去重绘"，而且这样不依赖"图片缓存是否关闭"这个配置。 */
    s_dsc.data = out;
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

    /* ---- 1. 解码器按需建（引擎 + 一块 600KB 画布）----
     * 放在动 LVGL 之前，和下面开流同一种做法："先把数据源备好，再画界面"。
     * 失败不致命：下面照样建 screen，只是显示一行提示。 */
    s_jd = jpeg_decode_init();
    if (s_jd == NULL) {
        ESP_LOGE(TAG, "解码器没起来，只显示提示");
    }

    /* ---- 2. 开流。⚠️ **同步阻塞**（约 15~20ms），而且我们此刻正跑在 LVGL 任务里
     * —— 这一下会卡住界面，已知并接受（契约见 usb_camera.h）。
     * 返回值要留着：下面据此决定给"预览"还是给"一行提示"。 ---- */
    const esp_err_t stream_err = usb_camera_stream_start();

    lvgl_port_lock(0);

    s_scr = lv_obj_create(NULL);

    /* 标题和返回按钮都由公共状态栏提供，本 App 不自己画 */
    const ui_status_bar_cfg_t bar = {
        .title     = "Camera",
        .show_back = true,
    };
    ui_status_bar_apply(&bar);

    /* 预览需要两样都在：解码器 + 流。任何一个没起来就只给一行提示 ——
     * 这既是给用户看的，也是排查"为什么是一片黑"时的第一个线索。 */
    if (s_jd == NULL || stream_err != ESP_OK) {
        /* ⚠️ 这里**不能**直接 return：本函数必须保证最后有一张 screen 被 load。
         *    否则 app_manager 接着会调 desktop_leave() 把桌面那张拆掉，而这边什么
         *    都没建 —— LVGL 就没有有效 screen 了。所以照样建 screen，只给提示。 */
        const char *text = "Decoder unavailable";
        if (s_jd != NULL) {
            /* 解码器没问题，是流没起来。再分两种（错误码含义见 usb_camera.h）：
             *   INVALID_STATE = init 时就没 open（相机不在场）
             *   其余          = 有流，但这次 start 失败（相机中途掉了 / 总线错） */
            text = (stream_err == ESP_ERR_INVALID_STATE) ? "No camera" : "Stream error";
        }
        lv_obj_t *msg = lv_label_create(s_scr);
        lv_label_set_text(msg, text);
        lv_obj_center(msg);
    } else {
        /* LVGL 不认解码器的画布，得手工把"一块 RGB565 像素"描述出来。
         * 尺寸就是解码器的输出尺寸；画布指针自始至终是同一块（内容原地更新），
         * 而 data 要等第一次成功解码才有值（见 on_refresh）。 */
        s_dsc.header.magic  = LV_IMAGE_HEADER_MAGIC;
        s_dsc.header.cf     = LV_COLOR_FORMAT_RGB565;
        s_dsc.header.w      = JPEG_DEC_FRAME_W;
        s_dsc.header.h      = JPEG_DEC_FRAME_H;
        s_dsc.header.stride = JPEG_DEC_FRAME_W * JPEG_DEC_FRAME_PIXEL;
        s_dsc.data_size     = JPEG_DEC_FRAME_BUF_SIZE;

        /* ⚠️ 这里**不**给 s_image 设 src —— 画布要等第一次成功解码才拿得到（接口在
         *    jpeg_decode_frame 里给）。空 src 的 lv_image 什么都不画、不会崩，
         *    所以不再需要以前那套"先借一次读槽当初始画面"。
         *    尺寸显式设出来，定位就不依赖"src 什么时候到位"了。 */
        s_image = lv_image_create(s_scr);
        lv_obj_set_size(s_image, JPEG_DEC_FRAME_W, JPEG_DEC_FRAME_H);
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

    /* 关流：用户已经不看了，就不该继续让摄像头推流、让解码器空跑。
     * 同样**同步阻塞**（stop 约 100ms）—— 退出相机 App 时这一下会卡住界面。
     * 返回值**故意忽略**：停不掉也没什么可做的（本组件不重连）。 */
    usb_camera_stream_stop();

    lvgl_port_lock(0);

    /* ① 定时器挂在 LVGL 全局的定时器链上，**不属于 s_scr** —— 不删的话它会在
     *    screen 释放之后继续跑，回调里去操作已释放的对象。
     *    和 clock.c 里那个每秒定时器是同一个坑。 */
    if (s_timer != NULL) {
        lv_timer_delete(s_timer);
        s_timer = NULL;
    }

    /* ② 拆解码器：引擎和那块 600KB 画布一起释放。这里**没有等待** ——
     *    同步解码没有任务，"拆"就是两个 free。
     *
     *    安全性来自 app_manager 的切换顺序（enter(新) → leave(旧)）：走到这里时
     *    相机这张 screen 已经不在台面上了，不会再有人去画那块画布。
     *
     *    以前那条"先还读槽 -> 再拆解码器"的顺序纪律也不需要了：输出不再是
     *    frame_buf，没有"读槽"这回事（输入侧的读槽在 on_refresh 里当场就还了）。 */
    if (s_jd != NULL) {
        jpeg_decode_deinit(s_jd);
        s_jd = NULL;
    }
    s_dsc.data = NULL;              /* 画布已经没了，别留一个指向已释放内存的指针 */

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
    /* 1. USB Host + UVC 驱动 + 帧源。这里会**同步等摄像头就位**并 open 那唯一一条
     *    流（最多 5s），失败即返回错误 —— 本组件不重连，详见 usb_camera.h。 */
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
