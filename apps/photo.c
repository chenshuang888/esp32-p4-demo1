/*
 * 相册 App —— 看 SD 卡里拍过的照片 + 播 SD 卡里录的视频
 *
 * ⚠️ 定位和相机 App 一样是**验证台**，不是相册产品。它要证明两条链路：
 *      SD 读文件 -> JPEG 解码 -> 上屏                  （照片）
 *      SD 读 AVI -> 解封装取帧 -> JPEG 解码 -> 上屏     （视频）
 *    所以只有一列文件名 + 点开看/播，没有缩略图、没有翻页、没有删除、没有进度条，
 *    也没有"未插卡"之类的引导（列不出来就是空列表 + 一行提示）。
 *
 * 真正的活都在现成接口上，本 App 只是把它们接起来：
 *   POSIX        读文件（sd_card 组件挂上 FATFS 之后直接 fopen）
 *   avi_reader   解封装（同步接口，把 AVI 里的一帧帧 JPEG 顺序取出来）
 *   jpeg_decoder 解码（同步一次性接口，不关心帧从哪来）
 *
 * 照片：把文件字节读进缓冲 -> jpeg_decode_frame() -> 画布交给 lv_image，不多一份拷贝。
 * 视频：用 lv_timer 按 AVI 头里声明的 fps 节拍，每 tick 取一帧、解一帧、上屏。
 *       **同步做在 LVGL 任务里** —— 一帧 ≈ 读 65KB(~21ms) + 解码(~5ms) ≈ 26ms，
 *       而帧间隔 70ms（14fps），只占 37%，所以异步是纯开销（讨论记录见项目记忆）。
 *
 * ⚠️ 读取缓冲**必须 128B 对齐**（这里以前是 heap_caps_malloc，是错的）：
 *    ESP32-P4 的 SD 盘层在源缓冲不满足 DMA 对齐时会**静默**把一次多块读拆成
 *    512B 单块读，速度掉约 20 倍。详见 avi_writer.c 顶部注释 / README 坑位表。
 *
 * 输入为什么不需要 frame_buf：那个类型的价值在于"生产者和消费者会并发"，而这里
 * 读写都在 LVGL 任务里、一读一解就结束了。
 *
 * 生命周期：解码引擎 + 600KB 画布，在 enter 建、leave 销毁；播放会话更短
 * （avi_reader + 播放定时器），点开建、回列表或 leave 时停掉。没有任务，
 * 所以销毁是平凡的。
 */
#include "photo.h"

#include <dirent.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"

#include "app_manager.h"
#include "avi_reader.h"
#include "jpeg_decoder.h"
#include "picture.h"
#include "sd_card.h"
#include "ui_status_bar.h"

static const char *TAG = "photo";

#define PHOTO_MAX         64      /* 一次最多列出多少项；先不做分页，超出的直接不列 */
#define PHOTO_NAME_MAX    64
/* 单张 JPEG 的最大字节数：相机拍出来实测 20~80KB，80000 覆盖波动尖峰。
 * 这是相册**输入缓冲**的尺寸，和拍照侧 jbuf 的槽大小是两个独立的量，各自定义。
 * 取 80000 而不是 80*1024，是为了和 PHOTO_DMA_ALIGN 成整数倍（80000 = 625 × 128）。 */
#define PHOTO_SLOT_SIZE   80000

/* 读缓冲的对齐要求（硬要求，不是调优项）：ESP32-P4 的 SD 盘层在缓冲不满足 DMA 对齐时
 * 会**静默**把一次多块读拆成 512B 单块读，速度掉约 20 倍。见文件头注释。 */
#define PHOTO_DMA_ALIGN   128

/* ---- 界面：enter 建、leave 清 ---- */
static lv_obj_t *s_scr    = NULL;
static lv_obj_t *s_list   = NULL;   /* 文件名列表 */
static lv_obj_t *s_imgbox = NULL;   /* 看图 / 播放页容器：图在里面居中 */
static lv_obj_t *s_image  = NULL;
static lv_obj_t *s_status = NULL;   /* 底部一行：张数 / 文件名 / 出错原因 */

