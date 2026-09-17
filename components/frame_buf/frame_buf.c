#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"

#include "frame_buf.h"

struct frame_buf_s {
    uint8_t  *buf[2];           /* 两个槽（由 alloc 注入分配） */
    uint32_t  len[2];           /* has_len 时：每槽实际数据长度 */
    uint8_t   write_idx;        /* 生产者当前写槽 */
    uint8_t   read_idx;         /* 消费者当前读槽 */
    bool      read_in_progress; /* 消费者正持 read_idx 在读，commit 据此决定发布 */
    bool      has_len;
    SemaphoreHandle_t lock;     /* 保护索引与标志 */
    SemaphoreHandle_t new_frame;/* 唤醒消费者（binary，只保证"有最新"） */
};

/* 默认分配器：PSRAM 64B 对齐（驱动 DMA 写要求）。
 * 释放统一用 heap_caps_free，注入的 allocator 须为 heap_caps 系 */
static void *fb_default_alloc(size_t size, size_t *actual)
{
    void *p = heap_caps_aligned_alloc(64, size, MALLOC_CAP_SPIRAM);
    if (p != NULL && actual != NULL) {
        *actual = size;
    }
    return p;
}

frame_buf_t *frame_buf_create(const frame_buf_cfg_t *cfg)
{
    if (cfg == NULL || cfg->slot_size == 0) {
        return NULL;
    }
    frame_buf_t *fb = calloc(1, sizeof(*fb));
    if (fb == NULL) {
        return NULL;
    }
    /* 双缓冲初始必须分离：write=0/read=1，否则首次 commit 的 swap 无效 */
    fb->read_idx = 1;
    fb->has_len = cfg->has_len;

    void *(*alloc)(size_t, size_t *) = cfg->alloc ? cfg->alloc : fb_default_alloc;
    for (int i = 0; i < 2; i++) {
        size_t actual = 0;
        fb->buf[i] = alloc(cfg->slot_size, &actual);
        if (fb->buf[i] == NULL) {
            for (int j = 0; j < i; j++) {
                heap_caps_free(fb->buf[j]);
            }
            free(fb);
            return NULL;
        }
    }

    fb->lock = xSemaphoreCreateMutex();
    fb->new_frame = xSemaphoreCreateBinary();
    if (fb->lock == NULL || fb->new_frame == NULL) {
        for (int i = 0; i < 2; i++) {
            heap_caps_free(fb->buf[i]);
        }
        if (fb->lock) {
            vSemaphoreDelete(fb->lock);
        }
        if (fb->new_frame) {
            vSemaphoreDelete(fb->new_frame);
        }
        free(fb);
        return NULL;
    }
    return fb;
}

void frame_buf_delete(frame_buf_t *fb)
{
    if (fb == NULL) {
        return;
    }
    for (int i = 0; i < 2; i++) {
        if (fb->buf[i]) {
            heap_caps_free(fb->buf[i]);
            fb->buf[i] = NULL;
        }
    }
    if (fb->lock) {
        vSemaphoreDelete(fb->lock);
        fb->lock = NULL;
    }
    if (fb->new_frame) {
        vSemaphoreDelete(fb->new_frame);
        fb->new_frame = NULL;
    }
    free(fb);
}

uint8_t *frame_buf_get_write(frame_buf_t *fb)
{
    /* write_idx 只被生产者自身在 commit 里修改，此处无竞态，无需锁 */
    return fb->buf[fb->write_idx];
}

bool frame_buf_commit(frame_buf_t *fb, uint32_t len)
{
    bool published = false;
    xSemaphoreTake(fb->lock, portMAX_DELAY);
    if (!fb->read_in_progress) {
        /* 消费者已释放读槽才发布；否则本帧丢弃，继续在写槽覆盖 */
        if (fb->has_len) {
            fb->len[fb->write_idx] = len;
        }
        uint8_t tmp = fb->write_idx;
        fb->write_idx = fb->read_idx;
        fb->read_idx = tmp;
        published = true;
    }
    xSemaphoreGive(fb->lock);
    if (published) {
        xSemaphoreGive(fb->new_frame); /* give 放锁外，binary sem 原子 */
    }
    return published;
}

int frame_buf_wait_new(frame_buf_t *fb, uint32_t timeout_ms)
{
    return xSemaphoreTake(fb->new_frame, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

uint8_t *frame_buf_get_read(frame_buf_t *fb, uint32_t *out_len)
{
    xSemaphoreTake(fb->lock, portMAX_DELAY);
    fb->read_in_progress = true;
    uint8_t *p = fb->buf[fb->read_idx];
    if (out_len != NULL && fb->has_len) {
        *out_len = fb->len[fb->read_idx];
    }
    xSemaphoreGive(fb->lock);
    return p;
}

void frame_buf_read_done(frame_buf_t *fb)
{
    xSemaphoreTake(fb->lock, portMAX_DELAY);
    fb->read_in_progress = false;
    xSemaphoreGive(fb->lock);
}
