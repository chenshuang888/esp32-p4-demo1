/*
 * sd_card_save —— 拍照保存实现
 *
 * 一个常驻任务 + 一个信号量 + 一个结果队列，逻辑是线性的：
 *   trigger -> give(sem) -> 任务醒来 -> 取最新一帧 -> 写文件 -> 结果进队列
 *
 * 移植自 demo1 的同名组件，改动只有两处：
 *   1. 目录常量改用 sd_card.h 的 SD_CARD_PHOTO_DIR（demo1 是在三个文件里各写一份字面量）
 *   2. next_photo_path 里解析编号不再把 uint32_t* 强转成 unsigned long* 交给 sscanf
 */
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

#include "sd_card.h"
#include "sd_card_save.h"

static const char *TAG = "sd_card_save";

/* 保存任务：优先级低于解码器(8)和 USB 驱动，写盘慢一点没关系，别抢它们的核 */
#define SAVE_TASK_STACK 4096
#define SAVE_TASK_PRIO  3
#define SAVE_TASK_CORE  1

static frame_buf_t      *s_jbuf     = NULL;  /* 注入：解码器产出的 JPEG 双缓冲 */
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
static int next_photo_path(char *path, size_t cap)
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

static void post_result(bool ok, const char *fname)
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

static void save_task(void *arg)
{
    (void)arg;

    for (;;) {
        if (xSemaphoreTake(s_save_sem, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        /* 取最新一帧。注意：**所有分支都必须 read_done** ——
         * 漏了的话解码器再也发布不了新帧（丢旧保新 -> 帧被连续丢弃），画面就冻住了。 */
        uint32_t len = 0;
        uint8_t *data = frame_buf_get_read(s_jbuf, &len);
        if (len == 0) {             /* 从没解出过帧（没插摄像头）：读槽还没被写过 */
            frame_buf_read_done(s_jbuf);
            ESP_LOGW(TAG, "no frame yet, nothing to save");
            post_result(false, NULL);
            continue;
        }

        char path[64];
        if (next_photo_path(path, sizeof(path)) != 0) {
            frame_buf_read_done(s_jbuf);
            post_result(false, NULL);
            continue;
        }

        FILE *f = fopen(path, "wb");
        if (f == NULL) {
            ESP_LOGE(TAG, "fopen %s failed", path);
            frame_buf_read_done(s_jbuf);
            post_result(false, NULL);
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
        post_result(n == len, path + strlen(SD_CARD_PHOTO_DIR) + 1);
    }
}

esp_err_t sd_card_save_init(frame_buf_t *jbuf)
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

    if (xTaskCreatePinnedToCore(save_task, "sd_save", SAVE_TASK_STACK, NULL,
                                SAVE_TASK_PRIO, NULL, SAVE_TASK_CORE) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "ready, photo dir: %s", SD_CARD_PHOTO_DIR);
    return ESP_OK;
}

void sd_card_save_trigger(void)
{
    if (s_save_sem == NULL) {
        /* init 没成功过（只可能是内存不足）。没有这个哨兵的话下面会解引用 NULL 崩掉，
         * 而"拍了照没反应"比"按一下快门就重启"好处理得多。 */
        return;
    }
    xSemaphoreGive(s_save_sem);
}

bool sd_card_save_get_result(sd_save_result_t *out)
{
    if (out == NULL) {
        return false;
    }
    return xQueueReceive(s_result_q, out, 0) == pdTRUE;
}