/* ---- 解码链路：enter 建、leave 销毁 ---- */
static uint8_t        *s_in  = NULL;    /* 读文件用的 JPEG 输入缓冲（128B 对齐的 PSRAM） */
static jpeg_decoder_t *s_jd  = NULL;    /* 解码器（引擎 + 它自己那块输出画布） */
static lv_image_dsc_t  s_dsc;

/* ---- 播放会话：点开视频建、回列表 / leave 停 ---- */
typedef enum { V_IDLE = 0, V_PLAYING, V_PAUSED, V_ENDED } vstate_t;

static avi_reader_t *s_rd     = NULL;   /* 当前打开的视频 */
static lv_timer_t   *s_vtimer = NULL;   /* 播放节拍，周期 = 1000 / fps */
static vstate_t      s_vstate = V_IDLE;
static int           s_vindex = -1;     /* 正在播的列表下标（恢复提示文字用） */

/* 扫描结果。文件名整份留在这里，点击时按下标去取 —— 行对象的 user_data 只存下标。
 * s_is_video 与 s_names 一一对应（同下标同类型），决定点开是看图还是播放。 */
static char s_names[PHOTO_MAX][PHOTO_NAME_MAX];
static bool s_is_video[PHOTO_MAX];
static int  s_count = 0;

/*
 * 扫照片目录，把 .jpg / .avi 的文件名收进 s_names（并在 s_is_video 里记下类型）。
 *
 * 不排序：readdir 给的是目录里的原始顺序，也就是拍下来的先后顺序 ——
 * 对"看拍过/录过的东西"来说这正好是想要的顺序，不用再排一遍。
 */
static void photo_scan(void)
{
    s_count = 0;
    int n_photo = 0, n_video = 0;

    DIR *dir = opendir(SD_CARD_PHOTO_DIR);
    if (dir == NULL) {
        ESP_LOGW(TAG, "opendir(%s) failed (没插卡？目录还没建？)", SD_CARD_PHOTO_DIR);
        return;
    }

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (s_count >= PHOTO_MAX) {
            ESP_LOGW(TAG, "more than %d files, rest ignored", PHOTO_MAX);
            break;
        }
        /* FAT 的 8.3 短文件名一律存大写（.JPG / .AVI），所以这里必须忽略大小写 */
        const char *dot = strrchr(ent->d_name, '.');
        if (dot == NULL) {
            continue;
        }
        bool is_vid;
        if (strcasecmp(dot, ".jpg") == 0) {
            is_vid = false;
        } else if (strcasecmp(dot, ".avi") == 0) {
            is_vid = true;
        } else {
            continue;
        }
        /* %.63s：ent->d_name 是 char[256]，不限定精度的话 GCC 会算出
         * "最多 255 字节写进 64 字节数组" 并以 -Werror=format-truncation 报错。
         * 63 = PHOTO_NAME_MAX - 1，留一个字节给结尾符。
         * 代价是超长文件名会被截断（名字变短 -> fopen 失败 -> 界面上报 "Open failed"），
         * 但对"相机自己拍出来的 IMG_XXXX.jpg / VID_XXXX.avi"来说永远不会碰到。 */
        snprintf(s_names[s_count], PHOTO_NAME_MAX, "%.63s", ent->d_name);
        s_is_video[s_count] = is_vid;
        s_count++;
        if (is_vid) {
            n_video++;
        } else {
            n_photo++;
        }
    }
    closedir(dir);
    ESP_LOGI(TAG, "found %d jpg + %d avi in %s", n_photo, n_video, SD_CARD_PHOTO_DIR);
}

/* 列表页那行提示：没有东西时把目录打出来，方便判断是"没拍"还是"卡没挂上" */
static void photo_show_list_status(void)
{
    if (s_count == 0) {
        lv_label_set_text(s_status, "No files in " SD_CARD_PHOTO_DIR);
    } else {
        lv_label_set_text_fmt(s_status, "%d files", s_count);
    }
}

