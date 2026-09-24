/*
 * 相册 App —— 看 SD 卡里拍过的照片
 *
 * ⚠️ 定位和相机 App 一样是**验证台**，不是相册产品：它要证明的是
 *    "SD 读文件 -> JPEG 解码 -> 上屏"这条链路通。
 *    所以只有一列文件名 + 点开看一张，没有缩略图、没有翻页、没有删除，
 *    也没有"未插卡"之类的引导（列不出来就是空列表 + 一行提示）。
 *
 * 真正的活都在两个现成接口上，本 App 只是把它们接起来：
 *   POSIX        读文件（sd_card 组件挂上 FATFS 之后直接 fopen）
 *   jpeg_decoder 解码（同步一次性接口，不关心帧从哪来）
 *
 * 解码链路：把文件字节读进**一块普通 PSRAM 缓冲**，直接调 jpeg_decode_frame()
 * 同步解到解码器自己的画布上，然后把那块画布交给 lv_image —— 中间不多一份拷贝。
 *
 * 输入为什么不需要 frame_buf：那个类型的价值在于"生产者和消费者会并发"，而这里
 * 读写都在 LVGL 任务里、一读一解就结束了。IDF 也明确说输入缓冲**没有对齐要求**
 * （只有输出有，见 jpeg_decoder.c 里的说明），所以普通 heap_caps_malloc 就够 ——
 * 顺带从两个槽 160KB 降到一块 80KB。
 *
 * 生命周期：解码引擎 + 600KB 画布，在 enter 建、leave 销毁。没有任务，
 * 所以销毁是平凡的（不再有"等解码任务退出"那一百毫秒）。
 */
#include "photo.h"

#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"

#include "app_manager.h"
#include "jpeg_decoder.h"
#include "picture.h"
#include "sd_card.h"
#include "ui_status_bar.h"

static const char *TAG = "photo";

#define PHOTO_MAX         64      /* 一次最多列出多少张；先不做分页，超出的直接不列 */
#define PHOTO_NAME_MAX    64
/* 单张 JPEG 的最大字节数：相机拍出来实测 20~80KB，120KB 覆盖波动尖峰与质量档上调。
 * 这是相册**输入缓冲**的尺寸，和拍照侧 jbuf 的槽大小是两个独立的量，各自定义。 */
#define PHOTO_SLOT_SIZE   80000

/* ---- 界面：enter 建、leave 清 ---- */
static lv_obj_t *s_scr    = NULL;
static lv_obj_t *s_list   = NULL;   /* 文件名列表 */
static lv_obj_t *s_imgbox = NULL;   /* 看图页容器：图在里面居中 */
static lv_obj_t *s_image  = NULL;
static lv_obj_t *s_status = NULL;   /* 底部一行：张数 / 文件名 / 出错原因 */

/* ---- 解码链路：enter 建、leave 销毁 ---- */
static uint8_t        *s_in  = NULL;    /* 本 App 建：读文件用的 JPEG 输入缓冲（普通 PSRAM） */
static jpeg_decoder_t *s_jd  = NULL;    /* 解码器（引擎 + 它自己那块输出画布） */
static lv_image_dsc_t  s_dsc;

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
 * 只切两个容器的显示/隐藏 —— 屏幕上那张图仍然指着解码器的画布，但画布归解码器
 * 所有、且这一轮不会再被写（只有 photo_show 里解码才会写它），所以这里不需要
 * 做任何"还槽"的动作。
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

    /* ---- 解码链路：本 App 建输入缓冲 -> 解码器（它自己带输出画布）---- */
    s_in = heap_caps_malloc(PHOTO_SLOT_SIZE, MALLOC_CAP_SPIRAM);
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

    /* ① 拆解码链路：引擎 + 600KB 画布一起释放，**没有等待**（同步解码没有任务）。
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
