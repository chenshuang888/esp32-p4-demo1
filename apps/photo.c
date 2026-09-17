/*
 * 相册 App —— 看 SD 卡里拍过的照片
 *
 * ⚠️ 定位和相机 App 一样是**验证台**，不是相册产品：它要证明的是
 *    "SD 读文件 -> JPEG 解码 -> 上屏"这条链路通。
 *    所以只有一列文件名 + 点开看一张，没有缩略图、没有翻页、没有删除，
 *    也没有"未插卡"之类的引导（列不出来就是空列表 + 一行提示）。
 *
 * 真正的活都在两个现成接口上，本 App 只是把它们接起来：
 *   POSIX       读文件（sd_card 组件挂上 FATFS 之后直接 fopen）
 *   jpeg_decoder 解码（它本来就是按"上游可以是 USB 相机，也可以是 SD 相册"设计的）
 *
 * 解码链路是这样接的：本 App 自己建一个输入 frame_buf，把文件字节读进去 commit，
 * 解码器任务解完把 RGB565 写进它的输出 frame_buf，这里取读槽直接交给 lv_image ——
 * 中间不多一份拷贝。
 *
 * 生命周期：解码器（含一条任务、约 1.7MB PSRAM）在 enter 建、leave 销毁。
 * 换来的是不进相册时不占这些资源；相机 App 那条解码器是常驻的，两者互不干扰
 * （各自有独立的输入 frame_buf 和引擎）。
 */
#include "photo.h"

#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"

#include "app_manager.h"
#include "frame_buf.h"
#include "jpeg_decode_task.h"
#include "picture.h"
#include "sd_card.h"
#include "ui_status_bar.h"

static const char *TAG = "photo";

#define PHOTO_MAX         64      /* 一次最多列出多少张；先不做分页，超出的直接不列 */
#define PHOTO_NAME_MAX    64
#define PHOTO_SLOT_SIZE   JPEG_BUF_SLOT_SIZE   /* 单张 JPEG 最大字节数，和拍照侧同一个来源 */
#define DECODE_TIMEOUT_MS 200     /* 解码典型 5ms；超时说明这帧解不出来（不是 JPEG / 太大） */

/* ---- 界面：enter 建、leave 清 ---- */
static lv_obj_t *s_scr    = NULL;
static lv_obj_t *s_list   = NULL;   /* 文件名列表 */
static lv_obj_t *s_imgbox = NULL;   /* 看图页容器：图在里面居中 */
static lv_obj_t *s_image  = NULL;
static lv_obj_t *s_status = NULL;   /* 底部一行：张数 / 文件名 / 出错原因 */

/* ---- 解码链路：enter 建、leave 销毁 ---- */
static frame_buf_t   *s_in  = NULL;     /* 本 App 建，喂给解码器的 JPEG 输入槽 */
static frame_buf_t   *s_out = NULL;     /* 解码器的 RGB565 输出槽（借来的，不用释放） */
static jpeg_decode_t *s_jd  = NULL;
static lv_image_dsc_t s_dsc;
static bool           s_frame_held = false;

/* 扫描结果。文件名整份留在这里，点击时按下标去取 —— 行对象的 user_data 只存下标 */
static char s_names[PHOTO_MAX][PHOTO_NAME_MAX];
static int  s_count = 0;

/*
 * 扫照片目录，把 .jpg 的文件名收进 s_names。
 *
 * 不排序：readdir 给的是目录里的原始顺序，也就是拍下来的先后顺序 ——
 * 对"看拍过的照片"来说这正好是想要的那个顺序，不用再排一遍。
 */
static void photo_scan(void)
{
    s_count = 0;

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
        /* FAT 的 8.3 短文件名一律存大写（.JPG），所以这里必须忽略大小写 */
        const char *dot = strrchr(ent->d_name, '.');
        if (dot == NULL || strcasecmp(dot, ".jpg") != 0) {
            continue;
        }
        /* %.63s：ent->d_name 是 char[256]，不限定精度的话 GCC 会算出
         * "最多 255 字节写进 64 字节数组" 并以 -Werror=format-truncation 报错。
         * 63 = PHOTO_NAME_MAX - 1，留一个字节给结尾符。
         * 代价是超长文件名会被截断（名字变短 -> fopen 失败 -> 界面上报 "Open failed"），
         * 但对"相机自己拍出来的 IMG_XXXX.jpg"来说永远不会碰到。 */
        snprintf(s_names[s_count], PHOTO_NAME_MAX, "%.63s", ent->d_name);
        s_count++;
    }
    closedir(dir);
    ESP_LOGI(TAG, "found %d jpg file(s) in %s", s_count, SD_CARD_PHOTO_DIR);
}

/* 列表页那行提示：没照片时把目录打出来，方便判断是"没拍"还是"卡没挂上" */
static void photo_show_list_status(void)
{
    if (s_count == 0) {
        lv_label_set_text(s_status, "No photos in " SD_CARD_PHOTO_DIR);
    } else {
        lv_label_set_text_fmt(s_status, "%d photos", s_count);
    }
}

/*
 * 回到列表页。
 *
 * ⚠️ 这里刻意**不**释放解码输出的读槽：屏幕上那张图还指着它的内存，
 *    现在还回去，解码器下一次 commit 就会往这块内存里写新数据。
 *    改成"下次真要解码之前才还"（见 photo_show 里的第 ① 步）—— 和相机 App
 *    的持帧纪律一致，也保证任何时刻"屏幕上那一帧"都是被占住的。
 */