/* =====================================================================
 * 视频播放
 *
 * 同步做在 LVGL 任务里，用 lv_timer 控速（周期 = AVI 头声明的 fps）。
 * 每 tick：取一帧 -> 同步解码 -> 上屏。一帧约 26ms 而帧间隔约 70ms，定时器不会被
 * 工作拖住（LVGL 以"上次实际运行时刻"为基准重新计时）。
 *
 * 状态机（点画面切换）：
 *     V_PLAYING --点--> V_PAUSED --点--> V_PLAYING
 *         |
 *         +-- 读到文件尾 --> V_ENDED --点--> 回列表（photo_show_list 里把会话停掉）
 *
 * ⚠️ 播放会话（avi_reader + 定时器）不属于任何 screen，screen 被删不会带走它们 ——
 *    回列表和 leave 都必须显式 video_stop()（和 clock.c 那个定时器是同一条纪律）。
 * ===================================================================== */

/* 停掉播放会话：删定时器 + 关文件 + 释放缓冲。幂等，没在播时是空操作。 */
static void video_stop(void)
{
    if (s_vtimer != NULL) {
        lv_timer_delete(s_vtimer);
        s_vtimer = NULL;
    }
    if (s_rd != NULL) {
        avi_reader_close(s_rd);
        s_rd = NULL;
    }
    s_vstate = V_IDLE;
    s_vindex = -1;
}

/* 播放结束（正常播完或出错）：停住节拍，停在当前这一帧上。
 * ⚠️ 用 pause 而不是 delete —— 本函数可能正跑在该定时器的回调里，删自己不保险。 */
static void video_finish(const char *msg)
{
    if (s_vtimer != NULL) {
        lv_timer_pause(s_vtimer);
    }
    s_vstate = V_ENDED;
    lv_label_set_text(s_status, msg);
}

/* 播放节拍。跑在 LVGL 任务里（定时器回调，已持锁），所以里面不要再加 LVGL 锁。 */
static void on_video_tick(lv_timer_t *t)
{
    (void)t;
    if (s_vstate != V_PLAYING || s_rd == NULL || s_jd == NULL) {
        return;
    }

    const uint8_t *jpg = NULL;
    size_t len = 0;
    const esp_err_t err = avi_reader_next(s_rd, &jpg, &len);
    if (err == ESP_ERR_NOT_FOUND) {                 /* 正常播完 */
        video_finish("End");
        return;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "avi_reader_next failed: %s", esp_err_to_name(err));
        video_finish("Read failed");
        return;
    }

    /* 同步解码。坏帧是预期内的（文件截断 / 损坏），这时接口不改写画布，
     * 直接跳过这一帧、留着上一帧 —— 和相机预览是同一个取舍。 */
    uint8_t *out = NULL;
    if (jpeg_decode_frame(s_jd, jpg, len, &out) != ESP_OK) {
        return;
    }
    s_dsc.data = out;
    lv_image_set_src(s_image, &s_dsc);
}

/* 点开一个 .avi：建会话、起节拍、立刻画第一帧 */
static void video_start(int idx)
{
    if (s_jd == NULL || idx < 0 || idx >= s_count) {
        return;                     /* 解码器没起来（见 enter 的失败分支） */
    }
    video_stop();                   /* 保险：清掉上一次可能残留的会话 */

    char path[96];
    snprintf(path, sizeof(path), SD_CARD_PHOTO_DIR "/%s", s_names[idx]);

    s_rd = avi_reader_open(path);
    if (s_rd == NULL) {
        lv_label_set_text(s_status, "Open failed");
        return;
    }

    avi_info_t info;
    if (!avi_reader_get_info(s_rd, &info)) {
        ESP_LOGE(TAG, "%s: no avih, can't get fps/size", path);
        lv_label_set_text(s_status, "Bad AVI header");
        video_stop();
        return;
    }

    /* 解码器的输出尺寸是编译期写死的（JPEG_DEC_FRAME_W/H），喂别的尺寸会静默画错，
     * 所以这里直接把不支持的文件挡掉。 */
    if (info.width != JPEG_DEC_FRAME_W || info.height != JPEG_DEC_FRAME_H) {
        ESP_LOGE(TAG, "%s: %ux%u unsupported (only %dx%d)",
                 path, (unsigned)info.width, (unsigned)info.height,
                 JPEG_DEC_FRAME_W, JPEG_DEC_FRAME_H);
        lv_label_set_text(s_status, "Unsupported size");
        video_stop();
        return;
    }

    uint32_t fps = info.fps ? info.fps : 15;        /* 头里没写就按 15 兜底 */
    if (fps < 1) {
        fps = 1;
    } else if (fps > 60) {
        fps = 60;
    }

    s_vstate = V_PLAYING;
    s_vindex = idx;
    s_vtimer = lv_timer_create(on_video_tick, 1000 / fps, NULL);
    if (s_vtimer == NULL) {
        ESP_LOGE(TAG, "lv_timer_create failed");
        lv_label_set_text(s_status, "No timer");
        video_stop();
        return;
    }

    lv_obj_remove_flag(s_imgbox, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_list, LV_OBJ_FLAG_HIDDEN);

    /* 先画第一帧，免得空等一个周期 */
    on_video_tick(NULL);
    if (s_vstate == V_PLAYING) {
        lv_label_set_text(s_status, s_names[idx]);
    }

    ESP_LOGI(TAG, "play %s (%ux%u @%ufps)",
             path + strlen(SD_CARD_PHOTO_DIR) + 1,
             (unsigned)info.width, (unsigned)info.height, (unsigned)fps);
}

/* 点画面：播放 <-> 暂停（已结束的状态由 on_image_clicked 处理成"回列表"） */
static void video_toggle_pause(void)
{
    if (s_vstate == V_PLAYING) {
        s_vstate = V_PAUSED;
        if (s_vtimer != NULL) {
            lv_timer_pause(s_vtimer);
        }
        lv_label_set_text(s_status, "Paused");
    } else if (s_vstate == V_PAUSED) {
        s_vstate = V_PLAYING;
        if (s_vtimer != NULL) {
            lv_timer_resume(s_vtimer);
        }
        lv_label_set_text(s_status, (s_vindex >= 0) ? s_names[s_vindex] : "");
    }
}

/*
 * 回到列表页。
 *
 * 先停掉可能正在跑的播放会话（幂等），再切两个容器的显示/隐藏。屏幕上那张图仍然
 * 指着解码器的画布，但画布归解码器所有、且这一轮不会再被写，所以不需要"还槽"。
 */
static void photo_show_list(void)
{
    video_stop();

    lv_obj_add_flag(s_imgbox, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_list, LV_OBJ_FLAG_HIDDEN);
    photo_show_list_status();
}

/* 点列表里的一项：读文件 -> 交给解码器 -> 把结果贴到看图控件上 */
static void photo_show(int idx)
{
    if (s_jd == NULL) {
        return;                     /* 解码器没起来（见 enter 里的失败分支） */
    }
    if (idx < 0 || idx >= s_count) {
        return;
    }

    char path[96];
    snprintf(path, sizeof(path), SD_CARD_PHOTO_DIR "/%s", s_names[idx]);

    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        ESP_LOGE(TAG, "fopen %s failed", path);
        lv_label_set_text(s_status, "Open failed");
        return;
    }
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0 || size > PHOTO_SLOT_SIZE) {
        /* 超出输入槽就没法解码。相机拍出来是 20~80KB，只有"这张不是相机拍的"
         * 才会撞到这里 —— 如实报出来，别装作能看。 */
        ESP_LOGE(TAG, "%s size %ld out of slot (%d)", path, size, PHOTO_SLOT_SIZE);
        fclose(f);
        lv_label_set_text(s_status, "File size out of range");
        return;
    }
    const size_t n = fread(s_in, 1, (size_t)size, f);
    fclose(f);
    if (n != (size_t)size) {
        ESP_LOGE(TAG, "%s short read: %u/%ld", path, (unsigned)n, size);
        lv_label_set_text(s_status, "Read failed");
        return;
    }

    /* 同步解码。⚠️ 这一下会挡住 LVGL 任务约 4.9ms（坏帧会走到引擎 15ms 的上限）——
     *    已知并接受，契约见 jpeg_decoder.h。
     *    失败时不改写 out，屏幕上前一张图原样留着，所以这里不用管。 */
    uint8_t *out = NULL;
    if (jpeg_decode_frame(s_jd, s_in, n, &out) != ESP_OK) {
        lv_label_set_text(s_status, "Decode failed");
        return;
    }

    /* 上屏：拿到的是解码器画布里的**那一块**地址，不是拷贝 */
    s_dsc.data = out;
    lv_image_set_src(s_image, &s_dsc);

    lv_obj_remove_flag(s_imgbox, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_list, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(s_status, s_names[idx]);
}