static void photo_show_list(void)
{
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
    uint8_t *slot = frame_buf_get_write(s_in);
    const size_t n = fread(slot, 1, (size_t)size, f);
    fclose(f);
    if (n != (size_t)size) {
        ESP_LOGE(TAG, "%s short read: %u/%ld", path, (unsigned)n, size);
        lv_label_set_text(s_status, "Read failed");
        return;
    }

    /* ① 先还上一帧的读槽：不还的话解码器发布不了新结果（commit 失败），下面必然超时。
     *    还在这里是安全的 —— 屏幕上那张图马上就被新图替换掉，
     *    而 LVGL 的重绘在我们返回之后才发生，中间不会有人去画已释放的那块内存。 */
    if (s_frame_held) {
        frame_buf_read_done(s_out);
        s_frame_held = false;
    }

    /* ② 把文件字节交给解码器，然后等它出结果。
     *    commit 返回 false = 解码器还占着输入读槽（上一帧没消化完），本帧被丢了。 */
    if (!frame_buf_commit(s_in, (uint32_t)n)) {
        lv_label_set_text(s_status, "Decoder busy");
        return;
    }
    if (!frame_buf_wait_new(s_out, DECODE_TIMEOUT_MS)) {
        lv_label_set_text(s_status, "Decode failed");
        return;
    }

    /* ③ 取读槽上屏。注意拿到的是解码器输出缓冲里的**那一块**地址，不是拷贝 */
    s_dsc.data = frame_buf_get_read(s_out, NULL);
    s_frame_held = true;
    lv_image_set_src(s_image, &s_dsc);

    lv_obj_remove_flag(s_imgbox, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_list, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(s_status, s_names[idx]);
}

/*
 * 点列表中的一行。user_data 里存的是**下标本身**（把值转成指针），
 * 不是 &i —— 循环里的局部变量出了循环就失效了，传地址会踩内存。
 */
static void on_photo_clicked(lv_event_t *e)
{
    photo_show((int)(intptr_t)lv_event_get_user_data(e));
}

/* 点画面本身回列表页 */
static void on_image_clicked(lv_event_t *e)
{
    (void)e;
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

    /* ---- 解码链路：本 App 建输入槽 -> 解码器 -> 它的输出槽 ---- */
    const frame_buf_cfg_t in_cfg = {
        .slot_size = PHOTO_SLOT_SIZE,
        .has_len   = true,          /* 解码器要知道这一帧 JPEG 有多少字节 */
    };
    s_in = frame_buf_create(&in_cfg);
    if (s_in != NULL) {
        s_jd = jpeg_decode_create(s_in);
    }
    if (s_jd != NULL && jpeg_decode_start(s_jd) != ESP_OK) {
        jpeg_decode_destroy(s_jd);
        s_jd = NULL;
    }
    s_out = (s_jd != NULL) ? jpeg_decode_get_fb(s_jd) : NULL;

    if (s_out != NULL) {
        s_dsc.header.magic  = LV_IMAGE_HEADER_MAGIC;
        s_dsc.header.cf     = LV_COLOR_FORMAT_RGB565;
        s_dsc.header.w      = JPEG_DEC_FRAME_W;
        s_dsc.header.h      = JPEG_DEC_FRAME_H;
        s_dsc.header.stride = JPEG_DEC_FRAME_W * JPEG_DEC_FRAME_PIXEL;
        s_dsc.data_size     = JPEG_DEC_FRAME_BUF_SIZE;

        /* ⚠️ data 初值必须指向**有效内存**（哪怕内容是黑的），否则 LVGL 绘制空 data
         *    会崩。做法照抄相机 App：先借一次读槽当初始画面，立刻还回去。 */
        s_dsc.data = frame_buf_get_read(s_out, NULL);
        frame_buf_read_done(s_out);
        s_frame_held = false;

        s_image = lv_image_create(s_imgbox);
        lv_image_set_src(s_image, &s_dsc);
        /* 640×480 原样、不缩放，居中放在看图容器里（和相机 App 同一个取舍） */
        lv_obj_center(s_image);
        lv_obj_add_flag(s_image, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(s_image, on_image_clicked, LV_EVENT_CLICKED, NULL);
    } else {
        ESP_LOGE(TAG, "decoder unavailable, 列表能看但点开没反应");
    }

    /* 初始显示列表页 */
    lv_obj_add_flag(s_imgbox, LV_OBJ_FLAG_HIDDEN);

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

    /* ① 先还掉可能还压着的读槽，再拆解码链路 —— 顺序反了的话 frame_buf_delete
     *    之后 s_out 就是野指针了。 */
    if (s_frame_held) {
        frame_buf_read_done(s_out);
        s_frame_held = false;
    }
    if (s_jd != NULL) {
        /* 内部会等解码任务自己退出（它 100ms 一轮轮询），实测约 100ms。
         * 这一步是在持着 LVGL 锁的情况下等的 —— 退相册会有一瞬间的停顿，
         * 但换来的是"引擎和两个输出缓冲都真的释放了"。 */
        jpeg_decode_destroy(s_jd);
        s_jd = NULL;
    }
    s_out = NULL;
    if (s_in != NULL) {                 /* 输入槽是本 App 建的，由本 App 释放 */
        frame_buf_delete(s_in);
        s_in = NULL;
    }

    /* ② 最后删 screen。必须异步：本回调可能正跑在 screen 内对象的事件上 */
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