/*
 * 点列表中的一行。user_data 里存的是**下标本身**（把值转成指针），
 * 不是 &i —— 循环里的局部变量出了循环就失效了，传地址会踩内存。
 * 按下标查类型：照片看图，视频播放。
 */
static void on_photo_clicked(lv_event_t *e)
{
    const int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= s_count) {
        return;
    }
    if (s_is_video[idx]) {
        video_start(idx);
    } else {
        photo_show(idx);
    }
}

/*
 * 点画面本身：
 *   播放中 / 暂停中 -> 暂停 / 继续（视频）
 *   其它（看照片、或视频已播完）-> 回列表
 */
static void on_image_clicked(lv_event_t *e)
{
    (void)e;
    if (s_vstate == V_PLAYING || s_vstate == V_PAUSED) {
        video_toggle_pause();
        return;
    }
    photo_show_list();
}

static void photo_enter(void)
{
    ESP_LOGI(TAG, "enter");
    lvgl_port_lock(0);

    s_scr = lv_obj_create(NULL);

    /* 标题和返回按钮都由公共状态栏提供（返回 = 回桌面），本 App 不自己画 */
    const ui_status_bar_cfg_t bar = {
        .title     = "Photos",
        .show_back = true,
    };
    ui_status_bar_apply(&bar);

    /*
     * 布局：body 铺满整屏，只用 padding 顶部让出状态栏的高度，里面竖排三样 ——
     * 列表 / 看图容器 / 状态文字。列表和看图容器都 flex_grow=1，
     * 谁被隐藏另一个就自动占满（flex 会跳过隐藏的子项），所以都不用手算高度。
     */
    lv_obj_t *body = lv_obj_create(s_scr);
    lv_obj_remove_style_all(body);          /* 布局要在这之后设：remove_style_all 会把 layout 一起清掉 */
    lv_obj_set_size(body, lv_pct(100), lv_pct(100));
    lv_obj_set_style_pad_top(body, UI_STATUS_BAR_HEIGHT, 0);
    lv_obj_set_style_pad_row(body, 0, 0);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);

    s_list = lv_list_create(body);
    lv_obj_set_width(s_list, lv_pct(100));
    lv_obj_set_flex_grow(s_list, 1);

    s_imgbox = lv_obj_create(body);
    lv_obj_remove_style_all(s_imgbox);
    lv_obj_set_width(s_imgbox, lv_pct(100));
    lv_obj_set_flex_grow(s_imgbox, 1);

    s_status = lv_label_create(body);
    lv_label_set_text(s_status, "");

    /* ---- 解码链路：本 App 建输入缓冲 -> 解码器（它自己带输出画布）----
     * ⚠️ 输入缓冲必须 128B 对齐：它是 fread 的落点，而 ESP32-P4 的 SD 盘层在缓冲
     *    不满足 DMA 对齐时会**静默**把一次多块读拆成 512B 单块读（慢约 20 倍）。 */
    s_in = heap_caps_aligned_alloc(PHOTO_DMA_ALIGN, PHOTO_SLOT_SIZE, MALLOC_CAP_SPIRAM);
    s_jd = jpeg_decode_init();

    if (s_in == NULL || s_jd == NULL) {
        ESP_LOGE(TAG, "解码链路没起来，列表能看但点开没反应");
        /* 半成品也收干净：不留任何资源 */
        if (s_jd != NULL) {
            jpeg_decode_deinit(s_jd);
            s_jd = NULL;
        }
        if (s_in != NULL) {
            heap_caps_free(s_in);
            s_in = NULL;
        }
    } else {
        s_dsc.header.magic  = LV_IMAGE_HEADER_MAGIC;
        s_dsc.header.cf     = LV_COLOR_FORMAT_RGB565;
        s_dsc.header.w      = JPEG_DEC_FRAME_W;
        s_dsc.header.h      = JPEG_DEC_FRAME_H;
        s_dsc.header.stride = JPEG_DEC_FRAME_W * JPEG_DEC_FRAME_PIXEL;
        s_dsc.data_size     = JPEG_DEC_FRAME_BUF_SIZE;

        /* ⚠️ 这里**不**给 s_image 设 src —— 画布要等第一次成功解码才拿得到。
         *    空 src 的 lv_image 什么都不画、不会崩，所以不再需要以前那套
         *    "先借一次读槽当初始画面"。尺寸显式设出来，居中就不依赖 src 何时到位。 */
        s_image = lv_image_create(s_imgbox);
        lv_obj_set_size(s_image, JPEG_DEC_FRAME_W, JPEG_DEC_FRAME_H);
        /* 640×480 原样、不缩放，居中放在看图容器里（和相机 App 同一个取舍） */
        lv_obj_center(s_image);
        lv_obj_add_flag(s_image, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(s_image, on_image_clicked, LV_EVENT_CLICKED, NULL);
    }

    /* 初始显示列表页 */
    lv_obj_add_flag(s_imgbox, LV_OBJ_FLAG_HIDDEN);

    /* 播放状态复位。leave 理应已经停干净（s_rd / s_vtimer 都是 NULL），这里是双保险 */
    s_vstate = V_IDLE;
    s_vindex = -1;

    /* ---- 填列表 ---- */
    photo_scan();
    for (int i = 0; i < s_count; i++) {
        lv_obj_t *row = lv_list_add_button(s_list, NULL, s_names[i]);
        lv_obj_add_event_cb(row, on_photo_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
    photo_show_list_status();

    lv_screen_load(s_scr);
    lvgl_port_unlock();
}

static void photo_leave(void)
{
    ESP_LOGI(TAG, "leave");
    lvgl_port_lock(0);

    /* ① 先停播放会话（删定时器 + 关文件 + 释放缓冲）。**必须在拆解码器之前** ——
     *    定时器回调要用解码器，反过来的话若定时器还在跑就会踩已释放的引擎。
     *    也和 clock.c 那条纪律一致：游离于 screen 之外的资源，leave 里一个都不能漏。 */
    video_stop();

    /* ② 拆解码链路：引擎 + 600KB 画布一起释放，**没有等待**（同步解码没有任务）。
     *    安全性来自 app_manager 的顺序（enter(新) → leave(旧)）：走到这里时相册
     *    这张 screen 已经不在台面上了，不会再有人去画那块画布。
     *    以前那条"先还读槽再拆"的顺序纪律不需要了 —— 输出不再是 frame_buf。 */
    if (s_jd != NULL) {
        jpeg_decode_deinit(s_jd);
        s_jd = NULL;
    }
    s_dsc.data = NULL;                  /* 画布已经没了，别留野指针 */
    if (s_in != NULL) {                 /* 输入缓冲是本 App 建的，由本 App 释放 */
        heap_caps_free(s_in);
        s_in = NULL;
    }

    /* ③ 最后删 screen。必须异步：本回调可能正跑在 screen 内对象的事件上 */
    lv_obj_delete_async(s_scr);
    s_scr = NULL;
    s_list = NULL;
    s_imgbox = NULL;
    s_image = NULL;
    s_status = NULL;

    lvgl_port_unlock();
}

static const app_desc_t s_desc = {
    .name  = "Photos",
    .icon  = PICTURE_ICON_PHOTO,
    .enter = photo_enter,
    .leave = photo_leave,
};

void photo_register(void)
{
    app_manager_register(&s_desc);
}
